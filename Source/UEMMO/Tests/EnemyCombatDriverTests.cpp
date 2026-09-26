// M1-043: enemy-side combat driver in-loop tests. Game code only drove the
// player's combat component (APrototypeCharacter::Tick, M1-041); the training
// enemy's own component had no per-frame driver, so the M1-033 victim-side
// hit stop once opened never ended (the victim stayed in MOVE_None forever),
// the M1-020 hit stun never advanced and a launcher could never float the
// frozen target. These tests lock the GAME-side enemy wiring: a real
// ATrainingEnemy runs inside a fully ticked temp world (the engine's own
// FTestWorldWrapper precedent: world context + InitializeActorsForPlay +
// BeginPlay + UWorld::Tick(LEVELTICK_All) - real actor ticks, real character
// movement, real physics) next to a real player attacker. The ENEMY path
// under test gets ZERO test-side injection: its input clock comes only from
// its own Tick inside the world tick, exactly like the player side of M1-041.
// The test only presses keys through the production intent entry
// SubmitCombatInput and never calls SetInputClockSeconds / TickCombat /
// SetFacingProvider / RequestHitStop on the enemy component.
#include "Misc/AutomationTest.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/TrainingEnemy.h"
#include "../PrototypeCharacter.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_043
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M1_043_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with arena
	// content (the M1-022/M1-027 placement convention; a different corner
	// than the M1-041 base so both suites stay isolated). X horizontal, Y
	// depth, Z height; the floor top sits exactly at the base Z.
	const FVector M1_043_SceneBase(44000.0, 45000.0, 600.0);

	// All three ground attacks (light_01, light_02, launcher) share the hit
	// box offset X=95 and half extent X=85 (M1-009 assets), so the box covers
	// base.X+10..base.X+180 for facing +1: an enemy feet anchor at +95 stays
	// reachable through every stage.
	const float M1_043_EnemyFeetOffsetX = 95.0f;

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M1_043_FloorHalfThickness = 100.0f;
	const float M1_043_FloorHalfExtentXY = 4000.0f;

	// Spawn heights above the floor top: the characters drop a short distance
	// through real no-controller physics and settle onto the floor.
	const float M1_043_PlayerSpawnHeight = 120.0f;
	const float M1_043_EnemySpawnHeight = 90.0f;

	// Frame caps: the settle phase waits for the ground contact of both
	// characters; M1_043_MaxRunFrames covers the press schedule, the hit, the
	// 40 ms stop and the launcher float; M1_043_MaxFlightFrames additionally
	// covers the whole 700 cm/s float flight (rise plus fall, roughly 1.5 s)
	// and the 0.70 s landing recovery.
	constexpr int32 M1_043_MaxSettleFrames = 300;
	constexpr int32 M1_043_MaxRunFrames = 600;
	constexpr int32 M1_043_MaxFlightFrames = 400;

	// The 40 ms hit stop lasts 2.4 frames at 1/60; the cap gives it roughly
	// 0.3 s of real frames to end by itself (red baseline: it never ends).
	constexpr int32 M1_043_MaxHitStopEndFrames = 20;

	// Recorded combat events (the assertions read this struct only).
	struct FM1_043_Events
	{
		TArray<FName> Started;
		int32 Hits = 0;
	};

	// One scheduled key press: fired exactly once when the world clock
	// reaches FireAt (the press goes through the production intent entry, so
	// PressedAt is read from the world time by the character itself).
	struct FM1_043_Press
	{
		ECombatInput Action = ECombatInput::Light;
		double FireAt = 0.0;
		bool bFired = false;
	};

	// World-static blocking floor box the characters stand on (temp worlds
	// ship no geometry; the walking/landing physics needs one blocker). The
	// standard BlockAll profile (QueryAndPhysics, WorldStatic, block all) is
	// the same collision a real level floor carries.
	static AActor* M1_043_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base - FVector(0.0, 0.0, M1_043_FloorHalfThickness), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M1_043_Floor"));
		Actor->SetRootComponent(Floor);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->SetBoxExtent(FVector(M1_043_FloorHalfExtentXY, M1_043_FloorHalfExtentXY, M1_043_FloorHalfThickness));
		Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Floor->RegisterComponent();
		// The root was assigned after the spawn transform was applied, so the
		// world location is (re-)applied explicitly (the M1-022 pattern).
		Floor->SetWorldLocation(Base - FVector(0.0, 0.0, M1_043_FloorHalfThickness));
		return Actor;
	}

	// Spawns the real prototype pawn FeetHeight above the floor top and
	// enables no-controller physics so its movement component initializes and
	// integrates like the possessed game pawn does (the two-step activation
	// the M1-041 scene pinned: activate the component, apply the default
	// movement mode).
	static APrototypeCharacter* M1_043_SpawnPlayer(UWorld& World, const FRotator& Facing)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			M1_043_SceneBase + FVector(0.0, 0.0, M1_043_PlayerSpawnHeight),
			Facing, Params);
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

	// Spawns the real training enemy FeetHeight above the floor top, offset
	// horizontally to the requested enemy feet anchor. The production dummy
	// already carries bRunPhysicsWithNoController in its constructor, so the
	// component initializes with running physics at spawn (no two-step).
	static ATrainingEnemy* M1_043_SpawnEnemy(UWorld& World, float FeetOffsetX)
	{
		FActorSpawnParameters Params;
		ATrainingEnemy* Enemy = World.SpawnActor<ATrainingEnemy>(
			ATrainingEnemy::StaticClass(),
			M1_043_SceneBase + FVector(FeetOffsetX, 0.0, M1_043_EnemySpawnHeight),
			FRotator::ZeroRotator, Params);
		if (Enemy == nullptr)
		{
			return nullptr;
		}
		if (UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement())
		{
			// The production dummy carries this flag in its constructor; the
			// explicit set only documents the same intent for this scene.
			Movement->bRunPhysicsWithNoController = true;
		}
		return Enemy;
	}

	// One full scene: the wrapper owns the manually ticked temp world; the
	// tests build it, press keys and read the results. Unlike the M1-041
	// scene there is NO enemy combat stand-in anywhere in this file.
	struct FM1_043_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		APrototypeCharacter* Player = nullptr;
		UCombatComponent* PlayerCombat = nullptr;
		ATrainingEnemy* Enemy = nullptr;
		UCombatComponent* EnemyCombat = nullptr;
		FM1_043_Events Events;

		// Builds world + floor + player + enemy and binds the event record.
		// Returns false after reporting the problem so the caller can bail.
		bool Build(FAutomationTestBase& Test, const FRotator& PlayerFacing, float EnemyFeetOffsetX)
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
			AActor* Floor = M1_043_SpawnFloor(*World, M1_043_SceneBase);
			if (Floor == nullptr)
			{
				Test.AddError(TEXT("the floor failed to spawn"));
				return false;
			}
			// A hand-built world never runs the map-load pass that normally
			// builds the scene collision tree (FlushLevelStreaming ->
			// EnsureCollisionTreeIsBuilt); until it is built every scene query
			// in this world misses. Build it once before the first probe/tick.
			World->EnsureCollisionTreeIsBuilt();
			// Precondition probe: the walking physics needs this blocker to
			// stop a falling pawn capsule (the exact query shape the falling
			// physics sweeps every frame). Reported before anything else so a
			// missing floor cannot masquerade as a wiring failure.
			FHitResult ProbeHit(NoInit);
			ProbeHit.Init();
			const FCollisionShape ProbeShape = FCollisionShape::MakeCapsule(36.0f, 96.0f);
			const bool bFloorBlocks = World->SweepSingleByChannel(
				ProbeHit,
				M1_043_SceneBase + FVector(0.0, 0.0, M1_043_PlayerSpawnHeight),
				M1_043_SceneBase - FVector(0.0, 0.0, 2.0 * M1_043_FloorHalfThickness),
				FQuat::Identity,
				ECC_Pawn,
				ProbeShape,
				FCollisionQueryParams(TEXT("M1_043_FloorProbe"), false));
			UBoxComponent* FloorBox = Floor->FindComponentByClass<UBoxComponent>();
			if (!Test.TestTrue(FString::Printf(TEXT(
				"the floor blocks a pawn capsule sweep (hit=%s floorRegistered=%d floorBody=%d)"),
				ProbeHit.Component.IsValid() ? *ProbeHit.Component->GetName() : TEXT("none"),
				FloorBox != nullptr && FloorBox->IsRegistered() ? 1 : 0,
				FloorBox != nullptr && FloorBox->GetBodyInstance() != nullptr ? 1 : 0),
				bFloorBlocks))
			{
				FBodyInstance* Body = FloorBox != nullptr ? FloorBox->GetBodyInstance() : nullptr;
				Test.AddError(FString::Printf(TEXT(
					"floor probe details (physicsScene=%d bodyInScene=%d bodyQueryEnabled=%d bodyProfile=%s)"),
					World->GetPhysicsScene() != nullptr ? 1 : 0,
					Body != nullptr && Body->GetPhysicsActorHandle() != nullptr ? 1 : 0,
					Body != nullptr && (Body->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics || Body->GetCollisionEnabled() == ECollisionEnabled::QueryOnly) ? 1 : 0,
					FloorBox != nullptr ? *FloorBox->GetCollisionProfileName().ToString() : TEXT("none")));
				return false;
			}
			Player = M1_043_SpawnPlayer(*World, PlayerFacing);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			PlayerCombat = Player->GetCombat();
			if (!Test.TestTrue(TEXT("the player carries a combat component"), PlayerCombat != nullptr))
			{
				return false;
			}
			Enemy = M1_043_SpawnEnemy(*World, EnemyFeetOffsetX);
			if (!Test.TestNotNull(TEXT("the training enemy spawns"), Enemy))
			{
				return false;
			}
			EnemyCombat = Enemy->GetCombatComponent();
			if (!Test.TestTrue(TEXT("the enemy carries its own combat component"), EnemyCombat != nullptr))
			{
				return false;
			}

			PlayerCombat->OnStarted.AddLambda([this](FName AttackId, uint64 /*InstanceId*/)
			{
				Events.Started.Add(AttackId);
			});
			PlayerCombat->OnHitConfirmed.AddLambda([this](const FCombatHit& /*Hit*/)
			{
				++Events.Hits;
			});
			return true;
		}

		// Ticks the world until both characters settled onto the floor (real
		// walking physics). No combat stand-in here: the enemy's combat state
		// is irrelevant for settling and the tested path must stay untouched.
		// Returns false after reporting when the ground contact never
		// happened within the settle cap.
		bool Settle(FAutomationTestBase& Test)
		{
			UCharacterMovementComponent* PlayerMovement = Player->GetCharacterMovement();
			UCharacterMovementComponent* EnemyMovement = Enemy->GetCharacterMovement();
			for (int32 Frame = 0; Frame < M1_043_MaxSettleFrames; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M1_043_FrameSeconds)))
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
			Test.AddError(FString::Printf(TEXT(
				"the characters never settled onto the floor within the settle cap "
				"(worldSeconds=%.3f playerMode=%d playerVel=%.1f playerZ=%.1f "
				"enemyMode=%d enemyVel=%.1f enemyZ=%.1f enemyBegun=%d)"),
				World->GetTimeSeconds(),
				PlayerMovement ? static_cast<int32>(PlayerMovement->MovementMode.GetValue()) : -1,
				PlayerMovement ? PlayerMovement->Velocity.Size() : -1.0f,
				Player->GetActorLocation().Z,
				EnemyMovement ? static_cast<int32>(EnemyMovement->MovementMode.GetValue()) : -1,
				EnemyMovement ? EnemyMovement->Velocity.Size() : -1.0f,
				Enemy->GetActorLocation().Z,
				Enemy->HasActorBegunPlay() ? 1 : 0));
			return false;
		}

		float EnemyHealth() const
		{
			const UHealthComponent* Health = Enemy->GetHealthComponent();
			return (Health != nullptr) ? Health->GetHealth() : -1.0f;
		}
	};
}

