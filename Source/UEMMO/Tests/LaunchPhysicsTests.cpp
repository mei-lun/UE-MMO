#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

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

namespace UE::UEMMO::Tasks::M1_022
{
	// Remote base so the temp world can never collide with arena content.
	// X horizontal, Y depth, Z height; the base is used as the feet origin of
	// the attacker (box actors root at their feet).
	const FVector M1_022_SceneBase(40000.0, 45000.0, 600.0);

	// Half extent of one attacker body box; the same shape the M1-019/M1-020
	// suites use, tall enough to sit fully inside the launcher hit box
	// (offset (95,0,90), half extent (85,50,70)).
	const FVector M1_022_BodyHalfExtent(20.0, 20.0, 95.0);

	// World acquisition, same order as the M1-018/M1-019/M1-020 pattern: a
	// private temp world first, the shared game world as fallback.
	static UWorld* M1_022_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M1_022_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M1_022 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M1_022 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	// Loads the real read-only catalog from Config/DefaultGame.ini (the same
	// loading path production and the earlier combat tests use). Returns null
	// after reporting the failure so the caller can bail out early.
	static UAttackCatalog* M1_022_NewCatalog(FAutomationTestBase& Test)
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
	// combat component attached to the catalog: the attacker side of the launch.
	static AActor* M1_022_SpawnAttacker(UWorld& World, const FVector& FeetLocation, UAttackCatalog* Catalog)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), FeetLocation, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M1_022_Body"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M1_022_BodyHalfExtent);
		Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Body->SetCollisionObjectType(ECC_Pawn);
		Body->SetCollisionResponseToAllChannels(ECR_Ignore);
		Body->RegisterComponent();
		Body->SetWorldLocation(FeetLocation);

		UCombatComponent* Combat = NewObject<UCombatComponent>(Actor, TEXT("M1_022_Combat"));
		Combat->RegisterComponent();
		if (Catalog != nullptr)
		{
			Combat->InitializeFromCatalog(Catalog);
		}
		return Actor;
	}

	// Advances the component by exactly Count single 1/60 s frames.
	static void M1_022_TickFrames(UCombatComponent& Combat, int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Combat.TickCombat(1.0f / 60.0f);
		}
	}

	// Applies exactly the two pending-velocity steps of one real movement
	// update, in PerformMovement order (ApplyAccumulatedForces, then
	// HandlePendingLaunch), without a world tick. The temp worlds of this
	// suite never integrate physics, so the engine's deferred pending launch
	// velocity and pending impulses would otherwise stay invisible to the
	// assertions; these two public engine functions are the exact apply steps
	// a ticked PerformMovement would run.
	static void M1_022_DrivePendingVelocity(UCharacterMovementComponent& Movement)
	{
		Movement.ApplyAccumulatedForces(1.0f / 60.0f);
		Movement.HandlePendingLaunch();
	}

	// One scene: the real catalog, an attacker box actor at the scene base and
	// one training enemy spawned at the launcher hit box center for the
	// requested facing (the launcher box matches light_01 geometry: offset
	// (95,0,90), half extent (85,50,70)).
	struct FM1_022_LaunchScene
	{
		UAttackCatalog* Catalog = nullptr;
		const UAttackDefinition* Definition = nullptr;
		AActor* Attacker = nullptr;
		UCombatComponent* Combat = nullptr;
		ATrainingEnemy* Enemy = nullptr;
		FVector BoxCenter = FVector::ZeroVector;
		int32 HitCount = 0;
		FCombatHit LastHit;

		// Builds attacker + enemy and asserts the launcher source values the
		// acceptance criteria are phrased against. Returns false after
		// reporting the problem so the caller can bail out early.
		bool Build(FAutomationTestBase& Test, UWorld& World, int32 Facing)
		{
			Catalog = M1_022_NewCatalog(Test);
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

			Attacker = M1_022_SpawnAttacker(World, M1_022_SceneBase, Catalog);
			if (!Test.TestNotNull(TEXT("the attacker spawns"), Attacker))
			{
				return false;
			}
			Combat = Attacker->FindComponentByClass<UCombatComponent>();
			if (!Test.TestTrue(TEXT("the attacker carries a combat component"), Combat != nullptr))
			{
				return false;
			}
			Combat->OnHitConfirmed.AddLambda([this](const FCombatHit& Hit)
			{
				++HitCount;
				LastHit = Hit;
			});

			// The enemy capsule center sits at the hit box center for this
			// facing (feet at center - 88), so the launcher box covers it.
			BoxCenter = ComputeHitBox(M1_022_SceneBase, Facing, *Definition).Center;
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
			// actors skip PreInitializeComponents/InitializeComponents/
			// PostInitializeComponents entirely (AActor::PostActorConstruction
			// gates all three). The real arena runs inside a begun world where
			// the engine, for this controller-less dummy with
			// bRunPhysicsWithNoController: 1) auto-activates the movement
			// component (AActor::InitializeComponents) and 2) initializes the
			// default movement mode (ACharacter::PostInitializeComponents ->
			// SetDefaultMovementMode). Mirror exactly those two engine steps
			// here, or every AddImpulse/Launch is silently dropped by the
			// movement component's (mode != MOVE_None && IsActive()) guard.
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
			// M1-022 launch prerequisite diagnostics: the launch applies only
			// when the movement mode is not MOVE_None, the component is active
			// and its data is valid. The assertion message carries the state.
			const UCharacterMovementComponent* EnemyMovement = Enemy->GetCharacterMovement();
			const bool bCanAcceptLaunch = EnemyMovement != nullptr && EnemyMovement->MovementMode != MOVE_None
				&& EnemyMovement->IsActive() && EnemyMovement->HasValidData();
			if (!Test.TestTrue(FString::Printf(TEXT("the enemy movement component accepts launches (mode=%d active=%d validData=%d controller=%d noControllerPhysics=%d worldNetMode=%d worldBegunPlay=%d)"),
				static_cast<int32>(EnemyMovement ? EnemyMovement->MovementMode.GetValue() : MOVE_None),
				EnemyMovement && EnemyMovement->IsActive() ? 1 : 0,
				EnemyMovement && EnemyMovement->HasValidData() ? 1 : 0,
				Enemy->GetController() ? 1 : 0,
				EnemyMovement && EnemyMovement->bRunPhysicsWithNoController ? 1 : 0,
				static_cast<int32>(Enemy->GetNetMode()),
				Enemy->GetWorld() && Enemy->GetWorld()->HasBegunPlay() ? 1 : 0),
				bCanAcceptLaunch))
			{
				return false;
			}
			return Test.TestTrue(TEXT("the training enemy starts alive at full health"),
				Enemy->GetHealthComponent() != nullptr && Enemy->GetHealthComponent()->IsAlive()
				&& FMath::IsNearlyEqual(Enemy->GetHealthComponent()->GetHealth(), 100.0f));
		}

		// Starts one launcher instance with the requested facing and ticks
		// exactly to its first active frame (12), where the hit lands.
		bool HitWithLauncher(FAutomationTestBase& Test, int32 Facing)
		{
			if (!Test.TestTrue(TEXT("the launcher attack starts"), Combat->TryStartAttack(FName(TEXT("launcher")), Facing)))
			{
				return false;
			}
			M1_022_TickFrames(*Combat, 13);
			return true;
		}

		// Ticks the remaining launcher frames after the first active frame so
		// the attacker is Free for the next instance.
		void FinishLauncherInstance()
		{
			M1_022_TickFrames(*Combat, 27);
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

using namespace UE::UEMMO::Tasks::M1_022;

// Core acceptance case: a launcher hit that actually deducted health puts the
// target into a physical launch through its CharacterMovement - after the
// engine's pending-velocity apply step the vertical speed is the definition's
// launch speed (700 cm/s), the horizontal speed is the facing-mirrored
// knockback (70 cm/s), the mode is Falling and the actor location is
// untouched (velocity injection, not a teleport).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_022LauncherHitLaunchesTargetAtDefinitionSpeeds,
	"UEMMO.Tasks.M1_022.LauncherHitLaunchesTargetAtDefinitionSpeeds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_022LauncherHitLaunchesTargetAtDefinitionSpeeds::RunTest(const FString& Parameters)
{
	UWorld* World = M1_022_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_022_LaunchScene Scene;
	if (!Scene.Build(*this, *World, /*Facing*/ 1))
	{
		return true;
	}

	const FVector LocationBefore = Scene.Enemy->GetActorLocation();
	// Temp worlds have no floor, so the spawned dummy initializes in Falling
	// (SetDefaultMovementMode) and only the real arena settles it onto the
	// floor. The fresh preconditions that matter here: zero velocity and an
	// untouched air combo (the ground side of the first launcher is carried
	// by the combo record and verified by the count test below).
	TestTrue(TEXT("precondition: the fresh enemy has zero velocity"), Scene.EnemyMovement()->Velocity.IsZero());
	TestEqual(TEXT("precondition: the fresh enemy carries no air combo"), Scene.Enemy->GetAirComboCount(), 0);

	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("the launcher hit the enemy exactly once on its first active frame"), Scene.HitCount, 1);
	TestEqual(TEXT("the accepted hit removed exactly the launcher base damage"), Scene.EnemyHealth(), 82.0f);

	// One real movement update's pending-velocity steps (no world tick).
	M1_022_DrivePendingVelocity(*Scene.EnemyMovement());

	const FVector Velocity = Scene.EnemyMovement()->Velocity;
	TestTrue(TEXT("the launch set the Z velocity to the definition launch speed (700, positive up)"),
		FMath::IsNearlyEqual(Velocity.Z, Scene.Definition->LaunchSpeed, 0.01f) && Velocity.Z > 0.0f);
	TestTrue(TEXT("the launch kept the X knockback at the facing-mirrored definition speed (70)"),
		FMath::IsNearlyEqual(Velocity.X, Scene.Definition->KnockbackSpeed, 0.01f));
	TestTrue(TEXT("the launch added no Y velocity"), FMath::IsNearlyEqual(Velocity.Y, 0.0f, 0.01f));
	TestTrue(TEXT("the launched target is airborne in the Falling movement mode"),
		Scene.EnemyMovement()->MovementMode == MOVE_Falling);
	TestTrue(TEXT("the airborne target reads as Rising from its positive vertical speed"),
		Scene.Enemy->GetAirState() == ECombatAirState::Rising);

	// Velocity injection, never a teleport: the launch itself must not move
	// the actor; position changes only come from later collision integration.
	const FVector LocationAfter = Scene.Enemy->GetActorLocation();
	TestTrue(TEXT("the launch injected velocity without teleporting the actor"),
		LocationAfter.Equals(LocationBefore, 0.01f));

	// Kinematic proof of the height acceptance (see the dedicated height test):
	// with g = 980 cm/s^2 the rise peaks at v^2/(2g) = 700^2/1960 = 250 cm.
	return true;
}

// Facing -1 mirrors only the horizontal knockback: the launch keeps the same
// positive vertical speed, X flips to the negative definition speed and no Y
// velocity appears out of nowhere.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_022LeftFacingLaunchMirrorsXOnly,
	"UEMMO.Tasks.M1_022.LeftFacingLaunchMirrorsXOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_022LeftFacingLaunchMirrorsXOnly::RunTest(const FString& Parameters)
{
	UWorld* World = M1_022_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_022_LaunchScene Scene;
	if (!Scene.Build(*this, *World, /*Facing*/ -1))
	{
		return true;
	}

	const FVector LocationBefore = Scene.Enemy->GetActorLocation();
	if (!Scene.HitWithLauncher(*this, -1))
	{
		return true;
	}
	TestEqual(TEXT("the left-facing launcher hit the enemy exactly once"), Scene.HitCount, 1);

	M1_022_DrivePendingVelocity(*Scene.EnemyMovement());

	const FVector Velocity = Scene.EnemyMovement()->Velocity;
	TestTrue(TEXT("the left-facing launch flips only the X knockback (-70)"),
		FMath::IsNearlyEqual(Velocity.X, -Scene.Definition->KnockbackSpeed, 0.01f) && Velocity.X < 0.0f);
	TestTrue(TEXT("the left-facing launch keeps the same positive Z launch speed (700)"),
		FMath::IsNearlyEqual(Velocity.Z, Scene.Definition->LaunchSpeed, 0.01f) && Velocity.Z > 0.0f);
	TestTrue(TEXT("the left-facing launch adds no Y velocity"), FMath::IsNearlyEqual(Velocity.Y, 0.0f, 0.01f));
	TestTrue(TEXT("the left-facing launch injected velocity without teleporting the actor"),
		Scene.Enemy->GetActorLocation().Equals(LocationBefore, 0.01f));
	return true;
}

// Z override instead of stacking: two consecutive launcher hits keep the
// vertical speed at the definition launch speed (700, not 1400) while the
// horizontal knockback stays additive through the launch (70 + 70).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_022RepeatedLauncherHitsOverrideZInsteadOfStacking,
	"UEMMO.Tasks.M1_022.RepeatedLauncherHitsOverrideZInsteadOfStacking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_022RepeatedLauncherHitsOverrideZInsteadOfStacking::RunTest(const FString& Parameters)
{
	UWorld* World = M1_022_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_022_LaunchScene Scene;
	if (!Scene.Build(*this, *World, /*Facing*/ 1))
	{
		return true;
	}

	// First launcher hit: the target rises at the definition speeds.
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	M1_022_DrivePendingVelocity(*Scene.EnemyMovement());
	TestTrue(TEXT("the first launcher hit launched the target"),
		FMath::IsNearlyEqual(Scene.EnemyMovement()->Velocity.Z, Scene.Definition->LaunchSpeed, 0.01f));

	// Second launcher hit while the target is still airborne (no tick ever
	// landed it): the Z launch speed is replaced, never summed.
	Scene.FinishLauncherInstance();
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("the second launcher instance hit the airborne target once more"), Scene.HitCount, 2);
	M1_022_DrivePendingVelocity(*Scene.EnemyMovement());

	const FVector Velocity = Scene.EnemyMovement()->Velocity;
	TestTrue(TEXT("the repeated launcher hit overrode Z back to 700 instead of stacking to 1400"),
		FMath::IsNearlyEqual(Velocity.Z, Scene.Definition->LaunchSpeed, 0.01f) && Velocity.Z < 1400.0f);
	TestTrue(TEXT("the repeated launcher hit kept the horizontal knockback additive (140)"),
		FMath::IsNearlyEqual(Velocity.X, 2.0f * Scene.Definition->KnockbackSpeed, 0.01f));
	return true;
}

