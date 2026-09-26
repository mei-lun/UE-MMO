#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/AirComboPolicy.h"
#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatGeometry.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/TrainingEnemy.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_025
{
	// Remote base so the temp world can never collide with arena content.
	// X horizontal, Y depth, Z height; the base is the feet origin of the
	// attacker (box actors root at their feet).
	const FVector M1_025_SceneBase(50000.0, 55000.0, 600.0);

	// Half extent of one attacker body box; the same shape the M1-019/M1-022
	// suites use, tall enough to sit fully inside the launcher hit box
	// (offset (95,0,90), half extent (85,50,70)).
	const FVector M1_025_BodyHalfExtent(20.0, 20.0, 95.0);

	// Simulated mid-float vertical speed injected between the first and the
	// second launcher hit. The real second launcher can only land after the
	// cancel window (frame 18, 0.3 s) plus the follow-up instance's active
	// frame, by which time the first launch (700 cm/s) has decayed well below
	// the scaled 490 cm/s second launch (kinematics: Z = 700 - 980 * t). The
	// injection reproduces exactly that regime without ticking physics.
	const float M1_025_MidFloatRiseSpeed = 300.0f;

	// World acquisition, same order as the M1-018/M1-022 pattern: a private
	// temp world first, the shared game world as fallback.
	static UWorld* M1_025_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M1_025_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M1_025 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M1_025 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	// Loads the real read-only catalog from Config/DefaultGame.ini (the same
	// loading path production and the earlier combat tests use). Returns null
	// after reporting the failure so the caller can bail out early.
	static UAttackCatalog* M1_025_NewCatalog(FAutomationTestBase& Test)
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
	static AActor* M1_025_SpawnAttacker(UWorld& World, const FVector& FeetLocation, UAttackCatalog* Catalog)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), FeetLocation, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M1_025_Body"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M1_025_BodyHalfExtent);
		Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Body->SetCollisionObjectType(ECC_Pawn);
		Body->SetCollisionResponseToAllChannels(ECR_Ignore);
		Body->RegisterComponent();
		Body->SetWorldLocation(FeetLocation);

		UCombatComponent* Combat = NewObject<UCombatComponent>(Actor, TEXT("M1_025_Combat"));
		Combat->RegisterComponent();
		if (Catalog != nullptr)
		{
			Combat->InitializeFromCatalog(Catalog);
		}
		return Actor;
	}

	// Advances the component by exactly Count single 1/60 s frames.
	static void M1_025_TickFrames(UCombatComponent& Combat, int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Combat.TickCombat(1.0f / 60.0f);
		}
	}

	// Applies exactly the two pending-velocity steps of one real movement
	// update, in PerformMovement order (ApplyAccumulatedForces, then
	// HandlePendingLaunch), without a world tick - the same proven pattern
	// the M1-022 suite uses for temp worlds that never integrate physics.
	static void M1_025_DrivePendingVelocity(UCharacterMovementComponent& Movement)
	{
		Movement.ApplyAccumulatedForces(1.0f / 60.0f);
		Movement.HandlePendingLaunch();
	}

	// One scene: the real catalog, an attacker box actor at the scene base and
	// one training enemy spawned at the launcher hit box center for the
	// requested facing (the launcher box matches light_01 geometry: offset
	// (95,0,90), half extent (85,50,70)).
	struct FM1_025_LaunchScene
	{
		UAttackCatalog* Catalog = nullptr;
		const UAttackDefinition* Definition = nullptr;
		AActor* Attacker = nullptr;
		UCombatComponent* Combat = nullptr;
		ATrainingEnemy* Enemy = nullptr;
		FVector BoxCenter = FVector::ZeroVector;
		int32 HitCount = 0;

		// Builds attacker + enemy and asserts the launcher source values the
		// acceptance criteria are phrased against. Returns false after
		// reporting the problem so the caller can bail out early.
		bool Build(FAutomationTestBase& Test, UWorld& World, int32 Facing)
		{
			Catalog = M1_025_NewCatalog(Test);
			if (Catalog == nullptr)
			{
				return false;
			}
			Definition = Catalog->Find(FName(TEXT("launcher")));
			if (!Test.TestTrue(TEXT("the catalog holds launcher"), Definition != nullptr))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("launcher keeps the design initial values (duration 40, window [12,17), damage 18, knockback 70, launch 700)"),
				Definition->DurationFrames == 40
				&& Definition->ActiveWindow.StartFrame == 12 && Definition->ActiveWindow.EndFrame == 17
				&& FMath::IsNearlyEqual(Definition->BaseDamage, 18.0f)
				&& FMath::IsNearlyEqual(Definition->KnockbackSpeed, 70.0f)
				&& FMath::IsNearlyEqual(Definition->LaunchSpeed, 700.0f)))
			{
				return false;
			}

			Attacker = M1_025_SpawnAttacker(World, M1_025_SceneBase, Catalog);
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
			// facing (feet at center - 88), so the launcher box covers it.
			BoxCenter = ComputeHitBox(M1_025_SceneBase, Facing, *Definition).Center;
			FActorSpawnParameters SpawnParams;
			Enemy = World.SpawnActor<ATrainingEnemy>(
				ATrainingEnemy::StaticClass(), BoxCenter, FRotator::ZeroRotator, SpawnParams);
			if (!Test.TestNotNull(TEXT("the training enemy spawns"), Enemy))
			{
				return false;
			}
			// BeginPlay owns death wiring; temp worlds may not have begun play,
			// so dispatch it once explicitly and never twice (M1-020 pattern).
			if (!Enemy->HasActorBegunPlay())
			{
				Enemy->DispatchBeginPlay();
			}
			// A temp world never reaches AreActorsInitialized, so spawned
			// actors skip the movement component activation a begun world
			// performs; mirror the two engine steps exactly (M1-022 pattern)
			// or every AddImpulse/Launch is silently dropped.
			UCharacterMovementComponent* EnemyMovementRaw = Enemy->GetCharacterMovement();
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
			return Test.TestTrue(TEXT("the training enemy starts alive at full health with a fresh float cycle"),
				Enemy->GetHealthComponent() != nullptr && Enemy->GetHealthComponent()->IsAlive()
				&& FMath::IsNearlyEqual(Enemy->GetHealthComponent()->GetHealth(), 100.0f)
				&& Enemy->GetLauncherCycleCount() == 0);
		}

		// Starts one launcher instance with the requested facing and ticks
		// exactly to its first active frame (12), where the hit lands.
		bool HitWithLauncher(FAutomationTestBase& Test, int32 Facing)
		{
			if (!Test.TestTrue(TEXT("the launcher attack starts"), Combat->TryStartAttack(FName(TEXT("launcher")), Facing)))
			{
				return false;
			}
			M1_025_TickFrames(*Combat, 13);
			return true;
		}

		// Ticks the remaining launcher frames after the first active frame so
		// the attacker is Free for the next instance (the instance's own dedup
		// keeps any further active-window frame from hitting again).
		void FinishLauncherInstance()
		{
			M1_025_TickFrames(*Combat, 27);
		}

		// Simulates the mid-float regime the real second launcher arrives in:
		// keeps the horizontal velocity and sets the vertical speed to the
		// pre-declared simulation constant (still rising, already below the
		// scaled second launch of 490 cm/s).
		void SimulateMidFloatRise()
		{
			UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement();
			Movement->Velocity = FVector(Movement->Velocity.X, 0.0f, M1_025_MidFloatRiseSpeed);
		}

		float EnemyHealth() const
		{
			const UHealthComponent* Health = Enemy->GetHealthComponent();
			return (Health != nullptr) ? Health->GetHealth() : -1.0f;
		}

		UCharacterMovementComponent* EnemyMovement() const
		{
			return Enemy->GetCharacterMovement();
		}
	};
}

