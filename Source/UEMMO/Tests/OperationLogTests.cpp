// M3-023: operation log system (user-requested 2026-09-30). The suite locks
// the four card requirements:
// 1. DAILY ROLLOVER - a write on a new date DELETES the log file and reopens
//    it with a fresh header (verified with an injected fake now provider, so
//    the test never waits for a real midnight).
// 2. Same-day writes APPEND in order and never clear the file.
// 3. RegisterPlayer auto-subscribes: manual CombatComponent broadcasts
//    (Started/Finished/HitConfirmed) and room-session broadcasts
//    (OnRunStarted/OnRunEnded) produce their log rows.
// 4. THE IN-LOOP SEQUENCE: a real APrototypeCharacter in a fully ticked temp
//    world (the engine FTestWorldWrapper precedent, M1-041 style) presses X
//    through the production SubmitCombatInput entry; the log then contains,
//    in timestamp order, the input row, the AttackState row, the Combat
//    Started row, the HitConfirmed row WITH THE DAMAGE VALUE and the Combat
//    Finished row - this proves the "program response" category is recorded.
// Harness notes: the subsystem under test is created through a REAL
// UGameInstance (the ProfileTests M3-003 pattern for the session tests, the
// GameCombatWiringTests M1-041 pattern for the in-loop test). The log file
// itself is the production file (ProjectSavedDir/OperationLogs/), read back
// with FFileHelper::LoadFileToString; tests never hardcode the path.
//
// Stub-failure note: against the red stub (Log writes nothing,
// RegisterPlayer binds nothing) the pure "no crash" prelude of
// LogSafeWithoutPlayerOrWorld passes, and every file-content, rollover and
// subscription assertion fails - assertion red, not build red.

