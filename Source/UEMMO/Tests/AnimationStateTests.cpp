// M3-028: player locomotion animation gates. The user's playtest reported
// "the player character has no walking/running animation while moving"
// (sliding or standing pose) although idle renders through the same AnimBP
// and attack montages play through the M3-024 slot. The chain under test:
//   character velocity
//     -> NativeUpdateAnimation (Speed/Direction/PoseIndex, C++)
//     -> AnimBP BlendSpacePlayer X/Y (the M1-031 graph, Speed variable pin)
//     -> BlendListByInt pose 0 (PoseIndex pin)
//     -> DefaultSlot (M3-024) -> Root -> mesh pose.
// Four separate gates pin the chain so the failing layer names itself:
//   1. BlendSpace gate: the asset's axis ranges and samples cover the real
//      planar speeds (X 0..420, Direction -180..180, idle at 0, jog samples
//      at the actual 280/357/420 magnitudes).
//   2. Graph gate (editor only): the SAVED AnimBP keeps the M1-031/M3-024
//      node census AND the BlendSpacePlayer X/Y pins stay wired to the
//      Speed/Direction variables and the BlendListByInt stays wired to
//      PoseIndex - a lost variable binding compiles silently and renders as
//      an idle slide, so the pin links are asserted on the disk asset.
//   3. Runtime gate: a real character in a manually ticked temp world walks
//      through the production movement entry; the anim instance's exported
//      Speed must track the real velocity, the pose must stay on the
//      locomotion BlendSpace (PoseIndex 0, no montage) and the mesh POSE must
//      actually move (per-frame bone deltas while walking far above the idle
//      baseline) - a frozen pose under full speed is the reported symptom.
//   4. Rendered capture companion (M3-024/M3-027 pattern): idle -> sustained
//      movement -> idle screenshots plus a JSON telemetry timeline so the
//      walking pose can be EYEBALLED against the standing pose.
#include "Misc/AutomationTest.h"

#include "../Character/PrototypeAnimInstance.h"
#include "../PrototypeCharacter.h"

