#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/CombatComponent.h"
#include "../PrototypeHUD.h"
#include "../UI/DamageNumberModel.h"

#if WITH_DEV_AUTOMATION_TESTS

// M1-035: pure-logic tests for the debug HUD's hit-feedback display model
// (damage-number pool, health bar data, combo counter) plus the HUD's
// event-driven feed binding. All world-less: the model runs on an explicitly
// injected clock and the HUD feed is driven by broadcasting OnHitConfirmed on
// a bare component, exactly the event the real damage application raises.

namespace UE::UEMMO::Tasks::M1_035
{
	// Builds one accepted-hit event with the real applied values the contract
	// guarantees (Damage is the ApplyDamage return value, > 0).
	static FCombatHit M1_035_MakeHit(float Damage, const FVector& Location, FName AttackId)
	{
		FCombatHit Hit;
		Hit.Damage = Damage;
		Hit.WorldHitLocation = Location;
		Hit.AttackId = AttackId;
		return Hit;
	}
}

using namespace UE::UEMMO::Tasks::M1_035;

// Three accepted hits enter the pool in order; numbers expire 0.6 s after
// their spawn on the injected clock (never advanced internally), the pool is
// reusable after draining and Clear() empties it immediately.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_035DamageNumbersAddExpireByInjectedClock,
	"UEMMO.Tasks.M1_035.DamageNumbersAddExpireByInjectedClock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_035DamageNumbersAddExpireByInjectedClock::RunTest(const FString& Parameters)
{
	FDamageNumberPool Pool;
	Pool.SetNowSeconds(10.0);
	Pool.Add(10.0f, FVector(1.0, 2.0, 3.0), FName(TEXT("light_01")));
	Pool.SetNowSeconds(10.1);
	Pool.Add(8.0f, FVector(4.0, 5.0, 6.0), FName(TEXT("light_02")));
	Pool.SetNowSeconds(10.2);
	Pool.Add(12.0f, FVector(7.0, 8.0, 9.0), FName(TEXT("light_01")));

	// Early return instead of indexing an empty pool: a failing count must
	// fail the test, never crash the runner.
	if (!TestEqual(TEXT("three accepted hits put three numbers into the pool"), Pool.Num(), 3))
	{
		return true;
	}
	TestEqual(TEXT("the first entry carries the actually applied damage"), Pool.GetEntries()[0].Damage, 10.0f);
	TestEqual(TEXT("the first entry keeps its spawn time"), Pool.GetEntries()[0].SpawnTimeSeconds, 10.0);
	TestTrue(TEXT("the first entry keeps the world hit location"),
		Pool.GetEntries()[0].Location.Equals(FVector(1.0, 2.0, 3.0)));
	TestEqual(TEXT("the first entry carries the attack id"),
		Pool.GetEntries()[0].AttackId, FName(TEXT("light_01")));

	// 0.59 s after the third spawn: nothing expired yet.
	Pool.SetNowSeconds(10.79);
	if (!TestEqual(TEXT("an entry younger than 0.6 s stays alive"), Pool.Num(), 1))
	{
		return true;
	}
	TestEqual(TEXT("the surviving entry is the newest one"), Pool.GetEntries()[0].Damage, 12.0f);

	// 0.61 s after the third spawn: the whole 0.6 s life is over, all cleared.
	Pool.SetNowSeconds(10.81);
	TestEqual(TEXT("after the 0.6 s lifetime the pool is empty"), Pool.Num(), 0);

	// The drained pool accepts new numbers again.
	Pool.Add(5.0f, FVector::ZeroVector, NAME_None);
	TestEqual(TEXT("a drained pool accepts new numbers"), Pool.Num(), 1);

	// Clear empties the pool immediately (reset flow).
	Pool.Clear();
	TestEqual(TEXT("Clear empties the pool immediately"), Pool.Num(), 0);
	return true;
}

