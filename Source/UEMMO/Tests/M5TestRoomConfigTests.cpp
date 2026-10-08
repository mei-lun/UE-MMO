// M5-018A: the config-driven system test room. Pins the test_room source
// table parser (strict schema, the empty-vehicles-legal rule, the explicit
// not-implemented refusal for a future vehicle list), the driver's spawn
// loop (target health/policy/location come from the config, an unknown enemy
// or policy id fails explicitly and names the target, no stand-in actor) and
// the menu's second legal entry (the system test room definition travels to
// L_SystemTestRoom without a console command).

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/TestRoomConfigDriver.h"
#include "../Combat/System/ReactionResolver.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Profile/GameFlowSubsystem.h"

#include "Components/CapsuleComponent.h"
#include "EngineUtils.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_018A
{
	// A minimal valid room: three targets across the three M5-003 sample
	// policies, the future vehicle list empty (legal).
	const TCHAR* const M5_018A_ValidConfig = TEXT(R"({
		"schema_version": 1,
		"table": "test_room",
		"rows": [
			{
				"room_id": "room_test_fixture",
				"targets": [
					{"target_id": "dummy_normal", "enemy_id": "melee_grunt", "max_health": 80,
					 "policy_id": "normal", "location_cm": {"x": 300, "y": 0, "z": 88}},
					{"target_id": "dummy_heavy", "enemy_id": "melee_grunt", "max_health": 120,
					 "policy_id": "heavy", "location_cm": {"x": 550, "y": 150, "z": 88}}
				],
				"vehicle_spawns": [],
				"reward_pool_id": "starter"
			}
		]
	})");
}

using namespace UE::UEMMO::Tasks::M5_018A;

// ---------------------------------------------------------------------------
// ParserRefusesMalformedConfigs
// ---------------------------------------------------------------------------

