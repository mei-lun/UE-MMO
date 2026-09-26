#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatGeometry.h"
#include "../Combat/CombatInputBuffer.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/TrainingEnemy.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_024
{
	// Remote base so the temp world can never collide with arena content.
	// X horizontal, Y depth, Z height; the base is used as the feet origin of
	// the attacker (box actors root at their feet).
	const FVector M1_024_SceneBase(40000.0, 45000.0, 600.0);

	// Half extent of one attacker body box; the same shape the M1-019/M1-022
	// suites use, tall enough to sit fully inside the aerial hit box (offset
	// (95,0,90), half extent (85,50,70)).
	const FVector M1_024_BodyHalfExtent(20.0, 20.0, 95.0);

	// The attack ids this card routes between. aerial_01's source values are
	// the interface contract section 3 row: (28, [6,10), [12,23), 12) with a
	// small 60 cm/s launch component.
	const FName M1_024_AerialAttackId(TEXT("aerial_01"));
	const FName M1_024_GroundLightAttackId(TEXT("light_01"));
	const FName M1_024_LauncherAttackId(TEXT("launcher"));

	// The tests drive the component with single 1/60 s steps, so logic frame N
	// sits at N/60 s of game time. The input clock is injected explicitly
	// (interface contract section 2); TickCombat never advances it itself.
	constexpr double M1_024_FrameSeconds = 1.0 / 60.0;

	// World acquisition, same order as the M1-018/M1-019/M1-022 pattern: a
	// private temp world first, the shared game world as fallback.
	static UWorld* M1_024_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M1_024_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M1_024 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M1_024 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	// Loads the real read-only catalog from Config/DefaultGame.ini (the same
	// loading path production and the earlier combat tests use). Returns null
	// after reporting the failure so the caller can bail out early.
	static UAttackCatalog* M1_024_NewCatalog(FAutomationTestBase& Test)
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

	// Spawns a plain actor with one query-enabled box root at Location (the
	// actor location is the feet origin) and a combat component attached to
	// the catalog: the attacker side of the aerial hit.
	static AActor* M1_024_SpawnAttacker(UWorld& World, const FVector& FeetLocation, UAttackCatalog* Catalog)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), FeetLocation, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M1_024_Body"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M1_024_BodyHalfExtent);
		Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Body->SetCollisionObjectType(ECC_Pawn);
		Body->SetCollisionResponseToAllChannels(ECR_Ignore);
		Body->RegisterComponent();
		Body->SetWorldLocation(FeetLocation);

		UCombatComponent* Combat = NewObject<UCombatComponent>(Actor, TEXT("M1_024_Combat"));
		Combat->RegisterComponent();
		if (Catalog != nullptr)
		{
			Combat->InitializeFromCatalog(Catalog);
		}
		return Actor;
	}

	// Spawns a training enemy whose capsule center sits at CapsuleCenter and
	// mirrors the M1-022 physics activation steps: a temp world never reaches
	// AreActorsInitialized, so the engine skips the movement component
	// auto-activation and the default movement mode init that the real arena
	// performs; without them every launch/impulse is silently dropped.
	static ATrainingEnemy* M1_024_SpawnEnemy(FAutomationTestBase& Test, UWorld& World, const FVector& CapsuleCenter)
	{
		FActorSpawnParameters SpawnParams;
		ATrainingEnemy* Enemy = World.SpawnActor<ATrainingEnemy>(
			ATrainingEnemy::StaticClass(), CapsuleCenter, FRotator::ZeroRotator, SpawnParams);
		if (!Test.TestNotNull(TEXT("the training enemy spawns"), Enemy))
		{
			return nullptr;
		}
		if (!Enemy->HasActorBegunPlay())
		{
			Enemy->DispatchBeginPlay();
		}
		UCharacterMovementComponent* MovementRaw = Enemy->GetCharacterMovement();
		if (MovementRaw != nullptr)
		{
			if (!MovementRaw->IsActive())
			{
				MovementRaw->Activate(/*bReset*/ true);
			}
			if (MovementRaw->MovementMode == MOVE_None)
			{
				MovementRaw->SetDefaultMovementMode();
			}
		}
		return Enemy;
	}

	// Applies exactly the two pending-velocity steps of one real movement
	// update, in PerformMovement order (ApplyAccumulatedForces, then
	// HandlePendingLaunch), without a world tick (the M1-022 pattern).
	static void M1_024_DrivePendingVelocity(UCharacterMovementComponent& Movement)
	{
		Movement.ApplyAccumulatedForces(1.0f / 60.0f);
		Movement.HandlePendingLaunch();
	}

	// Advances the component by exactly Count single 1/60 s frames.
	static void M1_024_TickFrames(UCombatComponent& Combat, int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Combat.TickCombat(static_cast<float>(M1_024_FrameSeconds));
		}
	}

	// Queues one combat intent with an explicit press time (input game clock).
	static void M1_024_QueueAction(UCombatComponent& Component, uint64 Sequence, ECombatInput Action, double PressedAt)
	{
		FBufferedCombatInput Input;
		Input.Sequence = Sequence;
		Input.Action = Action;
		Input.PressedAt = PressedAt;
		Component.QueueInput(Input);
	}

	// Builds a world-less combat component with the real catalog: the input
	// routing tests (air/ground J/K) drive this, no World needed.
	static UCombatComponent* M1_024_NewRoutedComponent(FAutomationTestBase& Test)
	{
		UAttackCatalog* Catalog = M1_024_NewCatalog(Test);
		if (Catalog == nullptr)
		{
			return nullptr;
		}
		UCombatComponent* Component = NewObject<UCombatComponent>();
		if (!Test.TestTrue(TEXT("InitializeFromCatalog accepts the initialized catalog"),
			Component->InitializeFromCatalog(Catalog)))
		{
			return nullptr;
		}
		return Component;
	}

	// One temp-world scene: the real catalog, an attacker actor whose feet sit
	// at AttackerFeet, and one training enemy whose capsule center sits at the
	// aerial_01 hit box center for the requested facing (feet at center - 88).
	struct FM1_024_AerialScene
	{
		UAttackCatalog* Catalog = nullptr;
		const UAttackDefinition* Definition = nullptr;
		AActor* Attacker = nullptr;
		UCombatComponent* Combat = nullptr;
		ATrainingEnemy* Enemy = nullptr;
		FVector BoxCenter = FVector::ZeroVector;
		int32 HitCount = 0;
		FCombatHit LastHit;

		// Builds attacker + enemy and asserts the aerial_01 source values the
		// acceptance criteria are phrased against. Returns false after
		// reporting the problem so the caller can bail out early.
		bool Build(FAutomationTestBase& Test, UWorld& World, const FVector& AttackerFeet, int32 Facing)
		{
			Catalog = M1_024_NewCatalog(Test);
			if (Catalog == nullptr)
			{
				return false;
			}
			Definition = Catalog->Find(M1_024_AerialAttackId);
			if (!Test.TestTrue(TEXT("the catalog holds aerial_01"), Definition != nullptr))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("aerial_01 keeps the design initial values (duration 28, window [6,10), damage 12, knockback 80, launch 60)"),
				Definition->DurationFrames == 28
				&& Definition->ActiveWindow.StartFrame == 6 && Definition->ActiveWindow.EndFrame == 10
				&& FMath::IsNearlyEqual(Definition->BaseDamage, 12.0f)
				&& FMath::IsNearlyEqual(Definition->KnockbackSpeed, 80.0f)
				&& FMath::IsNearlyEqual(Definition->LaunchSpeed, 60.0f)))
			{
				return false;
			}

			Attacker = M1_024_SpawnAttacker(World, AttackerFeet, Catalog);
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

			BoxCenter = ComputeHitBox(AttackerFeet, Facing, *Definition).Center;
			Enemy = M1_024_SpawnEnemy(Test, World, BoxCenter);
			if (Enemy == nullptr)
			{
				return false;
			}
			return Test.TestTrue(TEXT("the training enemy starts alive at full health"),
				Enemy->GetHealthComponent() != nullptr && Enemy->GetHealthComponent()->IsAlive()
				&& FMath::IsNearlyEqual(Enemy->GetHealthComponent()->GetHealth(), 100.0f));
		}

		// Starts one aerial_01 instance with the requested facing (must succeed).
		bool StartAerial(FAutomationTestBase& Test, int32 Facing)
		{
			return Test.TestTrue(TEXT("aerial_01 starts"), Combat->TryStartAttack(M1_024_AerialAttackId, Facing));
		}

		// Ticks exactly to the first active frame (6), where the hit lands.
		void TickToFirstActiveFrame()
		{
			M1_024_TickFrames(*Combat, 7);
		}

		// Ticks the remaining aerial_01 frames after the first active frame so
		// the attacker is Free for the next instance.
		void FinishInstance()
		{
			M1_024_TickFrames(*Combat, 21);
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

using namespace UE::UEMMO::Tasks::M1_024;

// Input routing in the air: with the injected airborne predicate returning
// true, a buffered Light (J) consumed from Free starts aerial_01 - never the
// ground light_01 - and mints a fresh instance with the buffer emptied.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_024AirborneLightRoutesToAerial01,
	"UEMMO.Tasks.M1_024.AirborneLightRoutesToAerial01",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_024AirborneLightRoutesToAerial01::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = M1_024_NewRoutedComponent(*this);
	if (Component == nullptr)
	{
		return true;
	}
	Component->SetAirStateProvider([]()
	{
		return true; // the owner reports IsFalling
	});
	Component->SetInputClockSeconds(1.0);
	M1_024_QueueAction(*Component, 1, ECombatInput::Light, 1.0);
	TestEqual(TEXT("precondition: the airborne Light press is buffered"), Component->GetSnapshot().BufferSize, 1);

	M1_024_TickFrames(*Component, 1);

	const FCombatSnapshot Started = Component->GetSnapshot();
	TestTrue(TEXT("the airborne Light started aerial_01"), Started.AttackId == M1_024_AerialAttackId);
	TestTrue(TEXT("the airborne Light never started the ground light_01"),
		Started.AttackId != M1_024_GroundLightAttackId);
	TestEqual(TEXT("the routed start minted instance id 1"), Started.InstanceId, uint64(1));
	TestTrue(TEXT("the routed start is running"), Started.ActionState == ECombatActionState::Attacking);
	TestEqual(TEXT("the consumed press emptied the buffer"), Started.BufferSize, 0);

	// One press, one start: a further tick neither starts again nor consumes.
	M1_024_TickFrames(*Component, 1);
	TestEqual(TEXT("a further tick consumed nothing"), Component->GetSnapshot().InstanceId, uint64(1));
	return true;
}

