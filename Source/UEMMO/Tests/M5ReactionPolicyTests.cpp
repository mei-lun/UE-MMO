// M5-014: the hit-reaction policy (interface contract sections 1 and 5).
// Pins the ReactionResolver pure functions - the "hit received request"
// computed from the attack reaction (M5-003) and the target policy (M5-003)
// plus the target's state facts - and their propagation into the
// CombatComponent hit pipeline. The default policy reproduces the legacy
// M5-013 adaptation path byte for byte (10/14/18/12 damage faces, stun
// forwarding with the max-override semantics, the launcher's 1.0/0.7/refused
// float-cycle Z scaling, the facing-mirrored impulse with the Y depth never
// locked); changing the policy values changes the stun and the impulse
// without touching any character or component class code. The resolver is
// stateless: the component stays the only HitStun/Knockdown timing and
// action-state source (interrupt/reset clear the deadlines, M1 semantics).

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatGeometry.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/ReactionResolver.h"
#include "../Enemy/TrainingEnemy.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"

#include <limits>
#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Compile-time pins: the request is a pointer-free value snapshot and the
// refusal/override enums stay append-only uint8 with 0 = the neutral value.
// ---------------------------------------------------------------------------

static_assert(std::is_trivially_copyable_v<FHitReactionRequest>, "FHitReactionRequest must stay a trivially copyable value snapshot");
static_assert(!std::is_pointer_v<decltype(FReactionHitContext::TargetPolicy)>, "FReactionHitContext::TargetPolicy must stay a value container");
static_assert(static_cast<uint8>(EReactionRefuseReason::None) == 0, "EReactionRefuseReason::None must stay 0");
static_assert(static_cast<uint8>(EReactionRefuseReason::TargetDead) == 1, "EReactionRefuseReason::TargetDead must stay 1");
static_assert(static_cast<uint8>(EReactionRefuseReason::GetUpProtection) == 2, "EReactionRefuseReason::GetUpProtection must stay 2");
static_assert(static_cast<uint8>(EHitStunOverride::Max) == 0, "EHitStunOverride::Max must stay 0");

namespace UE::UEMMO::Tasks::M5_014
{
	// ------------------------------------------------------------------
	// Resolver-level helpers (pure; no World anywhere in these tests).
	// ------------------------------------------------------------------

