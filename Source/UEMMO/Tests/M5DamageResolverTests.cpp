// M5-011: the pure-function damage resolver (interface contract section 1,
// owner 011). Pins the legacy M1-019 formula reproduction
// max(1, round((base + AttackPower * coefficient) * 100 / (100 + max(0, Defense))))
// over the four legacy melee attacks (10/14/18/12 at AttackPower=0 and
// Defense=0), damage-immunity as an exact zero (never the legacy min-1 fake
// damage), poise as a control-side-only fact that never touches the damage
// number, gate-driven control acceptance and the refusal of every non-finite
// or negative input. Pure checks: this file only builds plain structs and
// calls the resolver; it never touches worlds, assets, wall clocks or random
// number streams.

#include "Misc/AutomationTest.h"

#include "../Combat/System/DamageResolver.h"

#include <limits>
#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Compile-time pins: the resolution outcome is a value snapshot, never a
// World/Actor pointer holder (interface contract section 0.5), and the block
// reason ordinals are append-only (never renumber).
// ---------------------------------------------------------------------------

static_assert(!std::is_pointer_v<decltype(FDamageOutcome::FinalDamage)>, "FDamageOutcome::FinalDamage must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageOutcome::bWasImmune)>, "FDamageOutcome::bWasImmune must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageOutcome::bWasBlocked)>, "FDamageOutcome::bWasBlocked must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageOutcome::bControlAccepted)>, "FDamageOutcome::bControlAccepted must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageOutcome::bStaggerAccepted)>, "FDamageOutcome::bStaggerAccepted must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageOutcome::bLaunchAccepted)>, "FDamageOutcome::bLaunchAccepted must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageOutcome::bControlFacesPoiseThreshold)>, "FDamageOutcome::bControlFacesPoiseThreshold must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageOutcome::BlockReason)>, "FDamageOutcome::BlockReason must stay a value type");
static_assert(std::is_trivially_copyable_v<FDamageOutcome>, "FDamageOutcome must stay a trivially copyable value snapshot");
static_assert(std::is_default_constructible_v<FDamageOutcome>, "FDamageOutcome must stay default constructible");

static_assert(static_cast<uint8>(EDamageBlockReason::None) == 0, "EDamageBlockReason::None must stay 0");
static_assert(static_cast<uint8>(EDamageBlockReason::NonFiniteInput) == 1, "EDamageBlockReason::NonFiniteInput must stay 1");
static_assert(static_cast<uint8>(EDamageBlockReason::NegativeInput) == 2, "EDamageBlockReason::NegativeInput must stay 2");
static_assert(static_cast<uint8>(EDamageBlockReason::NonPositiveBaseDamage) == 3, "EDamageBlockReason::NonPositiveBaseDamage must stay 3");
static_assert(static_cast<uint8>(EDamageBlockReason::OverflowedResult) == 4, "EDamageBlockReason::OverflowedResult must stay 4");

namespace UE::UEMMO::Tasks::M5_011
{
	/** One of the four legacy melee attack profiles (M5-003 pinned values). */
	static FDamageProfile MakeLegacyProfile(const TCHAR* Id, float BaseDamage)
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = Id;
		Profile.BaseDamage = BaseDamage;
		Profile.AttackCoefficient = 1.0f;
		Profile.HitStunSeconds = 0.0f;
		Profile.KnockbackCmPerSecond = 0.0f;
		Profile.LaunchCmPerSecond = 0.0f;
		Profile.HitStopSeconds = 0.04f;
		return Profile;
	}

	/** The four legacy melee attacks with their exact legacy base damages. */
	static TArray<FDamageProfile> MakeLegacyFour()
	{
		TArray<FDamageProfile> Profiles;
		Profiles.Add(MakeLegacyProfile(TEXT("light_01"), 10.0f));
		Profiles.Add(MakeLegacyProfile(TEXT("light_02"), 14.0f));
		Profiles.Add(MakeLegacyProfile(TEXT("launcher"), 18.0f));
		Profiles.Add(MakeLegacyProfile(TEXT("aerial_01"), 12.0f));
		return Profiles;
	}

	/** The legacy normal target: every gate open, no immunity, no poise. */
	static FTargetReaction MakeNormalTarget()
	{
		FTargetReaction Target;
		Target.PolicyId = TEXT("normal");
		return Target;
	}

	/**
	 * The M1-019 legacy formula, copied line for line from
	 * M1_019_ComputeHitDamage (CombatComponent.cpp): the resolver must
	 * reproduce this byte for byte for every legal input.
	 */
	static float LegacyM1019Formula(float BaseDamage, float AttackPower, float AttackCoefficient, float Defense)
	{
		const float RawDamage = (BaseDamage + AttackPower * AttackCoefficient)
			* 100.0f / (100.0f + FMath::Max(0.0f, Defense));
		return FMath::Max(1.0f, FMath::RoundToFloat(RawDamage));
	}

	/** Field-by-field equality of two outcomes (used for determinism and poise invariance). */
	static bool OutcomesIdentical(const FDamageOutcome& A, const FDamageOutcome& B)
	{
		return A.FinalDamage == B.FinalDamage
			&& A.bWasImmune == B.bWasImmune
			&& A.bWasBlocked == B.bWasBlocked
			&& A.bControlAccepted == B.bControlAccepted
			&& A.bStaggerAccepted == B.bStaggerAccepted
			&& A.bLaunchAccepted == B.bLaunchAccepted
			&& A.bControlFacesPoiseThreshold == B.bControlFacesPoiseThreshold
			&& A.BlockReason == B.BlockReason;
	}
}

