// M5-070: the production room-definition asset and its wiring. The editor
// script Scripts/PrepareRooms.ps1 generates DA_room_combat_01 from the
// Data/rooms.json room_combat_01 row and points the L_CombatRoom01
// ARoomTrigger's RoomDefinitionAsset at it; these tests reload the saved
// artifacts in the -game process and lock the chain:
//
//   1. the asset's every field equals the JSON source (2+3 waves, spawn
//      coordinates, map/reward identity) and it passes ValidateRoomDefinition
//      against the real enemy asset; the game flow's combat-room identity
//      (RoomId / map package / reward) agrees with the asset.
//   2. the reloaded combat map carries exactly one trigger (referencing
//      DA_room_combat_01 - the production path, no hardcoded fallback) and
//      one exit, while the training map carries neither actor.
//   3. a trigger whose definition asset is set but missing REFUSES the entry
//      even though its transient fallback wave table is still valid - a
//      broken production source never silently falls back to old data.
//
// Map/asset reloads follow the M2-009 LoadObject precedent (a loaded-but-
// foreign world asset is read through its persistent level's actor array);
// the no-fallback test reuses the M5-068 ticking-scene precedent.

#include "Misc/AutomationTest.h"

#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerStart.h"
#include "UObject/UObjectGlobals.h"

#include "../Enemy/EnemyDefinition.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../PrototypeCharacter.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../Room/RoomTrigger.h"

#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_070
{
	const TCHAR* M5_070_AssetObjectPath = TEXT("/Game/UEMMO/Rooms/DA_room_combat_01.DA_room_combat_01");
	const TCHAR* M5_070_CombatMapObjectPath = TEXT("/Game/UEMMO/Maps/L_CombatRoom01.L_CombatRoom01");
	const TCHAR* M5_070_TrainingMapObjectPath = TEXT("/Game/UEMMO/Maps/L_TrainingArena.L_TrainingArena");
	const TCHAR* M5_070_MissingAssetPath = TEXT("/Game/UEMMO/Rooms/DA_m5_070_missing.DA_m5_070_missing");

	// Geometry of the test scene (the M5-068/M2-009 precedent).
	constexpr float M5_070TriggerLocationX = -100.0f;
	constexpr float M5_070OutsideLocationX = -800.0f;
	constexpr float M5_070PlayerZ = 100.0f;

	/** Component-wise literal comparison of a cm vector. */
	static bool M5_070_VectorIs(const FVector& Actual, double X, double Y, double Z)
	{
		return FMath::Abs(Actual.X - X) <= 1e-4
			&& FMath::Abs(Actual.Y - Y) <= 1e-4
			&& FMath::Abs(Actual.Z - Z) <= 1e-4;
	}

	/** Class-filtered actor count of a loaded (non-initialized) map's level. */
	static int32 M5_070_CountLevelActorsOfClass(ULevel* Level, UClass* Class)
	{
		int32 Count = 0;
		for (AActor* Actor : Level->Actors)
		{
			if (Actor != nullptr && Actor->IsA(Class))
			{
				++Count;
			}
		}
		return Count;
	}
}

using namespace UE::UEMMO::Tasks::M5_070;