// Input routing on the ground: a buffered Light keeps starting light_01 both
// with an explicit grounded predicate and with no provider bound at all (the
// pre-M1-024 default behavior stays verbatim for bare components).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_024GroundedLightKeepsLight01,
	"UEMMO.Tasks.M1_024.GroundedLightKeepsLight01",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_024GroundedLightKeepsLight01::RunTest(const FString& Parameters)
{
	UCombatComponent* Grounded = M1_024_NewRoutedComponent(*this);
	if (Grounded == nullptr)
	{
		return true;
	}
	Grounded->SetAirStateProvider([]()
	{
		return false; // the owner reports grounded
	});
	Grounded->SetInputClockSeconds(1.0);
	M1_024_QueueAction(*Grounded, 1, ECombatInput::Light, 1.0);
	M1_024_TickFrames(*Grounded, 1);

	const FCombatSnapshot GroundStart = Grounded->GetSnapshot();
	TestTrue(TEXT("the grounded Light started light_01"), GroundStart.AttackId == M1_024_GroundLightAttackId);
	TestTrue(TEXT("the grounded Light never started aerial_01"),
		GroundStart.AttackId != M1_024_AerialAttackId);
	TestEqual(TEXT("the grounded start minted instance id 1"), GroundStart.InstanceId, uint64(1));
	TestEqual(TEXT("the consumed press emptied the buffer"), GroundStart.BufferSize, 0);

	// An unbound provider reads as grounded: the bare-component behavior is
	// unchanged, a buffered Light still starts light_01.
	UCombatComponent* Bare = M1_024_NewRoutedComponent(*this);
	if (Bare == nullptr)
	{
		return true;
	}
	Bare->SetInputClockSeconds(2.0);
	M1_024_QueueAction(*Bare, 1, ECombatInput::Light, 2.0);
	M1_024_TickFrames(*Bare, 1);
	TestTrue(TEXT("an unbound provider keeps the ground light_01 start"),
		Bare->GetSnapshot().AttackId == M1_024_GroundLightAttackId);
	return true;
}