#include "Animation/AnimInstance.h"
#include "Animation/BlendSpace.h"
#include "AnimNodes/AnimNode_BlendSpacePlayer.h"
#include "AnimNodes/AnimNode_BlendListByInt.h"
#include "AnimNodes/AnimNode_Slot.h"
#include "Animation/AnimNodeBase.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_028
{
	// ---- shared constants --------------------------------------------------

	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M3_028_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with arena content
	// (the M1-022/M1-024/M1-027 placement convention). X horizontal, Y depth,
	// Z height; the floor top sits exactly at the base Z.
	const FVector M3_028_SceneBase(42000.0, 47000.0, 600.0);

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M3_028_FloorHalfThickness = 100.0f;
	const float M3_028_FloorHalfExtentXY = 4000.0f;

	// Spawn height above the floor top: the character drops a short distance
	// and settles through real no-controller physics (the M3-024 pattern).
	const float M3_028_PlayerSpawnHeight = 120.0f;

	// Phase frame caps. The extra tail after the settle lets the 0.6 s land
	// pose (spawn drop touchdown, LandPoseDuration) elapse so the idle window
	// observes the locomotion pose, not the landing hold.
	constexpr int32 M3_028_MaxSettleFrames = 300;
	constexpr int32 M3_028_LandTailFrames = 60;
	constexpr int32 M3_028_IdleSampleFrames = 60;
	constexpr int32 M3_028_MoveSampleFrames = 180;
	constexpr int32 M3_028_StopSampleFrames = 90;

	// Movement gate thresholds. The planar forward speed is 420 cm/s
	// (M1-029 PlanarSpeedX); the locomotion BlendSpace jog samples start at
	// 280, so a sustained walk above 200 cm/s samples the jog group. A pose
	// that never leaves the idle sample moves the bones far less than a jog
	// cycle, so the per-frame bone motion (component-space translation deltas,
	// summed over the skeleton) is the frozen-pose detector.
	constexpr float M3_028_IdleSpeedLimit = 10.0f;      // cm/s, the IsMoving threshold
	constexpr float M3_028_WalkingSpeedGate = 250.0f;   // cm/s, sustained walk gate
	constexpr float M3_028_SpeedTrackingTolerance = 1.0f; // cm/s, exported Speed vs velocity
	constexpr float M3_028_IdleBoneMotionFloor = 0.05f;  // cm/frame, idle must not be perfectly frozen
	constexpr float M3_028_MoveBoneMotionGate = 1.5f;    // cm/frame, walking motion floor
	constexpr float M3_028_MoveBoneMotionIdleRatio = 3.0f; // walking vs idle amplitude ratio

	// Asset paths (the M1-031/M3-024 shipped names).
	const TCHAR* const M3_028_AnimBlueprintPath = TEXT("/Game/UEMMO/Animation/ABP_Prototype.ABP_Prototype");
	const TCHAR* const M3_028_BlendSpacePath = TEXT("/Game/UEMMO/Animation/BS_Prototype_Locomotion.BS_Prototype_Locomotion");
	const TCHAR* const M3_028_BlendSpaceName = TEXT("BS_Prototype_Locomotion");

	// Evidence directory for the rendered capture companion.
	const TCHAR* const M3_028_EvidenceDir = TEXT("Artifacts/Tasks/M3-028");

	// ---- shared helpers ----------------------------------------------------

	// Spawns the world floor (the M1-041/M3-024 blocker box pattern).
	static AActor* M3_028_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base - FVector(0.0, 0.0, M3_028_FloorHalfThickness), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M3_028_Floor"));
		Actor->SetRootComponent(Floor);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->SetBoxExtent(FVector(M3_028_FloorHalfExtentXY, M3_028_FloorHalfExtentXY, M3_028_FloorHalfThickness));
		Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Floor->RegisterComponent();
		Floor->SetWorldLocation(Base - FVector(0.0, 0.0, M3_028_FloorHalfThickness));
		return Actor;
	}

	// Spawns the real prototype pawn above the floor and mirrors the arena
	// initialization so no-controller walking physics runs (M3-024 pattern).
	static APrototypeCharacter* M3_028_SpawnPlayer(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			Base + FVector(0.0, 0.0, M3_028_PlayerSpawnHeight),
			FRotator::ZeroRotator, Params);
		if (Player == nullptr)
		{
			return nullptr;
		}
		if (UCharacterMovementComponent* Movement = Player->GetCharacterMovement())
		{
			Movement->bRunPhysicsWithNoController = true;
			if (!Movement->IsActive())
			{
				Movement->Activate(/*bReset*/ true);
			}
			if (Movement->MovementMode == MOVE_None)
			{
				Movement->SetDefaultMovementMode();
			}
		}
		if (USkeletalMeshComponent* PlayerMesh = Player->GetMesh())
		{
			// ACharacter meshes default to OnlyTickPoseWhenRendered, which never
			// refreshes bones in a headless temp world and would freeze the bone
			// motion metric at zero regardless of the animation state. The real
			// game renders the pawn every frame, so always-tick mirrors what the
			// player actually sees.
			PlayerMesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
		}
		return Player;
	}

	// Per-frame bone motion metric: the summed component-space translation
	// delta over the whole skeleton between two consecutive frames. A playing
	// jog cycle moves the limbs several centimeters per frame; a frozen or
	// idle-breathing pose moves them far less. First call seeds the scratch
	// buffer and returns -1 (no delta yet).
	static float M3_028_BoneMotionDelta(USkeletalMeshComponent& Mesh, TArray<FVector>& ScratchPrevious)
	{
		const TArray<FTransform>& Transforms = Mesh.GetComponentSpaceTransforms();
		if (Transforms.Num() == 0)
		{
			return -1.0f;
		}
		if (ScratchPrevious.Num() != Transforms.Num())
		{
			ScratchPrevious.Reset(Transforms.Num());
			for (const FTransform& Transform : Transforms)
			{
				ScratchPrevious.Add(Transform.GetTranslation());
			}
			return -1.0f;
		}
		float Total = 0.0f;
		for (int32 Index = 0; Index < Transforms.Num(); ++Index)
		{
			Total += (Transforms[Index].GetTranslation() - ScratchPrevious[Index]).Size();
		}
		for (int32 Index = 0; Index < Transforms.Num(); ++Index)
		{
			ScratchPrevious[Index] = Transforms[Index].GetTranslation();
		}
		return Total;
	}

	// Logs the LIVE runtime anim-node state. The serialized node UPROPERTYs
	// (X/Y, ActiveChildIndex) are editor-only defaults the running graph never
	// writes once the pins are connected (UE 5.5+ moves connected pin values
	// into the instance anim node data), so the authoritative reads are the
	// node virtual getters plus the engine's own debug-data walk, which prints
	// the live slot weight, the blend list active child and per-pose weights
	// and the BlendSpace play time - the same source the Anim debug panel
	// renders. The walk starts at the montage slot, so the whole shipped graph
	// (slot -> blend list -> BlendSpace/sequence players) is covered.
	static void M3_028_LogRuntimeNodeState(const TCHAR* Headline, const UPrototypeAnimInstance& Anim)
	{
		const FStructProperty* SlotNodeProperty = nullptr;
		const FStructProperty* BlendSpaceNodeProperty = nullptr;
		const FStructProperty* BlendListNodeProperty = nullptr;
		for (TFieldIterator<FStructProperty> It(Anim.GetClass()); It; ++It)
		{
			if (It->Struct == FAnimNode_Slot::StaticStruct())
			{
				SlotNodeProperty = *It;
			}
			else if (It->Struct == FAnimNode_BlendSpacePlayer::StaticStruct())
			{
				BlendSpaceNodeProperty = *It;
			}
			else if (It->Struct == FAnimNode_BlendListByInt::StaticStruct())
			{
				BlendListNodeProperty = *It;
			}
		}
		if (BlendSpaceNodeProperty == nullptr)
		{
			UE_LOG(LogTemp, Display, TEXT("UEMMO M3_028 node state [%s]: no BlendSpacePlayer node property on the generated class."), Headline);
			return;
		}
		const void* BlendSpaceNode = BlendSpaceNodeProperty->ContainerPtrToValuePtr<void>(&Anim);
		const FAnimNode_BlendSpacePlayer* BlendSpaceNodeTyped = static_cast<const FAnimNode_BlendSpacePlayer*>(BlendSpaceNode);
		const FVector BoundPosition = BlendSpaceNodeTyped->GetPosition();
		const FVector FilteredPosition = BlendSpaceNodeTyped->GetFilteredPosition();
		const FObjectPropertyBase* BlendSpaceProperty = CastField<FObjectPropertyBase>(BlendSpaceNodeProperty->Struct->FindPropertyByName(TEXT("BlendSpace")));
		const UObject* BlendSpaceAsset = BlendSpaceProperty != nullptr
			? BlendSpaceProperty->GetObjectPropertyValue(BlendSpaceProperty->ContainerPtrToValuePtr<void>(BlendSpaceNode))
			: nullptr;

		FString BlendListText = TEXT("no BlendListByInt node property");
		if (BlendListNodeProperty != nullptr)
		{
			void* BlendListNode = const_cast<void*>(BlendListNodeProperty->ContainerPtrToValuePtr<void>(&Anim));
			const int32 ActiveChild = const_cast<FAnimNode_BlendListByInt*>(
				static_cast<const FAnimNode_BlendListByInt*>(BlendListNode))->GetActiveChildIndex();
			BlendListText = FString::Printf(TEXT("activeChild=%d"), ActiveChild);
		}

		UE_LOG(LogTemp, Display, TEXT(
			"UEMMO M3_028 node state [%s]: blendspace=%s bound=(%.1f,%.1f) filtered=(%.1f,%.1f) playRate=%.3f looping=%d evaluator=%d %s"),
			Headline,
			BlendSpaceAsset != nullptr ? *BlendSpaceAsset->GetName() : TEXT("null"),
			BoundPosition.X, BoundPosition.Y, FilteredPosition.X, FilteredPosition.Y,
			BlendSpaceNodeTyped->GetPlayRate(),
			BlendSpaceNodeTyped->IsLooping() ? 1 : 0,
			BlendSpaceNodeTyped->IsEvaluator() ? 1 : 0,
			*BlendListText);

		// Full live-tree walk with weights from the montage slot down.
		if (SlotNodeProperty != nullptr)
		{
			void* SlotNode = const_cast<void*>(SlotNodeProperty->ContainerPtrToValuePtr<void>(&Anim));
			FNodeDebugData DebugData(&Anim);
			const_cast<FAnimNode_Slot*>(static_cast<const FAnimNode_Slot*>(SlotNode))->GatherDebugData(DebugData);
			TArray<FNodeDebugData::FFlattenedDebugData> Flattened;
			int32 ChainId = 0;
			DebugData.GetFlattenedDebugData(Flattened, 0, ChainId);
			for (const FNodeDebugData::FFlattenedDebugData& Entry : Flattened)
			{
				UE_LOG(LogTemp, Display, TEXT("UEMMO M3_028 live tree [%s]: w=%.2f %s"),
					Headline, Entry.AbsoluteWeight, *Entry.DebugLine);
			}
		}
	}

	// One telemetry row of the runtime gate (logged and summarized).
	static void M3_028_LogAnimRow(int32 Frame, const APrototypeCharacter& Player, const UPrototypeAnimInstance& Anim, float BoneMotion)
	{
		const UCharacterMovementComponent* Movement = Player.GetCharacterMovement();
		const FVector Velocity = Player.GetVelocity();
		UE_LOG(LogTemp, Display, TEXT(
			"UEMMO M3_028 frame=%d mode=%d vel=%.1f,%.1f,%.1f planar=%.1f speed=%.1f dir=%.1f pose=%d moving=%d falling=%d bones=%.2f"),
			Frame,
			Movement != nullptr ? static_cast<int32>(Movement->MovementMode) : -1,
			Velocity.X, Velocity.Y, Velocity.Z,
			Velocity.Size2D(),
			Anim.Speed, Anim.Direction, Anim.PoseIndex,
			Anim.IsMoving ? 1 : 0, Anim.IsFalling ? 1 : 0, BoneMotion);
	}

	// Requests one screenshot of the already staged state (the M3-011/M3-024
	// latent-command precedent); skips gracefully without rendering.
	struct FM3_028_ScreenshotLatentCommand : public IAutomationLatentCommand
	{
		FString AbsolutePath;

		explicit FM3_028_ScreenshotLatentCommand(const FString& InAbsolutePath)
			: AbsolutePath(InAbsolutePath)
		{
		}

		virtual bool Update() override
		{
			if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
			{
				UE_LOG(LogTemp, Display, TEXT("M3_028 screenshot skipped (no rendering): %s"), *AbsolutePath);
				return true;
			}
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsolutePath), /*Tree*/ true);
			FScreenshotRequest::RequestScreenshot(AbsolutePath, /*bShowUI*/ true, /*bAddFilenameSuffix*/ false);
			UE_LOG(LogTemp, Display, TEXT("M3_028 screenshot requested: %s"), *AbsolutePath);
			return true;
		}
	};

	// ---- runtime scene (the M3_024 temp world pattern) ---------------------

	struct FM3_028_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		APrototypeCharacter* Player = nullptr;
		USkeletalMeshComponent* Mesh = nullptr;
		UPrototypeAnimInstance* Anim = nullptr;

		bool Build(FAutomationTestBase& Test)
		{
			if (!Test.TestTrue(TEXT("the manually ticked test world is created (engine FTestWorldWrapper precedent)"),
				Wrapper.CreateTestWorld(EWorldType::Game)))
			{
				return false;
			}
			World = Wrapper.GetTestWorld();
			if (!Test.TestNotNull(TEXT("the test world is available"), World))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("play begins in the test world"), Wrapper.BeginPlayInTestWorld()))
			{
				return false;
			}
			AActor* Floor = M3_028_SpawnFloor(*World, M3_028_SceneBase);
			if (!Test.TestNotNull(TEXT("the floor spawns"), Floor))
			{
				return false;
			}
			World->EnsureCollisionTreeIsBuilt();
			Player = M3_028_SpawnPlayer(*World, M3_028_SceneBase);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			Mesh = Player->GetMesh();
			if (!Test.TestNotNull(TEXT("the character mesh exists"), Mesh))
			{
				return false;
			}
			UAnimInstance* BaseAnim = Mesh->GetAnimInstance();
			if (!Test.TestNotNull(TEXT("the mesh runs an anim instance"), BaseAnim))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("the anim instance is the real UPrototypeAnimInstance AnimBP"),
				BaseAnim->IsA<UPrototypeAnimInstance>()))
			{
				return false;
			}
			Anim = Cast<UPrototypeAnimInstance>(BaseAnim);
			return true;
		}

		bool Tick()
		{
			return Wrapper.TickTestWorld(M3_028_FrameSeconds);
		}

		// Ticks until the pawn walks grounded and near-rests (or the cap runs
		// out) and reports whether the settle succeeded.
		bool Settle(FAutomationTestBase& Test)
		{
			bool bSettled = false;
			for (int32 Frame = 0; Frame < M3_028_MaxSettleFrames && !bSettled; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks"), Tick()))
				{
					return false;
				}
				const UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
				bSettled = Movement != nullptr
					&& Movement->MovementMode == MOVE_Walking
					&& Movement->Velocity.Size() < 1.0f;
			}
			if (!Test.TestTrue(TEXT("the character settles grounded on the floor"), bSettled))
			{
				return false;
			}
			// Land-pose tail: the spawn drop touches down, the 0.6 s land hold
			// must elapse before the locomotion pose is expected again.
			for (int32 Frame = 0; Frame < M3_028_LandTailFrames; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks (land tail)"), Tick()))
				{
					return false;
				}
			}
			return true;
		}
	};
}