#include "Misc/AutomationTest.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/CombatHitTypes.h"
#include "../Enemy/TrainingEnemy.h"
#include "../Logging/OperationLogSubsystem.h"
#include "../PrototypeCharacter.h"
#include "../Room/RoomResult.h"
#include "../Room/RoomSessionSubsystem.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/DataAsset.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_023
{
	// World names are uniquified: several suites live in one process and two
	// of this suite's own sessions can be alive in one test.
	static int32 M3_023_SessionCounter = 0;

	// Reads the production log file through the subsystem's own path.
	static bool M3_023_ReadLog(const UOperationLogSubsystem* OpLog, FString& OutContent)
	{
		return OpLog != nullptr
			&& !OpLog->GetLogFilePath().IsEmpty()
			&& FFileHelper::LoadFileToString(OutContent, *OpLog->GetLogFilePath());
	}

	// Counts non-overlapping occurrences of Needle in Haystack.
	static int32 M3_023_CountOccurrences(const FString& Haystack, const TCHAR* Needle)
	{
		int32 Count = 0;
		int32 Position = 0;
		while ((Position = Haystack.Find(Needle, ESearchCase::CaseSensitive, ESearchDir::FromStart, Position)) != INDEX_NONE)
		{
			++Count;
			++Position;
		}
		return Count;
	}

	/**
	 * One real UGameInstance + its world (the ProfileTests M3-003 harness):
	 * Create() initializes the instance, so the production subsystem collection
	 * instantiates UOperationLogSubsystem. TearDown() follows the engine
	 * FTestWorldWrapper::DestroyTestWorld order.
	 */
	struct FM3_023_Session
	{
		UWorld* World = nullptr;
		FWorldContext* Context = nullptr;
		UGameInstance* GameInstance = nullptr;
		UOperationLogSubsystem* OpLog = nullptr;

		static FM3_023_Session Create(FAutomationTestBase& Test)
		{
			FM3_023_Session Session;
			++M3_023_SessionCounter;
			const FName WorldName = FName(*FString::Printf(TEXT("M3_023_TestWorld_%d"), M3_023_SessionCounter));
			Session.World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, WorldName);
			if (!Test.TestNotNull(TEXT("a private test world is available"), Session.World))
			{
				return Session;
			}
			Session.GameInstance = NewObject<UGameInstance>(GEngine);
			Session.Context = &GEngine->CreateNewWorldContext(EWorldType::Game);
			Session.Context->OwningGameInstance = Session.GameInstance;
			Session.World->SetGameInstance(Session.GameInstance);
			Session.Context->SetCurrentWorld(Session.World);
			// The production path that creates every registered
			// UGameInstanceSubsystem (engine FTestWorldWrapper precedent).
			Session.GameInstance->Init();
			Session.OpLog = Session.GameInstance->GetSubsystem<UOperationLogSubsystem>();
			Test.TestNotNull(TEXT("the operation log subsystem exists in the game instance's collection"), Session.OpLog);
			return Session;
		}

		void TearDown()
		{
			if (World)
			{
				World->RemoveFromRoot();
				if (GameInstance)
				{
					GameInstance->Shutdown();
					GameInstance = nullptr;
				}
				if (Context)
				{
					GEngine->DestroyWorldContext(World);
					Context = nullptr;
				}
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				World = nullptr;
			}
		}
	};

	// -- In-loop scene helpers (the GameCombatWiringTests M1-041 pattern, ------
	//    renamed constants so the per-file namespaces stay independent) --------

	// Remote scene base (X horizontal, Y depth, Z height); the floor top sits
	// exactly at the base Z. Different coordinates from every other suite.
	const FVector M3_023_SceneBase(52000.0, 55000.0, 600.0);

	// All ground attacks share hit box offset X=95, so an enemy feet anchor at
	// +95 stays reachable (the M1-041 placement convention).
	const float M3_023_EnemyFeetOffsetX = 95.0f;

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M3_023_FloorHalfThickness = 100.0f;
	const float M3_023_FloorHalfExtentXY = 4000.0f;
	const float M3_023_PlayerSpawnHeight = 120.0f;
	const float M3_023_EnemySpawnHeight = 90.0f;

	constexpr float M3_023_FrameSeconds = 1.0f / 60.0f;
	constexpr int32 M3_023_MaxSettleFrames = 300;
	constexpr int32 M3_023_MaxRunFrames = 900;

	// World-static blocking floor box (temp worlds ship no geometry).
	static AActor* M3_023_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base - FVector(0.0, 0.0, M3_023_FloorHalfThickness), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M3_023_Floor"));
		Actor->SetRootComponent(Floor);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->SetBoxExtent(FVector(M3_023_FloorHalfExtentXY, M3_023_FloorHalfExtentXY, M3_023_FloorHalfThickness));
		Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Floor->RegisterComponent();
		Floor->SetWorldLocation(Base - FVector(0.0, 0.0, M3_023_FloorHalfThickness));
		return Actor;
	}

	// Spawns the real prototype pawn above the floor and enables no-controller
	// physics (the M1-041 spawn pattern).
	static APrototypeCharacter* M3_023_SpawnPlayer(UWorld& World)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			M3_023_SceneBase + FVector(0.0, 0.0, M3_023_PlayerSpawnHeight), FRotator::ZeroRotator, Params);
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
		return Player;
	}

	static ATrainingEnemy* M3_023_SpawnEnemy(UWorld& World, float FeetOffsetX)
	{
		FActorSpawnParameters Params;
		ATrainingEnemy* Enemy = World.SpawnActor<ATrainingEnemy>(
			ATrainingEnemy::StaticClass(),
			M3_023_SceneBase + FVector(FeetOffsetX, 0.0, M3_023_EnemySpawnHeight), FRotator::ZeroRotator, Params);
		return Enemy;
	}

	// One full in-loop scene: a manually ticked temp world with floor, player
	// and enemy, exactly like the M1-041 harness.
	struct FM3_023_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		APrototypeCharacter* Player = nullptr;
		UCombatComponent* PlayerCombat = nullptr;
		ATrainingEnemy* Enemy = nullptr;

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
			if (!Test.TestTrue(TEXT("play begins in the test world (full actor initialization)"),
				Wrapper.BeginPlayInTestWorld()))
			{
				return false;
			}
			AActor* Floor = M3_023_SpawnFloor(*World, M3_023_SceneBase);
			if (!Test.TestNotNull(TEXT("the floor spawns"), Floor))
			{
				return false;
			}
			// A hand-built world never runs the map-load collision tree pass.
			World->EnsureCollisionTreeIsBuilt();
			Player = M3_023_SpawnPlayer(*World);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			PlayerCombat = Player->GetCombat();
			if (!Test.TestTrue(TEXT("the player carries a combat component"), PlayerCombat != nullptr))
			{
				return false;
			}
			Enemy = M3_023_SpawnEnemy(*World, M3_023_EnemyFeetOffsetX);
			if (!Test.TestNotNull(TEXT("the training enemy spawns"), Enemy))
			{
				return false;
			}
			return true;
		}

		// Ticks the world until both characters settled onto the floor.
		bool Settle(FAutomationTestBase& Test)
		{
			UCharacterMovementComponent* PlayerMovement = Player->GetCharacterMovement();
			UCharacterMovementComponent* EnemyMovement = Enemy->GetCharacterMovement();
			for (int32 Frame = 0; Frame < M3_023_MaxSettleFrames; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks during settle"),
					Wrapper.TickTestWorld(M3_023_FrameSeconds)))
				{
					return false;
				}
				const bool bPlayerGrounded = PlayerMovement != nullptr
					&& PlayerMovement->MovementMode == MOVE_Walking
					&& PlayerMovement->Velocity.Size() < 1.0f;
				const bool bEnemyGrounded = EnemyMovement != nullptr
					&& EnemyMovement->MovementMode == MOVE_Walking
					&& EnemyMovement->Velocity.Size() < 1.0f;
				if (bPlayerGrounded && bEnemyGrounded)
				{
					return true;
				}
			}
			Test.AddError(TEXT("the characters never settled onto the floor within the settle cap"));
			return false;
		}
	};
}