using namespace UE::UEMMO::Tasks::M1_025;

// Policy, fresh cycle: the first launcher of a target's float cycle is
// allowed at the full launch speed (Z scale 1.0).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_025PolicyFirstLauncherAllowedAtFullScale,
	"UEMMO.Tasks.M1_025.PolicyFirstLauncherAllowedAtFullScale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_025PolicyFirstLauncherAllowedAtFullScale::RunTest(const FString& Parameters)
{
	const FAirComboDecision Decision = EvaluateAirCombo(0);
	TestTrue(TEXT("the first launcher of a float cycle is allowed"), Decision.bAllowed);
	TestTrue(TEXT("the first launcher rises at the full launch speed (Z scale 1.0)"),
		FMath::IsNearlyEqual(Decision.ZScale, 1.0f));
	return true;
}

// Policy, second launch: still allowed, but the Z scale decays to 70 percent
// (the 700 cm/s launcher definition then applies 490 cm/s).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_025PolicySecondLauncherAllowedAtSeventyPercent,
	"UEMMO.Tasks.M1_025.PolicySecondLauncherAllowedAtSeventyPercent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_025PolicySecondLauncherAllowedAtSeventyPercent::RunTest(const FString& Parameters)
{
	const FAirComboDecision Decision = EvaluateAirCombo(1);
	TestTrue(TEXT("the second launcher of a float cycle is still allowed"), Decision.bAllowed);
	TestTrue(TEXT("the second launcher Z scale decays to 0.7"),
		FMath::IsNearlyEqual(Decision.ZScale, 0.7f));
	TestTrue(TEXT("the decayed scale turns the 700 cm/s definition launch into 490 cm/s"),
		FMath::IsNearlyEqual(700.0f * Decision.ZScale, 490.0f));
	return true;
}