using namespace UE::UEMMO::Tasks::M5_011;

// ---------------------------------------------------------------------------
// LegacyFourAttacksReproduced
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_011LegacyFourAttacksReproduced,
	"UEMMO.Tasks.M5_011.LegacyFourAttacksReproduced",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_011LegacyFourAttacksReproduced::RunTest(const FString& Parameters)
{
	// At AttackPower=0 and Defense=0 the resolver reproduces the legacy M1-019
	// results exactly: 10/14/18/12, no immunity, no refusal.
	const TArray<FDamageProfile> Profiles = MakeLegacyFour();
	const float LegacyDamages[] = {10.0f, 14.0f, 18.0f, 12.0f};
	const FTargetReaction Target = MakeNormalTarget();

	for (int32 Index = 0; Index < Profiles.Num(); ++Index)
	{
		const FDamageOutcome Outcome = ResolveDamage(Profiles[Index], Target, 0.0f, 0.0f);
		const FString Label = FString::Printf(TEXT("%s at AttackPower=0/Defense=0 deals its legacy damage"),
			*Profiles[Index].DamageProfileId.ToString());
		TestEqual(Label, Outcome.FinalDamage, LegacyDamages[Index]);
		TestFalse(FString::Printf(TEXT("%s is not immune"), *Profiles[Index].DamageProfileId.ToString()), Outcome.bWasImmune);
		TestFalse(FString::Printf(TEXT("%s is not blocked"), *Profiles[Index].DamageProfileId.ToString()), Outcome.bWasBlocked);
		TestTrue(FString::Printf(TEXT("%s carries no block reason"), *Profiles[Index].DamageProfileId.ToString()),
			Outcome.BlockReason == EDamageBlockReason::None);
	}

	// The parameter-default forms agree with the explicit four-argument call:
	// ResolveDamage(Attack, Target) and ResolveDamage(Attack, Target, Power)
	// are the same computation with AttackPower and Defense defaulting to 0.
	const FDamageOutcome TwoArg = ResolveDamage(Profiles[0], Target);
	const FDamageOutcome ThreeArg = ResolveDamage(Profiles[0], Target, 0.0f);
	const FDamageOutcome FourArg = ResolveDamage(Profiles[0], Target, 0.0f, 0.0f);
	TestEqual(TEXT("the two-argument default call matches the explicit call"), TwoArg.FinalDamage, FourArg.FinalDamage);
	TestEqual(TEXT("the three-argument default call matches the explicit call"), ThreeArg.FinalDamage, FourArg.FinalDamage);
	TestTrue(TEXT("the default forms agree on every flag"), OutcomesIdentical(TwoArg, FourArg) && OutcomesIdentical(ThreeArg, FourArg));

	// A control-immune target still takes full damage: the two immunity axes
	// are separate (M5-003 contract, resolution side).
	FTargetReaction ControlImmune = MakeNormalTarget();
	ControlImmune.bImmuneControl = true;
	const FDamageOutcome DamageOnControlImmune = ResolveDamage(Profiles[0], ControlImmune, 0.0f, 0.0f);
	TestEqual(TEXT("a control-immune target still takes full legacy damage"), DamageOnControlImmune.FinalDamage, 10.0f);
	TestFalse(TEXT("control immunity is not damage immunity"), DamageOnControlImmune.bWasImmune);
	return true;
}