// The Launcher routing never changes: an airborne K still starts the launcher
// (this card only re-routes the Light; the launcher allowance in the air is
// carried verbatim from the M1-021 mapping).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_024AirborneLauncherKeepsLauncher,
	"UEMMO.Tasks.M1_024.AirborneLauncherKeepsLauncher",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_024AirborneLauncherKeepsLauncher::RunTest(const FString& Parameters)
{
	UCombatComponent* Component = M1_024_NewRoutedComponent(*this);
	if (Component == nullptr)
	{
		return true;
	}
	Component->SetAirStateProvider([]()
	{
		return true; // the owner reports IsFalling
	});
	Component->SetInputClockSeconds(1.0);
	M1_024_QueueAction(*Component, 1, ECombatInput::Launcher, 1.0);
	M1_024_TickFrames(*Component, 1);

	const FCombatSnapshot Started = Component->GetSnapshot();
	TestTrue(TEXT("the airborne Launcher still starts the launcher"), Started.AttackId == M1_024_LauncherAttackId);
	TestEqual(TEXT("the launcher start minted instance id 1"), Started.InstanceId, uint64(1));
	TestEqual(TEXT("the consumed press emptied the buffer"), Started.BufferSize, 0);
	return true;
}

// The airborne hit box is computed from the raised feet: with the attacker's
// feet 250 cm above the scene base the aerial_01 box center sits at
// feet + (95,0,90) - the confirmed payload reports exactly that center - and a
// grounded enemy standing at the base level (Z separated only) is not hit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_024AerialHitBoxRisesWithAirborneFeet,
	"UEMMO.Tasks.M1_024.AerialHitBoxRisesWithAirborneFeet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_024AerialHitBoxRisesWithAirborneFeet::RunTest(const FString& Parameters)
{
	UWorld* World = M1_024_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	// The attacker's feet are 250 cm above the base; the box-root attacker
	// derives its feet from the actor location, so the raised spawn is the
	// raised feet origin.
	const FVector RaisedFeet = M1_024_SceneBase + FVector(0.0, 0.0, 250.0);
	FM1_024_AerialScene Scene;
	if (!Scene.Build(*this, *World, RaisedFeet, /*Facing*/ 1))
	{
		return true;
	}
	// A grounded enemy stands at the base level under the raised box: its
	// capsule (center at base + 88, half height 88) overlaps the box in X and
	// Y but its top (base + 176) sits below the box bottom (raised feet + 90
	// - 70 = base + 270), so only Z separates it from the aerial attack.
	ATrainingEnemy* GroundEnemy = M1_024_SpawnEnemy(*this, *World,
		M1_024_SceneBase + FVector(95.0, 0.0, 88.0));
	if (GroundEnemy == nullptr)
	{
		return true;
	}
	if (GroundEnemy->GetCharacterMovement() != nullptr)
	{
		GroundEnemy->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	}

	if (!Scene.StartAerial(*this, 1))
	{
		return true;
	}
	Scene.TickToFirstActiveFrame();

	TestEqual(TEXT("only the raised airborne enemy was hit"), Scene.HitCount, 1);
	TestEqual(TEXT("the raised enemy lost exactly the aerial base damage"), Scene.EnemyHealth(), 88.0f);
	const UHealthComponent* GroundHealth = GroundEnemy->FindComponentByClass<UHealthComponent>();
	TestTrue(TEXT("the ground-level enemy kept full health"), GroundHealth != nullptr
		&& FMath::IsNearlyEqual(GroundHealth->GetHealth(), 100.0f));
	TestTrue(TEXT("the confirmed payload reports the raised box center (feet + (95,0,90))"),
		Scene.LastHit.WorldHitLocation.Equals(Scene.BoxCenter, 1e-4f));
	TestTrue(TEXT("the box center rose by the full 250 cm feet lift plus the 90 cm offset"),
		FMath::IsNearlyEqual(Scene.LastHit.WorldHitLocation.Z - M1_024_SceneBase.Z, 340.0f, 1e-3f)
		&& FMath::IsNearlyEqual(Scene.BoxCenter.Z - RaisedFeet.Z, 90.0f, 1e-3f));
	return true;
}