// Adding past the 32-entry cap evicts the earliest entries while the pool
// stays at exactly 32; every remaining entry stays plainly readable (the pool
// owns data copies, so eviction can never dangle a reference).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_035DamageNumbersCapThirtyTwoEvictsEarliest,
	"UEMMO.Tasks.M1_035.DamageNumbersCapThirtyTwoEvictsEarliest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_035DamageNumbersCapThirtyTwoEvictsEarliest::RunTest(const FString& Parameters)
{
	FDamageNumberPool Pool;

	// 35 adds over a 0.34 s span: nothing expires (0.34 < 0.6), so the cap is
	// what removes entries - exactly the three earliest ones.
	for (int32 Index = 0; Index < 35; ++Index)
	{
		Pool.SetNowSeconds(Index * 0.01);
		Pool.Add(10.0f, FVector::ZeroVector, NAME_None);
	}

	if (!TestEqual(TEXT("after 35 adds the pool holds exactly the 32-entry cap"), Pool.Num(), 32))
	{
		return true;
	}
	TestTrue(TEXT("the earliest surviving entry spawned at 0.03 (three earliest evicted)"),
		FMath::IsNearlyEqual(Pool.GetEntries()[0].SpawnTimeSeconds, 0.03, 0.0001));
	TestTrue(TEXT("the newest entry spawned at 0.34 and survived"),
		FMath::IsNearlyEqual(Pool.GetEntries()[Pool.Num() - 1].SpawnTimeSeconds, 0.34, 0.0001));

	// Full iteration over the capped pool reads every entry without any stale
	// access (data copies only).
	float DamageSum = 0.0f;
	for (const FDamageNumberEntry& Entry : Pool.GetEntries())
	{
		DamageSum += Entry.Damage;
	}
	TestEqual(TEXT("all 32 surviving entries are readable and intact"), DamageSum, 320.0f);

	// One more add still evicts only the earliest entry.
	Pool.SetNowSeconds(0.35);
	Pool.Add(10.0f, FVector::ZeroVector, NAME_None);
	if (!TestEqual(TEXT("the pool stays at the 32-entry cap after further adds"), Pool.Num(), 32))
	{
		return true;
	}
	TestTrue(TEXT("the next-earliest entry (0.04) now fronts the pool"),
		FMath::IsNearlyEqual(Pool.GetEntries()[0].SpawnTimeSeconds, 0.04, 0.0001));
	return true;
}

// Health bar display data tracks a real health pool change (100/100 -> 90/100
// reads back exactly what was fed, never an invented value), clamps race
// writes into the frame and carries the alive flag and the no-data state.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_035HealthBarDataTracksChangeReadable,
	"UEMMO.Tasks.M1_035.HealthBarDataTracksChangeReadable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_035HealthBarDataTracksChangeReadable::RunTest(const FString& Parameters)
{
	FHealthBarData Data;
	TestFalse(TEXT("a fresh bar has no data yet (player placeholder state)"), Data.bHasData);
	TestEqual(TEXT("a fresh bar draws an empty ratio"), Data.GetRatio(), 0.0f);

	Data.Set(100.0f, 100.0f, true);
	TestTrue(TEXT("a fed bar has data"), Data.bHasData);
	TestEqual(TEXT("full health reads 100"), Data.CurrentHP, 100.0f);
	TestEqual(TEXT("max health reads 100"), Data.MaxHP, 100.0f);
	TestTrue(TEXT("an alive pool reports alive"), Data.bAlive);
	TestEqual(TEXT("a full pool fills the bar completely"), Data.GetRatio(), 1.0f);

	Data.Set(90.0f, 100.0f, true);
	TestEqual(TEXT("after a 10-damage hit the bar reads 90"), Data.CurrentHP, 90.0f);
	TestEqual(TEXT("the max stays 100"), Data.MaxHP, 100.0f);
	TestTrue(TEXT("the change is readable as the 0.9 fill ratio"), FMath::IsNearlyEqual(Data.GetRatio(), 0.9f));

	// Race writes clamp into the frame instead of overflowing the bar.
	Data.Set(-3.0f, 100.0f, true);
	TestEqual(TEXT("a negative current clamps to 0"), Data.CurrentHP, 0.0f);
	Data.Set(250.0f, 100.0f, true);
	TestEqual(TEXT("an over-max current clamps to the max"), Data.CurrentHP, 100.0f);

	Data.Set(0.0f, 100.0f, false);
	TestFalse(TEXT("a dead pool reports not alive"), Data.bAlive);
	TestEqual(TEXT("a dead pool reads 0 health"), Data.CurrentHP, 0.0f);

	// A non-positive max never divides by zero: ratio stays a safe 0.
	Data.Set(50.0f, 0.0f, true);
	TestEqual(TEXT("a non-positive max yields the safe empty ratio"), Data.GetRatio(), 0.0f);
	return true;
}

