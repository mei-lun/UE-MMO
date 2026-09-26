// M1-041: game-mode in-loop combat wiring tests. The component-level suites
// (M1-021/023/024) all inject the input clock explicitly, which masked the
// real-game defect the user hit (X/Z dead in the shipped game): the character
// never injected the clock itself, so the Free-state start/chain consumption
// stayed gated off. These tests lock the GAME-side wiring: a real
// APrototypeCharacter runs inside a fully ticked temp world (the engine's own
// FTestWorldWrapper precedent: world context + InitializeActorsForPlay +
// BeginPlay + UWorld::Tick(LEVELTICK_All) - real actor ticks, real character
// movement, real physics), the test only presses keys through the production
// intent entry SubmitCombatInput and lets the character's own Tick inject the
// input clock and the facing. The test itself never calls
// SetInputClockSeconds / SetFacingProvider / TryStartAttack.
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

namespace UE::UEMMO::Tasks::M1_041
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M1_041_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with arena
	// content (the M1-022/M1-027 placement convention). X horizontal, Y
	// depth, Z height; the floor top sits exactly at the base Z.
	const FVector M1_041_SceneBase(40000.0, 45000.0, 600.0);

	// All three ground attacks (light_01, light_02, launcher) share the hit
	// box offset X=95 and half extent X=85 (M1-009 assets), so the box covers
	// base.X+10..base.X+180 for facing +1: an enemy feet anchor at +95 stays
	// reachable through every combo stage.
	const float M1_041_EnemyFeetOffsetX = 95.0f;

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M1_041_FloorHalfThickness = 100.0f;
	const float M1_041_FloorHalfExtentXY = 4000.0f;

	// Spawn heights above the floor top: the characters drop a short distance
	// through real no-controller physics and settle onto the floor.
	const float M1_041_PlayerSpawnHeight = 120.0f;
	const float M1_041_EnemySpawnHeight = 90.0f;

	// Frame caps: the settle phase waits for the ground contact of both
	// characters; the combo loop waits for the third hit plus the launcher
	// rise plus the launcher's own finish.
	constexpr int32 M1_041_MaxSettleFrames = 300;
	constexpr int32 M1_041_MaxComboFrames = 1500;

	// Recorded combat events (the assertions read this struct only).
	struct FM1_041_Events
	{
		TArray<FName> Started;
		TArray<FName> Finished;
		TArray<int32> FacingAtStart;
		int32 Hits = 0;
	};

	// One scheduled key press: fired exactly once when the world clock
	// reaches FireAt (the press goes through the production intent entry, so
	// PressedAt is read from the world time by the character itself).
	struct FM1_041_Press
	{
		ECombatInput Action = ECombatInput::Light;
		double FireAt = 0.0;
		bool bFired = false;
	};

	// World-static blocking floor box the characters stand on (temp worlds
	// ship no geometry; the walking/landing physics needs one blocker). The
	// standard BlockAll profile (QueryAndPhysics, WorldStatic, block all) is
	// the same collision a real level floor carries.
	static AActor* M1_041_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base - FVector(0.0, 0.0, M1_041_FloorHalfThickness), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M1_041_Floor"));
		Actor->SetRootComponent(Floor);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->SetBoxExtent(FVector(M1_041_FloorHalfExtentXY, M1_041_FloorHalfExtentXY, M1_041_FloorHalfThickness));
		Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Floor->RegisterComponent();
		// The root was assigned after the spawn transform was applied, so the
		// world location is (re-)applied explicitly (the M1-022 pattern).
		Floor->SetWorldLocation(Base - FVector(0.0, 0.0, M1_041_FloorHalfThickness));
		return Actor;
	}

	// Spawns the real prototype pawn FeetHeight above the floor top and
	// enables no-controller physics so its movement component initializes and
	// integrates like the possessed game pawn does (the engine auto-activates
	// the component and sets the default movement mode during the play init;
	// without a controller the flag is what keeps the physics running).
	static APrototypeCharacter* M1_041_SpawnPlayer(UWorld& World, const FRotator& Facing)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			M1_041_SceneBase + FVector(0.0, 0.0, M1_041_PlayerSpawnHeight),
			Facing, Params);
		if (Player == nullptr)
		{
			return nullptr;
		}
		if (UCharacterMovementComponent* Movement = Player->GetCharacterMovement())
		{
			// The pawn spawned into an already begun world, so the component
			// init ran before this flag was set (mode stayed MOVE_None, which
			// silently drops every integration step). Mirror the two engine
			// steps the real arena's initialization performs (the M1-022/
			// M1-027 pattern): activate the component and apply the default
			// movement mode, then the no-controller physics runs and settles
			// the pawn onto the floor like a possessed game pawn.
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
	// horizontally to the requested enemy feet anchor (the anchor is shared
	// by every current attack box, see M1_041_EnemyFeetOffsetX above).
	static ATrainingEnemy* M1_041_SpawnEnemy(UWorld& World, float FeetOffsetX)
	{
		FActorSpawnParameters Params;
		ATrainingEnemy* Enemy = World.SpawnActor<ATrainingEnemy>(
			ATrainingEnemy::StaticClass(),
			M1_041_SceneBase + FVector(FeetOffsetX, 0.0, M1_041_EnemySpawnHeight),
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
	// tests build it, press keys and read the results.
	struct FM1_041_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		APrototypeCharacter* Player = nullptr;
		UCombatComponent* PlayerCombat = nullptr;
		ATrainingEnemy* Enemy = nullptr;
		FM1_041_Events Events;

		// Builds world + floor + player + enemy and binds the event record.
		// Returns false after reporting the problem so the caller can bail.
		//
		// Everything spawns into an ALREADY BEGUN world (the engine wrapper
		// runs SetGameMode/InitializeActorsForPlay/BeginPlay first, then the
		// actors spawn through the normal mid-session path where component
		// registration commits bodies and BeginPlay dispatches immediately -
		// the real BeginPlay that injects the catalog, the jump handler, the
		// air state and (M1-041) the facing provider into the component).
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
			AActor* Floor = M1_041_SpawnFloor(*World, M1_041_SceneBase);
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
				M1_041_SceneBase + FVector(0.0, 0.0, M1_041_PlayerSpawnHeight),
				M1_041_SceneBase - FVector(0.0, 0.0, 2.0 * M1_041_FloorHalfThickness),
				FQuat::Identity,
				ECC_Pawn,
				ProbeShape,
				FCollisionQueryParams(TEXT("M1_041_FloorProbe"), false));
			UBoxComponent* FloorBox = Floor->FindComponentByClass<UBoxComponent>();
			if (!Test.TestTrue(FString::Printf(TEXT(
				"the floor blocks a pawn capsule sweep (hit=%s floorRegistered=%d floorBody=%d)"),
				ProbeHit.Component.IsValid() ? *ProbeHit.Component->GetName() : TEXT("none"),
				FloorBox != nullptr && FloorBox->IsRegistered() ? 1 : 0,
				FloorBox != nullptr && FloorBox->GetBodyInstance() != nullptr ? 1 : 0),
				bFloorBlocks))
			{
				// Pinpoint why the blocker is invisible to scene queries.
				FHitResult ObjHit(NoInit);
				ObjHit.Init();
				const bool bObjHit = World->SweepSingleByObjectType(
					ObjHit,
					M1_041_SceneBase + FVector(0.0, 0.0, M1_041_PlayerSpawnHeight),
					M1_041_SceneBase - FVector(0.0, 0.0, 2.0 * M1_041_FloorHalfThickness),
					FQuat::Identity,
					FCollisionObjectQueryParams(ECC_WorldStatic),
					ProbeShape);
				FBodyInstance* Body = FloorBox != nullptr ? FloorBox->GetBodyInstance() : nullptr;
				Test.AddError(FString::Printf(TEXT(
					"floor probe details (physicsScene=%d bodyInScene=%d bodyQueryEnabled=%d bodyProfile=%s objectSweepHit=%d)"),
					World->GetPhysicsScene() != nullptr ? 1 : 0,
					Body != nullptr && Body->GetPhysicsActorHandle() != nullptr ? 1 : 0,
					Body != nullptr && (Body->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics || Body->GetCollisionEnabled() == ECollisionEnabled::QueryOnly) ? 1 : 0,
					FloorBox != nullptr ? *FloorBox->GetCollisionProfileName().ToString() : TEXT("none"),
					bObjHit ? 1 : 0));
				return false;
			}
			Player = M1_041_SpawnPlayer(*World, PlayerFacing);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			PlayerCombat = Player->GetCombat();
			if (!Test.TestTrue(TEXT("the player carries a combat component"), PlayerCombat != nullptr))
			{
				return false;
			}
			Enemy = M1_041_SpawnEnemy(*World, EnemyFeetOffsetX);
			if (!Test.TestNotNull(TEXT("the training enemy spawns"), Enemy))
			{
				return false;
			}

			PlayerCombat->OnStarted.AddLambda([this](FName AttackId, uint64 /*InstanceId*/)
			{
				Events.Started.Add(AttackId);
				// The facing the component actually used for the start is
				// visible in the snapshot minted for this instance.
				Events.FacingAtStart.Add(PlayerCombat->GetSnapshot().Facing);
			});
			PlayerCombat->OnFinished.AddLambda([this](FName AttackId, uint64 /*InstanceId*/)
			{
				Events.Finished.Add(AttackId);
			});
			PlayerCombat->OnHitConfirmed.AddLambda([this](const FCombatHit& /*Hit*/)
			{
				++Events.Hits;
			});
			return true;
		}

		// Ticks the world until both characters settled onto the floor (real
		// walking physics). Returns false after reporting when the ground
		// contact never happened within the settle cap.
		bool Settle(FAutomationTestBase& Test)
		{
			UCharacterMovementComponent* PlayerMovement = Player->GetCharacterMovement();
			UCharacterMovementComponent* EnemyMovement = Enemy->GetCharacterMovement();
			for (int32 Frame = 0; Frame < M1_041_MaxSettleFrames; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M1_041_FrameSeconds)))
				{
					return false;
				}
				TickEnemyCombatStandIn();
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
				"(worldSeconds=%.3f playerMode=%d playerTick=%d playerMoveTick=%d playerVel=%.1f playerZ=%.1f "
				"enemyMode=%d enemyTick=%d enemyMoveTick=%d enemyVel=%.1f enemyZ=%.1f "
				"playerBegun=%d playerInitialized=%d enemyBegun=%d)"),
				World->GetTimeSeconds(),
				PlayerMovement ? static_cast<int32>(PlayerMovement->MovementMode.GetValue()) : -1,
				Player->IsActorTickEnabled() ? 1 : 0,
				(PlayerMovement != nullptr && PlayerMovement->IsComponentTickEnabled()) ? 1 : 0,
				PlayerMovement ? PlayerMovement->Velocity.Size() : -1.0f,
				Player->GetActorLocation().Z,
				EnemyMovement ? static_cast<int32>(EnemyMovement->MovementMode.GetValue()) : -1,
				Enemy->IsActorTickEnabled() ? 1 : 0,
				(EnemyMovement != nullptr && EnemyMovement->IsComponentTickEnabled()) ? 1 : 0,
				EnemyMovement ? EnemyMovement->Velocity.Size() : -1.0f,
				Enemy->GetActorLocation().Z,
				Player->HasActorBegunPlay() ? 1 : 0,
				Player->IsActorInitialized() ? 1 : 0,
				Enemy->HasActorBegunPlay() ? 1 : 0));
			return false;
		}

		float EnemyHealth() const
		{
			const UHealthComponent* Health = Enemy->GetHealthComponent();
			return (Health != nullptr) ? Health->GetHealth() : -1.0f;
		}

		// M1-041 known-gap stand-in, enemy side only: game code never ticks
		// the training enemy's combat component (APrototypeCharacter drives
		// its own; the enemy carries no driver and the component's own tick
		// is disabled). Without a drive the M1-033 hit stop requested on the
		// target's component never ends, so the launcher's pending launch
		// would never leave the frozen movement. This stand-in fills exactly
		// that enemy-side gap once per frame; the PLAYER path under test gets
		// NO test-side injection - its input clock comes exclusively from its
		// own Tick inside the world tick (the wiring pivot asserted above).
		void TickEnemyCombatStandIn()
		{
			UCombatComponent* EnemyCombat = Enemy != nullptr ? Enemy->GetCombatComponent() : nullptr;
			if (EnemyCombat != nullptr && World != nullptr)
			{
				EnemyCombat->SetInputClockSeconds(World->GetTimeSeconds());
				EnemyCombat->TickCombat(M1_041_FrameSeconds);
			}
		}
	};
}