using namespace UE::UEMMO::Tasks::M3_023;

// -- 1. Daily rollover: a write on a new date clears the file and reopens it --

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_023DailyRolloverClearsFileOnDateChange,
	"UEMMO.Tasks.M3_023.DailyRolloverClearsFileOnDateChange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_023DailyRolloverClearsFileOnDateChange::RunTest(const FString& Parameters)
{
	FM3_023_Session Session = FM3_023_Session::Create(*this);
	if (Session.OpLog == nullptr)
	{
		Session.TearDown();
		return true;
	}

	// Day 1: two writes through two categories. The first write opens the day
	// (the file is created with the fresh header), the second appends.
	const FDateTime Day1(2026, 3, 1, 10, 0, 0, 0);
	Session.OpLog->SetNowProvider([Day1]() { return Day1; });
	Session.OpLog->LogInput(TEXT("first-day input row"));
	Session.OpLog->LogState(TEXT("first-day state row"));

	FString Content;
	if (!TestTrue(TEXT("the log file exists after the day-1 writes"), M3_023_ReadLog(Session.OpLog, Content)))
	{
		Session.TearDown();
		return true;
	}
	TestTrue(TEXT("the day-1 header names the injected date"),
		Content.Contains(TEXT("=== Operation log started 2026-03-01 (")));
	TestTrue(TEXT("the day-1 input row reached the file"),
		Content.Contains(TEXT("first-day input row")));
	TestTrue(TEXT("the day-1 state row reached the file"),
		Content.Contains(TEXT("first-day state row")));
	TestTrue(TEXT("the active log date equals the injected day-1 date"),
		Session.OpLog->GetActiveLogDate() == Day1.GetDate());

	// Day 2: one more write. The date change MUST clear the file: the day-1
	// header and both day-1 rows are gone, only the day-2 header and the new
	// row remain.
	const FDateTime Day2(2026, 3, 2, 23, 59, 59, 500);
	Session.OpLog->SetNowProvider([Day2]() { return Day2; });
	Session.OpLog->LogResponse(TEXT("second-day response row"));

	TestTrue(TEXT("the log file is readable after the day-2 write"), M3_023_ReadLog(Session.OpLog, Content));
	TestTrue(TEXT("the day-2 header names the injected date"),
		Content.Contains(TEXT("=== Operation log started 2026-03-02 (")));
	TestTrue(TEXT("the day-2 response row reached the file"),
		Content.Contains(TEXT("second-day response row")));
	TestEqual(TEXT("exactly one header remains after the rollover (the day-1 content was cleared)"),
		M3_023_CountOccurrences(Content, TEXT("=== Operation log started")), 1);
	TestTrue(TEXT("the day-1 content is gone after the rollover"),
		!Content.Contains(TEXT("first-day input row"))
		&& !Content.Contains(TEXT("first-day state row"))
		&& !Content.Contains(TEXT("=== Operation log started 2026-03-01 (")));

	Session.TearDown();
	return true;
}

