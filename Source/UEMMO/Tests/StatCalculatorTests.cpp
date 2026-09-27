// M3-005: final stat recalculation from base + equipped gear (interface
// contract section 8). Pins the complete from-scratch recalculation (100
// equip/unequip cycles through the real M3-004 equipment model always return
// to the exact base row - no accumulation), the replacement-equals-fresh-
// recalculation idempotence (repeated and reordered recalculation of the same
// snapshot is stable), the CurrentHP clamp semantics on a MaxHP change
// (lowering clamps, raising never heals), the hardening rules (non-finite
// equipment entries rejected as a whole, finite negative fields clamped to 0,
// final MaxHP floored at 1, sums saturated into float range) and the
// single-notification change detection (pure StatsChanged plus the
// FStatChangeTracker delegate firing at most once per actual change). One
// profile regression test pins the M3-005 integration: the snapshot derives
// its stats as the level-curve base plus the stored equipment bonus.
//
// Pure logic only: no World, no assets, no wall clock. The profile fixture
// parents the subsystem to a bare UGameInstance object (the M3-006 precedent:
// UGameInstanceSubsystem is Within=GameInstance; never Init()ed, no World).
//
// Stub-failure note: against the red stub (Recalculate echoes the base row
// and ignores equipment, ClampHealthOnMaxChange echoes CurrentHP, StatsChanged
// always reports false, the profile snapshot still ignores the stored bonus),
// the cycle/replacement/clamp/hardening/profile tests fail on values and the
// change-tracker test fails on the missed second notification; only the
// first-report path of the tracker accidentally holds under the stub. Exact
// red/green counts live in the task report.

#include "Misc/AutomationTest.h"

#include "../Items/StatCalculator.h"
#include "../Items/EquipmentModel.h"
#include "../Profile/ProfileSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "UObject/UObjectGlobals.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_005
{
	/** Quiet NaN factory (UE's TNumericLimits<float> exposes no NaN constant). */
	static float M3_005NaN()
	{
		return std::numeric_limits<float>::quiet_NaN();
	}

	/** Positive infinity factory. */
	static float M3_005Infinity()
	{
		return std::numeric_limits<float>::infinity();
	}

	/** Builds one FItemStats row with explicit values. */
	static FItemStats MakeM3_005Stats(float Attack, float Defense, float MaxHP)
	{
		FItemStats Stats;
		Stats.Attack = Attack;
		Stats.Defense = Defense;
		Stats.MaxHP = MaxHP;
		return Stats;
	}

	/** Builds one valid item definition with a distinct stat shape. */
	static FItemDefinition MakeM3_005Definition(const TCHAR* DefinitionName, EItemSlot Slot,
		float Attack, float Defense, float MaxHP)
	{
		FItemDefinition Definition;
		Definition.DefinitionId = FName(DefinitionName);
		Definition.DisplayName = FString(DefinitionName);
		Definition.Slot = Slot;
		Definition.BaseStats = MakeM3_005Stats(Attack, Defense, MaxHP);
		Definition.Rarity = EItemRarity::Normal;
		return Definition;
	}

	/** Builds a valid instance with a deterministic FGuid (no NewGuid randomness). */
	static FItemInstance MakeM3_005Instance(uint32 Seed, const TCHAR* DefinitionName,
		float Attack, float Defense, float MaxHP)
	{
		FItemInstance Instance;
		Instance.InstanceId = FGuid(Seed, 0x5EEDu, Seed + 7u, Seed * 3u + 1u);
		Instance.DefinitionId = FName(DefinitionName);
		Instance.RollSeed = static_cast<int64>(Seed);
		Instance.RolledStats = MakeM3_005Stats(Attack, Defense, MaxHP);
		Instance.Level = 1;
		return Instance;
	}

	/** Exact float equality on all three fields (deterministic recalculation). */
	static bool M3_005SameStats(const FItemStats& A, const FItemStats& B)
	{
		return A.Attack == B.Attack
			&& A.Defense == B.Defense
			&& A.MaxHP == B.MaxHP;
	}

	/** Asserts one final row against the expected three values. */
	static void M3_005_ExpectStats(FAutomationTestBase& Test, const FString& What,
		const FItemStats& Actual, float ExpectedAttack, float ExpectedDefense, float ExpectedMaxHP)
	{
		Test.TestEqual(FString::Printf(TEXT("%s: the final Attack"), *What), Actual.Attack, ExpectedAttack);
		Test.TestEqual(FString::Printf(TEXT("%s: the final Defense"), *What), Actual.Defense, ExpectedDefense);
		Test.TestEqual(FString::Printf(TEXT("%s: the final MaxHP"), *What), Actual.MaxHP, ExpectedMaxHP);
	}

	/** The shared base row: a level-curve-like 10 Attack / 5 Defense / 100 MaxHP. */
	static FItemStats MakeM3_005BaseStats()
	{
		return MakeM3_005Stats(10.0f, 5.0f, 100.0f);
	}
}

