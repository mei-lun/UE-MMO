// M3-024: montage visibility gates. The two playtest rounds reported the same
// symptom ("only movement and jump; no X attack, no Z launcher") while the
// combat logs proved every press started an attack instance. The symptom
// decomposes into three separately locked layers:
//   1. Playback layer (the M1-032 wiring, expected green before and after):
//      a real character pressing the attack input through the production
//      entry must have a montage actively playing on its real anim instance
//      with the position advancing. Montages PLAY independently of the graph
//      topology, so a failure here would mean the montage never played at
//      all - a different bug than the reported one.
//   2. Visibility layer (the M3-024 fix, red before / green after): the
//      locomotion AnimBP must contain exactly one DefaultSlot node whose
//      output is the ONLY pose feeding the AnimGraph root. Montage poses
//      render ONLY through a slot node registered in the compiled graph;
//      without one the played montage is invisible (the exact playtest
//      symptom: state machine runs, montage advances, screen shows idle).
//   3. Default-map layer (red before / green after): the game must boot
//      straight into the combat room so attacks have damagable targets (the
//      second feedback round also showed zero hits on the default arena).
#include "Misc/AutomationTest.h"

#include "../Character/PrototypeAnimInstance.h"
#include "../Combat/CombatComponent.h"
#include "../PrototypeCharacter.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "EdGraph/EdGraph.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_024
{
	// ---- shared constants --------------------------------------------------

	// The montage the light_01 attack plays. Naming convention mandated by
	// M1-032 (mirrors UCombatPresentationComponent::FindMontageForAttack):
	// /Game/UEMMO/Animation/Montages/MNT_<AttackId>.MNT_<AttackId>.
	const TCHAR* const M3_024_Light01MontagePath = TEXT("/Game/UEMMO/Animation/Montages/MNT_light_01.MNT_light_01");

	// The montage slot track name (the M1-032 factory output) and the graph
	// slot name the montage must route into: they must match exactly or the
	// played montage has no rendering destination in the AnimGraph.
	const TCHAR* const M3_024_MontageSlotName = TEXT("DefaultSlot");

	// The locomotion AnimBP the M1-031 builder assembles and M3-024 re-runs.
	const TCHAR* const M3_024_AnimBlueprintPath = TEXT("/Game/UEMMO/Animation/ABP_Prototype.ABP_Prototype");

	// The default map the packaged game boots into: the combat room with the
	// activation zone and damagable enemies (the M3-024 GameDefaultMap change).
	const TCHAR* const M3_024_DefaultMapPath = TEXT("/Game/UEMMO/Maps/L_CombatRoom01");

#if WITH_EDITOR
	// ---- graph census helpers (game-target compilable: node classes are
	// identified by class name; the editor-only AnimGraph module headers are
	// not includable from this module's game build) --------------------------

	struct FM3_024_GraphCensus
	{
		UEdGraph* AnimGraph = nullptr;
		UEdGraphNode* RootNode = nullptr;
		UObject* SlotNode = nullptr;
		int32 SlotNodes = 0;
		int32 RootNodes = 0;
		int32 BlendListNodes = 0;
		int32 BlendSpacePlayers = 0;
		int32 SequencePlayers = 0;
	};

	static bool M3_024_CollectGraphCensus(FAutomationTestBase& Test, FM3_024_GraphCensus& Out)
	{
		UBlueprint* AnimBP = LoadObject<UBlueprint>(nullptr, M3_024_AnimBlueprintPath);
		if (!Test.TestNotNull(TEXT("the locomotion AnimBP loads from disk"), AnimBP))
		{
			return false;
		}
		for (UEdGraph* Graph : AnimBP->FunctionGraphs)
		{
			if (Graph != nullptr && Graph->GetFName() == TEXT("AnimGraph"))
			{
				Out.AnimGraph = Graph;
				break;
			}
		}
		if (!Test.TestNotNull(TEXT("the AnimBP carries an AnimGraph"), Out.AnimGraph))
		{
			return false;
		}
		for (UEdGraphNode* Node : Out.AnimGraph->Nodes)
		{
			if (Node == nullptr)
			{
				continue;
			}
			const FName ClassName = Node->GetClass()->GetFName();
			if (ClassName == FName(TEXT("AnimGraphNode_Slot")))
			{
				++Out.SlotNodes;
				Out.SlotNode = Node;
			}
			else if (ClassName == FName(TEXT("AnimGraphNode_Root")))
			{
				++Out.RootNodes;
				Out.RootNode = Node;
			}
			else if (ClassName == FName(TEXT("AnimGraphNode_BlendListByInt")))
			{
				++Out.BlendListNodes;
			}
			else if (ClassName == FName(TEXT("AnimGraphNode_BlendSpacePlayer")))
			{
				++Out.BlendSpacePlayers;
			}
			else if (ClassName == FName(TEXT("AnimGraphNode_SequencePlayer")))
			{
				++Out.SequencePlayers;
			}
		}
		UE_LOG(LogTemp, Display, TEXT(
			"UEMMO M3_024 AnimGraph census: Slot=%d Root=%d BlendListByInt=%d BlendSpacePlayer=%d SequencePlayer=%d"),
			Out.SlotNodes, Out.RootNodes, Out.BlendListNodes, Out.BlendSpacePlayers, Out.SequencePlayers);
		return true;
	}

	// Reads Node.SlotName from a slot graph node through reflection (the
	// editor graph node class itself is not includable from game targets).
	static FName M3_024_ReadSlotNodeName(UObject* SlotNode)
	{
		if (SlotNode == nullptr)
		{
			return NAME_None;
		}
		FStructProperty* NodeProperty = CastField<FStructProperty>(SlotNode->GetClass()->FindPropertyByName(TEXT("Node")));
		if (NodeProperty == nullptr)
		{
			return NAME_None;
		}
		FNameProperty* SlotNameProperty = CastField<FNameProperty>(NodeProperty->Struct->FindPropertyByName(TEXT("SlotName")));
		if (SlotNameProperty == nullptr)
		{
			return NAME_None;
		}
		const void* NodePtr = NodeProperty->ContainerPtrToValuePtr<void>(SlotNode);
		return *reinterpret_cast<const FName*>(SlotNameProperty->ContainerPtrToValuePtr<void>(NodePtr));
	}

	// ---- in-loop scene (the M1-041 real-character pattern, player only) ----

	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M3_024_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with arena
	// content (the M1-022/M1-027 placement convention). X horizontal, Y
	// depth, Z height; the floor top sits exactly at the base Z.
	const FVector M3_024_SceneBase(40000.0, 45000.0, 600.0);

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M3_024_FloorHalfThickness = 100.0f;
	const float M3_024_FloorHalfExtentXY = 4000.0f;

	// Spawn height above the floor top: the character drops a short distance
	// through real no-controller physics and settles onto the floor.
	const float M3_024_PlayerSpawnHeight = 120.0f;

	// Frame caps: the settle phase waits for the ground contact; the playback
	// phase covers the light_01 montage window (26 logic frames = 0.433 s)
	// several times over so the observation never races the blend-in.
	constexpr int32 M3_024_MaxSettleFrames = 300;
	constexpr int32 M3_024_MaxPlaybackFrames = 150;

	static APrototypeCharacter* M3_024_SpawnPlayer(UWorld& World, const FRotator& Facing)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			M3_024_SceneBase + FVector(0.0, 0.0, M3_024_PlayerSpawnHeight),
			Facing, Params);
		if (Player == nullptr)
		{
			return nullptr;
		}
		if (UCharacterMovementComponent* Movement = Player->GetCharacterMovement())
		{
			// The pawn spawned into an already begun world; mirror the real
			// arena initialization (the M1-041 pattern) so the movement
			// component runs without a controller and settles on the floor.
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
		return Player;
	}

	static AActor* M3_024_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base - FVector(0.0, 0.0, M3_024_FloorHalfThickness), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M3_024_Floor"));
		Actor->SetRootComponent(Floor);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->SetBoxExtent(FVector(M3_024_FloorHalfExtentXY, M3_024_FloorHalfExtentXY, M3_024_FloorHalfThickness));
		Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Floor->RegisterComponent();
		// The root was assigned after the spawn transform was applied, so the
		// world location is re-applied explicitly (the M1-022 pattern).
		Floor->SetWorldLocation(Base - FVector(0.0, 0.0, M3_024_FloorHalfThickness));
		return Actor;
	}

	// ---- staged capture helpers (the M3-011 latent-command precedent) ------

	// Submits one attack press through the production intent entry (the exact
	// target of the real X binding) from the game loop.
	struct FM3_024_PressAttackLatentCommand : public IAutomationLatentCommand
	{
		TWeakObjectPtr<APrototypeCharacter> PlayerPtr;

		explicit FM3_024_PressAttackLatentCommand(APrototypeCharacter* InPlayer)
			: PlayerPtr(InPlayer)
		{
		}

		virtual bool Update() override
		{
			APrototypeCharacter* Player = PlayerPtr.Get();
			if (Player == nullptr)
			{
				UE_LOG(LogTemp, Warning, TEXT("M3_024 capture staging skipped (player gone)"));
				return true;
			}
			Player->SubmitCombatInput(ECombatInput::Light);
			UE_LOG(LogTemp, Display, TEXT("M3_024 staged attack press submitted"));
			return true;
		}
	};

	// Logs the montage state at the capture instant (screenshot evidence
	// companion; the hard in-loop assertions live in the headless suites).
	struct FM3_024_MontageProbeLatentCommand : public IAutomationLatentCommand
	{
		TWeakObjectPtr<APrototypeCharacter> PlayerPtr;

		explicit FM3_024_MontageProbeLatentCommand(APrototypeCharacter* InPlayer)
			: PlayerPtr(InPlayer)
		{
		}

		virtual bool Update() override
		{
			APrototypeCharacter* Player = PlayerPtr.Get();
			UAnimInstance* Anim = Player != nullptr && Player->GetMesh() != nullptr
				? Player->GetMesh()->GetAnimInstance()
				: nullptr;
			const UAnimMontage* Active = Anim != nullptr ? Anim->GetCurrentActiveMontage() : nullptr;
			UE_LOG(LogTemp, Display, TEXT(
				"M3_024 montage probe: animInstance=%s activeMontage=%s playing=%d position=%.3f"),
				Anim != nullptr ? *Anim->GetClass()->GetName() : TEXT("none"),
				Active != nullptr ? *Active->GetName() : TEXT("none"),
				(Anim != nullptr && Active != nullptr && Anim->Montage_IsPlaying(Active)) ? 1 : 0,
				(Anim != nullptr && Active != nullptr) ? Anim->Montage_GetPosition(Active) : -1.0f);
			return true;
		}
	};

	// Presses the production X entry on a cadence until the montage is active
	// on the real anim instance (the world ticks between latent updates).
	// The -game automation clock can jump hundreds of milliseconds in one
	// frame right after the startup stall, which ages a single staged press
	// past the 150 ms buffered-input lifetime before any combat tick can act
	// on it; a real 60 fps keyboard press is consumed the next frame (the
	// user's playtest logs show exactly that). The cadence loop reproduces a
	// human mash and is only for the rendered capture companion; the hard
	// single-press in-loop assertions live in the headless suites.
	struct FM3_024_WaitMontageActiveLatentCommand : public IAutomationLatentCommand
	{
		TWeakObjectPtr<APrototypeCharacter> PlayerPtr;
		double TimeoutSeconds = 5.0;
		double PressIntervalSeconds = 0.25;
		double StartedSeconds = 0.0;
		double LastPressSeconds = -1.0;
		bool bStarted = false;
		int32 UpdateCount = 0;
		int32 PressCount = 0;

		explicit FM3_024_WaitMontageActiveLatentCommand(APrototypeCharacter* InPlayer)
			: PlayerPtr(InPlayer)
		{
		}

		virtual bool Update() override
		{
			const double Now = FPlatformTime::Seconds();
			if (!bStarted)
			{
				bStarted = true;
				StartedSeconds = Now;
			}
			APrototypeCharacter* Player = PlayerPtr.Get();
			UAnimInstance* Anim = Player != nullptr && Player->GetMesh() != nullptr
				? Player->GetMesh()->GetAnimInstance()
				: nullptr;
			const UAnimMontage* Active = Anim != nullptr ? Anim->GetCurrentActiveMontage() : nullptr;
			if (Active != nullptr)
			{
				UE_LOG(LogTemp, Display, TEXT(
					"M3_024 capture poll: montage ACTIVE after %.2fs after %d staged presses (%s at position %.3f)"),
					Now - StartedSeconds, PressCount, *Active->GetName(),
					Anim->Montage_GetPosition(Active));
				return true;
			}
			// Cadence press: one staged X per interval while no montage plays.
			if (Player != nullptr && Now - LastPressSeconds >= PressIntervalSeconds)
			{
				LastPressSeconds = Now;
				++PressCount;
				Player->SubmitCombatInput(ECombatInput::Light);
			}
			// Per-update telemetry for the first seconds: buffer contents, clock
			// continuity and state, so a silently rejected/dropped intent or a
			// non-ticking combat walk is visible in the capture log.
			if (UpdateCount < 120)
			{
				++UpdateCount;
				UCombatComponent* CombatTelemetry = Player != nullptr ? Player->GetCombat() : nullptr;
				FBufferedCombatInput BufferedTelemetry;
				const bool bBufferedTelemetry = CombatTelemetry != nullptr
					&& CombatTelemetry->PeekInputBuffer(BufferedTelemetry, 0);
				UE_LOG(LogTemp, Display, TEXT(
					"M3_024 poll tick %d: clock=%.2f state=%d buffer0=%d seq=%llu pressedAt=%.2f presses=%d montage=%s"),
					UpdateCount,
					CombatTelemetry != nullptr ? CombatTelemetry->GetInputClockSeconds() : -1.0,
					CombatTelemetry != nullptr ? static_cast<int32>(CombatTelemetry->GetSnapshot().ActionState) : -1,
					bBufferedTelemetry ? 1 : 0,
					bBufferedTelemetry ? BufferedTelemetry.Sequence : 0ULL,
					bBufferedTelemetry ? BufferedTelemetry.PressedAt : -1.0,
					PressCount,
					Active != nullptr ? TEXT("yes") : TEXT("no"));
			}
			if (Now - StartedSeconds >= TimeoutSeconds)
			{
				UCombatComponent* Combat = Player != nullptr ? Player->GetCombat() : nullptr;
				const FCombatSnapshot Snapshot = Combat != nullptr ? Combat->GetSnapshot() : FCombatSnapshot();
				// Inside view: is the staged intent still buffered (silent
				// Push rejection vs. silent walk skip) and does the attached
				// catalog resolve the mapped attack ids?
				FBufferedCombatInput Buffered;
				const bool bIntentBuffered = Combat != nullptr && Combat->PeekInputBuffer(Buffered, 0);
				UAttackCatalog* Catalog = nullptr;
				for (TObjectIterator<UAttackCatalog> It; It; ++It)
				{
					UAttackCatalog* Candidate = *It;
					if (Candidate != nullptr && Player != nullptr
						&& Candidate->GetOuter() == Player
						&& !Candidate->HasAnyFlags(RF_ClassDefaultObject))
					{
						Catalog = Candidate;
						break;
					}
				}
				UE_LOG(LogTemp, Warning, TEXT(
					"M3_024 capture poll: montage never active within %.1fs (combat=%s actionState=%d inputClock=%.2f "
					"dead=%d snapshotAttack=%s snapshotFrame=%d intentBuffered=%d bufferedAction=%d bufferedPressedAt=%.2f "
					"catalog=%s findLight01=%d findAerial01=%d falling=%d)"),
					TimeoutSeconds,
					Combat != nullptr ? TEXT("present") : TEXT("missing"),
					Combat != nullptr ? static_cast<int32>(Snapshot.ActionState) : -1,
					Combat != nullptr ? Combat->GetInputClockSeconds() : -1.0,
					Combat != nullptr && Combat->IsDead() ? 1 : 0,
					*Snapshot.AttackId.ToString(),
					Snapshot.Frame,
					bIntentBuffered ? 1 : 0,
					bIntentBuffered ? static_cast<int32>(Buffered.Action) : -1,
					bIntentBuffered ? Buffered.PressedAt : -1.0,
					Catalog != nullptr ? TEXT("found") : TEXT("missing"),
					Catalog != nullptr && Catalog->Find(FName(TEXT("light_01"))) != nullptr ? 1 : 0,
					Catalog != nullptr && Catalog->Find(FName(TEXT("aerial_01"))) != nullptr ? 1 : 0,
					Player != nullptr && Player->GetCharacterMovement() != nullptr
						&& Player->GetCharacterMovement()->IsFalling() ? 1 : 0);
				return true;
			}
			return false;
		}
	};

	// Requests one screenshot (the state was staged before it runs). Creates
	// the target directory first; skips gracefully without rendering.
	struct FM3_024_ScreenshotLatentCommand : public IAutomationLatentCommand
	{
		FString AbsolutePath;

		explicit FM3_024_ScreenshotLatentCommand(const FString& InAbsolutePath)
			: AbsolutePath(InAbsolutePath)
		{
		}

		virtual bool Update() override
		{
			if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
			{
				UE_LOG(LogTemp, Display, TEXT("M3_024 screenshot skipped (no rendering): %s"), *AbsolutePath);
				return true;
			}
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsolutePath), /*Tree*/ true);
			FScreenshotRequest::RequestScreenshot(AbsolutePath, /*bShowUI*/ true, /*bAddFilenameSuffix*/ false);
			UE_LOG(LogTemp, Display, TEXT("M3_024 screenshot requested: %s"), *AbsolutePath);
			return true;
		}
	};
}
#endif // WITH_EDITOR (census uses editor-only graph APIs)

