#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatGeometry.h"
#include "../Combat/CombatPresentationComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/TrainingEnemy.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

// M1-033: local hit stop with input retention. One accepted hit pauses the
// attacker and its target locally for FCombatHit::HitStopSeconds (0.04 s from
// the definitions): the action clock freezes, the injected input game clock
// pins (presses still queue), the victim's movement saves its state once and
// integrates nothing, and the animation pauses through the presentation
// component. No global time dilation: unrelated actors advance normally.
namespace UE::UEMMO::Tasks::M1_033
{
	// Remote base so the temp world can never collide with arena content.
	// X horizontal, Y depth, Z height; box actors root at their feet.
	const FVector M1_033_SceneBase(42000.0, 46000.0, 600.0);

	// Half extent of one test body box (the M1-019/M1-020 suite shape: tall
	// enough that two ground-level combatants 95 cm apart overlap the light_01
	// hit box, offset (95, 0, 90), half extent (85, 50, 70)).
	const FVector M1_033_BodyHalfExtent(20.0, 20.0, 95.0);

	const TCHAR* const M1_033_LightAttackIdText = TEXT("light_01");

	// World acquisition, same order as the M1-018+ pattern: a private temp
	// world first, the shared game world as fallback.
	static UWorld* M1_033_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M1_033_TestWorld")));
		if (TempWorld != nullptr)
		{
			UE_LOG(LogTemp, Log, TEXT("M1_033 world source: private temp world (UWorld::CreateWorld)"));
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M1_033 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	// Loads the real read-only catalog from Config/DefaultGame.ini (the same
	// loading path production and the earlier combat tests use). Returns null
	// after reporting the failure so the caller can bail out early.
	static UAttackCatalog* M1_033_NewCatalog(FAutomationTestBase& Test)
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

	// Spawns a plain actor with one query-enabled box root at Location.
	// bWithHealth attaches a fresh UHealthComponent; bWithCombat attaches a
	// fresh UCombatComponent bound to the catalog.
	static AActor* M1_033_SpawnCombatant(UWorld& World, const FVector& Location, UAttackCatalog* Catalog, bool bWithHealth, bool bWithCombat)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}

		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M1_033_Body"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M1_033_BodyHalfExtent);
		Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		Body->SetCollisionObjectType(ECC_Pawn);
		Body->SetCollisionResponseToAllChannels(ECR_Ignore);
		Body->RegisterComponent();
		Body->SetWorldLocation(Location);

		if (bWithHealth)
		{
			UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M1_033_Health"));
			Health->RegisterComponent();
		}
		if (bWithCombat)
		{
			UCombatComponent* Combat = NewObject<UCombatComponent>(Actor, TEXT("M1_033_Combat"));
			Combat->RegisterComponent();
			if (Catalog != nullptr)
			{
				Combat->InitializeFromCatalog(Catalog);
			}
		}
		return Actor;
	}

	// One simulated game frame in the contract's per-frame order (interface
	// contract section 2: one input clock injection before TickCombat).
	static void M1_033_InjectAndTick(UCombatComponent& Combat, int32 FrameIndex)
	{
		Combat.SetInputClockSeconds(FrameIndex / 60.0);
		Combat.TickCombat(1.0f / 60.0f);
	}

	// Counts HitConfirmed broadcasts (hits this component DEALT).
	struct FM1_033_HitEvents
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

	// One hit scene: the real catalog, an attacker and a victim 95 cm apart on
	// X (both with health and combat), and a presentation component bound to
	// the attacker - the production hit-stop trigger, because the presenter is
	// the place that observes OnHitConfirmed and requests the stop on both
	// parties. The victim roots at the base feet level; the attacker faces +1.
	struct FM1_033_HitScene
	{
		UAttackCatalog* Catalog = nullptr;
		const UAttackDefinition* Definition = nullptr;
		AActor* Attacker = nullptr;
		AActor* Victim = nullptr;
		UCombatComponent* AttackerCombat = nullptr;
		UCombatComponent* VictimCombat = nullptr;
		UCombatPresentationComponent* AttackerPresentation = nullptr;
		FM1_033_HitEvents Events;

		bool Build(FAutomationTestBase& Test, UWorld& World, const FVector& Base)
		{
			Catalog = M1_033_NewCatalog(Test);
			if (Catalog == nullptr)
			{
				return false;
			}
			Definition = Catalog->Find(FName(M1_033_LightAttackIdText));
			if (!Test.TestTrue(TEXT("the catalog holds light_01"), Definition != nullptr))
			{
				return false;
			}

			Attacker = M1_033_SpawnCombatant(World, Base, Catalog, /*bWithHealth*/ true, /*bWithCombat*/ true);
			Victim = M1_033_SpawnCombatant(World, Base + FVector(95.0, 0.0, 0.0), Catalog, /*bWithHealth*/ true, /*bWithCombat*/ true);
			if (!Test.TestNotNull(TEXT("the attacker spawns"), Attacker)
				|| !Test.TestNotNull(TEXT("the victim spawns"), Victim))
			{
				return false;
			}
			AttackerCombat = Attacker->FindComponentByClass<UCombatComponent>();
			VictimCombat = Victim->FindComponentByClass<UCombatComponent>();
			if (!Test.TestTrue(TEXT("both combatants carry a combat component"), AttackerCombat != nullptr && VictimCombat != nullptr))
			{
				return false;
			}

			// The production trigger: the presenter observes the confirmed hit
			// and requests the local stop on the attacker and the target.
			AttackerPresentation = NewObject<UCombatPresentationComponent>(Attacker, TEXT("M1_033_AttackerPresentation"));
			AttackerPresentation->SetSources(AttackerCombat, nullptr);
			Events.Bind(*AttackerCombat);
			return true;
		}

		// Starts the attacker's light_01 and drives both combatants through the
		// per-frame loop until the first active frame (7) landed its hit.
		bool DriveToFirstHit(FAutomationTestBase& Test)
		{
			if (!Test.TestTrue(TEXT("the attacker's light_01 starts"), AttackerCombat->TryStartAttack(FName(M1_033_LightAttackIdText), 1)))
			{
				return false;
			}
			for (int32 FrameIndex = 0; FrameIndex <= 7; ++FrameIndex)
			{
				M1_033_InjectAndTick(*VictimCombat, FrameIndex);
				M1_033_InjectAndTick(*AttackerCombat, FrameIndex);
			}
			return Test.TestEqual(TEXT("the attacker hit exactly once on its first active frame"), Events.HitCount, 1);
		}
	};

	// Spawns a training enemy whose capsule center sits at Location and whose
	// movement component accepts real movement updates in the temp world (the
	// M1-022 pattern: temp worlds never run InitializeComponents, so the
	// movement component's activation and default mode are mirrored here).
	static ATrainingEnemy* M1_033_SpawnEnemy(UWorld& World, const FVector& CapsuleCenter, FAutomationTestBase& Test)
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
		UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement();
		if (!Test.TestNotNull(TEXT("the training enemy carries a movement component"), Movement))
		{
			return nullptr;
		}
		if (!Movement->IsActive())
		{
			Movement->Activate(/*bReset*/ true);
		}
		if (Movement->MovementMode == MOVE_None)
		{
			Movement->SetDefaultMovementMode();
		}
		return Enemy;
	}
}