// -- 2. Same-day writes append in order and never clear -----------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_023SameDayWritesAppendInOrder,
	"UEMMO.Tasks.M3_023.SameDayWritesAppendInOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_023SameDayWritesAppendInOrder::RunTest(const FString& Parameters)
{
	FM3_023_Session Session = FM3_023_Session::Create(*this);
	if (Session.OpLog == nullptr)
	{
		Session.TearDown();
		return true;
	}

	const FDateTime FixedDay(2026, 5, 5, 8, 0, 0, 0);
	Session.OpLog->SetNowProvider([FixedDay]() { return FixedDay; });
	Session.OpLog->LogInput(TEXT("append alpha"));
	Session.OpLog->LogState(TEXT("append beta"));

	FString Content;
	if (!TestTrue(TEXT("the log file exists after the first two same-day writes"),
		M3_023_ReadLog(Session.OpLog, Content)))
	{
		Session.TearDown();
		return true;
	}
	const int32 AlphaIndex = Content.Find(TEXT("append alpha"), ESearchCase::CaseSensitive, ESearchDir::FromStart);
	const int32 BetaIndex = Content.Find(TEXT("append beta"), ESearchCase::CaseSensitive, ESearchDir::FromStart);
	TestTrue(TEXT("the first same-day row reached the file"), AlphaIndex != INDEX_NONE);
	TestTrue(TEXT("the second same-day row reached the file"), BetaIndex != INDEX_NONE);
	TestTrue(TEXT("the two same-day rows keep their write order"),
		AlphaIndex != INDEX_NONE && BetaIndex != INDEX_NONE && AlphaIndex < BetaIndex);

	// A later write on the SAME day appends without clearing anything.
	Session.OpLog->LogResponse(TEXT("append gamma"));
	Session.OpLog->LogInput(TEXT("append delta"));
	TestTrue(TEXT("the file is still readable after the later same-day writes"),
		M3_023_ReadLog(Session.OpLog, Content));
	TestTrue(TEXT("the earlier same-day rows survived the later same-day writes (append, never clear)"),
		Content.Contains(TEXT("append alpha")) && Content.Contains(TEXT("append beta")));
	const int32 GammaIndex = Content.Find(TEXT("append gamma"), ESearchCase::CaseSensitive, ESearchDir::FromStart);
	const int32 DeltaIndex = Content.Find(TEXT("append delta"), ESearchCase::CaseSensitive, ESearchDir::FromStart);
	TestTrue(TEXT("the later same-day rows keep their write order behind the earlier ones"),
		GammaIndex != INDEX_NONE && DeltaIndex != INDEX_NONE
		&& BetaIndex != INDEX_NONE && BetaIndex < GammaIndex && GammaIndex < DeltaIndex);

	Session.TearDown();
	return true;
}