// ---------------------------------------------------------------------------
// LegacyFormulaAttackPowerDefenseGrid
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_011LegacyFormulaAttackPowerDefenseGrid,
	"UEMMO.Tasks.M5_011.LegacyFormulaAttackPowerDefenseGrid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_011LegacyFormulaAttackPowerDefenseGrid::RunTest(const FString& Parameters)
{
	// Differential check: for a grid of attack powers and defenses the
	// resolver must equal the copied M1-019 formula exactly (both sides run
	// the same float operations, so the comparison is bitwise).
	const TArray<FDamageProfile> Profiles = MakeLegacyFour();
	const float AttackPowers[] = {0.0f, 1.0f, 5.0f, 37.0f, 120.0f, 9999.0f};
	const float Defenses[] = {0.0f, 7.0f, 25.0f, 99.0f, 250.0f, 1000.0f, 10000.0f};
	const FTargetReaction Target = MakeNormalTarget();

	for (const FDamageProfile& Profile : Profiles)
	{
		for (const float AttackPower : AttackPowers)
		{
			for (const float Defense : Defenses)
			{
				const FDamageOutcome Outcome = ResolveDamage(Profile, Target, AttackPower, Defense);
				const float Expected = LegacyM1019Formula(Profile.BaseDamage, AttackPower, Profile.AttackCoefficient, Defense);
				const FString Label = FString::Printf(TEXT("%s AP=%.0f D=%.0f matches the legacy formula"),
					*Profile.DamageProfileId.ToString(), AttackPower, Defense);
				TestEqual(Label, Outcome.FinalDamage, Expected);
				TestFalse(FString::Printf(TEXT("%s AP=%.0f D=%.0f resolves without refusal"),
					*Profile.DamageProfileId.ToString(), AttackPower, Defense), Outcome.bWasBlocked);
			}
		}
	}

	// Reviewable spot checks from the acceptance card ("Attack+5 and Defense
	// results are checkable"): (10+5)*100/100=15, (10+0)*100/125=8,
	// (10+5)*100/150=10 and the +5 values of the other three attacks.
	const FDamageProfile Light01 = Profiles[0];
	TestEqual(TEXT("light_01 with AttackPower 5 deals 15"), ResolveDamage(Light01, Target, 5.0f, 0.0f).FinalDamage, 15.0f);
	TestEqual(TEXT("light_01 against Defense 25 deals 8"), ResolveDamage(Light01, Target, 0.0f, 25.0f).FinalDamage, 8.0f);
	TestEqual(TEXT("light_01 with AttackPower 5 against Defense 50 deals 10"), ResolveDamage(Light01, Target, 5.0f, 50.0f).FinalDamage, 10.0f);
	TestEqual(TEXT("light_02 with AttackPower 5 deals 19"), ResolveDamage(Profiles[1], Target, 5.0f, 0.0f).FinalDamage, 19.0f);
	TestEqual(TEXT("launcher with AttackPower 5 deals 23"), ResolveDamage(Profiles[2], Target, 5.0f, 0.0f).FinalDamage, 23.0f);
	TestEqual(TEXT("aerial_01 with AttackPower 5 deals 17"), ResolveDamage(Profiles[3], Target, 5.0f, 0.0f).FinalDamage, 17.0f);

	// Rounding order is round-then-floor-max: (10+0)*100/300 = 2.5 rounds
	// half away from zero to 3 (the legacy RoundToFloat behavior).
	TestEqual(TEXT("a 2.5 raw result rounds to 3"), ResolveDamage(Light01, Target, 0.0f, 300.0f).FinalDamage, 3.0f);

	// The legacy max(1, ...) floor survives: a huge defense drives the raw
	// result below 1 and the resolver keeps exactly 1 (for a non-immune
	// target), never 0 and never negative.
	const float Floored = ResolveDamage(Light01, Target, 0.0f, 10000.0f).FinalDamage;
	TestEqual(TEXT("a defense of 10000 still floors light_01 damage at 1"), Floored, 1.0f);
	return true;
}

// ---------------------------------------------------------------------------
// ImmuneDamageIsExactlyZero
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_011ImmuneDamageIsExactlyZero,
	"UEMMO.Tasks.M5_011.ImmuneDamageIsExactlyZero",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_011ImmuneDamageIsExactlyZero::RunTest(const FString& Parameters)
{
	// immune_damage means damage exactly 0: the hit connects but the legacy
	// min-1 floor must NOT manufacture fake damage for an immune target.
	FTargetReaction Immune = MakeNormalTarget();
	Immune.bImmuneDamage = true;
	const FDamageProfile Light01 = MakeLegacyProfile(TEXT("light_01"), 10.0f);

	const FDamageOutcome Baseline = ResolveDamage(Light01, Immune, 0.0f, 0.0f);
	TestEqual(TEXT("an immune target takes exactly 0"), Baseline.FinalDamage, 0.0f);
	TestTrue(TEXT("the outcome reports the immunity"), Baseline.bWasImmune);
	TestFalse(TEXT("immunity is not a refusal of the request"), Baseline.bWasBlocked);
	TestTrue(TEXT("immunity carries no block reason"), Baseline.BlockReason == EDamageBlockReason::None);

	// A colossal attack power still resolves to 0: the immunity short-circuit
	// happens before the formula and its floor.
	const FDamageOutcome Colossal = ResolveDamage(Light01, Immune, 999999.0f, 0.0f);
	TestEqual(TEXT("an immune target takes 0 even against attack power 999999"), Colossal.FinalDamage, 0.0f);
	TestTrue(TEXT("the colossal attack is still reported as immune"), Colossal.bWasImmune);

	// The immunity short-circuit also precedes overflow: a base damage whose
	// raw result would overflow to infinity still resolves to exactly 0.
	FDamageProfile Huge = MakeLegacyProfile(TEXT("huge_probe"), 3.0e38f);
	const FDamageOutcome HugeImmune = ResolveDamage(Huge, Immune, 0.0f, 0.0f);
	TestEqual(TEXT("an immune target takes 0 even for an overflowing hit"), HugeImmune.FinalDamage, 0.0f);
	TestTrue(TEXT("the overflowing hit on an immune target is reported as immune"), HugeImmune.bWasImmune);

	// Separation of the axes: damage immunity does not refuse control. The
	// same hit against a stagger/launch-accepting immune target keeps its
	// control acceptance flags.
	FDamageProfile Launcher = MakeLegacyProfile(TEXT("launcher"), 18.0f);
	Launcher.HitStunSeconds = 1.0f;
	Launcher.KnockbackCmPerSecond = 70.0f;
	Launcher.LaunchCmPerSecond = 700.0f;
	const FDamageOutcome ControlOnImmune = ResolveDamage(Launcher, Immune, 0.0f, 0.0f);
	TestTrue(TEXT("a damage-immune target still accepts the stagger request"), ControlOnImmune.bStaggerAccepted);
	TestTrue(TEXT("a damage-immune target still accepts the launch request"), ControlOnImmune.bLaunchAccepted);
	TestTrue(TEXT("a damage-immune target still accepts control overall"), ControlOnImmune.bControlAccepted);

	// And in the other direction: a control-immune target keeps taking full
	// damage (also pinned in LegacyFourAttacksReproduced, repeated here next
	// to the immunity cases for contrast).
	FTargetReaction ControlImmune = MakeNormalTarget();
	ControlImmune.bImmuneControl = true;
	const FDamageOutcome DamageOnControlImmune = ResolveDamage(Light01, ControlImmune, 5.0f, 0.0f);
	TestEqual(TEXT("a control-immune target takes full damage"), DamageOnControlImmune.FinalDamage, 15.0f);
	TestFalse(TEXT("control immunity is not damage immunity"), DamageOnControlImmune.bWasImmune);
	return true;
}

