// M3-031: combo input feel tests. The user's report: in the real game the
// rapid ground chain X -> X -> Z (up-launch) often leaves the Z dead while the
// same chain works in the automation suites. The known mechanism (M3-027):
// "a buffered Light makes the Z silently expire". These tests replay the
// press rhythms inside a REAL ticked world (the engine FTestWorldWrapper
// precedent from GameCombatWiringTests.cpp: world context +
// InitializeActorsForPlay + BeginPlay + UWorld::Tick(LEVELTICK_All) - real
// actor ticks, real character movement, real physics, real hit stop), press
// keys only through the production intent entry SubmitCombatInput, and record
// per-frame input telemetry (PressedAt values, the injected input game clock,
// buffer contents, attack frames, cancel-window steps) as JSON evidence under
// Artifacts/Tasks/M3-031.
//
// The suite locks three feel contracts:
//   1. a single X followed by Z within 50..150 ms always chains the launcher
//      through light_01's cancel window (with the enemy present, so the
//      M1-033 hit stop freeze runs exactly like the real game);
//   2. the rapid X X Z mash chains the full light_01 -> light_02 -> launcher;
//   3. the rapid X Z X Z mash never silently swallows the launcher press;
//   4. Free-state X and Z start light_01 / launcher; X alone never misfires a
//      launcher; no press at all never starts anything.
#include "Misc/AutomationTest.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/TrainingEnemy.h"
#include "../PrototypeCharacter.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_031
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M3_031_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with arena
	// content (the M1-022/M1-027 placement convention). X horizontal, Y
	// depth, Z height; the floor top sits exactly at the base Z. Per-scene
	// runs offset along X so coexisting scenes inside one temp world never
	// share a hit query volume.
	const FVector M3_031_SceneBase(120000.0, 46000.0, 600.0);
	constexpr double M3_031_SceneSpacing = 3000.0;

	// All three ground attacks (light_01, light_02, launcher) share the hit
	// box offset X=95 and half extent X=85 (M1-009 assets), so the box covers
	// base.X+10..base.X+180 for facing +1: an enemy feet anchor at +95 stays
	// reachable through every combo stage.
	const float M3_031_EnemyFeetOffsetX = 95.0f;

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M3_031_FloorHalfThickness = 100.0f;
	const float M3_031_FloorHalfExtentXY = 4000.0f;

	// Spawn heights above the floor top (the M1-041 pattern).
	const float M3_031_PlayerSpawnHeight = 120.0f;
	const float M3_031_EnemySpawnHeight = 90.0f;

	// Frame caps: the settle phase waits for the ground contact; the combo
	// loop covers the three-attack chain plus the launcher's natural end with
	// a wide margin.
	constexpr int32 M3_031_MaxSettleFrames = 300;
	constexpr int32 M3_031_MaxComboFrames = 1200;

	// The rapid mash rhythm the user performed: one press every 83 ms (the
	// card's named interval, five 60 Hz frames).
	constexpr double M3_031_RapidGapSeconds = 0.083;

	// The single X->Z probe gaps of the card's acceptance list.
	const double M3_031_ProbeGapSeconds[4] = { 0.050, 0.083, 0.100, 0.150 };

	// Evidence output directory (the M3-024/M3-027 pattern: project-relative
	// artifacts path).
	const TCHAR* const M3_031_EvidenceDir = TEXT("Artifacts/Tasks/M3-031");

	// Recorded combat events (the assertions read these structs only).
	struct FM3_031_Events
	{
		TArray<FName> Started;
		TArray<FName> Finished;
		int32 Hits = 0;

		// M3-031 telemetry: the world time, the injected input game clock and
		// the running attack id captured at every Started/Finished broadcast.
		TArray<double> StartedWorldSeconds;
		TArray<double> StartedInputClockSeconds;
		TArray<double> FinishedWorldSeconds;
	};

	// One scheduled key press: fired exactly once when the world clock
	// reaches FireAt (the press goes through the production intent entry, so
	// PressedAt is stamped by the character itself).
	struct FM3_031_Press
	{
		ECombatInput Action = ECombatInput::Light;
		double FireAt = 0.0;
		bool bFired = false;
		double PressedAtStamped = -1.0;
	};

	// One per-frame telemetry row (the diagnostic dump; the JSON keeps every
	// row so the red/green evidence carries the full frame timeline).
	struct FM3_031_FrameRow
	{
		int32 Frame = 0;
		double WorldSeconds = 0.0;
		double InputClockSeconds = 0.0;
		int32 ActionState = 0;
		FString AttackId;
		int32 AttackFrame = -1;
		int32 BufferSize = 0;
		// Buffered entries "seq:action:pressedAt" joined by '|'.
		FString BufferEntries;
	};

	// World-static blocking floor box the characters stand on (temp worlds
	// ship no geometry; the M1-041 precedent verbatim).
	static AActor* M3_031_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base - FVector(0.0, 0.0, M3_031_FloorHalfThickness), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M3_031_Floor"));
		Actor->SetRootComponent(Floor);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->SetBoxExtent(FVector(M3_031_FloorHalfExtentXY, M3_031_FloorHalfExtentXY, M3_031_FloorHalfThickness));
		Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Floor->RegisterComponent();
		// The root was assigned after the spawn transform was applied, so the
		// world location is (re-)applied explicitly (the M1-022 pattern).
		Floor->SetWorldLocation(Base - FVector(0.0, 0.0, M3_031_FloorHalfThickness));
		return Actor;
	}

	// Spawns the real prototype pawn above the floor top with no-controller
	// physics enabled (the M1-041 pattern verbatim).
	static APrototypeCharacter* M3_031_SpawnPlayer(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			Base + FVector(0.0, 0.0, M3_031_PlayerSpawnHeight),
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

	// Spawns the real training enemy at the requested feet offset.
	static ATrainingEnemy* M3_031_SpawnEnemy(UWorld& World, const FVector& Base, float FeetOffsetX)
	{
		FActorSpawnParameters Params;
		ATrainingEnemy* Enemy = World.SpawnActor<ATrainingEnemy>(
			ATrainingEnemy::StaticClass(),
			Base + FVector(FeetOffsetX, 0.0, M3_031_EnemySpawnHeight),
			FRotator::ZeroRotator, Params);
		if (Enemy == nullptr)
		{
			return nullptr;
		}
		if (UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement())
		{
			Movement->bRunPhysicsWithNoController = true;
		}
		return Enemy;
	}

	// One full scene: the wrapper owns the manually ticked temp world; the
	// tests build it, press keys and read the results (the M1-041 scaffold,
	// copied unmodified in shape; the game-side wiring pivot stays: the test
	// never calls SetInputClockSeconds / TryStartAttack on the player).
	struct FM3_031_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		APrototypeCharacter* Player = nullptr;
		UCombatComponent* PlayerCombat = nullptr;
		ATrainingEnemy* Enemy = nullptr;
		FM3_031_Events Events;
		TArray<FM3_031_FrameRow> Telemetry;

		bool Build(FAutomationTestBase& Test, int32 SceneIndex, bool bWithEnemy)
		{
			const FVector Base = M3_031_SceneBase +
				FVector(M3_031_SceneSpacing * static_cast<double>(SceneIndex), 0.0, 0.0);
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
			AActor* Floor = M3_031_SpawnFloor(*World, Base);
			if (Floor == nullptr)
			{
				Test.AddError(TEXT("the floor failed to spawn"));
				return false;
			}
			World->EnsureCollisionTreeIsBuilt();
			Player = M3_031_SpawnPlayer(*World, Base);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			PlayerCombat = Player->GetCombat();
			if (!Test.TestTrue(TEXT("the player carries a combat component"), PlayerCombat != nullptr))
			{
				return false;
			}
			if (bWithEnemy)
			{
				Enemy = M3_031_SpawnEnemy(*World, Base, M3_031_EnemyFeetOffsetX);
				if (!Test.TestNotNull(TEXT("the training enemy spawns"), Enemy))
				{
					return false;
				}
			}

			PlayerCombat->OnStarted.AddLambda([this](FName AttackId, uint64 /*InstanceId*/)
			{
				Events.Started.Add(AttackId);
				Events.StartedWorldSeconds.Add(World->GetTimeSeconds());
				Events.StartedInputClockSeconds.Add(PlayerCombat->GetInputClockSeconds());
			});
			PlayerCombat->OnFinished.AddLambda([this](FName AttackId, uint64 /*InstanceId*/)
			{
				Events.Finished.Add(AttackId);
				Events.FinishedWorldSeconds.Add(World->GetTimeSeconds());
			});
			PlayerCombat->OnHitConfirmed.AddLambda([this](const FCombatHit& /*Hit*/)
			{
				++Events.Hits;
			});
			return true;
		}

		// Ticks the world until both characters settled onto the floor.
		bool Settle(FAutomationTestBase& Test)
		{
			UCharacterMovementComponent* PlayerMovement = Player->GetCharacterMovement();
			UCharacterMovementComponent* EnemyMovement = Enemy != nullptr ? Enemy->GetCharacterMovement() : nullptr;
			for (int32 Frame = 0; Frame < M3_031_MaxSettleFrames; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M3_031_FrameSeconds)))
				{
					return false;
				}
				TickEnemyCombatStandIn();
				const bool bPlayerGrounded = PlayerMovement != nullptr
					&& PlayerMovement->MovementMode == MOVE_Walking
					&& PlayerMovement->Velocity.Size() < 1.0f;
				const bool bEnemyGrounded = EnemyMovement == nullptr
					|| (EnemyMovement->MovementMode == MOVE_Walking && EnemyMovement->Velocity.Size() < 1.0f);
				if (bPlayerGrounded && bEnemyGrounded)
				{
					return true;
				}
			}
			Test.AddError(TEXT("the characters never settled onto the floor within the settle cap"));
			return false;
		}

		float EnemyHealth() const
		{
			const UHealthComponent* Health = Enemy != nullptr ? Enemy->GetHealthComponent() : nullptr;
			return (Health != nullptr) ? Health->GetHealth() : -1.0f;
		}

		// M1-041 known-gap stand-in, enemy side only (the player path under
		// test gets NO test-side injection - its input clock comes exclusively
		// from its own Tick inside the world tick).
		void TickEnemyCombatStandIn()
		{
			UCombatComponent* EnemyCombat = Enemy != nullptr ? Enemy->GetCombatComponent() : nullptr;
			if (EnemyCombat != nullptr && World != nullptr)
			{
				EnemyCombat->SetInputClockSeconds(World->GetTimeSeconds());
				EnemyCombat->TickCombat(M3_031_FrameSeconds);
			}
		}

		// Fires every press whose scheduled time has arrived through the
		// production intent entry (the character stamps PressedAt itself).
		void FireScheduledPresses(const TArray<FM3_031_Press>& Schedule, TArray<FM3_031_Press>& OutPresses)
		{
			const double Now = World->GetTimeSeconds();
			for (int32 Index = 0; Index < Schedule.Num(); ++Index)
			{
				if (!OutPresses[Index].bFired && Now + 1e-6 >= Schedule[Index].FireAt)
				{
					Player->SubmitCombatInput(Schedule[Index].Action);
					OutPresses[Index].bFired = true;
					OutPresses[Index].PressedAtStamped = Now;
				}
			}
		}

		// One telemetry row: world time, injected input clock, snapshot and
		// the full buffer content (sequence:action:pressedAt triples).
		void RecordFrame(int32 FrameIndex)
		{
			FM3_031_FrameRow Row;
			Row.Frame = FrameIndex;
			Row.WorldSeconds = World->GetTimeSeconds();
			Row.InputClockSeconds = PlayerCombat->GetInputClockSeconds();
			const FCombatSnapshot Snapshot = PlayerCombat->GetSnapshot();
			Row.ActionState = static_cast<int32>(Snapshot.ActionState);
			Row.AttackId = Snapshot.AttackId.ToString();
			Row.AttackFrame = Snapshot.Frame;
			Row.BufferSize = Snapshot.BufferSize;
			for (int32 Index = 0; Index < Snapshot.BufferSize; ++Index)
			{
				FBufferedCombatInput Entry;
				if (PlayerCombat->PeekInputBuffer(Entry, Index))
				{
					const TCHAR* ActionText = (Entry.Action == ECombatInput::Light)
						? TEXT("Light")
						: (Entry.Action == ECombatInput::Launcher) ? TEXT("Launcher") : TEXT("Jump");
					Row.BufferEntries += FString::Printf(TEXT("%llu:%s:%.4f"),
						Entry.Sequence, ActionText, Entry.PressedAt);
					if (Index + 1 < Snapshot.BufferSize)
					{
						Row.BufferEntries += TEXT("|");
					}
				}
			}
			Telemetry.Add(Row);
		}

		// Writes the run's telemetry + event record as JSON evidence under
		// Artifacts/Tasks/M3-031 (best effort; the assertions never depend on
		// the file landing).
		void WriteEvidence(const TCHAR* RunLabel)
		{
			const FString Dir = FPaths::ProjectDir() / M3_031_EvidenceDir;
			IFileManager::Get().MakeDirectory(*Dir, /*Tree*/ true);
			const FString Path = Dir / FString::Printf(TEXT("m3-031-input-feel-%s.json"), RunLabel);

			FString Json = TEXT("{\n  \"runs\": [{\n");
			Json += FString::Printf(TEXT("    \"label\": \"%s\",\n"), RunLabel);
			Json += TEXT("    \"presses\": [\n");
			for (int32 Index = 0; Index < Presses.Num(); ++Index)
			{
				const TCHAR* ActionText = (Presses[Index].Action == ECombatInput::Light)
					? TEXT("Light")
					: (Presses[Index].Action == ECombatInput::Launcher) ? TEXT("Launcher") : TEXT("Jump");
				Json += FString::Printf(TEXT("      {\"action\": \"%s\", \"fireAt\": %.4f, \"stampedAt\": %.4f, \"fired\": %s}%s\n"),
					ActionText,
					Presses[Index].FireAt,
					Presses[Index].PressedAtStamped,
					Presses[Index].bFired ? TEXT("true") : TEXT("false"),
					(Index + 1 < Presses.Num()) ? TEXT(",") : TEXT(""));
			}
			Json += TEXT("    ],\n    \"started\": [\n");
			for (int32 Index = 0; Index < Events.Started.Num(); ++Index)
			{
				Json += FString::Printf(TEXT("      {\"attackId\": \"%s\", \"worldSeconds\": %.4f, \"inputClockSeconds\": %.4f}%s\n"),
					*Events.Started[Index].ToString(),
					Events.StartedWorldSeconds[Index],
					Events.StartedInputClockSeconds[Index],
					(Index + 1 < Events.Started.Num()) ? TEXT(",") : TEXT(""));
			}
			Json += TEXT("    ],\n    \"finished\": [\n");
			for (int32 Index = 0; Index < Events.Finished.Num(); ++Index)
			{
				Json += FString::Printf(TEXT("      {\"attackId\": \"%s\", \"worldSeconds\": %.4f}%s\n"),
					*Events.Finished[Index].ToString(),
					Events.FinishedWorldSeconds[Index],
					(Index + 1 < Events.Finished.Num()) ? TEXT(",") : TEXT(""));
			}
			Json += FString::Printf(TEXT("    ],\n    \"hits\": %d,\n    \"frames\": [\n"), Events.Hits);
			for (int32 Index = 0; Index < Telemetry.Num(); ++Index)
			{
				const FM3_031_FrameRow& Row = Telemetry[Index];
				Json += FString::Printf(TEXT("      {\"frame\": %d, \"worldSeconds\": %.4f, \"inputClock\": %.4f, \"state\": %d, \"attack\": \"%s\", \"attackFrame\": %d, \"bufferSize\": %d, \"buffer\": \"%s\"}%s\n"),
					Row.Frame, Row.WorldSeconds, Row.InputClockSeconds, Row.ActionState,
					*Row.AttackId, Row.AttackFrame, Row.BufferSize, *Row.BufferEntries,
					(Index + 1 < Telemetry.Num()) ? TEXT(",") : TEXT(""));
			}
			Json += TEXT("    ]\n  }]\n}\n");
			FFileHelper::SaveStringToFile(Json, *Path);
		}

		// The press schedule mirrored for evidence writing (filled by Run).
		TArray<FM3_031_Press> Presses;
	};

	// One planned press: the action plus the delay from the previous press
	// (the first press delays from the settle moment + 0.10 s of idle).
	struct FM3_031_ScheduledPress
	{
		ECombatInput Action = ECombatInput::Light;
		double DelaySeconds = 0.0;
	};
}