using namespace UE::UEMMO::Tasks::M1_043;

// Acceptance 1 + the relaunch half of the freeze story: a real light hit
// opens the 40 ms victim-side hit stop (MOVE_None freeze) and the enemy's
// OWN tick - its per-frame SetInputClockSeconds + TickCombat - ends it, so
// the movement mode restores and a later launcher still floats the target.
// On the pre-M1-043 wiring the stop never ends: the enemy stays frozen in
// MOVE_None forever and the launcher can never float it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_043EnemyLightHitStopEndsMovementRestoresAndRelaunchable,
	"UEMMO.Tasks.M1_043.EnemyLightHitStopEndsMovementRestoresAndRelaunchable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_043EnemyLightHitStopEndsMovementRestoresAndRelaunchable::RunTest(const FString& Parameters)
{
	FM1_043_Scene Scene;
	if (!Scene.Build(*this, /*PlayerFacing*/ FRotator::ZeroRotator, M1_043_EnemyFeetOffsetX))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}

	// The wiring pivot: the enemy's own Tick must have injected the input
	// game clock by now (a bare component reads 0.0 forever without it).
	TestTrue(TEXT("the enemy's own Tick injected the input game clock (clock > 0 after settle)"),
		Scene.EnemyCombat->GetInputClockSeconds() > 0.0);

	// The light press lands the first hit; the launcher press comes after the
	// light's 0.22 s stun and the player's light_01 fully finished, so the
	// relaunch happens through a Free attacker and a recovered victim.
	const double SettledAt = Scene.World->GetTimeSeconds();
	TArray<FM1_043_Press> Presses;
	Presses.Add({ ECombatInput::Light, SettledAt + 0.10, false });
	Presses.Add({ ECombatInput::Launcher, SettledAt + 0.85, false });

	UCharacterMovementComponent* EnemyMovement = Scene.Enemy->GetCharacterMovement();

	bool bFrozenObserved = false;
	bool bStopEnded = false;
	int32 StopEndFrame = -1;
	bool bModeRestored = false;
	bool bFloating = false;
	int32 Hit1Frame = -1;

	for (int32 Frame = 0; Frame < M1_043_MaxRunFrames; ++Frame)
	{
		const double Now = Scene.World->GetTimeSeconds();
		for (FM1_043_Press& Press : Presses)
		{
			if (!Press.bFired && Now + 1e-6 >= Press.FireAt)
			{
				// The production intent entry (the exact target of the real
				// X/Z input bindings); the character reads PressedAt itself.
				Scene.Player->SubmitCombatInput(Press.Action);
				Press.bFired = true;
			}
		}
		if (!TestTrue(TEXT("the test world ticks"), Scene.Wrapper.TickTestWorld(M1_043_FrameSeconds)))
		{
			break;
		}

		if (Scene.Events.Hits >= 1 && Hit1Frame < 0)
		{
			Hit1Frame = Frame;
			// The victim-side freeze engaged: hit stop active and the enemy
			// movement integrates nothing (MOVE_None).
			bFrozenObserved = Scene.EnemyCombat->IsHitStopActive()
				&& EnemyMovement != nullptr
				&& EnemyMovement->MovementMode == MOVE_None;
		}
		if (Hit1Frame >= 0 && !bStopEnded && !Scene.EnemyCombat->IsHitStopActive())
		{
			bStopEnded = true;
			StopEndFrame = Frame;
		}
		if (bStopEnded && !bModeRestored && EnemyMovement != nullptr
			&& EnemyMovement->MovementMode == MOVE_Walking)
		{
			bModeRestored = true;
		}
		if (Scene.Events.Hits >= 2 && !bFloating && EnemyMovement != nullptr
			&& EnemyMovement->Velocity.Z > 1.0f)
		{
			bFloating = true;
		}
		if (bStopEnded && bModeRestored && bFloating)
		{
			break;
		}
	}

	// The freeze is real (the M1-033 victim path actually engaged).
	TestTrue(TEXT("the first light hit opened the victim-side hit stop and froze the enemy movement (MOVE_None)"),
		bFrozenObserved);
	// The enemy's own tick ended the 40 ms stop: the pivot assertion - the
	// unwired baseline never leaves the freeze (red).
	TestTrue(FString::Printf(TEXT(
		"the enemy's own Tick ended the 40 ms hit stop within %d frames (ended=%d at frame %d after the hit frame %d)"),
		M1_043_MaxHitStopEndFrames, bStopEnded ? 1 : 0, StopEndFrame, Hit1Frame),
		bStopEnded && StopEndFrame - Hit1Frame <= M1_043_MaxHitStopEndFrames);
	// The movement mode came back (the restore puts the saved mode back).
	TestTrue(TEXT("the enemy movement mode was restored after the hit stop ended (MOVE_Walking again)"),
		bModeRestored);
	// And the once-frozen enemy is floatable again through a later launcher.
	TestTrue(TEXT("the launcher hit after the frozen phase floated the enemy again (vertical speed > 0)"),
		bFloating);
	return true;
}