using namespace UE::UEMMO::Tasks::M3_024;

// 1. The visibility gate: the AnimBP graph must contain exactly one slot
//    node named like the montage slot track, and that slot must be the ONLY
//    pose feeding the AnimGraph root (no bypass: the locomotion output goes
//    through the slot, so played montages replace the locomotion pose).
//    Red on the pre-M3-024 asset (zero slot nodes = montages invisible).
#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_024AnimGraphRoutesMontageSlotToRoot,
	"UEMMO.Tasks.M3_024.AnimGraphRoutesMontageSlotToRoot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_024AnimGraphRoutesMontageSlotToRoot::RunTest(const FString& Parameters)
{
	FM3_024_GraphCensus Census;
	if (!M3_024_CollectGraphCensus(*this, Census))
	{
		return true;
	}

	// The montage rendering destination exists exactly once (a second build
	// of the graph must never duplicate the slot).
	TestEqual(TEXT("the AnimGraph contains exactly one montage slot node"), Census.SlotNodes, 1);
	// The rest of the M1-031 graph stays intact (regression lock).
	TestEqual(TEXT("the AnimGraph keeps exactly one root node"), Census.RootNodes, 1);
	TestEqual(TEXT("the AnimGraph keeps exactly one pose blend list"), Census.BlendListNodes, 1);
	TestEqual(TEXT("the AnimGraph keeps exactly one locomotion BlendSpace player"), Census.BlendSpacePlayers, 1);
	TestEqual(TEXT("the AnimGraph keeps exactly three air/land sequence players"), Census.SequencePlayers, 3);
	if (Census.SlotNodes != 1 || Census.RootNodes != 1)
	{
		return true;
	}

	// The slot carries the montage slot track name (M1-032 montages play into
	// DefaultSlot; a renamed slot would silently orphan every montage).
	TestEqual(TEXT("the slot node carries the montage slot track name"),
		M3_024_ReadSlotNodeName(Census.SlotNode), FName(M3_024_MontageSlotName));

	// The root's Result input is fed exclusively by the slot output: no
	// bypass link may skip the slot (that would resurrect the invisibility
	// while the census still looks correct).
	UEdGraphPin* RootInput = Census.RootNode->FindPin(TEXT("Result"), EGPD_Input);
	if (!TestNotNull(TEXT("the AnimGraph root has a Result input pin"), RootInput))
	{
		return true;
	}
	TestEqual(TEXT("the root pose has exactly one source"), RootInput->LinkedTo.Num(), 1);
	TestTrue(TEXT("the root pose comes from the montage slot node"),
		RootInput->LinkedTo.Num() == 1 && RootInput->LinkedTo[0] != nullptr
		&& RootInput->LinkedTo[0]->GetOwningNode() == Census.SlotNode);

	// The slot input is fed (by the locomotion pose blend list), so the slot
	// passthrough has a source to fall back to between montages.
	UEdGraphNode* SlotGraphNode = Cast<UEdGraphNode>(Census.SlotNode);
	if (!TestNotNull(TEXT("the slot node is a graph node"), SlotGraphNode))
	{
		return true;
	}
	UEdGraphPin* SlotInput = SlotGraphNode->FindPin(TEXT("Source"), EGPD_Input);
	if (!TestNotNull(TEXT("the slot node has a Source input pin"), SlotInput))
	{
		return true;
	}
	TestTrue(TEXT("the slot input is fed by the locomotion blend"), SlotInput->LinkedTo.Num() >= 1);

	// The montage side of the contract: the light_01 montage plays into the
	// same slot name the graph exposes (name match = the montage routes into
	// the slot when it plays).
	UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, M3_024_Light01MontagePath);
	if (!TestNotNull(TEXT("the light_01 montage loads"), Montage))
	{
		return true;
	}
	TestTrue(TEXT("the light_01 montage has a slot track"), Montage->SlotAnimTracks.Num() >= 1);
	if (Montage->SlotAnimTracks.Num() >= 1)
	{
		TestEqual(TEXT("the light_01 montage plays into the graph's slot name"),
			Montage->SlotAnimTracks[0].SlotName, FName(M3_024_MontageSlotName));
	}
	return true;
}
#endif // WITH_EDITOR (graph census test)