// ---------------------------------------------------------------------------
// IllegalInputsAreRefused
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_011IllegalInputsAreRefused,
	"UEMMO.Tasks.M5_011.IllegalInputsAreRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_011IllegalInputsAreRefused::RunTest(const FString& Parameters)
{
	const FTargetReaction Target = MakeNormalTarget();
	const float NaN = std::numeric_limits<float>::quiet_NaN();
	const float Inf = std::numeric_limits<float>::infinity();

	// A refused request resolves to a zeroed outcome with an explicit reason:
	// no damage, no immunity, no control acceptance (rejections must carry a
	// reason, interface contract section 0.3).
	auto ExpectRefused = [this](const TCHAR* What, const FDamageOutcome& Outcome, EDamageBlockReason Reason)
	{
		const FString Label = FString::Printf(TEXT("%s is refused"), What);
		TestTrue(Label, Outcome.bWasBlocked);
		TestTrue(FString::Printf(TEXT("%s carries the expected reason"), What), Outcome.BlockReason == Reason);
		TestEqual(FString::Printf(TEXT("%s yields zero damage"), What), Outcome.FinalDamage, 0.0f);
		TestFalse(FString::Printf(TEXT("%s is not an immunity"), What), Outcome.bWasImmune);
		TestFalse(FString::Printf(TEXT("%s accepts no control"), What), Outcome.bControlAccepted);
	};

	// Non-finite attack power and defense are refused.
	FDamageProfile Light01 = MakeLegacyProfile(TEXT("light_01"), 10.0f);
	ExpectRefused(TEXT("a NaN AttackPower"), ResolveDamage(Light01, Target, NaN, 0.0f), EDamageBlockReason::NonFiniteInput);
	ExpectRefused(TEXT("an infinite AttackPower"), ResolveDamage(Light01, Target, Inf, 0.0f), EDamageBlockReason::NonFiniteInput);
	ExpectRefused(TEXT("a NaN Defense"), ResolveDamage(Light01, Target, 0.0f, NaN), EDamageBlockReason::NonFiniteInput);
	ExpectRefused(TEXT("an infinite Defense"), ResolveDamage(Light01, Target, 0.0f, Inf), EDamageBlockReason::NonFiniteInput);

	// Non-finite or negative profile numbers are refused too: the resolver
	// defends in depth even though ValidateDamageProfile gates the tables.
	FDamageProfile NanBase = Light01;
	NanBase.BaseDamage = NaN;
	ExpectRefused(TEXT("a NaN BaseDamage"), ResolveDamage(NanBase, Target, 0.0f, 0.0f), EDamageBlockReason::NonFiniteInput);

	FDamageProfile InfiniteBase = Light01;
	InfiniteBase.BaseDamage = Inf;
	ExpectRefused(TEXT("an infinite BaseDamage"), ResolveDamage(InfiniteBase, Target, 0.0f, 0.0f), EDamageBlockReason::NonFiniteInput);

	FDamageProfile NanCoefficient = Light01;
	NanCoefficient.AttackCoefficient = NaN;
	ExpectRefused(TEXT("a NaN AttackCoefficient"), ResolveDamage(NanCoefficient, Target, 5.0f, 0.0f), EDamageBlockReason::NonFiniteInput);

	FDamageProfile InfiniteCoefficient = Light01;
	InfiniteCoefficient.AttackCoefficient = Inf;
	ExpectRefused(TEXT("an infinite AttackCoefficient"), ResolveDamage(InfiniteCoefficient, Target, 5.0f, 0.0f), EDamageBlockReason::NonFiniteInput);

	FDamageProfile NegativeCoefficient = Light01;
	NegativeCoefficient.AttackCoefficient = -0.5f;
	ExpectRefused(TEXT("a negative AttackCoefficient"), ResolveDamage(NegativeCoefficient, Target, 5.0f, 0.0f), EDamageBlockReason::NegativeInput);

	FDamageProfile NanStun = Light01;
	NanStun.HitStunSeconds = NaN;
	ExpectRefused(TEXT("a NaN HitStunSeconds"), ResolveDamage(NanStun, Target, 0.0f, 0.0f), EDamageBlockReason::NonFiniteInput);

	FDamageProfile NegativeLaunch = Light01;
	NegativeLaunch.LaunchCmPerSecond = -700.0f;
	ExpectRefused(TEXT("a negative LaunchCmPerSecond"), ResolveDamage(NegativeLaunch, Target, 0.0f, 0.0f), EDamageBlockReason::NegativeInput);

	FDamageProfile NegativeKnockback = Light01;
	NegativeKnockback.KnockbackCmPerSecond = -90.0f;
	ExpectRefused(TEXT("a negative KnockbackCmPerSecond"), ResolveDamage(NegativeKnockback, Target, 0.0f, 0.0f), EDamageBlockReason::NegativeInput);

	// Negative magnitudes and a non-positive base damage are refused with
	// their own reasons.
	ExpectRefused(TEXT("a negative AttackPower"), ResolveDamage(Light01, Target, -1.0f, 0.0f), EDamageBlockReason::NegativeInput);
	ExpectRefused(TEXT("a negative Defense"), ResolveDamage(Light01, Target, 0.0f, -1.0f), EDamageBlockReason::NegativeInput);

	FDamageProfile ZeroBase = Light01;
	ZeroBase.BaseDamage = 0.0f;
	ExpectRefused(TEXT("a zero BaseDamage"), ResolveDamage(ZeroBase, Target, 0.0f, 0.0f), EDamageBlockReason::NonPositiveBaseDamage);

	FDamageProfile NegativeBase = Light01;
	NegativeBase.BaseDamage = -5.0f;
	ExpectRefused(TEXT("a negative BaseDamage"), ResolveDamage(NegativeBase, Target, 0.0f, 0.0f), EDamageBlockReason::NonPositiveBaseDamage);

	// Overflow is refused, not clamped: a raw result that overflows to
	// infinity (3e38 * 100) resolves to a refusal, never to a huge damage.
	FDamageProfile Overflowing = MakeLegacyProfile(TEXT("overflow_probe"), 3.0e38f);
	ExpectRefused(TEXT("an overflowing raw result"), ResolveDamage(Overflowing, Target, 0.0f, 0.0f), EDamageBlockReason::OverflowedResult);

	// Overflowing through the attack power: (3e38 finite + huge product)
	// overflows inside the formula and is refused as well.
	FDamageProfile Base3e37 = MakeLegacyProfile(TEXT("overflow_power_probe"), 3.0e37f);
	ExpectRefused(TEXT("an attack-power overflow"), ResolveDamage(Base3e37, Target, 1.0e37f, 0.0f), EDamageBlockReason::OverflowedResult);

	// A refusal short-circuits everything, even immunity evaluation: an
	// illegal context against an immune target is a refusal, not an immunity.
	FTargetReaction Immune = MakeNormalTarget();
	Immune.bImmuneDamage = true;
	const FDamageOutcome BlockedImmune = ResolveDamage(Light01, Immune, NaN, 0.0f);
	TestTrue(TEXT("an illegal context against an immune target is still a refusal"), BlockedImmune.bWasBlocked);
	TestFalse(TEXT("a refused context never reports immunity"), BlockedImmune.bWasImmune);
	TestEqual(TEXT("a refused context deals zero damage"), BlockedImmune.FinalDamage, 0.0f);
	return true;
}