// Acceptance 2 (the crispest red): a launcher as the very first hit. The
// launch impulse arrives while the 40 ms victim freeze holds (the launch is
// folded into the saved velocity and released by the restore), so the enemy
// rises only if its own tick ends the stop. On the unwired baseline the
// freeze never lifts, the launch stays swallowed and the target never rises.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_043EnemyLauncherFloatsTargetThroughItsOwnTick,
	"UEMMO.Tasks.M1_043.EnemyLauncherFloatsTargetThroughItsOwnTick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_043EnemyLauncherFloatsTargetThroughItsOwnTick::RunTest(const FString& Parameters)
{
	FM1_043_Scene Scene;
	if (!Scene.Build(*this, /*PlayerFacing*/ FRotator::ZeroRotator, M1_043_EnemyFeetOffsetX))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	TestTrue(TEXT("the enemy's own Tick injected the input game clock (clock > 0 after settle)"),
		Scene.EnemyCombat->GetInputClockSeconds() > 0.0);

	const double SettledAt = Scene.World->GetTimeSeconds();
	TArray<FM1_043_Press> Presses;
	Presses.Add({ ECombatInput::Launcher, SettledAt + 0.10, false });

	UCharacterMovementComponent* EnemyMovement = Scene.Enemy->GetCharacterMovement();

	bool bFrozenObserved = false;
	bool bStopEnded = false;
	int32 StopEndFrame = -1;
	bool bFloating = false;
	int32 Hit1Frame = -1;

	for (int32 Frame = 0; Frame < M1_043_MaxRunFrames; ++Frame)
	{
		const double Now = Scene.World->GetTimeSeconds();
		for (FM1_043_Press& Press : Presses)
		{
			if (!Press.bFired && Now + 1e-6 >= Press.FireAt)
			{
				Scene.Player->SubmitCombatInput(Press.Action);
				Press.bFired = true;
			}
		}
		if (!TestTrue(TEXT("the test world ticks"), Scene.Wrapper.TickTestWorld(M1_043_FrameSeconds)))
		{
			break;
		}

		if (Scene.Events.Hits >= 1 && Hit1Frame < 0)
		{
			Hit1Frame = Frame;
			bFrozenObserved = Scene.EnemyCombat->IsHitStopActive()
				&& EnemyMovement != nullptr
				&& EnemyMovement->MovementMode == MOVE_None;
		}
		if (Hit1Frame >= 0 && !bStopEnded && !Scene.EnemyCombat->IsHitStopActive())
		{
			bStopEnded = true;
			StopEndFrame = Frame;
		}
		if (Scene.Events.Hits >= 1 && !bFloating && EnemyMovement != nullptr
			&& EnemyMovement->Velocity.Z > 1.0f)
		{
			bFloating = true;
		}
		if (bStopEnded && bFloating)
		{
			break;
		}
	}

	TestTrue(TEXT("the launcher hit opened the victim-side hit stop and froze the enemy movement (MOVE_None)"),
		bFrozenObserved);
	TestTrue(FString::Printf(TEXT(
		"the enemy's own Tick ended the 40 ms hit stop within %d frames (ended=%d at frame %d after the hit frame %d)"),
		M1_043_MaxHitStopEndFrames, bStopEnded ? 1 : 0, StopEndFrame, Hit1Frame),
		bStopEnded && StopEndFrame - Hit1Frame <= M1_043_MaxHitStopEndFrames);
	// The launch was no longer swallowed by a permanent freeze: the target
	// rose through the real movement integration (M1-022 launch speed).
	TestTrue(TEXT("the launcher hit floated the target through the enemy's own tick (vertical speed > 0)"),
		bFloating);
	return true;
}