using namespace UE::UEMMO::Tasks::M3_028;

// 1. The BlendSpace gate: the axis ranges must cover the real planar speeds
//    and the samples must include idle plus jog magnitudes at every class of
//    direction. A range left at the factory default (0..100) would clamp every
//    real walk speed into one spot and read as "no animation".
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_028BlendSpaceCoversPlanarSpeeds,
	"UEMMO.Tasks.M3_028.BlendSpaceCoversPlanarSpeeds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_028BlendSpaceCoversPlanarSpeeds::RunTest(const FString& Parameters)
{
	UBlendSpace* BlendSpace = LoadObject<UBlendSpace>(nullptr, M3_028_BlendSpacePath);
	if (!TestNotNull(TEXT("the locomotion BlendSpace loads"), BlendSpace))
	{
		return true;
	}

	// Axis ranges (the fixed C-array the M1-031 builder writes through
	// reflection; Python cannot address it, C++ can).
	constexpr float M3_028_AxisTolerance = 0.01f;
	if (const FProperty* ParamProperty = BlendSpace->GetClass()->FindPropertyByName(TEXT("BlendParameters")))
	{
		const FBlendParameter* Params = static_cast<const FBlendParameter*>(ParamProperty->ContainerPtrToValuePtr<void>(BlendSpace));
		UE_LOG(LogTemp, Display, TEXT("UEMMO M3_028 BlendSpace axis X: '%s' [%.1f..%.1f] grid=%d, axis Y: '%s' [%.1f..%.1f] grid=%d"),
			*Params[0].DisplayName, Params[0].Min, Params[0].Max, Params[0].GridNum,
			*Params[1].DisplayName, Params[1].Min, Params[1].Max, Params[1].GridNum);
		TestTrue(TEXT("the BlendSpace X axis covers the full forward planar speed (min 0)"),
			FMath::IsNearlyEqual(Params[0].Min, 0.0f, M3_028_AxisTolerance));
		TestTrue(TEXT("the BlendSpace X axis covers the full forward planar speed (max 420)"),
			FMath::IsNearlyEqual(Params[0].Max, 420.0f, M3_028_AxisTolerance));
		TestTrue(TEXT("the BlendSpace Y axis spans the full direction range"),
			FMath::IsNearlyEqual(Params[1].Min, -180.0f, M3_028_AxisTolerance)
			&& FMath::IsNearlyEqual(Params[1].Max, 180.0f, M3_028_AxisTolerance));
	}
	else
	{
		AddError(TEXT("the BlendParameters property was not found on the BlendSpace"));
	}

	// Samples: idle at zero speed, forward jog at the full speed, side jogs at
	// the depth speed, and every referenced animation loadable.
	const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();
	TestTrue(TEXT("the BlendSpace keeps idle plus the eight directions"), Samples.Num() >= 9);
	bool bHasIdle = false;
	bool bHasForwardJog = false;
	bool bHasSideJogs = false;
	bool bAllLoadable = Samples.Num() > 0;
	for (const FBlendSample& Sample : Samples)
	{
		UE_LOG(LogTemp, Display, TEXT("UEMMO M3_028 BlendSpace sample (%.0f, %.0f) -> %s"),
			Sample.SampleValue.X, Sample.SampleValue.Y,
			Sample.Animation != nullptr ? *Sample.Animation->GetPathName() : TEXT("null"));
		if (Sample.Animation == nullptr)
		{
			bAllLoadable = false;
			continue;
		}
		if (FMath::IsNearlyZero(Sample.SampleValue.X, M3_028_AxisTolerance))
		{
			bHasIdle = true;
		}
		if (FMath::IsNearlyEqual(Sample.SampleValue.X, 420.0f, M3_028_AxisTolerance)
			&& FMath::IsNearlyZero(Sample.SampleValue.Y, M3_028_AxisTolerance))
		{
			bHasForwardJog = true;
		}
		if (FMath::IsNearlyEqual(Sample.SampleValue.X, 280.0f, M3_028_AxisTolerance)
			&& (FMath::IsNearlyEqual(Sample.SampleValue.Y, 90.0f, M3_028_AxisTolerance)
				|| FMath::IsNearlyEqual(Sample.SampleValue.Y, -90.0f, M3_028_AxisTolerance)))
		{
			bHasSideJogs = true;
		}
	}
	TestTrue(TEXT("the BlendSpace contains the idle sample at speed 0"), bHasIdle);
	TestTrue(TEXT("the BlendSpace contains the forward jog sample at the full speed"), bHasForwardJog);
	TestTrue(TEXT("the BlendSpace contains the side jog samples at the depth speed"), bHasSideJogs);
	TestTrue(TEXT("every BlendSpace sample references a loadable animation"), bAllLoadable);

	// Runtime sampling probe: the engine samples through the built grid
	// (BlendSpaceData), not through the raw sample list. A BlendSpace whose
	// grid never got built returns nothing here and freezes every player at
	// its first pose - the reported symptom - while the raw samples still
	// read fine.
	auto ProbeSampling = [this, BlendSpace](float Speed, float Direction, const TCHAR* Label)
	{
		TArray<FBlendSampleData> Sampled;
		int32 TriangulationIndex = -1;
		const bool bSampled = BlendSpace->GetSamplesFromBlendInput(FVector(Speed, Direction, 0.0f), Sampled, TriangulationIndex, true);
		float TotalWeight = 0.0f;
		for (const FBlendSampleData& Sample : Sampled)
		{
			TotalWeight += Sample.TotalWeight;
			UE_LOG(LogTemp, Display, TEXT("UEMMO M3_028 sampling probe [%s] at (%.0f, %.0f): sample %s weight %.3f"),
				Label, Speed, Direction,
				Sample.Animation != nullptr ? *Sample.Animation->GetName() : TEXT("null"),
				Sample.TotalWeight);
		}
		TestTrue(FString::Printf(TEXT("the BlendSpace samples the %s position (grid data built)"), Label),
			bSampled && Sampled.Num() > 0 && TotalWeight > 0.5f);
	};
	ProbeSampling(0.0f, 0.0f, TEXT("idle"));
	ProbeSampling(420.0f, 0.0f, TEXT("forward jog"));
	ProbeSampling(280.0f, 90.0f, TEXT("right jog"));
	ProbeSampling(420.0f, 180.0f, TEXT("backward jog"));
	return true;
}

