// M3-006: level/XP curve and level-up (interface contract section 8). Locks
// the extracted pure FExperienceCurve module: the 100 x Level requirement
// rows with the 0-requirement max level and the [1, 10] clamp semantics, the
// exact/multi-level/remainder behavior of AddExperience (negative amounts
// rejected without change, XP clamped to 0 at the max level), the level
// stat table (MaxHP 100+10x(L-1), Attack 2x(L-1), Defense L-1) and the
// FStatCalculator complete from-scratch recalculation with the M3-005
// reserved equipment-bonus entry. A regression test pins the profile
// integration: after UProfileSubsystem::AddXP the snapshot stats follow the
// NEW level. Pure logic only: no World, no assets, no wall clock, and no
// HealthComponent anywhere (leveling up must never auto-refill HP; the
// calculator is a pure function over plain structs).
//
// Stub-failure note: against the constant-0 red stub, the curve tests
// (requirements, cascade/remainder, stat rows, calculator) fail on values
// while the rejection test and the profile regression test pass - the stub's
// "always rejects with echoed outs" accidentally satisfies the rejection
// semantics, and the profile path still runs the M3-003 inline formulas
// until the extraction refactor. Exact red/green counts live in the task
// report.

#include "Misc/AutomationTest.h"

#include "../Profile/ExperienceCurve.h"
#include "../Profile/ProfileSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_006
{
	/**
	 * Asserts one AddExperience result as the triple (leveled-up flag, new
	 * level, remaining XP) with per-assertion descriptions naming the case.
	 */
	static void M3_006_ExpectCurveResult(FAutomationTestBase& Test, const TCHAR* What,
		bool bReturnedLevelUp, bool bExpectedLevelUp,
		int32 NewLevel, int32 ExpectedLevel,
		int32 RemainingXP, int32 ExpectedXP)
	{
		Test.TestTrue(FString::Printf(TEXT("%s: the level-up flag is %s"),
			What, bExpectedLevelUp ? TEXT("true") : TEXT("false")), bReturnedLevelUp == bExpectedLevelUp);
		Test.TestEqual(FString::Printf(TEXT("%s: the resulting level"), What), NewLevel, ExpectedLevel);
		Test.TestEqual(FString::Printf(TEXT("%s: the remaining XP"), What), RemainingXP, ExpectedXP);
	}

	/** Runs one AddExperience call and checks the full result triple. */
	static void M3_006_AddAndExpect(FAutomationTestBase& Test, const TCHAR* What,
		int32 Level, int32 XP, int32 Amount,
		bool bExpectedLevelUp, int32 ExpectedLevel, int32 ExpectedXP)
	{
		int32 NewLevel = -12345;
		int32 RemainingXP = -12345;
		const bool bLeveledUp = FExperienceCurve::AddExperience(Level, XP, Amount, NewLevel, RemainingXP);
		M3_006_ExpectCurveResult(Test, What, bLeveledUp, bExpectedLevelUp, NewLevel, ExpectedLevel, RemainingXP, ExpectedXP);
	}

	/** Asserts one level stat row (MaxHP/Attack/Defense) of GetBaseStatsForLevel. */
	static void M3_006_ExpectStatRow(FAutomationTestBase& Test, const TCHAR* What,
		int32 Level, int32 ExpectedMaxHP, int32 ExpectedAttack, int32 ExpectedDefense)
	{
		const FLevelBaseStats Row = FExperienceCurve::GetBaseStatsForLevel(Level);
		Test.TestEqual(FString::Printf(TEXT("%s: the row MaxHP"), What), Row.MaxHP, ExpectedMaxHP);
		Test.TestEqual(FString::Printf(TEXT("%s: the row Attack"), What), Row.Attack, ExpectedAttack);
		Test.TestEqual(FString::Printf(TEXT("%s: the row Defense"), What), Row.Defense, ExpectedDefense);
	}
}