// Consecutive hits inside the 1.5 s window increment the combo (1 -> 2 -> 3);
// once the window elapsed since the last hit the counter reads 0 and the next
// hit starts a fresh count at 1. The window is configurable; Reset clears.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_035ComboCounterIncrementsAndTimesOut,
	"UEMMO.Tasks.M1_035.ComboCounterIncrementsAndTimesOut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_035ComboCounterIncrementsAndTimesOut::RunTest(const FString& Parameters)
{
	FComboCounter Combo;
	TestTrue(TEXT("the default window is the configured 1.5 s"),
		FMath::IsNearlyEqual(Combo.GetTimeoutSeconds(), 1.5, 0.0001));

	// Three hits 0.5 s apart all land inside the window.
	TestEqual(TEXT("the first hit opens the combo at 1"), Combo.NotifyHit(0.0), 1);
	TestEqual(TEXT("a hit inside the window increments to 2"), Combo.NotifyHit(0.5), 2);
	TestEqual(TEXT("a third hit inside the window increments to 3"), Combo.NotifyHit(1.0), 3);
	TestEqual(TEXT("the combo reads 3 right after the last hit"), Combo.EvaluateCombo(1.0), 3);
	TestEqual(TEXT("the combo still reads 3 at 1.49 s after the last hit"), Combo.EvaluateCombo(2.49), 3);

	// Exactly 1.5 s after the last hit the combo has timed out (boundary is
	// inclusive: at the window edge the counter already reads 0).
	TestEqual(TEXT("at the 1.5 s window edge the combo reads 0"), Combo.EvaluateCombo(2.5), 0);

	// The next hit after the timeout starts a fresh count at 1.
	TestEqual(TEXT("a hit after the timeout restarts the combo at 1"), Combo.NotifyHit(3.0), 1);
	TestEqual(TEXT("the restarted combo reads 1"), Combo.EvaluateCombo(3.0), 1);

	// A configurable shorter window times out correspondingly earlier.
	FComboCounter Short(0.8);
	TestEqual(TEXT("a custom window is carried"), Short.NotifyHit(10.0), 1);
	TestEqual(TEXT("a hit inside the custom window increments"), Short.NotifyHit(10.5), 2);
	TestEqual(TEXT("the custom window keeps the combo alive at 0.79 s elapsed"),
		Short.EvaluateCombo(11.29), 2);
	TestEqual(TEXT("a hit past the custom window restarts at 1"), Short.NotifyHit(11.5), 1);

	// Reset clears the count entirely (reset flow).
	Short.Reset();
	TestEqual(TEXT("Reset clears the combo to 0"), Short.EvaluateCombo(11.5), 0);
	return true;
}