// Policy, third and later launches: refused (no launch impulse at all, the
// Z scale reads 0); the hit site keeps damage and dedup regardless.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_025PolicyThirdLauncherRefused,
	"UEMMO.Tasks.M1_025.PolicyThirdLauncherRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_025PolicyThirdLauncherRefused::RunTest(const FString& Parameters)
{
	const FAirComboDecision ThirdDecision = EvaluateAirCombo(2);
	TestFalse(TEXT("the third launcher of a float cycle is refused"), ThirdDecision.bAllowed);
	TestTrue(TEXT("the refused launcher carries no Z scale"),
		FMath::IsNearlyEqual(ThirdDecision.ZScale, 0.0f));

	const FAirComboDecision FourthDecision = EvaluateAirCombo(3);
	TestFalse(TEXT("every launcher beyond the third stays refused"), FourthDecision.bAllowed);
	TestTrue(TEXT("the later refusals carry no Z scale either"),
		FMath::IsNearlyEqual(FourthDecision.ZScale, 0.0f));
	return true;
}

// Policy robustness: a negative count has no producer anywhere; the policy
// defensively reads it as a fresh cycle instead of refusing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_025PolicyNegativeCountReadsAsFreshCycle,
	"UEMMO.Tasks.M1_025.PolicyNegativeCountReadsAsFreshCycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_025PolicyNegativeCountReadsAsFreshCycle::RunTest(const FString& Parameters)
{
	const FAirComboDecision Decision = EvaluateAirCombo(-1);
	TestTrue(TEXT("a negative count is defensively treated as a fresh cycle"), Decision.bAllowed);
	TestTrue(TEXT("the fresh-cycle treatment uses the full Z scale"),
		FMath::IsNearlyEqual(Decision.ZScale, 1.0f));
	return true;
}