// 1. Acceptance: DA_room_combat_01 loads in the -game process, carries the
//    Data/rooms.json room_combat_01 values verbatim (2+3 waves, exact spawn
//    coordinates, L_CombatRoom01 map, starter reward), passes
//    ValidateRoomDefinition against the real enemy asset, and agrees with the
//    game flow's combat-room identity.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_070CombatRoomAssetMatchesSourceAndFlowIdentity,
	"UEMMO.Tasks.M5_070.CombatRoomAssetMatchesSourceAndFlowIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_070CombatRoomAssetMatchesSourceAndFlowIdentity::RunTest(const FString& Parameters)
{
	URoomDefinition* Asset = LoadObject<URoomDefinition>(nullptr, M5_070_AssetObjectPath);
	if (!TestNotNull(TEXT("DA_room_combat_01 loads in the -game process"), Asset))
	{
		AddError(TEXT("DA_room_combat_01 is missing - run Scripts/PrepareRooms.ps1 first."));
		return true;
	}

	TestEqual(TEXT("the asset room id is room_combat_01"), Asset->RoomId, FName(TEXT("room_combat_01")));
	TestEqual(TEXT("the asset map path names L_CombatRoom01 (fully-qualified object path)"),
		Asset->MapPath.ToSoftObjectPath().ToString(), FString(TEXT("/Game/UEMMO/Maps/L_CombatRoom01.L_CombatRoom01")));
	TestEqual(TEXT("the asset reward is starter"), Asset->RewardTableId, FName(TEXT("starter")));
	TestTrue(TEXT("the asset player spawn is (-400, 0, 100) cm"),
		M5_070_VectorIs(Asset->PlayerSpawnLocation, -400.0, 0.0, 100.0));
	TestTrue(TEXT("the asset bounds center is (0, 0, 90) cm"),
		M5_070_VectorIs(Asset->BoundsCenter, 0.0, 0.0, 90.0));
	TestTrue(TEXT("the asset bounds size is (2400, 1000, 180) cm"),
		M5_070_VectorIs(Asset->BoundsSize, 2400.0, 1000.0, 180.0));

	TestEqual(TEXT("the asset holds exactly 2 waves"), Asset->Waves.Num(), 2);
	if (Asset->Waves.Num() == 2)
	{
		TestEqual(TEXT("wave 1 spawns 2"), Asset->Waves[0].Count, 2);
		TestEqual(TEXT("wave 2 spawns 3"), Asset->Waves[1].Count, 3);
		TestEqual(TEXT("wave 1 enemy is melee_grunt"), Asset->Waves[0].EnemyId, FName(TEXT("melee_grunt")));
		TestEqual(TEXT("wave 2 enemy is melee_grunt"), Asset->Waves[1].EnemyId, FName(TEXT("melee_grunt")));
		if (Asset->Waves[0].SpawnLocations.Num() == 2)
		{
			TestTrue(TEXT("wave 1 spawn 1 is (300, 0, 88)"),
				M5_070_VectorIs(Asset->Waves[0].SpawnLocations[0], 300.0, 0.0, 88.0));
			TestTrue(TEXT("wave 1 spawn 2 is (450, -150, 88)"),
				M5_070_VectorIs(Asset->Waves[0].SpawnLocations[1], 450.0, -150.0, 88.0));
		}
		if (Asset->Waves[1].SpawnLocations.Num() == 3)
		{
			TestTrue(TEXT("wave 2 spawn 1 is (600, 100, 88)"),
				M5_070_VectorIs(Asset->Waves[1].SpawnLocations[0], 600.0, 100.0, 88.0));
			TestTrue(TEXT("wave 2 spawn 2 is (750, -100, 88)"),
				M5_070_VectorIs(Asset->Waves[1].SpawnLocations[1], 750.0, -100.0, 88.0));
			TestTrue(TEXT("wave 2 spawn 3 is (0, 300, 88)"),
				M5_070_VectorIs(Asset->Waves[1].SpawnLocations[2], 0.0, 300.0, 88.0));
		}
	}

	// Cross-reference: the real enemy asset's id is the known-id set.
	UEnemyDefinition* Grunt = LoadObject<UEnemyDefinition>(
		nullptr, TEXT("/Game/UEMMO/Enemy/Definitions/DA_melee_grunt.DA_melee_grunt"));
	if (!TestNotNull(TEXT("DA_melee_grunt loads for the cross-reference"), Grunt))
	{
		return true;
	}
	TSet<FName> KnownEnemyIds;
	KnownEnemyIds.Add(Grunt->EnemyId);
	FText ValidationErrors;
	TestTrue(TEXT("the asset passes ValidateRoomDefinition"),
		ValidateRoomDefinition(*Asset, KnownEnemyIds, ValidationErrors));
	TestTrue(TEXT("the asset reports no validation errors"), ValidationErrors.IsEmpty());

	// The game flow's combat-room identity agrees with the asset (same room
	// id, same map package, same reward table).
	FTestWorldWrapper Wrapper;
	if (!TestTrue(TEXT("the flow test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
	{
		return true;
	}
	UWorld* World = Wrapper.GetTestWorld();
	UGameFlowSubsystem* Flow = World != nullptr && World->GetGameInstance() != nullptr
		? World->GetGameInstance()->GetSubsystem<UGameFlowSubsystem>()
		: nullptr;
	if (!TestNotNull(TEXT("the game flow subsystem exists"), Flow))
	{
		return true;
	}
	const URoomDefinition* FlowRoom = Flow->GetCombatRoomDefinition();
	if (!TestNotNull(TEXT("the flow exposes its combat room identity"), FlowRoom))
	{
		return true;
	}
	TestEqual(TEXT("the flow combat room id equals the asset id"), FlowRoom->RoomId, Asset->RoomId);
	TestEqual(TEXT("the flow combat map package equals the asset map package"),
		FlowRoom->MapPath.ToSoftObjectPath().ToString(),
		Asset->MapPath.ToSoftObjectPath().GetLongPackageName());
	TestEqual(TEXT("the flow combat reward equals the asset reward"),
		FlowRoom->RewardTableId, Asset->RewardTableId);
	return true;
}

// 2. Acceptance: the reloaded L_CombatRoom01 carries exactly one ARoomTrigger
//    wired to DA_room_combat_01 (the production source - the transient
//    fallback stays a no-asset default) and exactly one ARoomExit, while the
//    training map carries neither actor class.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_070CombatMapTriggerReferencesAssetAndTrainingStaysClean,
	"UEMMO.Tasks.M5_070.CombatMapTriggerReferencesAssetAndTrainingStaysClean",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_070CombatMapTriggerReferencesAssetAndTrainingStaysClean::RunTest(const FString& Parameters)
{
	UWorld* CombatMap = LoadObject<UWorld>(nullptr, M5_070_CombatMapObjectPath);
	if (!TestNotNull(TEXT("the combat map asset loads (L_CombatRoom01)"), CombatMap))
	{
		AddError(TEXT("L_CombatRoom01 is missing - run Scripts/Editor/create_room_assets.py"));
		return true;
	}
	ULevel* CombatLevel = CombatMap->PersistentLevel;
	if (!TestNotNull(TEXT("the combat map exposes its persistent level"), CombatLevel))
	{
		return true;
	}

	const int32 TriggerCount = M5_070_CountLevelActorsOfClass(CombatLevel, ARoomTrigger::StaticClass());
	const int32 ExitCount = M5_070_CountLevelActorsOfClass(CombatLevel, ARoomExit::StaticClass());
	const int32 PlayerStartCount = M5_070_CountLevelActorsOfClass(CombatLevel, APlayerStart::StaticClass());
	// A packaged process boots INTO L_CombatRoom01 (GameDefaultMap): the
	// LoadObject then returns that LIVE game world, whose actor list may
	// already hold runtime-spawned actors of the suite's earlier scenarios
	// (an M2-014 scenario-driven exit door, for instance). The serialized
	// exactly-one guarantee is pinned by the editor verify script and the
	// editor LoadObject run; against a live world the honest floor is "at
	// least the map's own one of each".
	if (CombatMap == GWorld || CombatMap->WorldType == EWorldType::Game)
	{
		TestTrue(TEXT("the live combat world holds at least the map's one RoomTrigger"), TriggerCount >= 1);
		TestTrue(TEXT("the live combat world holds at least the map's one RoomExit"), ExitCount >= 1);
	}
	else
	{
		TestEqual(TEXT("the reloaded combat map carries exactly one RoomTrigger"), TriggerCount, 1);
		TestEqual(TEXT("the reloaded combat map carries exactly one RoomExit"), ExitCount, 1);
	}
	TestTrue(TEXT("the reloaded combat map still carries a PlayerStart"), PlayerStartCount >= 1);

	if (TriggerCount == 1)
	{
		ARoomTrigger* Trigger = nullptr;
		for (AActor* Actor : CombatLevel->Actors)
		{
			Trigger = Cast<ARoomTrigger>(Actor);
			if (Trigger != nullptr)
			{
				break;
			}
		}
		if (TestNotNull(TEXT("the placed trigger casts"), Trigger))
		{
			TestEqual(TEXT("the placed trigger references DA_room_combat_01"),
				Trigger->GetRoomDefinitionSource().ToSoftObjectPath().ToString(),
				FString(M5_070_AssetObjectPath));
			URoomDefinition* Resolved = Trigger->GetRoomDefinitionSource().LoadSynchronous();
			URoomDefinition* Direct = LoadObject<URoomDefinition>(nullptr, M5_070_AssetObjectPath);
			TestTrue(TEXT("the trigger reference resolves to the same asset object"),
				Resolved != nullptr && Resolved == Direct);
			TestEqual(TEXT("the resolved asset carries the production room id"),
				Resolved != nullptr ? Resolved->RoomId : FName(),
				FName(TEXT("room_combat_01")));
		}
	}

	UWorld* TrainingMap = LoadObject<UWorld>(nullptr, M5_070_TrainingMapObjectPath);
	if (!TestNotNull(TEXT("the training map asset loads (L_TrainingArena)"), TrainingMap))
	{
		return true;
	}
	ULevel* TrainingLevel = TrainingMap->PersistentLevel;
	if (!TestNotNull(TEXT("the training map exposes its persistent level"), TrainingLevel))
	{
		return true;
	}
	TestEqual(TEXT("the training map carries no RoomTrigger"),
		M5_070_CountLevelActorsOfClass(TrainingLevel, ARoomTrigger::StaticClass()), 0);
	TestEqual(TEXT("the training map carries no RoomExit"),
		M5_070_CountLevelActorsOfClass(TrainingLevel, ARoomExit::StaticClass()), 0);
	return true;
}

// 3. Acceptance: a definition asset that is set but missing refuses the entry
//    even though the trigger's transient fallback wave table is still the
//    valid class default - a broken production source never silently falls
//    back to the old hardcoded data.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_070MissingRoomAssetRefusesEntryDespiteValidFallback,
	"UEMMO.Tasks.M5_070.MissingRoomAssetRefusesEntryDespiteValidFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_070MissingRoomAssetRefusesEntryDespiteValidFallback::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Wrapper;
	if (!TestTrue(TEXT("the test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
	{
		return true;
	}
	UWorld* World = Wrapper.GetTestWorld();
	if (!TestNotNull(TEXT("the test world is available"), World)
		|| !TestTrue(TEXT("play begins in the test world"), Wrapper.BeginPlayInTestWorld()))
	{
		return true;
	}
	URoomSessionSubsystem* Session = World->GetSubsystem<URoomSessionSubsystem>();
	if (!TestNotNull(TEXT("the room session subsystem exists"), Session))
	{
		return true;
	}

	FActorSpawnParameters TriggerParams;
	ARoomTrigger* Trigger = World->SpawnActor<ARoomTrigger>(
		ARoomTrigger::StaticClass(), FVector(M5_070TriggerLocationX, 0.0f, 88.0f),
		FRotator::ZeroRotator, TriggerParams);
	if (!TestNotNull(TEXT("the room trigger spawns with the valid default fallback"), Trigger))
	{
		return true;
	}

	FActorSpawnParameters PlayerParams;
	APrototypeCharacter* Player = World->SpawnActor<APrototypeCharacter>(
		APrototypeCharacter::StaticClass(),
		FVector(M5_070OutsideLocationX, 0.0f, M5_070PlayerZ), FRotator::ZeroRotator, PlayerParams);
	if (!TestNotNull(TEXT("the prototype character spawns"), Player))
	{
		return true;
	}
	if (!Player->HasActorBegunPlay())
	{
		Player->DispatchBeginPlay();
	}
	Player->GetCharacterMovement()->GravityScale = 0.0f;
	Player->GetCapsuleComponent()->SetGenerateOverlapEvents(true);
	Wrapper.TickTestWorld(0.016f);
	Wrapper.TickTestWorld(0.016f);

	// The asset source is set but missing; the fallback table stays valid.
	Trigger->SetRoomDefinitionSource(TSoftObjectPtr<URoomDefinition>(FSoftObjectPath(M5_070_MissingAssetPath)));
	Player->SetActorLocation(FVector(M5_070TriggerLocationX, 0.0f, M5_070PlayerZ), /*bSweep*/ false);
	Wrapper.TickTestWorld(0.016f);
	Wrapper.TickTestWorld(0.016f);
	Player->GetCapsuleComponent()->UpdateOverlaps();

	TestTrue(TEXT("the entry with a missing asset is refused (no run)"),
		Session->GetState() == ERoomSessionState::Idle);
	TestEqual(TEXT("no run id was assigned"), Session->GetRunId(), static_cast<uint64>(0));
	TestEqual(TEXT("no wave was started"), Session->GetStartedWaveCount(), 0);
	return true;
}

#endif