using namespace UE::UEMMO::Tasks::M1_033;

// Core acceptance: after the accepted hit the attacker's injected input game
// clock pins at the hit value while the owner keeps injecting advancing values
// (interface contract section 2: Pause/HitStop does not advance the input
// clock), and presses still queue during the freeze.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_033AttackerInputClockPinnedDuringHitStopAndInputStillQueues,
	"UEMMO.Tasks.M1_033.AttackerInputClockPinnedDuringHitStopAndInputStillQueues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_033AttackerInputClockPinnedDuringHitStopAndInputStillQueues::RunTest(const FString& Parameters)
{
	UWorld* World = M1_033_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_033_HitScene Scene;
	if (!Scene.Build(*this, *World, M1_033_SceneBase) || !Scene.DriveToFirstHit(*this))
	{
		return true;
	}

	// The hit opened the local stop on both parties: the action clock is
	// frozen and the timeline holds on the hit frame.
	TestTrue(TEXT("the attacker's local hit stop is active"), Scene.AttackerCombat->IsHitStopActive());
	TestTrue(TEXT("the attacker's action clock is frozen"), Scene.AttackerCombat->IsClockFrozen());
	TestEqual(TEXT("the attack timeline holds on the hit frame"), Scene.AttackerCombat->GetSnapshot().Frame, 7);
	TestTrue(TEXT("the target's local hit stop is active"), Scene.VictimCombat->IsHitStopActive());

	// Frame 8: the injected now advances, the pinned input clock does not.
	M1_033_InjectAndTick(*Scene.VictimCombat, 8);
	M1_033_InjectAndTick(*Scene.AttackerCombat, 8);
	TestTrue(TEXT("the attacker's input clock stayed pinned at the hit value while the injected now advanced"),
		FMath::IsNearlyEqual(Scene.AttackerCombat->GetInputClockSeconds(), 7.0 / 60.0, 1.0e-9));
	TestTrue(TEXT("the target's input clock stayed pinned too"),
		FMath::IsNearlyEqual(Scene.VictimCombat->GetInputClockSeconds(), 7.0 / 60.0, 1.0e-9));

	// Presses still queue during the freeze (interface contract section 2).
	FBufferedCombatInput Press;
	Press.Sequence = 1;
	Press.Action = ECombatInput::Light;
	Press.PressedAt = Scene.AttackerCombat->GetInputClockSeconds();
	Scene.AttackerCombat->QueueInput(Press);
	FBufferedCombatInput Peeked;
	TestTrue(TEXT("the press queued during the freeze is buffered"), Scene.AttackerCombat->PeekInputBuffer(Peeked));
	TestEqual(TEXT("the frozen-period press kept its sequence"), Peeked.Sequence, uint64(1));

	// Frame 9: still inside the 40 ms stop (33 ms elapsed), still pinned.
	M1_033_InjectAndTick(*Scene.VictimCombat, 9);
	M1_033_InjectAndTick(*Scene.AttackerCombat, 9);
	TestTrue(TEXT("the stop is still active at 33 ms of injected advancement"), Scene.AttackerCombat->IsHitStopActive());
	TestTrue(TEXT("the input clock still reads the frozen value"),
		FMath::IsNearlyEqual(Scene.AttackerCombat->GetInputClockSeconds(), 7.0 / 60.0, 1.0e-9));

	// A second press queues behind the first with its own sequence.
	FBufferedCombatInput SecondPress;
	SecondPress.Sequence = 2;
	SecondPress.Action = ECombatInput::Launcher;
	SecondPress.PressedAt = Scene.AttackerCombat->GetInputClockSeconds();
	Scene.AttackerCombat->QueueInput(SecondPress);
	TestEqual(TEXT("both frozen-period presses are buffered in order"), Scene.AttackerCombat->GetSnapshot().BufferSize, 2);
	return true;
}