// Core acceptance: three launcher hits inside one float cycle. The first
// rises at 700, the second applies the decayed 490 launch, the third still
// deducts the full 18 damage (dedup stays per instance) but refuses the
// launch impulse entirely - Z stays where the second launch put it while the
// X knockback of the hit survives, and the policy count never increments.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_025SameCycleLauncherZDecaysThenThirdRefusesImpulse,
	"UEMMO.Tasks.M1_025.SameCycleLauncherZDecaysThenThirdRefusesImpulse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_025SameCycleLauncherZDecaysThenThirdRefusesImpulse::RunTest(const FString& Parameters)
{
	UWorld* World = M1_025_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_025_LaunchScene Scene;
	if (!Scene.Build(*this, *World, /*Facing*/ 1))
	{
		return true;
	}

	// First launcher: the target rises at the full definition launch speed
	// and the float cycle's policy count opens at 1.
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("the first launcher hit the enemy exactly once"), Scene.HitCount, 1);
	TestEqual(TEXT("the first hit removed exactly the launcher base damage"), Scene.EnemyHealth(), 82.0f);
	TestEqual(TEXT("the applied launch opened the float cycle count at 1"), Scene.Enemy->GetLauncherCycleCount(), 1);
	M1_025_DrivePendingVelocity(*Scene.EnemyMovement());
	TestTrue(TEXT("the first launcher rose the target at the definition launch speed (700)"),
		FMath::IsNearlyEqual(Scene.EnemyMovement()->Velocity.Z, Scene.Definition->LaunchSpeed, 0.01f));

	// Second launcher in the same cycle: the hit arrives in the mid-float
	// regime (simulated rise of 300, below the scaled second launch) and the
	// decayed 0.7 scale applies 490 cm/s. Damage and the count increment stay
	// unchanged.
	Scene.FinishLauncherInstance();
	Scene.SimulateMidFloatRise();
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("the second launcher instance hit the airborne target once more"), Scene.HitCount, 2);
	TestEqual(TEXT("the second hit removed the launcher base damage again"), Scene.EnemyHealth(), 64.0f);
	TestEqual(TEXT("the second applied launch incremented the float cycle count to 2"), Scene.Enemy->GetLauncherCycleCount(), 2);
	M1_025_DrivePendingVelocity(*Scene.EnemyMovement());
	const FVector SecondVelocity = Scene.EnemyMovement()->Velocity;
	TestTrue(TEXT("the second launcher applied the decayed launch (490 = 700 * 0.7)"),
		FMath::IsNearlyEqual(SecondVelocity.Z, 490.0f, 0.01f));
	TestTrue(TEXT("the second launcher kept the horizontal knockback additive (140)"),
		FMath::IsNearlyEqual(SecondVelocity.X, 2.0f * Scene.Definition->KnockbackSpeed, 0.01f));
	TestTrue(TEXT("the decayed launch kept the target airborne"),
		Scene.EnemyMovement()->MovementMode == MOVE_Falling && Scene.Enemy->GetAirState() == ECombatAirState::Rising);

	// Third launcher in the same cycle: the policy refuses the launch
	// impulse. The hit still deducts the full base damage, the dedup key is
	// still recorded for the instance (one hit per instance), the X knockback
	// survives and Z stays exactly where the second launch left it.
	Scene.FinishLauncherInstance();
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("the refused third launcher still hit the enemy exactly once"), Scene.HitCount, 3);
	TestEqual(TEXT("the third hit removed the full launcher base damage despite the refusal"), Scene.EnemyHealth(), 46.0f);
	TestEqual(TEXT("the refused launch left the float cycle count at 2"), Scene.Enemy->GetLauncherCycleCount(), 2);
	M1_025_DrivePendingVelocity(*Scene.EnemyMovement());
	const FVector ThirdVelocity = Scene.EnemyMovement()->Velocity;
	TestTrue(TEXT("the refused third launcher applied no launch impulse (Z unchanged at 490)"),
		FMath::IsNearlyEqual(ThirdVelocity.Z, 490.0f, 0.01f));
	TestTrue(TEXT("the refused third launcher kept the X knockback on the hit (210)"),
		FMath::IsNearlyEqual(ThirdVelocity.X, 3.0f * Scene.Definition->KnockbackSpeed, 0.01f));

	// Dedup stays intact for the refusing instance: ticking its remaining
	// frames (active window still open) must never land a second hit.
	Scene.FinishLauncherInstance();
	TestEqual(TEXT("the refused launcher instance never hits the same target twice"), Scene.HitCount, 3);
	return true;
}