using namespace UE::UEMMO::Tasks::M1_041;

// Core acceptance case (the user's dead combo, replayed inside a real ticked
// world): a real APrototypeCharacter presses X, X, Z through the production
// intent entry while its own Tick injects the input game clock and the
// facing. The combo must chain light_01 -> light_02 -> launcher, land all
// three hits (HP 100 -> 90 -> 76 -> 58) and float the target (Z speed > 0).
// On the pre-M1-041 wiring this fails at the first gate: the character never
// injects the clock, so every press stays buffered forever.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_041RealCharacterXXZComboStartsHitsAndLaunches,
	"UEMMO.Tasks.M1_041.RealCharacterXXZComboStartsHitsAndLaunches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_041RealCharacterXXZComboStartsHitsAndLaunches::RunTest(const FString& Parameters)
{
	FM1_041_Scene Scene;
	if (!Scene.Build(*this, /*PlayerFacing*/ FRotator::ZeroRotator, M1_041_EnemyFeetOffsetX))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}

	// The wiring pivot: the character's own Tick must have injected the input
	// game clock by now (a bare component reads 0.0 forever without it).
	TestTrue(TEXT("the character's own Tick injected the input game clock (clock > 0 after settle)"),
		Scene.PlayerCombat->GetInputClockSeconds() > 0.0);

	// The user's X X Z sequence, scheduled on the world clock: the first X
	// starts light_01 from Free; the second X lands inside light_01's cancel
	// window [12,24); the Z lands inside light_02's cancel window [16,29).
	// The generous gaps also clear the 40 ms hit-stop freezes after each hit,
	// so the timing exercises the M1-033 pinned-clock path without racing it.
	const double SettledAt = Scene.World->GetTimeSeconds();
	TArray<FM1_041_Press> Presses;
	Presses.Add({ ECombatInput::Light, SettledAt + 0.10, false });
	Presses.Add({ ECombatInput::Light, SettledAt + 0.45, false });
	Presses.Add({ ECombatInput::Launcher, SettledAt + 0.85, false });

	// HP recorded the first time each hit level is observed (100 -> 90 ->
	// 76 -> 58 with the M1-009 damages 10/14/18); -1 means never observed.
	float HpAfterHit1 = -1.0f;
	float HpAfterHit2 = -1.0f;
	float HpAfterHit3 = -1.0f;
	bool bFloating = false;

	for (int32 Frame = 0; Frame < M1_041_MaxComboFrames; ++Frame)
	{
		const double Now = Scene.World->GetTimeSeconds();
		for (FM1_041_Press& Press : Presses)
		{
			if (!Press.bFired && Now + 1e-6 >= Press.FireAt)
			{
				// The production intent entry (the exact target of the real
				// X/Z input bindings); the character reads PressedAt itself.
				Scene.Player->SubmitCombatInput(Press.Action);
				Press.bFired = true;
			}
		}
		if (!TestTrue(TEXT("the test world ticks"), Scene.Wrapper.TickTestWorld(M1_041_FrameSeconds)))
		{
			break;
		}
		Scene.TickEnemyCombatStandIn();

		if (Scene.Events.Hits == 1 && HpAfterHit1 < 0.0f)
		{
			HpAfterHit1 = Scene.EnemyHealth();
		}
		if (Scene.Events.Hits == 2 && HpAfterHit2 < 0.0f)
		{
			HpAfterHit2 = Scene.EnemyHealth();
		}
		if (Scene.Events.Hits == 3 && HpAfterHit3 < 0.0f)
		{
			HpAfterHit3 = Scene.EnemyHealth();
		}
		if (Scene.Events.Hits == 3 && !bFloating)
		{
			const UCharacterMovementComponent* EnemyMovement = Scene.Enemy->GetCharacterMovement();
			bFloating = EnemyMovement != nullptr && EnemyMovement->Velocity.Z > 1.0f;
		}
		// Done once the launcher instance finished (the two chained lights
		// finish through the cancel-window retirements before that).
		if (Scene.Events.Finished.Num() >= 3 && bFloating)
		{
			break;
		}
	}

	// The combo ran end to end: three starts in order with the real facing.
	TestEqual(TEXT("exactly three attacks started (light_01, light_02, launcher)"), Scene.Events.Started.Num(), 3);
	if (Scene.Events.Started.Num() == 3)
	{
		TestEqual(TEXT("the first press started light_01 from Free"),
			Scene.Events.Started[0], FName(TEXT("light_01")));
		TestEqual(TEXT("the second press chained into light_02 through light_01's cancel window"),
			Scene.Events.Started[1], FName(TEXT("light_02")));
		TestEqual(TEXT("the third press chained into launcher through light_02's cancel window"),
			Scene.Events.Started[2], FName(TEXT("launcher")));
	}
	for (int32 Index = 0; Index < Scene.Events.FacingAtStart.Num(); ++Index)
	{
		TestEqual(FString::Printf(TEXT("attack start %d carried the yaw-derived facing +1 (right)"), Index),
			Scene.Events.FacingAtStart[Index], 1);
	}

	// The three hits actually applied the design damage.
	TestEqual(TEXT("the first hit lands and deducts exactly the light_01 damage"), HpAfterHit1, 90.0f, 0.01f);
	TestEqual(TEXT("the second hit lands and deducts exactly the light_02 damage"), HpAfterHit2, 76.0f, 0.01f);
	TestEqual(TEXT("the launcher hit lands and deducts exactly the launcher damage"), HpAfterHit3, 58.0f, 0.01f);

	// The launcher floated the target (M1-022: the launch rises at the
	// definition speed through the real movement integration).
	TestTrue(TEXT("the launcher hit left the target floating (vertical speed > 0)"), bFloating);

	// Every instance retired exactly once: the two chained lights through
	// their cancel-window retirements, the launcher through its natural end.
	TestEqual(TEXT("exactly three attacks finished (light_01, light_02, launcher)"), Scene.Events.Finished.Num(), 3);
	if (Scene.Events.Finished.Num() == 3)
	{
		TestEqual(TEXT("the retired first instance finished as light_01"),
			Scene.Events.Finished[0], FName(TEXT("light_01")));
		TestEqual(TEXT("the retired second instance finished as light_02"),
			Scene.Events.Finished[1], FName(TEXT("light_02")));
		TestEqual(TEXT("the launcher finished its natural timeline"),
			Scene.Events.Finished[2], FName(TEXT("launcher")));
	}
	TestTrue(TEXT("the combat component returned to Free after the full combo"),
		Scene.PlayerCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	return true;
}