// The stop ends once the injected advancement crossed HitStopSeconds (40 ms of
// 1/60 s frames): the input clock resumes with that injection and the action
// timeline continues with exactly one frame per tick - the frozen deltas were
// dropped, never back-filled (interface contract section 2: no catch-up).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_033FreezeEndsWithInjectedAdvancementNoActionCatchUp,
	"UEMMO.Tasks.M1_033.FreezeEndsWithInjectedAdvancementNoActionCatchUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_033FreezeEndsWithInjectedAdvancementNoActionCatchUp::RunTest(const FString& Parameters)
{
	UWorld* World = M1_033_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM1_033_HitScene Scene;
	if (!Scene.Build(*this, *World, M1_033_SceneBase) || !Scene.DriveToFirstHit(*this))
	{
		return true;
	}

	// Frames 8 and 9: 33 ms of injected advancement, still short of 40 ms.
	for (int32 FrameIndex = 8; FrameIndex <= 9; ++FrameIndex)
	{
		M1_033_InjectAndTick(*Scene.VictimCombat, FrameIndex);
		M1_033_InjectAndTick(*Scene.AttackerCombat, FrameIndex);
	}
	TestTrue(TEXT("the stop holds through 33 ms of injected advancement"), Scene.AttackerCombat->IsHitStopActive());
	TestTrue(TEXT("the input clock is still pinned before the 40 ms boundary"),
		FMath::IsNearlyEqual(Scene.AttackerCombat->GetInputClockSeconds(), 7.0 / 60.0, 1.0e-9));
	TestEqual(TEXT("the timeline still holds the hit frame"), Scene.AttackerCombat->GetSnapshot().Frame, 7);

	// Frame 10: the injected advancement (50 ms) crossed the 40 ms stop. The
	// freeze ends with this very injection and the clock resumes advancing.
	M1_033_InjectAndTick(*Scene.VictimCombat, 10);
	M1_033_InjectAndTick(*Scene.AttackerCombat, 10);
	TestFalse(TEXT("the hit stop ended once the injected advancement crossed 40 ms"), Scene.AttackerCombat->IsHitStopActive());
	TestFalse(TEXT("the action clock unfroze with the stop"), Scene.AttackerCombat->IsClockFrozen());
	TestTrue(TEXT("the input clock resumed with the crossing injection"),
		FMath::IsNearlyEqual(Scene.AttackerCombat->GetInputClockSeconds(), 10.0 / 60.0, 1.0e-9));
	TestTrue(TEXT("the target's input clock resumed with the same injection"),
		FMath::IsNearlyEqual(Scene.VictimCombat->GetInputClockSeconds(), 10.0 / 60.0, 1.0e-9));

	// No catch-up: the frozen frames were dropped, so the ending tick advanced
	// exactly one frame (7 -> 8), and every later tick advances exactly one.
	TestEqual(TEXT("the ending tick advanced exactly one frame, no backlog burst"), Scene.AttackerCombat->GetSnapshot().Frame, 8);
	M1_033_InjectAndTick(*Scene.VictimCombat, 11);
	M1_033_InjectAndTick(*Scene.AttackerCombat, 11);
	M1_033_InjectAndTick(*Scene.VictimCombat, 12);
	M1_033_InjectAndTick(*Scene.AttackerCombat, 12);
	TestEqual(TEXT("the timeline keeps advancing one frame per tick after the stop"), Scene.AttackerCombat->GetSnapshot().Frame, 10);

	// No permanent freeze: the attack still finishes naturally on its timeline.
	bool bFinished = false;
	for (int32 FrameIndex = 13; FrameIndex <= 40; ++FrameIndex)
	{
		M1_033_InjectAndTick(*Scene.VictimCombat, FrameIndex);
		M1_033_InjectAndTick(*Scene.AttackerCombat, FrameIndex);
		if (Scene.AttackerCombat->GetSnapshot().ActionState == ECombatActionState::Free)
		{
			bFinished = true;
			break;
		}
	}
	TestTrue(TEXT("the attack finished naturally after the stop lifted"), bFinished);
	return true;
}