// 2. The graph gate (editor only): the SAVED AnimBP keeps the M1-031/M3-024
//    node census and the locomotion inputs stay wired - the BlendSpacePlayer
//    X/Y pins must be driven by the Speed/Direction variables and the
//    BlendListByInt index pin by PoseIndex. A silently lost variable binding
//    compiles fine and renders exactly the reported symptom.
#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_028AnimGraphWiresLocomotionInputs,
	"UEMMO.Tasks.M3_028.AnimGraphWiresLocomotionInputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_028AnimGraphWiresLocomotionInputs::RunTest(const FString& Parameters)
{
	UBlueprint* AnimBP = LoadObject<UBlueprint>(nullptr, M3_028_AnimBlueprintPath);
	if (!TestNotNull(TEXT("the locomotion AnimBP loads from disk"), AnimBP))
	{
		return true;
	}
	UEdGraph* AnimGraph = nullptr;
	for (UEdGraph* Graph : AnimBP->FunctionGraphs)
	{
		if (Graph != nullptr && Graph->GetFName() == TEXT("AnimGraph"))
		{
			AnimGraph = Graph;
			break;
		}
	}
	if (!TestNotNull(TEXT("the AnimBP carries an AnimGraph"), AnimGraph))
	{
		return true;
	}

	// Census regression lock (the M1-031/M3-024 shipped graph shape).
	UEdGraphNode* RootNode = nullptr;
	UObject* SlotNode = nullptr;
	UEdGraphNode* BlendSpaceNode = nullptr;
	UEdGraphNode* BlendListNode = nullptr;
	int32 RootNodes = 0;
	int32 SlotNodes = 0;
	int32 BlendSpacePlayers = 0;
	int32 SequencePlayers = 0;
	int32 BlendListNodes = 0;
	int32 VariableGets = 0;
	for (UEdGraphNode* Node : AnimGraph->Nodes)
	{
		if (Node == nullptr)
		{
			continue;
		}
		const FName ClassName = Node->GetClass()->GetFName();
		if (ClassName == FName(TEXT("AnimGraphNode_Root"))) { ++RootNodes; RootNode = Node; }
		else if (ClassName == FName(TEXT("AnimGraphNode_Slot"))) { ++SlotNodes; SlotNode = Node; }
		else if (ClassName == FName(TEXT("AnimGraphNode_BlendSpacePlayer"))) { ++BlendSpacePlayers; BlendSpaceNode = Node; }
		else if (ClassName == FName(TEXT("AnimGraphNode_SequencePlayer"))) { ++SequencePlayers; }
		else if (ClassName == FName(TEXT("AnimGraphNode_BlendListByInt"))) { ++BlendListNodes; BlendListNode = Node; }
		else if (ClassName == FName(TEXT("K2Node_VariableGet"))) { ++VariableGets; }
	}
	UE_LOG(LogTemp, Display, TEXT(
		"UEMMO M3_028 AnimGraph census: Root=%d Slot=%d BlendSpacePlayer=%d SequencePlayer=%d BlendListByInt=%d VariableGet=%d"),
		RootNodes, SlotNodes, BlendSpacePlayers, SequencePlayers, BlendListNodes, VariableGets);
	TestEqual(TEXT("the AnimGraph keeps exactly one root node"), RootNodes, 1);
	TestEqual(TEXT("the AnimGraph keeps exactly one montage slot node"), SlotNodes, 1);
	TestEqual(TEXT("the AnimGraph keeps exactly one locomotion BlendSpace player"), BlendSpacePlayers, 1);
	TestEqual(TEXT("the AnimGraph keeps exactly three air/land sequence players"), SequencePlayers, 3);
	TestEqual(TEXT("the AnimGraph keeps exactly one pose blend list"), BlendListNodes, 1);
	TestEqual(TEXT("the AnimGraph keeps exactly three variable getters (Speed/Direction/PoseIndex)"), VariableGets, 3);

	// A lost link must be named by its own failure: one checker per input.
	auto CheckVariableBinding = [this](UEdGraphNode& SourceNode, const TCHAR* InputPinName,
		const TCHAR* VariableName, const TCHAR* Description)
	{
		UEdGraphPin* InputPin = SourceNode.FindPin(InputPinName, EGPD_Input);
		if (!TestNotNull(FString::Printf(TEXT("%s: the %s input pin exists"), Description, InputPinName), InputPin))
		{
			return;
		}
		if (!TestEqual(FString::Printf(TEXT("%s: the %s pin has exactly one link"), Description, InputPinName),
			InputPin->LinkedTo.Num(), 1))
		{
			return;
		}
		UEdGraphPin* SourcePin = InputPin->LinkedTo[0];
		UEdGraphNode* SourceGraphNode = SourcePin != nullptr ? SourcePin->GetOwningNode() : nullptr;
		TestTrue(FString::Printf(TEXT("%s: the %s pin is driven by a variable getter"), Description, InputPinName),
			SourceGraphNode != nullptr && SourceGraphNode->GetClass()->GetFName() == FName(TEXT("K2Node_VariableGet")));
		TestEqual(FString::Printf(TEXT("%s: the %s pin is driven by the %s variable"), Description, InputPinName, VariableName),
			SourcePin != nullptr ? SourcePin->GetFName() : FName(NAME_None), FName(VariableName));
	};

	if (BlendSpaceNode != nullptr)
	{
		CheckVariableBinding(*BlendSpaceNode, TEXT("X"), TEXT("Speed"),
			TEXT("the locomotion BlendSpace player"));
		CheckVariableBinding(*BlendSpaceNode, TEXT("Y"), TEXT("Direction"),
			TEXT("the locomotion BlendSpace player"));

		// The player must reference the shipped locomotion BlendSpace asset
		// (read the FAnimNode_BlendSpacePlayer BlendSpace object property
		// through reflection - the editor node header stays out of this file).
		const FStructProperty* NodeProperty = CastField<FStructProperty>(BlendSpaceNode->GetClass()->FindPropertyByName(TEXT("Node")));
		const FObjectPropertyBase* BlendSpaceProperty = NodeProperty != nullptr
			? CastField<FObjectPropertyBase>(NodeProperty->Struct->FindPropertyByName(TEXT("BlendSpace")))
			: nullptr;
		if (TestNotNull(TEXT("the BlendSpace player node carries a Node struct"), const_cast<FStructProperty*>(NodeProperty))
			&& TestNotNull(TEXT("the BlendSpace player node has a BlendSpace property"), const_cast<FObjectPropertyBase*>(BlendSpaceProperty)))
		{
			const void* NodePtr = NodeProperty->ContainerPtrToValuePtr<void>(BlendSpaceNode);
			const UObject* BlendSpaceAsset = BlendSpaceProperty->GetObjectPropertyValue(BlendSpaceProperty->ContainerPtrToValuePtr<void>(NodePtr));
			TestNotNull(TEXT("the BlendSpace player references a BlendSpace asset"), const_cast<UObject*>(BlendSpaceAsset));
			if (BlendSpaceAsset != nullptr)
			{
				TestTrue(TEXT("the BlendSpace player references the shipped locomotion BlendSpace"),
					BlendSpaceAsset->GetName() == M3_028_BlendSpaceName);
			}
		}
	}
	if (BlendListNode != nullptr)
	{
		CheckVariableBinding(*BlendListNode, TEXT("ActiveChildIndex"), TEXT("PoseIndex"),
			TEXT("the pose blend list"));
	}
	return true;
}
#endif // WITH_EDITOR