	/** The legacy light_01 numbers (the M5-003 pinned values). */
	static FDamageProfile M5_014_MakeLight01()
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = TEXT("light_01");
		Profile.BaseDamage = 10.0f;
		Profile.AttackCoefficient = 1.0f;
		Profile.HitStunSeconds = 0.22f;
		Profile.KnockbackCmPerSecond = 90.0f;
		Profile.LaunchCmPerSecond = 0.0f;
		Profile.HitStopSeconds = 0.04f;
		return Profile;
	}

	/** The legacy launcher numbers (the M5-003 pinned values). */
	static FDamageProfile M5_014_MakeLauncher()
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = TEXT("launcher");
		Profile.BaseDamage = 18.0f;
		Profile.AttackCoefficient = 1.0f;
		Profile.HitStunSeconds = 1.0f;
		Profile.KnockbackCmPerSecond = 70.0f;
		Profile.LaunchCmPerSecond = 700.0f;
		Profile.HitStopSeconds = 0.04f;
		return Profile;
	}

	/** The legacy normal target: every gate open, no poise, no immunity. */
	static FTargetReaction M5_014_MakeNormalTarget()
	{
		return MakeLegacyNormalTargetReaction();
	}

	/** A context for one hit: the given attack and policy, facing +1, alive, no cycle scaling. */
	static FReactionHitContext M5_014_MakeContext(const FDamageProfile& Attack, const FTargetReaction& Policy)
	{
		FReactionHitContext Context;
		Context.Attack = Attack;
		Context.AttackReaction = FAttackReaction();
		Context.TargetPolicy = Policy;
		Context.Facing = 1;
		return Context;
	}

	/** True when every request face is off and every magnitude is zero (the refused shape). */
	static bool M5_014_IsEmptyRequest(const FHitReactionRequest& Request)
	{
		return !Request.bDamageApplies
			&& !Request.bControlAccepted
			&& !Request.bStagger
			&& !Request.bLaunch
			&& !Request.bKnockdown
			&& Request.StunSeconds == 0.0f
			&& !Request.bInterruptAttack
			&& Request.ImpulseVelocity.IsZero()
			&& Request.LaunchCmPerSecond == 0.0f
			&& !Request.bLaunchAdmitted
			&& Request.KnockdownSeconds == 0.0f
			&& Request.RecoveringSeconds == 0.0f;
	}

	// ------------------------------------------------------------------
	// Component-level scene (the M5-013 temp world + real ini catalog mode).
	// ------------------------------------------------------------------

	// Remote base so the temp world can never collide with arena content.
	// X horizontal, Y depth, Z height; the base is the attacker's feet origin.
	const FVector M5_014_SceneBase(52000.0, 56000.0, 600.0);

	// Half extent of one test body box; sits fully inside the light_01 and
	// launcher hit boxes (both offset (95,0,90), half extent (85,50,70)).
	const FVector M5_014_BodyHalfExtent(20.0, 20.0, 95.0);

	// World acquisition, M1-019 pattern: a private temp world, GWorld fallback.
	static UWorld* M5_014_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M5_014_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M5_014 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M5_014 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	/** Destroys the temp world again; each test owns exactly one. */
	struct FM5_014_WorldScope
	{
		UWorld* World = nullptr;

		~FM5_014_WorldScope()
		{
			if (World != nullptr && World != GWorld && GEngine != nullptr)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			}
		}
	};

	// Loads the real read-only catalog from Config/DefaultGame.ini (the same
	// loading path production and the earlier combat tests use).
	static UAttackCatalog* M5_014_NewCatalog(FAutomationTestBase& Test)
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
	struct FM5_014_HitEvents
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
	 * actor with a combat component whose feet sit at a fixed injected
	 * origin, and helpers to spawn box targets and training enemies at the
	 * active hit box center. Each case builds its own scene so corpses and
	 * float-cycle counts never leak between cases.
	 */
	struct FM5_014_Scene
	{
		UAttackCatalog* Catalog = nullptr;
		AActor* Attacker = nullptr;
		UCombatComponent* Combat = nullptr;
		FM5_014_HitEvents Events;
		FVector SceneBase = FVector::ZeroVector;

		bool Build(FAutomationTestBase& Test, UWorld& World, const FVector& InBase)
		{
			SceneBase = InBase;
			Catalog = M5_014_NewCatalog(Test);
			if (Catalog == nullptr)
			{
				return false;
			}

			FActorSpawnParameters Params;
			Attacker = World.SpawnActor<AActor>(AActor::StaticClass(), SceneBase, FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("the attacker spawns"), Attacker))
			{
				return false;
			}
			UBoxComponent* Body = NewObject<UBoxComponent>(Attacker, TEXT("M5_014_AttackerBody"));
			Attacker->SetRootComponent(Body);
			Body->SetMobility(EComponentMobility::Movable);
			Body->SetBoxExtent(M5_014_BodyHalfExtent);
			Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Body->SetCollisionObjectType(ECC_Pawn);
			Body->SetCollisionResponseToAllChannels(ECR_Ignore);
			Body->RegisterComponent();
			Body->SetWorldLocation(SceneBase);

			Combat = NewObject<UCombatComponent>(Attacker, TEXT("M5_014_Combat"));
			if (!Test.TestTrue(TEXT("the attacker combat component attaches the catalog"),
				Combat != nullptr && Combat->InitializeFromCatalog(Catalog)))
			{
				return false;
			}
			Combat->RegisterComponent();
			Combat->SetFeetLocationProvider([FixedFeet = SceneBase]() { return FixedFeet; });
			Events.Bind(*Combat);
			return true;
		}

		const UAttackDefinition* FindDefinition(FAutomationTestBase& Test, const TCHAR* AttackId)
		{
			const UAttackDefinition* Definition = Catalog->Find(FName(AttackId));
			Test.TestTrue(FString::Printf(TEXT("the catalog holds %s"), AttackId), Definition != nullptr);
			return Definition;
		}

		// Spawns a box target (health, optionally a bare victim combat
		// component, optionally its own catalog so it can run attacks) at the
		// world-space hit box center of the requested attack/facing pair.
		AActor* SpawnBoxTarget(UWorld& World, const UAttackDefinition& Definition, int32 Facing,
			bool bWithCombat = false, bool bTargetWithCatalog = false)
		{
			const FVector BoxCenter = ComputeHitBox(SceneBase, Facing, Definition).Center;
			FActorSpawnParameters Params;
			AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), BoxCenter, FRotator::ZeroRotator, Params);
			if (Actor == nullptr)
			{
				return nullptr;
			}
			UBoxComponent* TargetBody = NewObject<UBoxComponent>(Actor, TEXT("M5_014_TargetBody"));
			Actor->SetRootComponent(TargetBody);
			TargetBody->SetMobility(EComponentMobility::Movable);
			TargetBody->SetBoxExtent(M5_014_BodyHalfExtent);
			TargetBody->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			TargetBody->SetCollisionObjectType(ECC_Pawn);
			TargetBody->SetCollisionResponseToAllChannels(ECR_Ignore);
			TargetBody->RegisterComponent();
			TargetBody->SetWorldLocation(BoxCenter);

			UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_014_Health"));
			Health->RegisterComponent();
			if (bWithCombat)
			{
				UCombatComponent* VictimCombat = NewObject<UCombatComponent>(Actor, TEXT("M5_014_VictimCombat"));
				VictimCombat->RegisterComponent();
				if (bTargetWithCatalog)
				{
					VictimCombat->InitializeFromCatalog(Catalog);
				}
			}
			return Actor;
		}

		// Spawns a training enemy at the hit box center for the requested
		// facing (the M1-022 pattern: capsule center at the box center, the
		// movement component activated for launches).
		ATrainingEnemy* SpawnTrainingEnemy(UWorld& World, const UAttackDefinition& Definition, int32 Facing)
		{
			const FVector BoxCenter = ComputeHitBox(SceneBase, Facing, Definition).Center;
			FActorSpawnParameters SpawnParams;
			ATrainingEnemy* Enemy = World.SpawnActor<ATrainingEnemy>(
				ATrainingEnemy::StaticClass(), BoxCenter, FRotator::ZeroRotator, SpawnParams);
			if (Enemy == nullptr)
			{
				return nullptr;
			}
			// BeginPlay owns death wiring; temp worlds may not have begun
			// play, so dispatch it once explicitly and never twice.
			if (!Enemy->HasActorBegunPlay())
			{
				Enemy->DispatchBeginPlay();
			}
			// Mirror the engine's auto-activation steps (the M1-022 lesson):
			// without them the movement component silently drops launches.
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
			return Enemy;
		}

		static float TargetHealth(AActor& Target)
		{
			const UHealthComponent* Health = Target.FindComponentByClass<UHealthComponent>();
			return (Health != nullptr) ? Health->GetHealth() : -1.0f;
		}

		static UCombatComponent* TargetCombat(AActor& Target)
		{
			return Target.FindComponentByClass<UCombatComponent>();
		}

		// Retires a case target as a corpse: a dead combatant is never a
		// query target again (the temp world has no world context, so
		// DestroyActor is not an option - the M5-013 lesson).
		static void RetireTarget(AActor* Target)
		{
			if (Target != nullptr)
			{
				if (UHealthComponent* Health = Target->FindComponentByClass<UHealthComponent>())
				{
					Health->ApplyDamage(Health->GetMaxHealth());
				}
			}
		}

		// Advances the component by exactly Count single 1/60 s frames.
		void TickFrames(int32 Count)
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Combat->TickCombat(1.0f / 60.0f);
			}
		}

		// Starts one attack instance and ticks exactly to its first active
		// frame, where the hit lands.
		bool HitOnce(FAutomationTestBase& Test, const TCHAR* AttackId, int32 Facing)
		{
			const UAttackDefinition* Definition = FindDefinition(Test, AttackId);
			if (Definition == nullptr)
			{
				return false;
			}
			if (!Test.TestTrue(FString::Printf(TEXT("%s starts"), AttackId),
				Combat->TryStartAttack(FName(AttackId), Facing)))
			{
				return false;
			}
			TickFrames(Definition->ActiveWindow.StartFrame + 1);
			return true;
		}

		// Ticks the running instance past its final frame so the attacker is
		// Free again (and the shot's ledger keys release).
		void FinishInstance(FAutomationTestBase& Test, const TCHAR* AttackId)
		{
			const UAttackDefinition* Definition = FindDefinition(Test, AttackId);
			if (Definition != nullptr)
			{
				TickFrames(Definition->DurationFrames + 2);
			}
		}
	};
}

using namespace UE::UEMMO::Tasks::M5_014;

// ---------------------------------------------------------------------------
// DefaultRequestMatchesLegacyValues
// ---------------------------------------------------------------------------