// -- 3. RegisterPlayer auto-subscribes combat and room events ------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_023RegisterPlayerBindsCombatAndRoomEvents,
	"UEMMO.Tasks.M3_023.RegisterPlayerBindsCombatAndRoomEvents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_023RegisterPlayerBindsCombatAndRoomEvents::RunTest(const FString& Parameters)
{
	// A real player in a real (not manually ticked) test world: the manual
	// delegate broadcasts below are the card's "simulate CombatComponent
	// delegate broadcasts" shape - no catalog, no ticking, pure subscription
	// verification on top of the production BeginPlay wiring.
	FM3_023_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	UOperationLogSubsystem* OpLog = UOperationLogSubsystem::FindForContext(Scene.Player);
	if (!TestNotNull(TEXT("the operation log subsystem resolves through the player"), OpLog))
	{
		Scene.Wrapper.DestroyTestWorld(false);
		return true;
	}
	// The explicit re-register on top of BeginPlay's automatic one is the
	// documented idempotent rebind (UnbindAll + one fresh binding set).
	TestTrue(TEXT("RegisterPlayer accepts the real player"), OpLog->RegisterPlayer(Scene.Player));
	TestTrue(TEXT("a registered player is reported registered"), OpLog->IsPlayerRegistered());

	UCombatComponent* Combat = Scene.Player->GetCombat();
	Combat->OnStarted.Broadcast(FName(TEXT("light_01")), 7);
	Combat->OnFinished.Broadcast(FName(TEXT("light_01")), 7);
	FCombatHit Hit;
	Hit.AttackId = FName(TEXT("light_01"));
	Hit.AttackInstanceId = 7;
	Hit.Damage = 12.0f;
	Hit.Target = Scene.Player;
	Combat->OnHitConfirmed.Broadcast(Hit);

	// The room events fire from the world's session through the same binding
	// set (no run was started, so the started handler reports the fresh ids).
	URoomSessionSubsystem* RoomSession = Scene.World->GetSubsystem<URoomSessionSubsystem>();
	if (!TestNotNull(TEXT("the room session subsystem exists in the test world"), RoomSession))
	{
		Scene.Wrapper.DestroyTestWorld(false);
		return true;
	}
	RoomSession->OnRunStarted().Broadcast();
	FRoomResult Result;
	Result.RunId = 3;
	Result.SettlementId = 4;
	Result.RoomId = FName(TEXT("TrainingRoom"));
	Result.bCleared = true;
	Result.KilledCount = 2;
	Result.ElapsedSeconds = 11.5;
	RoomSession->OnRunEnded().Broadcast(Result);

	FString Content;
	if (!TestTrue(TEXT("the log file is readable after the manual broadcasts"), M3_023_ReadLog(OpLog, Content)))
	{
		Scene.Wrapper.DestroyTestWorld(false);
		return true;
	}
	TestTrue(TEXT("the combat Started row was logged"),
		Content.Contains(TEXT("Combat Started: light_01 #7")));
	TestTrue(TEXT("the combat Finished row was logged"),
		Content.Contains(TEXT("Combat Finished: light_01 #7")));
	TestTrue(TEXT("the combat HitConfirmed row was logged with the damage value"),
		Content.Contains(TEXT("Combat HitConfirmed: light_01 #7 damage=12")));
	TestTrue(TEXT("the HitConfirmed row names the target actor"),
		Content.Contains(TEXT("target=")) && Content.Contains(TEXT("PrototypeCharacter")));
	TestTrue(TEXT("the room run-started row was logged"),
		Content.Contains(TEXT("Room RunStarted: run=0")));
	TestTrue(TEXT("the room run-ended row was logged with the result values"),
		Content.Contains(TEXT("Room RunEnded: run=3 cleared=1 killed=2")));

	// Unregister removes the bindings: a second broadcast set stays unlogged.
	OpLog->UnregisterPlayer();
	TestTrue(TEXT("after UnregisterPlayer no player is reported registered"), !OpLog->IsPlayerRegistered());
	FString ContentBeforeUnregisterBroadcast;
	M3_023_ReadLog(OpLog, ContentBeforeUnregisterBroadcast);
	Combat->OnStarted.Broadcast(FName(TEXT("light_02")), 8);
	FString ContentAfter;
	M3_023_ReadLog(OpLog, ContentAfter);
	TestEqual(TEXT("no combat row is logged after UnregisterPlayer"),
		M3_023_CountOccurrences(ContentAfter, TEXT("Combat Started:")),
		M3_023_CountOccurrences(ContentBeforeUnregisterBroadcast, TEXT("Combat Started:")));

	Scene.Wrapper.DestroyTestWorld(false);
	return true;
}