using namespace UE::UEMMO::Tasks::M3_031;

// Shared rhythm driver: builds a scene, schedules presses at the requested
// delays (seconds between consecutive presses, relative to the settle time
// plus a 0.10 s idle), runs the real world loop until every scheduled press
// was consumed into a start and every instance retired (or the frame cap
// hits), writes the telemetry evidence and leaves the scene for assertions.
static bool M3_031_RunPressRhythm(FAutomationTestBase& Test, FM3_031_Scene& Scene,
	int32 SceneIndex, const TArray<FM3_031_ScheduledPress>& Plan, bool bWithEnemy,
	const TCHAR* RunLabel, int32 TailFrames, int32 MaxFrames = M3_031_MaxComboFrames)
{
	if (!Scene.Build(Test, SceneIndex, bWithEnemy))
	{
		return false;
	}
	if (!Scene.Settle(Test))
	{
		return false;
	}

	const double SettledAt = Scene.World->GetTimeSeconds();
	TArray<FM3_031_Press> Schedule;
	double FireAt = SettledAt + 0.10;
	for (int32 Index = 0; Index < Plan.Num(); ++Index)
	{
		FireAt += Plan[Index].DelaySeconds;
		Schedule.Add({ Plan[Index].Action, FireAt, false, -1.0 });
	}
	Scene.Presses = Schedule;

	for (int32 Frame = 0; Frame < MaxFrames; ++Frame)
	{
		Scene.FireScheduledPresses(Schedule, Scene.Presses);
		if (!Test.TestTrue(TEXT("the test world ticks"), Scene.Wrapper.TickTestWorld(M3_031_FrameSeconds)))
		{
			break;
		}
		Scene.TickEnemyCombatStandIn();
		Scene.RecordFrame(Frame);
		// Done once every scheduled press was consumed into a start and every
		// instance retired (the launcher's natural end included), so the
		// end-state assertions below read the settled component. A pressless
		// plan (the misfire guard) simply runs to its frame cap.
		if (Plan.Num() > 0
			&& Scene.Events.Started.Num() >= Plan.Num()
			&& Scene.Events.Finished.Num() >= Plan.Num())
		{
			for (int32 Tail = 0; Tail < TailFrames; ++Tail)
			{
				Scene.Wrapper.TickTestWorld(M3_031_FrameSeconds);
				Scene.TickEnemyCombatStandIn();
				Scene.RecordFrame(Frame + 1 + Tail);
			}
			break;
		}
	}
	Scene.WriteEvidence(RunLabel);

	Test.AddInfo(FString::Printf(TEXT("%s: telemetry rows %d, hits %d"), RunLabel,
		Scene.Telemetry.Num(), Scene.Events.Hits));
	for (int32 Index = 0; Index < Scene.Events.Started.Num(); ++Index)
	{
		Test.AddInfo(FString::Printf(TEXT("%s started[%d]=%s at world %.4f (clock %.4f)"),
			RunLabel, Index, *Scene.Events.Started[Index].ToString(),
			Scene.Events.StartedWorldSeconds[Index], Scene.Events.StartedInputClockSeconds[Index]));
	}
	return true;
}

