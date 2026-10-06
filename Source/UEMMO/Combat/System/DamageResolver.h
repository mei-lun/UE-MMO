#pragma once

#include "CoreMinimal.h"

#include "DamageTypes.h"
#include "ReactionTypes.h"

#include <type_traits>

/**
 * M5-011: the pure-function damage resolver (M5 interface contract section 1,
 * owner 011). Turns one legal hit context - an attack's FDamageProfile, the
 * target's FTargetReaction policy, the attacker's attack power and the
 * target's defense - into the damage number the hit applies, plus the
 * stateless part of the control negotiation.
 *
 * Contract of ResolveDamage:
 * - Pure: no World, no Actor, no health mutation, no mutable state and no
 *   random number stream. The same inputs always produce the same outcome;
 *   applying the damage stays with the unified entry (M5-012) and the
 *   HealthComponent.
 * - Legacy compatible: for every legal input the damage equals the M1-019
 *   formula byte for byte,
 *     max(1, round((base + AttackPower * coefficient) * 100 / (100 + max(0, Defense))))
 *   with M1-019's exact order of operations (float arithmetic, then
 *   FMath::RoundToFloat, then the max(1, ...) floor). At AttackPower=0 and
 *   Defense=0 the four legacy melee attacks resolve to 10/14/18/12.
 * - Immunity is exactly zero: a damage-immune target (bImmuneDamage) takes
 *   0 damage, never the legacy min-1 floor. Damage immunity and control
 *   acceptance stay separate: an immune target keeps its gate-level control
 *   acceptance, and a control-immune target keeps taking full damage.
 * - Poise never touches damage: PoiseMax changes no damage number and no
 *   damage flag. Depleting a poise pool is stateful and belongs to the
 *   caller; the outcome only reports whether the target's control faces a
 *   poise threshold at all (bControlFacesPoiseThreshold).
 * - Illegal contexts are refused with an explicit reason (interface contract
 *   section 0.3: rejections must carry a reason, never a bare bool): every
 *   consumed input must be finite and not negative, the base damage must be
 *   positive, and a raw result that overflows to a non-finite value is
 *   refused instead of clamped. A refusal short-circuits everything - its
 *   outcome carries no immunity and no control acceptance.
 * - Rounding order and limits are explicit: round to nearest (half away
 *   from zero, the legacy RoundToFloat) then the max(1, ...) floor for
 *   non-immune targets; the only clamps are the legacy floor and the legacy
 *   max(0, Defense) defense term (a negative Defense is refused outright
 *   here, which subsumes that clamp).
 *
 * Control summary scope: the resolver evaluates exactly the controls the
 * frozen FTargetReaction policy gates - stagger (gated by bAllowStagger,
 * requested when HitStunSeconds > 0) and launch (gated by bAllowLaunch,
 * requested when LaunchCmPerSecond > 0), each also refused by a blanket
 * bImmuneControl. Knockback carries no policy gate in the frozen contract,
 * so it is not part of this summary; its impulse stays with the applying
 * layer. Poise thresholds are stateful and stay with the caller.
 */

/**
 * Why a hit's damage part was refused. Append only - never renumber.
 * A normal resolution (applied damage or exact-zero immunity) carries None.
 */
enum class EDamageBlockReason : uint8
{
	/** The context was legal: the outcome carries a real resolution. */
	None = 0,

	/** At least one consumed input was NaN or infinite. */
	NonFiniteInput = 1,

	/** At least one consumed magnitude (power, defense, coefficient, control magnitudes) was negative. */
	NegativeInput = 2,

	/** The profile's BaseDamage was zero or negative (never a legal profile). */
	NonPositiveBaseDamage = 3,

	/** The raw formula result overflowed to a non-finite value; refused, not clamped. */
	OverflowedResult = 4
};

/**
 * The resolution of one hit's damage part. A value snapshot: trivially
 * copyable, no pointers, no World/Actor references.
 */
struct FDamageOutcome
{
	/**
	 * Damage in health points the hit applies. Exactly 0 when refused
	 * (bWasBlocked) or immune (bWasImmune); otherwise the legacy formula
	 * result including its max(1, ...) floor, so at least 1.
	 */
	float FinalDamage = 0.0f;

	/** True when the target policy refuses all damage (bImmuneDamage): the hit connects but deals exactly 0. */
	bool bWasImmune = false;

	/** True when the request itself was refused (see BlockReason): nothing was computed, nothing applies. */
	bool bWasBlocked = false;

	/** True when at least one policy-gated control kind (stagger, launch) is requested and accepted. */
	bool bControlAccepted = false;

	/** True when the hit requests stagger (HitStunSeconds > 0) and the target's stagger gate lets it through. */
	bool bStaggerAccepted = false;

	/** True when the hit requests launch (LaunchCmPerSecond > 0) and the target's launch gate lets it through. */
	bool bLaunchAccepted = false;

	/**
	 * True when the target carries a poise pool (PoiseMax > 0): the target's
	 * control faces a poise threshold. This is a fact about the target, not a
	 * decision - depleting the pool is the caller's stateful job and never
	 * changes the damage.
	 */
	bool bControlFacesPoiseThreshold = false;

	/** Refusal reason; None unless bWasBlocked. */
	EDamageBlockReason BlockReason = EDamageBlockReason::None;
};

/**
 * Resolves the damage part of one hit. AttackPower defaults to 0 and Defense
 * to 0 so the legacy unwired-combatant call (both sides at their 0/0
 * defaults) is the shortest form; Defense is an explicit parameter because
 * the legacy formula reads it from the victim's combat component, which no
 * M5-003 value type carries.
 */
FDamageOutcome ResolveDamage(const FDamageProfile& Attack, const FTargetReaction& Target,
	float AttackPower = 0.0f, float Defense = 0.0f);

// Compile-time pins: the resolution outcome is a value snapshot, never a
// World/Actor pointer holder (interface contract section 0.5).
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