// ---------------------------------------------------------------------------
// PoiseNeverTouchesDamage
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_011PoiseNeverTouchesDamage,
	"UEMMO.Tasks.M5_011.PoiseNeverTouchesDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_011PoiseNeverTouchesDamage::RunTest(const FString& Parameters)
{
	// Poise is a control-side threshold: it must never influence the damage
	// number or any damage flag. Three targets identical except PoiseMax.
	FTargetReaction NoPoise = MakeNormalTarget();
	FTargetReaction Poised = MakeNormalTarget();
	Poised.PoiseMax = 120.0f;
	Poised.PoiseRegenSeconds = 2.0f;
	FTargetReaction MassivePoise = MakeNormalTarget();
	MassivePoise.PoiseMax = 1.0e9f;

	const TArray<FDamageProfile> Profiles = MakeLegacyFour();
	const float AttackPowers[] = {0.0f, 7.0f, 240.0f};
	const float Defenses[] = {0.0f, 40.0f, 333.0f};

	for (const FDamageProfile& Profile : Profiles)
	{
		for (const float AttackPower : AttackPowers)
		{
			for (const float Defense : Defenses)
			{
				const FDamageOutcome NoPoiseOutcome = ResolveDamage(Profile, NoPoise, AttackPower, Defense);
				const FDamageOutcome PoisedOutcome = ResolveDamage(Profile, Poised, AttackPower, Defense);
				const FDamageOutcome MassiveOutcome = ResolveDamage(Profile, MassivePoise, AttackPower, Defense);
				const FString Label = FString::Printf(TEXT("%s AP=%.0f D=%.0f damage ignores PoiseMax"),
					*Profile.DamageProfileId.ToString(), AttackPower, Defense);
				TestEqual(Label, PoisedOutcome.FinalDamage, NoPoiseOutcome.FinalDamage);
				TestEqual(FString::Printf(TEXT("%s AP=%.0f D=%.0f damage ignores even a massive PoiseMax"),
					*Profile.DamageProfileId.ToString(), AttackPower, Defense),
					MassiveOutcome.FinalDamage, NoPoiseOutcome.FinalDamage);
				// bControlFacesPoiseThreshold is the one field ALLOWED to
				// differ here (it reports the threshold itself); every other
				// flag must ignore PoiseMax.
				TestEqual(FString::Printf(TEXT("%s AP=%.0f D=%.0f immunity flag ignores PoiseMax"),
					*Profile.DamageProfileId.ToString(), AttackPower, Defense),
					static_cast<bool>(PoisedOutcome.bWasImmune), static_cast<bool>(NoPoiseOutcome.bWasImmune));
				TestEqual(FString::Printf(TEXT("%s AP=%.0f D=%.0f refusal flag ignores PoiseMax"),
					*Profile.DamageProfileId.ToString(), AttackPower, Defense),
					static_cast<bool>(PoisedOutcome.bWasBlocked), static_cast<bool>(NoPoiseOutcome.bWasBlocked));
				TestEqual(FString::Printf(TEXT("%s AP=%.0f D=%.0f control summary ignores PoiseMax"),
					*Profile.DamageProfileId.ToString(), AttackPower, Defense),
					static_cast<bool>(PoisedOutcome.bControlAccepted), static_cast<bool>(NoPoiseOutcome.bControlAccepted));
				TestEqual(FString::Printf(TEXT("%s AP=%.0f D=%.0f block reason ignores PoiseMax"),
					*Profile.DamageProfileId.ToString(), AttackPower, Defense),
					PoisedOutcome.BlockReason, NoPoiseOutcome.BlockReason);
			}
		}
	}

	// The one place poise shows up is the threshold fact: the outcome reports
	// whether the target's control faces a poise threshold at all.
	const FDamageProfile Light01 = Profiles[0];
	TestFalse(TEXT("a poise-free target reports no poise threshold"),
		ResolveDamage(Light01, NoPoise, 0.0f, 0.0f).bControlFacesPoiseThreshold);
	TestTrue(TEXT("a poise-carrying target reports the poise threshold"),
		ResolveDamage(Light01, Poised, 0.0f, 0.0f).bControlFacesPoiseThreshold);
	TestTrue(TEXT("a massive poise pool reports the poise threshold"),
		ResolveDamage(Light01, MassivePoise, 0.0f, 0.0f).bControlFacesPoiseThreshold);

	// Gate-level control acceptance is unchanged by poise: the resolver only
	// exposes the stateless gate summary; depleting the pool is the caller's
	// stateful job (unified entry, M5-012).
	FDamageProfile Launcher = MakeLegacyProfile(TEXT("launcher"), 18.0f);
	Launcher.HitStunSeconds = 1.0f;
	Launcher.LaunchCmPerSecond = 700.0f;
	const FDamageOutcome NoPoiseControl = ResolveDamage(Launcher, NoPoise, 0.0f, 0.0f);
	const FDamageOutcome PoisedControl = ResolveDamage(Launcher, Poised, 0.0f, 0.0f);
	TestEqual(TEXT("poise does not change the stagger gate summary"),
		static_cast<bool>(PoisedControl.bStaggerAccepted), static_cast<bool>(NoPoiseControl.bStaggerAccepted));
	TestEqual(TEXT("poise does not change the launch gate summary"),
		static_cast<bool>(PoisedControl.bLaunchAccepted), static_cast<bool>(NoPoiseControl.bLaunchAccepted));
	TestEqual(TEXT("poise does not change the overall control summary"),
		static_cast<bool>(PoisedControl.bControlAccepted), static_cast<bool>(NoPoiseControl.bControlAccepted));

	// A non-finite PoiseMax is a consumed field and refuses the resolution.
	const float NaN = std::numeric_limits<float>::quiet_NaN();
	FTargetReaction NanPoise = MakeNormalTarget();
	NanPoise.PoiseMax = NaN;
	const FDamageOutcome NanPoiseOutcome = ResolveDamage(Light01, NanPoise, 0.0f, 0.0f);
	TestTrue(TEXT("a NaN PoiseMax is refused"), NanPoiseOutcome.bWasBlocked);
	TestTrue(TEXT("the NaN PoiseMax refusal names the non-finite input"),
		NanPoiseOutcome.BlockReason == EDamageBlockReason::NonFiniteInput);
	return true;
}