// Reentry takes max(remaining, new), never a sum: a second hit inside the
// running stop extends it to the new 40 ms (not 80), and a new stop that is
// shorter than the remaining one keeps the remainder.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_033HitStopReentryTakesMaxRemainingNotSum,
	"UEMMO.Tasks.M1_033.HitStopReentryTakesMaxRemainingNotSum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_033HitStopReentryTakesMaxRemainingNotSum::RunTest(const FString& Parameters)
{
	// Part A: two attackers hit the same victim one frame apart. The victim's
	// stop reenters with 23.3 ms remaining and must end 40 ms after the second
	// hit (max), not 63.3 ms (sum) and not 23.3 ms (keep-old).
	UWorld* World = M1_033_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	UAttackCatalog* Catalog = M1_033_NewCatalog(*this);
	if (Catalog == nullptr)
	{
		return true;
	}
	AActor* FirstAttacker = M1_033_SpawnCombatant(*World, M1_033_SceneBase, Catalog, /*bWithHealth*/ false, /*bWithCombat*/ true);
	AActor* SecondAttacker = M1_033_SpawnCombatant(*World, M1_033_SceneBase, Catalog, /*bWithHealth*/ false, /*bWithCombat*/ true);
	AActor* Victim = M1_033_SpawnCombatant(*World, M1_033_SceneBase + FVector(95.0, 0.0, 0.0), Catalog, /*bWithHealth*/ true, /*bWithCombat*/ true);
	if (!TestNotNull(TEXT("the first attacker spawns"), FirstAttacker)
		|| !TestNotNull(TEXT("the second attacker spawns"), SecondAttacker)
		|| !TestNotNull(TEXT("the victim spawns"), Victim))
	{
		return true;
	}
	UCombatComponent* CombatA1 = FirstAttacker->FindComponentByClass<UCombatComponent>();
	UCombatComponent* CombatA2 = SecondAttacker->FindComponentByClass<UCombatComponent>();
	UCombatComponent* CombatV = Victim->FindComponentByClass<UCombatComponent>();
	if (!TestTrue(TEXT("all three combatants carry combat components"), CombatA1 != nullptr && CombatA2 != nullptr && CombatV != nullptr))
	{
		return true;
	}

	// Each attacker carries its own presenter (the production trigger); the
	// query-only bodies ignore each other, so both hit boxes cover the victim.
	UCombatPresentationComponent* PresenterA1 = NewObject<UCombatPresentationComponent>(FirstAttacker, TEXT("M1_033_PresenterA1"));
	PresenterA1->SetSources(CombatA1, nullptr);
	UCombatPresentationComponent* PresenterA2 = NewObject<UCombatPresentationComponent>(SecondAttacker, TEXT("M1_033_PresenterA2"));
	PresenterA2->SetSources(CombatA2, nullptr);
	FM1_033_HitEvents EventsA1;
	FM1_033_HitEvents EventsA2;
	EventsA1.Bind(*CombatA1);
	EventsA2.Bind(*CombatA2);

	TestTrue(TEXT("the first attacker's light_01 starts"), CombatA1->TryStartAttack(FName(M1_033_LightAttackIdText), 1));
	M1_033_InjectAndTick(*CombatV, 0);
	M1_033_InjectAndTick(*CombatA1, 0);
	TestTrue(TEXT("the second attacker's light_01 starts one frame later"), CombatA2->TryStartAttack(FName(M1_033_LightAttackIdText), 1));

	// i=7: hit 1 opens the victim's stop (40 ms). i=8: hit 2 reenters it with
	// 23.3 ms remaining -> max(23.3, 40) = 40 ms measured from the reentry.
	for (int32 FrameIndex = 1; FrameIndex <= 10; ++FrameIndex)
	{
		M1_033_InjectAndTick(*CombatV, FrameIndex);
		M1_033_InjectAndTick(*CombatA1, FrameIndex);
		M1_033_InjectAndTick(*CombatA2, FrameIndex);
	}
	TestEqual(TEXT("the first attacker hit once"), EventsA1.HitCount, 1);
	TestEqual(TEXT("the second attacker hit once"), EventsA2.HitCount, 1);

	// After frame 10: keep-old would have ended the stop at frame 10 (the
	// 23.3 ms remainder consumed by frames 9 and 10) - max kept it running.
	TestTrue(TEXT("the reentered stop outlives the pre-reentry remainder (max, not keep-old)"), CombatV->IsHitStopActive());
	TestTrue(TEXT("the victim's clock is still pinned at the first hit's value"),
		FMath::IsNearlyEqual(CombatV->GetInputClockSeconds(), 7.0 / 60.0, 1.0e-9));

	// Frame 11: the refreshed 40 ms is consumed (a 63.3 ms sum would keep the
	// stop through frame 12; keep-old ended at frame 10).
	M1_033_InjectAndTick(*CombatV, 11);
	M1_033_InjectAndTick(*CombatA1, 11);
	M1_033_InjectAndTick(*CombatA2, 11);
	TestFalse(TEXT("the refreshed stop ended 40 ms after the second hit (max, not sum)"), CombatV->IsHitStopActive());
	TestTrue(TEXT("the victim's clock resumed at the frame-11 injection"),
		FMath::IsNearlyEqual(CombatV->GetInputClockSeconds(), 11.0 / 60.0, 1.0e-9));

	// The second attacker's own first stop (opened at frame 8) ends on the
	// same injection, and the first attacker's stop (frame 7) already ended
	// with the frame-10 injection: neither attacker is permanently frozen.
	TestFalse(TEXT("the second attacker's stop ended with the same injection"), CombatA2->IsHitStopActive());
	TestFalse(TEXT("the first attacker's stop already ended (frame 10)"), CombatA1->IsHitStopActive());

	// Part B: remaining greater than the new request keeps the remainder. A
	// bare component runs the tick-driven countdown path (no injections).
	UCombatComponent* Bare = NewObject<UCombatComponent>();
	Bare->RequestHitStop(0.04f);
	TestTrue(TEXT("the direct request opened the stop"), Bare->IsHitStopActive());
	Bare->TickCombat(0.020f);
	TestTrue(TEXT("the stop is active after 20 ms"), Bare->IsHitStopActive());
	Bare->RequestHitStop(0.01f);
	Bare->TickCombat(0.015f);
	TestTrue(TEXT("the shorter reentry kept the remaining 20 ms (take-new would have ended here)"), Bare->IsHitStopActive());
	Bare->TickCombat(0.006f);
	TestFalse(TEXT("the kept remainder ended after its own 21 ms (additive 30 ms would still hold)"), Bare->IsHitStopActive());
	return true;
}