// AirComboCount semantics: the first launcher from ground contact counts 1, a
// launcher hit before the next ground contact increments (2), a ground
// contact record closes the combo so the next launcher counts 1 again, and
// ResetEnemy restores the fresh state. LastGroundedTime records the ground
// contact time. The Z-factor decay and the two-launch cap stay with M1-025;
// this counter only records.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_022AirComboCountTracksLauncherComboUntilGroundContact,
	"UEMMO.Tasks.M1_022.AirComboCountTracksLauncherComboUntilGroundContact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_022AirComboCountTracksLauncherComboUntilGroundContact::RunTest(const FString& Parameters)
{
	UWorld* World = M1_022_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_022_LaunchScene Scene;
	if (!Scene.Build(*this, *World, /*Facing*/ 1))
	{
		return true;
	}

	// A fresh dummy carries no combo and no velocity (temp worlds spawn it
	// mid-air in Falling; the first launcher's ground side comes from the
	// combo record, proven by the count flow below).
	TestEqual(TEXT("a fresh enemy carries no air combo"), Scene.Enemy->GetAirComboCount(), 0);
	TestTrue(TEXT("precondition: the fresh enemy has zero velocity"), Scene.EnemyMovement()->Velocity.IsZero());

	// First launcher from the ground: the combo opens at 1.
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("the ground launcher hit opened the air combo at 1"), Scene.Enemy->GetAirComboCount(), 1);
	M1_022_DrivePendingVelocity(*Scene.EnemyMovement());
	TestTrue(TEXT("the launched enemy reads as Rising"), Scene.Enemy->GetAirState() == ECombatAirState::Rising);

	// Simulated apex: the actual vertical speed decides the phase (interface
	// contract section 4: Rising/Falling follow the real velocity).
	Scene.EnemyMovement()->Velocity = FVector(Scene.EnemyMovement()->Velocity.X, 0.0f, -300.0f);
	TestTrue(TEXT("the enemy reads as Falling once its vertical speed is negative"),
		Scene.Enemy->GetAirState() == ECombatAirState::Falling);

	// Second launcher before any ground contact: the combo increments.
	Scene.FinishLauncherInstance();
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("the airborne launcher hit incremented the air combo to 2"), Scene.Enemy->GetAirComboCount(), 2);
	M1_022_DrivePendingVelocity(*Scene.EnemyMovement());
	TestTrue(TEXT("the second launch rose again"), Scene.Enemy->GetAirState() == ECombatAirState::Rising);

	// A ground contact record closes the combo: the next launcher opens a
	// fresh combo at 1 (the same semantic the real Landed notify feeds).
	Scene.Enemy->RecordGroundContact(/*NowSeconds*/ 12.5);
	TestTrue(TEXT("the ground contact record left the last-grounded time"), 
		FMath::IsNearlyEqual(Scene.Enemy->GetLastGroundedTimeSeconds(), 12.5, 0.0001));
	Scene.FinishLauncherInstance();
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	TestEqual(TEXT("the launcher after a ground contact opens a fresh combo at 1"), Scene.Enemy->GetAirComboCount(), 1);

	// ResetEnemy restores the fresh room state: no combo, grounded, zeroed
	// velocity and no launch residue.
	Scene.Enemy->ResetEnemy();
	TestEqual(TEXT("ResetEnemy cleared the air combo"), Scene.Enemy->GetAirComboCount(), 0);
	TestTrue(TEXT("ResetEnemy put the enemy back to Grounded"),
		Scene.Enemy->GetAirState() == ECombatAirState::Grounded);
	TestTrue(TEXT("ResetEnemy zeroed the launch velocity"), Scene.EnemyMovement()->Velocity.IsZero());
	return true;
}