// Ground recovery: a recorded ground contact (the same record point the real
// Landed notify feeds) closes the float cycle, so the next launcher rises at
// the full 700 again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_025GroundContactRestoresFullLauncherZ,
	"UEMMO.Tasks.M1_025.GroundContactRestoresFullLauncherZ",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_025GroundContactRestoresFullLauncherZ::RunTest(const FString& Parameters)
{
	UWorld* World = M1_025_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_025_LaunchScene Scene;
	if (!Scene.Build(*this, *World, /*Facing*/ 1))
	{
		return true;
	}

	// Spend the cycle: first launch at 700, second at the decayed 490.
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	M1_025_DrivePendingVelocity(*Scene.EnemyMovement());
	Scene.FinishLauncherInstance();
	Scene.SimulateMidFloatRise();
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	M1_025_DrivePendingVelocity(*Scene.EnemyMovement());
	TestTrue(TEXT("precondition: the second launcher applied the decayed 490 launch"),
		FMath::IsNearlyEqual(Scene.EnemyMovement()->Velocity.Z, 490.0f, 0.01f));
	TestEqual(TEXT("precondition: the float cycle count sits at 2"), Scene.Enemy->GetLauncherCycleCount(), 2);

	// The ground contact record (real landings feed it) reopens the cycle.
	Scene.Enemy->RecordGroundContact(/*NowSeconds*/ 9.0);
	TestEqual(TEXT("the ground contact cleared the float cycle count"), Scene.Enemy->GetLauncherCycleCount(), 0);

	// The next launcher of the recovered cycle rises at the full 700 again.
	Scene.FinishLauncherInstance();
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("the launcher after ground contact hit once more"), Scene.HitCount, 3);
	TestEqual(TEXT("the recovered cycle's launch incremented the count to 1"), Scene.Enemy->GetLauncherCycleCount(), 1);
	M1_025_DrivePendingVelocity(*Scene.EnemyMovement());
	TestTrue(TEXT("the launcher after ground contact rose the target at the full launch speed (700)"),
		FMath::IsNearlyEqual(Scene.EnemyMovement()->Velocity.Z, Scene.Definition->LaunchSpeed, 0.01f));
	TestEqual(TEXT("the three hits deducted the launcher damage every time"), Scene.EnemyHealth(), 46.0f);
	return true;
}

// ResetEnemy (the replayable-room reset) also reopens the float cycle: the
// next launcher after a reset rises at the full 700 again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_025ResetEnemyReopensFloatCycleAtFullLaunch,
	"UEMMO.Tasks.M1_025.ResetEnemyReopensFloatCycleAtFullLaunch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_025ResetEnemyReopensFloatCycleAtFullLaunch::RunTest(const FString& Parameters)
{
	UWorld* World = M1_025_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_025_LaunchScene Scene;
	if (!Scene.Build(*this, *World, /*Facing*/ 1))
	{
		return true;
	}

	// Spend the cycle: first launch at 700, second at the decayed 490.
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	M1_025_DrivePendingVelocity(*Scene.EnemyMovement());
	Scene.FinishLauncherInstance();
	Scene.SimulateMidFloatRise();
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	M1_025_DrivePendingVelocity(*Scene.EnemyMovement());
	TestEqual(TEXT("precondition: the float cycle count sits at 2"), Scene.Enemy->GetLauncherCycleCount(), 2);
	Scene.FinishLauncherInstance();

	// The room reset restores health, velocity and a fresh float cycle.
	Scene.Enemy->ResetEnemy();
	TestEqual(TEXT("ResetEnemy cleared the float cycle count"), Scene.Enemy->GetLauncherCycleCount(), 0);
	TestEqual(TEXT("ResetEnemy restored the full health pool"), Scene.EnemyHealth(), 100.0f);
	TestTrue(TEXT("ResetEnemy zeroed the velocity"), Scene.EnemyMovement()->Velocity.IsZero());

	// The next launcher of the fresh cycle rises at the full 700 again.
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("the launcher after the reset hit once"), Scene.HitCount, 3);
	TestEqual(TEXT("the fresh cycle's launch opened the count at 1"), Scene.Enemy->GetLauncherCycleCount(), 1);
	M1_025_DrivePendingVelocity(*Scene.EnemyMovement());
	TestTrue(TEXT("the launcher after the reset rose the target at the full launch speed (700)"),
		FMath::IsNearlyEqual(Scene.EnemyMovement()->Velocity.Z, Scene.Definition->LaunchSpeed, 0.01f));
	TestEqual(TEXT("the post-reset hit removed the launcher damage from the restored pool"), Scene.EnemyHealth(), 82.0f);
	return true;
}