// With the legacy default policy the resolver's request reproduces the old
// hardcoded behavior exactly: light_01 stuns 0.22 s (max override) with the
// facing-mirrored 90 cm/s knockback and no launch; the launcher on a fresh
// float cycle rises at the full 700, on the second launch at 490 (0.7), and
// from the third launch on the launch is refused while the damage, the stun
// and the interrupt still stand. The down-state durations ride the request
// at the legacy 0.45/0.25 values.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_014DefaultRequestMatchesLegacyValues,
	"UEMMO.Tasks.M5_014.DefaultRequestMatchesLegacyValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_014DefaultRequestMatchesLegacyValues::RunTest(const FString& Parameters)
{
	// 1. light_01 against a normal target: the legacy stagger/knockback shape.
	{
		const FReactionHitContext Context = M5_014_MakeContext(M5_014_MakeLight01(), M5_014_MakeNormalTarget());
		const FHitReactionRequest Request = ResolveHitReaction(Context);

		TestFalse("a normal light_01 hit is not refused by the state gate", Request.bRefuseHit);
		TestTrue("a normal light_01 hit carries no refuse reason",
			Request.RefuseReason == EReactionRefuseReason::None);
		TestTrue("the damage face applies", Request.bDamageApplies);
		TestTrue("the control face is accepted", Request.bControlAccepted);
		TestTrue("the stagger is accepted", Request.bStagger);
		TestEqual("the stun request carries the legacy light_01 0.22 s", Request.StunSeconds, 0.22f);
		TestTrue("the stun override is the legacy max rule", Request.StunOverride == EHitStunOverride::Max);
		TestTrue("the accepted hit interrupts the running attack", Request.bInterruptAttack);
		TestEqual("the impulse X is the knockback mirrored by facing +1", static_cast<float>(Request.ImpulseVelocity.X), 90.0f);
		TestTrue("the impulse Y is always 0 (the depth axis is never locked)", Request.ImpulseVelocity.Y == 0.0f);
		TestEqual("light_01 requests no launch Z", static_cast<float>(Request.ImpulseVelocity.Z), 0.0f);
		TestEqual("the admitted launch is 0", Request.LaunchCmPerSecond, 0.0f);
		TestFalse("light_01 requests no launch", Request.bLaunch);
		TestFalse("no launch implies no knockdown", Request.bKnockdown);
		TestFalse("no float-cycle scaling participates for light_01", Request.bLaunchAdmitted);
		TestEqual("the request carries the legacy knockdown 0.45 s", Request.KnockdownSeconds, 0.45f);
		TestEqual("the request carries the legacy recovering 0.25 s", Request.RecoveringSeconds, 0.25f);
	}

	// 2. launcher on a fresh float cycle: the full 700 launch.
	{
		FReactionHitContext Context = M5_014_MakeContext(M5_014_MakeLauncher(), M5_014_MakeNormalTarget());
		Context.bPerCycleLaunchScaling = true;
		Context.LaunchesUsedThisCycle = 0;
		const FHitReactionRequest Request = ResolveHitReaction(Context);

		TestTrue("the fresh-cycle launcher launch is accepted", Request.bLaunch);
		TestTrue("an accepted launch lands into the legacy knockdown", Request.bKnockdown);
		TestTrue("the float-cycle policy admitted the launch", Request.bLaunchAdmitted);
		TestEqual("the admitted launch is the legacy 700", Request.LaunchCmPerSecond, 700.0f);
		TestTrue("the launcher impulse is the legacy (70, 0, 700)",
			Request.ImpulseVelocity.Equals(FVector(70.0f, 0.0f, 700.0f), 1.0e-4f));
		TestEqual("the launcher stun request carries the legacy 1.0 s", Request.StunSeconds, 1.0f);
		TestTrue("the damage face stands next to the launch", Request.bDamageApplies);
	}

	// 3. launcher on the second launch of the cycle: the legacy 0.7 scale.
	{
		FReactionHitContext Context = M5_014_MakeContext(M5_014_MakeLauncher(), M5_014_MakeNormalTarget());
		Context.bPerCycleLaunchScaling = true;
		Context.LaunchesUsedThisCycle = 1;
		const FHitReactionRequest Request = ResolveHitReaction(Context);

		TestTrue("the second launch of the cycle is accepted", Request.bLaunch);
		TestEqual("the second launch rises at the legacy 490 (0.7 scale)", Request.LaunchCmPerSecond, 490.0f);
		TestTrue("the second launch impulse is the legacy (70, 0, 490)",
			Request.ImpulseVelocity.Equals(FVector(70.0f, 0.0f, 490.0f), 1.0e-4f));
	}

	// 4. launcher on the third launch of the cycle: the launch is refused
	//    while the damage, the stun and the interrupt still stand.
	{
		FReactionHitContext Context = M5_014_MakeContext(M5_014_MakeLauncher(), M5_014_MakeNormalTarget());
		Context.bPerCycleLaunchScaling = true;
		Context.LaunchesUsedThisCycle = 2;
		const FHitReactionRequest Request = ResolveHitReaction(Context);

		TestFalse("the third launch of the cycle is refused", Request.bLaunch);
		TestFalse("a refused launch implies no knockdown", Request.bKnockdown);
		TestFalse("the refused launch is not admitted", Request.bLaunchAdmitted);
		TestEqual("the refused launch admits 0", Request.LaunchCmPerSecond, 0.0f);
		TestTrue("the refused launch carries no launch Z", Request.ImpulseVelocity.Z == 0.0f);
		TestTrue("the refused launch keeps its X knockback", Request.ImpulseVelocity.X == 70.0f);
		TestTrue("the refused launcher still applies its damage (damage and dedup stand)", Request.bDamageApplies);
		TestTrue("the refused launcher still stuns", Request.bStagger);
		TestEqual("the refused launcher still requests its 1.0 s stun", Request.StunSeconds, 1.0f);
		TestTrue("the refused launcher still interrupts", Request.bInterruptAttack);
	}

	// 5. the legacy float-cycle scales pin the M1-025 constants exactly.
	{
		const FTargetReaction Policy = M5_014_MakeNormalTarget();
		TestEqual("the default policy scales launch 0 at 1.0", ResolveLaunchZScale(Policy, 0), 1.0f);
		TestEqual("the default policy scales launch 1 at 0.7", ResolveLaunchZScale(Policy, 1), 0.7f);
		TestEqual("the default policy refuses launch 2", ResolveLaunchZScale(Policy, 2), 0.0f);
		TestEqual("a negative count is defensively a fresh cycle", ResolveLaunchZScale(Policy, -1), 1.0f);
	}
	return true;
}

// ---------------------------------------------------------------------------
// PolicyValuesDriveStunAndImpulse
// ---------------------------------------------------------------------------