using namespace UE::UEMMO::Tasks::M3_006;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_006ExactLevelUpConsumesExactRequirement,
	"UEMMO.Tasks.M3_006.ExactLevelUpConsumesExactRequirement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_006ExactLevelUpConsumesExactRequirement::RunTest(const FString& Parameters)
{
	// The requirement rows: 100 x current level for 1..9, and 0 at the max
	// level (no next level to buy). Out-of-range input clamps into [1, 10].
	TestEqual(TEXT("GetNextLevelXP(1) is 100"), FExperienceCurve::GetNextLevelXP(1), 100);
	TestEqual(TEXT("GetNextLevelXP(2) is 200"), FExperienceCurve::GetNextLevelXP(2), 200);
	TestEqual(TEXT("GetNextLevelXP(9) is 900"), FExperienceCurve::GetNextLevelXP(9), 900);
	TestEqual(TEXT("GetNextLevelXP(10) is 0 (max level has no next level)"),
		FExperienceCurve::GetNextLevelXP(10), 0);
	TestEqual(TEXT("GetNextLevelXP(0) clamps to the level-1 requirement 100"),
		FExperienceCurve::GetNextLevelXP(0), 100);
	TestEqual(TEXT("GetNextLevelXP(-5) clamps to the level-1 requirement 100"),
		FExperienceCurve::GetNextLevelXP(-5), 100);
	TestEqual(TEXT("GetNextLevelXP(11) clamps to the max level's 0"),
		FExperienceCurve::GetNextLevelXP(11), 0);

	// The card's exact case: +100 XP from level 1 pays exactly the 100 x 1
	// requirement and lands on level 2 with 0 XP left.
	M3_006_AddAndExpect(*this, TEXT("level 1 gains exactly 100 XP"),
		1, 0, 100, /*bExpectedLevelUp*/ true, /*ExpectedLevel*/ 2, /*ExpectedXP*/ 0);

	// Exact payment works from any level below the top: 400 x 4 buys level 5.
	M3_006_AddAndExpect(*this, TEXT("level 4 gains exactly 400 XP"),
		4, 0, 400, /*bExpectedLevelUp*/ true, /*ExpectedLevel*/ 5, /*ExpectedXP*/ 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_006MultiLevelCascadeRetainsRemainder,
	"UEMMO.Tasks.M3_006.MultiLevelCascadeRetainsRemainder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_006MultiLevelCascadeRetainsRemainder::RunTest(const FString& Parameters)
{
	// The card's second case, verified against the 100 x Level curve: 350 XP
	// from level 1 pays 100 (level 1 -> 2) plus 200 (level 2 -> 3) and keeps
	// the 50 remainder below level 3's 300 requirement - the loop consumes
	// per boundary, it does not just add one level.
	M3_006_AddAndExpect(*this, TEXT("level 1 gains 350 XP"),
		1, 0, 350, /*bExpectedLevelUp*/ true, /*ExpectedLevel*/ 3, /*ExpectedXP*/ 50);

	// A smaller cascade banks the remainder too: 150 pays the 100
	// requirement and retains 50 below level 2's 200.
	M3_006_AddAndExpect(*this, TEXT("level 1 gains 150 XP"),
		1, 0, 150, /*bExpectedLevelUp*/ true, /*ExpectedLevel*/ 2, /*ExpectedXP*/ 50);

	// A banked remainder keeps feeding the cascade: from level 2 with 50 XP,
	// +450 makes 500 total; level 2 costs 200 (300 left), level 3 costs 300
	// (0 left) and level 4's 400 stops the loop - two level-ups in one call.
	M3_006_AddAndExpect(*this, TEXT("level 2 with 50 banked XP gains 450 XP"),
		2, 50, 450, /*bExpectedLevelUp*/ true, /*ExpectedLevel*/ 4, /*ExpectedXP*/ 0);

	// The full climb costs 100+200+...+900 = 4500: one call lands exactly on
	// the max level with 0 XP.
	M3_006_AddAndExpect(*this, TEXT("level 1 gains the full 4500 XP climb"),
		1, 0, 4500, /*bExpectedLevelUp*/ true, /*ExpectedLevel*/ 10, /*ExpectedXP*/ 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_006NonPositiveAmountRejectedWithoutChange,
	"UEMMO.Tasks.M3_006.NonPositiveAmountRejectedWithoutChange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_006NonPositiveAmountRejectedWithoutChange::RunTest(const FString& Parameters)
{
	// XP only grows: a negative amount is rejected with no change at all -
	// the reported state echoes the unchanged inputs.
	M3_006_AddAndExpect(*this, TEXT("a level 1 profile with 0 XP gains -7 XP"),
		1, 0, -7, /*bExpectedLevelUp*/ false, /*ExpectedLevel*/ 1, /*ExpectedXP*/ 0);

	// Zero cannot grow XP either.
	M3_006_AddAndExpect(*this, TEXT("a level 1 profile with 0 XP gains 0 XP"),
		1, 0, 0, /*bExpectedLevelUp*/ false, /*ExpectedLevel*/ 1, /*ExpectedXP*/ 0);

	// A rejection must not touch an existing XP pool either.
	M3_006_AddAndExpect(*this, TEXT("a level 3 profile with 40 XP gains -100 XP"),
		3, 40, -100, /*bExpectedLevelUp*/ false, /*ExpectedLevel*/ 3, /*ExpectedXP*/ 40);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_006MaxLevelClampsXPToZero,
	"UEMMO.Tasks.M3_006.MaxLevelClampsXPToZero",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_006MaxLevelClampsXPToZero::RunTest(const FString& Parameters)
{
	// At the max level XP has no meaning: the curve clamps it to 0 and
	// reports no level-up, whatever the amount.
	M3_006_AddAndExpect(*this, TEXT("the max level gains 1000 XP"),
		10, 0, 1000, /*bExpectedLevelUp*/ false, /*ExpectedLevel*/ 10, /*ExpectedXP*/ 0);

	// Even a nonzero starting pool reads 0 at the top level.
	M3_006_AddAndExpect(*this, TEXT("the max level with 999 banked XP gains 5 XP"),
		10, 999, 5, /*bExpectedLevelUp*/ false, /*ExpectedLevel*/ 10, /*ExpectedXP*/ 0);

	// A cascade that LANDS on the max level clamps the landing remainder:
	// 4600 pays the full 4500 climb and the extra 100 is discarded at 10.
	M3_006_AddAndExpect(*this, TEXT("level 1 gains 4600 XP, overshooting the climb"),
		1, 0, 4600, /*bExpectedLevelUp*/ true, /*ExpectedLevel*/ 10, /*ExpectedXP*/ 0);

	// A saturating addition cannot wrap into a negative XP pool: it just
	// levels to the top and clamps.
	M3_006_AddAndExpect(*this, TEXT("level 1 gains MAX_int32 XP"),
		1, 0, MAX_int32, /*bExpectedLevelUp*/ true, /*ExpectedLevel*/ 10, /*ExpectedXP*/ 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_006BaseStatsFollowLevelFormula,
	"UEMMO.Tasks.M3_006.BaseStatsFollowLevelFormula",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_006BaseStatsFollowLevelFormula::RunTest(const FString& Parameters)
{
	// The design table rows: level 1 = 100/0/0, level 3 = 120/4/2, level 10
	// = 190/18/9 (the initial values are a design choice; later tuning must
	// be recorded).
	M3_006_ExpectStatRow(*this, TEXT("the level-1 row"), 1, 100, 0, 0);
	M3_006_ExpectStatRow(*this, TEXT("the level-3 row"), 3, 120, 4, 2);
	M3_006_ExpectStatRow(*this, TEXT("the level-10 row"), 10, 190, 18, 9);

	// Out-of-range levels clamp into the closed [1, 10] table.
	M3_006_ExpectStatRow(*this, TEXT("the clamped level-0 row"), 0, 100, 0, 0);
	M3_006_ExpectStatRow(*this, TEXT("the clamped level-99 row"), 99, 190, 18, 9);
	M3_006_ExpectStatRow(*this, TEXT("the clamped level-(-1) row"), -1, 100, 0, 0);

	// The individual formulas agree with the row helper.
	TestEqual(TEXT("GetMaxHPForLevel(3) is 120"), FExperienceCurve::GetMaxHPForLevel(3), 120);
	TestEqual(TEXT("GetAttackForLevel(3) is 4"), FExperienceCurve::GetAttackForLevel(3), 4);
	TestEqual(TEXT("GetDefenseForLevel(3) is 2"), FExperienceCurve::GetDefenseForLevel(3), 2);
	TestEqual(TEXT("GetMaxHPForLevel(10) is 190"), FExperienceCurve::GetMaxHPForLevel(10), 190);
	TestEqual(TEXT("GetDefenseForLevel(10) is 9"), FExperienceCurve::GetDefenseForLevel(10), 9);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_006StatCalculatorRecalculatesCompleteBaseStats,
	"UEMMO.Tasks.M3_006.StatCalculatorRecalculatesCompleteBaseStats",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_006StatCalculatorRecalculatesCompleteBaseStats::RunTest(const FString& Parameters)
{
	// With the (M3-005-reserved) equipment bonus left at its default zero,
	// the recalculation returns the pure curve row untouched.
	const FLevelBaseStats Level3Row = FExperienceCurve::GetBaseStatsForLevel(3);
	const FLevelBaseStats RecalculatedDefault = FStatCalculator::Recalculate(Level3Row);
	TestEqual(TEXT("the default recalculation keeps the level-3 MaxHP"), RecalculatedDefault.MaxHP, 120);
	TestEqual(TEXT("the default recalculation keeps the level-3 Attack"), RecalculatedDefault.Attack, 4);
	TestEqual(TEXT("the default recalculation keeps the level-3 Defense"), RecalculatedDefault.Defense, 2);

	// The reserved equipment entry is already a complete from-scratch sum:
	// base row + bonus, recomputed whole every call (never incremental).
	FLevelBaseStats EquipmentBonus;
	EquipmentBonus.MaxHP = 10;
	EquipmentBonus.Attack = 2;
	EquipmentBonus.Defense = 1;
	const FLevelBaseStats Final = FStatCalculator::Recalculate(Level3Row, EquipmentBonus);
	TestEqual(TEXT("the final MaxHP sums base 120 plus equipment 10"), Final.MaxHP, 130);
	TestEqual(TEXT("the final Attack sums base 4 plus equipment 2"), Final.Attack, 6);
	TestEqual(TEXT("the final Defense sums base 2 plus equipment 1"), Final.Defense, 3);

	// Recalculation from the NEW base stats after a level-up: the cascade of
	// the card's 350-XP case ends at level 3, and recomputing the whole
	// block from that level reads exactly the level-3 row. No HealthComponent
	// exists anywhere in this chain - leveling up never auto-refills HP; any
	// CurrentHP clamping against a changed MaxHP belongs to the caller
	// (M3-005/M3-010 wiring).
	int32 NewLevel = 0;
	int32 RemainingXP = 0;
	TestTrue(TEXT("the 350-XP cascade levels up"), FExperienceCurve::AddExperience(1, 0, 350, NewLevel, RemainingXP));
	const FLevelBaseStats NewBase = FExperienceCurve::GetBaseStatsForLevel(NewLevel);
	const FLevelBaseStats NewFinal = FStatCalculator::Recalculate(NewBase);
	TestEqual(TEXT("the recalculation from the new level reads MaxHP 120"), NewFinal.MaxHP, 120);
	TestEqual(TEXT("the recalculation from the new level reads Attack 4"), NewFinal.Attack, 4);
	TestEqual(TEXT("the recalculation from the new level reads Defense 2"), NewFinal.Defense, 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_006ProfileSnapshotStatsFollowNewLevelAfterAddXP,
	"UEMMO.Tasks.M3_006.ProfileSnapshotStatsFollowNewLevelAfterAddXP",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_006ProfileSnapshotStatsFollowNewLevelAfterAddXP::RunTest(const FString& Parameters)
{
	// Regression over the M3-006 extraction: the profile's formulas are now
	// one-line delegations to FExperienceCurve, so the subsystem must behave
	// exactly as before. UGameInstanceSubsystem is Within=GameInstance, so
	// this world-free fixture parents the subsystem to a bare UGameInstance
	// object (the engine's own NewObject<UGameInstance>(GEngine) precedent,
	// never Init()ed - no subsystem collection, no World). The test pins the
	// formula/snapshot math only; the real production instantiation and the
	// GameInstance lifetime behavior stay with the M3-003 suite.
	UGameInstance* OwnerInstance = NewObject<UGameInstance>(GEngine);
	UProfileSubsystem* Profile = OwnerInstance ? NewObject<UProfileSubsystem>(OwnerInstance) : nullptr;
	if (!TestNotNull(TEXT("the profile subsystem object is constructible without a world"), Profile))
	{
		return true;
	}
	OwnerInstance->AddToRoot();
	Profile->AddToRoot();

	TestFalse(TEXT("a fresh object has no profile"), Profile->HasProfile());
	Profile->NewProfile();
	TestTrue(TEXT("NewProfile creates the profile"), Profile->HasProfile());

	// The card's 150-XP case through the profile: level 2 with the 50
	// remainder, and the snapshot derives the level-2 row 110/2/1 - the
	// stats follow the NEW level, never the pre-gain one.
	if (!TestTrue(TEXT("AddXP(150) levels the profile up"), Profile->AddXP(150)))
	{
		OwnerInstance->RemoveFromRoot();
		Profile->RemoveFromRoot();
		return true;
	}
	TestEqual(TEXT("the profile reached level 2"), Profile->GetLevel(), 2);
	TestEqual(TEXT("the profile kept 50 XP"), Profile->GetXP(), 50);

	const FProfileSnapshot Snapshot = Profile->GetProfileSnapshot();
	TestEqual(TEXT("the snapshot mirrors level 2"), Snapshot.Level, 2);
	TestEqual(TEXT("the snapshot mirrors the 50 XP"), Snapshot.XP, 50);
	TestEqual(TEXT("the snapshot derives the level-2 MaxHP 110"), Snapshot.MaxHP, 110);
	TestEqual(TEXT("the snapshot derives the level-2 Attack 2"), Snapshot.Attack, 2);
	TestEqual(TEXT("the snapshot derives the level-2 Defense 1"), Snapshot.Defense, 1);

	// The static surface routes through the shared curve after the refactor.
	TestEqual(TEXT("UProfileSubsystem::GetNextLevelXP(3) reads the curve's 300"),
		UProfileSubsystem::GetNextLevelXP(3), 300);
	TestEqual(TEXT("UProfileSubsystem::GetMaxHPForLevel(10) reads the curve's 190"),
		UProfileSubsystem::GetMaxHPForLevel(10), 190);

	OwnerInstance->RemoveFromRoot();
	Profile->RemoveFromRoot();
	return true;
}

#endif