// The aerial Z compensation takes max(currentZ, launch speed): a floating
// target rising at 100 cm/s keeps its 100 (never demoted to the 60 launch
// speed) while a slow target at 30 cm/s is raised to exactly 60 - one small
// compensation per accepted hit, never a per-frame acceleration.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_024AerialHitZCompensationTakesMaxWithCurrentZ,
	"UEMMO.Tasks.M1_024.AerialHitZCompensationTakesMaxWithCurrentZ",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_024AerialHitZCompensationTakesMaxWithCurrentZ::RunTest(const FString& Parameters)
{
	UWorld* World = M1_024_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_024_AerialScene Scene;
	if (!Scene.Build(*this, *World, M1_024_SceneBase, /*Facing*/ 1))
	{
		return true;
	}
	// A second enemy beside the first, both inside the box depth (the box half
	// extent is 50 in Y; the capsule radius 34 keeps both overlapping).
	ATrainingEnemy* SlowEnemy = M1_024_SpawnEnemy(*this, *World, Scene.BoxCenter + FVector(0.0, 40.0, 0.0));
	if (SlowEnemy == nullptr)
	{
		return true;
	}

	// Fresh temp-world enemies float in Falling; pin their vertical speeds.
	Scene.EnemyMovement()->Velocity = FVector(0.0f, 0.0f, 100.0f);
	SlowEnemy->GetCharacterMovement()->Velocity = FVector(0.0f, 0.0f, 30.0f);
	TestTrue(TEXT("precondition: the fast target floats at Z=100"),
		Scene.Enemy->GetAirState() == ECombatAirState::Rising
		&& FMath::IsNearlyEqual(Scene.EnemyMovement()->Velocity.Z, 100.0f, 0.01f));
	TestTrue(TEXT("precondition: the slow target floats at Z=30"),
		SlowEnemy->GetAirState() == ECombatAirState::Rising
		&& FMath::IsNearlyEqual(SlowEnemy->GetCharacterMovement()->Velocity.Z, 30.0f, 0.01f));

	if (!Scene.StartAerial(*this, 1))
	{
		return true;
	}
	Scene.TickToFirstActiveFrame();
	TestEqual(TEXT("both floating targets were hit on the first active frame"), Scene.HitCount, 2);

	M1_024_DrivePendingVelocity(*Scene.EnemyMovement());
	M1_024_DrivePendingVelocity(*SlowEnemy->GetCharacterMovement());

	TestTrue(TEXT("the 100 cm/s target kept its higher Z (max(100,60)=100, never demoted)"),
		FMath::IsNearlyEqual(Scene.EnemyMovement()->Velocity.Z, 100.0f, 0.01f));
	TestTrue(TEXT("the 30 cm/s target was raised to exactly the launch speed (max(30,60)=60)"),
		FMath::IsNearlyEqual(SlowEnemy->GetCharacterMovement()->Velocity.Z, 60.0f, 0.01f));
	TestTrue(TEXT("the compensated targets read as airborne"),
		Scene.Enemy->GetAirState() != ECombatAirState::Grounded
		&& SlowEnemy->GetAirState() != ECombatAirState::Grounded);
	return true;
}

