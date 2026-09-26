#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatGeometry.h"
#include "../Combat/CombatHitTypes.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/TrainingEnemy.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_026
{
	// Remote base so the temp world can never collide with arena content.
	// X horizontal, Y depth, Z height; the base is used as the feet origin of
	// the attacker (box actors root at their feet). Unique from the M1-018
	// through M1-025 bases so the suites never share a query space.
	const FVector M1_026_SceneBase(60000.0, 65000.0, 600.0);

	// Half extent of one attacker body box; the same shape the M1-019/M1-022/
	// M1-025 suites use, tall enough to sit fully inside the launcher hit box
	// (offset (95,0,90), half extent (85,50,70)).
	const FVector M1_026_BodyHalfExtent(20.0, 20.0, 95.0);

	// M1-026 recovery durations under test (interface contract section 6: a
	// launched landing builds one Knockdown 0.45 s -> Recovering 0.25 s ->
	// Free process, 0.70 s in total). The clock checkpoints around the
	// boundaries keep a 0.01 s margin so double rounding can never flip an
	// assertion; the Knockdown boundary at exactly 0.45 s is safe because the
	// landing time 0.0 + the constant is the same double as the literal.
	constexpr double M1_026_KnockdownSeconds = 0.45;
	constexpr double M1_026_RecoveryTotalSeconds = 0.70;

	// World acquisition, same order as the M1-018/M1-022/M1-025 pattern: a
	// private temp world first, the shared game world as fallback.
	static UWorld* M1_026_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M1_026_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M1_026 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M1_026 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	// Loads the real read-only catalog from Config/DefaultGame.ini (the same
	// loading path production and the earlier combat tests use). Returns null
	// after reporting the failure so the caller can bail out early.
	static UAttackCatalog* M1_026_NewCatalog(FAutomationTestBase& Test)
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

	// Spawns a plain actor with one query-enabled box root at Location and a
	// combat component attached to the catalog: the attacker side.
	static AActor* M1_026_SpawnAttacker(UWorld& World, const FVector& FeetLocation, UAttackCatalog* Catalog)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), FeetLocation, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M1_026_Body"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M1_026_BodyHalfExtent);
		Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Body->SetCollisionObjectType(ECC_Pawn);
		Body->SetCollisionResponseToAllChannels(ECR_Ignore);
		Body->RegisterComponent();
		Body->SetWorldLocation(FeetLocation);

		UCombatComponent* Combat = NewObject<UCombatComponent>(Actor, TEXT("M1_026_Combat"));
		Combat->RegisterComponent();
		if (Catalog != nullptr)
		{
			Combat->InitializeFromCatalog(Catalog);
		}
		return Actor;
	}

	// Advances the component by exactly Count single 1/60 s frames.
	static void M1_026_TickFrames(UCombatComponent& Combat, int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Combat.TickCombat(1.0f / 60.0f);
		}
	}

	// Applies exactly the two pending-velocity steps of one real movement
	// update, in PerformMovement order (ApplyAccumulatedForces, then
	// HandlePendingLaunch), without a world tick - the same proven pattern
	// the M1-022/M1-025 suites use for temp worlds that never integrate
	// physics.
	static void M1_026_DrivePendingVelocity(UCharacterMovementComponent& Movement)
	{
		Movement.ApplyAccumulatedForces(1.0f / 60.0f);
		Movement.HandlePendingLaunch();
	}

	// One synthetic victim-side hit for the NotifyHitReceived entry, in the
	// M1-020 pattern: only the fields the victim entry consumes are filled
	// (target identity and the requested stun); it never deducts health.
	static FCombatHit M1_026_MakeStunHit(AActor* Victim, float StunSeconds)
	{
		FCombatHit Hit;
		Hit.Target = Victim;
		Hit.StunSeconds = StunSeconds;
		return Hit;
	}

	// Drives the recovery state machine to its settled Free state after one
	// clock jump past both deadlines: the Knockdown -> Recovering and
	// Recovering -> Free transitions are checked on successive TickCombat
	// calls (exactly like the per-frame game loop), so one jump needs two
	// ticks to land on Free.
	static void M1_026_SettleRecoveryToFree(UCombatComponent& Combat)
	{
		Combat.TickCombat(1.0f / 60.0f);
		Combat.TickCombat(1.0f / 60.0f);
	}

	// One scene: the real catalog, an attacker box actor at the scene base and
	// one training enemy spawned at the launcher hit box center for the
	// requested facing. The launcher and light_01 boxes share the same
	// geometry (asserted in Build), so one enemy position serves both attacks.
	struct FM1_026_LandingScene
	{
		UAttackCatalog* Catalog = nullptr;
		const UAttackDefinition* LauncherDefinition = nullptr;
		const UAttackDefinition* LightDefinition = nullptr;
		AActor* Attacker = nullptr;
		UCombatComponent* Combat = nullptr;
		ATrainingEnemy* Enemy = nullptr;
		UCombatComponent* EnemyCombat = nullptr;
		UCharacterMovementComponent* EnemyMovementRaw = nullptr;
		FVector BoxCenter = FVector::ZeroVector;
		int32 HitCount = 0;

		// Builds attacker + enemy and asserts the attack source values the
		// acceptance criteria are phrased against. Returns false after
		// reporting the problem so the caller can bail out early.
		bool Build(FAutomationTestBase& Test, UWorld& World, int32 Facing)
		{
			Catalog = M1_026_NewCatalog(Test);
			if (Catalog == nullptr)
			{
				return false;
			}
			LauncherDefinition = Catalog->Find(FName(TEXT("launcher")));
			if (!Test.TestTrue(TEXT("the catalog holds launcher"), LauncherDefinition != nullptr))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("launcher keeps the design initial values (duration 40, window [12,17), damage 18, knockback 70, launch 700)"),
				LauncherDefinition->DurationFrames == 40
				&& LauncherDefinition->ActiveWindow.StartFrame == 12 && LauncherDefinition->ActiveWindow.EndFrame == 17
				&& FMath::IsNearlyEqual(LauncherDefinition->BaseDamage, 18.0f)
				&& FMath::IsNearlyEqual(LauncherDefinition->KnockbackSpeed, 70.0f)
				&& FMath::IsNearlyEqual(LauncherDefinition->LaunchSpeed, 700.0f)))
			{
				return false;
			}
			LightDefinition = Catalog->Find(FName(TEXT("light_01")));
			if (!Test.TestTrue(TEXT("the catalog holds light_01"), LightDefinition != nullptr))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("light_01 keeps the design initial values (duration 26, window [7,11), damage 10, knockback 90, launch 0, stun 0.22)"),
				LightDefinition->DurationFrames == 26
				&& LightDefinition->ActiveWindow.StartFrame == 7 && LightDefinition->ActiveWindow.EndFrame == 11
				&& FMath::IsNearlyEqual(LightDefinition->BaseDamage, 10.0f)
				&& FMath::IsNearlyEqual(LightDefinition->KnockbackSpeed, 90.0f)
				&& FMath::IsNearlyEqual(LightDefinition->LaunchSpeed, 0.0f)
				&& FMath::IsNearlyEqual(LightDefinition->HitStunSeconds, 0.22f, 1.0e-6f)))
			{
				return false;
			}
			// The light attack must reach the same enemy position as the
			// launcher (shared box geometry) and must never launch a grounded
			// victim (LaunchSpeed 0 keeps the launched-airborne flag false),
			// which the plain-landing case relies on.
			if (!Test.TestTrue(TEXT("launcher and light_01 share the hit box geometry"),
				ComputeHitBox(M1_026_SceneBase, Facing, *LauncherDefinition).Center
				.Equals(ComputeHitBox(M1_026_SceneBase, Facing, *LightDefinition).Center, 1e-4f)))
			{
				return false;
			}

			Attacker = M1_026_SpawnAttacker(World, M1_026_SceneBase, Catalog);
			if (!Test.TestNotNull(TEXT("the attacker spawns"), Attacker))
			{
				return false;
			}
			Combat = Attacker->FindComponentByClass<UCombatComponent>();
			if (!Test.TestTrue(TEXT("the attacker carries a combat component"), Combat != nullptr))
			{
				return false;
			}
			Combat->OnHitConfirmed.AddLambda([this](const FCombatHit&)
			{
				++HitCount;
			});

			// The enemy capsule center sits at the hit box center for this
			// facing (feet at center - 88), so both attack boxes cover it.
			BoxCenter = ComputeHitBox(M1_026_SceneBase, Facing, *LauncherDefinition).Center;
			FActorSpawnParameters SpawnParams;
			Enemy = World.SpawnActor<ATrainingEnemy>(
				ATrainingEnemy::StaticClass(), BoxCenter, FRotator::ZeroRotator, SpawnParams);
			if (!Test.TestNotNull(TEXT("the training enemy spawns"), Enemy))
			{
				return false;
			}
			// BeginPlay owns the death wiring; temp worlds may not have begun
			// play, so dispatch it once explicitly and never twice (M1-020
			// pattern).
			if (!Enemy->HasActorBegunPlay())
			{
				Enemy->DispatchBeginPlay();
			}
			// Temp worlds never reach AreActorsInitialized, so the spawned
			// dummy skips the engine's component initialization steps. Mirror
			// exactly the two steps that matter (the M1-022 pattern): activate
			// the movement component and initialize the default movement mode,
			// or every AddImpulse/Launch is silently dropped.
			EnemyMovementRaw = Enemy->GetCharacterMovement();
			if (EnemyMovementRaw != nullptr)
			{
				if (!EnemyMovementRaw->IsActive())
				{
					EnemyMovementRaw->Activate(/*bReset*/ true);
				}
				if (EnemyMovementRaw->MovementMode == MOVE_None)
				{
					EnemyMovementRaw->SetDefaultMovementMode();
				}
			}
			EnemyCombat = Enemy->FindComponentByClass<UCombatComponent>();
			if (!Test.TestTrue(TEXT("the training enemy carries its own combat component"), EnemyCombat != nullptr))
			{
				return false;
			}
			return Test.TestTrue(TEXT("the training enemy starts alive at full health"),
				Enemy->GetHealthComponent() != nullptr && Enemy->GetHealthComponent()->IsAlive()
				&& FMath::IsNearlyEqual(Enemy->GetHealthComponent()->GetHealth(), 100.0f));
		}

		// Starts one launcher instance with the requested facing, ticks exactly
		// to its first active frame (12) where the hit lands, and applies the
		// pending-velocity steps so the launch is live on the target.
		bool HitWithLauncher(FAutomationTestBase& Test, int32 Facing)
		{
			if (!Test.TestTrue(TEXT("the launcher attack starts"), Combat->TryStartAttack(FName(TEXT("launcher")), Facing)))
			{
				return false;
			}
			M1_026_TickFrames(*Combat, 13);
			M1_026_DrivePendingVelocity(*EnemyMovementRaw);
			return true;
		}

		// Ticks the remaining launcher frames so the attacker is Free for the
		// next instance.
		void FinishLauncherInstance()
		{
			M1_026_TickFrames(*Combat, 27);
		}

		// Starts one light_01 instance with the requested facing, ticks exactly
		// to its first active frame (7) where the hit lands, and applies the
		// pending-velocity steps.
		bool HitWithLight(FAutomationTestBase& Test, int32 Facing)
		{
			if (!Test.TestTrue(TEXT("the light_01 attack starts"), Combat->TryStartAttack(FName(TEXT("light_01")), Facing)))
			{
				return false;
			}
			M1_026_TickFrames(*Combat, 8);
			M1_026_DrivePendingVelocity(*EnemyMovementRaw);
			return true;
		}

		// Ticks the remaining light_01 frames so the attacker is Free for the
		// next instance.
		void FinishLightInstance()
		{
			M1_026_TickFrames(*Combat, 18);
		}

		float EnemyHealth() const
		{
			const UHealthComponent* Health = Enemy->GetHealthComponent();
			return (Health != nullptr) ? Health->GetHealth() : -1.0f;
		}

		bool EnemyAlive() const
		{
			const UHealthComponent* Health = Enemy->GetHealthComponent();
			return (Health != nullptr) && Health->IsAlive();
		}
	};
}