// -- 4. Line format and the three fixed categories -----------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_023LogLineFormatAndCategories,
	"UEMMO.Tasks.M3_023.LogLineFormatAndCategories",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_023LogLineFormatAndCategories::RunTest(const FString& Parameters)
{
	FM3_023_Session Session = FM3_023_Session::Create(*this);
	if (Session.OpLog == nullptr)
	{
		Session.TearDown();
		return true;
	}

	// A pinned time makes the timestamp part deterministic: HH:MM:SS.mmm.
	const FDateTime PinnedTime(2026, 7, 7, 7, 8, 9, 123);
	Session.OpLog->SetNowProvider([PinnedTime]() { return PinnedTime; });
	Session.OpLog->LogInput(TEXT("format probe input"));
	Session.OpLog->LogState(TEXT("format probe state"));
	Session.OpLog->LogResponse(TEXT("format probe response"));
	Session.OpLog->Log(FName(TEXT("Custom")), TEXT("format probe custom"));

	FString Content;
	if (!TestTrue(TEXT("the log file is readable after the format probes"),
		M3_023_ReadLog(Session.OpLog, Content)))
	{
		Session.TearDown();
		return true;
	}
	// The full line shape "[HH:MM:SS.mmm][Category] Message" is asserted on the
	// Input row (zero-padded fields, millisecond precision, fixed category).
	TestTrue(TEXT("the input row carries the full [HH:MM:SS.mmm][Input] prefix"),
		Content.Contains(TEXT("[07:08:09.123][Input] format probe input")));
	TestTrue(TEXT("the state row carries the [State] category"),
		Content.Contains(TEXT("[07:08:09.123][State] format probe state")));
	TestTrue(TEXT("the response row carries the [Response] category"),
		Content.Contains(TEXT("[07:08:09.123][Response] format probe response")));
	TestTrue(TEXT("Log(FName) passes the caller's category through"),
		Content.Contains(TEXT("[07:08:09.123][Custom] format probe custom")));

	Session.TearDown();
	return true;
}

// -- 5. Safe without player / world / combat ----------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_023LogSafeWithoutPlayerOrWorld,
	"UEMMO.Tasks.M3_023.LogSafeWithoutPlayerOrWorld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_023LogSafeWithoutPlayerOrWorld::RunTest(const FString& Parameters)
{
	FM3_023_Session Session = FM3_023_Session::Create(*this);
	if (Session.OpLog == nullptr)
	{
		Session.TearDown();
		return true;
	}

	// No player registered, no combat involved: every entry point must run.
	Session.OpLog->LogInput(TEXT("orphan input row"));
	Session.OpLog->LogState(TEXT("orphan state row"));
	Session.OpLog->LogResponse(TEXT("orphan response row"));
	Session.OpLog->Log(FName(TEXT("Any")), TEXT("orphan any row"));
	TestTrue(TEXT("a null RegisterPlayer is refused"),
		Session.OpLog->RegisterPlayer(nullptr) == false);
	TestTrue(TEXT("no player is reported registered after the null request"),
		Session.OpLog->IsPlayerRegistered() == false);
	// UnregisterPlayer without any registration must be a plain no-op.
	Session.OpLog->UnregisterPlayer();

	FString Content;
	TestTrue(TEXT("the log file is readable after the orphan rows"),
		M3_023_ReadLog(Session.OpLog, Content));
	TestTrue(TEXT("the orphan rows reached the file (logging needs no registration)"),
		Content.Contains(TEXT("orphan input row")) && Content.Contains(TEXT("orphan response row")));

	// Context resolution is null-safe: no context, and a context without any
	// reachable game instance both answer null instead of crashing (the bare
	// object is a concrete AActor - UObject itself is abstract).
	TestTrue(TEXT("FindForContext(nullptr) answers null"),
		UOperationLogSubsystem::FindForContext(nullptr) == nullptr);
	UObject* BareObject = NewObject<AActor>(GetTransientPackage());
	TestTrue(TEXT("FindForContext of a context without a world/game instance answers null"),
		UOperationLogSubsystem::FindForContext(BareObject) == nullptr);

	Session.TearDown();
	return true;
}