// Changing the FTargetReaction/FAttackReaction values changes the resolver's
// request without any class code change: the stagger gate drops the stun,
// the blanket control immunity strips every control kind while the damage
// stands, the damage immunity zeroes the damage face while the control
// stands, and the launch policy scales/refuses the launch impulses.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_014PolicyValuesDriveStunAndImpulse,
	"UEMMO.Tasks.M5_014.PolicyValuesDriveStunAndImpulse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_014PolicyValuesDriveStunAndImpulse::RunTest(const FString& Parameters)
{
	// 1. A stagger-gated target (bAllowStagger=false): no stun, damage stands.
	{
		FTargetReaction NoStagger = M5_014_MakeNormalTarget();
		NoStagger.PolicyId = TEXT("no_stagger");
		NoStagger.bAllowStagger = false;
		const FReactionHitContext Context = M5_014_MakeContext(M5_014_MakeLight01(), NoStagger);
		const FHitReactionRequest Request = ResolveHitReaction(Context);

		TestFalse("the stagger-gated target refuses the stagger", Request.bStagger);
		TestEqual("the refused stagger requests a 0 s stun", Request.StunSeconds, 0.0f);
		TestFalse("light_01 has no other control, so the control face is refused", Request.bControlAccepted);
		TestTrue("the damage face is independent of the control face", Request.bDamageApplies);
		TestTrue("a damage-only hit still interrupts the running attack", Request.bInterruptAttack);
		TestTrue("the stagger gate does not touch the knockback impulse",
			Request.ImpulseVelocity.Equals(FVector(90.0f, 0.0f, 0.0f), 1.0e-4f));
	}

	// 2. A super-armored target (bImmuneControl=true): full damage, zero control.
	{
		FTargetReaction SuperArmor = M5_014_MakeNormalTarget();
		SuperArmor.PolicyId = TEXT("super_armor");
		SuperArmor.bImmuneControl = true;
		FReactionHitContext Context = M5_014_MakeContext(M5_014_MakeLauncher(), SuperArmor);
		Context.bPerCycleLaunchScaling = true;
		const FHitReactionRequest Request = ResolveHitReaction(Context);

		TestFalse("the super-armored target accepts no stagger", Request.bStagger);
		TestFalse("the super-armored target accepts no launch", Request.bLaunch);
		TestFalse("the super-armored target accepts no knockdown", Request.bKnockdown);
		TestFalse("the control face is refused", Request.bControlAccepted);
		TestTrue("the super-armored target still takes the damage", Request.bDamageApplies);
		TestEqual("the refused launch carries no Z", static_cast<float>(Request.ImpulseVelocity.Z), 0.0f);
		TestTrue("the refused control keeps the X knockback request", Request.ImpulseVelocity.X == 70.0f);
	}

	// 3. A damage-immune target (bImmuneDamage=true): zero damage, control stands.
	{
		FTargetReaction Ghost = M5_014_MakeNormalTarget();
		Ghost.PolicyId = TEXT("ghost");
		Ghost.bImmuneDamage = true;
		const FReactionHitContext Context = M5_014_MakeContext(M5_014_MakeLight01(), Ghost);
		const FHitReactionRequest Request = ResolveHitReaction(Context);

		TestFalse("the damage-immune target takes no damage", Request.bDamageApplies);
		TestTrue("the damage-immune target still accepts the stagger (control immunity is a separate axis)", Request.bStagger);
		TestTrue("the control face stands", Request.bControlAccepted);
		TestEqual("the stagger request keeps its 0.22 s", Request.StunSeconds, 0.22f);
		TestTrue("a pure-control hit still interrupts the running attack", Request.bInterruptAttack);
		TestTrue("the knockback request carries no damage gate (the execution gates it)",
			Request.ImpulseVelocity.Equals(FVector(90.0f, 0.0f, 0.0f), 1.0e-4f));
	}

	// 4. A custom launch scale ({0.5}): the launcher rises at 350.
	{
		FTargetReaction HalfLaunch = M5_014_MakeNormalTarget();
		HalfLaunch.PolicyId = TEXT("half_launch");
		HalfLaunch.LaunchZScales = {0.5f};
		FReactionHitContext Context = M5_014_MakeContext(M5_014_MakeLauncher(), HalfLaunch);
		Context.bPerCycleLaunchScaling = true;
		Context.LaunchesUsedThisCycle = 0;
		const FHitReactionRequest Request = ResolveHitReaction(Context);

		TestEqual("a 0.5 launch scale admits 350", Request.LaunchCmPerSecond, 350.0f);
		TestTrue("the 0.5-scaled impulse is (70, 0, 350)",
			Request.ImpulseVelocity.Equals(FVector(70.0f, 0.0f, 350.0f), 1.0e-4f));
		// The scale clamps to the last entry for later launches.
		Context.LaunchesUsedThisCycle = 1;
		const FHitReactionRequest Second = ResolveHitReaction(Context);
		TestEqual("the single-entry scale clamps to 0.5 on the second launch", Second.LaunchCmPerSecond, 350.0f);
	}

	// 5. A tighter launch cap (MaxLaunchesPerAirCycle=1): the second launch is refused.
	{
		FTargetReaction SingleLaunch = M5_014_MakeNormalTarget();
		SingleLaunch.PolicyId = TEXT("single_launch");
		SingleLaunch.MaxLaunchesPerAirCycle = 1;
		FReactionHitContext Context = M5_014_MakeContext(M5_014_MakeLauncher(), SingleLaunch);
		Context.bPerCycleLaunchScaling = true;
		Context.LaunchesUsedThisCycle = 1;
		const FHitReactionRequest Request = ResolveHitReaction(Context);

		TestFalse("the capped second launch is refused", Request.bLaunch);
		TestFalse("the capped second launch is not admitted", Request.bLaunchAdmitted);
		TestEqual("the capped second launch admits 0", Request.LaunchCmPerSecond, 0.0f);
		TestTrue("the capped launcher still applies its damage", Request.bDamageApplies);
		TestTrue("the capped launcher still stuns", Request.bStagger);
	}

	// 6. A wider launch cap (MaxLaunchesPerAirCycle=3): the third launch
	//    reads the last scale entry (0.7 -> 490).
	{
		FTargetReaction TripleLaunch = M5_014_MakeNormalTarget();
		TripleLaunch.PolicyId = TEXT("triple_launch");
		TripleLaunch.MaxLaunchesPerAirCycle = 3;
		FReactionHitContext Context = M5_014_MakeContext(M5_014_MakeLauncher(), TripleLaunch);
		Context.bPerCycleLaunchScaling = true;
		Context.LaunchesUsedThisCycle = 2;
		const FHitReactionRequest Request = ResolveHitReaction(Context);

		TestTrue("the widened cap admits the third launch", Request.bLaunch);
		TestEqual("the third launch clamps to the last scale entry (490)", Request.LaunchCmPerSecond, 490.0f);
	}

	// 7. Explicit gate pairs: the launch gate and the knockdown gate.
	{
		FTargetReaction NoLaunch = M5_014_MakeNormalTarget();
		NoLaunch.PolicyId = TEXT("no_launch");
		NoLaunch.bAllowLaunch = false;
		FReactionHitContext Context = M5_014_MakeContext(M5_014_MakeLauncher(), NoLaunch);
		Context.bPerCycleLaunchScaling = true;
		const FHitReactionRequest Request = ResolveHitReaction(Context);
		TestFalse("the launch-gated target refuses the launch", Request.bLaunch);
		TestFalse("the refused launch implies no knockdown", Request.bKnockdown);
		TestTrue("the launch gate does not touch the damage", Request.bDamageApplies);

		FTargetReaction NoKnockdown = M5_014_MakeNormalTarget();
		NoKnockdown.PolicyId = TEXT("no_down");
		NoKnockdown.bAllowKnockdown = false;
		FReactionHitContext KnockdownContext = M5_014_MakeContext(M5_014_MakeLauncher(), NoKnockdown);
		KnockdownContext.bPerCycleLaunchScaling = true;
		const FHitReactionRequest KnockdownRequest = ResolveHitReaction(KnockdownContext);
		TestTrue("the knockdown-gated target still accepts the launch", KnockdownRequest.bLaunch);
		TestFalse("the knockdown-gated target refuses the knockdown", KnockdownRequest.bKnockdown);
	}

	// 8. Custom down-state durations ride the request.
	{
		FTargetReaction LongDown = M5_014_MakeNormalTarget();
		LongDown.PolicyId = TEXT("long_down");
		LongDown.KnockdownSeconds = 1.2f;
		LongDown.RecoveringSeconds = 0.8f;
		const FReactionHitContext Context = M5_014_MakeContext(M5_014_MakeLauncher(), LongDown);
		const FHitReactionRequest Request = ResolveHitReaction(Context);
		TestEqual("the request carries the custom knockdown 1.2 s", Request.KnockdownSeconds, 1.2f);
		TestEqual("the request carries the custom recovering 0.8 s", Request.RecoveringSeconds, 0.8f);
	}

	// 9. Facing -1 mirrors the X only; the Y depth stays unlocked (Y = 0).
	{
		FReactionHitContext LightContext = M5_014_MakeContext(M5_014_MakeLight01(), M5_014_MakeNormalTarget());
		LightContext.Facing = -1;
		const FHitReactionRequest LightRequest = ResolveHitReaction(LightContext);
		TestEqual("facing -1 mirrors the light_01 knockback to -90", static_cast<float>(LightRequest.ImpulseVelocity.X), -90.0f);
		TestTrue("facing -1 never adds a Y depth velocity", LightRequest.ImpulseVelocity.Y == 0.0f);

		FReactionHitContext LauncherContext = M5_014_MakeContext(M5_014_MakeLauncher(), M5_014_MakeNormalTarget());
		LauncherContext.Facing = -1;
		LauncherContext.bPerCycleLaunchScaling = true;
		const FHitReactionRequest LauncherRequest = ResolveHitReaction(LauncherContext);
		TestEqual("facing -1 mirrors the launcher knockback to -70", static_cast<float>(LauncherRequest.ImpulseVelocity.X), -70.0f);
		TestEqual("facing -1 keeps the launch Z", static_cast<float>(LauncherRequest.ImpulseVelocity.Z), 700.0f);
	}
	return true;
}