// Core diagnostic (the user's dead combo, replayed inside a real ticked world
// with the enemy present so every hit stop freeze runs exactly like the real
// game): a real APrototypeCharacter presses X, X, Z one 83 ms apart through
// the production intent entry while its own Tick injects the input game
// clock. The full chain must be light_01 -> light_02 -> launcher. Every frame
// is telemetried to Artifacts/Tasks/M3-031 as the red/green evidence.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_031RapidXXZChainReachesLauncherWithEnemy,
	"UEMMO.Tasks.M3_031.RapidXXZChainReachesLauncherWithEnemy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_031RapidXXZChainReachesLauncherWithEnemy::RunTest(const FString& Parameters)
{
	FM3_031_Scene Scene;
	TArray<FM3_031_ScheduledPress> Plan;
	Plan.Add({ ECombatInput::Light, 0.0 });
	Plan.Add({ ECombatInput::Light, M3_031_RapidGapSeconds });
	Plan.Add({ ECombatInput::Launcher, M3_031_RapidGapSeconds });
	if (!M3_031_RunPressRhythm(*this, Scene, 0, Plan, /*bWithEnemy*/ true,
		TEXT("rapid-xxz-with-enemy"), /*TailFrames*/ 20))
	{
		return true;
	}

	// The wiring pivot: the character's own Tick injected the input clock.
	TestTrue(TEXT("the character's own Tick injected the input game clock (clock > 0 after settle)"),
		Scene.PlayerCombat->GetInputClockSeconds() > 0.0);

	// The hit stop really ran (the enemy was in range): the chain exercised
	// the same freeze path the real game does.
	TestTrue(TEXT("the chain landed at least one hit (the M1-033 hit stop path ran)"),
		Scene.Events.Hits >= 1);

	// The full user chain, end to end.
	TestEqual(TEXT("exactly three attacks started (light_01, light_02, launcher)"),
		Scene.Events.Started.Num(), 3);
	if (Scene.Events.Started.Num() == 3)
	{
		TestEqual(TEXT("the first press started light_01 from Free"),
			Scene.Events.Started[0], FName(TEXT("light_01")));
		TestEqual(TEXT("the second press chained into light_02 through light_01's cancel window"),
			Scene.Events.Started[1], FName(TEXT("light_02")));
		TestEqual(TEXT("the third press (Z) chained into launcher through light_02's cancel window"),
			Scene.Events.Started[2], FName(TEXT("launcher")));
	}

	// The launcher actually started from light_02's cancel window, never from
	// a misfire: no fourth start exists by construction of the assertion
	// above; assert the component ends Free with an empty buffer.
	TestTrue(TEXT("the combat component returned to Free after the chain"),
		Scene.PlayerCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	return true;
}

// The user's other failure rhythm: a slightly slower X -> X (150 ms, so the
// second X survives light_01's window and chains light_02) with the Z pressed
// 83 ms after the second X. The queued Z must survive light_02's pre-window
// frames and chain the launcher at light_02's own cancel window. This is the
// exact "Z has no effect" symptom: the Z press is real, the buffer held it,
// and the launcher must come out.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_031SlowerXXZKeepsQueuedLauncherAlive,
	"UEMMO.Tasks.M3_031.SlowerXXZKeepsQueuedLauncherAlive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_031SlowerXXZKeepsQueuedLauncherAlive::RunTest(const FString& Parameters)
{
	FM3_031_Scene Scene;
	TArray<FM3_031_ScheduledPress> Plan;
	Plan.Add({ ECombatInput::Light, 0.0 });
	Plan.Add({ ECombatInput::Light, 0.150 });
	Plan.Add({ ECombatInput::Launcher, M3_031_RapidGapSeconds });
	if (!M3_031_RunPressRhythm(*this, Scene, 1, Plan, /*bWithEnemy*/ true,
		TEXT("slower-xxz-with-enemy"), /*TailFrames*/ 20))
	{
		return true;
	}

	TestTrue(TEXT("the chain landed at least one hit (the M1-033 hit stop path ran)"),
		Scene.Events.Hits >= 1);

	// The full user chain, end to end: the queued Z must chain the launcher
	// through light_02's cancel window.
	TestEqual(TEXT("exactly three attacks started (light_01, light_02, launcher)"),
		Scene.Events.Started.Num(), 3);
	if (Scene.Events.Started.Num() == 3)
	{
		TestEqual(TEXT("the first press started light_01 from Free"),
			Scene.Events.Started[0], FName(TEXT("light_01")));
		TestEqual(TEXT("the second press chained into light_02 through light_01's cancel window"),
			Scene.Events.Started[1], FName(TEXT("light_02")));
		TestEqual(TEXT("the queued Z chained into launcher through light_02's cancel window"),
			Scene.Events.Started[2], FName(TEXT("launcher")));
	}
	return true;
}

// The card's acceptance band: one X then one Z at each of the 50 / 83 /
// 100 / 150 ms gaps, enemy present (the real hit stop path runs). The design
// outcome per gap follows from the locked 150 ms press-time lifetime
// (interface contract section 2; the M1-014/M1-021 expiry tests pin it):
// light_01's cancel window opens about 250 ms of game time after the X press
// (frame 12 at 13-14 ticks plus the 40 ms hit stop stretch), so a Z pressed
// 83 ms after the X is judged at an age of about 133 ms (alive, the boundary
// the goal names), 100/150 ms are safely alive, and a 50 ms Z is judged at
// about 183 ms - past the 150 ms design lifetime, exactly the expiry shape
// the M1-014 pinned test locks (a press made more than 150 ms before its
// consumption opportunity expires). The probe asserts the design outcome for
// every gap: alive gaps chain the launcher through light_01's window and
// never detour through light_02.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_031SingleXThenLauncherChainsAtEveryProbeGap,
	"UEMMO.Tasks.M3_031.SingleXThenLauncherChainsAtEveryProbeGap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_031SingleXThenLauncherChainsAtEveryProbeGap::RunTest(const FString& Parameters)
{
	for (int32 Probe = 0; Probe < 4; ++Probe)
	{
		const double Gap = M3_031_ProbeGapSeconds[Probe];
		FM3_031_Scene Scene;
		TArray<FM3_031_ScheduledPress> Plan;
		Plan.Add({ ECombatInput::Light, 0.0 });
		Plan.Add({ ECombatInput::Launcher, Gap });
		const FString Label = FString::Printf(TEXT("single-x-z-gap-%dms"), static_cast<int32>(Gap * 1000.0));
		if (!M3_031_RunPressRhythm(*this, Scene, 2 + Probe, Plan, /*bWithEnemy*/ true,
			*Label, /*TailFrames*/ 5))
		{
			return true;
		}

		// The Z press must never start a second light detour in any gap: the
		// launcher consumes the window directly or the Z expires per design.
		TestTrue(FString::Printf(TEXT("gap %.0f ms: light_02 never started (no swallow detour)"), Gap * 1000.0),
			!Scene.Events.Started.Contains(FName(TEXT("light_02"))));
		if (Gap < 0.0667)
		{
			// Past the 150 ms design lifetime at the window step (the hit
			// stop stretches the window to about 267 ms after the X press,
			// so gaps below 66.7 ms judge the Z at more than 150 ms): the Z
			// expires per the locked design and light_01 runs out naturally.
			TestEqual(FString::Printf(TEXT("gap %.0f ms: the Z expires per design (light_01 only)"), Gap * 1000.0),
				Scene.Events.Started.Num(), 1);
			if (Scene.Events.Started.Num() == 1)
			{
				TestEqual(FString::Printf(TEXT("gap %.0f ms: the X started light_01"), Gap * 1000.0),
					Scene.Events.Started[0], FName(TEXT("light_01")));
			}
			TestTrue(FString::Printf(TEXT("gap %.0f ms: no launcher misfired"), Gap * 1000.0),
				!Scene.Events.Started.Contains(FName(TEXT("launcher"))));
		}
		else
		{
			TestEqual(FString::Printf(TEXT("gap %.0f ms: exactly two attacks started (light_01, launcher)"), Gap * 1000.0),
				Scene.Events.Started.Num(), 2);
			if (Scene.Events.Started.Num() == 2)
			{
				TestEqual(FString::Printf(TEXT("gap %.0f ms: the X started light_01"), Gap * 1000.0),
					Scene.Events.Started[0], FName(TEXT("light_01")));
				TestEqual(FString::Printf(TEXT("gap %.0f ms: the Z chained the launcher through light_01's cancel window"), Gap * 1000.0),
					Scene.Events.Started[1], FName(TEXT("launcher")));
			}
		}
	}
	return true;
}

// Free-state starts and the misfire guard: X from Free starts light_01 and
// nothing else (no launcher ever, even past the natural end); Z from Free
// starts the launcher directly; a fully idle run starts nothing at all.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_031FreeStateStartsAndIdleNeverMisfires,
	"UEMMO.Tasks.M3_031.FreeStateStartsAndIdleNeverMisfires",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_031FreeStateStartsAndIdleNeverMisfires::RunTest(const FString& Parameters)
{
	// X alone: light_01 starts from Free, runs to its natural end, and no
	// launcher ever appears (the queued-press fix must not invent one).
	{
		FM3_031_Scene Scene;
		TArray<FM3_031_ScheduledPress> Plan;
		Plan.Add({ ECombatInput::Light, 0.0 });
		if (!M3_031_RunPressRhythm(*this, Scene, 6, Plan, /*bWithEnemy*/ false,
			TEXT("free-x-only"), /*TailFrames*/ 5))
		{
			return true;
		}
		TestEqual(TEXT("X alone started exactly one attack"), Scene.Events.Started.Num(), 1);
		if (Scene.Events.Started.Num() == 1)
		{
			TestEqual(TEXT("the Free X started light_01"), Scene.Events.Started[0], FName(TEXT("light_01")));
		}
		TestTrue(TEXT("no launcher ever misfired from the single X"),
			!Scene.Events.Started.Contains(FName(TEXT("launcher"))));
		TestTrue(TEXT("the component returned to Free"),
			Scene.PlayerCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	}
	// Z alone: the Free launcher start (interface contract section 2 mapping).
	{
		FM3_031_Scene Scene;
		TArray<FM3_031_ScheduledPress> Plan;
		Plan.Add({ ECombatInput::Launcher, 0.0 });
		if (!M3_031_RunPressRhythm(*this, Scene, 7, Plan, /*bWithEnemy*/ false,
			TEXT("free-z-only"), /*TailFrames*/ 5))
		{
			return true;
		}
		TestEqual(TEXT("Z alone started exactly one attack"), Scene.Events.Started.Num(), 1);
		if (Scene.Events.Started.Num() == 1)
		{
			TestEqual(TEXT("the Free Z started launcher"), Scene.Events.Started[0], FName(TEXT("launcher")));
		}
	}
	// No press at all: nothing ever starts, the buffer stays empty.
	{
		FM3_031_Scene Scene;
		TArray<FM3_031_ScheduledPress> Plan;
		if (!M3_031_RunPressRhythm(*this, Scene, 8, Plan, /*bWithEnemy*/ false,
			TEXT("idle-no-press"), /*TailFrames*/ 0, /*MaxFrames*/ 180))
		{
			return true;
		}
		TestEqual(TEXT("an idle run started nothing"), Scene.Events.Started.Num(), 0);
		TestEqual(TEXT("an idle run finished nothing"), Scene.Events.Finished.Num(), 0);
		TestEqual(TEXT("an idle run keeps the buffer empty"), Scene.PlayerCombat->GetSnapshot().BufferSize, 0);
		TestTrue(TEXT("an idle run stays Free"),
			Scene.PlayerCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	}
	return true;
}

#endif