// One aerial follow-up per float cycle: after an aerial hit the same floating
// target refuses the next aerial hit entirely (no damage, no second Z
// compensation, no hit event) until it lands; a grounded target opens a fresh
// cycle and can be aerial-hit again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_024SecondAerialFollowUpRefusedUntilLanding,
	"UEMMO.Tasks.M1_024.SecondAerialFollowUpRefusedUntilLanding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_024SecondAerialFollowUpRefusedUntilLanding::RunTest(const FString& Parameters)
{
	UWorld* World = M1_024_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_024_AerialScene Scene;
	if (!Scene.Build(*this, *World, M1_024_SceneBase, /*Facing*/ 1))
	{
		return true;
	}
	TestTrue(TEXT("precondition: the fresh temp-world enemy floats (Falling)"),
		Scene.Enemy->GetAirState() == ECombatAirState::Falling);

	// Cycle 1, first aerial hit: accepted, damages and compensates once.
	if (!Scene.StartAerial(*this, 1))
	{
		return true;
	}
	Scene.TickToFirstActiveFrame();
	TestEqual(TEXT("the first aerial hit landed"), Scene.HitCount, 1);
	TestEqual(TEXT("the first aerial hit removed exactly the base damage"), Scene.EnemyHealth(), 88.0f);
	M1_024_DrivePendingVelocity(*Scene.EnemyMovement());
	TestTrue(TEXT("the first aerial hit compensated Z to the launch speed"),
		FMath::IsNearlyEqual(Scene.EnemyMovement()->Velocity.Z, 60.0f, 0.01f));
	Scene.FinishInstance();
	TestTrue(TEXT("the first aerial instance finished back to Free"),
		Scene.Combat->GetSnapshot().ActionState == ECombatActionState::Free);

	// Same float cycle, second aerial attack: refused entirely - no damage,
	// no second compensation, no second hit event, no dedup-key poisoning.
	if (!Scene.StartAerial(*this, 1))
	{
		return true;
	}
	Scene.TickToFirstActiveFrame();
	TestEqual(TEXT("the second aerial attack hit nothing in the same float cycle"), Scene.HitCount, 1);
	TestEqual(TEXT("the refused follow-up left the target at 88"), Scene.EnemyHealth(), 88.0f);
	M1_024_DrivePendingVelocity(*Scene.EnemyMovement());
	TestTrue(TEXT("the refused follow-up applied no second Z compensation (still 60)"),
		FMath::IsNearlyEqual(Scene.EnemyMovement()->Velocity.Z, 60.0f, 0.01f));
	Scene.FinishInstance();

	// The target lands: the float cycle closes and the aerial follow-up is
	// available again.
	Scene.EnemyMovement()->SetMovementMode(MOVE_Walking);
	TestTrue(TEXT("precondition: the landed target reads Grounded"),
		Scene.Enemy->GetAirState() == ECombatAirState::Grounded);
	if (!Scene.StartAerial(*this, 1))
	{
		return true;
	}
	Scene.TickToFirstActiveFrame();
	TestEqual(TEXT("the aerial hit after landing was accepted"), Scene.HitCount, 2);
	TestEqual(TEXT("the fresh-cycle hit removed the base damage again (88 -> 76)"), Scene.EnemyHealth(), 76.0f);
	M1_024_DrivePendingVelocity(*Scene.EnemyMovement());
	TestTrue(TEXT("the fresh-cycle hit compensated Z once more (60)"),
		FMath::IsNearlyEqual(Scene.EnemyMovement()->Velocity.Z, 60.0f, 0.01f));
	return true;
}