// ---------------------------------------------------------------------------
// StateGatesRefuseTheWholeHit
// ---------------------------------------------------------------------------

// The state gate reads the facts the caller feeds it: a dead target refuses
// the whole hit first (death has the highest priority, over the get-up
// protection too), a target inside its landing recovery refuses the whole
// hit, and an illegal damage context is not a state refusal - it carries no
// faces and the unified entry owns the refusal with its explicit reason.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_014StateGatesRefuseTheWholeHit,
	"UEMMO.Tasks.M5_014.StateGatesRefuseTheWholeHit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_014StateGatesRefuseTheWholeHit::RunTest(const FString& Parameters)
{
	// 1. A dead target: the whole hit is refused with nothing on any face.
	{
		FReactionHitContext Context = M5_014_MakeContext(M5_014_MakeLauncher(), M5_014_MakeNormalTarget());
		Context.bTargetDead = true;
		const FHitReactionRequest Request = ResolveHitReaction(Context);

		TestTrue("a dead target refuses the hit", Request.bRefuseHit);
		TestTrue("the dead-target refusal names death", Request.RefuseReason == EReactionRefuseReason::TargetDead);
		TestTrue("the dead-target refusal carries no face and no magnitude", M5_014_IsEmptyRequest(Request));
	}

	// 2. Death beats the get-up protection: both flags refuse as TargetDead.
	{
		FReactionHitContext Context = M5_014_MakeContext(M5_014_MakeLauncher(), M5_014_MakeNormalTarget());
		Context.bTargetDead = true;
		Context.bTargetInLandingRecovery = true;
		const FHitReactionRequest Request = ResolveHitReaction(Context);
		TestTrue("death has priority over the landing recovery", Request.bRefuseHit
			&& Request.RefuseReason == EReactionRefuseReason::TargetDead);
	}

	// 3. A get-up protected target (landing recovery): the whole hit refuses.
	{
		FReactionHitContext Context = M5_014_MakeContext(M5_014_MakeLauncher(), M5_014_MakeNormalTarget());
		Context.bTargetInLandingRecovery = true;
		const FHitReactionRequest Request = ResolveHitReaction(Context);

		TestTrue("a get-up protected target refuses the hit", Request.bRefuseHit);
		TestTrue("the get-up refusal names the protection", Request.RefuseReason == EReactionRefuseReason::GetUpProtection);
		TestTrue("the get-up refusal carries no face and no magnitude", M5_014_IsEmptyRequest(Request));
	}

	// 4. An illegal damage context: not a state refusal, but no faces.
	{
		FDamageProfile Broken = M5_014_MakeLight01();
		Broken.BaseDamage = std::numeric_limits<float>::quiet_NaN();
		const FReactionHitContext Context = M5_014_MakeContext(Broken, M5_014_MakeNormalTarget());
		const FHitReactionRequest Request = ResolveHitReaction(Context);

		TestFalse("an illegal damage context is not a state refusal", Request.bRefuseHit);
		TestTrue("the illegal context names no state reason", Request.RefuseReason == EReactionRefuseReason::None);
		TestTrue("the illegal context carries no face and no magnitude", M5_014_IsEmptyRequest(Request));
	}
	return true;
}

// ---------------------------------------------------------------------------
// BypassPoiseKeepsExplicitGates
// ---------------------------------------------------------------------------

// The attack side's BypassPoise penetrates the target's poise threshold
// only - never the explicit allow gates, a blanket control immunity or a
// get-up window. On the poise-free legacy target the penetration changes
// nothing: the request is identical to EControlPenetration::None.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_014BypassPoiseKeepsExplicitGates,
	"UEMMO.Tasks.M5_014.BypassPoiseKeepsExplicitGates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_014BypassPoiseKeepsExplicitGates::RunTest(const FString& Parameters)
{
	// 1. On the poise-free normal target, BypassPoise equals None.
	{
		FReactionHitContext NoneContext = M5_014_MakeContext(M5_014_MakeLauncher(), M5_014_MakeNormalTarget());
		NoneContext.bPerCycleLaunchScaling = true;
		const FHitReactionRequest NoneRequest = ResolveHitReaction(NoneContext);

		FReactionHitContext BypassContext = NoneContext;
		BypassContext.AttackReaction.ControlPenetration = EControlPenetration::BypassPoise;
		const FHitReactionRequest BypassRequest = ResolveHitReaction(BypassContext);

		TestTrue("BypassPoise matches None on a poise-free target (stun)",
			BypassRequest.bStagger == NoneRequest.bStagger && BypassRequest.StunSeconds == NoneRequest.StunSeconds);
		TestTrue("BypassPoise matches None on a poise-free target (launch)",
			BypassRequest.bLaunch == NoneRequest.bLaunch
			&& BypassRequest.LaunchCmPerSecond == NoneRequest.LaunchCmPerSecond);
		TestTrue("BypassPoise matches None on a poise-free target (impulse)",
			BypassRequest.ImpulseVelocity.Equals(NoneRequest.ImpulseVelocity, 1.0e-4f));
	}

	// 2. BypassPoise never bypasses the blanket control immunity.
	{
		FTargetReaction SuperArmor = M5_014_MakeNormalTarget();
		SuperArmor.PolicyId = TEXT("bypass_super_armor");
		SuperArmor.bImmuneControl = true;
		FReactionHitContext Context = M5_014_MakeContext(M5_014_MakeLauncher(), SuperArmor);
		Context.AttackReaction.ControlPenetration = EControlPenetration::BypassPoise;
		const FHitReactionRequest Request = ResolveHitReaction(Context);

		TestFalse("BypassPoise still respects the blanket control immunity (stagger)", Request.bStagger);
		TestFalse("BypassPoise still respects the blanket control immunity (launch)", Request.bLaunch);
		TestTrue("the immune control keeps the damage standing", Request.bDamageApplies);
	}

	// 3. BypassPoise never bypasses the explicit stagger gate.
	{
		FTargetReaction NoStagger = M5_014_MakeNormalTarget();
		NoStagger.PolicyId = TEXT("bypass_no_stagger");
		NoStagger.bAllowStagger = false;
		FReactionHitContext Context = M5_014_MakeContext(M5_014_MakeLight01(), NoStagger);
		Context.AttackReaction.ControlPenetration = EControlPenetration::BypassPoise;
		const FHitReactionRequest Request = ResolveHitReaction(Context);

		TestFalse("BypassPoise still respects the explicit stagger gate", Request.bStagger);
		TestEqual("the gated stagger stays a 0 s request", Request.StunSeconds, 0.0f);
		TestTrue("the gated stagger keeps the damage standing", Request.bDamageApplies);
	}
	return true;
}