// Acceptance 3: the 0.22 s light hit stun advances on the enemy's own
// injected input clock and returns the component to Free. The stun deadline
// is fixed at hit time on the injected clock; the 40 ms stop pins the clock
// (no stun progress inside it), so the measured clock span from the stop end
// to the Free transition must be the remaining stun fraction of 0.22 s -
// clock-driven progress, not frame-count luck.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_043EnemyHitStunAdvancesOnItsInjectedClock,
	"UEMMO.Tasks.M1_043.EnemyHitStunAdvancesOnItsInjectedClock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_043EnemyHitStunAdvancesOnItsInjectedClock::RunTest(const FString& Parameters)
{
	FM1_043_Scene Scene;
	if (!Scene.Build(*this, /*PlayerFacing*/ FRotator::ZeroRotator, M1_043_EnemyFeetOffsetX))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	TestTrue(TEXT("the enemy's own Tick injected the input game clock (clock > 0 after settle)"),
		Scene.EnemyCombat->GetInputClockSeconds() > 0.0);

	const double SettledAt = Scene.World->GetTimeSeconds();
	TArray<FM1_043_Press> Presses;
	Presses.Add({ ECombatInput::Light, SettledAt + 0.10, false });

	UCharacterMovementComponent* EnemyMovement = Scene.Enemy->GetCharacterMovement();

	bool bFrozenObserved = false;
	bool bStopEnded = false;
	int32 StopEndFrame = -1;
	bool bHitStunObserved = false;
	bool bStunEnded = false;
	double ClockAtStopEnd = 0.0;
	double ClockAtFree = 0.0;
	int32 Hit1Frame = -1;

	for (int32 Frame = 0; Frame < M1_043_MaxRunFrames; ++Frame)
	{
		const double Now = Scene.World->GetTimeSeconds();
		for (FM1_043_Press& Press : Presses)
		{
			if (!Press.bFired && Now + 1e-6 >= Press.FireAt)
			{
				Scene.Player->SubmitCombatInput(Press.Action);
				Press.bFired = true;
			}
		}
		if (!TestTrue(TEXT("the test world ticks"), Scene.Wrapper.TickTestWorld(M1_043_FrameSeconds)))
		{
			break;
		}

		if (Scene.Events.Hits >= 1 && Hit1Frame < 0)
		{
			Hit1Frame = Frame;
			bFrozenObserved = Scene.EnemyCombat->IsHitStopActive()
				&& EnemyMovement != nullptr
				&& EnemyMovement->MovementMode == MOVE_None;
		}
		if (Hit1Frame >= 0 && !bStopEnded && !Scene.EnemyCombat->IsHitStopActive())
		{
			bStopEnded = true;
			StopEndFrame = Frame;
			ClockAtStopEnd = Scene.EnemyCombat->GetInputClockSeconds();
			// The stun outlasts the 40 ms stop (0.22 s > 40 ms of pinned
			// clock), so the state right after the stop is HitStun.
			bHitStunObserved = Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::HitStun;
		}
		if (bStopEnded && !bStunEnded
			&& Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free)
		{
			bStunEnded = true;
			ClockAtFree = Scene.EnemyCombat->GetInputClockSeconds();
			break;
		}
	}

	TestTrue(TEXT("the first light hit opened the victim-side hit stop and froze the enemy movement (MOVE_None)"),
		bFrozenObserved);
	TestTrue(FString::Printf(TEXT(
		"the enemy's own Tick ended the 40 ms hit stop within %d frames (ended=%d at frame %d after the hit frame %d)"),
		M1_043_MaxHitStopEndFrames, bStopEnded ? 1 : 0, StopEndFrame, Hit1Frame),
		bStopEnded && StopEndFrame - Hit1Frame <= M1_043_MaxHitStopEndFrames);
	TestTrue(TEXT("the enemy sat in HitStun right after the hit stop ended"),
		bHitStunObserved);
	// The stun ended by itself and the enemy is Free again (red baseline:
	// the pinned clock never reaches the deadline, the state never resolves).
	TestTrue(TEXT("the enemy's hit stun ended by itself (back to Free)"), bStunEnded);
	// The measured span is the clock-driven remainder of the 0.22 s stun
	// (stop consumed 2-4 frames of pinned clock; frame quantization adds one
	// boundary): bounded away from both zero and a full extra second.
	const double StunSpan = ClockAtFree - ClockAtStopEnd;
	TestTrue(FString::Printf(TEXT(
		"the hit stun resolved on the injected input clock (span %.3f s in [0.05, 0.30] of the 0.22 s stun)"),
		StunSpan),
		bStunEnded && StunSpan > 0.05 && StunSpan <= 0.30);
	return true;
}

