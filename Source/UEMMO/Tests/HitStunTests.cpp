#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatGeometry.h"
#include "../Combat/CombatHitQuery.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/TrainingEnemy.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_020
{
	// Remote base so the temp world can never collide with arena content.
	// X horizontal, Y depth, Z height; the base Z doubles as the shared feet
	// level of the ground-level combatants (box actors root at their feet).
	const FVector M1_020_SceneBase(40000.0, 45000.0, 600.0);

	// Half extent of one test body box. The bodies are deliberately taller than
	// the M1-019 ones: two ground-level combatants 95 cm apart must overlap each
	// other's light_01 hit box (center Z = feet + 90, half extent Z 70), which
	// needs a body reaching well above the feet level. Body Z range at the base
	// [505, 695] vs hit box Z range [620, 760] keeps a solid 75 cm overlap.
	const FVector M1_020_BodyHalfExtent(20.0, 20.0, 95.0);

	// World acquisition, same order as the M1-018/M1-019 pattern: a private
	// temp world first, the shared game world as fallback.
	static UWorld* M1_020_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M1_020_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M1_020 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M1_020 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	// Spawns a plain actor with one query-enabled box root at Location.
	// bWithHealth attaches a fresh UHealthComponent; bWithCombat attaches a
	// fresh UCombatComponent and (when given) the catalog for its own attacks.
	static AActor* M1_020_SpawnCombatant(UWorld& World, const FVector& Location, UAttackCatalog* Catalog, bool bWithHealth, bool bWithCombat)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}

		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M1_020_Body"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M1_020_BodyHalfExtent);
		Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Body->SetCollisionObjectType(ECC_Pawn);
		Body->SetCollisionResponseToAllChannels(ECR_Ignore);
		Body->RegisterComponent();
		Body->SetWorldLocation(Location);

		if (bWithHealth)
		{
			UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M1_020_Health"));
			Health->RegisterComponent();
		}
		if (bWithCombat)
		{
			UCombatComponent* Combat = NewObject<UCombatComponent>(Actor, TEXT("M1_020_Combat"));
			Combat->RegisterComponent();
			if (Catalog != nullptr)
			{
				Combat->InitializeFromCatalog(Catalog);
			}
		}
		return Actor;
	}

	// Loads the real read-only catalog from Config/DefaultGame.ini (the same
	// loading path production and the earlier combat tests use). Returns null
	// after reporting the failure so the caller can bail out early.
	static UAttackCatalog* M1_020_NewCatalog(FAutomationTestBase& Test)
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

	// Builds one synthetic accepted hit for the victim-side entry. Only the
	// fields NotifyHitReceived consumes are filled (Target identity and the
	// requested stun); the full payload contract was covered by M1-019.
	static FCombatHit M1_020_MakeStunHit(AActor* Victim, float StunSeconds)
	{
		FCombatHit Hit;
		Hit.Target = Victim;
		Hit.StunSeconds = StunSeconds;
		return Hit;
	}

	// Advances the component by exactly Count single 1/60 s frames.
	static void M1_020_TickFrames(UCombatComponent& Combat, int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Combat.TickCombat(1.0f / 60.0f);
		}
	}

	static float M1_020_HealthOf(const AActor* Actor)
	{
		const UHealthComponent* Health = (Actor != nullptr) ? Actor->FindComponentByClass<UHealthComponent>() : nullptr;
		return (Health != nullptr) ? Health->GetHealth() : -1.0f;
	}

	// Counts HitConfirmed broadcasts (hits this component DEALT) of one combatant.
	struct FM1_020_HitEvents
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

	// One mutual scene: the real catalog and two ground-level combatants 95 cm
	// apart on X, each with health and a combat component. The attacker (A) at
	// the base faces +1, the victim (B) faces -1, so each one's light_01 hit
	// box covers the other's body and both directions of an exchange are live.
	struct FM1_020_MutualScene
	{
		UAttackCatalog* Catalog = nullptr;
		const UAttackDefinition* Definition = nullptr;
		AActor* Attacker = nullptr;
		AActor* Victim = nullptr;
		UCombatComponent* CombatA = nullptr;
		UCombatComponent* CombatB = nullptr;
		FM1_020_HitEvents EventsA;
		FM1_020_HitEvents EventsB;

		// Builds both combatants and asserts the geometry preconditions with
		// the real read-only query. Returns false after reporting the problem
		// so the caller can bail out early.
		bool Build(FAutomationTestBase& Test, UWorld& World)
		{
			Catalog = M1_020_NewCatalog(Test);
			if (Catalog == nullptr)
			{
				return false;
			}
			Definition = Catalog->Find(FName(TEXT("light_01")));
			if (!Test.TestTrue(TEXT("the catalog holds light_01"), Definition != nullptr))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("light_01 keeps the design hit stun of 0.22 s"),
				Definition->HitStunSeconds > 0.0f && FMath::IsNearlyEqual(Definition->HitStunSeconds, 0.22f, 1.0e-6f)))
			{
				return false;
			}

			Attacker = M1_020_SpawnCombatant(World, M1_020_SceneBase, Catalog, /*bWithHealth*/ true, /*bWithCombat*/ true);
			Victim = M1_020_SpawnCombatant(World, M1_020_SceneBase + FVector(95.0, 0.0, 0.0), Catalog, /*bWithHealth*/ true, /*bWithCombat*/ true);
			if (!Test.TestNotNull(TEXT("the attacker spawns"), Attacker)
				|| !Test.TestNotNull(TEXT("the victim spawns"), Victim))
			{
				return false;
			}
			CombatA = Attacker->FindComponentByClass<UCombatComponent>();
			CombatB = Victim->FindComponentByClass<UCombatComponent>();
			if (!Test.TestTrue(TEXT("both combatants carry a combat component"), CombatA != nullptr && CombatB != nullptr))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("both combatants start alive at full health"),
				FMath::IsNearlyEqual(M1_020_HealthOf(Attacker), 100.0f) && FMath::IsNearlyEqual(M1_020_HealthOf(Victim), 100.0f)))
			{
				return false;
			}

			// Geometry preconditions through the real query: each body sits
			// inside the other's light_01 hit box for the chosen facings.
			const FCombatHitBox BoxA = ComputeHitBox(M1_020_SceneBase, 1, *Definition);
			const TArray<TWeakObjectPtr<AActor>> SeenByA = QueryTargets(&World, BoxA, Attacker, /*AttackerTeam*/ 0);
			if (!Test.TestTrue(TEXT("the attacker's hit box sees the victim"),
				SeenByA.Num() == 1 && SeenByA[0].Get() == Victim))
			{
				return false;
			}
			const FCombatHitBox BoxB = ComputeHitBox(Victim->GetActorLocation(), -1, *Definition);
			const TArray<TWeakObjectPtr<AActor>> SeenByB = QueryTargets(&World, BoxB, Victim, /*AttackerTeam*/ 0);
			if (!Test.TestTrue(TEXT("the victim's hit box sees the attacker"),
				SeenByB.Num() == 1 && SeenByB[0].Get() == Attacker))
			{
				return false;
			}

			EventsA.Bind(*CombatA);
			EventsB.Bind(*CombatB);
			return true;
		}
	};
}