// -- 6. In-loop sequence: input row, attack rows, hit row with damage ----------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_023InLoopPressAttackHitWritesChronologicalLog,
	"UEMMO.Tasks.M3_023.InLoopPressAttackHitWritesChronologicalLog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_023InLoopPressAttackHitWritesChronologicalLog::RunTest(const FString& Parameters)
{
	FM3_023_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		Scene.Wrapper.DestroyTestWorld(false);
		return true;
	}

	UOperationLogSubsystem* OpLog = UOperationLogSubsystem::FindForContext(Scene.Player);
	if (!TestNotNull(TEXT("the operation log subsystem resolves in the in-loop world"), OpLog))
	{
		Scene.Wrapper.DestroyTestWorld(false);
		return true;
	}

	// Warm-up marker: one direct write opens the log day and gives the tail
	// reader a stable anchor, so earlier suites' rows never pollute the
	// ordering assertions (the marker write also proves the file exists).
	OpLog->Log(FName(TEXT("M3_023")), TEXT("in-loop sequence marker"));
	FString MarkerProbe;
	if (!TestTrue(TEXT("the log file exists after the marker write"), M3_023_ReadLog(OpLog, MarkerProbe)))
	{
		Scene.Wrapper.DestroyTestWorld(false);
		return true;
	}
	TestTrue(TEXT("the marker write reached the file"),
		MarkerProbe.Contains(TEXT("in-loop sequence marker")));

	// The production intent entry drives the press; the character's own Tick
	// injects the input clock and the facing (the M1-041 wiring pivot - this
	// test calls no component API directly).
	Scene.Player->SubmitCombatInput(ECombatInput::Light);

	// Event observers as the run gate (the log assertions below are
	// independent of these - they only prove the sequence actually ran).
	bool bStarted = false;
	bool bFinished = false;
	bool bHit = false;
	Scene.PlayerCombat->OnStarted.AddLambda([&bStarted](FName, uint64) { bStarted = true; });
	Scene.PlayerCombat->OnFinished.AddLambda([&bFinished](FName, uint64) { bFinished = true; });
	Scene.PlayerCombat->OnHitConfirmed.AddLambda([&bHit](const FCombatHit&) { bHit = true; });

	for (int32 Frame = 0; Frame < M3_023_MaxRunFrames && !(bStarted && bHit && bFinished); ++Frame)
	{
		if (!TestTrue(TEXT("the test world ticks during the attack run"),
			Scene.Wrapper.TickTestWorld(M3_023_FrameSeconds)))
		{
			break;
		}
	}
	if (!TestTrue(TEXT("the single Light press started, hit and finished one attack in-world"),
		bStarted && bHit && bFinished))
	{
		Scene.Wrapper.DestroyTestWorld(false);
		return true;
	}

	FString Content;
	if (!TestTrue(TEXT("the log file is readable after the in-loop run"), M3_023_ReadLog(OpLog, Content)))
	{
		Scene.Wrapper.DestroyTestWorld(false);
		return true;
	}
	// Everything from the marker on is this test's own traffic.
	const int32 MarkerIndex = Content.Find(TEXT("in-loop sequence marker"),
		ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	TestTrue(TEXT("the marker line is still present in the log"), MarkerIndex != INDEX_NONE);
	const FString Tail = MarkerIndex != INDEX_NONE ? Content.Mid(MarkerIndex) : Content;

	// The four card categories in one sequence, in timestamp (append) order:
	// input -> action state -> attack started -> hit confirmed (damage!) ->
	// attack finished. The HitConfirmed row IS the program-response record.
	const int32 InputIndex = Tail.Find(TEXT("[Input] Pressed X (Light)"));
	const int32 AttackingIndex = Tail.Find(TEXT("AttackState: Attacking (light_01"));
	const int32 StartedIndex = Tail.Find(TEXT("Combat Started: light_01 #1"));
	const int32 HitIndex = Tail.Find(TEXT("[Response] Combat HitConfirmed: light_01 #1 damage=10 target=TrainingEnemy"));
	const int32 FinishedIndex = Tail.Find(TEXT("Combat Finished: light_01 #1"));

	TestTrue(TEXT("the input row was logged for the submitted Light press"), InputIndex != INDEX_NONE);
	TestTrue(TEXT("the AttackState row was logged for the started attack"), AttackingIndex != INDEX_NONE);
	TestTrue(TEXT("the Combat Started row was logged for the fresh instance #1"), StartedIndex != INDEX_NONE);
	TestTrue(TEXT("the HitConfirmed row was logged with the exact light_01 damage (10) and the enemy target"),
		HitIndex != INDEX_NONE);
	TestTrue(TEXT("the Combat Finished row was logged for the retired instance #1"), FinishedIndex != INDEX_NONE);
	TestTrue(TEXT("the logged rows keep the chronological order input < state < started < hit < finished"),
		InputIndex != INDEX_NONE && AttackingIndex != INDEX_NONE && StartedIndex != INDEX_NONE
		&& HitIndex != INDEX_NONE && FinishedIndex != INDEX_NONE
		&& InputIndex < AttackingIndex && AttackingIndex < StartedIndex
		&& StartedIndex < HitIndex && HitIndex < FinishedIndex);

	Scene.Wrapper.DestroyTestWorld(false);
	return true;
}

#endif