// ResetCombat clears the per-float-cycle aerial follow-up records: a target
// whose cycle was already spent accepts an aerial hit again right after the
// attacker reset, without any landing in between.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_024ResetCombatClearsAerialFollowUpCycle,
	"UEMMO.Tasks.M1_024.ResetCombatClearsAerialFollowUpCycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_024ResetCombatClearsAerialFollowUpCycle::RunTest(const FString& Parameters)
{
	UWorld* World = M1_024_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_024_AerialScene Scene;
	if (!Scene.Build(*this, *World, M1_024_SceneBase, /*Facing*/ 1))
	{
		return true;
	}

	// First aerial hit spends the target's float cycle.
	if (!Scene.StartAerial(*this, 1))
	{
		return true;
	}
	Scene.TickToFirstActiveFrame();
	TestEqual(TEXT("the first aerial hit landed"), Scene.HitCount, 1);
	Scene.FinishInstance();

	// Proof of the spent cycle: the next aerial attack is refused while the
	// target still floats.
	if (!Scene.StartAerial(*this, 1))
	{
		return true;
	}
	Scene.TickToFirstActiveFrame();
	TestEqual(TEXT("precondition: the same-cycle follow-up is refused"), Scene.HitCount, 1);
	Scene.FinishInstance();
	TestTrue(TEXT("precondition: the target still floats"),
		Scene.Enemy->GetAirState() != ECombatAirState::Grounded);

	// The attacker reset clears the follow-up records: the still-floating
	// target accepts an aerial hit again (the unified reset semantics stay
	// with M1-027; this card only clears its own cycle bookkeeping).
	Scene.Combat->ResetCombat();
	if (!Scene.StartAerial(*this, 1))
	{
		return true;
	}
	Scene.TickToFirstActiveFrame();
	TestEqual(TEXT("the aerial hit after the reset was accepted"), Scene.HitCount, 2);
	TestEqual(TEXT("the post-reset hit removed the base damage (88 -> 76)"), Scene.EnemyHealth(), 76.0f);
	return true;
}

