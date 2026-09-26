#include "Misc/AutomationTest.h"
#include "EngineUtils.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatGeometry.h"
#include "../Combat/CombatHitTypes.h"
#include "../Combat/CombatInputBuffer.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/TrainingEnemy.h"
#include "../PrototypeCharacter.h"
#include "../Room/TrainingResetService.h"

#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_027
{
	// Remote scene base, unique from every other suite's base so the temp
	// world never shares a query space with arena content or other tests.
	// X horizontal, Y depth, Z height; the base is the player's spawn.
	const FVector M1_027_PlayerSpawn(80000.0, 82000.0, 600.0);

	// Vertical half height of the player capsule (InitCapsuleSize(36, 96));
	// the combat feet origin is the capsule center minus this value.
	const float M1_027_PlayerHalfHeight = 96.0f;

	// Y spacing of the second registered enemy; far outside the 50 cm Y half
	// extent of every hit box, so only enemy 0 ever sits in a hit box.
	const float M1_027_SecondEnemyYOffset = 400.0f;

	// Spawn point of the service-less fallback player of the R-key test,
	// remote from the main scene base.
	const FVector M1_027_BarePlayerSpawn(80000.0, 86000.0, 600.0);

	// World acquisition, same order as the M1-018 through M1-026 pattern: a
	// private temp world first, the shared game world as fallback.
	static UWorld* M1_027_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M1_027_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M1_027 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M1_027 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	// Loads the real read-only catalog from Config/DefaultGame.ini (the same
	// loading path production and the earlier combat tests use). Returns null
	// after reporting the failure so the caller can bail out early.
	static UAttackCatalog* M1_027_NewCatalog(FAutomationTestBase& Test)
	{
		UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
		FText Error;
		if (!Test.TestTrue(TEXT("the attack catalog initializes from DefaultGame.ini"), Catalog->InitializeFromConfig(Error)))
		{
			Test.AddError(FString::Printf(TEXT("catalog initialization failed: %s"), *Error.ToString()));
			return nullptr;
		}
		return Catalog;
	}

	// Advances the component by exactly Count single 1/60 s frames.
	static void M1_027_TickFrames(UCombatComponent& Combat, int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Combat.TickCombat(1.0f / 60.0f);
		}
	}

	// Applies exactly the two pending-velocity steps of one real movement
	// update, in PerformMovement order (ApplyAccumulatedForces, then
	// HandlePendingLaunch), without a world tick - the same proven pattern
	// the M1-022/M1-025/M1-026 suites use for temp worlds that never
	// integrate physics.
	static void M1_027_DrivePendingVelocity(UCharacterMovementComponent& Movement)
	{
		Movement.ApplyAccumulatedForces(1.0f / 60.0f);
		Movement.HandlePendingLaunch();
	}

	// One synthetic victim-side hit for the NotifyHitReceived entry, in the
	// M1-020/M1-026 pattern: only the fields the victim entry consumes are
	// filled (target identity and the requested stun); it never deducts
	// health.
	static FCombatHit M1_027_MakeStunHit(AActor* Victim, float StunSeconds)
	{
		FCombatHit Hit;
		Hit.Target = Victim;
		Hit.StunSeconds = StunSeconds;
		return Hit;
	}

	// Activates the movement component the way the real arena's component
	// initialization would (temp worlds skip PreInitializeComponents, so the
	// M1-022 pattern mirrors the two engine steps by hand).
	static void M1_027_ActivateMovement(UCharacterMovementComponent* Movement)
	{
		if (Movement != nullptr)
		{
			if (!Movement->IsActive())
			{
				Movement->Activate(/*bReset*/ true);
			}
			if (Movement->MovementMode == MOVE_None)
			{
				Movement->SetDefaultMovementMode();
			}
		}
	}

	// One scene: the real catalog, one player pawn spawned at the scene base,
	// EnemyCount training enemies (enemy 0 stands exactly on the player's
	// launcher hit box center, the position both first moves reach) and the
	// reset service with every participant registered.
	struct FM1_027_ResetScene
	{
		UAttackCatalog* Catalog = nullptr;
		UTrainingResetService* Service = nullptr;
		APrototypeCharacter* Player = nullptr;
		UCombatComponent* PlayerCombat = nullptr;
		UCharacterMovementComponent* PlayerMovement = nullptr;
		TArray<ATrainingEnemy*> Enemies;
		FVector EnemyAnchor = FVector::ZeroVector;
		int32 HitCount = 0;
		uint64 LastHitInstanceId = 0;

		// Builds catalog + player + enemies + service. Returns false after
		// reporting the problem so the caller can bail out early.
		bool Build(FAutomationTestBase& Test, UWorld& World, int32 EnemyCount)
		{
			Catalog = M1_027_NewCatalog(Test);
			if (Catalog == nullptr)
			{
				return false;
			}
			const UAttackDefinition* LauncherDefinition = Catalog->Find(FName(TEXT("launcher")));
			if (!Test.TestTrue(TEXT("the catalog holds launcher"), LauncherDefinition != nullptr))
			{
				return false;
			}

			FActorSpawnParameters SpawnParams;
			Player = World.SpawnActor<APrototypeCharacter>(
				APrototypeCharacter::StaticClass(), M1_027_PlayerSpawn, FRotator::ZeroRotator, SpawnParams);
			if (!Test.TestNotNull(TEXT("the player spawns"), Player))
			{
				return false;
			}
			if (!Player->HasActorBegunPlay())
			{
				Player->DispatchBeginPlay();
			}
			M1_027_ActivateMovement(Player->GetCharacterMovement());
			PlayerMovement = Player->GetCharacterMovement();
			PlayerCombat = Player->GetCombat();
			if (!Test.TestTrue(TEXT("the player carries a combat component"), PlayerCombat != nullptr))
			{
				return false;
			}
			PlayerCombat->OnHitConfirmed.AddLambda([this](const FCombatHit& Hit)
			{
				++HitCount;
				LastHitInstanceId = Hit.AttackInstanceId;
			});

			// Enemy 0 stands on the player's launcher hit box center (the
			// launcher box matches the light_01 box, so one anchor serves
			// both first moves); further enemies offset in Y stay outside
			// every hit box and only ride the registration set.
			const FVector PlayerFeet = Player->GetActorLocation() - FVector(0.0f, 0.0f, M1_027_PlayerHalfHeight);
			EnemyAnchor = ComputeHitBox(PlayerFeet, 1, *LauncherDefinition).Center;

			for (int32 Index = 0; Index < EnemyCount; ++Index)
			{
				const FVector Anchor = EnemyAnchor + FVector(0.0f, Index * M1_027_SecondEnemyYOffset, 0.0f);
				ATrainingEnemy* Enemy = World.SpawnActor<ATrainingEnemy>(
					ATrainingEnemy::StaticClass(), Anchor, FRotator::ZeroRotator, SpawnParams);
				if (!Test.TestNotNull(TEXT("the training enemy spawns"), Enemy))
				{
					return false;
				}
				if (!Enemy->HasActorBegunPlay())
				{
					Enemy->DispatchBeginPlay();
				}
				// Pin the anchor explicitly so the reset target is
				// deterministic regardless of BeginPlay capture order.
				Enemy->SetSpawnAnchor(Anchor, FRotator::ZeroRotator);
				M1_027_ActivateMovement(Enemy->GetCharacterMovement());
				// The production dummy carries no attack catalog; the tests
				// inject one so the enemy's own component can prove the
				// alive/Free/attackable state after each reset.
				if (Enemy->GetCombatComponent() != nullptr)
				{
					Enemy->GetCombatComponent()->InitializeFromCatalog(Catalog);
				}
				Enemies.Add(Enemy);
			}

			Service = NewObject<UTrainingResetService>();
			Service->RegisterPlayer(Player);
			for (ATrainingEnemy* Enemy : Enemies)
			{
				Service->RegisterEnemy(Enemy);
			}
			// The two-way wiring: the service knows its participants, and the
			// pawn knows the service its R-key path routes into.
			Player->SetTrainingResetService(Service);
			return Test.TestTrue(TEXT("the service registered every participant"),
				Service->GetRegisteredEnemyCount() == EnemyCount && Service->HasRegisteredPlayer());
		}

		ATrainingEnemy* Enemy(int32 Index = 0) const { return Enemies[Index]; }

		UCombatComponent* EnemyCombat(int32 Index = 0) const { return Enemies[Index]->GetCombatComponent(); }

		float EnemyHealth(int32 Index = 0) const
		{
			const UHealthComponent* Health = Enemies[Index]->GetHealthComponent();
			return (Health != nullptr) ? Health->GetHealth() : -1.0f;
		}

		// One launcher instance from the player ticks exactly to its first
		// active frame (12), where the hit lands on enemy 0.
		bool HitEnemyWithLauncher(FAutomationTestBase& Test)
		{
			if (!Test.TestTrue(TEXT("the launcher attack starts"), PlayerCombat->TryStartAttack(FName(TEXT("launcher")), 1)))
			{
				return false;
			}
			M1_027_TickFrames(*PlayerCombat, 13);
			return true;
		}

		// One light_01 instance from the player ticks exactly to its first
		// active frame (7), where the hit lands on enemy 0.
		bool HitEnemyWithLight(FAutomationTestBase& Test)
		{
			if (!Test.TestTrue(TEXT("the light_01 attack starts"), PlayerCombat->TryStartAttack(FName(TEXT("light_01")), 1)))
			{
				return false;
			}
			M1_027_TickFrames(*PlayerCombat, 8);
			return true;
		}
	};
}