using namespace UE::UEMMO::Tasks::M1_026;

// Core acceptance case: a launched (hit airborne) enemy that lands builds
// exactly one Knockdown 0.45 s -> Recovering 0.25 s -> Free process on the
// injected input clock. The float cycle stays open through the whole process
// and reopens (LauncherCycleCount = 0) only at the Recovering -> Free
// transition - the fourth air-combo policy clear point.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_026LaunchedLandingBuildsOneKnockdownRecoveryProcess,
	"UEMMO.Tasks.M1_026.LaunchedLandingBuildsOneKnockdownRecoveryProcess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_026LaunchedLandingBuildsOneKnockdownRecoveryProcess::RunTest(const FString& Parameters)
{
	UWorld* World = M1_026_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_026_LandingScene Scene;
	if (!Scene.Build(*this, *World, /*Facing*/ 1))
	{
		return true;
	}

	TestTrue(TEXT("precondition: the fresh enemy is Free"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual(TEXT("precondition: the fresh enemy carries no float cycle"), Scene.Enemy->GetLauncherCycleCount(), 0);

	// The launcher hit lifts the enemy: the hit is accepted (damage 18), the
	// float cycle opens at 1 and the enemy reads airborne.
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("the launcher hit the enemy exactly once"), Scene.HitCount, 1);
	TestEqual(TEXT("the accepted launch hit removed exactly the launcher base damage"), Scene.EnemyHealth(), 82.0f);
	TestEqual(TEXT("the applied launch opened the float cycle at 1"), Scene.Enemy->GetLauncherCycleCount(), 1);
	TestTrue(TEXT("precondition: the launched enemy is airborne"),
		Scene.Enemy->GetAirState() != ECombatAirState::Grounded);

	// The launched landing opens the recovery process: Knockdown first.
	Scene.Enemy->NotifyLanded(0.0);
	TestTrue(TEXT("the launched landing knocked the enemy down"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);
	TestTrue(TEXT("the knocked-down enemy reports the landing recovery"),
		Scene.EnemyCombat->IsInLandingRecovery());
	TestEqual(TEXT("the float cycle stays open through the knockdown"), Scene.Enemy->GetLauncherCycleCount(), 1);

	// 0.44 s on the input clock: still down. Exactly 0.45 s: getting up.
	Scene.EnemyCombat->SetInputClockSeconds(0.0);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("ticking at the landing time keeps the knockdown"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);
	Scene.EnemyCombat->SetInputClockSeconds(M1_026_KnockdownSeconds - 0.01);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("just before 0.45 s the enemy is still knocked down"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);
	Scene.EnemyCombat->SetInputClockSeconds(M1_026_KnockdownSeconds);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("at 0.45 s the knockdown turned into Recovering"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Recovering);
	TestEqual(TEXT("the float cycle still reads open during Recovering"), Scene.Enemy->GetLauncherCycleCount(), 1);

	// 0.69 s: still recovering. 0.71 s (past the 0.70 s total): Free again.
	Scene.EnemyCombat->SetInputClockSeconds(M1_026_RecoveryTotalSeconds - 0.01);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("just before 0.70 s the enemy is still Recovering"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Recovering);
	Scene.EnemyCombat->SetInputClockSeconds(M1_026_RecoveryTotalSeconds + 0.01);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("past the 0.70 s total the enemy is Free again"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestFalse(TEXT("the recovered enemy no longer reports the landing recovery"),
		Scene.EnemyCombat->IsInLandingRecovery());
	TestEqual(TEXT("the completed recovery reopened the float cycle (the fourth clear point)"),
		Scene.Enemy->GetLauncherCycleCount(), 0);
	return true;
}

// One landing event builds exactly one process: duplicate Landed notifies and
// direct recovery entries while a process runs never restart the timers - the
// Recovering flip still happens at 0.45 s after the FIRST landing and the
// total still ends at 0.70 s.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_026DuplicateLandedNeverRestartsTheProcess,
	"UEMMO.Tasks.M1_026.DuplicateLandedNeverRestartsTheProcess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_026DuplicateLandedNeverRestartsTheProcess::RunTest(const FString& Parameters)
{
	UWorld* World = M1_026_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_026_LandingScene Scene;
	if (!Scene.Build(*this, *World, /*Facing*/ 1))
	{
		return true;
	}

	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	Scene.Enemy->NotifyLanded(0.0);
	TestTrue(TEXT("precondition: the launched landing knocked the enemy down"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);

	// Bounce-style duplicate landing events never restart the process.
	Scene.Enemy->NotifyLanded(0.1);
	TestTrue(TEXT("the duplicate landing kept the knockdown"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);
	Scene.Enemy->NotifyLanded(0.2);
	TestTrue(TEXT("the second duplicate landing kept the knockdown"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);
	// The component-level entry refuses a second process while one runs.
	TestFalse(TEXT("the direct recovery entry is refused while a process runs"),
		Scene.EnemyCombat->BeginLandingRecovery(0.2));
	TestTrue(TEXT("the refused entry left the knockdown untouched"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);

	// The timers still belong to the first landing: Recovering at exactly
	// 0.45 s (a restart at 0.2 s would keep the enemy down past 0.45 s).
	Scene.EnemyCombat->SetInputClockSeconds(M1_026_KnockdownSeconds);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("the duplicate landings never pushed the 0.45 s Recovering flip away"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Recovering);
	TestFalse(TEXT("the direct recovery entry is refused while Recovering"),
		Scene.EnemyCombat->BeginLandingRecovery(M1_026_KnockdownSeconds + 0.05));

	// Multi-frame ticking inside Recovering never restarts or extends it.
	Scene.EnemyCombat->SetInputClockSeconds(M1_026_KnockdownSeconds + 0.05);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("ticking inside Recovering keeps the process (no restart)"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Recovering);
	Scene.EnemyCombat->SetInputClockSeconds(M1_026_RecoveryTotalSeconds + 0.01);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("the process still ends at the 0.70 s total"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	return true;
}

// The recovery refuses hits in the fullest sense: during Knockdown an
// attacker's accepted-geometry hit produces no damage, no HitConfirmed
// broadcast, no impulse and no stun takeover; after the recovery completes a
// fresh hit lands normally.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_026RecoveryRefusesHitsUntilFreeAgain,
	"UEMMO.Tasks.M1_026.RecoveryRefusesHitsUntilFreeAgain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_026RecoveryRefusesHitsUntilFreeAgain::RunTest(const FString& Parameters)
{
	UWorld* World = M1_026_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_026_LandingScene Scene;
	if (!Scene.Build(*this, *World, /*Facing*/ 1))
	{
		return true;
	}

	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("precondition: the launcher hit landed once"), Scene.HitCount, 1);
	TestEqual(TEXT("precondition: the enemy took the launcher damage"), Scene.EnemyHealth(), 82.0f);
	// Retire the launcher instance before the light attack: the attacker runs
	// one action at a time and its launcher is still mid-timeline here.
	Scene.FinishLauncherInstance();
	Scene.Enemy->NotifyLanded(0.0);
	TestTrue(TEXT("precondition: the launched landing knocked the enemy down"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);

	// A light_01 hit during the knockdown is refused in the fullest sense.
	const FVector VelocityBefore = Scene.EnemyMovementRaw->Velocity;
	if (!Scene.HitWithLight(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("the hit during the knockdown produced no HitConfirmed"), Scene.HitCount, 1);
	TestEqual(TEXT("the refused hit deducted no health"), Scene.EnemyHealth(), 82.0f);
	TestTrue(TEXT("the refused hit added no impulse"),
		Scene.EnemyMovementRaw->Velocity.Equals(VelocityBefore, 0.01f));
	TestTrue(TEXT("the refused hit never stunned the victim out of its knockdown"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);
	Scene.FinishLightInstance();

	// After the recovery completes the enemy is Free and hittable again.
	Scene.EnemyCombat->SetInputClockSeconds(M1_026_RecoveryTotalSeconds + 0.01);
	M1_026_SettleRecoveryToFree(*Scene.EnemyCombat);
	TestTrue(TEXT("precondition: the recovery returned the enemy to Free"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	if (!Scene.HitWithLight(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("the hit after the recovery was accepted"), Scene.HitCount, 2);
	TestEqual(TEXT("the accepted hit deducted the light base damage"), Scene.EnemyHealth(), 72.0f);
	TestTrue(TEXT("the accepted hit added its horizontal knockback"),
		FMath::IsNearlyEqual(Scene.EnemyMovementRaw->Velocity.X,
			Scene.LauncherDefinition->KnockbackSpeed + Scene.LightDefinition->KnockbackSpeed, 0.01f));
	return true;
}

// A dead enemy never recovers: the launched-landing path refuses the recovery
// entry for a dead combatant, the health pool stays gone and no tick ever
// revives it (death > landing recovery priority).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_026DeadEnemyLandingNeverRecovers,
	"UEMMO.Tasks.M1_026.DeadEnemyLandingNeverRecovers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_026DeadEnemyLandingNeverRecovers::RunTest(const FString& Parameters)
{
	UWorld* World = M1_026_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_026_LandingScene Scene;
	if (!Scene.Build(*this, *World, /*Facing*/ 1))
	{
		return true;
	}

	// The enemy is launched (the flag that would owe a knockdown), then killed
	// through the only damage entry point before it lands.
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	TestTrue(TEXT("precondition: the launched enemy is airborne"),
		Scene.Enemy->GetAirState() != ECombatAirState::Grounded);
	Scene.Enemy->GetHealthComponent()->ApplyDamage(100.0f);
	TestFalse(TEXT("precondition: the enemy is dead"), Scene.EnemyAlive());
	TestTrue(TEXT("precondition: the lethal hit marked the combat component dead"), Scene.EnemyCombat->IsDead());
	TestEqual(TEXT("precondition: the death cleared the float cycle"), Scene.Enemy->GetLauncherCycleCount(), 0);

	// The landing of the dead enemy builds no recovery process.
	Scene.Enemy->NotifyLanded(1.0);
	TestTrue(TEXT("the dead enemy's landing never entered Knockdown"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestFalse(TEXT("the dead enemy never reports a landing recovery"),
		Scene.EnemyCombat->IsInLandingRecovery());
	TestFalse(TEXT("the direct recovery entry is refused for a dead combatant"),
		Scene.EnemyCombat->BeginLandingRecovery(1.0));
	TestTrue(TEXT("the dead enemy stays dead"), Scene.EnemyCombat->IsDead() && !Scene.EnemyAlive());
	TestEqual(TEXT("the dead enemy's health pool stays at zero"), Scene.EnemyHealth(), 0.0f);

	// Advancing the clock never revives the dead enemy.
	Scene.EnemyCombat->SetInputClockSeconds(2.0);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("the clock advance neither revived the enemy nor started a recovery"),
		Scene.EnemyCombat->IsDead() && !Scene.EnemyAlive()
		&& Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	return true;
}

// A plain landing (the enemy was never hit airborne) never knocks down: a
// fresh enemy stays Free through its landing, and a grounded light-hit victim
// keeps its ordinary hit stun through a landing instead of exchanging it for
// a knockdown.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_026PlainLandingNeverKnocksDown,
	"UEMMO.Tasks.M1_026.PlainLandingNeverKnocksDown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_026PlainLandingNeverKnocksDown::RunTest(const FString& Parameters)
{
	UWorld* World = M1_026_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_026_LandingScene Scene;
	if (!Scene.Build(*this, *World, /*Facing*/ 1))
	{
		return true;
	}

	// Part 1: a fresh enemy (never launched) lands - no knockdown, no cycle.
	TestEqual(TEXT("precondition: the fresh enemy carries no float cycle"), Scene.Enemy->GetLauncherCycleCount(), 0);
	Scene.Enemy->NotifyLanded(0.0);
	TestTrue(TEXT("the plain landing left the fresh enemy Free"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestFalse(TEXT("the plain landing built no recovery process"),
		Scene.EnemyCombat->IsInLandingRecovery());
	TestEqual(TEXT("the plain landing opened no float cycle"), Scene.Enemy->GetLauncherCycleCount(), 0);

	// Part 2: a grounded light hit stuns the enemy (LaunchSpeed 0 never sets
	// the launched-airborne flag); a later plain landing keeps the ordinary
	// stun instead of knocking the enemy down.
	if (!Scene.HitWithLight(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("precondition: the grounded light hit landed once"), Scene.HitCount, 1);
	TestTrue(TEXT("precondition: the light hit stunned the enemy"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::HitStun);
	Scene.Enemy->NotifyLanded(0.5);
	TestTrue(TEXT("the plain landing during a stun kept the hit stun (no knockdown)"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::HitStun);
	TestFalse(TEXT("the plain landing during a stun built no recovery process"),
		Scene.EnemyCombat->IsInLandingRecovery());
	Scene.EnemyCombat->SetInputClockSeconds(0.22);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("the stun still ended on its own schedule"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the whole plain flow opened no float cycle"), Scene.Enemy->GetLauncherCycleCount(), 0);
	return true;
}

// The recovery completion reopens the air-combo policy: after the 0.70 s
// process the float cycle is cleared, so the next launcher rises at the full
// 700 cm/s definition speed instead of the decayed 490 cm/s second-launch
// scale.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_026RecoveryCompletionReopensLauncherAtFullSpeed,
	"UEMMO.Tasks.M1_026.RecoveryCompletionReopensLauncherAtFullSpeed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_026RecoveryCompletionReopensLauncherAtFullSpeed::RunTest(const FString& Parameters)
{
	UWorld* World = M1_026_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_026_LandingScene Scene;
	if (!Scene.Build(*this, *World, /*Facing*/ 1))
	{
		return true;
	}

	// First launcher: the float cycle opens at 1 and the landing keeps it open
	// through the recovery process.
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("precondition: the first launcher hit landed"), Scene.HitCount, 1);
	TestEqual(TEXT("precondition: the float cycle opened at 1"), Scene.Enemy->GetLauncherCycleCount(), 1);
	Scene.Enemy->NotifyLanded(0.0);
	TestTrue(TEXT("precondition: the launched landing knocked the enemy down"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);
	TestEqual(TEXT("precondition: the float cycle stays open during the recovery"),
		Scene.Enemy->GetLauncherCycleCount(), 1);
	Scene.EnemyCombat->SetInputClockSeconds(M1_026_RecoveryTotalSeconds + 0.01);
	M1_026_SettleRecoveryToFree(*Scene.EnemyCombat);
	TestTrue(TEXT("precondition: the recovery returned the enemy to Free"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the completed recovery cleared the float cycle"), Scene.Enemy->GetLauncherCycleCount(), 0);

	// The recovery took 0.70 s, so the first launch's rise has decayed away;
	// the temp world never integrates physics, so the decay is injected
	// (the M1-025 pattern) before the next launcher.
	Scene.FinishLauncherInstance();
	Scene.EnemyMovementRaw->Velocity = FVector::ZeroVector;

	// The next launcher is the first of a fresh cycle: full 700 cm/s, not the
	// decayed 490 cm/s a still-open cycle would produce.
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("the post-recovery launcher hit was accepted"), Scene.HitCount, 2);
	TestTrue(TEXT("the post-recovery launcher rose at the full 700 cm/s definition speed"),
		FMath::IsNearlyEqual(Scene.EnemyMovementRaw->Velocity.Z, Scene.LauncherDefinition->LaunchSpeed, 0.01f));
	TestEqual(TEXT("the fresh float cycle opened at 1"), Scene.Enemy->GetLauncherCycleCount(), 1);
	TestEqual(TEXT("the post-recovery launcher removed exactly its base damage"), Scene.EnemyHealth(), 64.0f);
	return true;
}

// The landing recovery and the hit stun are mutually exclusive with the
// death > knockdown/recovering > hit stun priority: a stun request during the
// recovery is refused without touching the process, and a landing during an
// existing stun replaces the stun with the knockdown.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_026HitStunAndKnockdownAreMutuallyExclusive,
	"UEMMO.Tasks.M1_026.HitStunAndKnockdownAreMutuallyExclusive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_026HitStunAndKnockdownAreMutuallyExclusive::RunTest(const FString& Parameters)
{
	UWorld* World = M1_026_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_026_LandingScene Scene;
	if (!Scene.Build(*this, *World, /*Facing*/ 1))
	{
		return true;
	}

	// Part 1: a stun request during the knockdown is refused entirely - the
	// process keeps its original timers and never becomes a stun.
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	Scene.Enemy->NotifyLanded(0.0);
	TestTrue(TEXT("precondition: the launched landing knocked the enemy down"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);
	Scene.EnemyCombat->SetInputClockSeconds(0.0);
	Scene.EnemyCombat->NotifyHitReceived(M1_026_MakeStunHit(Scene.Enemy, 0.5f));
	TestTrue(TEXT("the stun request during the knockdown did not take over"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);
	Scene.EnemyCombat->SetInputClockSeconds(M1_026_KnockdownSeconds - 0.01);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("the refused stun neither extended nor restarted the knockdown"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);
	Scene.EnemyCombat->SetInputClockSeconds(M1_026_KnockdownSeconds);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("the knockdown still flipped to Recovering on schedule"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Recovering);
	Scene.EnemyCombat->SetInputClockSeconds(M1_026_RecoveryTotalSeconds + 0.01);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("the recovery still completed to Free"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the completed recovery cleared the float cycle"), Scene.Enemy->GetLauncherCycleCount(), 0);

	// Part 2: a hit stun on the airborne enemy is replaced by the landing's
	// knockdown (knockdown > hit stun), and the process runs its full course.
	Scene.FinishLauncherInstance();
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("precondition: the second launcher hit landed"), Scene.HitCount, 2);
	Scene.EnemyCombat->SetInputClockSeconds(1.0);
	Scene.EnemyCombat->NotifyHitReceived(M1_026_MakeStunHit(Scene.Enemy, 0.5f));
	TestTrue(TEXT("precondition: the airborne enemy is stunned"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::HitStun);
	Scene.Enemy->NotifyLanded(1.1);
	TestTrue(TEXT("the landing's knockdown replaced the hit stun"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);
	TestTrue(TEXT("the replaced state reports the landing recovery"),
		Scene.EnemyCombat->IsInLandingRecovery());
	// The new process timers run from the landing at 1.1 (flip 1.55, end 1.8).
	Scene.EnemyCombat->SetInputClockSeconds(1.54);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("the replaced knockdown still runs at 1.54 s"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);
	Scene.EnemyCombat->SetInputClockSeconds(1.56);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("the replaced knockdown flipped to Recovering at 1.55 s"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Recovering);
	Scene.EnemyCombat->SetInputClockSeconds(1.79);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("the replaced process is still Recovering before its 1.8 s end"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Recovering);
	Scene.EnemyCombat->SetInputClockSeconds(1.81);
	Scene.EnemyCombat->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("the replaced process completed to Free at 1.8 s"),
		Scene.EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the second recovery cleared the float cycle"), Scene.Enemy->GetLauncherCycleCount(), 0);
	return true;
}

#endif
