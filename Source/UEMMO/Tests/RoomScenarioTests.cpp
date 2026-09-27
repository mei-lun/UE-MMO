// M2-014: the two-wave room scenario regression. Full game-flow scenes on the
// M2-010/M2-011 scaffold precedent: one real BeginPlay-initialized temp game
// world per test with the production URoomSessionSubsystem, a real floor, real
// ticks, the shipped-health prototype pawn, and the injected session clock
// driven alongside every tick exactly the way a game frame driver performs.
// The room definition is a hand-built double of the room_training_01 shape
// (2+3 waves of melee_grunt), the M2-007/M2-008/M2-010/M2-011 precedent for
// runtime definitions (the JSON-to-asset loading belongs to the catalog task).
//
// Every kill flows through the legal lethal path only: the enemy's own health
// component ApplyDamage (the public API), whose OnDied reaches the spawner's
// death binding and from there the session kill bookkeeping - no test ever
// writes a Cleared state or bypasses the session. The five scenes:
//
//   1. Full clear: wave 0 (2) -> kill both -> 1.0 s gap -> wave 1 (3) -> kill
//      all -> Cleared, with the actual spawn/death/result event sequence
//      recorded (timestamps + event types) and order-asserted.
//   2. Negative: the last wave keeps one enemy alive - the run stays Running
//      with no settlement far past every gap; the last death then clears.
//   3. Failure + retry: the player pawn dies (health pool -> PlayerDied), the
//      run fails with no late spawns, RetryRoom starts a fresh run (new
//      RunId) that clears normally.
//   4. Mid-run exit: LeaveRoom while wave 0 is still generating - no late
//      spawns, no settlement (an exit is not a result); re-entering starts a
//      fresh run that clears.
//   5. Three consecutive runs: every run spawns exactly 5, kills exactly 5,
//      clears exactly once, adds exactly 5 enemy actors and leaves zero alive
//      enemies; the inter-wave wait is re-armed per run (no stale timer).
//
// Each test also writes its recorded event sequence as a JSON evidence file
// under Artifacts/Tasks/M2-014/scenario-json (the success, fail-retry and
// exit scenes are the three the card mandates).
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/HealthComponent.h"
#include "../Enemy/EnemyDefinition.h"
#include "../Enemy/MeleeEnemy.h"
#include "../PrototypeCharacter.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomRetryService.h"
#include "../Room/RoomSessionSubsystem.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M2_014
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M2_014_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with other
	// suites' arenas. X horizontal, Y depth, Z height; the floor top sits at
	// the base Z.
	const FVector M2_014_SceneBase(76000.0, 83000.0, 600.0);

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M2_014_FloorHalfThickness = 100.0f;
	const float M2_014_FloorHalfExtentXY = 4000.0f;

	// Spawn heights above the floor top.
	const float M2_014_PlayerSpawnHeight = 120.0f;
	const float M2_014_EnemySpawnHeight = 90.0f;

	// The wave spawner's slot interval and the session's inter-wave wait (the
	// M2-007/M2-008 production constants the scenario timeline rides on).
	constexpr double M2_014_SpawnIntervalSeconds = 0.3;
	constexpr double M2_014_WaveGapSeconds = 1.0;

	// Tolerance around an exact injected-clock deadline: the wave start may
	// fire up to the pump epsilon early and at most one frame late.
	constexpr double M2_014_DeadlineEarlySeconds = 0.02;
	constexpr double M2_014_DeadlineLateSeconds = 0.15;

	// Loose birth-location band of the kill helper (cm). A fresh enemy that
	// spawns exactly where an old corpse lies is nudged a few centimetres by
	// the physics solver; the band still catches any gross mis-spawn while
	// tolerating that nudge. Exact birth positions are pinned by the M2-007
	// WaveSpawner suite; this suite verifies the flow around them.
	const double M2_014_BirthLocationBandCm = 300.0;

	// World-static blocking box (the floor shares the M2-010 builder shape).
	static AActor* M2_014_SpawnBoxActor(UWorld& World, const FVector& Center, const FVector& HalfExtent, const TCHAR* Name)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Center, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Box = NewObject<UBoxComponent>(Actor, Name);
		Actor->SetRootComponent(Box);
		Box->SetMobility(EComponentMobility::Movable);
		Box->SetBoxExtent(HalfExtent);
		Box->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Box->RegisterComponent();
		Box->SetWorldLocation(Center);
		return Actor;
	}

	// Spawns the real prototype pawn above the floor top (the M2-004 rig: the
	// pawn carries its own UHealthComponent from spawn; no runtime attachment).
	static APrototypeCharacter* M2_014_SpawnPlayer(UWorld& World)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			M2_014_SceneBase + FVector(0.0, 0.0, M2_014_PlayerSpawnHeight),
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
		return Player;
	}

	// Definition double of the Data/enemies.json "melee_grunt" row (the
	// M2-008/M2-010 suite precedent: the progression takes the definition as
	// a parameter; the JSON-to-asset loading belongs to the catalog task).
	static UEnemyDefinition* M2_014_MakeEnemyDef()
	{
		UEnemyDefinition* Def = NewObject<UEnemyDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Def->EnemyId = FName(TEXT("melee_grunt"));
		Def->MaxHP = 60.0f;
		Def->AttackPower = 0.0f;
		Def->MoveSpeed = 220.0f;
		Def->AttackRangeX = 160.0f;
		Def->AlignYTolerance = 35.0f;
		Def->TelegraphSeconds = 0.35f;
		Def->SpawnGraceSeconds = 0.5f;
		Def->MeleeAttackId = FName(TEXT("light_01"));
		return Def;
	}

	// Hand-built definition double of the room_training_01 shape (2+3 waves,
	// the M2-007/M2-008/M2-010/M2-011 precedent): wave 0 two enemies, wave 1
	// three enemies, all inside the floor rectangle of this suite's arena.
	static URoomDefinition* M2_014_MakeRoom()
	{
		URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Room->RoomId = FName(TEXT("room_m2_014_scenario"));
		Room->RewardTableId = FName(TEXT("reward_m2_014"));

		FRoomWaveDefinition Wave0;
		Wave0.EnemyId = FName(TEXT("melee_grunt"));
		Wave0.Count = 2;
		Wave0.SpawnLocations.Add(M2_014_SceneBase + FVector(300.0, 0.0, M2_014_EnemySpawnHeight));
		Wave0.SpawnLocations.Add(M2_014_SceneBase + FVector(450.0, -150.0, M2_014_EnemySpawnHeight));
		Room->Waves.Add(Wave0);

		FRoomWaveDefinition Wave1;
		Wave1.EnemyId = FName(TEXT("melee_grunt"));
		Wave1.Count = 3;
		Wave1.SpawnLocations.Add(M2_014_SceneBase + FVector(600.0, 100.0, M2_014_EnemySpawnHeight));
		Wave1.SpawnLocations.Add(M2_014_SceneBase + FVector(750.0, -100.0, M2_014_EnemySpawnHeight));
		Wave1.SpawnLocations.Add(M2_014_SceneBase + FVector(0.0, 300.0, M2_014_EnemySpawnHeight));
		Room->Waves.Add(Wave1);
		return Room;
	}

	// One recorded scenario event: the injected-clock timestamp plus a stable
	// type tag ("RunStarted", "WaveStarted", "EnemySpawned", "EnemyDied",
	// "PlayerDied", "RunEnded") and a free-text detail.
	struct FM2_014_Event
	{
		double TimeSeconds = 0.0;
		FString Type;
		FString Detail;
	};

	// The scenario event log; assertions read the type stream and timestamps,
	// the JSON writer serializes the whole sequence as report evidence.
	struct FM2_014_EventLog
	{
		TArray<FM2_014_Event> Events;

		void Add(double NowSeconds, const TCHAR* Type, const FString& Detail)
		{
			FM2_014_Event Event;
			Event.TimeSeconds = NowSeconds;
			Event.Type = Type;
			Event.Detail = Detail;
			Events.Add(Event);
		}

		/** The type stream in broadcast order (the order-assertion input). */
		TArray<FString> Types() const
		{
			TArray<FString> Out;
			for (const FM2_014_Event& Event : Events)
			{
				Out.Add(Event.Type);
			}
			return Out;
		}

		/** Every recorded timestamp of one event type, in broadcast order. */
		TArray<double> TimesOf(const TCHAR* Type) const
		{
			TArray<double> Out;
			for (const FM2_014_Event& Event : Events)
			{
				if (Event.Type == Type)
				{
					Out.Add(Event.TimeSeconds);
				}
			}
			return Out;
		}

		/** True when no timestamp ever moves backwards (the timeline guard). */
		bool TimesAreMonotonic() const
		{
			for (int32 Index = 1; Index < Events.Num(); ++Index)
			{
				if (Events[Index].TimeSeconds < Events[Index - 1].TimeSeconds - 1e-9)
				{
					return false;
				}
			}
			return true;
		}

		/** One human-readable line per event (the AddInfo diagnostics). */
		FString Describe() const
		{
			FString Out;
			for (const FM2_014_Event& Event : Events)
			{
				Out += FString::Printf(TEXT("t=%.3fs %s (%s)\n"), Event.TimeSeconds, *Event.Type, *Event.Detail);
			}
			return Out;
		}
	};

	// Minimal JSON string escaping for the evidence writer (the event types
	// and details are ASCII diagnostics; quotes and backslashes are the only
	// characters that need care).
	static FString M2_014_JsonEscape(const FString& In)
	{
		FString Out;
		for (const TCHAR Character : In)
		{
			if (Character == TEXT('\\'))
			{
				Out += TEXT("\\\\");
			}
			else if (Character == TEXT('"'))
			{
				Out += TEXT("\\\"");
			}
			else if (Character == TEXT('\n'))
			{
				Out += TEXT("\\n");
			}
			else
			{
				Out.AppendChar(Character);
			}
		}
		return Out;
	}

	/**
	 * Writes one scenario's event sequence as the card's JSON evidence file
	 * under Artifacts/Tasks/M2-014/scenario-json. ExtraFields carries
	 * pre-formatted JSON members (comma separated, no trailing comma) with
	 * the scenario's summary numbers. Returns false (and reports) when the
	 * file could not be written.
	 */
	static bool M2_014_WriteScenarioJson(FAutomationTestBase& Test, const TCHAR* FileName,
		const TCHAR* ScenarioName, const FM2_014_EventLog& Log, const FString& ExtraFields)
	{
		FString Json = TEXT("{\n");
		Json += FString::Printf(TEXT("  \"scenario\": \"%s\",\n"), *M2_014_JsonEscape(ScenarioName));
		Json += TEXT("  \"recorded_by\": \"UEMMO.Tasks.M2_014\",\n");
		Json += FString::Printf(TEXT("  \"event_count\": %d,\n"), Log.Events.Num());
		if (!ExtraFields.IsEmpty())
		{
			Json += TEXT("  ") + ExtraFields + TEXT(",\n");
		}
		Json += TEXT("  \"events\": [\n");
		for (int32 Index = 0; Index < Log.Events.Num(); ++Index)
		{
			const FM2_014_Event& Event = Log.Events[Index];
			Json += FString::Printf(TEXT("    { \"t\": %.6f, \"type\": \"%s\", \"detail\": \"%s\" }%s\n"),
				Event.TimeSeconds, *M2_014_JsonEscape(Event.Type), *M2_014_JsonEscape(Event.Detail),
				(Index + 1 < Log.Events.Num()) ? TEXT(",") : TEXT(""));
		}
		Json += TEXT("  ]\n}\n");

		const FString Directory = FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M2-014") / TEXT("scenario-json");
		IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
		const FString Path = Directory / FileName;
		if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Test.AddError(FString::Printf(TEXT("could not write the scenario evidence JSON to %s"), *Path));
			return false;
		}
		Test.AddInfo(FString::Printf(TEXT("scenario evidence JSON written to %s"), *Path));
		return true;
	}

	/**
	 * One scenario scene: the temp world, the real session subsystem, the
	 * prototype pawn and the event recording. The scene's event log records
	 * every session run start/end and player death through direct delegate
	 * bindings, and every wave start plus enemy spawn through a per-frame
	 * world sample; enemy deaths arrive through the death binding the sample
	 * attaches to each freshly spawned enemy (the real death pipeline).
	 */
	struct FM2_014_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		URoomSessionSubsystem* Session = nullptr;
		APrototypeCharacter* Player = nullptr;
		double ClockSeconds = 0.0;
		FM2_014_EventLog Log;

		// Delegate counters (the assertions read these only).
		int32 StartedCount = 0;
		int32 EndedCount = 0;
		TArray<FRoomResult> EndedResults;
		int32 PlayerDiedCount = 0;

		// Per-frame sampling state: the last observed wave count and every
		// enemy the sampler has already tracked (with the death binding).
		int32 LastStartedWaveCount = 0;
		TArray<TWeakObjectPtr<AMeleeEnemy>> TrackedEnemies;
		int32 EnemySerial = 0;

		// Enemies this scenario already killed (the corpse of an earlier
		// wave or run never shadows a fresh enemy of the same location).
		TArray<TWeakObjectPtr<AActor>> KilledEnemies;

		bool Build(FAutomationTestBase& Test)
		{
			if (!Test.TestTrue(TEXT("the test world is created (engine FTestWorldWrapper precedent)"),
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
			Session = World->GetSubsystem<URoomSessionSubsystem>();
			if (!Test.TestNotNull(TEXT("the room session subsystem exists in the test world"), Session))
			{
				return false;
			}
			Session->OnRunStarted().AddLambda([this]()
			{
				++StartedCount;
				Log.Add(ClockSeconds, TEXT("RunStarted"),
					FString::Printf(TEXT("run %llu"), Session->GetRunId()));
			});
			Session->OnRunEnded().AddLambda([this](const FRoomResult& Result)
			{
				++EndedCount;
				EndedResults.Add(Result);
				Log.Add(ClockSeconds, TEXT("RunEnded"),
					FString::Printf(TEXT("run %llu cleared=%d killed=%d settlement=%llu"),
						Result.RunId, Result.bCleared ? 1 : 0, Result.KilledCount, Result.SettlementId));
			});

			AActor* Floor = M2_014_SpawnBoxActor(*World,
				M2_014_SceneBase - FVector(0.0, 0.0, M2_014_FloorHalfThickness),
				FVector(M2_014_FloorHalfExtentXY, M2_014_FloorHalfExtentXY, M2_014_FloorHalfThickness),
				TEXT("M2_014_Floor"));
			if (Floor == nullptr)
			{
				Test.AddError(TEXT("the floor failed to spawn"));
				return false;
			}
			// A hand-built world never runs the map-load collision tree pass;
			// build it once before the first tick (the M1-041 pattern).
			World->EnsureCollisionTreeIsBuilt();

			Player = M2_014_SpawnPlayer(*World);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			Player->PlayerDied.AddLambda([this]()
			{
				++PlayerDiedCount;
				Log.Add(ClockSeconds, TEXT("PlayerDied"),
					FString::Printf(TEXT("player death #%d"), PlayerDiedCount));
			});
			// The registration under test (the death handler binds here).
			Session->SetPlayer(Player);
			return true;
		}

		/** One event-log line per fresh world observation, after the pump. */
		void Sample()
		{
			if (Session != nullptr)
			{
				const int32 Started = Session->GetStartedWaveCount();
				if (Started > LastStartedWaveCount)
				{
					Log.Add(ClockSeconds, TEXT("WaveStarted"),
						FString::Printf(TEXT("wave %d"), Session->GetCurrentWaveIndex()));
					LastStartedWaveCount = Started;
				}
			}
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				AMeleeEnemy* Enemy = *It;
				if (!IsValid(Enemy))
				{
					continue;
				}
				bool bTracked = false;
				for (const TWeakObjectPtr<AMeleeEnemy>& Known : TrackedEnemies)
				{
					if (Known.Get() == Enemy)
					{
						bTracked = true;
						break;
					}
				}
				if (bTracked)
				{
					continue;
				}
				TrackedEnemies.Add(Enemy);
				const int32 Serial = ++EnemySerial;
				const FString EnemyName = Enemy->GetName();
				Log.Add(ClockSeconds, TEXT("EnemySpawned"),
					FString::Printf(TEXT("enemy #%d (%s)"), Serial, *EnemyName));
				if (Enemy->GetHealthComponent() != nullptr)
				{
					Enemy->GetHealthComponent()->OnDied.AddLambda([this, Serial, EnemyName]()
					{
						Log.Add(ClockSeconds, TEXT("EnemyDied"),
							FString::Printf(TEXT("enemy #%d (%s)"), Serial, *EnemyName));
					});
				}
			}
		}

		/** Injects one exact clock value and samples the world right after. */
		void InjectClock(double NowSeconds)
		{
			ClockSeconds = NowSeconds;
			Session->SetSessionClockSeconds(NowSeconds);
			Sample();
		}

		/** Ticks the world once with the requested delta. */
		bool TickSeconds(FAutomationTestBase& Test, float DeltaSeconds)
		{
			return Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(DeltaSeconds));
		}

		/**
		 * Advances the world with real 60 fps ticks while injecting the
		 * session clock alongside (the same per-frame duty a game driver
		 * performs), so due births, the inter-wave wait and the settlement
		 * elapse through the production pump path.
		 */
		bool AdvanceSeconds(FAutomationTestBase& Test, double Seconds)
		{
			const double StepSeconds = static_cast<double>(M2_014_FrameSeconds);
			double Remaining = Seconds;
			while (Remaining > 1e-9)
			{
				const double StepNow = FMath::Min(StepSeconds, Remaining);
				InjectClock(ClockSeconds + StepNow);
				if (!TickSeconds(Test, M2_014_FrameSeconds))
				{
					return false;
				}
				Sample();
				Remaining -= StepNow;
			}
			return true;
		}

		/**
		 * Anchors/continues the CURRENT wave and advances past every
		 * remaining slot of it (the first slot of a freshly started wave is
		 * born at its start instant; the rest follow the 0.3 s interval).
		 */
		bool SpawnCurrentWaveFully(FAutomationTestBase& Test, const URoomDefinition& Room, int32 WaveIndex)
		{
			const int32 Count = FMath::Max(0, Room.Waves[WaveIndex].Count);
			const double Span = M2_014_SpawnIntervalSeconds * static_cast<double>(Count - 1) + 0.05;
			return AdvanceSeconds(Test, Span);
		}

		/** One lethal hit through the pawn's own health pool. */
		bool KillPlayer(FAutomationTestBase& Test)
		{
			if (!Test.TestTrue(TEXT("the player health pool accepts the lethal damage"),
				Player->GetHealth()->ApplyDamage(9999.0f) > 0.0f))
			{
				return false;
			}
			return Test.TestTrue(TEXT("the player health pool is dead after the lethal damage"),
				!Player->GetHealth()->IsAlive());
		}

		// -- World bookkeeping helpers --------------------------------------

		/** Alive melee enemies (corpses deliberately not counted). */
		int32 AliveEnemies() const
		{
			int32 Count = 0;
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				UHealthComponent* Health = It->GetHealthComponent();
				if (Health != nullptr && Health->IsAlive())
				{
					++Count;
				}
			}
			return Count;
		}

		/** Every melee enemy actor in the world including corpses. */
		int32 TotalEnemies() const
		{
			int32 Count = 0;
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				++Count;
			}
			return Count;
		}

		/** Every actor in the world (the per-run leak check). */
		int32 TotalActors() const
		{
			int32 Count = 0;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				++Count;
			}
			return Count;
		}

		/**
		 * Finds the alive, never-yet-killed melee enemy closest to the given
		 * configured spawn location. Dead actors and already processed enemies
		 * are skipped, so a fresh enemy that was born exactly where an earlier
		 * wave's or run's corpse lies - and got nudged a few centimetres away
		 * by the physics solver - is still found (the closest live candidate
		 * wins).
		 */
		AMeleeEnemy* FindNextAliveEnemyAt(const FVector& Location) const
		{
			AMeleeEnemy* Best = nullptr;
			double BestDistSquared = TNumericLimits<double>::Max();
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				AMeleeEnemy* Enemy = *It;
				UHealthComponent* Health = (Enemy != nullptr) ? Enemy->GetHealthComponent() : nullptr;
				if (Health == nullptr || !Health->IsAlive())
				{
					continue;
				}
				bool bAlreadyKilled = false;
				for (const TWeakObjectPtr<AActor>& Killed : KilledEnemies)
				{
					if (Killed.Get() == Enemy)
					{
						bAlreadyKilled = true;
						break;
					}
				}
				if (bAlreadyKilled)
				{
					continue;
				}
				const double DistSquared = FVector::DistSquared(Enemy->GetActorLocation(), Location);
				if (DistSquared < BestDistSquared)
				{
					BestDistSquared = DistSquared;
					Best = Enemy;
				}
			}
			return Best;
		}
	};

	// Diagnostics for a failed find: every alive enemy with its position and
	// every already processed enemy count (the position probe of the kill
	// helper's null path).
	static void M2_014_DumpAliveEnemyPositions(FAutomationTestBase& Test, const FM2_014_Scene& Scene)
	{
		for (TActorIterator<AMeleeEnemy> It(Scene.World); It; ++It)
		{
			UHealthComponent* Health = It->GetHealthComponent();
			const bool bAlive = (Health != nullptr && Health->IsAlive());
			Test.AddInfo(FString::Printf(TEXT("world enemy %s alive=%d at %s"),
				*It->GetName(), bAlive ? 1 : 0, *It->GetActorLocation().ToString()));
		}
		Test.AddInfo(FString::Printf(TEXT("already processed enemies: %d"), Scene.KilledEnemies.Num()));
	}

	/**
	 * Finds the next alive, never-yet-killed melee enemy at or near the given
	 * configured spawn location (the closest live candidate wins), verifies
	 * it was born at or near the configured location, kills it through its
	 * health component (the legal lethal ApplyDamage path - the death then
	 * flows through the spawner's real death binding into the session
	 * bookkeeping), registers it as processed and verifies the death took.
	 */
	static bool M2_014_KillNextEnemyAt(FAutomationTestBase& Test, FM2_014_Scene& Scene,
		const FVector& Location, const TCHAR* What)
	{
		AMeleeEnemy* Enemy = Scene.FindNextAliveEnemyAt(Location);
		if (!Test.TestNotNull(What, Enemy))
		{
			M2_014_DumpAliveEnemyPositions(Test, Scene);
			return false;
		}
		const double DistSquared = FVector::DistSquared(Enemy->GetActorLocation(), Location);
		if (!Test.TestTrue(FString::Printf(TEXT("%s was born at or near the configured spawn location (dist %.1f cm)"),
			What, FMath::Sqrt(DistSquared)),
			DistSquared <= M2_014_BirthLocationBandCm * M2_014_BirthLocationBandCm))
		{
			return false;
		}
		Enemy->GetHealthComponent()->ApplyDamage(9999.0f);
		Scene.KilledEnemies.Add(Enemy);
		return Test.TestTrue(TEXT("the enemy died from the applied damage"),
			Enemy->GetHealthComponent() != nullptr && !Enemy->GetHealthComponent()->IsAlive());
	}

	// The expected full-clear event type stream of one 2+3 run: run start,
	// wave 0 (2 spawns, 2 deaths), wave 1 (3 spawns, 3 deaths), one end.
	static TArray<FString> M2_014_ExpectedFullClearTypes()
	{
		return {
			TEXT("RunStarted"),
			TEXT("WaveStarted"),
			TEXT("EnemySpawned"), TEXT("EnemySpawned"),
			TEXT("EnemyDied"), TEXT("EnemyDied"),
			TEXT("WaveStarted"),
			TEXT("EnemySpawned"), TEXT("EnemySpawned"), TEXT("EnemySpawned"),
			TEXT("EnemyDied"), TEXT("EnemyDied"), TEXT("EnemyDied"),
			TEXT("RunEnded")
		};
	}
}