// Ground light regression: the buffered ground J still starts light_01 and
// the accepted ground hit behaves exactly as before M1-024 - exactly the base
// damage once, the definition impulse payload with no launch component, the
// box center at the unraised feet and no vertical speed change on the target.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_024GroundLightHitRegressionUnchanged,
	"UEMMO.Tasks.M1_024.GroundLightHitRegressionUnchanged",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_024GroundLightHitRegressionUnchanged::RunTest(const FString& Parameters)
{
	UWorld* World = M1_024_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_024_AerialScene Scene;
	if (!Scene.Build(*this, *World, M1_024_SceneBase, /*Facing*/ 1))
	{
		return true;
	}
	// The victim stands grounded at the ground-level light_01 box (the same
	// geometry as the aerial box, but from the unraised base feet).
	Scene.EnemyMovement()->SetMovementMode(MOVE_Walking);

	// Route the ground J through the buffer: grounded predicate, then the
	// input-driven Free start starts light_01 (the M1-021 mapping verbatim).
	Scene.Combat->SetAirStateProvider([]()
	{
		return false;
	});
	Scene.Combat->SetInputClockSeconds(1.0);
	M1_024_QueueAction(*Scene.Combat, 1, ECombatInput::Light, 1.0);
	M1_024_TickFrames(*Scene.Combat, 1);
	TestTrue(TEXT("the buffered ground J started light_01"),
		Scene.Combat->GetSnapshot().AttackId == M1_024_GroundLightAttackId);

	// Frames 0..7: the light_01 active window [7,11) opens on frame 7.
	M1_024_TickFrames(*Scene.Combat, 8);
	TestEqual(TEXT("the ground light hit exactly once"), Scene.HitCount, 1);
	TestEqual(TEXT("the ground light removed exactly 10 HP (100 -> 90)"), Scene.EnemyHealth(), 90.0f);

	// The accepted hit carries the untouched light_01 payload: the hit box
	// center sits at the unraised feet + (95, 0, 90) and no launch component
	// exists. The buffered start carries the component's stored facing (0
	// until the game-side facing wiring task), so the mirrored knockback is
	// 0 on this path - the pre-existing M1-021 behavior, unchanged here.
	const FCombatHit& Hit = Scene.LastHit;
	TestEqual(TEXT("the payload carries light_01"), Hit.AttackId, M1_024_GroundLightAttackId);
	TestTrue(TEXT("the buffered-path impulse carries the stored facing (0) with no launch component"),
		Hit.Impulse.Equals(FVector(0.0f, 0.0f, 0.0f), 1e-4f));
	TestTrue(TEXT("the payload reports the unraised ground box center"),
		Hit.WorldHitLocation.Equals(Scene.BoxCenter, 1e-4f));
	TestEqual(TEXT("the light hit never changed the target's vertical speed"),
		Scene.EnemyMovement()->Velocity.Z, 0.0);

	// The rest of the attack never deducts again.
	M1_024_TickFrames(*Scene.Combat, 18);
	TestTrue(TEXT("the ground light finished back to Free"),
		Scene.Combat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the rest of the ground light added no hit"), Scene.HitCount, 1);
	TestEqual(TEXT("the target stays at 90"), Scene.EnemyHealth(), 90.0f);

	// Full mirror regression with a direct start carrying facing +1: the
	// impulse X flips to the positive knockback (90) and Z stays 0.
	TestTrue(TEXT("precondition: the attacker is Free for the direct restart"),
		Scene.Combat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestTrue(TEXT("the direct light_01 restart with facing +1 succeeds"),
		Scene.Combat->TryStartAttack(M1_024_GroundLightAttackId, 1));
	M1_024_TickFrames(*Scene.Combat, 8);
	TestEqual(TEXT("the direct instance hit once more"), Scene.HitCount, 2);
	TestEqual(TEXT("the facing +1 hit removed 10 HP again (90 -> 80)"), Scene.EnemyHealth(), 80.0f);
	TestTrue(TEXT("the direct-path impulse mirrors the facing (+90, no launch component)"),
		Scene.LastHit.Impulse.Equals(FVector(90.0f, 0.0f, 0.0f), 1e-4f));
	return true;
}

#endif