using namespace UE::UEMMO::Tasks::M1_027;

// State 1 of the four-state acceptance: while the player and the enemy are
// both mid-attack, the unified reset tears both instances down (Free, empty
// buffer, no active id) and restores the player physics (spawn position, zero
// velocity, spawn facing); afterwards both combatants can start attacks again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_027ResetFromAttackingStateRestoresFreeMovableAndAttackable,
	"UEMMO.Tasks.M1_027.ResetFromAttackingStateRestoresFreeMovableAndAttackable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_027ResetFromAttackingStateRestoresFreeMovableAndAttackable::RunTest(const FString& Parameters)
{
	UWorld* World = M1_027_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_027_ResetScene Scene;
	if (!Scene.Build(*this, *World, /*EnemyCount*/ 1))
	{
		return true;
	}

	// Both combatants enter Attacking; the player also carries one buffered
	// input and displaced physics (position away from spawn, live velocity,
	// flipped facing) to prove the reset covers the physics half too.
	if (!TestTrue(TEXT("the player attack starts"), Scene.PlayerCombat->TryStartAttack(FName(TEXT("light_01")), 1)))
	{
		return true;
	}
	if (!TestTrue(TEXT("the enemy attack starts"), Scene.EnemyCombat()->TryStartAttack(FName(TEXT("light_01")), 1)))
	{
		return true;
	}
	FBufferedCombatInput Intent;
	Intent.Sequence = 1;
	Intent.Action = ECombatInput::Light;
	Intent.PressedAt = 1.0;
	Scene.PlayerCombat->QueueInput(Intent);
	Scene.Player->SetActorLocation(M1_027_PlayerSpawn + FVector(500.0f, 0.0f, 0.0f), false, nullptr, ETeleportType::TeleportPhysics);
	Scene.PlayerMovement->Velocity = FVector(300.0f, 40.0f, 0.0f);
	Scene.Player->SetActorRotation(FRotator(0.0f, 180.0f, 0.0f));
	TestTrue(TEXT("precondition: the player is mid-attack with a buffered input"),
		Scene.PlayerCombat->GetSnapshot().ActionState == ECombatActionState::Attacking
		&& Scene.PlayerCombat->GetSnapshot().BufferSize == 1);
	TestTrue(TEXT("precondition: the enemy is mid-attack"),
		Scene.EnemyCombat()->GetSnapshot().ActionState == ECombatActionState::Attacking);

	Scene.Service->ResetTrainingSession();

	const FCombatSnapshot PlayerAfter = Scene.PlayerCombat->GetSnapshot();
	TestTrue(TEXT("the player reset back to Free with no active instance and an empty buffer"),
		PlayerAfter.ActionState == ECombatActionState::Free
		&& PlayerAfter.AttackId == NAME_None
		&& PlayerAfter.InstanceId == 0
		&& PlayerAfter.BufferSize == 0);
	TestTrue(TEXT("the enemy reset back to Free with no active instance"),
		Scene.EnemyCombat()->GetSnapshot().ActionState == ECombatActionState::Free
		&& Scene.EnemyCombat()->GetSnapshot().InstanceId == 0);
	TestTrue(TEXT("the reset player accepts movement again"), Scene.PlayerCombat->CanAcceptMovement());
	TestTrue(TEXT("the reset enemy accepts movement again"), Scene.EnemyCombat()->CanAcceptMovement());

	// Both combatants can attack again right after the reset.
	if (!TestTrue(TEXT("the player can start a fresh attack after the reset"),
		Scene.PlayerCombat->TryStartAttack(FName(TEXT("light_01")), 1)))
	{
		return true;
	}
	TestTrue(TEXT("the fresh player attack instance id is a new nonzero id"),
		Scene.PlayerCombat->GetSnapshot().InstanceId > 0);
	if (!TestTrue(TEXT("the enemy can start a fresh attack after the reset"),
		Scene.EnemyCombat()->TryStartAttack(FName(TEXT("light_01")), 1)))
	{
		return true;
	}
	TestTrue(TEXT("the fresh enemy attack instance id is a new nonzero id"),
		Scene.EnemyCombat()->GetSnapshot().InstanceId > 0);

	// The physics half: spawn position, zero velocity, spawn facing; the
	// enemy teleports back onto its anchor.
	TestTrue(TEXT("the player is back at the spawn position"),
		Scene.Player->GetActorLocation().Equals(M1_027_PlayerSpawn, 0.01f));
	TestTrue(TEXT("the player velocity is zeroed"), Scene.PlayerMovement->Velocity.IsZero());
	TestTrue(TEXT("the player facing is restored to the spawn rotation"),
		Scene.Player->GetActorRotation().Equals(FRotator::ZeroRotator));
	TestTrue(TEXT("the enemy is back on its spawn anchor"),
		Scene.Enemy()->GetActorLocation().Equals(Scene.EnemyAnchor, 0.01f));
	return true;
}