// A floating (launched) target hit by a non-launching attack keeps its
// vertical state: the light knockback adds horizontally through the pending
// impulse path and never zeroes the launched Z speed, and the combat book
// keeping (stun) does not disturb the airborne phase either.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_022LightHitOnAirborneTargetPreservesZ,
	"UEMMO.Tasks.M1_022.LightHitOnAirborneTargetPreservesZ",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_022LightHitOnAirborneTargetPreservesZ::RunTest(const FString& Parameters)
{
	UWorld* World = M1_022_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_022_LaunchScene Scene;
	if (!Scene.Build(*this, *World, /*Facing*/ 1))
	{
		return true;
	}

	// Launch the enemy first (the launcher box matches the light box, so the
	// same attacker position reaches both attacks).
	if (!Scene.HitWithLauncher(*this, 1))
	{
		return true;
	}
	M1_022_DrivePendingVelocity(*Scene.EnemyMovement());
	TestTrue(TEXT("precondition: the enemy is launched and rising"),
		Scene.Enemy->GetAirState() == ECombatAirState::Rising
		&& FMath::IsNearlyEqual(Scene.EnemyMovement()->Velocity.Z, Scene.Definition->LaunchSpeed, 0.01f));

	// A light_01 hit against the airborne enemy: X knockback is additive,
	// Z stays at the launch speed (no ground walk, no stun can zero it).
	Scene.FinishLauncherInstance();
	if (!TestTrue(TEXT("the light_01 attack starts"), Scene.Combat->TryStartAttack(FName(TEXT("light_01")), 1)))
	{
		return true;
	}
	M1_022_TickFrames(*Scene.Combat, 8);
	TestEqual(TEXT("the light hit confirmed against the airborne enemy"), Scene.HitCount, 2);
	M1_022_DrivePendingVelocity(*Scene.EnemyMovement());

	const FVector Velocity = Scene.EnemyMovement()->Velocity;
	TestTrue(TEXT("the light hit on the floating target kept the launched Z speed (700, not zeroed)"),
		FMath::IsNearlyEqual(Velocity.Z, Scene.Definition->LaunchSpeed, 0.01f));
	TestTrue(TEXT("the light hit added its own horizontal knockback (70 + 90)"),
		FMath::IsNearlyEqual(Velocity.X, Scene.Definition->KnockbackSpeed + 90.0f, 0.01f));
	TestTrue(TEXT("the floating target still reads as airborne after the light hit"),
		Scene.Enemy->GetAirState() != ECombatAirState::Grounded);
	return true;
}

