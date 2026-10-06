// M5-016: the configured down state (interface contract sections 1 and 6).
// Pins the parameterized landing recovery: the knockdown/recovering
// durations are the component's own target policy grant (the M5-003 fields,
// legacy defaults 0.45/0.25 reproduce the M1-026 constants byte for byte),
// an illegal grant falls back to the legacy constants instead of poisoning
// the deadlines, the get-up protection refuses hits across the whole
// process, a plain jump never knocks down while a hit-airborne landing
// does, a dead combatant never recovers, a running process is never
// restarted by a repeated landing, and the unified reset chain (F2 / room
// retry) leaves no residual velocity, buffered input, hit-set records or
// protection flags.

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"

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

#include "Components/BoxComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_016
{
	// Remote base so the temp world can never collide with arena content.
	// X horizontal, Y depth, Z height; the base is the attacker's feet origin.
	const FVector M5_016_SceneBase(92000.0, 56000.0, 600.0);

	// Half extent of one test body box; sits fully inside the shared hit box
	// of the legacy attacks (offset (95,0,90), half extent (85,50,70)).
	const FVector M5_016_BodyHalfExtent(20.0, 20.0, 95.0);

	// World acquisition, M1-019 pattern: a private temp world, GWorld fallback.
	static UWorld* M5_016_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M5_016_TestWorld")));
		if (TempWorld != nullptr)
		{
			return TempWorld;
		}
		return GWorld;
	}

	/** Destroys the temp world again; each test owns exactly one. */
	struct FM5_016_WorldScope
	{
		UWorld* World = nullptr;

		~FM5_016_WorldScope()
		{
			if (World != nullptr && World != GWorld && GEngine != nullptr)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			}
		}
	};

	// Loads the real read-only catalog from Config/DefaultGame.ini.
	static UAttackCatalog* M5_016_NewCatalog(FAutomationTestBase& Test)
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

	// Counts HitConfirmed broadcasts and keeps the last payload.
	struct FM5_016_HitEvents
	{
		int32 HitCount = 0;
		FCombatHit LastHit;

		void Bind(UCombatComponent& Component)
		{
			Component.OnHitConfirmed.AddLambda([this](const FCombatHit& Hit)
			{
				++HitCount;
				LastHit = Hit;
			});
		}
	};

	/**
	 * One scene at its own remote base: the real catalog, one attacker box
	 * actor with a combat component, and a box victim (health + victim
	 * combat component) at the shared hit box center.
	 */
	struct FM5_016_Scene
	{
		UAttackCatalog* Catalog = nullptr;
		AActor* Attacker = nullptr;
		UCombatComponent* Combat = nullptr;
		AActor* Target = nullptr;
		UCombatComponent* VictimCombat = nullptr;
		FM5_016_HitEvents Events;

		bool Build(FAutomationTestBase& Test, UWorld& World, const FVector& InBase)
		{
			Catalog = M5_016_NewCatalog(Test);
			if (Catalog == nullptr)
			{
				return false;
			}

			FActorSpawnParameters Params;
			Attacker = World.SpawnActor<AActor>(AActor::StaticClass(), InBase, FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("the attacker spawns"), Attacker))
			{
				return false;
			}
			UBoxComponent* Body = NewObject<UBoxComponent>(Attacker, TEXT("M5_016_AttackerBody"));
			Attacker->SetRootComponent(Body);
			Body->SetMobility(EComponentMobility::Movable);
			Body->SetBoxExtent(M5_016_BodyHalfExtent);
			Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Body->SetCollisionObjectType(ECC_Pawn);
			Body->SetCollisionResponseToAllChannels(ECR_Ignore);
			Body->RegisterComponent();
			Body->SetWorldLocation(InBase);

			Combat = NewObject<UCombatComponent>(Attacker, TEXT("M5_016_Combat"));
			if (!Test.TestTrue(TEXT("the attacker combat component attaches the catalog"),
				Combat != nullptr && Combat->InitializeFromCatalog(Catalog)))
			{
				return false;
			}
			Combat->RegisterComponent();
			Combat->SetFeetLocationProvider([FixedFeet = InBase]() { return FixedFeet; });
			Events.Bind(*Combat);

			const UAttackDefinition* Definition = Catalog->Find(FName(TEXT("light_01")));
			if (!Test.TestTrue(TEXT("the catalog holds light_01"), Definition != nullptr))
			{
				return false;
			}
			const FVector BoxCenter = ComputeHitBox(InBase, /*Facing*/ 1, *Definition).Center;
			Target = World.SpawnActor<AActor>(AActor::StaticClass(), BoxCenter, FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("the victim spawns"), Target))
			{
				return false;
			}
			UBoxComponent* TargetBody = NewObject<UBoxComponent>(Target, TEXT("M5_016_TargetBody"));
			Target->SetRootComponent(TargetBody);
			TargetBody->SetMobility(EComponentMobility::Movable);
			TargetBody->SetBoxExtent(M5_016_BodyHalfExtent);
			TargetBody->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			TargetBody->SetCollisionObjectType(ECC_Pawn);
			TargetBody->SetCollisionResponseToAllChannels(ECR_Ignore);
			TargetBody->RegisterComponent();
			TargetBody->SetWorldLocation(BoxCenter);

			UHealthComponent* Health = NewObject<UHealthComponent>(Target, TEXT("M5_016_Health"));
			Health->RegisterComponent();
			VictimCombat = NewObject<UCombatComponent>(Target, TEXT("M5_016_VictimCombat"));
			VictimCombat->RegisterComponent();
			return true;
		}

		// Retires the case target as a corpse (the temp world has no world
		// context, so DestroyActor is not an option - the M5-013 lesson).
		void RetireTarget()
		{
			if (Target != nullptr)
			{
				if (UHealthComponent* Health = Target->FindComponentByClass<UHealthComponent>())
				{
					Health->ApplyDamage(Health->GetMaxHealth());
				}
			}
		}

		// Starts one attack instance and ticks exactly to its first active
		// frame, where the hit lands.
		bool HitOnce(FAutomationTestBase& Test, const TCHAR* AttackId, int32 Facing)
		{
			const UAttackDefinition* Definition = Catalog->Find(FName(AttackId));
			if (!Test.TestTrue(FString::Printf(TEXT("the catalog holds %s"), AttackId), Definition != nullptr))
			{
				return false;
			}
			if (!Test.TestTrue(FString::Printf(TEXT("%s starts"), AttackId),
				Combat->TryStartAttack(FName(AttackId), Facing)))
			{
				return false;
			}
			for (int32 Index = 0; Index <= Definition->ActiveWindow.StartFrame; ++Index)
			{
				Combat->TickCombat(1.0f / 60.0f);
			}
			return true;
		}

		// Ticks the running instance past its final frame so the attacker is
		// Free again (and the shot's ledger keys release).
		void FinishInstance(FAutomationTestBase& Test, const TCHAR* AttackId)
		{
			const UAttackDefinition* Definition = Catalog->Find(FName(AttackId));
			if (Definition != nullptr)
			{
				for (int32 Index = 0; Index < Definition->DurationFrames + 2; ++Index)
				{
					Combat->TickCombat(1.0f / 60.0f);
				}
			}
		}
	};

	// A hit payload naming the victim (the NotifyHitReceived entry requires
	// the target to match the component owner).
	static FCombatHit M5_016_MakeHit(AActor& Victim)
	{
		FCombatHit Hit;
		Hit.Target = &Victim;
		Hit.Damage = 10.0f;
		Hit.StunSeconds = 0.22f;
		return Hit;
	}
}