using namespace UE::UEMMO::Tasks::M3_005;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_005EquipUnequipHundredCyclesReturnToBaseStats,
	"UEMMO.Tasks.M3_005.EquipUnequipHundredCyclesReturnToBaseStats",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_005EquipUnequipHundredCyclesReturnToBaseStats::RunTest(const FString& Parameters)
{
	// Card acceptance: repeated equip/unequip must never accumulate stats. Each
	// cycle drives the REAL M3-004 equipment model (equip -> recalculate from
	// base + the equipped row, unequip -> recalculate from base alone) and both
	// results are asserted every cycle, 100 times.
	const FItemStats Base = MakeM3_005BaseStats();
	FItemDefinitionCatalog Catalog;
	Catalog.AddDefinition(MakeM3_005Definition(TEXT("weapon_m3_005"), EItemSlot::Weapon, 5.0f, 0.0f, 20.0f), nullptr);

	FInventoryModel Inventory;
	FEquipmentModel Equipment;
	Equipment.SetDefinitionCatalog(&Catalog);
	Equipment.AttachToInventory(Inventory);

	const FItemInstance Weapon = MakeM3_005Instance(0xA1u, TEXT("weapon_m3_005"), 5.0f, 0.0f, 20.0f);
	if (Inventory.TryAdd(Weapon) != EInventoryAddResult::Added)
	{
		AddError(TEXT("setup: the weapon could not be added to the inventory"));
		return true;
	}

	const FItemStats BasePlusWeapon = MakeM3_005Stats(15.0f, 5.0f, 120.0f);
	for (int32 Cycle = 1; Cycle <= 100; ++Cycle)
	{
		const FString What = FString::Printf(TEXT("cycle %d"), Cycle);

		TestTrue(FString::Printf(TEXT("%s: the weapon equips"), *What),
			Equipment.Equip(EItemSlot::Weapon, Weapon.InstanceId, Inventory) == EEquipmentEquipResult::Equipped);
		if (Cycle == 1)
		{
			// Guard against a vacuous loop: the mapping really toggled.
			TestEqual(TEXT("cycle 1: the weapon slot is occupied while equipped"), Equipment.NumEquippedSlots(), 1);
		}

		TArray<FItemStats> Equipped;
		Equipped.Reset();
		Equipped.Add(Weapon.RolledStats);
		M3_005_ExpectStats(*this, What + TEXT(": equipped row"), FStatCalculator::Recalculate(Base, Equipped),
			BasePlusWeapon.Attack, BasePlusWeapon.Defense, BasePlusWeapon.MaxHP);

		TestTrue(FString::Printf(TEXT("%s: the weapon unequips"), *What),
			Equipment.Unequip(EItemSlot::Weapon) == EEquipmentUnequipResult::Unequipped);
		if (Cycle == 1)
		{
			TestEqual(TEXT("cycle 1: no slot is occupied after unequip"), Equipment.NumEquippedSlots(), 0);
		}

		TArray<FItemStats> Empty;
		Empty.Reset();
		const FItemStats BackToBase = FStatCalculator::Recalculate(Base, Empty);
		M3_005_ExpectStats(*this, What + TEXT(": unequipped row"), BackToBase,
			Base.Attack, Base.Defense, Base.MaxHP);
		TestTrue(FString::Printf(TEXT("%s: the unequipped row is bit-identical to the base row"), *What),
			M3_005SameStats(BackToBase, Base));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_005ReplacementEqualsDirectRecalcFromSnapshot,
	"UEMMO.Tasks.M3_005.ReplacementEqualsDirectRecalcFromSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_005ReplacementEqualsDirectRecalcFromSnapshot::RunTest(const FString& Parameters)
{
	// Card acceptance: replacing equipment must yield exactly what a direct
	// recalculation from the base + current equipped snapshot yields (never an
	// incremental patch), and repeated recalculation of the same snapshot is
	// bit-stable (idempotent - no drift, no cross-call state).
	const FItemStats Base = MakeM3_005BaseStats();
	const FItemStats WeaponA = MakeM3_005Stats(5.0f, 0.0f, 0.0f);
	const FItemStats WeaponB = MakeM3_005Stats(7.0f, 1.0f, 2.0f);

	TArray<FItemStats> EquippedA;
	EquippedA.Reset();
	EquippedA.Add(WeaponA);
	const FItemStats WithA = FStatCalculator::Recalculate(Base, EquippedA);
	M3_005_ExpectStats(*this, TEXT("the first weapon's row"), WithA, 15.0f, 5.0f, 100.0f);

	// The replacement: the equipped snapshot is now [B] alone (same slot).
	TArray<FItemStats> EquippedB;
	EquippedB.Reset();
	EquippedB.Add(WeaponB);
	const FItemStats DirectFromSnapshot = FStatCalculator::Recalculate(Base, EquippedB);
	M3_005_ExpectStats(*this, TEXT("the direct recalculation from the replaced snapshot"), DirectFromSnapshot, 17.0f, 6.0f, 102.0f);

	// Repeated recalculation of the SAME snapshot is bit-identical.
	const FItemStats RepeatOne = FStatCalculator::Recalculate(Base, EquippedB);
	const FItemStats RepeatTwo = FStatCalculator::Recalculate(Base, EquippedB);
	TestTrue(TEXT("the first repeated recalculation is bit-identical"), M3_005SameStats(RepeatOne, DirectFromSnapshot));
	TestTrue(TEXT("the second repeated recalculation is bit-identical"), M3_005SameStats(RepeatTwo, DirectFromSnapshot));

	// No cross-call state: recalculating the OLD snapshot again still reads
	// the old row (a complete from-scratch sum, not an accumulator).
	const FItemStats OldAgain = FStatCalculator::Recalculate(Base, EquippedA);
	TestTrue(TEXT("recalculating the old snapshot again reproduces the old row"), M3_005SameStats(OldAgain, WithA));

	// Two equipped items sum the same regardless of list order.
	TArray<FItemStats> BothAB;
	BothAB.Reset();
	BothAB.Add(WeaponA);
	BothAB.Add(WeaponB);
	TArray<FItemStats> BothBA;
	BothBA.Reset();
	BothBA.Add(WeaponB);
	BothBA.Add(WeaponA);
	const FItemStats FinalAB = FStatCalculator::Recalculate(Base, BothAB);
	const FItemStats FinalBA = FStatCalculator::Recalculate(Base, BothBA);
	M3_005_ExpectStats(*this, TEXT("the both-equipped row"), FinalAB, 22.0f, 6.0f, 102.0f);
	TestTrue(TEXT("the equipped-list order does not change the sum"), M3_005SameStats(FinalAB, FinalBA));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_005MaxHPChangeClampSemantics,
	"UEMMO.Tasks.M3_005.MaxHPChangeClampSemantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_005MaxHPChangeClampSemantics::RunTest(const FString& Parameters)
{
	// Card acceptance: lowering the max correctly clamps the pool ...
	TestEqual(TEXT("lowering 130 -> 110 clamps a 120 pool to 110"),
		FStatCalculator::ClampHealthOnMaxChange(120.0f, 130.0f, 110.0f), 110.0f);
	TestEqual(TEXT("a pool above the new max clamps even while raising"),
		FStatCalculator::ClampHealthOnMaxChange(200.0f, 100.0f, 150.0f), 150.0f);

	// ... and raising the max never restores already-lost health.
	TestEqual(TEXT("raising 100 -> 130 keeps a damaged 80 pool at 80"),
		FStatCalculator::ClampHealthOnMaxChange(80.0f, 100.0f, 130.0f), 80.0f);
	TestEqual(TEXT("an unchanged max keeps the pool"), FStatCalculator::ClampHealthOnMaxChange(50.0f, 100.0f, 100.0f), 50.0f);
	TestEqual(TEXT("an empty pool stays empty"), FStatCalculator::ClampHealthOnMaxChange(0.0f, 100.0f, 200.0f), 0.0f);

	// Hardening: a non-finite current pool reads as 0 (NaN would leak through
	// a raw min() because NaN comparisons are always false).
	const float ClampedNaNPool = FStatCalculator::ClampHealthOnMaxChange(M3_005NaN(), 100.0f, 110.0f);
	TestTrue(TEXT("a NaN current pool clamps to 0"), !FMath::IsNaN(ClampedNaNPool) && ClampedNaNPool == 0.0f);

	// A garbage new max falls back to the old max while that is legal, so the
	// pool is clamped to the last known-good bound instead of destroyed.
	TestEqual(TEXT("a NaN new max falls back to the old 100 for an 80 pool"),
		FStatCalculator::ClampHealthOnMaxChange(80.0f, 100.0f, M3_005NaN()), 80.0f);
	TestEqual(TEXT("a NaN new max falls back to the old 100 for a 200 pool"),
		FStatCalculator::ClampHealthOnMaxChange(200.0f, 100.0f, M3_005NaN()), 100.0f);
	TestEqual(TEXT("a negative new max falls back to the old 100 for an 80 pool"),
		FStatCalculator::ClampHealthOnMaxChange(80.0f, 100.0f, -5.0f), 80.0f);
	TestEqual(TEXT("a NaN old and new max leave nothing legal - the pool reads 0"),
		FStatCalculator::ClampHealthOnMaxChange(M3_005NaN(), M3_005NaN(), M3_005NaN()), 0.0f);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_005InvalidEntriesRejectedAndValuesClamped,
	"UEMMO.Tasks.M3_005.InvalidEntriesRejectedAndValuesClamped",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_005InvalidEntriesRejectedAndValuesClamped::RunTest(const FString& Parameters)
{
	// Hardening rules (documented on FStatCalculator): an equipped entry with
	// ANY non-finite field is rejected as a whole; finite negative fields are
	// clamped to 0 per field; the final MaxHP is floored at 1; sums saturate
	// at MAX_flt instead of overflowing to Inf.
	const FItemStats Base = MakeM3_005BaseStats();
	const float PositiveInfinity = M3_005Infinity();

	// A NaN entry contributes nothing, a later valid entry still counts, and
	// rejection does not depend on the entry order.
	TArray<FItemStats> MixedWithNaN;
	MixedWithNaN.Reset();
	MixedWithNaN.Add(MakeM3_005Stats(M3_005NaN(), 3.0f, 20.0f));
	MixedWithNaN.Add(MakeM3_005Stats(1.0f, 1.0f, 5.0f));
	M3_005_ExpectStats(*this, TEXT("the NaN entry is rejected as a whole"),
		FStatCalculator::Recalculate(Base, MixedWithNaN), 11.0f, 6.0f, 105.0f);

	TArray<FItemStats> MixedReordered;
	MixedReordered.Reset();
	MixedReordered.Add(MakeM3_005Stats(1.0f, 1.0f, 5.0f));
	MixedReordered.Add(MakeM3_005Stats(M3_005NaN(), 3.0f, 20.0f));
	TestTrue(TEXT("the rejection does not depend on the entry order"),
		M3_005SameStats(FStatCalculator::Recalculate(Base, MixedReordered), FStatCalculator::Recalculate(Base, MixedWithNaN)));

	// An infinite entry is poisoned too and rejected as a whole.
	TArray<FItemStats> WithInfinity;
	WithInfinity.Reset();
	WithInfinity.Add(MakeM3_005Stats(PositiveInfinity, 0.0f, 0.0f));
	M3_005_ExpectStats(*this, TEXT("the infinite entry is rejected as a whole"),
		FStatCalculator::Recalculate(Base, WithInfinity), Base.Attack, Base.Defense, Base.MaxHP);

	// Finite negative fields clamp to 0 per field, the rest of the entry counts.
	TArray<FItemStats> WithNegatives;
	WithNegatives.Reset();
	WithNegatives.Add(MakeM3_005Stats(-5.0f, 3.0f, -2.0f));
	M3_005_ExpectStats(*this, TEXT("negative fields clamp to 0"),
		FStatCalculator::Recalculate(Base, WithNegatives), 10.0f, 8.0f, 100.0f);

	// The base row itself is sanitized (never rejected - a final must exist).
	TArray<FItemStats> Empty;
	Empty.Reset();
	M3_005_ExpectStats(*this, TEXT("a NaN/negative base row sanitizes to zeros"),
		FStatCalculator::Recalculate(MakeM3_005Stats(M3_005NaN(), -3.0f, 0.0f), Empty), 0.0f, 0.0f, 1.0f);

	// The final MaxHP floor of 1: an all-zero row still yields a living pool.
	M3_005_ExpectStats(*this, TEXT("an all-zero recalculation floors MaxHP at 1"),
		FStatCalculator::Recalculate(MakeM3_005Stats(0.0f, 0.0f, 0.0f), Empty), 0.0f, 0.0f, 1.0f);

	// Overflow protection: sums saturate at MAX_flt and stay finite.
	TArray<FItemStats> HugeAttack;
	HugeAttack.Reset();
	HugeAttack.Add(MakeM3_005Stats(MAX_flt, 0.0f, 0.0f));
	const FItemStats AttackSaturated = FStatCalculator::Recalculate(MakeM3_005Stats(MAX_flt, 0.0f, 0.0f), HugeAttack);
	TestEqual(TEXT("an overflowing Attack saturates at MAX_flt"), AttackSaturated.Attack, MAX_flt);
	TestTrue(TEXT("the saturated Attack stays finite"), FMath::IsFinite(AttackSaturated.Attack));

	TArray<FItemStats> HugeMaxHP;
	HugeMaxHP.Reset();
	HugeMaxHP.Add(MakeM3_005Stats(0.0f, 0.0f, MAX_flt));
	const FItemStats MaxHPSaturated = FStatCalculator::Recalculate(MakeM3_005Stats(0.0f, 0.0f, MAX_flt), HugeMaxHP);
	TestEqual(TEXT("an overflowing MaxHP saturates at MAX_flt"), MaxHPSaturated.MaxHP, MAX_flt);
	TestTrue(TEXT("the saturated MaxHP stays finite"), FMath::IsFinite(MaxHPSaturated.MaxHP));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_005StatsChangedDetectsOnlyRealChanges,
	"UEMMO.Tasks.M3_005.StatsChangedDetectsOnlyRealChanges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_005StatsChangedDetectsOnlyRealChanges::RunTest(const FString& Parameters)
{
	// The pure comparison: identical rows are unchanged; any single differing
	// field is a change; NaN always reports as changed (never silently
	// swallowed); identical infinities are not a change.
	const FItemStats Row = MakeM3_005Stats(10.0f, 5.0f, 100.0f);

	TestFalse(TEXT("an identical row reports unchanged"), FStatCalculator::StatsChanged(Row, Row));
	TestTrue(TEXT("a differing Attack reports a change"),
		FStatCalculator::StatsChanged(Row, MakeM3_005Stats(11.0f, 5.0f, 100.0f)));
	TestTrue(TEXT("a differing Defense reports a change"),
		FStatCalculator::StatsChanged(Row, MakeM3_005Stats(10.0f, 6.0f, 100.0f)));
	TestTrue(TEXT("a differing MaxHP reports a change"),
		FStatCalculator::StatsChanged(Row, MakeM3_005Stats(10.0f, 5.0f, 101.0f)));

	const FItemStats NaNRow = MakeM3_005Stats(M3_005NaN(), 5.0f, 100.0f);
	const FItemStats AnotherNaNRow = MakeM3_005Stats(M3_005NaN(), 5.0f, 100.0f);
	TestTrue(TEXT("a NaN row against a finite row reports a change"), FStatCalculator::StatsChanged(Row, NaNRow));
	TestTrue(TEXT("two separately built NaN rows still report a change (NaN never equals NaN)"),
		FStatCalculator::StatsChanged(NaNRow, AnotherNaNRow));

	const FItemStats InfinityRow = MakeM3_005Stats(M3_005Infinity(), 5.0f, 100.0f);
	TestFalse(TEXT("two identical infinities are not a change"), FStatCalculator::StatsChanged(InfinityRow, InfinityRow));
	TestTrue(TEXT("an infinity against a finite row reports a change"), FStatCalculator::StatsChanged(Row, InfinityRow));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_005ChangeTrackerNotifiesExactlyOncePerChange,
	"UEMMO.Tasks.M3_005.ChangeTrackerNotifiesExactlyOncePerChange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_005ChangeTrackerNotifiesExactlyOncePerChange::RunTest(const FString& Parameters)
{
	// The single-notification form (documented on FStatChangeTracker): the
	// tracker commits every fresh row, but executes the bound delegate and
	// reports true only when the row REALLY changed - including the very
	// first report - so a UI listening through it can never be event-stormed
	// by repeated identical recalculations.
	int32 NotificationCount = 0;
	FItemStats NotifiedRow;

	FStatChangeTracker Tracker;
	Tracker.OnStatsChanged.BindLambda([&NotificationCount, &NotifiedRow](const FItemStats& NewFinal)
	{
		++NotificationCount;
		NotifiedRow = NewFinal;
	});

	const FItemStats Row = MakeM3_005Stats(10.0f, 5.0f, 100.0f);
	const FItemStats Other = MakeM3_005Stats(12.0f, 5.0f, 100.0f);

	TestFalse(TEXT("nothing was tracked before the first apply"), Tracker.HasSnapshot());
	TestTrue(TEXT("the first apply reports a change"), Tracker.ApplyRecalculation(Row));
	TestEqual(TEXT("the first change notified exactly once"), NotificationCount, 1);
	TestTrue(TEXT("the notification carried the new row"), M3_005SameStats(NotifiedRow, Row));
	TestTrue(TEXT("the first row is tracked"), Tracker.HasSnapshot() && M3_005SameStats(Tracker.GetLastFinal(), Row));

	// The card case: the same recalculation twice - the second must NOT
	// notify (bChanged false, delegate silent).
	TestFalse(TEXT("the repeated identical recalculation reports no change"), Tracker.ApplyRecalculation(Row));
	TestEqual(TEXT("the identical repeat stayed silent"), NotificationCount, 1);

	TestTrue(TEXT("a real change reports again"), Tracker.ApplyRecalculation(Other));
	TestEqual(TEXT("the second real change notified exactly once"), NotificationCount, 2);
	TestTrue(TEXT("the second notification carried the new row"), M3_005SameStats(NotifiedRow, Other));

	TestFalse(TEXT("repeating the new row is silent again"), Tracker.ApplyRecalculation(Other));
	TestEqual(TEXT("no third notification happened"), NotificationCount, 2);

	// After Reset there is no previous row again: the next apply notifies.
	Tracker.Reset();
	TestFalse(TEXT("Reset forgot the tracked row"), Tracker.HasSnapshot());
	TestTrue(TEXT("the first apply after Reset reports a change"), Tracker.ApplyRecalculation(Row));
	TestEqual(TEXT("the post-reset change notified once"), NotificationCount, 3);

	Tracker.OnStatsChanged.Unbind();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_005ProfileSnapshotAddsEquippedBonusToLevelBase,
	"UEMMO.Tasks.M3_005.ProfileSnapshotAddsEquippedBonusToLevelBase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_005ProfileSnapshotAddsEquippedBonusToLevelBase::RunTest(const FString& Parameters)
{
	// Profile integration regression (minimal-increment form): the snapshot's
	// derived stats are the COMPLETE FStatCalculator recalculation of the
	// level-curve base plus the stored equipment bonus row - so after an
	// equip/unequip the snapshot reads level formula + equipment sum, and an
	// unequip (zero bonus) returns to the pure level row. The CurrentHP
	// wiring stays out of scope: the profile holds no CurrentHP (M1-015's
	// HealthComponent owns it); only the pure clamp exists on the calculator.
	UGameInstance* OwnerInstance = NewObject<UGameInstance>(GEngine);
	UProfileSubsystem* Profile = OwnerInstance ? NewObject<UProfileSubsystem>(OwnerInstance) : nullptr;
	if (!TestNotNull(TEXT("the profile subsystem object is constructible without a world"), Profile))
	{
		return true;
	}
	OwnerInstance->AddToRoot();
	Profile->AddToRoot();

	Profile->NewProfile();
	TestTrue(TEXT("NewProfile creates the profile"), Profile->HasProfile());

	const FProfileSnapshot Fresh = Profile->GetProfileSnapshot();
	TestEqual(TEXT("the fresh snapshot derives the level-1 MaxHP 100"), Fresh.MaxHP, 100);
	TestEqual(TEXT("the fresh snapshot derives the level-1 Attack 0"), Fresh.Attack, 0);
	TestEqual(TEXT("the fresh snapshot derives the level-1 Defense 0"), Fresh.Defense, 0);
	const FItemStats ZeroBonus = Profile->GetEquippedStatBonus();
	TestTrue(TEXT("a fresh profile stores a zero equipment bonus"),
		ZeroBonus.Attack == 0.0f && ZeroBonus.Defense == 0.0f && ZeroBonus.MaxHP == 0.0f);

	// "Equip": the owner pushes the equipment sum; the snapshot adds it on
	// top of the level formula.
	Profile->SetEquippedStatBonus(MakeM3_005Stats(2.0f, 1.0f, 10.0f));
	const FProfileSnapshot Equipped = Profile->GetProfileSnapshot();
	TestEqual(TEXT("the equipped snapshot reads MaxHP 100 + 10"), Equipped.MaxHP, 110);
	TestEqual(TEXT("the equipped snapshot reads Attack 0 + 2"), Equipped.Attack, 2);
	TestEqual(TEXT("the equipped snapshot reads Defense 0 + 1"), Equipped.Defense, 1);

	// The bonus stacks on the NEW level base after a level-up: level 3 base
	// is 120/4/2, so with the 10/2/1 bonus the snapshot reads 130/6/3.
	if (TestTrue(TEXT("AddXP(350) levels the profile up to 3"), Profile->AddXP(350) && Profile->GetLevel() == 3))
	{
		const FProfileSnapshot Leveled = Profile->GetProfileSnapshot();
		TestEqual(TEXT("the level-3 equipped snapshot reads MaxHP 120 + 10"), Leveled.MaxHP, 130);
		TestEqual(TEXT("the level-3 equipped snapshot reads Attack 4 + 2"), Leveled.Attack, 6);
		TestEqual(TEXT("the level-3 equipped snapshot reads Defense 2 + 1"), Leveled.Defense, 3);
	}

	// "Unequip": a zero bonus returns the snapshot to the pure level row.
	Profile->SetEquippedStatBonus(FItemStats());
	const FProfileSnapshot Unequipped = Profile->GetProfileSnapshot();
	TestEqual(TEXT("the unequipped snapshot returns to the level-3 MaxHP 120"), Unequipped.MaxHP, 120);
	TestEqual(TEXT("the unequipped snapshot returns to the level-3 Attack 4"), Unequipped.Attack, 4);
	TestEqual(TEXT("the unequipped snapshot returns to the level-3 Defense 2"), Unequipped.Defense, 2);

	// A fresh profile clears the bonus with the rest of the state.
	Profile->SetEquippedStatBonus(MakeM3_005Stats(9.0f, 9.0f, 9.0f));
	Profile->NewProfile();
	const FItemStats ResetBonus = Profile->GetEquippedStatBonus();
	TestTrue(TEXT("NewProfile clears the equipment bonus"),
		ResetBonus.Attack == 0.0f && ResetBonus.Defense == 0.0f && ResetBonus.MaxHP == 0.0f);
	const FProfileSnapshot Restarted = Profile->GetProfileSnapshot();
	TestEqual(TEXT("the restarted snapshot reads the pure level-1 MaxHP 100"), Restarted.MaxHP, 100);
	TestEqual(TEXT("the restarted snapshot reads the pure level-1 Attack 0"), Restarted.Attack, 0);
	TestEqual(TEXT("the restarted snapshot reads the pure level-1 Defense 0"), Restarted.Defense, 0);

	OwnerInstance->RemoveFromRoot();
	Profile->RemoveFromRoot();
	return true;
}

#endif