using namespace UE::UEMMO::Tasks::M1_020;

// The core acceptance case: the victim is mid light_01 windup (its active
// window [7,11) never reached) when the attacker's first active frame lands an
// accepted hit. The hit interrupts the victim's attack immediately: the victim
// enters HitStun with the instance torn down, its own window never deducts the
// attacker, and after the 0.22 s stun the victim is Free again and a fresh
// instance can hit normally.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_020AttackInterruptedBeforeActiveWindowNeverHits,
	"UEMMO.Tasks.M1_020.AttackInterruptedBeforeActiveWindowNeverHits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_020AttackInterruptedBeforeActiveWindowNeverHits::RunTest(const FString& Parameters)
{
	UWorld* World = M1_020_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_020_MutualScene Scene;
	if (!Scene.Build(*this, *World))
	{
		return true;
	}

	// The victim starts its attack and never ticks it: it sits mid windup
	// (Attacking, window not reached) while the attacker advances alone.
	if (!TestTrue(TEXT("the victim's light_01 starts"), Scene.CombatB->TryStartAttack(FName(TEXT("light_01")), -1)))
	{
		return true;
	}
	TestTrue(TEXT("precondition: the victim is attacking"),
		Scene.CombatB->GetSnapshot().ActionState == ECombatActionState::Attacking);
	Scene.CombatB->SetInputClockSeconds(0.0);

	if (!TestTrue(TEXT("the attacker's light_01 starts"), Scene.CombatA->TryStartAttack(FName(TEXT("light_01")), 1)))
	{
		return true;
	}
	// Frames 0..7: the first active frame (7) lands the accepted hit.
	M1_020_TickFrames(*Scene.CombatA, 8);
	TestEqual(TEXT("the attacker hit exactly once"), Scene.EventsA.HitCount, 1);
	TestEqual(TEXT("the accepted hit removed exactly the base damage"), M1_020_HealthOf(Scene.Victim), 90.0f);

	// The hit interrupted the victim: the state is HitStun and the instance is
	// torn down (no attack id, no instance id).
	TestTrue(TEXT("the victim is in HitStun after the accepted hit"),
		Scene.CombatB->GetSnapshot().ActionState == ECombatActionState::HitStun);
	TestTrue(TEXT("the interrupted attack id is cleared"),
		Scene.CombatB->GetSnapshot().AttackId == NAME_None);
	TestEqual(TEXT("the interrupted instance id is cleared"), Scene.CombatB->GetSnapshot().InstanceId, uint64(0));

	// The victim's own window never deducts the attacker: no HitConfirmed from
	// the victim and the attacker keeps full health.
	TestEqual(TEXT("the interrupted victim never dealt a hit"), Scene.EventsB.HitCount, 0);
	TestEqual(TEXT("the attacker keeps full health while the victim was interrupted"), M1_020_HealthOf(Scene.Attacker), 100.0f);

	// Attacks are refused during the stun (and refuse without minting one).
	TestFalse(TEXT("TryStartAttack is refused during the stun"),
		Scene.CombatB->TryStartAttack(FName(TEXT("light_01")), -1));
	TestTrue(TEXT("the refusal left the stun untouched"),
		Scene.CombatB->GetSnapshot().ActionState == ECombatActionState::HitStun);
	TestEqual(TEXT("the refusal minted no instance"), Scene.CombatB->GetSnapshot().InstanceId, uint64(0));

	// The input cache still receives during the stun (M1-004 semantics).
	FBufferedCombatInput Input;
	Input.Sequence = 1;
	Input.Action = ECombatInput::Light;
	Input.PressedAt = 0.1;
	Scene.CombatB->QueueInput(Input);
	FBufferedCombatInput Peeked;
	TestTrue(TEXT("the buffered input is still received during the stun"), Scene.CombatB->PeekInputBuffer(Peeked));
	TestEqual(TEXT("the buffered input kept its sequence"), Peeked.Sequence, uint64(1));
	TestEqual(TEXT("the buffer holds the input queued during the stun"), Scene.CombatB->GetSnapshot().BufferSize, 1);

	// Ticking inside the stun window changes nothing about the outcome.
	M1_020_TickFrames(*Scene.CombatB, 5);
	TestTrue(TEXT("the victim is still stunned before 0.22 s"),
		Scene.CombatB->GetSnapshot().ActionState == ECombatActionState::HitStun);
	TestEqual(TEXT("the stunned victim still dealt no hit"), Scene.EventsB.HitCount, 0);
	TestEqual(TEXT("the attacker still has full health"), M1_020_HealthOf(Scene.Attacker), 100.0f);

	// Exactly at the 0.22 s boundary (the input clock was 0 at the hit) the
	// stun ends and the victim is Free again.
	Scene.CombatB->SetInputClockSeconds(0.22);
	Scene.CombatB->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("the stun ended exactly at 0.22 s"),
		Scene.CombatB->GetSnapshot().ActionState == ECombatActionState::Free);

	// A fresh instance can start and hits the same target once: the new
	// instance is untouched by the interrupted one.
	if (!TestTrue(TEXT("the victim can attack again after the stun"),
		Scene.CombatB->TryStartAttack(FName(TEXT("light_01")), -1)))
	{
		return true;
	}
	TestEqual(TEXT("the recovered attack minted a fresh instance id"),
		Scene.CombatB->GetSnapshot().InstanceId, uint64(2));
	M1_020_TickFrames(*Scene.CombatB, 8);
	TestEqual(TEXT("the fresh instance hit the attacker exactly once"), Scene.EventsB.HitCount, 1);
	TestEqual(TEXT("the fresh hit carries the new instance id"), Scene.EventsB.LastHit.AttackInstanceId, uint64(2));
	TestEqual(TEXT("the fresh instance deducted exactly the base damage"), M1_020_HealthOf(Scene.Attacker), 90.0f);
	TestEqual(TEXT("the victim's health stays at the single stun-causing deduction"), M1_020_HealthOf(Scene.Victim), 90.0f);
	return true;
}