using namespace UE::UEMMO::Tasks::M5_016;

// ---------------------------------------------------------------------------
// DefaultDurationsPinTheLegacyBoundaries
// ---------------------------------------------------------------------------

// With the legacy default policy the landing process reproduces the M1-026
// constants byte for byte: a landing at t=10.0 runs Knockdown 0.45 s
// (refusing hits the whole way), flips to Recovering exactly at the 10.45
// boundary (still get-up protected) and returns to Free exactly at the
// 10.70 boundary, where a fresh hit is accepted again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_016DefaultDurationsPinTheLegacyBoundaries,
	"UEMMO.Tasks.M5_016.DefaultDurationsPinTheLegacyBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_016DefaultDurationsPinTheLegacyBoundaries::RunTest(const FString& Parameters)
{
	UWorld* World = M5_016_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_016_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_016_Scene Scene;
	if (!Scene.Build(*this, *World, M5_016_SceneBase))
	{
		return true;
	}

	// The landing opens the process at the legacy durations.
	TestTrue("the landing opens the recovery", Scene.VictimCombat->BeginLandingRecovery(10.0));
	TestTrue("the process starts in Knockdown",
		Scene.VictimCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);

	// Inside the knockdown window the get-up protection refuses the hit.
	Scene.VictimCombat->SetInputClockSeconds(10.40);
	Scene.VictimCombat->TickCombat(1.0f / 60.0f);
	TestTrue("the knockdown holds before its 0.45 s boundary",
		Scene.VictimCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);
	Scene.VictimCombat->NotifyHitReceived(M5_016_MakeHit(*Scene.Target));
	TestTrue("the protected landing refused the hit (no stun replaced the knockdown)",
		Scene.VictimCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);

	// The knockdown -> recovering flip lands exactly on the 0.45 s boundary.
	Scene.VictimCombat->SetInputClockSeconds(10.45);
	Scene.VictimCombat->TickCombat(1.0f / 60.0f);
	TestTrue("the knockdown flips to Recovering exactly at the 0.45 s boundary",
		Scene.VictimCombat->GetSnapshot().ActionState == ECombatActionState::Recovering);
	Scene.VictimCombat->NotifyHitReceived(M5_016_MakeHit(*Scene.Target));
	TestTrue("the recovering phase still refuses the hit",
		Scene.VictimCombat->GetSnapshot().ActionState == ECombatActionState::Recovering);

	// The recovering -> free flip lands exactly on the 0.70 s total boundary.
	Scene.VictimCombat->SetInputClockSeconds(10.70);
	Scene.VictimCombat->TickCombat(1.0f / 60.0f);
	TestTrue("the recovering phase ends exactly at the 0.70 s total boundary",
		Scene.VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);

	// A fresh hit after the process is accepted again.
	Scene.VictimCombat->NotifyHitReceived(M5_016_MakeHit(*Scene.Target));
	TestTrue("the post-process hit is accepted (stun applied)",
		Scene.VictimCombat->GetSnapshot().ActionState == ECombatActionState::HitStun);
	Scene.RetireTarget();
	return true;
}