// ---------------------------------------------------------------------------
// ComponentDefaultPolicyKeepsLegacyBehavior
// ---------------------------------------------------------------------------

// Through the real CombatComponent active-window pipeline the default policy
// keeps the legacy behavior byte for byte: light_01 damages 10, stuns the
// victim and confirms the facing-mirrored impulse payload; the launcher on a
// training enemy runs the legacy float cycle 700 -> 490 -> refused while the
// damage and the cycle counting follow the M1-025 semantics.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_014ComponentDefaultPolicyKeepsLegacyBehavior,
	"UEMMO.Tasks.M5_014.ComponentDefaultPolicyKeepsLegacyBehavior",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_014ComponentDefaultPolicyKeepsLegacyBehavior::RunTest(const FString& Parameters)
{
	UWorld* World = M5_014_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_014_WorldScope WorldScope;
	WorldScope.World = World;

	// 1. light_01 against a box target with a victim combat component.
	{
		FM5_014_Scene Scene;
		if (!Scene.Build(*this, *World, M5_014_SceneBase))
		{
			return true;
		}
		const UAttackDefinition* Definition = Scene.FindDefinition(*this, TEXT("light_01"));
		if (Definition == nullptr)
		{
			return true;
		}
		AActor* Target = Scene.SpawnBoxTarget(*World, *Definition, /*Facing*/ 1, /*bWithCombat*/ true);
		if (!TestNotNull(TEXT("light_01 spawns its target"), Target))
		{
			return true;
		}

		if (!Scene.HitOnce(*this, TEXT("light_01"), /*Facing*/ 1))
		{
			return true;
		}
		TestEqual("the default light_01 hit lands exactly once", Scene.Events.HitCount, 1);
		TestEqual("the default light_01 removes exactly the legacy 10", FM5_014_Scene::TargetHealth(*Target), 90.0f);
		TestTrue("the confirmed payload carries the legacy facing-mirrored knockback",
			Scene.Events.LastHit.Impulse.Equals(FVector(90.0f, 0.0f, 0.0f), 1.0e-4f));
		TestEqual("the confirmed payload carries the legacy 0.22 s stun request",
			Scene.Events.LastHit.StunSeconds, 0.22f);
		UCombatComponent* VictimCombat = FM5_014_Scene::TargetCombat(*Target);
		if (TestNotNull(TEXT("precondition: the target carries a combat component"), VictimCombat))
		{
			TestTrue("the default policy stuns the victim through the legacy path",
				VictimCombat->GetSnapshot().ActionState == ECombatActionState::HitStun);
		}
		TestEqual("the running instance holds one unified ledger event",
			Scene.Combat->GetUnifiedLedgerRecordedEventCount(), 1);
		TestEqual("the running instance holds one unified ledger control",
			Scene.Combat->GetUnifiedLedgerAcceptedControlCount(), 1);

		Scene.FinishInstance(*this, TEXT("light_01"));
		TestEqual("the finished instance released its ledger events",
			Scene.Combat->GetUnifiedLedgerRecordedEventCount(), 0);
		FM5_014_Scene::RetireTarget(Target);
	}

	// 2. launcher against a training enemy: the legacy float cycle.
	{
		FM5_014_Scene Scene;
		if (!Scene.Build(*this, *World, M5_014_SceneBase + FVector(3000.0, 0.0, 0.0)))
		{
			return true;
		}
		const UAttackDefinition* Definition = Scene.FindDefinition(*this, TEXT("launcher"));
		if (Definition == nullptr)
		{
			return true;
		}
		ATrainingEnemy* Enemy = Scene.SpawnTrainingEnemy(*World, *Definition, /*Facing*/ 1);
		if (!TestNotNull(TEXT("the launcher spawns its training enemy"), Enemy))
		{
			return true;
		}

		// Launch 1: the full 700, counted into the float cycle.
		if (!Scene.HitOnce(*this, TEXT("launcher"), /*Facing*/ 1))
		{
			return true;
		}
		TestEqual("the first launcher hit lands exactly once", Scene.Events.HitCount, 1);
		TestTrue("the first launcher payload carries the legacy (70, 0, 700)",
			Scene.Events.LastHit.Impulse.Equals(FVector(70.0f, 0.0f, 700.0f), 1.0e-4f));
		TestEqual("the first launcher hit removes the legacy 18", Enemy->GetHealthComponent()->GetHealth(), 82.0f);
		TestEqual("the first launcher launch is counted", Enemy->GetLauncherCycleCount(), 1);
		Scene.FinishInstance(*this, TEXT("launcher"));

		// Launch 2: 490 (0.7), counted again.
		if (!Scene.HitOnce(*this, TEXT("launcher"), /*Facing*/ 1))
		{
			return true;
		}
		TestEqual("the second launcher hit lands exactly once", Scene.Events.HitCount, 2);
		TestTrue("the second launcher payload carries the legacy (70, 0, 490)",
			Scene.Events.LastHit.Impulse.Equals(FVector(70.0f, 0.0f, 490.0f), 1.0e-4f));
		TestEqual("the second launcher launch is counted", Enemy->GetLauncherCycleCount(), 2);
		Scene.FinishInstance(*this, TEXT("launcher"));

		// Launch 3: refused (Z 0), damage and ledger still apply, count frozen.
		if (!Scene.HitOnce(*this, TEXT("launcher"), /*Facing*/ 1))
		{
			return true;
		}
		TestEqual("the third launcher hit lands exactly once", Scene.Events.HitCount, 3);
		TestTrue("the third launcher payload carries no launch Z",
			Scene.Events.LastHit.Impulse.Equals(FVector(70.0f, 0.0f, 0.0f), 1.0e-4f));
		TestEqual("the third launcher still removes the legacy 18", Enemy->GetHealthComponent()->GetHealth(), 46.0f);
		TestEqual("the refused launch is not counted", Enemy->GetLauncherCycleCount(), 2);
		Scene.FinishInstance(*this, TEXT("launcher"));
	}
	return true;
}