// 3. The runtime gate: a real character walking through the production
//    movement entry must export fresh locomotion variables AND the mesh pose
//    must actually move - the exact reported symptom is a full-speed walk
//    with an unmoving pose.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_028RealCharacterLocomotionAnimState,
	"UEMMO.Tasks.M3_028.RealCharacterLocomotionAnimState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_028RealCharacterLocomotionAnimState::RunTest(const FString& Parameters)
{
	FM3_028_Scene Scene;
	if (!Scene.Build(*this) || !Scene.Settle(*this))
	{
		return true;
	}

	// ---- idle window -------------------------------------------------------
	float IdleMaxSpeed = 0.0f;
	float IdleMotionSum = 0.0f;
	int32 IdleMotionFrames = 0;
	TArray<FVector> PreviousBones;
	for (int32 Frame = 0; Frame < M3_028_IdleSampleFrames; ++Frame)
	{
		if (!TestTrue(TEXT("the test world ticks (idle)"), Scene.Tick()))
		{
			return true;
		}
		IdleMaxSpeed = FMath::Max(IdleMaxSpeed, Scene.Anim->Speed);
		const float Motion = M3_028_BoneMotionDelta(*Scene.Mesh, PreviousBones);
		if (Motion >= 0.0f)
		{
			IdleMotionSum += Motion;
			++IdleMotionFrames;
		}
	}
	const float IdleMotionAverage = IdleMotionFrames > 0 ? IdleMotionSum / IdleMotionFrames : 0.0f;
	UE_LOG(LogTemp, Display, TEXT(
		"UEMMO M3_028 idle window: maxSpeed=%.2f pose=%d montage=%s boneMotion=%.3f cm/frame over %d frames"),
		IdleMaxSpeed, Scene.Anim->PoseIndex,
		Scene.Anim->GetCurrentActiveMontage() != nullptr ? TEXT("active") : TEXT("none"),
		IdleMotionAverage, IdleMotionFrames);
	TestTrue(TEXT("the settled idle exports near-zero speed"), IdleMaxSpeed < M3_028_IdleSpeedLimit);
	TestTrue(TEXT("the settled idle stays on the locomotion pose"),
		Scene.Anim->PoseIndex == UPrototypeAnimInstance::Pose_Locomotion);
	TestTrue(TEXT("no montage plays during idle (the slot passes the locomotion pose through)"),
		Scene.Anim->GetCurrentActiveMontage() == nullptr);
	TestTrue(TEXT("the idle pose breathes (the metric detects pose motion at all)"),
		IdleMotionAverage > M3_028_IdleBoneMotionFloor);
	M3_028_LogRuntimeNodeState(TEXT("idle"), *Scene.Anim);

	// Resolve the live BlendSpacePlayer node once (the compiled AnimBP keeps
	// every anim graph node as an FStructProperty member of the generated
	// instance and the running graph evaluates exactly that memory).
	const FStructProperty* BlendSpaceNodeProperty = nullptr;
	for (TFieldIterator<FStructProperty> It(Scene.Anim->GetClass()); It; ++It)
	{
		if (It->Struct == FAnimNode_BlendSpacePlayer::StaticStruct())
		{
			BlendSpaceNodeProperty = *It;
			break;
		}
	}
	if (!TestNotNull(TEXT("the generated AnimBP class carries the BlendSpacePlayer node property"),
		const_cast<FStructProperty*>(BlendSpaceNodeProperty)))
	{
		return true;
	}
	void* BlendSpaceNode = BlendSpaceNodeProperty->ContainerPtrToValuePtr<void>(Scene.Anim);
	const FAnimNode_BlendSpacePlayer* BlendSpaceNodeTyped = static_cast<const FAnimNode_BlendSpacePlayer*>(BlendSpaceNode);
	const FObjectPropertyBase* LiveBlendSpaceProperty = CastField<FObjectPropertyBase>(
		BlendSpaceNodeProperty->Struct->FindPropertyByName(TEXT("BlendSpace")));
	if (LiveBlendSpaceProperty != nullptr)
	{
		const UObject* LiveBlendSpace = LiveBlendSpaceProperty->GetObjectPropertyValue(
			LiveBlendSpaceProperty->ContainerPtrToValuePtr<void>(BlendSpaceNode));
		TestTrue(TEXT("the live graph plays the shipped locomotion BlendSpace"),
			LiveBlendSpace != nullptr && LiveBlendSpace->GetName() == TEXT("BS_Prototype_Locomotion"));
	}

	// ---- walk window (production movement entry, sustained forward input) ---
	float MaxExportedSpeed = 0.0f;
	float MaxSampledX = 0.0f;
	int32 SampledXFrames = 0;
	int32 SampledXMismatchFrames = 0;
	float MaxPlanarVelocity = 0.0f;
	float SustainedSpeedSum = 0.0f;
	int32 SustainedFrames = 0;
	int32 TrackingFrames = 0;
	int32 TrackingMismatchFrames = 0;
	float MoveMotionSum = 0.0f;
	int32 MoveMotionFrames = 0;
	bool bPoseStayedLocomotion = true;
	bool bNoMontageWhileWalking = true;
	bool bDirectionStayedForward = true;
	for (int32 Frame = 0; Frame < M3_028_MoveSampleFrames; ++Frame)
	{
		// The same movement entry the production input path ends in
		// (ApplyPlanarMovement -> AddMovementInput on the character).
		Scene.Player->AddMovementInput(FVector::ForwardVector, 1.0f);
		if (!TestTrue(TEXT("the test world ticks (walk)"), Scene.Tick()))
		{
			return true;
		}
		const float PlanarVelocity = Scene.Player->GetVelocity().Size2D();
		MaxExportedSpeed = FMath::Max(MaxExportedSpeed, Scene.Anim->Speed);
		MaxPlanarVelocity = FMath::Max(MaxPlanarVelocity, PlanarVelocity);
		if (PlanarVelocity > M3_028_WalkingSpeedGate)
		{
			SustainedSpeedSum += Scene.Anim->Speed;
			++SustainedFrames;
			if (FMath::Abs(Scene.Anim->Speed - PlanarVelocity) > M3_028_SpeedTrackingTolerance)
			{
				++TrackingMismatchFrames;
			}
			++TrackingFrames;
			if (Scene.Anim->PoseIndex != UPrototypeAnimInstance::Pose_Locomotion)
			{
				bPoseStayedLocomotion = false;
			}
			if (Scene.Anim->GetCurrentActiveMontage() != nullptr)
			{
				bNoMontageWhileWalking = false;
			}
			if (FMath::Abs(Scene.Anim->Direction) > 30.0f)
			{
				bDirectionStayedForward = false;
			}
			{
				// The bound pin value lives in the instance anim node data
				// (GetPosition reads it); the serialized X property is only the
				// unconnected-pin default and never moves at runtime.
				const float SampledX = BlendSpaceNodeTyped->GetPosition().X;
				MaxSampledX = FMath::Max(MaxSampledX, SampledX);
				++SampledXFrames;
				if (FMath::Abs(SampledX - Scene.Anim->Speed) > 2.0f)
				{
					++SampledXMismatchFrames;
				}
			}
		}
		const float Motion = M3_028_BoneMotionDelta(*Scene.Mesh, PreviousBones);
		if (Motion >= 0.0f)
		{
			MoveMotionSum += Motion;
			++MoveMotionFrames;
		}
		if (Frame % 15 == 0)
		{
			M3_028_LogAnimRow(Frame, *Scene.Player, *Scene.Anim, Motion);
		}
		if (Frame == 90)
		{
			M3_028_LogRuntimeNodeState(TEXT("walking"), *Scene.Anim);
		}
	}
	const float SustainedSpeedAverage = SustainedFrames > 0 ? SustainedSpeedSum / SustainedFrames : 0.0f;
	const float MoveMotionAverage = MoveMotionFrames > 0 ? MoveMotionSum / MoveMotionFrames : 0.0f;
	UE_LOG(LogTemp, Display, TEXT(
		"UEMMO M3_028 walk window: maxPlanarVelocity=%.1f maxExportedSpeed=%.1f sustainedAvgSpeed=%.1f over %d frames "
		"trackingMismatch=%d/%d sampledX=%.1f xMismatch=%d/%d pose=%d dir=%.1f boneMotion=%.3f cm/frame"),
		MaxPlanarVelocity, MaxExportedSpeed, SustainedSpeedAverage, SustainedFrames,
		TrackingMismatchFrames, TrackingFrames, MaxSampledX, SampledXMismatchFrames, SampledXFrames,
		Scene.Anim->PoseIndex, Scene.Anim->Direction, MoveMotionAverage);

	// Harness precondition: the walk input really moved the character. A
	// failure here indicts the harness, not the animation chain.
	TestTrue(TEXT("the character reached walking velocity under sustained input (harness precondition)"),
		MaxPlanarVelocity > M3_028_WalkingSpeedGate);
	TestTrue(TEXT("the character walked sustained above the jog sample band (harness precondition)"),
		SustainedFrames >= 60 && SustainedSpeedAverage > 200.0f);

	// The animation chain gates.
	TestTrue(TEXT("the exported Speed followed the real walk speed"), MaxExportedSpeed > M3_028_WalkingSpeedGate);
	TestTrue(TEXT("the exported Speed matched the real velocity on every sustained frame"),
		TrackingFrames > 0 && TrackingMismatchFrames == 0);
	TestTrue(TEXT("the BlendSpacePlayer sampled X coordinate followed the exported Speed (the Speed binding feeds the graph)"),
		SampledXFrames > 0 && SampledXMismatchFrames == 0 && MaxSampledX > M3_028_WalkingSpeedGate);
	TestTrue(TEXT("the pose stayed on the locomotion BlendSpace while walking"),
		bPoseStayedLocomotion);
	TestTrue(TEXT("no montage hijacked the pose while walking"), bNoMontageWhileWalking);
	TestTrue(TEXT("the forward walk exported a forward-facing locomotion direction"), bDirectionStayedForward);
	TestTrue(TEXT("the walking pose moved (bone motion above the frozen-pose floor)"),
		MoveMotionAverage > M3_028_MoveBoneMotionGate);
	TestTrue(TEXT("the walking pose moved clearly more than the idle pose"),
		MoveMotionAverage > IdleMotionAverage * M3_028_MoveBoneMotionIdleRatio);

	// ---- stop window --------------------------------------------------------
	int32 FramesBackToIdle = -1;
	float StopMinSpeed = TNumericLimits<float>::Max();
	for (int32 Frame = 0; Frame < M3_028_StopSampleFrames; ++Frame)
	{
		if (!TestTrue(TEXT("the test world ticks (stop)"), Scene.Tick()))
		{
			return true;
		}
		StopMinSpeed = FMath::Min(StopMinSpeed, Scene.Anim->Speed);
		if (FramesBackToIdle < 0 && Scene.Anim->Speed < M3_028_IdleSpeedLimit
			&& Scene.Anim->PoseIndex == UPrototypeAnimInstance::Pose_Locomotion)
		{
			FramesBackToIdle = Frame;
		}
	}
	UE_LOG(LogTemp, Display, TEXT("UEMMO M3_028 stop window: returned to idle after %d frames (min speed %.2f, final pose %d)"),
		FramesBackToIdle, StopMinSpeed, Scene.Anim->PoseIndex);
	M3_028_LogRuntimeNodeState(TEXT("stopped"), *Scene.Anim);
	TestTrue(TEXT("the character returned to idle after stopping"),
		FramesBackToIdle >= 0 && FramesBackToIdle <= M3_028_StopSampleFrames);
	TestTrue(TEXT("the stopped character exports no residual speed"),
		Scene.Anim->Speed < M3_028_IdleSpeedLimit);
	TestTrue(TEXT("the stopped character stays on the locomotion pose"),
		Scene.Anim->PoseIndex == UPrototypeAnimInstance::Pose_Locomotion);
	return true;
}