// ---------------------------------------------------------------------------
// ConfiguredDurationsMoveTheBoundaries
// ---------------------------------------------------------------------------

// The component's own target policy grants the durations: a configured
// 0.9/0.1 policy moves both boundaries (the knockdown flips at 0.9, the
// free return at 1.0 total) without any class-code change, and an illegal
// grant (negative, non-finite) falls back to the legacy 0.45/0.25 instead
// of poisoning the deadlines.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_016ConfiguredDurationsMoveTheBoundaries,
	"UEMMO.Tasks.M5_016.ConfiguredDurationsMoveTheBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_016ConfiguredDurationsMoveTheBoundaries::RunTest(const FString& Parameters)
{
	UWorld* World = M5_016_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_016_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_016_Scene Scene;
	if (!Scene.Build(*this, *World, M5_016_SceneBase + FVector(3000.0, 0.0, 0.0)))
	{
		return true;
	}

	// 1. The configured 0.9/0.1 grant moves both boundaries.
	{
		FTargetReaction SlowGetUp = MakeLegacyNormalTargetReaction();
		SlowGetUp.PolicyId = TEXT("m5_016_slow_get_up");
		SlowGetUp.KnockdownSeconds = 0.9f;
		SlowGetUp.RecoveringSeconds = 0.1f;
		Scene.VictimCombat->SetTargetReactionPolicy(SlowGetUp);

		TestTrue("the landing opens the configured recovery", Scene.VictimCombat->BeginLandingRecovery(100.0));
		Scene.VictimCombat->SetInputClockSeconds(100.89);
		Scene.VictimCombat->TickCombat(1.0f / 60.0f);
		TestTrue("the configured knockdown holds before its 0.9 s boundary",
			Scene.VictimCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);
		Scene.VictimCombat->SetInputClockSeconds(100.90);
		Scene.VictimCombat->TickCombat(1.0f / 60.0f);
		TestTrue("the configured knockdown flips at exactly 0.9 s",
			Scene.VictimCombat->GetSnapshot().ActionState == ECombatActionState::Recovering);
		Scene.VictimCombat->SetInputClockSeconds(101.00);
		Scene.VictimCombat->TickCombat(1.0f / 60.0f);
		TestTrue("the configured process ends at exactly 1.0 s total",
			Scene.VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	}

	// 2. An illegal grant (negative) falls back to the legacy constants.
	{
		FTargetReaction BrokenGrant = MakeLegacyNormalTargetReaction();
		BrokenGrant.PolicyId = TEXT("m5_016_broken_grant");
		BrokenGrant.KnockdownSeconds = -1.0f;
		BrokenGrant.RecoveringSeconds = -1.0f;
		Scene.VictimCombat->SetTargetReactionPolicy(BrokenGrant);

		TestTrue("the landing opens the fallback recovery", Scene.VictimCombat->BeginLandingRecovery(200.0));
		Scene.VictimCombat->SetInputClockSeconds(200.44);
		Scene.VictimCombat->TickCombat(1.0f / 60.0f);
		TestTrue("the illegal grant keeps the legacy 0.45 s knockdown boundary",
			Scene.VictimCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);
		Scene.VictimCombat->SetInputClockSeconds(200.70);
		// A clock jump past both deadlines settles on the second tick (the
		// TickCombat per-frame flip contract).
		Scene.VictimCombat->TickCombat(1.0f / 60.0f);
		Scene.VictimCombat->TickCombat(1.0f / 60.0f);
		TestTrue("the illegal grant keeps the legacy 0.70 s total boundary",
			Scene.VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	}
	Scene.RetireTarget();
	return true;
}

// ---------------------------------------------------------------------------
// PlainJumpNeverKnocksDownAndLaunchedLandingDoes
// ---------------------------------------------------------------------------

// The landing route keeps the M1-026 trigger distinction through the real
// enemy path: a plain landing (never launched airborne) leaves the enemy
// Free and only records the ground contact, while a hit-airborne landing
// (the combat launch marker set) opens the configured recovery process.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_016PlainJumpNeverKnocksDownAndLaunchedLandingDoes,
	"UEMMO.Tasks.M5_016.PlainJumpNeverKnocksDownAndLaunchedLandingDoes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_016PlainJumpNeverKnocksDownAndLaunchedLandingDoes::RunTest(const FString& Parameters)
{
	UWorld* World = M5_016_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_016_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_016_Scene Scene;
	if (!Scene.Build(*this, *World, M5_016_SceneBase + FVector(6000.0, 0.0, 0.0)))
	{
		return true;
	}
		const UAttackDefinition* Definition = Scene.Catalog->Find(FName(TEXT("launcher")));
		if (!TestTrue(TEXT("the catalog holds launcher"), Definition != nullptr))
		{
			return true;
		}
		FActorSpawnParameters SpawnParams;
		const FVector BoxCenter = ComputeHitBox(M5_016_SceneBase + FVector(6000.0, 0.0, 0.0), 1, *Definition).Center;
		ATrainingEnemy* Enemy = World->SpawnActor<ATrainingEnemy>(
			ATrainingEnemy::StaticClass(), BoxCenter, FRotator::ZeroRotator, SpawnParams);
		if (!TestNotNull(TEXT("the landing case spawns its training enemy"), Enemy))
		{
			return true;
		}
		if (!Enemy->HasActorBegunPlay())
		{
			Enemy->DispatchBeginPlay();
		}
		UCharacterMovementComponent* EnemyMovement = Enemy->GetCharacterMovement();
		if (EnemyMovement != nullptr)
		{
			if (!EnemyMovement->IsActive())
			{
				EnemyMovement->Activate(/*bReset*/ true);
			}
			if (EnemyMovement->MovementMode == MOVE_None)
			{
				EnemyMovement->SetDefaultMovementMode();
			}
		}
		UCombatComponent* VictimCombat = Enemy->GetCombatComponent();
		if (!TestNotNull(TEXT("precondition: the training enemy carries a combat component"), VictimCombat))
		{
			return true;
		}

		// 1. The plain landing: no launched-airborne marker, no process.
		Enemy->NotifyLanded(10.0);
		TestTrue("the plain landing leaves the enemy Free",
			VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);
		TestFalse("the plain landing opens no protection", VictimCombat->IsInLandingRecovery());

		// 2. The hit-airborne landing: the combat launch path sets the
		//    launched-airborne marker, and the landing routes into the
		//    recovery process.
		if (!Scene.HitOnce(*this, TEXT("launcher"), 1))
		{
			return true;
		}
		TestTrue("precondition: the launcher landed on the enemy (launched airborne)",
			Scene.Events.LastHit.Target.Get() == Enemy && Scene.Events.LastHit.Impulse.Z > 0.0f);
		Scene.FinishInstance(*this, TEXT("launcher"));
		Enemy->NotifyLanded(11.0);
	TestTrue("the launched landing opens the knockdown",
		VictimCombat->GetSnapshot().ActionState == ECombatActionState::Knockdown);
	TestTrue("the launched landing is protected", VictimCombat->IsInLandingRecovery());

	// 3. The process completes on the legacy boundaries (the enemy default);
	//    the clock jump past both deadlines settles on the second tick.
	VictimCombat->SetInputClockSeconds(11.70);
	VictimCombat->TickCombat(1.0f / 60.0f);
	VictimCombat->TickCombat(1.0f / 60.0f);
	TestTrue("the launched landing recovery completes back to Free",
		VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	return true;
}

// ---------------------------------------------------------------------------
// DeadDoesNotRecoverAndRepeatedLandingKeepsTheTimer
// ---------------------------------------------------------------------------

// The process guards keep the M1-026 semantics: a repeated landing never
// restarts or extends a running recovery (the first deadlines stand), and a
// dead combatant never recovers - neither mid-process nor on a later
// landing request.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_016DeadDoesNotRecoverAndRepeatedLandingKeepsTheTimer,
	"UEMMO.Tasks.M5_016.DeadDoesNotRecoverAndRepeatedLandingKeepsTheTimer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_016DeadDoesNotRecoverAndRepeatedLandingKeepsTheTimer::RunTest(const FString& Parameters)
{
	UWorld* World = M5_016_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_016_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_016_Scene Scene;
	if (!Scene.Build(*this, *World, M5_016_SceneBase + FVector(9000.0, 0.0, 0.0)))
	{
		return true;
	}

	// 1. A repeated landing during a running process is refused: the first
	//    deadlines stand (the flip still lands at the original boundary).
	TestTrue("the first landing opens the process", Scene.VictimCombat->BeginLandingRecovery(20.0));
	TestFalse("the repeated landing is refused", Scene.VictimCombat->BeginLandingRecovery(20.2));
	Scene.VictimCombat->SetInputClockSeconds(20.45);
	Scene.VictimCombat->TickCombat(1.0f / 60.0f);
	TestTrue("the first deadlines stand (the flip lands at 0.45 s, not the re-asked 0.65 s)",
		Scene.VictimCombat->GetSnapshot().ActionState == ECombatActionState::Recovering);
	Scene.VictimCombat->SetInputClockSeconds(20.70);
	Scene.VictimCombat->TickCombat(1.0f / 60.0f);
	TestTrue("the process completes at the original 0.70 s total",
		Scene.VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);

	// 2. A dead combatant never recovers: the entry refuses while dead.
	Scene.VictimCombat->SetDead(true);
	TestFalse("the dead combatant refuses a landing process", Scene.VictimCombat->BeginLandingRecovery(21.0));
	TestTrue("the dead combatant stays out of the down state",
		Scene.VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	Scene.RetireTarget();
	return true;
}

// ---------------------------------------------------------------------------
// ResetChainLeavesNoTransientState
// ---------------------------------------------------------------------------

// The unified reset chain (the room retry entry the F2 key shares) leaves
// no transient state behind: after a deliberately dirtied session - an
// airborne, stunned, mid-recovery player and enemy with buffered input,
// poise pressure, an open air window and spent float cycles - the reset
// restores zero velocities (pending launches and impulses included), empty
// input buffers, cleared protection flags and dead flags, full pools and
// fresh float cycles on every participant.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_016ResetChainLeavesNoTransientState,
	"UEMMO.Tasks.M5_016.ResetChainLeavesNoTransientState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_016ResetChainLeavesNoTransientState::RunTest(const FString& Parameters)
{
	UWorld* World = M5_016_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_016_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_016_Scene Scene;
	if (!Scene.Build(*this, *World, M5_016_SceneBase + FVector(12000.0, 0.0, 0.0)))
	{
		return true;
	}
	// The service scene: the box-actor stand-in cannot serve (the reset
	// chain zeroes CHARACTER velocities), so the participants are the real
	// player and training enemy actors, registered on the service.
	const FVector ServiceBase = M5_016_SceneBase + FVector(12000.0, 0.0, 0.0);
	FActorSpawnParameters SpawnParams;
	APrototypeCharacter* Player = World->SpawnActor<APrototypeCharacter>(
		APrototypeCharacter::StaticClass(), ServiceBase, FRotator::ZeroRotator, SpawnParams);
	if (!TestNotNull(TEXT("the reset case spawns its player"), Player))
	{
		return true;
	}
	if (!Player->HasActorBegunPlay())
	{
		Player->DispatchBeginPlay();
	}
	ATrainingEnemy* Enemy = World->SpawnActor<ATrainingEnemy>(
		ATrainingEnemy::StaticClass(), ServiceBase + FVector(95.0, 0.0, 0.0), FRotator::ZeroRotator, SpawnParams);
	if (!TestNotNull(TEXT("the reset case spawns its enemy"), Enemy))
	{
		return true;
	}
	if (!Enemy->HasActorBegunPlay())
	{
		Enemy->DispatchBeginPlay();
	}
	UTrainingResetService* Service = NewObject<UTrainingResetService>(World, TEXT("M5_016_ResetService"));
	Service->RegisterPlayer(Player);
	Service->RegisterEnemy(Enemy);
	if (!TestTrue(TEXT("precondition: the service registered both participants"),
		Service->HasRegisteredPlayer() && Service->GetRegisteredEnemyCount() == 1))
	{
		return true;
	}

	// Dirty every transient surface the reset must clear.
	FBufferedCombatInput Input;
	Input.Sequence = 1;
	Input.Action = ECombatInput::Light;
	Input.PressedAt = 1.0;
	Player->GetCombat()->QueueInput(Input);
	Player->GetCombat()->SetInputClockSeconds(5.0);
	UCharacterMovementComponent* PlayerMovement = Player->GetCharacterMovement();
	if (TestNotNull(TEXT("precondition: the player carries a movement component"), PlayerMovement))
	{
		PlayerMovement->SetMovementMode(MOVE_Falling);
		PlayerMovement->Velocity = FVector(300.0f, 0.0f, 700.0f);
	}
	// The enemy: mid-recovery (launched landing), a buffered input, poise
	// pressure and an open air-control window.
	UCombatComponent* EnemyCombat = Enemy->GetCombatComponent();
	if (!TestNotNull(TEXT("precondition: the enemy carries a combat component"), EnemyCombat))
	{
		return true;
	}
	EnemyCombat->QueueInput(Input);
	EnemyCombat->SetTargetReactionPolicy([]()
	{
		FTargetReaction Pool = MakeLegacyNormalTargetReaction();
		Pool.PolicyId = TEXT("m5_016_reset_pool");
		Pool.PoiseMax = 25.0f;
		return Pool;
	}());
	EnemyCombat->RecordPoisePressure(10.0f);
	EnemyCombat->OpenAirControlWindow(2.5);
	EnemyCombat->BeginLandingRecovery(5.0);
	if (UCharacterMovementComponent* EnemyMovement = Enemy->GetCharacterMovement())
	{
		EnemyMovement->SetMovementMode(MOVE_Falling);
		EnemyMovement->Velocity = FVector(0.0f, 0.0f, 400.0f);
	}

	// The reset: the unified chain clears every transient at once.
	Service->ResetTrainingSession();

	// Player: zero velocity, empty buffer, no protection, back to Free.
	if (UCharacterMovementComponent* PlayerMovementAfter = Player->GetCharacterMovement())
	{
		TestTrue("the reset zeroed the player velocity (pending forces included)",
			PlayerMovementAfter->Velocity.IsZero() && PlayerMovementAfter->PendingLaunchVelocity.IsZero());
	}
	TestEqual("the reset emptied the player input buffer", Player->GetCombat()->GetSnapshot().BufferSize, 0);
	TestTrue("the reset returned the player to Free",
		Player->GetCombat()->GetSnapshot().ActionState == ECombatActionState::Free);
	TestFalse("the reset cleared the player protection flags", Player->GetCombat()->IsInLandingRecovery());

	// Enemy: zero velocity (pending launch included), full pool, fresh cycle,
	// closed window, empty buffer, no protection, no dead flag.
	if (UCharacterMovementComponent* EnemyMovementAfter = Enemy->GetCharacterMovement())
	{
		TestTrue("the reset zeroed the enemy velocity (pending launch included)",
			EnemyMovementAfter->Velocity.IsZero() && EnemyMovementAfter->PendingLaunchVelocity.IsZero());
	}
	TestEqual("the reset emptied the enemy input buffer", EnemyCombat->GetSnapshot().BufferSize, 0);
	TestTrue("the reset returned the enemy to Free",
		EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestFalse("the reset cleared the enemy protection flags", EnemyCombat->IsInLandingRecovery());
	TestEqual("the reset restored the enemy poise pool to full", EnemyCombat->GetEffectivePoiseCurrent(), 25.0f);
	TestEqual("the reset reopened the enemy float cycle", EnemyCombat->GetLaunchCycleCount(), 0);
	TestFalse("the reset closed the enemy air-control window", EnemyCombat->IsAirControlExpired());
	TestEqual("the reset restored the enemy health",
		Enemy->GetHealthComponent()->GetHealth(), Enemy->GetHealthComponent()->GetMaxHealth());
	return true;
}

#endif