// The facing source: with the pawn spawned left (yaw 180, the exact value
// M1-029's planar flip writes) a single X press must start light_01 with the
// mirrored facing -1, which mirrors the hit box onto the enemy standing on
// the LEFT. With the facing unwired (stored 0, read as +1) the box opens to
// the right and the hit never lands.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_041RealCharacterFacingMirrorsHitboxToYaw,
	"UEMMO.Tasks.M1_041.RealCharacterFacingMirrorsHitboxToYaw",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_041RealCharacterFacingMirrorsHitboxToYaw::RunTest(const FString& Parameters)
{
	FM1_041_Scene Scene;
	if (!Scene.Build(*this, /*PlayerFacing*/ FRotator(0.0, 180.0, 0.0), -M1_041_EnemyFeetOffsetX))
	{
		return true;
	}
	if (!Scene.Settle(*this))
	{
		return true;
	}
	TestTrue(TEXT("the character's own Tick injected the input game clock (clock > 0 after settle)"),
		Scene.PlayerCombat->GetInputClockSeconds() > 0.0);

	const double SettledAt = Scene.World->GetTimeSeconds();
	TArray<FM1_041_Press> Presses;
	Presses.Add({ ECombatInput::Light, SettledAt + 0.10, false });

	for (int32 Frame = 0; Frame < M1_041_MaxComboFrames; ++Frame)
	{
		const double Now = Scene.World->GetTimeSeconds();
		for (FM1_041_Press& Press : Presses)
		{
			if (!Press.bFired && Now + 1e-6 >= Press.FireAt)
			{
				Scene.Player->SubmitCombatInput(Press.Action);
				Press.bFired = true;
			}
		}
		if (!TestTrue(TEXT("the test world ticks"), Scene.Wrapper.TickTestWorld(M1_041_FrameSeconds)))
		{
			break;
		}
		if (Scene.Events.Hits >= 1 && Scene.Events.Finished.Num() >= 1)
		{
			break;
		}
	}

	TestEqual(TEXT("exactly one attack started"), Scene.Events.Started.Num(), 1);
	TestEqual(TEXT("the press started light_01"),
		Scene.Events.Started.Num() == 1 ? Scene.Events.Started[0] : FName(), FName(TEXT("light_01")));
	TestEqual(TEXT("the start carried the yaw-derived facing -1 (left)"),
		Scene.Events.FacingAtStart.Num() == 1 ? Scene.Events.FacingAtStart[0] : 0, -1);
	TestEqual(TEXT("the mirrored light_01 hit the left-standing enemy exactly once"), Scene.Events.Hits, 1);
	TestEqual(TEXT("the hit deducted exactly the light_01 damage"), Scene.EnemyHealth(), 90.0f, 0.01f);
	return true;
}

#endif