// State 2 of the four-state acceptance: while the enemy floats after a real
// launcher hit (rising at 700 cm/s, air combo 1, float cycle 1, stunned, HP
// drained), the unified reset grounds it at full HP with every airborne
// counter cleared; a later plain landing must not knock it down, and the
// player side comes back Free too.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_027ResetFromLaunchedAirborneStateRestoresGroundedFreshTarget,
	"UEMMO.Tasks.M1_027.ResetFromLaunchedAirborneStateRestoresGroundedFreshTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_027ResetFromLaunchedAirborneStateRestoresGroundedFreshTarget::RunTest(const FString& Parameters)
{
	UWorld* World = M1_027_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_027_ResetScene Scene;
	if (!Scene.Build(*this, *World, /*EnemyCount*/ 1))
	{
		return true;
	}

	if (!Scene.HitEnemyWithLauncher(*this))
	{
		return true;
	}
	TestEqual(TEXT("the launcher hit the enemy exactly once"), Scene.HitCount, 1);
	M1_027_DrivePendingVelocity(*Scene.Enemy()->GetCharacterMovement());
	TestTrue(TEXT("precondition: the launched enemy rises at the launch speed"),
		FMath::IsNearlyEqual(Scene.Enemy()->GetCharacterMovement()->Velocity.Z, 700.0f, 0.01f)
		&& Scene.Enemy()->GetAirState() == ECombatAirState::Rising);
	TestTrue(TEXT("precondition: the launched enemy is stunned with both airborne counters open"),
		Scene.EnemyCombat()->GetSnapshot().ActionState == ECombatActionState::HitStun
		&& Scene.Enemy()->GetAirComboCount() == 1
		&& Scene.Enemy()->GetLauncherCycleCount() == 1);
	TestEqual(TEXT("precondition: the launcher damage drained the enemy HP"), Scene.EnemyHealth(), 82.0f);

	Scene.Service->ResetTrainingSession();

	TestEqual(TEXT("the reset restored the enemy HP to full"), Scene.EnemyHealth(), 100.0f);
	TestTrue(TEXT("the reset zeroed the launch velocity"), Scene.Enemy()->GetCharacterMovement()->Velocity.IsZero());
	TestTrue(TEXT("the reset grounded the enemy"), Scene.Enemy()->GetAirState() == ECombatAirState::Grounded);
	TestEqual(TEXT("the reset cleared the air combo count"), Scene.Enemy()->GetAirComboCount(), 0);
	TestEqual(TEXT("the reset cleared the launcher float cycle count"), Scene.Enemy()->GetLauncherCycleCount(), 0);
	TestTrue(TEXT("the reset returned the enemy to Free with no recovery process"),
		Scene.EnemyCombat()->GetSnapshot().ActionState == ECombatActionState::Free
		&& !Scene.EnemyCombat()->IsInLandingRecovery());
	TestTrue(TEXT("the enemy is back on its spawn anchor"),
		Scene.Enemy()->GetActorLocation().Equals(Scene.EnemyAnchor, 0.01f));

	// The cleared launched-airborne marker is observable at the next landing:
	// a plain landing after the reset must never build a knockdown process.
	Scene.Enemy()->NotifyLanded(/*NowSeconds*/ 5.0);
	TestTrue(TEXT("a plain landing after the reset never knocks the enemy down"),
		Scene.EnemyCombat()->GetSnapshot().ActionState == ECombatActionState::Free
		&& !Scene.EnemyCombat()->IsInLandingRecovery());

	// The player side (its launcher instance was torn down mid-flight) is
	// Free and can start a fresh attack.
	TestTrue(TEXT("the player reset back to Free"),
		Scene.PlayerCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestTrue(TEXT("the player can start a fresh attack after the reset"),
		Scene.PlayerCombat->TryStartAttack(FName(TEXT("light_01")), 1));
	return true;
}

// State 3 of the four-state acceptance: an enemy inside its Knockdown phase
// resets straight back to Free - no residual deadline fires later, movement
// is accepted again and a new stun request is processed (during recovery it
// would be refused).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_027ResetFromKnockdownStateLeavesNoResidualTimer,
	"UEMMO.Tasks.M1_027.ResetFromKnockdownStateLeavesNoResidualTimer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_027ResetFromKnockdownStateLeavesNoResidualTimer::RunTest(const FString& Parameters)
{
	UWorld* World = M1_027_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_027_ResetScene Scene;
	if (!Scene.Build(*this, *World, /*EnemyCount*/ 1))
	{
		return true;
	}

	if (!TestTrue(TEXT("the landing recovery process starts (Knockdown)"),
		Scene.EnemyCombat()->BeginLandingRecovery(/*NowSeconds*/ 1.0)))
	{
		return true;
	}
	TestTrue(TEXT("precondition: the enemy is inside its knockdown"),
		Scene.EnemyCombat()->GetSnapshot().ActionState == ECombatActionState::Knockdown
		&& Scene.EnemyCombat()->IsInLandingRecovery());

	Scene.Service->ResetTrainingSession();

	TestTrue(TEXT("the reset returned the knockdown enemy to Free"),
		Scene.EnemyCombat()->GetSnapshot().ActionState == ECombatActionState::Free
		&& !Scene.EnemyCombat()->IsInLandingRecovery());

	// No residual timer: with the input clock jumped far past where the old
	// knockdown (1.45 s) and recovering (1.70 s) deadlines sat, successive
	// ticks leave the state at Free (a surviving deadline would flip the
	// state machine) and a fresh attack starts immediately.
	Scene.EnemyCombat()->SetInputClockSeconds(/*NowSeconds*/ 50.0);
	M1_027_TickFrames(*Scene.EnemyCombat(), 2);
	TestTrue(TEXT("the old knockdown deadline never fires after the reset"),
		Scene.EnemyCombat()->GetSnapshot().ActionState == ECombatActionState::Free);
	TestTrue(TEXT("the reset enemy accepts movement again"), Scene.EnemyCombat()->CanAcceptMovement());

	// A new hit request is processed again (recovery would refuse it).
	Scene.EnemyCombat()->NotifyHitReceived(M1_027_MakeStunHit(Scene.Enemy(), 0.3f));
	TestTrue(TEXT("a fresh stun request is accepted after the reset"),
		Scene.EnemyCombat()->GetSnapshot().ActionState == ECombatActionState::HitStun);
	return true;
}

// State 4 of the four-state acceptance: an enemy inside its Recovering phase
// resets straight back to Free, the attack entry reopens and no leftover
// recovery deadline fires.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_027ResetFromRecoveringStateRestoresFreeAndAttackable,
	"UEMMO.Tasks.M1_027.ResetFromRecoveringStateRestoresFreeAndAttackable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_027ResetFromRecoveringStateRestoresFreeAndAttackable::RunTest(const FString& Parameters)
{
	UWorld* World = M1_027_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_027_ResetScene Scene;
	if (!Scene.Build(*this, *World, /*EnemyCount*/ 1))
	{
		return true;
	}

	if (!TestTrue(TEXT("the landing recovery process starts (Knockdown)"),
		Scene.EnemyCombat()->BeginLandingRecovery(/*NowSeconds*/ 1.0)))
	{
		return true;
	}
	// Advance the injected clock past the knockdown end (1.45 s): the next
	// tick flips the process into its Recovering phase.
	Scene.EnemyCombat()->SetInputClockSeconds(/*NowSeconds*/ 1.5);
	M1_027_TickFrames(*Scene.EnemyCombat(), 1);
	TestTrue(TEXT("precondition: the enemy is inside its recovery"),
		Scene.EnemyCombat()->GetSnapshot().ActionState == ECombatActionState::Recovering
		&& Scene.EnemyCombat()->IsInLandingRecovery());

	Scene.Service->ResetTrainingSession();

	TestTrue(TEXT("the reset returned the recovering enemy to Free"),
		Scene.EnemyCombat()->GetSnapshot().ActionState == ECombatActionState::Free
		&& !Scene.EnemyCombat()->IsInLandingRecovery());
	Scene.EnemyCombat()->SetInputClockSeconds(/*NowSeconds*/ 50.0);
	M1_027_TickFrames(*Scene.EnemyCombat(), 2);
	TestTrue(TEXT("the old recovery deadline never fires after the reset"),
		Scene.EnemyCombat()->GetSnapshot().ActionState == ECombatActionState::Free);
	TestTrue(TEXT("the reset enemy can start a fresh attack again"),
		Scene.EnemyCombat()->TryStartAttack(FName(TEXT("light_01")), 1));
	return true;
}

// The death case: a killed enemy (dead flag, empty HP pool) comes back fully
// alive, Free and attackable after the unified reset; the registration set
// keeps its size.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_027ResetAfterDeathRevivesAliveFreeAndAttackable,
	"UEMMO.Tasks.M1_027.ResetAfterDeathRevivesAliveFreeAndAttackable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_027ResetAfterDeathRevivesAliveFreeAndAttackable::RunTest(const FString& Parameters)
{
	UWorld* World = M1_027_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_027_ResetScene Scene;
	if (!Scene.Build(*this, *World, /*EnemyCount*/ 1))
	{
		return true;
	}

	TestEqual(TEXT("a lethal hit removes the whole health pool"),
		Scene.Enemy()->GetHealthComponent()->ApplyDamage(100.0f), 100.0f);
	TestTrue(TEXT("precondition: the enemy is dead"),
		!Scene.Enemy()->GetHealthComponent()->IsAlive()
		&& Scene.EnemyCombat()->IsDead()
		&& FMath::IsNearlyEqual(Scene.EnemyHealth(), 0.0f));

	Scene.Service->ResetTrainingSession();

	TestTrue(TEXT("the reset revived the enemy"), Scene.Enemy()->GetHealthComponent()->IsAlive());
	TestEqual(TEXT("the reset restored the enemy HP to full"), Scene.EnemyHealth(), 100.0f);
	TestTrue(TEXT("the reset dropped the dead flag"), !Scene.EnemyCombat()->IsDead());
	TestTrue(TEXT("the reset returned the enemy to Free and movable"),
		Scene.EnemyCombat()->GetSnapshot().ActionState == ECombatActionState::Free
		&& Scene.EnemyCombat()->CanAcceptMovement());
	TestTrue(TEXT("the revived enemy can start a fresh attack again"),
		Scene.EnemyCombat()->TryStartAttack(FName(TEXT("light_01")), 1));
	TestEqual(TEXT("the reset kept the registration set unchanged"),
		Scene.Service->GetRegisteredEnemyCount(), 1);
	return true;
}

// The idempotency acceptance: ten consecutive resets never leak a buffered
// input, a stun, a knockdown deadline, an extra registration or an extra
// enemy actor, and the fresh attack instance ids minted after each reset stay
// strictly increasing and unique (the session counter is never rewound).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_027TenConsecutiveResetsLeaveNoResidueAndIdsStayUnique,
	"UEMMO.Tasks.M1_027.TenConsecutiveResetsLeaveNoResidueAndIdsStayUnique",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_027TenConsecutiveResetsLeaveNoResidueAndIdsStayUnique::RunTest(const FString& Parameters)
{
	UWorld* World = M1_027_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_027_ResetScene Scene;
	if (!Scene.Build(*this, *World, /*EnemyCount*/ 2))
	{
		return true;
	}

	// Duplicate registrations are ignored: re-registering both enemies and
	// re-registering the player never grows the participant set.
	Scene.Service->RegisterEnemy(Scene.Enemy(0));
	Scene.Service->RegisterEnemy(Scene.Enemy(1));
	Scene.Service->RegisterPlayer(Scene.Player);
	TestEqual(TEXT("duplicate registrations never grow the enemy set"),
		Scene.Service->GetRegisteredEnemyCount(), 2);

	uint64 PreviousInstanceId = 0;
	TSet<uint64> SeenInstanceIds;
	for (int32 Iteration = 0; Iteration < 10; ++Iteration)
	{
		// Dirty every participant: one buffered input plus a running attack
		// on the player, a fresh stun on enemy 0, a fresh knockdown process
		// on enemy 1.
		FBufferedCombatInput Intent;
		Intent.Sequence = static_cast<uint64>(1000 + Iteration);
		Intent.Action = ECombatInput::Light;
		Intent.PressedAt = 1.0 + Iteration;
		Scene.PlayerCombat->QueueInput(Intent);
		const uint64 PreResetInstanceId = Scene.PlayerCombat->GetSnapshot().InstanceId;
		if (!TestTrue(FString::Printf(TEXT("iteration %d: the player attack starts"), Iteration),
			Scene.PlayerCombat->TryStartAttack(FName(TEXT("light_01")), 1)))
		{
			return true;
		}
		Scene.EnemyCombat(0)->NotifyHitReceived(M1_027_MakeStunHit(Scene.Enemy(0), 0.5f));
		if (!TestTrue(FString::Printf(TEXT("iteration %d: the knockdown process starts on enemy 1"), Iteration),
			Scene.EnemyCombat(1)->BeginLandingRecovery(2.0 + Iteration)))
		{
			return true;
		}
		TestTrue(FString::Printf(TEXT("iteration %d: every participant is dirty before the reset"), Iteration),
			Scene.PlayerCombat->GetSnapshot().ActionState == ECombatActionState::Attacking
			&& Scene.PlayerCombat->GetSnapshot().BufferSize == 1
			&& Scene.EnemyCombat(0)->GetSnapshot().ActionState == ECombatActionState::HitStun
			&& Scene.EnemyCombat(1)->IsInLandingRecovery());

		Scene.Service->ResetTrainingSession();

		const FCombatSnapshot PlayerAfter = Scene.PlayerCombat->GetSnapshot();
		TestTrue(FString::Printf(TEXT("iteration %d: the player is Free with an empty buffer and no active id"), Iteration),
			PlayerAfter.ActionState == ECombatActionState::Free
			&& PlayerAfter.AttackId == NAME_None
			&& PlayerAfter.InstanceId == 0
			&& PlayerAfter.BufferSize == 0);
		TestTrue(FString::Printf(TEXT("iteration %d: enemy 0 left the stun"), Iteration),
			Scene.EnemyCombat(0)->GetSnapshot().ActionState == ECombatActionState::Free);
		TestTrue(FString::Printf(TEXT("iteration %d: enemy 1 left the knockdown"), Iteration),
			Scene.EnemyCombat(1)->GetSnapshot().ActionState == ECombatActionState::Free
			&& !Scene.EnemyCombat(1)->IsInLandingRecovery());

		// A fresh instance after the reset: its id must be strictly larger
		// than the pre-reset instance of the SAME iteration (the reset never
		// rewinds the session counter) and larger than every earlier one.
		if (!TestTrue(FString::Printf(TEXT("iteration %d: the player can start a fresh attack after the reset"), Iteration),
			Scene.PlayerCombat->TryStartAttack(FName(TEXT("light_01")), 1)))
		{
			return true;
		}
		const uint64 PostResetInstanceId = Scene.PlayerCombat->GetSnapshot().InstanceId;
		TestTrue(FString::Printf(TEXT("iteration %d: the post-reset instance id is new and nonzero"), Iteration),
			PostResetInstanceId > 0 && PostResetInstanceId != PreResetInstanceId);
		TestTrue(FString::Printf(TEXT("iteration %d: the post-reset instance id strictly increases"), Iteration),
			Iteration == 0 || PostResetInstanceId > PreviousInstanceId);
		TestTrue(FString::Printf(TEXT("iteration %d: the post-reset instance id is unique across the session"), Iteration),
			!SeenInstanceIds.Contains(PostResetInstanceId));
		SeenInstanceIds.Add(PostResetInstanceId);
		PreviousInstanceId = PostResetInstanceId;

		// Finish the instance so the next iteration can start a new attack.
		M1_027_TickFrames(*Scene.PlayerCombat, 26);
	}

	TestEqual(TEXT("ten resets ran exactly ten session resets"),
		Scene.Service->GetResetCount(), 10);
	TestEqual(TEXT("the enemy registration set never grew"), Scene.Service->GetRegisteredEnemyCount(), 2);
	int32 WorldEnemyCount = 0;
	for (TActorIterator<ATrainingEnemy> It(World); It; ++It)
	{
		++WorldEnemyCount;
	}
	TestEqual(TEXT("ten resets never spawned or removed an enemy actor"), WorldEnemyCount, 2);
	return true;
}

// The replay acceptance: after one light_01 drained the enemy, the unified
// reset restores full HP and the same first move lands again on the same
// enemy (fresh instance, HP exactly 100 - 10).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_027EnemyHittableAgainByFirstMoveAfterResetFromFullHealth,
	"UEMMO.Tasks.M1_027.EnemyHittableAgainByFirstMoveAfterResetFromFullHealth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_027EnemyHittableAgainByFirstMoveAfterResetFromFullHealth::RunTest(const FString& Parameters)
{
	UWorld* World = M1_027_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_027_ResetScene Scene;
	if (!Scene.Build(*this, *World, /*EnemyCount*/ 1))
	{
		return true;
	}

	// First light_01: exactly one accepted hit drains 10 HP.
	if (!Scene.HitEnemyWithLight(*this))
	{
		return true;
	}
	const uint64 FirstInstanceId = Scene.LastHitInstanceId;
	TestEqual(TEXT("the first move hit exactly once"), Scene.HitCount, 1);
	TestEqual(TEXT("the first move drained exactly 10 HP"), Scene.EnemyHealth(), 90.0f);

	// Finish the instance, then reset: HP back to full, both sides Free.
	M1_027_TickFrames(*Scene.PlayerCombat, 20);
	Scene.Service->ResetTrainingSession();
	TestEqual(TEXT("the reset restored the enemy HP to full"), Scene.EnemyHealth(), 100.0f);
	TestTrue(TEXT("the reset returned the enemy to Free"),
		Scene.EnemyCombat()->GetSnapshot().ActionState == ECombatActionState::Free);
	TestTrue(TEXT("the reset returned the player to Free"),
		Scene.PlayerCombat->GetSnapshot().ActionState == ECombatActionState::Free);

	// The same first move lands again on the same enemy from full HP.
	if (!Scene.HitEnemyWithLight(*this))
	{
		return true;
	}
	TestEqual(TEXT("the first move after the reset hit exactly once more"), Scene.HitCount, 2);
	TestEqual(TEXT("the post-reset first move drained exactly 10 HP from the full pool"), Scene.EnemyHealth(), 90.0f);
	TestTrue(TEXT("the post-reset hit came from a new, unique attack instance"),
		Scene.LastHitInstanceId != FirstInstanceId && Scene.LastHitInstanceId > 0);
	return true;
}

// The R-key wrap: one press runs exactly one session reset through the
// service (the reset counter proves no double execution) and resets both
// sides; a pawn without a registered service keeps the M0 local reset.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_027PlayerResetRoutesThroughServiceExactlyOnceAndFallsBack,
	"UEMMO.Tasks.M1_027.PlayerResetRoutesThroughServiceExactlyOnceAndFallsBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_027PlayerResetRoutesThroughServiceExactlyOnceAndFallsBack::RunTest(const FString& Parameters)
{
	UWorld* World = M1_027_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_027_ResetScene Scene;
	if (!Scene.Build(*this, *World, /*EnemyCount*/ 1))
	{
		return true;
	}
	TestEqual(TEXT("a fresh service has run no reset yet"), Scene.Service->GetResetCount(), 0);

	// Dirty both sides the way a live session would be dirty: a running
	// attack, a stunned enemy and displaced player physics.
	if (!TestTrue(TEXT("the player attack starts"), Scene.PlayerCombat->TryStartAttack(FName(TEXT("light_01")), 1)))
	{
		return true;
	}
	Scene.EnemyCombat()->NotifyHitReceived(M1_027_MakeStunHit(Scene.Enemy(), 0.5f));
	Scene.Player->SetActorLocation(M1_027_PlayerSpawn + FVector(500.0f, 0.0f, 0.0f), false, nullptr, ETeleportType::TeleportPhysics);
	Scene.PlayerMovement->Velocity = FVector(300.0f, 0.0f, 0.0f);
	Scene.Player->SetActorRotation(FRotator(0.0f, 180.0f, 0.0f));

	// The R-key binding target: the unified entry, exactly once.
	Scene.Player->ResetPosition();
	TestEqual(TEXT("one reset entry ran exactly one session reset"),
		Scene.Service->GetResetCount(), 1);
	TestTrue(TEXT("the R entry restored the player position"),
		Scene.Player->GetActorLocation().Equals(M1_027_PlayerSpawn, 0.01f));
	TestTrue(TEXT("the R entry zeroed the player velocity"), Scene.PlayerMovement->Velocity.IsZero());
	TestTrue(TEXT("the R entry restored the player facing"),
		Scene.Player->GetActorRotation().Equals(FRotator::ZeroRotator));
	TestTrue(TEXT("the R entry reset the player combat state"),
		Scene.PlayerCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestTrue(TEXT("the R entry reset the enemy through the same entry"),
		Scene.EnemyCombat()->GetSnapshot().ActionState == ECombatActionState::Free);

	// A second press stays idempotent: exactly one more reset, still clean.
	Scene.Player->ResetPosition();
	TestEqual(TEXT("a second reset entry ran exactly one more session reset"),
		Scene.Service->GetResetCount(), 2);
	TestTrue(TEXT("the repeated entry keeps the session clean"),
		Scene.PlayerCombat->GetSnapshot().ActionState == ECombatActionState::Free
		&& Scene.EnemyCombat()->GetSnapshot().ActionState == ECombatActionState::Free);

	// The service-less fallback: a bare pawn keeps the M0 local reset
	// (position and velocity only, no service interaction).
	FActorSpawnParameters BareParams;
	APrototypeCharacter* Bare = World->SpawnActor<APrototypeCharacter>(
		APrototypeCharacter::StaticClass(), M1_027_BarePlayerSpawn, FRotator::ZeroRotator, BareParams);
	if (!TestTrue(TEXT("the service-less player spawns"), Bare != nullptr))
	{
		return true;
	}
	if (!Bare->HasActorBegunPlay())
	{
		Bare->DispatchBeginPlay();
	}
	M1_027_ActivateMovement(Bare->GetCharacterMovement());
	Bare->SetActorLocation(M1_027_BarePlayerSpawn + FVector(-800.0f, 0.0f, 0.0f), false, nullptr, ETeleportType::TeleportPhysics);
	Bare->GetCharacterMovement()->Velocity = FVector(200.0f, 0.0f, 0.0f);
	Bare->ResetPosition();
	TestTrue(TEXT("the service-less reset restored the spawn position"),
		Bare->GetActorLocation().Equals(M1_027_BarePlayerSpawn, 0.01f));
	TestTrue(TEXT("the service-less reset zeroed the velocity"),
		Bare->GetCharacterMovement()->Velocity.IsZero());
	TestEqual(TEXT("the service-less reset never touched the session service"),
		Scene.Service->GetResetCount(), 2);
	return true;
}

#endif