// Height acceptance proof: the launcher's launch speed rises the target
// higher than the 60 cm minimum. Kinematics (collision flight, no drag in
// the default CharacterMovement falling physics): with an initial vertical
// speed v = LaunchSpeed (700 cm/s from the real catalog definition) and the
// movement component's actual gravity g = 980 cm/s^2, the rise peaks at
// h = v^2 / (2g) = 700^2 / 1960 = 250 cm, which is greater than 60 cm. The
// test derives the number from the definition and the live gravity instead
// of hardcoding it, so the acceptance rides on the real values.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_022LauncherLaunchSpeedExceedsMinimumAirHeight,
	"UEMMO.Tasks.M1_022.LauncherLaunchSpeedExceedsMinimumAirHeight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_022LauncherLaunchSpeedExceedsMinimumAirHeight::RunTest(const FString& Parameters)
{
	UWorld* World = M1_022_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_022_LaunchScene Scene;
	if (!Scene.Build(*this, *World, /*Facing*/ 1))
	{
		return true;
	}

	const float LaunchZ = Scene.Definition->LaunchSpeed;
	const float Gravity = -Scene.EnemyMovement()->GetGravityZ();
	TestTrue(TEXT("precondition: the movement component falls under the standard 980 gravity"),
		FMath::IsNearlyEqual(Gravity, 980.0f, 0.5f) && Gravity > 0.0f);

	// h = v^2 / (2g) with the values above: 250 cm.
	const float MaxRiseHeight = (LaunchZ * LaunchZ) / (2.0f * Gravity);
	TestTrue(TEXT("the launch rises the target above the 60 cm acceptance minimum"),
		MaxRiseHeight > 60.0f);
	TestEqual(TEXT("the kinematic rise height matches the 250 cm expectation"), MaxRiseHeight, 250.0f, 1.0f);
	return true;
}

#endif
