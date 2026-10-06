#include "DamageResolver.h"

/**
 * M5-011: the pure-function damage resolver (see DamageResolver.h for the
 * full contract). No World, no Actor, no health mutation, no mutable state
 * and no random number stream: every input is read once, every output is
 * derived locally, and illegal contexts are refused with an explicit reason.
 */
FDamageOutcome ResolveDamage(const FDamageProfile& Attack, const FTargetReaction& Target,
	float AttackPower, float Defense)
{
	FDamageOutcome Outcome;

	// 1. Every consumed input must be finite. The resolver reads the damage
	//    inputs, the three control request magnitudes and the poise pool
	//    size (for the threshold fact); anything NaN or infinite refuses the
	//    whole context. Full table validation stays with the parser
	//    (ValidateDamageProfile / ValidateTargetReaction); this is the
	//    in-depth guard on the fields this function actually consumes.
	const bool bAllFinite = FMath::IsFinite(Attack.BaseDamage)
		&& FMath::IsFinite(Attack.AttackCoefficient)
		&& FMath::IsFinite(AttackPower)
		&& FMath::IsFinite(Defense)
		&& FMath::IsFinite(Attack.HitStunSeconds)
		&& FMath::IsFinite(Attack.KnockbackCmPerSecond)
		&& FMath::IsFinite(Attack.LaunchCmPerSecond)
		&& FMath::IsFinite(Target.PoiseMax);
	if (!bAllFinite)
	{
		Outcome.bWasBlocked = true;
		Outcome.BlockReason = EDamageBlockReason::NonFiniteInput;
		return Outcome;
	}

	// 2. Negative magnitudes are refused, not clamped. This subsumes the
	//    legacy max(0, Defense) clamp: Defense reaches the formula only when
	//    it is not negative, so the legacy behavior is preserved exactly for
	//    every legal input.
	const bool bAnyNegative = AttackPower < 0.0f
		|| Defense < 0.0f
		|| Attack.AttackCoefficient < 0.0f
		|| Attack.HitStunSeconds < 0.0f
		|| Attack.KnockbackCmPerSecond < 0.0f
		|| Attack.LaunchCmPerSecond < 0.0f;
	if (bAnyNegative)
	{
		Outcome.bWasBlocked = true;
		Outcome.BlockReason = EDamageBlockReason::NegativeInput;
		return Outcome;
	}

	// 3. A validated damage profile always carries a positive base damage;
	//    zero or negative refuses the context (the legacy formula would turn
	//    it into a fake min-1 damage).
	if (Attack.BaseDamage <= 0.0f)
	{
		Outcome.bWasBlocked = true;
		Outcome.BlockReason = EDamageBlockReason::NonPositiveBaseDamage;
		return Outcome;
	}

	// 4. Stateless part of the control negotiation: the per-kind policy
	//    gates plus the blanket control immunity. Poise never flips these
	//    flags - the pool is stateful and stays with the caller; the outcome
	//    only reports whether the target's control faces a poise threshold
	//    at all. Knockback carries no policy gate in the frozen contract and
	//    is not part of this summary.
	const bool bControlRefused = Target.bImmuneControl;
	Outcome.bStaggerAccepted = Attack.HitStunSeconds > 0.0f && !bControlRefused && Target.bAllowStagger;
	Outcome.bLaunchAccepted = Attack.LaunchCmPerSecond > 0.0f && !bControlRefused && Target.bAllowLaunch;
	Outcome.bControlAccepted = Outcome.bStaggerAccepted || Outcome.bLaunchAccepted;
	Outcome.bControlFacesPoiseThreshold = Target.PoiseMax > 0.0f;

	// 5. Damage immunity: the hit connects but deals exactly 0 - the legacy
	//    max(1, ...) floor must never manufacture fake damage here. The
	//    control summary above still applies: damage immunity and control
	//    acceptance are separate axes.
	if (Target.bImmuneDamage)
	{
		Outcome.bWasImmune = true;
		Outcome.FinalDamage = 0.0f;
		return Outcome;
	}

	// 6. The M1-019 legacy formula (M1_019_ComputeHitDamage), reproduced with
	//    the exact same order of float operations: raw product/quotient,
	//    then FMath::RoundToFloat (half away from zero for the positive raw
	//    results this formula produces), then the max(1, ...) floor. With
	//    AttackPower=0 and Defense=0 the four legacy melee attacks resolve to
	//    10/14/18/12.
	const float RawDamage = (Attack.BaseDamage + AttackPower * Attack.AttackCoefficient)
		* 100.0f / (100.0f + FMath::Max(0.0f, Defense));
	if (!FMath::IsFinite(RawDamage))
	{
		// Overflow is refused, not clamped: an unrepresentable damage number
		// is an illegal context, and 0 damage applies.
		Outcome.bWasBlocked = true;
		Outcome.bStaggerAccepted = false;
		Outcome.bLaunchAccepted = false;
		Outcome.bControlAccepted = false;
		Outcome.bControlFacesPoiseThreshold = false;
		Outcome.BlockReason = EDamageBlockReason::OverflowedResult;
		return Outcome;
	}

	Outcome.FinalDamage = FMath::Max(1.0f, FMath::RoundToFloat(RawDamage));
	return Outcome;
}