// Event-driven feed: broadcasting the real OnHitConfirmed on the bound
// component adds exactly one damage number per accepted hit with the actual
// applied values; without a broadcast nothing enters the pool.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_035HitConfirmedFeedAddsExactlyOnePerAcceptedHit,
	"UEMMO.Tasks.M1_035.HitConfirmedFeedAddsExactlyOnePerAcceptedHit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_035HitConfirmedFeedAddsExactlyOnePerAcceptedHit::RunTest(const FString& Parameters)
{
	APrototypeHUD* HUD = NewObject<APrototypeHUD>();
	UCombatComponent* Combat = NewObject<UCombatComponent>();
	TestNotNull(TEXT("the bare HUD exists"), HUD);
	TestNotNull(TEXT("the bare combat component exists"), Combat);
	if (HUD == nullptr || Combat == nullptr)
	{
		return true;
	}

	HUD->BindDamageFeed(Combat);
	TestEqual(TEXT("an empty pool starts at 0"), HUD->PeekDamageNumberCount(), 0);

	// One accepted hit -> exactly one number carrying the real applied values.
	Combat->OnHitConfirmed.Broadcast(M1_035_MakeHit(10.0f, FVector(195.0, 0.0, 90.0), FName(TEXT("light_01"))));
	if (!TestEqual(TEXT("one broadcast adds exactly one damage number"), HUD->PeekDamageNumberCount(), 1))
	{
		return true;
	}
	TestEqual(TEXT("the number carries the actually applied 10 damage"),
		HUD->PeekDamageNumbers().GetEntries()[0].Damage, 10.0f);
	TestTrue(TEXT("the number anchors at the hit location"),
		HUD->PeekDamageNumbers().GetEntries()[0].Location.Equals(FVector(195.0, 0.0, 90.0)));
	TestEqual(TEXT("the number carries the attack id"),
		HUD->PeekDamageNumbers().GetEntries()[0].AttackId, FName(TEXT("light_01")));

	// Two more accepted hits: the pool grows one per event and the combo
	// counts the consecutive hits (world-less clock reads 0.0, inside any
	// window).
	Combat->OnHitConfirmed.Broadcast(M1_035_MakeHit(10.0f, FVector::ZeroVector, FName(TEXT("light_01"))));
	Combat->OnHitConfirmed.Broadcast(M1_035_MakeHit(10.0f, FVector::ZeroVector, FName(TEXT("light_01"))));
	TestEqual(TEXT("each accepted hit adds exactly one number"), HUD->PeekDamageNumberCount(), 3);
	TestEqual(TEXT("three consecutive accepted hits count combo 3"),
		HUD->PeekComboCount(0.0), 3);

	// No further broadcast: nothing enters the pool (numbers only arrive
	// through the event, never from a per-frame path).
	TestEqual(TEXT("without a further hit nothing is added"), HUD->PeekDamageNumberCount(), 3);
	return true;
}

// Stale-source safety: unbinding a destroyed component never touches the dead
// object (weak references are checked first), a re-bound feed delivers exactly
// once and a broadcast from a destroyed source is ignored without crashing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_035StaleSourceBindingReadsSafeNoCrash,
	"UEMMO.Tasks.M1_035.StaleSourceBindingReadsSafeNoCrash",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_035StaleSourceBindingReadsSafeNoCrash::RunTest(const FString& Parameters)
{
	APrototypeHUD* HUD = NewObject<APrototypeHUD>();
	UCombatComponent* First = NewObject<UCombatComponent>();
	UCombatComponent* Second = NewObject<UCombatComponent>();
	TestNotNull(TEXT("the bare HUD exists"), HUD);
	TestNotNull(TEXT("the first component exists"), First);
	TestNotNull(TEXT("the second component exists"), Second);
	if (HUD == nullptr || First == nullptr || Second == nullptr)
	{
		return true;
	}

	HUD->BindDamageFeed(First);
	First->OnHitConfirmed.Broadcast(M1_035_MakeHit(10.0f, FVector::ZeroVector, NAME_None));
	TestEqual(TEXT("the bound source delivers its hit"), HUD->PeekDamageNumberCount(), 1);

	// The first source dies while bound; re-binding must unbind it only
	// through its (now invalid) weak reference - no stale access, no crash.
	First->MarkAsGarbage();
	HUD->BindDamageFeed(Second);

	Second->OnHitConfirmed.Broadcast(M1_035_MakeHit(10.0f, FVector::ZeroVector, NAME_None));
	TestEqual(TEXT("the re-bound source delivers exactly once (no stale double delivery)"),
		HUD->PeekDamageNumberCount(), 2);

	// The second source dies too: a broadcast from the destroyed component
	// reaches the handler but the invalid weak reference skips the feed.
	Second->MarkAsGarbage();
	Second->OnHitConfirmed.Broadcast(M1_035_MakeHit(10.0f, FVector::ZeroVector, NAME_None));
	TestEqual(TEXT("a broadcast from a destroyed source adds nothing and does not crash"),
		HUD->PeekDamageNumberCount(), 2);

	// Unbinding the destroyed source is safe as well; the seams all read.
	HUD->BindDamageFeed(nullptr);
	TestEqual(TEXT("the pool keeps its entries after the safe unbind"),
		HUD->PeekDamageNumberCount(), 2);
	TestEqual(TEXT("the combo seam reads safely after the stale unbind (the skipped stale hit never counted)"),
		HUD->PeekComboCount(0.0), 2);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