// 2. The playback layer (expected green before and after the M3-024 fix): a
//    real character pressing X through the production entry has the light_01
//    montage actively playing on its real anim instance and the montage
//    position advances through real ticks. Locks the M1-032 wiring so a
//    future "state machine runs but montage never plays" regression fails
//    here instead of reaching a playtest round.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_024RealCharacterMontagePlaysDuringAttack,
	"UEMMO.Tasks.M3_024.RealCharacterMontagePlaysDuringAttack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_024RealCharacterMontagePlaysDuringAttack::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Wrapper;
	if (!TestTrue(TEXT("the manually ticked test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
	{
		return true;
	}
	UWorld* World = Wrapper.GetTestWorld();
	if (!TestNotNull(TEXT("the test world is available"), World))
	{
		return true;
	}
	if (!TestTrue(TEXT("play begins in the test world"), Wrapper.BeginPlayInTestWorld()))
	{
		return true;
	}
	AActor* Floor = M3_024_SpawnFloor(*World, M3_024_SceneBase);
	if (!TestNotNull(TEXT("the floor spawns"), Floor))
	{
		return true;
	}
	World->EnsureCollisionTreeIsBuilt();
	APrototypeCharacter* Player = M3_024_SpawnPlayer(*World, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the real prototype character spawns"), Player))
	{
		return true;
	}

	// Settle: the character stands grounded on the floor (real walking
	// physics, the M1-041 settle gate) before the attack press.
	bool bSettled = false;
	for (int32 Frame = 0; Frame < M3_024_MaxSettleFrames && !bSettled; ++Frame)
	{
		if (!TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M3_024_FrameSeconds)))
		{
			return true;
		}
		const UCharacterMovementComponent* Movement = Player->GetCharacterMovement();
		bSettled = Movement != nullptr
			&& Movement->MovementMode == MOVE_Walking
			&& Movement->Velocity.Size() < 1.0f;
	}
	TestTrue(TEXT("the character settles grounded on the floor before the attack"), bSettled);

	// The real AnimBP drives the mesh (the constructor class finder wired
	// /Game/UEMMO/Animation/ABP_Prototype; a native fallback would not prove
	// the montage routes through the shipped graph).
	USkeletalMeshComponent* Mesh = Player->GetMesh();
	UAnimInstance* Anim = Mesh != nullptr ? Mesh->GetAnimInstance() : nullptr;
	if (!TestNotNull(TEXT("the character mesh runs an anim instance"), Anim))
	{
		return true;
	}
	TestTrue(TEXT("the anim instance is the real UPrototypeAnimInstance AnimBP"),
		Anim->IsA<UPrototypeAnimInstance>());

	// The light_01 montage loads (the M1-032 factory output, the same asset
	// the presentation component resolves for the light_01 attack).
	UAnimMontage* Montage = LoadObject<UAnimMontage>(nullptr, M3_024_Light01MontagePath);
	if (!TestNotNull(TEXT("the light_01 montage loads"), Montage))
	{
		return true;
	}
	TestTrue(TEXT("the montage has a positive composite length"), Montage->GetPlayLength() > 0.0f);

	// The production intent entry (the exact target of the real X binding).
	Player->SubmitCombatInput(ECombatInput::Light);

	// Tick the world and observe the montage through the anim instance: the
	// montage must become active (still playing / the reported active
	// montage) and its position must advance through real anim ticks.
	bool bMontageSeen = false;
	bool bActivePointerSeen = false;
	float FirstObservedPosition = -1.0f;
	float MaxObservedPosition = -1.0f;
	for (int32 Frame = 0; Frame < M3_024_MaxPlaybackFrames; ++Frame)
	{
		if (!TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M3_024_FrameSeconds)))
		{
			break;
		}
		const UAnimMontage* CurrentActive = Anim->GetCurrentActiveMontage();
		if (CurrentActive == Montage)
		{
			bActivePointerSeen = true;
		}
		if (Anim->Montage_IsPlaying(Montage) || CurrentActive == Montage)
		{
			bMontageSeen = true;
			const float Position = Anim->Montage_GetPosition(Montage);
			if (FirstObservedPosition < 0.0f)
			{
				FirstObservedPosition = Position;
			}
			MaxObservedPosition = FMath::Max(MaxObservedPosition, Position);
			UE_LOG(LogTemp, Display, TEXT("UEMMO M3_024 montage playing: frame=%d position=%.3f"),
				Frame, Position);
		}
	}
	TestTrue(TEXT("the light_01 montage became active on the real anim instance after the X press"), bMontageSeen);
	TestTrue(TEXT("GetCurrentActiveMontage reported the light_01 montage while it played"), bActivePointerSeen);
	TestTrue(TEXT("the montage position advanced through the playback (a state that runs without playing is the M3-024 regression shape)"),
		MaxObservedPosition > FirstObservedPosition + 0.01f);
	return true;
}