// The parser is strict: the schema version, the table name, the single row,
// the target ids, the positive health and the location all validate, and the
// refusals name their field. The future vehicle list is legal only empty.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_018AParserRefusesMalformedConfigs,
	"UEMMO.Tasks.M5_018A.ParserRefusesMalformedConfigs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_018AParserRefusesMalformedConfigs::RunTest(const FString& Parameters)
{
	// 1. The valid fixture parses with the full config.
	{
		FTestRoomConfig Config;
		FString Error;
		TestTrue("the valid fixture parses", ACombatTestRoomDriver::ParseTestRoomConfig(M5_018A_ValidConfig, Config, Error));
		TestEqual("the room id parses", Config.RoomId, FName(TEXT("room_test_fixture")));
		TestEqual("both targets parse", Config.Targets.Num(), 2);
		TestEqual("the first target's health comes from the config", Config.Targets[0].MaxHealth, 80.0f);
		TestEqual("the empty vehicle list stays legal", Config.VehicleSpawnIds.Num(), 0);
		TestEqual("the reward pool id parses", Config.RewardPoolId, FName(TEXT("starter")));
	}

	// 2. A non-empty vehicle list refuses with the explicit not-implemented
	//    error (the M5-037+ capability; never a stand-in).
	{
		const TCHAR* Vehicles = TEXT(R"({
			"schema_version": 1, "table": "test_room",
			"rows": [{"room_id": "room_v", "targets": [{"target_id": "t", "enemy_id": "melee_grunt",
				"max_health": 10, "policy_id": "normal", "location_cm": {"x": 0, "y": 0, "z": 0}}],
				"vehicle_spawns": ["jeep_one"]}]
		})");
		FTestRoomConfig Config;
		FString Error;
		TestFalse("the non-empty vehicle list refuses", ACombatTestRoomDriver::ParseTestRoomConfig(Vehicles, Config, Error));
		TestTrue("the refusal names the not-implemented capability",
			Error.Contains(TEXT("not implemented")));
	}

	// 3. Schema/table/row-shape refusals name their field.
	{
		FTestRoomConfig Config;
		FString Error;
		TestFalse("a wrong schema version refuses",
			ACombatTestRoomDriver::ParseTestRoomConfig(TEXT(R"({"schema_version": 2, "table": "test_room", "rows": []})"), Config, Error));
		TestTrue("the schema refusal names the field", Error.Contains(TEXT("schema_version")));

		TestFalse("a wrong table name refuses",
			ACombatTestRoomDriver::ParseTestRoomConfig(TEXT(R"({"schema_version": 1, "table": "weapons", "rows": []})"), Config, Error));
		TestTrue("the table refusal names the field", Error.Contains(TEXT("test_room")));

		const TCHAR* ZeroHealth = TEXT(R"({
			"schema_version": 1, "table": "test_room",
			"rows": [{"room_id": "r", "targets": [{"target_id": "t", "enemy_id": "melee_grunt",
				"max_health": 0, "policy_id": "normal", "location_cm": {"x": 0, "y": 0, "z": 0}}]}]
		})");
		TestFalse("a zero max health refuses",
			ACombatTestRoomDriver::ParseTestRoomConfig(ZeroHealth, Config, Error));
		TestTrue("the health refusal names the field", Error.Contains(TEXT("max_health")));

		const TCHAR* BadId = TEXT(R"({
			"schema_version": 1, "table": "test_room",
			"rows": [{"room_id": "Bad Id!", "targets": [{"target_id": "t", "enemy_id": "melee_grunt",
				"max_health": 10, "policy_id": "normal", "location_cm": {"x": 0, "y": 0, "z": 0}}]}]
		})");
		TestFalse("an illegal room id refuses",
			ACombatTestRoomDriver::ParseTestRoomConfig(BadId, Config, Error));
		TestTrue("the id refusal names the field", Error.Contains(TEXT("room_id")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// DriverSpawnsTargetsFromConfig
// ---------------------------------------------------------------------------

// The spawn loop applies the configured identity, health, policy and
// location to every target: a driver with the fixture spawns two melee
// enemies at the configured locations, the configured max health rides on
// the health component and the heavy policy lands on the target's combat
// component (launch-immune through the M5-003 heavy sample row).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_018ADriverSpawnsTargetsFromConfig,
	"UEMMO.Tasks.M5_018A.DriverSpawnsTargetsFromConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_018ADriverSpawnsTargetsFromConfig::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	if (!TestTrue(TEXT("a world is available"), World != nullptr))
	{
		return true;
	}

	ACombatTestRoomDriver* Driver = NewObject<ACombatTestRoomDriver>();
	FString Error;
	if (!TestTrue(TEXT("the fixture config parses into the driver"),
		ACombatTestRoomDriver::ParseTestRoomConfig(M5_018A_ValidConfig, Driver->GetConfigMutable(), Error)))
	{
		AddError(Error);
		return true;
	}

	// Remote base so the spawns never collide with arena content; the
	// configured locations are room-local and applied verbatim.
	TArray<FName> MissingIds;
	const int32 Spawned = Driver->SpawnTargets(*World, MissingIds);
	if (!TestEqual("both configured targets spawned", Spawned, 2))
	{
		return true;
	}
	TestEqual("no target id failed to resolve", MissingIds.Num(), 0);

	// The spawned targets carry the configured values (the definitions load
	// from the M2 enemy definitions; the health and the policy come from the
	// config, the location from the table verbatim).
	TArray<AMeleeEnemy*> Enemies;
	for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
	{
		Enemies.Add(*It);
	}
	if (!TestEqual("the world holds exactly the spawned targets", Enemies.Num(), 2))
	{
		return true;
	}

	bool bFoundHeavyAtConfiguredLocation = false;
	for (AMeleeEnemy* Enemy : Enemies)
	{
		const UHealthComponent* Health = Enemy->GetHealthComponent();
		if (Health == nullptr)
		{
			continue;
		}
		const UCombatComponent* Combat = Enemy->GetCombatComponent();
		const FVector Location = Enemy->GetActorLocation();
		if (FMath::IsNearlyEqual(Health->GetMaxHealth(), 120.0f)
			&& Combat != nullptr
			&& Combat->GetTargetReactionPolicy().PolicyId == FName(TEXT("heavy"))
			&& Combat->GetTargetReactionPolicy().bAllowLaunch == false
			&& FMath::Abs(Location.X - 550.0f) < 1.0f
			&& FMath::Abs(Location.Y - 150.0f) < 1.0f)
		{
			bFoundHeavyAtConfiguredLocation = true;
		}
	}
	TestTrue("the heavy target carries the configured health, policy and location",
		bFoundHeavyAtConfiguredLocation);

	// Cleanup: destroy the spawned actors (the shared world stays clean for
	// the suites that run after this one).
	for (AMeleeEnemy* Enemy : Enemies)
	{
		Enemy->Destroy();
	}
	Driver->Destroy();
	return true;
}

// ---------------------------------------------------------------------------
// UnknownIdsFailExplicitly
// ---------------------------------------------------------------------------

// An unknown enemy id or policy id fails explicitly: the target is named in
// the missing list, nothing spawns for it and no stand-in actor is created.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_018AUnknownIdsFailExplicitly,
	"UEMMO.Tasks.M5_018A.UnknownIdsFailExplicitly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_018AUnknownIdsFailExplicitly::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	if (!TestTrue(TEXT("a world is available"), World != nullptr))
	{
		return true;
	}

	const TCHAR* UnknownIds = TEXT(R"({
		"schema_version": 1, "table": "test_room",
		"rows": [{"room_id": "room_missing", "targets": [
			{"target_id": "good_one", "enemy_id": "melee_grunt", "max_health": 10,
			 "policy_id": "normal", "location_cm": {"x": 100, "y": 0, "z": 88}},
			{"target_id": "ghost_target", "enemy_id": "no_such_enemy", "max_health": 10,
			 "policy_id": "normal", "location_cm": {"x": 200, "y": 0, "z": 88}},
			{"target_id": "policy_ghost", "enemy_id": "melee_grunt", "max_health": 10,
			 "policy_id": "no_such_policy", "location_cm": {"x": 300, "y": 0, "z": 88}}]
		}]
	})");
	ACombatTestRoomDriver* Driver = NewObject<ACombatTestRoomDriver>();
	FTestRoomConfig Config;
	FString Error;
	if (!TestTrue("the unknown-id fixture parses (shape is legal)",
		ACombatTestRoomDriver::ParseTestRoomConfig(UnknownIds, Config, Error)))
	{
		AddError(Error);
		return true;
	}
	// The config must be visible to the spawn loop through the driver's own
	// surface (the same path LoadConfig uses).
	Driver->SetConfigForTesting(Config);

	// The refusal semantics: an unknown ENEMY id refuses the spawn entirely;
	// an unknown POLICY id spawns the target with the documented default
	// policy and a loud log - both failures are explicit and named.
	TArray<FName> MissingIds;
	const int32 Spawned = Driver->SpawnTargets(*World, MissingIds);
	TestEqual("two targets spawned (the enemy-refused one did not)", Spawned, 2);
	TestEqual("both id failures are reported", MissingIds.Num(), 2);
	TestTrue("the unknown enemy id is named", MissingIds.Contains(FName(TEXT("ghost_target"))));
	TestTrue("the unknown policy id is named", MissingIds.Contains(FName(TEXT("policy_ghost"))));

	// Exactly two ALIVE enemies exist (the shared world may still carry
	// corpses from an earlier case): the unknown-enemy target fabricated
	// nothing.
	int32 EnemyCount = 0;
	for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
	{
		const UHealthComponent* Health = It->GetHealthComponent();
		if (Health != nullptr && Health->IsAlive())
		{
			++EnemyCount;
		}
	}
	TestEqual("the unknown-enemy target spawned no stand-in", EnemyCount, 2);

	// Cleanup: destroy the enemies this test spawned.
	for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
	{
		It->Destroy();
	}
	Driver->Destroy();
	return true;
}