// ---------------------------------------------------------------------------
// ControlAcceptanceFollowsTargetGates
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_011ControlAcceptanceFollowsTargetGates,
	"UEMMO.Tasks.M5_011.ControlAcceptanceFollowsTargetGates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_011ControlAcceptanceFollowsTargetGates::RunTest(const FString& Parameters)
{
	// The launcher hit requests both stagger (1.0 s) and launch (700 cm/s);
	// the default target accepts both.
	FDamageProfile Launcher = MakeLegacyProfile(TEXT("launcher"), 18.0f);
	Launcher.HitStunSeconds = 1.0f;
	Launcher.KnockbackCmPerSecond = 70.0f;
	Launcher.LaunchCmPerSecond = 700.0f;
	const FTargetReaction Target = MakeNormalTarget();

	const FDamageOutcome Accepted = ResolveDamage(Launcher, Target, 0.0f, 0.0f);
	TestTrue(TEXT("the default target accepts the launcher stagger"), Accepted.bStaggerAccepted);
	TestTrue(TEXT("the default target accepts the launcher launch"), Accepted.bLaunchAccepted);
	TestTrue(TEXT("the default target accepts the launcher control overall"), Accepted.bControlAccepted);
	TestEqual(TEXT("control acceptance does not change the legacy damage"), Accepted.FinalDamage, 18.0f);

	// A launch-gated target (the heavy policy shape) refuses the launch but
	// keeps the stagger - and the damage is unchanged: refusing control is
	// never refusing damage.
	FTargetReaction LaunchGated = MakeNormalTarget();
	LaunchGated.bAllowLaunch = false;
	LaunchGated.PoiseMax = 120.0f;
	const FDamageOutcome LaunchRefused = ResolveDamage(Launcher, LaunchGated, 0.0f, 0.0f);
	TestFalse(TEXT("the launch-gated target refuses the launch request"), LaunchRefused.bLaunchAccepted);
	TestTrue(TEXT("the launch-gated target still accepts the stagger request"), LaunchRefused.bStaggerAccepted);
	TestTrue(TEXT("the launch-gated target still accepts control overall"), LaunchRefused.bControlAccepted);
	TestEqual(TEXT("refusing the launch does not change the damage"), LaunchRefused.FinalDamage, Accepted.FinalDamage);

	// A blanket control-immune target refuses every control kind but keeps
	// taking full damage.
	FTargetReaction ControlImmune = MakeNormalTarget();
	ControlImmune.bImmuneControl = true;
	const FDamageOutcome AllControlRefused = ResolveDamage(Launcher, ControlImmune, 0.0f, 0.0f);
	TestFalse(TEXT("a control-immune target refuses the stagger request"), AllControlRefused.bStaggerAccepted);
	TestFalse(TEXT("a control-immune target refuses the launch request"), AllControlRefused.bLaunchAccepted);
	TestFalse(TEXT("a control-immune target accepts no control overall"), AllControlRefused.bControlAccepted);
	TestFalse(TEXT("control immunity is not damage immunity"), AllControlRefused.bWasImmune);
	TestEqual(TEXT("a control-immune target takes full damage"), AllControlRefused.FinalDamage, 18.0f);

	// Per-kind independence: light_01 requests only stagger, so the launch
	// flag stays false (not requested) while the stagger is accepted.
	FDamageProfile Light01 = MakeLegacyProfile(TEXT("light_01"), 10.0f);
	Light01.HitStunSeconds = 0.22f;
	Light01.KnockbackCmPerSecond = 90.0f;
	const FDamageOutcome StaggerOnly = ResolveDamage(Light01, Target, 0.0f, 0.0f);
	TestTrue(TEXT("light_01 stagger is accepted"), StaggerOnly.bStaggerAccepted);
	TestFalse(TEXT("light_01 launch is not requested"), StaggerOnly.bLaunchAccepted);
	TestTrue(TEXT("light_01 control is accepted overall"), StaggerOnly.bControlAccepted);

	// A hit with no control magnitudes requests no control at all.
	FDamageProfile Plain = MakeLegacyProfile(TEXT("plain_probe"), 10.0f);
	const FDamageOutcome NoControl = ResolveDamage(Plain, Target, 0.0f, 0.0f);
	TestFalse(TEXT("a control-free hit accepts no stagger"), NoControl.bStaggerAccepted);
	TestFalse(TEXT("a control-free hit accepts no launch"), NoControl.bLaunchAccepted);
	TestFalse(TEXT("a control-free hit accepts no control overall"), NoControl.bControlAccepted);

	// Knockback is not a policy-gated control (the frozen FTargetReaction has
	// no knockback gate): a knockback-only hit reports no gated control
	// acceptance; the impulse itself stays with the applying layer.
	FDamageProfile KnockbackOnly = MakeLegacyProfile(TEXT("knockback_probe"), 10.0f);
	KnockbackOnly.KnockbackCmPerSecond = 90.0f;
	const FDamageOutcome KnockbackOutcome = ResolveDamage(KnockbackOnly, Target, 0.0f, 0.0f);
	TestFalse(TEXT("a knockback-only hit reports no stagger acceptance"), KnockbackOutcome.bStaggerAccepted);
	TestFalse(TEXT("a knockback-only hit reports no launch acceptance"), KnockbackOutcome.bLaunchAccepted);
	TestFalse(TEXT("a knockback-only hit reports no gated control acceptance"), KnockbackOutcome.bControlAccepted);
	TestEqual(TEXT("a knockback-only hit keeps its full damage"), KnockbackOutcome.FinalDamage, 10.0f);
	return true;
}