using namespace UE::UEMMO::Tasks::M2_014;

// 1. Acceptance: the full clear of one run - wave 0 (2 enemies) dies, the
//    1.0 s inter-wave wait elapses on the injected clock, wave 1 (3 enemies)
//    dies, and the run clears exactly once. The recorded event stream (the
//    card's spawn/death/result sequence with timestamps) matches the expected
//    order exactly, the wave-1 start sits at last-death + 1.0 s, and the end
//    result carries Cleared with exactly the five real kills.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_014FullClearEventSequenceClearsExactlyOnce,
	"UEMMO.Tasks.M2_014.FullClearEventSequenceClearsExactlyOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_014FullClearEventSequenceClearsExactlyOnce::RunTest(const FString& Parameters)
{
	FM2_014_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_014_MakeRoom();
	UEnemyDefinition* Def = M2_014_MakeEnemyDef();
	const FVector& LocW0A = Room->Waves[0].SpawnLocations[0];
	const FVector& LocW0B = Room->Waves[0].SpawnLocations[1];
	const FVector& LocW1A = Room->Waves[1].SpawnLocations[0];
	const FVector& LocW1B = Room->Waves[1].SpawnLocations[1];
	const FVector& LocW1C = Room->Waves[1].SpawnLocations[2];

	if (!TestTrue(TEXT("StartRoom accepts the run the scenario belongs to"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 RunId = Scene.Session->GetRunId();
	TestTrue(TEXT("BeginWaves starts the progression from wave 0"), Scene.Session->BeginWaves(Room, Def));

	// Wave 0: the anchor injection births slot 0, the 0.3 s slot births the
	// second enemy, then both die through the legal lethal path.
	Scene.InjectClock(0.0);
	TestEqual(TEXT("the anchor injection birthed the first wave-0 enemy"),
		Scene.Session->GetSpawnedEnemyCount(), 1);
	if (!Scene.AdvanceSeconds(*this, M2_014_SpawnIntervalSeconds))
	{
		return true;
	}
	TestEqual(TEXT("both wave-0 enemies were birthed by the 0.3 s slot"),
		Scene.Session->GetSpawnedEnemyCount(), 2);
	TestEqual(TEXT("both wave-0 enemies are alive in the world"), Scene.AliveEnemies(), 2);
	if (!M2_014_KillNextEnemyAt(*this, Scene, LocW0A, TEXT("the first wave-0 enemy was found at its location")))
	{
		return true;
	}
	if (!Scene.AdvanceSeconds(*this, 0.2))
	{
		return true;
	}
	if (!M2_014_KillNextEnemyAt(*this, Scene, LocW0B, TEXT("the second wave-0 enemy was found at its location")))
	{
		return true;
	}
	const double LastWave0DeathTime = Scene.ClockSeconds;
	TestEqual(TEXT("both wave-0 deaths were counted"), Scene.Session->GetKilledCount(), 2);
	TestTrue(TEXT("the completed wave 0 alone does not clear the run (still Running)"),
		Scene.Session->GetState() == ERoomSessionState::Running);

	// 0.5 s into the wait nothing may have started yet (the wait is armed at
	// the last death and must elapse only after the full 1.0 s).
	if (!Scene.AdvanceSeconds(*this, 0.5))
	{
		return true;
	}
	TestTrue(TEXT("0.5 s into the wait the run is still Running"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("0.5 s into the wait the wave index is still 0"), Scene.Session->GetCurrentWaveIndex(), 0);

	// Past the full gap wave 1 starts and births its three enemies.
	if (!Scene.AdvanceSeconds(*this, 0.7))
	{
		return true;
	}
	TestEqual(TEXT("past the full wait the wave index is 1"), Scene.Session->GetCurrentWaveIndex(), 1);
	if (!Scene.SpawnCurrentWaveFully(*this, *Room, 1))
	{
		return true;
	}
	TestEqual(TEXT("all three wave-1 enemies were birthed (run total 2+3)"),
		Scene.Session->GetSpawnedEnemyCount(), 5);
	TestEqual(TEXT("exactly three wave-1 enemies are alive"), Scene.AliveEnemies(), 3);

	// Every wave-1 death is required; the last one clears the run.
	if (!M2_014_KillNextEnemyAt(*this, Scene, LocW1A, TEXT("the first wave-1 enemy was found at its location")))
	{
		return true;
	}
	if (!Scene.AdvanceSeconds(*this, 0.2))
	{
		return true;
	}
	if (!M2_014_KillNextEnemyAt(*this, Scene, LocW1B, TEXT("the second wave-1 enemy was found at its location")))
	{
		return true;
	}
	TestTrue(TEXT("two wave-1 deaths alone keep the run Running"),
		Scene.Session->GetState() == ERoomSessionState::Running);

	if (!Scene.AdvanceSeconds(*this, 0.2))
	{
		return true;
	}
	if (!M2_014_KillNextEnemyAt(*this, Scene, LocW1C, TEXT("the third wave-1 enemy was found at its location")))
	{
		return true;
	}

	// Settlement: cleared exactly once, with exactly the five real kills.
	TestTrue(TEXT("the run is Cleared after the last wave-1 death"),
		Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("exactly five enemies were spawned for the run"), Scene.Session->GetSpawnedEnemyCount(), 5);
	TestEqual(TEXT("exactly five enemies were killed for the run"), Scene.Session->GetKilledCount(), 5);
	TestEqual(TEXT("OnRunEnded fired exactly once for the whole scenario"), Scene.EndedCount, 1);
	if (Scene.EndedResults.Num() == 1)
	{
		TestTrue(TEXT("the end result records Cleared"), Scene.EndedResults[0].bCleared);
		TestEqual(TEXT("the end result keeps the run id"), Scene.EndedResults[0].RunId, RunId);
		TestEqual(TEXT("the end result counts exactly the five real deaths"), Scene.EndedResults[0].KilledCount, 5);
	}
	// The M2-013 result archive: the cleared run is queryable and reward
	// eligible exactly once by its run id.
	FRoomResult Archived;
	TestTrue(TEXT("the cleared run's result is queryable from the archive"), Scene.Session->GetRunResult(RunId, Archived));
	TestTrue(TEXT("the cleared run's archived result records Cleared"), Archived.bCleared);
	TestTrue(TEXT("the cleared run is reward eligible"), Scene.Session->IsRewardEligible(RunId));
	TestFalse(TEXT("a duplicate MarkCleared after the clear is rejected"), Scene.Session->MarkCleared());
	TestEqual(TEXT("the duplicate clear fired no second end event"), Scene.EndedCount, 1);

	// The event stream: exact order, monotonic timestamps, the wave-1 start
	// on the 1.0 s deadline after the last wave-0 death, and the end after
	// the last death.
	const TArray<FString> ActualTypes = Scene.Log.Types();
	TestTrue(TEXT("the recorded event type stream matches the expected spawn/death/result order"),
		ActualTypes == M2_014_ExpectedFullClearTypes());
	TestTrue(TEXT("the event timestamps never move backwards"), Scene.Log.TimesAreMonotonic());
	const TArray<double> SpawnTimes = Scene.Log.TimesOf(TEXT("EnemySpawned"));
	const TArray<double> DeathTimes = Scene.Log.TimesOf(TEXT("EnemyDied"));
	const TArray<double> WaveStartTimes = Scene.Log.TimesOf(TEXT("WaveStarted"));
	const TArray<double> EndTimes = Scene.Log.TimesOf(TEXT("RunEnded"));
	TestTrue(TEXT("exactly five spawns, five deaths, two wave starts and one end were recorded"),
		SpawnTimes.Num() == 5 && DeathTimes.Num() == 5 && WaveStartTimes.Num() == 2 && EndTimes.Num() == 1);
	TestTrue(TEXT("the first enemy was born at the anchor instant"), SpawnTimes[0] <= 0.02);
	TestTrue(TEXT("the second enemy was born at the 0.3 s slot"), SpawnTimes[1] <= M2_014_SpawnIntervalSeconds + M2_014_DeadlineLateSeconds);
	TestTrue(TEXT("wave 1 started at the 1.0 s deadline after the last wave-0 death"),
		WaveStartTimes[1] >= LastWave0DeathTime + M2_014_WaveGapSeconds - M2_014_DeadlineEarlySeconds
		&& WaveStartTimes[1] <= LastWave0DeathTime + M2_014_WaveGapSeconds + M2_014_DeadlineLateSeconds);
	TestTrue(TEXT("the run ended at or after the last enemy death"), EndTimes[0] >= DeathTimes[4] - 1e-9);
	AddInfo(TEXT("Full clear event order:\n") + Scene.Log.Describe());

	// The card's JSON evidence for the success scenario.
	if (!M2_014_WriteScenarioJson(*this, TEXT("m2-014-scenario-clear.json"), TEXT("full-clear"),
		Scene.Log,
		FString::Printf(TEXT("\"run_id\": %llu, \"outcome\": \"Cleared\", \"spawned\": 5, \"killed\": 5, \"end_events\": 1"), RunId)))
	{
		return true;
	}
	return true;
}

// 2. Acceptance (the card's negative): the last wave with one enemy still
//    alive never settles - far past every gap the run stays Running, no end
//    event fires and no further spawn happens; only the last death clears.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_014PartialLastWaveStaysRunningWithoutSettlement,
	"UEMMO.Tasks.M2_014.PartialLastWaveStaysRunningWithoutSettlement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_014PartialLastWaveStaysRunningWithoutSettlement::RunTest(const FString& Parameters)
{
	FM2_014_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_014_MakeRoom();
	UEnemyDefinition* Def = M2_014_MakeEnemyDef();
	const FVector& LocW0A = Room->Waves[0].SpawnLocations[0];
	const FVector& LocW0B = Room->Waves[0].SpawnLocations[1];
	const FVector& LocW1A = Room->Waves[1].SpawnLocations[0];
	const FVector& LocW1B = Room->Waves[1].SpawnLocations[1];
	const FVector& LocW1C = Room->Waves[1].SpawnLocations[2];

	if (!TestTrue(TEXT("the run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	TestTrue(TEXT("the progression begins"), Scene.Session->BeginWaves(Room, Def));

	// Wave 0 fully born, both killed.
	Scene.InjectClock(0.0);
	if (!Scene.SpawnCurrentWaveFully(*this, *Room, 0))
	{
		return true;
	}
	TestEqual(TEXT("both wave-0 enemies were birthed"), Scene.Session->GetSpawnedEnemyCount(), 2);
	if (!M2_014_KillNextEnemyAt(*this, Scene, LocW0A, TEXT("the first wave-0 enemy was found"))
		|| !M2_014_KillNextEnemyAt(*this, Scene, LocW0B, TEXT("the second wave-0 enemy was found")))
	{
		return true;
	}

	// Wave 1 fully born (2+3 in total), then only two of three die.
	if (!Scene.AdvanceSeconds(*this, M2_014_WaveGapSeconds + 0.05))
	{
		return true;
	}
	if (!Scene.SpawnCurrentWaveFully(*this, *Room, 1))
	{
		return true;
	}
	TestEqual(TEXT("all five enemies were birthed for the run"), Scene.Session->GetSpawnedEnemyCount(), 5);
	if (!M2_014_KillNextEnemyAt(*this, Scene, LocW1A, TEXT("the first wave-1 enemy was found"))
		|| !M2_014_KillNextEnemyAt(*this, Scene, LocW1B, TEXT("the second wave-1 enemy was found")))
	{
		return true;
	}
	TestEqual(TEXT("exactly four kills were counted so far"), Scene.Session->GetKilledCount(), 4);
	TestEqual(TEXT("exactly one wave-1 enemy is still alive"), Scene.AliveEnemies(), 1);

	// Far past every gap: no settlement, no wave change, no extra spawn.
	if (!Scene.AdvanceSeconds(*this, 5.0))
	{
		return true;
	}
	TestTrue(TEXT("one alive last-wave enemy keeps the run Running far past the gap"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	TestFalse(TEXT("no Cleared state was reached with one enemy alive"),
		Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("no end event fired while the last enemy is alive"), Scene.EndedCount, 0);
	TestEqual(TEXT("the wave index stayed at the last wave"), Scene.Session->GetCurrentWaveIndex(), 1);
	TestEqual(TEXT("no enemy was born during the wait"), Scene.Session->GetSpawnedEnemyCount(), 5);
	TestEqual(TEXT("the world still holds exactly five enemies (five corpses plus the alive one)"),
		Scene.TotalEnemies(), 5);

	// The last death clears - the block was exactly the alive enemy.
	if (!M2_014_KillNextEnemyAt(*this, Scene, LocW1C, TEXT("the last wave-1 enemy was found")))
	{
		return true;
	}
	TestTrue(TEXT("the last death cleared the run"), Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("the settlement fired exactly one end event"), Scene.EndedCount, 1);
	if (Scene.EndedResults.Num() == 1)
	{
		TestTrue(TEXT("the end result records Cleared"), Scene.EndedResults[0].bCleared);
		TestEqual(TEXT("the end result counts exactly the five real deaths"), Scene.EndedResults[0].KilledCount, 5);
	}
	AddInfo(TEXT("Negative scenario event order:\n") + Scene.Log.Describe());
	return true;
}

// 3. Acceptance: the player pawn dies mid-run (health pool -> the real
//    PlayerDied broadcast), the run fails first-terminal-wins with no late
//    spawns during the wait; RetryRoom destroys only the failed run's
//    enemies, starts a fresh run (new RunId) that clears normally with its
//    own 2+3 progression and exactly one new end event.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_014PlayerDeathFailRetryThenFreshClear,
	"UEMMO.Tasks.M2_014.PlayerDeathFailRetryThenFreshClear",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_014PlayerDeathFailRetryThenFreshClear::RunTest(const FString& Parameters)
{
	FM2_014_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_014_MakeRoom();
	UEnemyDefinition* Def = M2_014_MakeEnemyDef();
	const FVector& LocW0A = Room->Waves[0].SpawnLocations[0];
	const FVector& LocW0B = Room->Waves[0].SpawnLocations[1];
	const FVector& LocW1A = Room->Waves[1].SpawnLocations[0];
	const FVector& LocW1B = Room->Waves[1].SpawnLocations[1];
	const FVector& LocW1C = Room->Waves[1].SpawnLocations[2];

	if (!TestTrue(TEXT("the first run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 OldRunId = Scene.Session->GetRunId();
	TestTrue(TEXT("the first progression begins"), Scene.Session->BeginWaves(Room, Def));

	// Wave 0 fully born, one enemy killed, then the player dies.
	Scene.InjectClock(0.0);
	if (!Scene.SpawnCurrentWaveFully(*this, *Room, 0))
	{
		return true;
	}
	TestEqual(TEXT("both wave-0 enemies were birthed before the death"), Scene.Session->GetSpawnedEnemyCount(), 2);
	if (!M2_014_KillNextEnemyAt(*this, Scene, LocW0A, TEXT("the first wave-0 enemy was found before the death")))
	{
		return true;
	}
	if (!Scene.KillPlayer(*this))
	{
		return true;
	}
	TestEqual(TEXT("the death broadcast fired exactly once for the lifecycle"), Scene.PlayerDiedCount, 1);
	TestTrue(TEXT("the player death failed the running run"), Scene.Session->GetState() == ERoomSessionState::Failed);
	TestEqual(TEXT("the failure fired exactly one end event"), Scene.EndedCount, 1);
	if (Scene.EndedResults.Num() == 1)
	{
		TestTrue(TEXT("the end result records the failure"), !Scene.EndedResults[0].bCleared);
		TestEqual(TEXT("the end result keeps the failed run id"), Scene.EndedResults[0].RunId, OldRunId);
		TestEqual(TEXT("the failed run counted exactly its one kill"), Scene.EndedResults[0].KilledCount, 1);
	}
	TestFalse(TEXT("the failed run is not reward eligible"), Scene.Session->IsRewardEligible(OldRunId));
	TestEqual(TEXT("no unborn spawns remain after the failure"), Scene.Session->GetRunPendingSpawnCount(), 0);

	// The 2 s acceptance window: no late spawn anywhere, no second end event.
	if (!Scene.AdvanceSeconds(*this, 2.0))
	{
		return true;
	}
	TestEqual(TEXT("no enemy was born during the 2 s wait after the failure"),
		Scene.Session->GetSpawnedEnemyCount(), 2);
	TestEqual(TEXT("the world holds exactly the two pre-death enemies"), Scene.TotalEnemies(), 2);
	TestEqual(TEXT("no further end event fired during the wait"), Scene.EndedCount, 1);

	// The retry: one call, full fresh run (the M2-010 service).
	if (!TestTrue(TEXT("RetryRoom restarts the failed run"),
		URoomRetryService::RetryRoom(Scene.World, Room, Def, Scene.Player)))
	{
		return true;
	}
	const uint64 NewRunId = Scene.Session->GetRunId();
	TestTrue(TEXT("the retry started a new, larger run id"), NewRunId > OldRunId);
	TestTrue(TEXT("the retried session is Running"), Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("the retry destroyed the failed run's registered enemies (no corpse left)"),
		Scene.TotalEnemies(), 0);
	TestEqual(TEXT("the new run restarted from wave 0"), Scene.Session->GetCurrentWaveIndex(), 0);
	TestEqual(TEXT("the new run counts no old kills"), Scene.Session->GetKilledCount(), 0);
	TestEqual(TEXT("the new run counts no old spawns"), Scene.Session->GetSpawnedEnemyCount(), 0);

	// The fresh run clears its own 2+3 progression normally.
	Scene.InjectClock(Scene.ClockSeconds);
	if (!Scene.SpawnCurrentWaveFully(*this, *Room, 0))
	{
		return true;
	}
	TestEqual(TEXT("the new run's wave 0 birthed exactly its two enemies"),
		Scene.Session->GetSpawnedEnemyCount(), 2);
	if (!M2_014_KillNextEnemyAt(*this, Scene, LocW0A, TEXT("the new run's first wave-0 enemy was found"))
		|| !M2_014_KillNextEnemyAt(*this, Scene, LocW0B, TEXT("the new run's second wave-0 enemy was found")))
	{
		return true;
	}
	if (!Scene.AdvanceSeconds(*this, M2_014_WaveGapSeconds + 0.05))
	{
		return true;
	}
	if (!Scene.SpawnCurrentWaveFully(*this, *Room, 1))
	{
		return true;
	}
	TestEqual(TEXT("the new run's wave 1 birthed exactly its three enemies"),
		Scene.Session->GetSpawnedEnemyCount(), 5);
	if (!M2_014_KillNextEnemyAt(*this, Scene, LocW1A, TEXT("the new run's first wave-1 enemy was found"))
		|| !M2_014_KillNextEnemyAt(*this, Scene, LocW1B, TEXT("the new run's second wave-1 enemy was found"))
		|| !M2_014_KillNextEnemyAt(*this, Scene, LocW1C, TEXT("the new run's third wave-1 enemy was found")))
	{
		return true;
	}
	TestTrue(TEXT("the retried run cleared normally"), Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("exactly two end events fired (failure, then clear)"), Scene.EndedCount, 2);
	if (Scene.EndedResults.Num() == 2)
	{
		TestTrue(TEXT("the retried run's result is Cleared"), Scene.EndedResults[1].bCleared);
		TestEqual(TEXT("the retried run's result carries the new run id"), Scene.EndedResults[1].RunId, NewRunId);
		TestEqual(TEXT("the retried run's result counts exactly its five real deaths"),
			Scene.EndedResults[1].KilledCount, 5);
	}
	TestTrue(TEXT("the retried run is reward eligible"), Scene.Session->IsRewardEligible(NewRunId));
	FRoomResult Archived;
	TestTrue(TEXT("the retried run's result is queryable from the archive"), Scene.Session->GetRunResult(NewRunId, Archived));
	TestTrue(TEXT("the failed run's result stays queryable too"), Scene.Session->GetRunResult(OldRunId, Archived));

	// The card's JSON evidence for the failure-retry scenario.
	AddInfo(TEXT("Fail-retry scenario event order:\n") + Scene.Log.Describe());
	if (!M2_014_WriteScenarioJson(*this, TEXT("m2-014-scenario-fail-retry.json"), TEXT("fail-retry"),
		Scene.Log,
		FString::Printf(TEXT("\"run_ids\": [%llu, %llu], \"outcome\": \"Failed-then-Cleared\", \"player_deaths\": 1, \"end_events\": 2"),
			OldRunId, NewRunId)))
	{
		return true;
	}
	return true;
}

// 4. Acceptance: LeaveRoom while wave 0 is still generating - the unborn
//    spawns are cancelled with no late birth, the exit is NOT a settlement
//    (no end event, the exited run is not queryable as a result), a stale
//    death of the leftover enemy changes nothing, and re-entering the room
//    starts a fresh run that clears normally.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_014MidRunExitStopsSpawnsAndReentryClears,
	"UEMMO.Tasks.M2_014.MidRunExitStopsSpawnsAndReentryClears",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_014MidRunExitStopsSpawnsAndReentryClears::RunTest(const FString& Parameters)
{
	FM2_014_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_014_MakeRoom();
	UEnemyDefinition* Def = M2_014_MakeEnemyDef();
	const FVector& LocW0A = Room->Waves[0].SpawnLocations[0];
	const FVector& LocW0B = Room->Waves[0].SpawnLocations[1];
	const FVector& LocW1A = Room->Waves[1].SpawnLocations[0];
	const FVector& LocW1B = Room->Waves[1].SpawnLocations[1];
	const FVector& LocW1C = Room->Waves[1].SpawnLocations[2];

	if (!TestTrue(TEXT("the run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 ExitedRunId = Scene.Session->GetRunId();
	TestTrue(TEXT("the progression begins"), Scene.Session->BeginWaves(Room, Def));

	// Wave 0 mid-generation: one enemy born, one still unborn, then leave.
	Scene.InjectClock(0.0);
	TestEqual(TEXT("one wave-0 enemy was born before the exit"), Scene.Session->GetSpawnedEnemyCount(), 1);
	TestEqual(TEXT("one enemy is still unborn before the exit"), Scene.Session->GetRunPendingSpawnCount(), 1);
	if (!TestTrue(TEXT("LeaveRoom accepts the mid-generation exit"), Scene.Session->LeaveRoom()))
	{
		return true;
	}
	TestTrue(TEXT("the session is Exiting after the leave"), Scene.Session->GetState() == ERoomSessionState::Exiting);
	TestEqual(TEXT("the exit cancelled the unborn spawns"), Scene.Session->GetRunPendingSpawnCount(), 0);
	TestEqual(TEXT("the exit fired no end event (an exit is not a settlement)"), Scene.EndedCount, 0);

	// The 3 s acceptance window: no late spawn anywhere.
	if (!Scene.AdvanceSeconds(*this, 3.0))
	{
		return true;
	}
	TestEqual(TEXT("no late enemy was born during the 3 s wait after the exit"), Scene.TotalEnemies(), 1);
	TestEqual(TEXT("no end event fired during the wait"), Scene.EndedCount, 0);
	FRoomResult ExitedResult;
	TestFalse(TEXT("the exited run never reached a terminal result (no archived result)"),
		Scene.Session->GetRunResult(ExitedRunId, ExitedResult));
	TestFalse(TEXT("the exited run is not reward eligible"), Scene.Session->IsRewardEligible(ExitedRunId));

	// A stale death of the leftover enemy during Exiting changes nothing.
	if (!M2_014_KillNextEnemyAt(*this, Scene, LocW0A, TEXT("the exited run's leftover enemy was found")))
	{
		return true;
	}
	TestEqual(TEXT("the stale death fired no end event"), Scene.EndedCount, 0);
	TestTrue(TEXT("the stale death left the session Exiting"),
		Scene.Session->GetState() == ERoomSessionState::Exiting);

	// Re-enter: a fresh run from the post-exit Exiting state clears normally.
	if (!TestTrue(TEXT("StartRoom is accepted again from Exiting"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 NewRunId = Scene.Session->GetRunId();
	TestTrue(TEXT("the re-entry started a new, larger run id"), NewRunId > ExitedRunId);
	TestTrue(TEXT("the re-entry progression begins"), Scene.Session->BeginWaves(Room, Def));
	Scene.InjectClock(Scene.ClockSeconds);
	if (!Scene.SpawnCurrentWaveFully(*this, *Room, 0))
	{
		return true;
	}
	TestEqual(TEXT("the re-entry's wave 0 birthed exactly its two enemies (world holds the old corpse plus the two)"),
		Scene.Session->GetSpawnedEnemyCount(), 2);
	if (!M2_014_KillNextEnemyAt(*this, Scene, LocW0A, TEXT("the re-entry's first wave-0 enemy was found"))
		|| !M2_014_KillNextEnemyAt(*this, Scene, LocW0B, TEXT("the re-entry's second wave-0 enemy was found")))
	{
		return true;
	}
	if (!Scene.AdvanceSeconds(*this, M2_014_WaveGapSeconds + 0.05))
	{
		return true;
	}
	if (!Scene.SpawnCurrentWaveFully(*this, *Room, 1))
	{
		return true;
	}
	TestEqual(TEXT("the re-entry's wave 1 birthed exactly its three enemies"),
		Scene.Session->GetSpawnedEnemyCount(), 5);
	if (!M2_014_KillNextEnemyAt(*this, Scene, LocW1A, TEXT("the re-entry's first wave-1 enemy was found"))
		|| !M2_014_KillNextEnemyAt(*this, Scene, LocW1B, TEXT("the re-entry's second wave-1 enemy was found"))
		|| !M2_014_KillNextEnemyAt(*this, Scene, LocW1C, TEXT("the re-entry's third wave-1 enemy was found")))
	{
		return true;
	}
	TestTrue(TEXT("the re-entry run cleared"), Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("exactly one end event fired for the whole scene (only the re-entry's clear)"),
		Scene.EndedCount, 1);
	if (Scene.EndedResults.Num() == 1)
	{
		TestTrue(TEXT("the re-entry's result is Cleared"), Scene.EndedResults[0].bCleared);
		TestEqual(TEXT("the re-entry's result carries the new run id"), Scene.EndedResults[0].RunId, NewRunId);
		TestEqual(TEXT("the re-entry's result counts exactly its five real deaths"),
			Scene.EndedResults[0].KilledCount, 5);
	}
	TestTrue(TEXT("the re-entry run is reward eligible"), Scene.Session->IsRewardEligible(NewRunId));

	// The card's JSON evidence for the mid-exit scenario.
	AddInfo(TEXT("Mid-exit scenario event order:\n") + Scene.Log.Describe());
	if (!M2_014_WriteScenarioJson(*this, TEXT("m2-014-scenario-exit.json"), TEXT("mid-run-exit"),
		Scene.Log,
		FString::Printf(TEXT("\"run_ids\": [%llu, %llu], \"outcome\": \"Exited-then-Cleared\", \"late_spawns_after_exit\": 0, \"end_events\": 1"),
			ExitedRunId, NewRunId)))
	{
		return true;
	}
	return true;
}

// 5. Acceptance: three consecutive full runs on one world never accumulate
//    anything - every run spawns exactly 5, kills exactly 5, clears exactly
//    once, adds exactly 5 enemy actors and ends with zero alive enemies; the
//    inter-wave wait is re-armed per run (0.5 s after the wave-0 deaths the
//    next wave has not started, so no stale timer from an earlier run fires).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_014ThreeConsecutiveRunsLeaveNoResidue,
	"UEMMO.Tasks.M2_014.ThreeConsecutiveRunsLeaveNoResidue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_014ThreeConsecutiveRunsLeaveNoResidue::RunTest(const FString& Parameters)
{
	FM2_014_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_014_MakeRoom();
	UEnemyDefinition* Def = M2_014_MakeEnemyDef();
	const FVector& LocW0A = Room->Waves[0].SpawnLocations[0];
	const FVector& LocW0B = Room->Waves[0].SpawnLocations[1];
	const FVector& LocW1A = Room->Waves[1].SpawnLocations[0];
	const FVector& LocW1B = Room->Waves[1].SpawnLocations[1];
	const FVector& LocW1C = Room->Waves[1].SpawnLocations[2];

	uint64 PreviousRunId = 0;
	for (int32 RunIndex = 1; RunIndex <= 3; ++RunIndex)
	{
		const FString Context = FString::Printf(TEXT("run %d"), RunIndex);
		const int32 EnemiesBefore = Scene.TotalEnemies();
		const int32 ActorsBefore = Scene.TotalActors();

		if (!TestTrue(FString::Printf(TEXT("%s starts"), *Context), Scene.Session->StartRoom(Room)))
		{
			return true;
		}
		const uint64 RunId = Scene.Session->GetRunId();
		TestTrue(FString::Printf(TEXT("%s carries a strictly larger run id"), *Context), RunId > PreviousRunId);
		PreviousRunId = RunId;
		if (!TestTrue(FString::Printf(TEXT("%s begins its waves"), *Context), Scene.Session->BeginWaves(Room, Def)))
		{
			return true;
		}

		// Wave 0: anchor, both born, both killed.
		Scene.InjectClock(Scene.ClockSeconds);
		if (!Scene.SpawnCurrentWaveFully(*this, *Room, 0))
		{
			return true;
		}
		TestEqual(FString::Printf(TEXT("%s birthed exactly two wave-0 enemies"), *Context),
			Scene.Session->GetSpawnedEnemyCount(), 2);
		if (!M2_014_KillNextEnemyAt(*this, Scene, LocW0A, TEXT("the first wave-0 enemy was found"))
			|| !M2_014_KillNextEnemyAt(*this, Scene, LocW0B, TEXT("the second wave-0 enemy was found")))
		{
			return true;
		}

		// Timer freshness: 0.5 s after the wave-0 deaths the next wave must
		// not have started (a stale armed wait from an earlier run would
		// fire on the very first injection after the kill).
		if (!Scene.AdvanceSeconds(*this, 0.5))
		{
			return true;
		}
		TestEqual(FString::Printf(TEXT("%s: 0.5 s into the wait the wave index is still 0 (no stale timer)"), *Context),
			Scene.Session->GetCurrentWaveIndex(), 0);

		// Wave 1: past the full gap, three born, three killed, one clear.
		if (!Scene.AdvanceSeconds(*this, M2_014_WaveGapSeconds + 0.05))
		{
			return true;
		}
		if (!Scene.SpawnCurrentWaveFully(*this, *Room, 1))
		{
			return true;
		}
		TestEqual(FString::Printf(TEXT("%s birthed all five of its enemies (2+3)"), *Context),
			Scene.Session->GetSpawnedEnemyCount(), 5);
		if (!M2_014_KillNextEnemyAt(*this, Scene, LocW1A, TEXT("the first wave-1 enemy was found"))
			|| !M2_014_KillNextEnemyAt(*this, Scene, LocW1B, TEXT("the second wave-1 enemy was found"))
			|| !M2_014_KillNextEnemyAt(*this, Scene, LocW1C, TEXT("the third wave-1 enemy was found")))
		{
			return true;
		}

		// Per-run settlement and world bookkeeping: one clear, five kills,
		// exactly five new actors (the corpses), zero alive enemies.
		TestTrue(FString::Printf(TEXT("%s cleared"), *Context), Scene.Session->GetState() == ERoomSessionState::Cleared);
		TestEqual(FString::Printf(TEXT("%s counted exactly five kills"), *Context), Scene.Session->GetKilledCount(), 5);
		TestEqual(FString::Printf(TEXT("%s fired exactly its own end event"), *Context), Scene.EndedCount, RunIndex);
		TestEqual(FString::Printf(TEXT("%s fired exactly its own start event"), *Context), Scene.StartedCount, RunIndex);
		if (Scene.EndedResults.Num() == RunIndex)
		{
			TestTrue(FString::Printf(TEXT("%s's end result is Cleared"), *Context),
				Scene.EndedResults[RunIndex - 1].bCleared);
			TestEqual(FString::Printf(TEXT("%s's end result keeps its run id"), *Context),
				Scene.EndedResults[RunIndex - 1].RunId, RunId);
		}
		TestTrue(FString::Printf(TEXT("%s is reward eligible"), *Context), Scene.Session->IsRewardEligible(RunId));
		TestEqual(FString::Printf(TEXT("%s left zero alive enemies"), *Context), Scene.AliveEnemies(), 0);
		TestEqual(FString::Printf(TEXT("%s added exactly five enemy actors to the world"), *Context),
			Scene.TotalEnemies() - EnemiesBefore, 5);
		TestEqual(FString::Printf(TEXT("%s added exactly five actors in total"), *Context),
			Scene.TotalActors() - ActorsBefore, 5);

		// The between-runs flow: leave the cleared room (exit, no event)
		// before the next run starts from the post-exit state.
		if (!TestTrue(FString::Printf(TEXT("%s leaves the room after the clear"), *Context), Scene.Session->LeaveRoom()))
		{
			return true;
		}
		TestEqual(FString::Printf(TEXT("%s's exit fired no extra end event"), *Context), Scene.EndedCount, RunIndex);
	}

	// No delegate stacking and no extra settlement across the whole loop.
	TestEqual(TEXT("three runs produced exactly three start events"), Scene.StartedCount, 3);
	TestEqual(TEXT("three runs produced exactly three end events"), Scene.EndedCount, 3);
	TestEqual(TEXT("the world never saw a player death"), Scene.PlayerDiedCount, 0);
	AddInfo(TEXT("Three-run scenario event order:\n") + Scene.Log.Describe());
	return true;
}

#endif