// The stun duration boundary at component level: a hit with StunSeconds 0.22
// on an explicit input clock stuns for exactly 0.22 s, refuses attacks and
// keeps CanAcceptMovement false in between, while the input cache keeps
// receiving; at exactly 0.22 s the state is Free again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_020HitStunBoundary022RefusesAttacksAndKeepsInputCache,
	"UEMMO.Tasks.M1_020.HitStunBoundary022RefusesAttacksAndKeepsInputCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_020HitStunBoundary022RefusesAttacksAndKeepsInputCache::RunTest(const FString& Parameters)
{
	UWorld* World = M1_020_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	UAttackCatalog* Catalog = M1_020_NewCatalog(*this);
	if (Catalog == nullptr)
	{
		return true;
	}
	AActor* Victim = M1_020_SpawnCombatant(*World, M1_020_SceneBase, Catalog, /*bWithHealth*/ true, /*bWithCombat*/ true);
	if (!TestNotNull(TEXT("the victim spawns"), Victim))
	{
		return true;
	}
	UCombatComponent* Combat = Victim->FindComponentByClass<UCombatComponent>();
	if (!TestTrue(TEXT("the victim carries a combat component"), Combat != nullptr))
	{
		return true;
	}

	// Hit at input clock 0.0 with the light_01 stun duration.
	Combat->SetInputClockSeconds(0.0);
	Combat->NotifyHitReceived(M1_020_MakeStunHit(Victim, 0.22f));
	TestTrue(TEXT("the hit put the component into HitStun"),
		Combat->GetSnapshot().ActionState == ECombatActionState::HitStun);
	TestTrue(TEXT("movement is refused during the stun"), !Combat->CanAcceptMovement());

	// Attacks are refused during the stun without state damage.
	TestFalse(TEXT("TryStartAttack is refused during the stun"),
		Combat->TryStartAttack(FName(TEXT("light_01")), 1));
	TestTrue(TEXT("the refusal left the stun untouched"),
		Combat->GetSnapshot().ActionState == ECombatActionState::HitStun);
	TestEqual(TEXT("the refusal minted no instance"), Combat->GetSnapshot().InstanceId, uint64(0));

	// The input cache still receives during the stun.
	FBufferedCombatInput Input;
	Input.Sequence = 1;
	Input.Action = ECombatInput::Light;
	Input.PressedAt = 0.1;
	Combat->QueueInput(Input);
	FBufferedCombatInput Peeked;
	TestTrue(TEXT("the buffered input is still received during the stun"), Combat->PeekInputBuffer(Peeked));
	TestEqual(TEXT("the buffer holds the input queued during the stun"), Combat->GetSnapshot().BufferSize, 1);

	// Just before the boundary the stun holds (the action delta plays no role:
	// the injected input clock decides).
	Combat->SetInputClockSeconds(0.21);
	Combat->TickCombat(0.0f);
	TestTrue(TEXT("the victim is still stunned at 0.21 s"),
		Combat->GetSnapshot().ActionState == ECombatActionState::HitStun);

	// Exactly at 0.22 s the stun ends and movement is accepted again.
	Combat->SetInputClockSeconds(0.22);
	Combat->TickCombat(0.0f);
	TestTrue(TEXT("the stun ended exactly at 0.22 s"),
		Combat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestTrue(TEXT("movement is accepted again after the stun"), Combat->CanAcceptMovement());

	// The cache kept the entry through the whole stun (recovery never pruned).
	TestTrue(TEXT("the buffered input survived the stun"), Combat->PeekInputBuffer(Peeked));
	TestEqual(TEXT("the buffer still holds the entry after recovery"), Combat->GetSnapshot().BufferSize, 1);

	// A new attack can start after the stun.
	TestTrue(TEXT("the component can attack again after the stun"),
		Combat->TryStartAttack(FName(TEXT("light_01")), 1));
	return true;
}

// A hit during an existing stun takes max(remaining, new), never a sum: with
// 0.10 s remaining a 0.22 s hit ends 0.22 s later (not 0.32 s of extra stun),
// and with 0.30 s remaining a 0.22 s hit leaves the original end untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_020NewHitStunTakesMaxNotSum,
	"UEMMO.Tasks.M1_020.NewHitStunTakesMaxNotSum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_020NewHitStunTakesMaxNotSum::RunTest(const FString& Parameters)
{
	UWorld* World = M1_020_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	UAttackCatalog* Catalog = M1_020_NewCatalog(*this);
	if (Catalog == nullptr)
	{
		return true;
	}
	AActor* Victim = M1_020_SpawnCombatant(*World, M1_020_SceneBase, Catalog, /*bWithHealth*/ true, /*bWithCombat*/ true);
	if (!TestNotNull(TEXT("the victim spawns"), Victim))
	{
		return true;
	}
	UCombatComponent* Combat = Victim->FindComponentByClass<UCombatComponent>();
	if (!TestTrue(TEXT("the victim carries a combat component"), Combat != nullptr))
	{
		return true;
	}

	// Part 1: 0.10 s remaining, refreshed by 0.22 s -> 0.22 s from now.
	Combat->SetInputClockSeconds(0.0);
	Combat->NotifyHitReceived(M1_020_MakeStunHit(Victim, 0.22f));
	TestTrue(TEXT("the first hit stuns"), Combat->GetSnapshot().ActionState == ECombatActionState::HitStun);

	Combat->SetInputClockSeconds(0.12); // remaining 0.10
	Combat->NotifyHitReceived(M1_020_MakeStunHit(Victim, 0.22f));
	TestTrue(TEXT("the second hit keeps the component stunned"),
		Combat->GetSnapshot().ActionState == ECombatActionState::HitStun);

	// 0.33 > original end (0.22): the refresh really extended the stun past
	// the first hit's end; 0.36 > refreshed end (0.34): it is not a 0.32 s sum.
	Combat->SetInputClockSeconds(0.33);
	Combat->TickCombat(0.0f);
	TestTrue(TEXT("the stun extends past the first hit's end (max, not keep-old)"),
		Combat->GetSnapshot().ActionState == ECombatActionState::HitStun);
	Combat->SetInputClockSeconds(0.36);
	Combat->TickCombat(0.0f);
	TestTrue(TEXT("the refreshed stun ends 0.22 s after the second hit (max, not sum)"),
		Combat->GetSnapshot().ActionState == ECombatActionState::Free);

	// Part 2: 0.30 s remaining, hit with 0.22 s -> the original end stays.
	// Stun of 0.4 s at clock 0.40 ends at 0.80; at clock 0.50 the remaining
	// 0.30 s outranks the incoming 0.22 s.
	Combat->ResetCombat();
	Combat->SetInputClockSeconds(0.40);
	Combat->NotifyHitReceived(M1_020_MakeStunHit(Victim, 0.4f));
	TestTrue(TEXT("the longer hit stuns"), Combat->GetSnapshot().ActionState == ECombatActionState::HitStun);

	Combat->SetInputClockSeconds(0.50); // remaining 0.30
	Combat->NotifyHitReceived(M1_020_MakeStunHit(Victim, 0.22f));
	TestTrue(TEXT("the shorter hit keeps the component stunned"),
		Combat->GetSnapshot().ActionState == ECombatActionState::HitStun);

	// 0.75 < 0.80: the original end survived (not shortened to 0.50 + 0.22);
	// 0.85 > 0.80: it also was not extended by a sum (0.50 + 0.30 + 0.22).
	Combat->SetInputClockSeconds(0.75);
	Combat->TickCombat(0.0f);
	TestTrue(TEXT("the longer remaining stun is kept (max, not take-new)"),
		Combat->GetSnapshot().ActionState == ECombatActionState::HitStun);
	Combat->SetInputClockSeconds(0.85);
	Combat->TickCombat(0.0f);
	TestTrue(TEXT("the shorter hit never extended the original end (max, not sum)"),
		Combat->GetSnapshot().ActionState == ECombatActionState::Free);
	return true;
}

// Death has the highest priority: an already dead component ignores hits
// entirely, and a lethal hit through the real attacker wiring marks the victim
// dead instead of stunning it - the dead victim never comes back when any stun
// timer would have ended.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_020DeathPriorityPresetDeadAndLethalNeverStunNeverRevive,
	"UEMMO.Tasks.M1_020.DeathPriorityPresetDeadAndLethalNeverStunNeverRevive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_020DeathPriorityPresetDeadAndLethalNeverStunNeverRevive::RunTest(const FString& Parameters)
{
	UWorld* World = M1_020_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	UAttackCatalog* Catalog = M1_020_NewCatalog(*this);
	if (Catalog == nullptr)
	{
		return true;
	}

	// Part A: a component that was set dead ignores hits entirely. The actor
	// sits far off in depth so its (still alive) health pool can never leak
	// into part B's hit boxes.
	AActor* PresetDead = M1_020_SpawnCombatant(*World, M1_020_SceneBase + FVector(0.0, 5000.0, 0.0), Catalog, /*bWithHealth*/ true, /*bWithCombat*/ true);
	if (!TestNotNull(TEXT("the preset-dead combatant spawns"), PresetDead))
	{
		return true;
	}
	UCombatComponent* DeadCombat = PresetDead->FindComponentByClass<UCombatComponent>();
	if (!TestTrue(TEXT("the preset-dead combatant carries a combat component"), DeadCombat != nullptr))
	{
		return true;
	}
	DeadCombat->SetDead(true);
	DeadCombat->NotifyHitReceived(M1_020_MakeStunHit(PresetDead, 0.22f));
	TestTrue(TEXT("the preset-dead component stays dead"), DeadCombat->IsDead());
	TestTrue(TEXT("the preset-dead component never entered HitStun"),
		DeadCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestFalse(TEXT("the preset-dead component still refuses attacks"),
		DeadCombat->TryStartAttack(FName(TEXT("light_01")), 1));

	// Part B: a lethal hit through the real attacker wiring. The victim is
	// mid attack when the killing blow lands.
	AActor* Attacker = M1_020_SpawnCombatant(*World, M1_020_SceneBase, Catalog, /*bWithHealth*/ true, /*bWithCombat*/ true);
	AActor* Victim = M1_020_SpawnCombatant(*World, M1_020_SceneBase + FVector(95.0, 0.0, 0.0), Catalog, /*bWithHealth*/ true, /*bWithCombat*/ true);
	if (!TestNotNull(TEXT("the attacker spawns"), Attacker) || !TestNotNull(TEXT("the victim spawns"), Victim))
	{
		return true;
	}
	UCombatComponent* CombatA = Attacker->FindComponentByClass<UCombatComponent>();
	UCombatComponent* CombatB = Victim->FindComponentByClass<UCombatComponent>();
	if (!TestTrue(TEXT("both combatants carry a combat component"), CombatA != nullptr && CombatB != nullptr))
	{
		return true;
	}
	FM1_020_HitEvents EventsA;
	FM1_020_HitEvents EventsB;
	EventsA.Bind(*CombatA);
	EventsB.Bind(*CombatB);

	// The victim keeps 5 HP, so light_01's 10 damage is lethal.
	UHealthComponent* VictimHealth = Victim->FindComponentByClass<UHealthComponent>();
	VictimHealth->ApplyDamage(95.0f);
	if (!TestTrue(TEXT("precondition: the victim keeps 5 HP"),
		VictimHealth->IsAlive() && FMath::IsNearlyEqual(VictimHealth->GetHealth(), 5.0f)))
	{
		return true;
	}
	if (!TestTrue(TEXT("the victim's light_01 starts"), CombatB->TryStartAttack(FName(TEXT("light_01")), -1)))
	{
		return true;
	}
	CombatB->SetInputClockSeconds(0.0);

	if (!TestTrue(TEXT("the attacker's light_01 starts"), CombatA->TryStartAttack(FName(TEXT("light_01")), 1)))
	{
		return true;
	}
	M1_020_TickFrames(*CombatA, 8);

	// The lethal hit was still a confirmed hit, and the victim is dead.
	TestEqual(TEXT("the lethal hit was confirmed once"), EventsA.HitCount, 1);
	TestTrue(TEXT("the victim's health is gone"), !VictimHealth->IsAlive());
	TestEqual(TEXT("the victim's health pool stays at zero"), VictimHealth->GetHealth(), 0.0f);

	// Death instead of stun: the component is dead, never stunned, and the
	// in-flight attack was torn down by the death path.
	TestTrue(TEXT("the lethal hit marked the victim's combat component dead"), CombatB->IsDead());
	TestTrue(TEXT("the lethal hit never stunned the victim"),
		CombatB->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the victim's in-flight attack was cancelled by death"),
		CombatB->GetSnapshot().InstanceId, uint64(0));
	TestEqual(TEXT("the dying victim never dealt a hit"), EventsB.HitCount, 0);
	TestEqual(TEXT("the attacker keeps full health"), M1_020_HealthOf(Attacker), 100.0f);

	// No revival when any stun timer would have ended long ago.
	CombatB->SetInputClockSeconds(1.0);
	M1_020_TickFrames(*CombatB, 5);
	TestTrue(TEXT("the victim is still dead after the would-be stun window"), CombatB->IsDead());
	TestTrue(TEXT("the dead victim's state stays Free, not HitStun"),
		CombatB->GetSnapshot().ActionState == ECombatActionState::Free);
	TestFalse(TEXT("the dead victim still refuses attacks"),
		CombatB->TryStartAttack(FName(TEXT("light_01")), -1));
	TestEqual(TEXT("the victim's health pool was never restored"), VictimHealth->GetHealth(), 0.0f);
	return true;
}

// An attacker interrupted while its own active window is still open stops
// dealing damage: the remaining active frames never deduct the target, and a
// fresh instance after the stun can hit the same target once more.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_020AttackerInterruptedMidWindowStopsPendingDamage,
	"UEMMO.Tasks.M1_020.AttackerInterruptedMidWindowStopsPendingDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_020AttackerInterruptedMidWindowStopsPendingDamage::RunTest(const FString& Parameters)
{
	UWorld* World = M1_020_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_020_MutualScene Scene;
	if (!Scene.Build(*this, *World))
	{
		return true;
	}

	// The attacker ticks into its active window and hits once (frame 7).
	Scene.CombatA->SetInputClockSeconds(0.0);
	if (!TestTrue(TEXT("the attacker's light_01 starts"), Scene.CombatA->TryStartAttack(FName(TEXT("light_01")), 1)))
	{
		return true;
	}
	M1_020_TickFrames(*Scene.CombatA, 8);
	TestEqual(TEXT("the attacker hit exactly once"), Scene.EventsA.HitCount, 1);
	TestEqual(TEXT("precondition: the attacker stands mid attack on the first active frame"),
		Scene.CombatA->GetSnapshot().Frame, 7);
	TestTrue(TEXT("precondition: frame 7 is inside the active window"),
		Scene.Definition->ActiveWindow.Contains(7));
	TestEqual(TEXT("the hit deducted exactly the base damage"), M1_020_HealthOf(Scene.Victim), 90.0f);

	// An accepted hit interrupts the attacker mid window.
	Scene.CombatA->NotifyHitReceived(M1_020_MakeStunHit(Scene.Attacker, 0.22f));
	TestTrue(TEXT("the attacker is stunned"), Scene.CombatA->GetSnapshot().ActionState == ECombatActionState::HitStun);
	TestTrue(TEXT("the interrupted attack id is cleared"),
		Scene.CombatA->GetSnapshot().AttackId == NAME_None);
	TestEqual(TEXT("the interrupted instance id is cleared"), Scene.CombatA->GetSnapshot().InstanceId, uint64(0));

	// The remaining active frames (8..10) and the rest of the timeline never
	// deduct again: 20 stunned ticks produce nothing.
	M1_020_TickFrames(*Scene.CombatA, 20);
	TestEqual(TEXT("the interrupted attacker dealt no further hit"), Scene.EventsA.HitCount, 1);
	TestEqual(TEXT("the target only lost the pre-interruption damage"), M1_020_HealthOf(Scene.Victim), 90.0f);

	// After the stun a fresh instance hits the already-hit target once more.
	Scene.CombatA->SetInputClockSeconds(0.22);
	Scene.CombatA->TickCombat(1.0f / 60.0f);
	TestTrue(TEXT("the attacker recovered to Free"),
		Scene.CombatA->GetSnapshot().ActionState == ECombatActionState::Free);
	if (!TestTrue(TEXT("the attacker can attack again after the stun"),
		Scene.CombatA->TryStartAttack(FName(TEXT("light_01")), 1)))
	{
		return true;
	}
	M1_020_TickFrames(*Scene.CombatA, 8);
	TestEqual(TEXT("the fresh instance hit the same target exactly once"), Scene.EventsA.HitCount, 2);
	TestEqual(TEXT("the fresh hit carries the new instance id"), Scene.EventsA.LastHit.AttackInstanceId, uint64(2));
	TestEqual(TEXT("the fresh instance deducted exactly the base damage"), M1_020_HealthOf(Scene.Victim), 80.0f);
	return true;
}

// The real training enemy wiring: the attacker's accepted hit automatically
// reaches the enemy's own combat component (no manual call), which stuns and
// recovers like any victim; a lethal hit kills it through the health pool's
// death event and the enemy never stuns or revives.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_020TrainingEnemyStunAndLethalDeathWiring,
	"UEMMO.Tasks.M1_020.TrainingEnemyStunAndLethalDeathWiring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_020TrainingEnemyStunAndLethalDeathWiring::RunTest(const FString& Parameters)
{
	UWorld* World = M1_020_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	UAttackCatalog* Catalog = M1_020_NewCatalog(*this);
	if (Catalog == nullptr)
	{
		return true;
	}
	const UAttackDefinition* Definition = Catalog->Find(FName(TEXT("light_01")));
	if (!TestTrue(TEXT("the catalog holds light_01"), Definition != nullptr))
	{
		return true;
	}

	AActor* Attacker = M1_020_SpawnCombatant(*World, M1_020_SceneBase, Catalog, /*bWithHealth*/ false, /*bWithCombat*/ true);
	if (!TestNotNull(TEXT("the attacker spawns"), Attacker))
	{
		return true;
	}
	UCombatComponent* CombatA = Attacker->FindComponentByClass<UCombatComponent>();
	if (!TestTrue(TEXT("the attacker carries a combat component"), CombatA != nullptr))
	{
		return true;
	}
	FM1_020_HitEvents EventsA;
	EventsA.Bind(*CombatA);

	// The enemy's capsule center sits at the attacker's hit box center height.
	FActorSpawnParameters SpawnParams;
	ATrainingEnemy* Enemy = World->SpawnActor<ATrainingEnemy>(
		ATrainingEnemy::StaticClass(), M1_020_SceneBase + FVector(95.0, 0.0, 90.0), FRotator::ZeroRotator, SpawnParams);
	if (!TestNotNull(TEXT("the training enemy spawns"), Enemy))
	{
		return true;
	}
	// BeginPlay owns the death wiring; temp worlds may not have begun play, so
	// dispatch it once explicitly and never twice.
	if (!Enemy->HasActorBegunPlay())
	{
		Enemy->DispatchBeginPlay();
	}
	UCombatComponent* EnemyCombat = Enemy->FindComponentByClass<UCombatComponent>();
	if (!TestTrue(TEXT("the training enemy carries its own combat component"), EnemyCombat != nullptr))
	{
		return true;
	}
	if (!TestTrue(TEXT("precondition: the training enemy starts alive at full health"),
		Enemy->GetHealthComponent() != nullptr && Enemy->GetHealthComponent()->IsAlive()
		&& FMath::IsNearlyEqual(Enemy->GetHealthComponent()->GetHealth(), 100.0f)))
	{
		return true;
	}

	// Part 1: the accepted hit stuns the enemy through the attacker wiring.
	EnemyCombat->SetInputClockSeconds(0.0);
	if (!TestTrue(TEXT("the attacker's light_01 starts"), CombatA->TryStartAttack(FName(TEXT("light_01")), 1)))
	{
		return true;
	}
	M1_020_TickFrames(*CombatA, 8);
	TestEqual(TEXT("the attacker hit the enemy exactly once"), EventsA.HitCount, 1);
	TestEqual(TEXT("the enemy lost exactly the base damage"),
		Enemy->GetHealthComponent()->GetHealth(), 90.0f);
	TestTrue(TEXT("the enemy is stunned through the automatic wiring"),
		EnemyCombat->GetSnapshot().ActionState == ECombatActionState::HitStun);
	EnemyCombat->SetInputClockSeconds(0.22);
	EnemyCombat->TickCombat(0.0f);
	TestTrue(TEXT("the enemy recovered to Free exactly at 0.22 s"),
		EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free);

	// Part 2: a lethal hit kills instead of stunning, through the health
	// pool's death event. Reset the room prop first (full health, alive).
	Enemy->ResetEnemy();
	if (!TestTrue(TEXT("precondition: the reset enemy is alive at full health and unstunned"),
		Enemy->GetHealthComponent()->IsAlive()
		&& FMath::IsNearlyEqual(Enemy->GetHealthComponent()->GetHealth(), 100.0f)
		&& EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free
		&& !EnemyCombat->IsDead()))
	{
		return true;
	}
	Enemy->GetHealthComponent()->ApplyDamage(95.0f);
	if (!TestTrue(TEXT("precondition: the enemy keeps 5 HP"),
		Enemy->GetHealthComponent()->IsAlive() && FMath::IsNearlyEqual(Enemy->GetHealthComponent()->GetHealth(), 5.0f)))
	{
		return true;
	}

	// Let the attacker's first instance finish before the killing blow.
	M1_020_TickFrames(*CombatA, 18);
	TestTrue(TEXT("the attacker recovered to Free after its first attack"),
		CombatA->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual(TEXT("the finished first attack never hit twice"), EventsA.HitCount, 1);
	if (!TestTrue(TEXT("the attacker's killing light_01 starts"),
		CombatA->TryStartAttack(FName(TEXT("light_01")), 1)))
	{
		return true;
	}
	M1_020_TickFrames(*CombatA, 8);

	// The enemy is dead - never stunned, never revived.
	TestEqual(TEXT("the killing hit was confirmed"), EventsA.HitCount, 2);
	TestTrue(TEXT("the enemy's health pool is gone"), !Enemy->GetHealthComponent()->IsAlive());
	TestEqual(TEXT("the enemy's health pool stays at zero"), Enemy->GetHealthComponent()->GetHealth(), 0.0f);
	TestTrue(TEXT("the lethal hit marked the enemy's combat component dead"), EnemyCombat->IsDead());
	TestTrue(TEXT("the lethal hit never stunned the enemy"),
		EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	EnemyCombat->SetInputClockSeconds(1.0);
	EnemyCombat->TickCombat(0.0f);
	TestTrue(TEXT("the enemy is still dead after the would-be stun window"), EnemyCombat->IsDead());
	TestEqual(TEXT("the enemy's health was never restored"), Enemy->GetHealthComponent()->GetHealth(), 0.0f);
	return true;
}

#endif