// The airborne target's movement freezes for the stop: the movement state is
// saved exactly once, integrates nothing while frozen (no falling), and comes
// back once the stop ends so the original velocity keeps flying.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_033VictimMovementFrozenAirborneThenRestoredOnce,
	"UEMMO.Tasks.M1_033.VictimMovementFrozenAirborneThenRestoredOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_033VictimMovementFrozenAirborneThenRestoredOnce::RunTest(const FString& Parameters)
{
	UWorld* World = M1_033_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	UAttackCatalog* Catalog = M1_033_NewCatalog(*this);
	if (Catalog == nullptr)
	{
		return true;
	}
	const UAttackDefinition* Definition = Catalog->Find(FName(M1_033_LightAttackIdText));
	if (!TestTrue(TEXT("the catalog holds light_01"), Definition != nullptr))
	{
		return true;
	}

	AActor* Attacker = M1_033_SpawnCombatant(*World, M1_033_SceneBase, Catalog, /*bWithHealth*/ false, /*bWithCombat*/ true);
	if (!TestNotNull(TEXT("the attacker spawns"), Attacker))
	{
		return true;
	}
	UCombatComponent* AttackerCombat = Attacker->FindComponentByClass<UCombatComponent>();
	if (!TestTrue(TEXT("the attacker carries a combat component"), AttackerCombat != nullptr))
	{
		return true;
	}
	UCombatPresentationComponent* Presenter = NewObject<UCombatPresentationComponent>(Attacker, TEXT("M1_033_Presenter"));
	Presenter->SetSources(AttackerCombat, nullptr);
	FM1_033_HitEvents Events;
	Events.Bind(*AttackerCombat);

	// The enemy's capsule center sits at the light_01 hit box center.
	const FVector BoxCenter = ComputeHitBox(M1_033_SceneBase, 1, *Definition).Center;
	ATrainingEnemy* Enemy = M1_033_SpawnEnemy(*World, BoxCenter, *this);
	if (Enemy == nullptr)
	{
		return true;
	}
	UCombatComponent* EnemyCombat = Enemy->FindComponentByClass<UCombatComponent>();
	UCharacterMovementComponent* EnemyMovement = Enemy->GetCharacterMovement();
	if (!TestTrue(TEXT("the enemy carries its combat component"), EnemyCombat != nullptr))
	{
		return true;
	}

	// Airborne falling state: the enemy is mid-fall with a real velocity.
	const FVector AirborneVelocity(120.0, 0.0, -300.0);
	EnemyMovement->SetMovementMode(MOVE_Falling);
	EnemyMovement->Velocity = AirborneVelocity;
	const FVector PositionBefore = Enemy->GetActorLocation();

	// The freeze saves the velocity AFTER the hit's knockback folded into it
	// (the light_01 knockback 90 at facing +1 arrives as a pending impulse and
	// the freeze applies the same accumulated-forces step a movement update
	// runs), so the frozen and the restored velocity carry the knockback.
	const FVector FrozenVelocity = AirborneVelocity
		+ FVector(Definition->KnockbackSpeed * 1.0f, 0.0f, 0.0f);

	if (!TestTrue(TEXT("the attacker's light_01 starts"), AttackerCombat->TryStartAttack(FName(M1_033_LightAttackIdText), 1)))
	{
		return true;
	}
	for (int32 FrameIndex = 0; FrameIndex <= 7; ++FrameIndex)
	{
		M1_033_InjectAndTick(*EnemyCombat, FrameIndex);
		M1_033_InjectAndTick(*AttackerCombat, FrameIndex);
	}
	TestEqual(TEXT("the attacker hit the enemy exactly once"), Events.HitCount, 1);
	TestTrue(TEXT("the enemy's local hit stop is active"), EnemyCombat->IsHitStopActive());

	// The frozen state: no movement integration, velocity kept (with the hit's
	// own knockback folded in; the engine's MOVE_None transition zeroes the
	// velocity internally and the freeze writes the saved value back).
	TestTrue(TEXT("the frozen enemy's movement integrates nothing (MOVE_None)"), EnemyMovement->MovementMode == MOVE_None);
	TestTrue(TEXT("the frozen enemy keeps its velocity (pre-hit fall speed plus the folded knockback)"),
		EnemyMovement->Velocity.Equals(FrozenVelocity, 0.01f));

	// Repeated movement updates while frozen move nothing: the body neither
	// falls nor drifts for the whole stop.
	EnemyMovement->StartNewPhysics(1.0f / 60.0f, 0);
	EnemyMovement->StartNewPhysics(1.0f / 60.0f, 0);
	TestTrue(TEXT("the frozen airborne enemy did not fall while the stop ran"),
		Enemy->GetActorLocation().Equals(PositionBefore, 0.01f));

	// The stop ends with the injected advancement crossing 40 ms (frame 10).
	for (int32 FrameIndex = 8; FrameIndex <= 10; ++FrameIndex)
	{
		M1_033_InjectAndTick(*EnemyCombat, FrameIndex);
		M1_033_InjectAndTick(*AttackerCombat, FrameIndex);
	}
	TestFalse(TEXT("the enemy's stop ended with the injected advancement"), EnemyCombat->IsHitStopActive());
	TestTrue(TEXT("the enemy's movement mode was restored exactly once (MOVE_Falling)"), EnemyMovement->MovementMode == MOVE_Falling);
	TestTrue(TEXT("the enemy's saved velocity came back (one save, one restore)"),
		EnemyMovement->Velocity.Equals(FrozenVelocity, 0.01f));

	// The restored velocity keeps flying: one movement update falls the body.
	EnemyMovement->StartNewPhysics(1.0f / 60.0f, 0);
	TestTrue(TEXT("the restored airborne enemy falls again with its original velocity"),
		Enemy->GetActorLocation().Z < PositionBefore.Z - 4.0f);

	// The stun kept running on the pinned clock (deadline = hit clock + 0.22 s
	// = 7/60 + 0.22) and ends exactly when the injected clock reaches it.
	TestTrue(TEXT("the enemy is still stunned right after the stop"),
		EnemyCombat->GetSnapshot().ActionState == ECombatActionState::HitStun);
	EnemyCombat->SetInputClockSeconds(7.0 / 60.0 + 0.22);
	EnemyCombat->TickCombat(0.0f);
	TestTrue(TEXT("the enemy recovered to Free exactly at the pinned-clock stun deadline"),
		EnemyCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	return true;
}

// The stop is strictly local: a third combatant's input clock keeps advancing,
// its movement mode stays its own and its body keeps falling while the attacker
// and the target are frozen. No global time dilation anywhere.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_033ThirdPartyCombatantUnaffectedByLocalHitStop,
	"UEMMO.Tasks.M1_033.ThirdPartyCombatantUnaffectedByLocalHitStop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_033ThirdPartyCombatantUnaffectedByLocalHitStop::RunTest(const FString& Parameters)
{
	UWorld* World = M1_033_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	UAttackCatalog* Catalog = M1_033_NewCatalog(*this);
	if (Catalog == nullptr)
	{
		return true;
	}
	const UAttackDefinition* Definition = Catalog->Find(FName(M1_033_LightAttackIdText));
	if (!TestTrue(TEXT("the catalog holds light_01"), Definition != nullptr))
	{
		return true;
	}

	AActor* Attacker = M1_033_SpawnCombatant(*World, M1_033_SceneBase, Catalog, /*bWithHealth*/ false, /*bWithCombat*/ true);
	AActor* Victim = M1_033_SpawnCombatant(*World, M1_033_SceneBase + FVector(95.0, 0.0, 0.0), Catalog, /*bWithHealth*/ true, /*bWithCombat*/ true);
	if (!TestNotNull(TEXT("the attacker spawns"), Attacker) || !TestNotNull(TEXT("the victim spawns"), Victim))
	{
		return true;
	}
	UCombatComponent* AttackerCombat = Attacker->FindComponentByClass<UCombatComponent>();
	UCombatComponent* VictimCombat = Victim->FindComponentByClass<UCombatComponent>();
	if (!TestTrue(TEXT("both combatants carry combat components"), AttackerCombat != nullptr && VictimCombat != nullptr))
	{
		return true;
	}
	UCombatPresentationComponent* Presenter = NewObject<UCombatPresentationComponent>(Attacker, TEXT("M1_033_Presenter"));
	Presenter->SetSources(AttackerCombat, nullptr);
	FM1_033_HitEvents Events;
	Events.Bind(*AttackerCombat);

	// The third combatant stands far off in depth: outside every hit box of
	// this scene, with its own falling body and its own combat clock.
	ATrainingEnemy* Third = M1_033_SpawnEnemy(*World, M1_033_SceneBase + FVector(0.0, 5000.0, 90.0), *this);
	if (Third == nullptr)
	{
		return true;
	}
	UCombatComponent* ThirdCombat = Third->FindComponentByClass<UCombatComponent>();
	UCharacterMovementComponent* ThirdMovement = Third->GetCharacterMovement();
	if (!TestTrue(TEXT("the third combatant carries its combat component"), ThirdCombat != nullptr))
	{
		return true;
	}
	ThirdMovement->SetMovementMode(MOVE_Falling);
	ThirdMovement->Velocity = FVector(0.0, 0.0, -200.0);

	if (!TestTrue(TEXT("the attacker's light_01 starts"), AttackerCombat->TryStartAttack(FName(M1_033_LightAttackIdText), 1)))
	{
		return true;
	}
	for (int32 FrameIndex = 0; FrameIndex <= 8; ++FrameIndex)
	{
		M1_033_InjectAndTick(*ThirdCombat, FrameIndex);
		M1_033_InjectAndTick(*VictimCombat, FrameIndex);
		M1_033_InjectAndTick(*AttackerCombat, FrameIndex);
	}
	TestEqual(TEXT("the attacker hit the victim once"), Events.HitCount, 1);
	TestTrue(TEXT("the attacker is frozen"), AttackerCombat->IsHitStopActive());
	TestTrue(TEXT("the target is frozen"), VictimCombat->IsHitStopActive());

	// The third combatant's clock advanced normally through the freeze frames.
	TestFalse(TEXT("the third combatant carries no hit stop"), ThirdCombat->IsHitStopActive());
	TestTrue(TEXT("the third combatant's input clock advanced while the pair was frozen"),
		FMath::IsNearlyEqual(ThirdCombat->GetInputClockSeconds(), 8.0 / 60.0, 1.0e-9));
	TestTrue(TEXT("the third combatant's movement mode is its own (not forced to MOVE_None)"),
		ThirdMovement->MovementMode == MOVE_Falling);

	// Its body keeps falling while the attacker and the target cannot move.
	const FVector ThirdBefore = Third->GetActorLocation();
	ThirdMovement->StartNewPhysics(1.0f / 60.0f, 0);
	TestTrue(TEXT("the third combatant's body keeps falling during the pair's freeze"),
		Third->GetActorLocation().Z < ThirdBefore.Z - 2.0f);
	return true;
}

// Reset and death clear a running stop: the clocks resume, the pause dispatch
// reaches the presentation component (animation resumes) and the component
// stays usable. Neither two consecutive stops nor a reset mid-stop can freeze
// a combatant permanently.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_033ResetAndDeathClearHitStop,
	"UEMMO.Tasks.M1_033.ResetAndDeathClearHitStop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_033ResetAndDeathClearHitStop::RunTest(const FString& Parameters)
{
	UWorld* World = M1_033_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}

	// Part A: a reset mid-stop clears everything and the victim can act again.
	{
		FM1_033_HitScene Scene;
		if (!Scene.Build(*this, *World, M1_033_SceneBase))
		{
			return true;
		}
		// The victim's own presenter records its pause dispatches; it binds
		// before the hit so the dispatch is observed live.
		UCombatPresentationComponent* VictimPresentation =
			NewObject<UCombatPresentationComponent>(Scene.Victim, TEXT("M1_033_VictimPresentation"));
		VictimPresentation->SetSources(Scene.VictimCombat, nullptr);

		if (!Scene.DriveToFirstHit(*this))
		{
			return true;
		}
		TestTrue(TEXT("precondition: the victim's stop is running"), Scene.VictimCombat->IsHitStopActive());

		// The hit dispatched exactly one pause to the victim's presentation.
		const TArray<bool>& VictimHistory = VictimPresentation->GetHitStopPauseDispatchHistory();
		TestTrue(TEXT("the stop dispatched one pause to the victim's presentation"),
			VictimHistory.Num() == 1 && VictimHistory[0]);

		// A reset mid-stop is a full teardown: clocks resume, animation resumes.
		Scene.VictimCombat->ResetCombat();
		TestFalse(TEXT("the reset cleared the victim's hit stop"), Scene.VictimCombat->IsHitStopActive());
		TestFalse(TEXT("the reset unfroze the victim's action clock"), Scene.VictimCombat->IsClockFrozen());
		TestTrue(TEXT("the stop dispatched the resume to the victim's presentation"),
			VictimHistory.Num() == 2 && VictimHistory[0] && !VictimHistory[1]);

		Scene.VictimCombat->SetInputClockSeconds(2.0);
		TestTrue(TEXT("the victim's input clock follows injections again after the reset"),
			FMath::IsNearlyEqual(Scene.VictimCombat->GetInputClockSeconds(), 2.0, 1.0e-9));
		TestTrue(TEXT("the victim can attack again after the reset"),
			Scene.VictimCombat->TryStartAttack(FName(M1_033_LightAttackIdText), -1));
	}

	// Part B: a lethal second hit while the stop runs marks the victim dead and
	// the death cleanup clears the stop (the freeze never outlives the body).
	{
		UAttackCatalog* DeathCatalog = M1_033_NewCatalog(*this);
		if (DeathCatalog == nullptr)
		{
			return true;
		}
		const FVector DeathBase = M1_033_SceneBase + FVector(0.0, 3000.0, 0.0);
		AActor* FirstAttacker = M1_033_SpawnCombatant(*World, DeathBase, DeathCatalog, /*bWithHealth*/ false, /*bWithCombat*/ true);
		AActor* SecondAttacker = M1_033_SpawnCombatant(*World, DeathBase, DeathCatalog, /*bWithHealth*/ false, /*bWithCombat*/ true);
		AActor* Victim = M1_033_SpawnCombatant(*World, DeathBase + FVector(95.0, 0.0, 0.0), DeathCatalog, /*bWithHealth*/ true, /*bWithCombat*/ true);
		if (!TestNotNull(TEXT("the death-scene first attacker spawns"), FirstAttacker)
			|| !TestNotNull(TEXT("the death-scene second attacker spawns"), SecondAttacker)
			|| !TestNotNull(TEXT("the death-scene victim spawns"), Victim))
		{
			return true;
		}
		UCombatComponent* CombatA1 = FirstAttacker->FindComponentByClass<UCombatComponent>();
		UCombatComponent* CombatA2 = SecondAttacker->FindComponentByClass<UCombatComponent>();
		UCombatComponent* CombatV = Victim->FindComponentByClass<UCombatComponent>();
		UHealthComponent* VictimHealth = Victim->FindComponentByClass<UHealthComponent>();
		if (!TestTrue(TEXT("the death scene carries its components"),
			CombatA1 != nullptr && CombatA2 != nullptr && CombatV != nullptr && VictimHealth != nullptr))
		{
			return true;
		}

		// 15 HP: hit 1 (10 damage) stuns and freezes, hit 2 is lethal.
		VictimHealth->ApplyDamage(85.0f);
		if (!TestTrue(TEXT("precondition: the victim keeps 15 HP"),
			VictimHealth->IsAlive() && FMath::IsNearlyEqual(VictimHealth->GetHealth(), 15.0f)))
		{
			return true;
		}

		UCombatPresentationComponent* PresenterA1 = NewObject<UCombatPresentationComponent>(FirstAttacker, TEXT("M1_033_DeathPresenterA1"));
		PresenterA1->SetSources(CombatA1, nullptr);
		UCombatPresentationComponent* PresenterA2 = NewObject<UCombatPresentationComponent>(SecondAttacker, TEXT("M1_033_DeathPresenterA2"));
		PresenterA2->SetSources(CombatA2, nullptr);
		UCombatPresentationComponent* PresenterV = NewObject<UCombatPresentationComponent>(Victim, TEXT("M1_033_DeathPresenterV"));
		PresenterV->SetSources(CombatV, nullptr);
		FM1_033_HitEvents EventsA1;
		FM1_033_HitEvents EventsA2;
		EventsA1.Bind(*CombatA1);
		EventsA2.Bind(*CombatA2);

		TestTrue(TEXT("the death scene's first attacker starts"), CombatA1->TryStartAttack(FName(M1_033_LightAttackIdText), 1));
		M1_033_InjectAndTick(*CombatV, 0);
		M1_033_InjectAndTick(*CombatA1, 0);
		TestTrue(TEXT("the death scene's second attacker starts one frame later"), CombatA2->TryStartAttack(FName(M1_033_LightAttackIdText), 1));
		for (int32 FrameIndex = 1; FrameIndex <= 8; ++FrameIndex)
		{
			M1_033_InjectAndTick(*CombatV, FrameIndex);
			M1_033_InjectAndTick(*CombatA1, FrameIndex);
			M1_033_InjectAndTick(*CombatA2, FrameIndex);
		}
		TestEqual(TEXT("the first hit stunned the victim"), EventsA1.HitCount, 1);
		TestEqual(TEXT("the second (lethal) hit landed"), EventsA2.HitCount, 1);

		// Death cleared the running stop: no freeze residue anywhere.
		TestTrue(TEXT("the lethal hit marked the victim dead"), CombatV->IsDead());
		TestFalse(TEXT("the death cleared the victim's hit stop"), CombatV->IsHitStopActive());
		TestFalse(TEXT("the death unfroze the victim's action clock"), CombatV->IsClockFrozen());
		const TArray<bool>& DeathHistory = PresenterV->GetHitStopPauseDispatchHistory();
		TestTrue(TEXT("the death path dispatched the resume to the victim's presentation (pause then resume)"),
			DeathHistory.Num() == 2 && DeathHistory[0] && !DeathHistory[1]);

		CombatV->SetInputClockSeconds(3.0);
		TestTrue(TEXT("the dead victim's input clock follows injections again"),
			FMath::IsNearlyEqual(CombatV->GetInputClockSeconds(), 3.0, 1.0e-9));
		TestFalse(TEXT("the dead victim still refuses attacks"), CombatV->TryStartAttack(FName(M1_033_LightAttackIdText), -1));
	}
	return true;
}