// ---------------------------------------------------------------------------
// MenuExposesTheSystemTestRoom
// ---------------------------------------------------------------------------

// The menu's second legal entry: the flow exposes the system test room
// definition with the room identity and the L_SystemTestRoom map path, so a
// menu click travels without a console open command.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_018AMenuExposesTheSystemTestRoom,
	"UEMMO.Tasks.M5_018A.MenuExposesTheSystemTestRoom",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_018AMenuExposesTheSystemTestRoom::RunTest(const FString& Parameters)
{
	// The flow subsystem lives in the test world's real game instance (the
	// NewGameLoopTests pattern; a bare NewObject violates the subsystem's
	// ClassWithin outer contract).
	FTestWorldWrapper Wrapper;
	if (!TestTrue(TEXT("the test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
	{
		return true;
	}
	UWorld* World = Wrapper.GetTestWorld();
	UGameFlowSubsystem* Flow = World != nullptr && World->GetGameInstance() != nullptr
		? World->GetGameInstance()->GetSubsystem<UGameFlowSubsystem>()
		: nullptr;
	if (!TestNotNull(TEXT("the game flow subsystem exists in the test game instance"), Flow))
	{
		return true;
	}
	URoomDefinition* Definition = Flow->GetSystemTestRoomDefinition();
	if (!TestNotNull("the flow exposes the system test room definition", Definition))
	{
		return true;
	}
	TestEqual("the room identity matches the source table",
		Definition->RoomId, FName(TEXT("room_system_test_01")));
	TestTrue("the map path points at the saved system test room",
		Definition->MapPath.ToSoftObjectPath().ToString().Contains(TEXT("L_SystemTestRoom")));
	TestEqual("the reward pool rides the starter table",
		Definition->RewardTableId, FName(TEXT("starter")));
	return true;
}

// ---------------------------------------------------------------------------
// M5-018C: DriverBeginPlaySpawnsTargets
// ---------------------------------------------------------------------------

// The RUNTIME wiring: a driver ACTOR whose BeginPlay runs loads the real
// Data/test_room.json and spawns the configured targets by itself. The
// M5-018A suites drove SpawnTargets directly on a bare NewObject, which left
// the placed map driver inert - the real room stayed empty (the M5-H01
// playtest found only the copied ResourcePreview_Quinn static prop, which no
// combat query can target). This test goes through the actor lifecycle so the
// wiring cannot regress again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_018CDriverBeginPlaySpawnsTargets,
	"UEMMO.Tasks.M5_018C.DriverBeginPlaySpawnsTargets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_018CDriverBeginPlaySpawnsTargets::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Wrapper;
	if (!TestTrue(TEXT("the test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
	{
		return true;
	}
	UWorld* World = Wrapper.GetTestWorld();
	if (!TestNotNull(TEXT("the test world exists"), World))
	{
		return true;
	}

	// A real driver actor at a remote base; the config targets spawn at their
	// room-local locations verbatim, so the base never collides with them.
	ACombatTestRoomDriver* Driver = World->SpawnActor<ACombatTestRoomDriver>(
		ACombatTestRoomDriver::StaticClass(),
		FVector(100000.0, 100000.0, 100.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("the driver actor spawned"), Driver))
	{
		return true;
	}

	// The lifecycle edge the real game runs (world BringAllActorsToLife) -
	// dispatched explicitly so the assertion is deterministic in both the
	// editor and the packaged automation contexts.
	Driver->DispatchBeginPlay();

	// The real Data/test_room.json carries exactly three targets across the
	// M5-003 sample policies; BeginPlay must have spawned all of them.
	TArray<AMeleeEnemy*> Enemies;
	for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
	{
		Enemies.Add(*It);
	}
	if (!TestEqual("BeginPlay spawned the three configured targets", Enemies.Num(), 3))
	{
		return true;
	}

	bool bNormalAtConfiguredLocation = false;
	bool bHeavyAtConfiguredLocation = false;
	bool bBossAtConfiguredLocation = false;
	for (AMeleeEnemy* Enemy : Enemies)
	{
		const UCombatComponent* Combat = Enemy->GetCombatComponent();
		const FVector Location = Enemy->GetActorLocation();
		if (Combat == nullptr)
		{
			continue;
		}
		const FName PolicyId = Combat->GetTargetReactionPolicy().PolicyId;
		const bool bAtX300 = FMath::Abs(Location.X - 300.0f) < 1.0f;
		const bool bAtX550 = FMath::Abs(Location.X - 550.0f) < 1.0f;
		if (PolicyId == FName(TEXT("normal")) && bAtX300 && FMath::Abs(Location.Y) < 1.0f)
		{
			bNormalAtConfiguredLocation = true;
		}
		if (PolicyId == FName(TEXT("heavy")) && bAtX550 && FMath::Abs(Location.Y - 150.0f) < 1.0f)
		{
			bHeavyAtConfiguredLocation = true;
		}
		if (PolicyId == FName(TEXT("boss")) && bAtX550 && FMath::Abs(Location.Y + 150.0f) < 1.0f)
		{
			bBossAtConfiguredLocation = true;
		}
	}
	TestTrue("the normal target stands at the configured location with its policy",
		bNormalAtConfiguredLocation);
	TestTrue("the heavy target stands at the configured location with its policy",
		bHeavyAtConfiguredLocation);
	TestTrue("the boss target stands at the configured location with its policy",
		bBossAtConfiguredLocation);
	return true;
}

#endif