// ---------------------------------------------------------------------------
// PureDeterministicValueSemantics
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_011PureDeterministicValueSemantics,
	"UEMMO.Tasks.M5_011.PureDeterministicValueSemantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_011PureDeterministicValueSemantics::RunTest(const FString& Parameters)
{
	// The resolver is a pure function over its inputs: the same context must
	// produce bitwise-identical outcomes on every call (no global random
	// streams, no state). The resolver translation unit contains no RNG and
	// no mutable globals; this loop is the observable pin.
	FDamageProfile Light01 = MakeLegacyProfile(TEXT("light_01"), 10.0f);
	Light01.HitStunSeconds = 0.22f;
	Light01.KnockbackCmPerSecond = 90.0f;
	FTargetReaction Poised = MakeNormalTarget();
	Poised.PoiseMax = 120.0f;

	const FDamageOutcome First = ResolveDamage(Light01, Poised, 13.0f, 77.0f);
	for (int32 Repeat = 1; Repeat < 32; ++Repeat)
	{
		const FDamageOutcome Again = ResolveDamage(Light01, Poised, 13.0f, 77.0f);
		if (!OutcomesIdentical(First, Again))
		{
			AddError(FString::Printf(TEXT("repeat %d of the same context produced a different outcome"), Repeat));
			break;
		}
	}
	TestEqual(TEXT("the fixed context keeps its legacy-derived damage"), First.FinalDamage,
		LegacyM1019Formula(10.0f, 13.0f, 1.0f, 77.0f));

	// Resolving one context does not disturb the next: interleaved distinct
	// contexts stay independent (order invariance).
	const FDamageProfile Light02 = MakeLegacyProfile(TEXT("light_02"), 14.0f);
	const FDamageOutcome InterleavedA = ResolveDamage(Light01, Poised, 13.0f, 77.0f);
	const FDamageOutcome Other = ResolveDamage(Light02, Poised, 5.0f, 25.0f);
	const FDamageOutcome InterleavedB = ResolveDamage(Light01, Poised, 13.0f, 77.0f);
	TestTrue(TEXT("interleaved resolutions do not disturb each other"), OutcomesIdentical(InterleavedA, InterleavedB));
	TestEqual(TEXT("the other context keeps its own value"), Other.FinalDamage, LegacyM1019Formula(14.0f, 5.0f, 1.0f, 25.0f));

	// The default-constructed outcome is a fully zeroed refusal-free value.
	FDamageOutcome Default = FDamageOutcome();
	TestEqual(TEXT("the default outcome deals zero damage"), Default.FinalDamage, 0.0f);
	TestFalse(TEXT("the default outcome is not immune"), Default.bWasImmune);
	TestFalse(TEXT("the default outcome is not blocked"), Default.bWasBlocked);
	TestFalse(TEXT("the default outcome accepts no control"), Default.bControlAccepted);
	TestFalse(TEXT("the default outcome accepts no stagger"), Default.bStaggerAccepted);
	TestFalse(TEXT("the default outcome accepts no launch"), Default.bLaunchAccepted);
	TestFalse(TEXT("the default outcome reports no poise threshold"), Default.bControlFacesPoiseThreshold);
	TestTrue(TEXT("the default outcome carries no block reason"), Default.BlockReason == EDamageBlockReason::None);

	// The outcome is a trivially copyable value snapshot: callers can copy it
	// across systems without sharing mutable state.
	FDamageOutcome Copy = First;
	Copy.FinalDamage = 0.0f;
	TestTrue(TEXT("copying the outcome leaves the original untouched"),
		First.FinalDamage > 0.0f && !OutcomesIdentical(First, Copy));
	return true;
}

#endif