// ---------------------------------------------------------------------------
// ComponentPolicyDrivesStunAndImpulse
// ---------------------------------------------------------------------------

// The acceptance core: injecting policy VALUES on the attacker's combat
// component changes the victim-visible behavior (the stun gate, the pure
// control shape, the launch impulses) with zero class-code changes - the
// same pipeline code produces different stun end states and impulses.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_014ComponentPolicyDrivesStunAndImpulse,
	"UEMMO.Tasks.M5_014.ComponentPolicyDrivesStunAndImpulse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_014ComponentPolicyDrivesStunAndImpulse::RunTest(const FString& Parameters)
{
	UWorld* World = M5_014_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_014_WorldScope WorldScope;
	WorldScope.World = World;

	// 1. A stagger-gated policy: the hit lands, the victim stays Free.
	{
		FM5_014_Scene Scene;
		if (!Scene.Build(*this, *World, M5_014_SceneBase + FVector(6000.0, 0.0, 0.0)))
		{
			return true;
		}
		FTargetReaction NoStagger = MakeLegacyNormalTargetReaction();
		NoStagger.PolicyId = TEXT("component_no_stagger");
		NoStagger.bAllowStagger = false;
		Scene.Combat->SetTargetReactionPolicy(NoStagger);

		const UAttackDefinition* Definition = Scene.FindDefinition(*this, TEXT("light_01"));
		if (Definition == nullptr)
		{
			return true;
		}
		AActor* Target = Scene.SpawnBoxTarget(*World, *Definition, 1, /*bWithCombat*/ true);
		if (!TestNotNull(TEXT("the stagger case spawns its target"), Target))
		{
			return true;
		}
		if (!Scene.HitOnce(*this, TEXT("light_01"), 1))
		{
			return true;
		}
		TestEqual("the stagger-gated hit still removes the legacy 10", FM5_014_Scene::TargetHealth(*Target), 90.0f);
		UCombatComponent* VictimCombat = FM5_014_Scene::TargetCombat(*Target);
		if (TestNotNull(TEXT("precondition: the stagger case target carries a combat component"), VictimCombat))
		{
			TestTrue("the stagger-gated victim stays Free (no stun deadline was set)",
				VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);
		}
		FM5_014_Scene::RetireTarget(Target);
	}

	// 2. A super-armored policy: damage lands, the victim stays Free.
	{
		FM5_014_Scene Scene;
		if (!Scene.Build(*this, *World, M5_014_SceneBase + FVector(9000.0, 0.0, 0.0)))
		{
			return true;
		}
		FTargetReaction SuperArmor = MakeLegacyNormalTargetReaction();
		SuperArmor.PolicyId = TEXT("component_super_armor");
		SuperArmor.bImmuneControl = true;
		Scene.Combat->SetTargetReactionPolicy(SuperArmor);

		const UAttackDefinition* Definition = Scene.FindDefinition(*this, TEXT("light_01"));
		if (Definition == nullptr)
		{
			return true;
		}
		AActor* Target = Scene.SpawnBoxTarget(*World, *Definition, 1, /*bWithCombat*/ true);
		if (!TestNotNull(TEXT("the super-armor case spawns its target"), Target))
		{
			return true;
		}
		if (!Scene.HitOnce(*this, TEXT("light_01"), 1))
		{
			return true;
		}
		TestEqual("the super-armored victim still takes the legacy 10", FM5_014_Scene::TargetHealth(*Target), 90.0f);
		UCombatComponent* VictimCombat = FM5_014_Scene::TargetCombat(*Target);
		if (TestNotNull(TEXT("precondition: the super-armor case target carries a combat component"), VictimCombat))
		{
			TestTrue("the super-armored victim keeps its action state",
				VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);
		}
		FM5_014_Scene::RetireTarget(Target);
	}

	// 3. A damage-immune policy: no damage, the pure-control stun still lands.
	{
		FM5_014_Scene Scene;
		if (!Scene.Build(*this, *World, M5_014_SceneBase + FVector(12000.0, 0.0, 0.0)))
		{
			return true;
		}
		FTargetReaction Ghost = MakeLegacyNormalTargetReaction();
		Ghost.PolicyId = TEXT("component_ghost");
		Ghost.bImmuneDamage = true;
		Scene.Combat->SetTargetReactionPolicy(Ghost);

		const UAttackDefinition* Definition = Scene.FindDefinition(*this, TEXT("light_01"));
		if (Definition == nullptr)
		{
			return true;
		}
		AActor* Target = Scene.SpawnBoxTarget(*World, *Definition, 1, /*bWithCombat*/ true);
		if (!TestNotNull(TEXT("the damage-immune case spawns its target"), Target))
		{
			return true;
		}
		if (!Scene.HitOnce(*this, TEXT("light_01"), 1))
		{
			return true;
		}
		TestEqual("the damage-immune victim keeps its full health", FM5_014_Scene::TargetHealth(*Target), 100.0f);
		TestEqual("a zero-damage hit bridges no legacy damage event", Scene.Events.HitCount, 0);
		UCombatComponent* VictimCombat = FM5_014_Scene::TargetCombat(*Target);
		if (TestNotNull(TEXT("precondition: the damage-immune case target carries a combat component"), VictimCombat))
		{
			TestTrue("the pure-control hit still stuns the victim through the legacy path",
				VictimCombat->GetSnapshot().ActionState == ECombatActionState::HitStun);
		}
		FM5_014_Scene::RetireTarget(Target);
	}

	// 4. A {0.5} launch scale policy: the launcher rises at 350, twice.
	{
		FM5_014_Scene Scene;
		if (!Scene.Build(*this, *World, M5_014_SceneBase + FVector(15000.0, 0.0, 0.0)))
		{
			return true;
		}
		FTargetReaction HalfLaunch = MakeLegacyNormalTargetReaction();
		HalfLaunch.PolicyId = TEXT("component_half_launch");
		HalfLaunch.LaunchZScales = {0.5f};
		Scene.Combat->SetTargetReactionPolicy(HalfLaunch);

		const UAttackDefinition* Definition = Scene.FindDefinition(*this, TEXT("launcher"));
		if (Definition == nullptr)
		{
			return true;
		}
		ATrainingEnemy* Enemy = Scene.SpawnTrainingEnemy(*World, *Definition, 1);
		if (!TestNotNull(TEXT("the half-launch case spawns its training enemy"), Enemy))
		{
			return true;
		}

		if (!Scene.HitOnce(*this, TEXT("launcher"), 1))
		{
			return true;
		}
		TestTrue("the half-scale policy launch 1 payload is (70, 0, 350)",
			Scene.Events.LastHit.Impulse.Equals(FVector(70.0f, 0.0f, 350.0f), 1.0e-4f));
		TestEqual("the half-scale first launch is counted", Enemy->GetLauncherCycleCount(), 1);
		Scene.FinishInstance(*this, TEXT("launcher"));

		if (!Scene.HitOnce(*this, TEXT("launcher"), 1))
		{
			return true;
		}
		TestTrue("the half-scale policy launch 2 clamps to (70, 0, 350)",
			Scene.Events.LastHit.Impulse.Equals(FVector(70.0f, 0.0f, 350.0f), 1.0e-4f));
		TestEqual("the half-scale second launch is counted", Enemy->GetLauncherCycleCount(), 2);
		Scene.FinishInstance(*this, TEXT("launcher"));
	}

	// 5. A tighter cap (MaxLaunchesPerAirCycle=1): the second launcher is a
	//    damage-only hit with no launch Z and no cycle count.
	{
		FM5_014_Scene Scene;
		if (!Scene.Build(*this, *World, M5_014_SceneBase + FVector(18000.0, 0.0, 0.0)))
		{
			return true;
		}
		FTargetReaction SingleLaunch = MakeLegacyNormalTargetReaction();
		SingleLaunch.PolicyId = TEXT("component_single_launch");
		SingleLaunch.MaxLaunchesPerAirCycle = 1;
		Scene.Combat->SetTargetReactionPolicy(SingleLaunch);

		const UAttackDefinition* Definition = Scene.FindDefinition(*this, TEXT("launcher"));
		if (Definition == nullptr)
		{
			return true;
		}
		ATrainingEnemy* Enemy = Scene.SpawnTrainingEnemy(*World, *Definition, 1);
		if (!TestNotNull(TEXT("the single-launch case spawns its training enemy"), Enemy))
		{
			return true;
		}

		if (!Scene.HitOnce(*this, TEXT("launcher"), 1))
		{
			return true;
		}
		TestTrue("the capped first launch keeps the full 700",
			Scene.Events.LastHit.Impulse.Equals(FVector(70.0f, 0.0f, 700.0f), 1.0e-4f));
		TestEqual("the capped first launch is counted", Enemy->GetLauncherCycleCount(), 1);
		Scene.FinishInstance(*this, TEXT("launcher"));

		if (!Scene.HitOnce(*this, TEXT("launcher"), 1))
		{
			return true;
		}
		TestTrue("the capped second launcher payload carries no launch Z",
			Scene.Events.LastHit.Impulse.Equals(FVector(70.0f, 0.0f, 0.0f), 1.0e-4f));
		TestEqual("the capped second launcher still removes the legacy 18",
			Enemy->GetHealthComponent()->GetHealth(), 64.0f);
		TestEqual("the capped refused launch is not counted", Enemy->GetLauncherCycleCount(), 1);
		Scene.FinishInstance(*this, TEXT("launcher"));
	}
	return true;
}