// Death clears the cycle data: after the target died (and is later reset back
// to life), the next launcher starts a fresh cycle at the full 700 - and a
// launcher that arrives while the target is still dead neither damages nor
// launches anything.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_025TargetDeathClearsFloatCycleData,
	"UEMMO.Tasks.M1_025.TargetDeathClearsFloatCycleData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_025TargetDeathClearsFloatCycleData::RunTest(const FString& Parameters)
{
	UWorld* World = M1_025_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_025_LaunchScene Scene;
	if (!Scene.Build(*this, *World, /*Facing*/ 1))
	{
		return true;
	}

	// Spend the cycle: first launch at 700, second at the decayed 490.
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	M1_025_DrivePendingVelocity(*Scene.EnemyMovement());
	Scene.FinishLauncherInstance();
	Scene.SimulateMidFloatRise();
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	M1_025_DrivePendingVelocity(*Scene.EnemyMovement());
	TestEqual(TEXT("precondition: the float cycle count sits at 2"), Scene.Enemy->GetLauncherCycleCount(), 2);
	TestEqual(TEXT("precondition: the enemy holds 64 health"), Scene.EnemyHealth(), 64.0f);

	// Death clears the cycle data immediately (the health death event feeds
	// the same clear the reset uses).
	Scene.Enemy->GetHealthComponent()->ApplyDamage(100.0f);
	TestTrue(TEXT("the enemy died from the applied damage"), !Scene.Enemy->GetHealthComponent()->IsAlive());
	TestEqual(TEXT("the death cleared the float cycle count"), Scene.Enemy->GetLauncherCycleCount(), 0);

	// A launcher against the dead target is refused whole: no damage event,
	// no impulse, and the cycle count stays cleared.
	Scene.FinishLauncherInstance();
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("the launcher against the dead target landed no hit event"), Scene.HitCount, 2);
	TestEqual(TEXT("the dead target keeps the cycle count cleared"), Scene.Enemy->GetLauncherCycleCount(), 0);
	Scene.FinishLauncherInstance();

	// ResetEnemy revives the target as a fresh cycle: the next launcher rises
	// at the full 700 again.
	Scene.Enemy->ResetEnemy();
	TestTrue(TEXT("ResetEnemy revived the target"), Scene.Enemy->GetHealthComponent()->IsAlive());
	TestEqual(TEXT("the revived target starts with a cleared cycle"), Scene.Enemy->GetLauncherCycleCount(), 0);
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("the launcher against the revived target hit once"), Scene.HitCount, 3);
	TestEqual(TEXT("the revived cycle's launch opened the count at 1"), Scene.Enemy->GetLauncherCycleCount(), 1);
	M1_025_DrivePendingVelocity(*Scene.EnemyMovement());
	TestTrue(TEXT("the launcher against the revived target rose at the full launch speed (700)"),
		FMath::IsNearlyEqual(Scene.EnemyMovement()->Velocity.Z, Scene.Definition->LaunchSpeed, 0.01f));
	return true;
}

#endif