// 3. The default-map gate (red before / green after): the engine's
//    GameDefaultMap points into the combat room, so the packaged game boots
//    straight into a map with the activation zone and damagable enemies
//    (the second feedback round: zero hits because the arena had no enemy).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_024GameDefaultMapBootsIntoCombatRoom,
	"UEMMO.Tasks.M3_024.GameDefaultMapBootsIntoCombatRoom",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_024GameDefaultMapBootsIntoCombatRoom::RunTest(const FString& Parameters)
{
	FString GameDefaultMap;
	GConfig->GetString(TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("GameDefaultMap"),
		GameDefaultMap, GEngineIni);
	TestEqual(TEXT("the engine config boots the game into the combat room"),
		GameDefaultMap, FString(M3_024_DefaultMapPath));

	// The configured default map actually exists in the project content (a
	// config pointing at a missing map would break every -game boot).
	const FString MapFile = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("UEMMO/Maps/L_CombatRoom01.umap"));
	TestTrue(TEXT("the combat room map package exists in project content"),
		IFileManager::Get().FileExists(*MapFile));
	return true;
}

// 4. Capture companion (the M3-011 precedent): in a real rendered game world
//    the staged locomotion state and the mid-montage attack state are
//    captured one screenshot each, so the fix can be EYEBALLED as a pose
//    difference (punch vs. locomotion stance). Headless runs skip silently;
//    the assertions live in the suites above.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_024AttackPoseScreenshots,
	"UEMMO.Tasks.M3_024.AttackPoseScreenshots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_024AttackPoseScreenshots::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	if (World == nullptr || GEngine == nullptr || GEngine->GameViewport == nullptr
		|| World->WorldType != EWorldType::Game)
	{
		AddInfo(TEXT("screenshot capture skipped: no running game world/viewport"));
		return true;
	}
	APrototypeCharacter* Player = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0));
	if (Player == nullptr)
	{
		AddInfo(TEXT("screenshot capture skipped: no prototype player in the running world"));
		return true;
	}

	const FString Directory = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M3-024"));

	// Locomotion state: the idle stance after the boot settles (screenshot 1;
	// kept short so the staged attack cannot be overtaken by room combat
	// events like an enemy swarm killing the static player).
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_024_ScreenshotLatentCommand(Directory / TEXT("attack-locomotion.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.3f));

	// Attack state: the cadence loop stages production X presses until the
	// montage is actually active on the anim instance; a short hold then lets
	// the montage reach its extended-punch range before the probe and the
	// capture (light_01 plays ~0.43 s real time; 0.15 s in is mid-punch).
	ADD_LATENT_AUTOMATION_COMMAND(FM3_024_WaitMontageActiveLatentCommand(Player));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.15f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_024_MontageProbeLatentCommand(Player));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_024_ScreenshotLatentCommand(Directory / TEXT("attack-montage.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
	return true;
}

#endif