// ---------------------------------------------------------------------------
// InterruptAndResetClearStunDeadline
// ---------------------------------------------------------------------------

// The component stays the only HitStun timing and state source: an accepted
// hit interrupts the victim's running attack (no Finished broadcast shape -
// the instance is torn down) and the stun is re-applied fresh; ResetCombat
// during the stun returns the component to Free with the deadline cleared,
// so a fresh attack starts and advances normally (the M1-020/M1-026
// semantics, re-pinned through the M5-014 resolver pipeline).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_014InterruptAndResetClearStunDeadline,
	"UEMMO.Tasks.M5_014.InterruptAndResetClearStunDeadline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_014InterruptAndResetClearStunDeadline::RunTest(const FString& Parameters)
{
	UWorld* World = M5_014_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_014_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_014_Scene Scene;
	if (!Scene.Build(*this, *World, M5_014_SceneBase + FVector(21000.0, 0.0, 0.0)))
	{
		return true;
	}
	const UAttackDefinition* Definition = Scene.FindDefinition(*this, TEXT("light_01"));
	if (Definition == nullptr)
	{
		return true;
	}
	AActor* Target = Scene.SpawnBoxTarget(*World, *Definition, 1, /*bWithCombat*/ true, /*bTargetWithCatalog*/ true);
	if (!TestNotNull(TEXT("the interrupt case spawns its target"), Target))
	{
		return true;
	}
	UCombatComponent* VictimCombat = FM5_014_Scene::TargetCombat(*Target);
	if (!TestNotNull(TEXT("precondition: the target carries a combat component"), VictimCombat))
	{
		return true;
	}

	// 1. The victim is mid-attack when the hit lands: the attack is
	//    interrupted and the stun is applied fresh.
	if (!TestTrue(TEXT("precondition: the victim starts its own light_01"),
		VictimCombat->TryStartAttack(FName(TEXT("light_01")), /*Facing*/ -1)))
	{
		return true;
	}
	if (!Scene.HitOnce(*this, TEXT("light_01"), /*Facing*/ 1))
	{
		return true;
	}
	{
		const FCombatSnapshot Interrupted = VictimCombat->GetSnapshot();
		TestTrue("the accepted hit interrupted the victim's running attack",
			Interrupted.ActionState == ECombatActionState::HitStun && Interrupted.AttackId == NAME_None
			&& Interrupted.InstanceId == 0);
	}

	// 2. The fresh stun deadline runs on the victim's own clock and ends
	//    back at Free (the component is the only timing source).
	VictimCombat->SetInputClockSeconds(0.1);
	VictimCombat->TickCombat(1.0f / 60.0f);
	TestTrue("the fresh stun still holds before its deadline",
		VictimCombat->GetSnapshot().ActionState == ECombatActionState::HitStun);
	VictimCombat->SetInputClockSeconds(0.3);
	VictimCombat->TickCombat(1.0f / 60.0f);
	TestTrue("the fresh stun ends at Free on the victim's clock",
		VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);

	// 3. A reset during a stun clears the deadline bookkeeping: the
	//    component returns to Free and a fresh attack starts and advances.
	if (!TestTrue(TEXT("precondition: the victim starts another attack"),
		VictimCombat->TryStartAttack(FName(TEXT("light_01")), -1)))
	{
		return true;
	}
	// The attacker's first instance is still running: finish it before the
	// second hit (one attack at a time per component).
	Scene.FinishInstance(*this, TEXT("light_01"));
	Scene.Combat->SetTargetReactionPolicy(MakeLegacyNormalTargetReaction());
	if (!Scene.HitOnce(*this, TEXT("light_01"), 1))
	{
		return true;
	}
	TestTrue("precondition: the second hit stunned the victim again",
		VictimCombat->GetSnapshot().ActionState == ECombatActionState::HitStun);
	VictimCombat->ResetCombat();
	TestTrue("the reset during a stun returns the component to Free",
		VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	if (!TestTrue(TEXT("a fresh attack starts after the reset"),
		VictimCombat->TryStartAttack(FName(TEXT("light_01")), -1)))
	{
		return true;
	}
	VictimCombat->SetInputClockSeconds(100.0);
	VictimCombat->TickCombat(1.0f / 60.0f);
	TestTrue("the fresh attack advances past its first frame (no stale stun deadline interferes)",
		VictimCombat->GetSnapshot().Frame >= 0
		&& VictimCombat->GetSnapshot().ActionState == ECombatActionState::Attacking);
	return true;
}

#endif