// 4. The rendered capture companion (the M3-011/M3-024/M3-027 latent-command
//    precedent): in a real rendered game world the idle stance, the sustained
//    walk and the post-stop idle are captured one screenshot each with a JSON
//    telemetry timeline, so the walking pose can be EYEBALLED against the
//    standing pose. Headless runs skip gracefully; the hard assertions live
//    in the suites above.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_028LocomotionPoseScreenshots,
	"UEMMO.Tasks.M3_028.LocomotionPoseScreenshots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_028LocomotionPoseScreenshots::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	if (World == nullptr || GEngine == nullptr || GEngine->GameViewport == nullptr
		|| World->WorldType != EWorldType::Game || !FApp::CanEverRender())
	{
		AddInfo(TEXT("screenshot capture skipped: no rendered game world/viewport"));
		return true;
	}
	APrototypeCharacter* Player = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0));
	if (Player == nullptr)
	{
		AddInfo(TEXT("screenshot capture skipped: no prototype player in the running world"));
		return true;
	}

	// Latent driver: ping-pongs sustained movement input through the
	// production movement entry (forward half, then backward half) so the
	// walk never runs into room walls, screenshots on a cadence and records
	// one telemetry row per frame into a JSON timeline. The backward leg also
	// exercises a second BlendSpace sample (backward jog).
	struct FM3_028_WalkCaptureLatentCommand : public IAutomationLatentCommand
	{
		APrototypeCharacter* Player;
		FString Directory;
		double DurationSeconds;
		double HalfSeconds;
		double StartSeconds = -1.0;
		int32 ScreenshotCount = 0;
		double LastScreenshotSeconds = -1.0;
		TArray<FVector> PreviousBones;
		TArray<FString> TelemetryRows;
		bool bLoggedStart = false;

		FM3_028_WalkCaptureLatentCommand(APrototypeCharacter* InPlayer, const FString& InDirectory,
			double InDurationSeconds, double InHalfSeconds)
			: Player(InPlayer), Directory(InDirectory), DurationSeconds(InDurationSeconds), HalfSeconds(InHalfSeconds)
		{
		}

		virtual bool Update() override
		{
			if (Player == nullptr)
			{
				return true;
			}
			const double Now = FPlatformTime::Seconds();
			if (StartSeconds < 0.0)
			{
				StartSeconds = Now;
			}
			const double Elapsed = Now - StartSeconds;
			UAnimInstance* NodeProbe = Player->GetMesh() != nullptr ? Player->GetMesh()->GetAnimInstance() : nullptr;
			const UPrototypeAnimInstance* NodeProbeExport = Cast<UPrototypeAnimInstance>(NodeProbe);
			if (NodeProbeExport != nullptr && !bLoggedStart)
			{
				bLoggedStart = true;
				UE::UEMMO::Tasks::M3_028::M3_028_LogRuntimeNodeState(TEXT("real-game walk start"), *NodeProbeExport);
			}
			if (Elapsed >= DurationSeconds)
			{
				if (NodeProbeExport != nullptr)
				{
					UE::UEMMO::Tasks::M3_028::M3_028_LogRuntimeNodeState(TEXT("real-game walk end"), *NodeProbeExport);
				}
				FlushTelemetry();
				return true;
			}
			// Movement input through the production entry, direction flipping
			// once at the midpoint (AddMovementInput's scale mirrors the
			// per-axis planar speed application).
			const bool bSecondHalf = Elapsed >= HalfSeconds;
			Player->AddMovementInput(FVector::ForwardVector, bSecondHalf ? -1.0f : 1.0f);

			// Telemetry row.
			UAnimInstance* Anim = Player->GetMesh() != nullptr ? Player->GetMesh()->GetAnimInstance() : nullptr;
			const UPrototypeAnimInstance* Export = Cast<UPrototypeAnimInstance>(Anim);
			const float Motion = Anim != nullptr && Player->GetMesh() != nullptr
				? FMath::Max(0.0f, UE::UEMMO::Tasks::M3_028::M3_028_BoneMotionDelta(*Player->GetMesh(), PreviousBones))
				: 0.0f;
			const FVector Location = Player->GetActorLocation();
			const FVector Velocity = Player->GetVelocity();
			TelemetryRows.Add(FString::Printf(TEXT(
				"{\"t\":%.3f,\"x\":%.1f,\"y\":%.1f,\"planar\":%.1f,\"speed\":%.1f,\"dir\":%.1f,\"pose\":%d,\"montage\":%d,\"bones\":%.2f}"),
				Elapsed, Location.X, Location.Y, Velocity.Size2D(),
				Export != nullptr ? Export->Speed : -1.0f,
				Export != nullptr ? Export->Direction : 0.0f,
				Export != nullptr ? Export->PoseIndex : -1,
				(Anim != nullptr && Anim->GetCurrentActiveMontage() != nullptr) ? 1 : 0,
				Motion));

			// Screenshot cadence.
			if (LastScreenshotSeconds < 0.0 || Now - LastScreenshotSeconds >= 0.4)
			{
				LastScreenshotSeconds = Now;
				++ScreenshotCount;
				const FString Path = FString::Printf(TEXT("%s/m3-028-move-%02d.png"), *Directory, ScreenshotCount);
				IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree*/ true);
				FScreenshotRequest::RequestScreenshot(Path, /*bShowUI*/ true, /*bAddFilenameSuffix*/ false);
				UE_LOG(LogTemp, Display, TEXT("M3_028 screenshot requested: %s (planar=%.1f speed=%.1f bones=%.2f)"),
					*Path, Velocity.Size2D(), Export != nullptr ? Export->Speed : -1.0f, Motion);
			}
			return false;
		}

	private:
		void FlushTelemetry()
		{
			const FString Path = Directory / TEXT("m3-028-locomotion-capture.json");
			FString Payload = TEXT("{\n  \"rows\": [\n");
			Payload += FString::Join(TelemetryRows, TEXT(",\n"));
			Payload += TEXT("\n  ]\n}");
			FFileHelper::SaveStringToFile(Payload, *Path);
			UE_LOG(LogTemp, Display, TEXT("M3_028 telemetry written: %s (%d rows)"), *Path, TelemetryRows.Num());
		}
	};

	const FString Directory = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() / M3_028_EvidenceDir);
	IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);

	// Idle stance after the boot settles.
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_028_ScreenshotLatentCommand(Directory / TEXT("m3-028-idle.png")));

	// Sustained walk: ping-pong halves of 0.8 s each, screenshots every 0.4 s.
	ADD_LATENT_AUTOMATION_COMMAND(FM3_028_WalkCaptureLatentCommand(Player, Directory, 1.6, 0.8));

	// Back to idle after the stop (the brake is near-instant at 2400 cm/s^2).
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.7f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_028_ScreenshotLatentCommand(Directory / TEXT("m3-028-idle-after.png")));
	return true;
}

#endif