// Acceptance 4: the full launched-landing recovery. The launcher floats the
// enemy through real physics; the real ACharacter::Landed notify feeds
// NotifyLanded, which opens exactly one Knockdown 0.45 s -> Recovering
// 0.25 s -> Free process on the enemy's own injected clock, and the float
// cycle reopens only when the recovery completed (LauncherCycleCount 1 -> 0).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_043EnemyLaunchedLandingRunsFullRecovery,
	"UEMMO.Tasks.M1_043.EnemyLaunchedLandingRunsFullRecovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_043EnemyLaunchedLandingRunsFullRecovery::RunTest(const FString& Parameters)
{
	FM1_043_Scene Scene;
	if (!Scene.Build(*this, /*PlayerFacing*/ FRotator::ZeroRotator, M1_043_EnemyFeetOffsetX))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	TestTrue(TEXT("the enemy's own Tick injected the input game clock (clock > 0 after settle)"),
		Scene.EnemyCombat->GetInputClockSeconds() > 0.0);

	const double SettledAt = Scene.World->GetTimeSeconds();
	TArray<FM1_043_Press> Presses;
	Presses.Add({ ECombatInput::Launcher, SettledAt + 0.10, false });

	UCharacterMovementComponent* EnemyMovement = Scene.Enemy->GetCharacterMovement();

	bool bFrozenObserved = false;
	bool bStopEnded = false;
	bool bFloating = false;
	bool bKnockdownObserved = false;
	bool bRecoveringObserved = false;
	bool bRecoveryFree = false;
	double ClockAtKnockdown = 0.0;
	double ClockAtRecovering = 0.0;
	double ClockAtFree = 0.0;
	int32 CycleCountDuringRecovery = -1;

	for (int32 Frame = 0; Frame < M1_043_MaxRunFrames + M1_043_MaxFlightFrames; ++Frame)
	{
		const double Now = Scene.World->GetTimeSeconds();
		for (FM1_043_Press& Press : Presses)
		{
			if (!Press.bFired && Now + 1e-6 >= Press.FireAt)
			{
				Scene.Player->SubmitCombatInput(Press.Action);
				Press.bFired = true;
			}
		}
		if (!TestTrue(TEXT("the test world ticks"), Scene.Wrapper.TickTestWorld(M1_043_FrameSeconds)))
		{
			break;
		}

		const ECombatActionState State = Scene.EnemyCombat->GetSnapshot().ActionState;
		if (Scene.Events.Hits >= 1 && !bFrozenObserved)
		{
			bFrozenObserved = Scene.EnemyCombat->IsHitStopActive()
				&& EnemyMovement != nullptr
				&& EnemyMovement->MovementMode == MOVE_None;
		}
		if (bFrozenObserved && !bStopEnded && !Scene.EnemyCombat->IsHitStopActive())
		{
			bStopEnded = true;
		}
		if (Scene.Events.Hits >= 1 && !bFloating && EnemyMovement != nullptr
			&& EnemyMovement->Velocity.Z > 1.0f)
		{
			bFloating = true;
		}
		// The real landing opened the recovery process (the launched-landing
		// marker was consumed by an accepted BeginLandingRecovery).
		if (State == ECombatActionState::Knockdown && !bKnockdownObserved)
		{
			bKnockdownObserved = true;
			ClockAtKnockdown = Scene.EnemyCombat->GetInputClockSeconds();
			CycleCountDuringRecovery = Scene.Enemy->GetLauncherCycleCount();
		}
		if (bKnockdownObserved && State == ECombatActionState::Recovering && !bRecoveringObserved)
		{
			bRecoveringObserved = true;
			ClockAtRecovering = Scene.EnemyCombat->GetInputClockSeconds();
		}
		if (bRecoveringObserved && State == ECombatActionState::Free && !bRecoveryFree)
		{
			bRecoveryFree = true;
			ClockAtFree = Scene.EnemyCombat->GetInputClockSeconds();
			break;
		}
	}

	TestTrue(TEXT("the launcher hit opened the victim-side hit stop and froze the enemy movement (MOVE_None)"),
		bFrozenObserved);
	TestTrue(TEXT("the enemy's own Tick ended the 40 ms hit stop"), bStopEnded);
	TestTrue(TEXT("the launcher floated the enemy (vertical speed > 0 before the landing)"), bFloating);
	// The recovery process ran its two clock-driven phases and returned Free.
	TestTrue(TEXT("the launched landing opened the Knockdown phase"), bKnockdownObserved);
	TestTrue(TEXT("the Knockdown flipped to Recovering on the injected clock"), bRecoveringObserved);
	TestTrue(TEXT("the Recovering phase ended back to Free"), bRecoveryFree);
	// The knockdown phase ran the design 0.45 s on the injected clock (frame
	// boundary quantization keeps it inside +/- one frame).
	const double KnockdownSpan = ClockAtRecovering - ClockAtKnockdown;
	TestTrue(FString::Printf(TEXT(
		"the Knockdown phase advanced the design 0.45 s on the injected clock (span %.3f s in [0.35, 0.55])"),
		KnockdownSpan),
		bRecoveringObserved && KnockdownSpan > 0.35 && KnockdownSpan <= 0.55);
	// The recovering phase ran the design 0.25 s on the injected clock.
	const double RecoveringSpan = ClockAtFree - ClockAtRecovering;
	TestTrue(FString::Printf(TEXT(
		"the Recovering phase advanced the design 0.25 s on the injected clock (span %.3f s in [0.17, 0.33])"),
		RecoveringSpan),
		bRecoveryFree && RecoveringSpan > 0.17 && RecoveringSpan <= 0.33);
	// The float cycle stayed open through the recovery and reopened exactly
	// at the recovery completion (the fourth clear point).
	TestEqual(TEXT("the float cycle stayed open (1 launch) during the Knockdown phase"),
		CycleCountDuringRecovery, 1);
	TestEqual(TEXT("the float cycle reopened when the recovery completed"),
		Scene.Enemy->GetLauncherCycleCount(), 0);
	return true;
}

#endif