// The 40 ms number is definition-driven, not hardcoded: every real attack
// definition carries HitStopSeconds 0.04, a hit field with a different value
// produces exactly that stop, and a zero-valued hit produces none.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_033HitStopDurationComesFromHitStopSeconds,
	"UEMMO.Tasks.M1_033.HitStopDurationComesFromHitStopSeconds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_033HitStopDurationComesFromHitStopSeconds::RunTest(const FString& Parameters)
{
	UAttackCatalog* Catalog = M1_033_NewCatalog(*this);
	if (Catalog == nullptr)
	{
		return true;
	}

	// (a) The card value rides on every current attack definition.
	const TCHAR* const AttackIds[] = { TEXT("light_01"), TEXT("light_02"), TEXT("launcher"), TEXT("aerial_01") };
	for (const TCHAR* const AttackId : AttackIds)
	{
		const UAttackDefinition* Definition = Catalog->Find(FName(AttackId));
		if (TestNotNull(FString::Printf(TEXT("%s definition exists"), AttackId), Definition))
		{
			TestTrue(FString::Printf(TEXT("%s carries the card hit stop of 0.04 s"), AttackId),
				FMath::IsNearlyEqual(Definition->HitStopSeconds, 0.04f, 1.0e-6f));
		}
	}

	// (b) The requested duration drives the countdown (no hardcoded 40 ms):
	// a 20 ms stop ends after 20 ms of real advancement.
	UCombatComponent* Bare = NewObject<UCombatComponent>();
	Bare->RequestHitStop(0.02f);
	TestTrue(TEXT("the 20 ms request opened the stop"), Bare->IsHitStopActive());
	for (int32 TickIndex = 0; TickIndex < 3; ++TickIndex)
	{
		Bare->TickCombat(0.005f);
	}
	TestTrue(TEXT("the 20 ms stop is still active after 15 ms"), Bare->IsHitStopActive());
	for (int32 TickIndex = 0; TickIndex < 2; ++TickIndex)
	{
		Bare->TickCombat(0.005f);
	}
	TestFalse(TEXT("the 20 ms stop ended at its own 25 ms tick (a hardcoded 40 ms would still hold)"), Bare->IsHitStopActive());

	// (c) The duration rides on FCombatHit::HitStopSeconds through the hit
	// presentation path: the presenter passes the hit field verbatim to both
	// parties. A synthetic hit with 30 ms stops for exactly 30 ms; a hit with
	// 0 s stops nothing.
	UCombatComponent* Source = NewObject<UCombatComponent>();
	UCombatPresentationComponent* Presenter = NewObject<UCombatPresentationComponent>();
	Presenter->SetSources(Source, nullptr);

	FCombatHit TimedHit;
	TimedHit.AttackInstanceId = 1;
	TimedHit.HitStopSeconds = 0.03f;
	Source->OnHitConfirmed.Broadcast(TimedHit);
	TestTrue(TEXT("the hit field's 30 ms opened the stop through the presenter"), Source->IsHitStopActive());
	for (int32 TickIndex = 0; TickIndex < 5; ++TickIndex)
	{
		Source->TickCombat(0.005f);
	}
	TestTrue(TEXT("the 30 ms stop is still active after 25 ms"), Source->IsHitStopActive());
	for (int32 TickIndex = 0; TickIndex < 2; ++TickIndex)
	{
		Source->TickCombat(0.005f);
	}
	TestFalse(TEXT("the 30 ms stop ended at its own 35 ms tick"), Source->IsHitStopActive());

	// The presenter dispatched one pause and one resume for the stop.
	const TArray<bool>& History = Presenter->GetHitStopPauseDispatchHistory();
	TestTrue(TEXT("the presentation dispatched one pause and one resume"),
		History.Num() == 2 && History[0] && !History[1]);

	FCombatHit ZeroHit;
	ZeroHit.AttackInstanceId = 2;
	ZeroHit.HitStopSeconds = 0.0f;
	Source->OnHitConfirmed.Broadcast(ZeroHit);
	TestFalse(TEXT("a zero-valued hit stop never opens"), Source->IsHitStopActive());
	TestTrue(TEXT("a zero-valued hit dispatched no pause"), History.Num() == 2);
	return true;
}

#endif
