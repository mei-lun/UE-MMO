#pragma once

#include "CoreMinimal.h"

#include "DamageTypes.h"

#include <type_traits>

/**
 * M5-003: attack reaction and target reaction policy value types (M5
 * interface contract section 1, first owner 003). The two sides of a hit's
 * control negotiation are expressed separately:
 * - FAttackReaction is the ATTACK side: how hard the attack pushes its
 *   control through the target's poise (control_penetration).
 * - FTargetReaction is the TARGET side: which controls the target accepts
 *   at all, its air-combo and down-state policy, and its separate damage,
 *   control, poise and death behaviors.
 *
 * The old melee defaults must be fully expressible and are the struct
 * defaults of FTargetReaction: air combo capped at 2 launches with 1.0/0.7
 * Z scales, knockdown 0.45 s plus recovering 0.25 s, no immunity of any
 * kind (see M5-001 baseline review of the hit pipeline).
 *
 * Pure value types: no .cpp, no UObject members, no World/Actor pointers
 * and no behavior. Validation is local and World-free; it never consults
 * other tables. Downstream owners (005/006 parsing, 012 unified entry,
 * 014-016 parameterization) implement strategies in their own files and
 * never edit this header.
 */

/**
 * How an attack's control interacts with the target's poise threshold
 * (task card M5-003: attack_reactions carries control_penetration and only
 * none/bypass_poise are supported). Append only - never renumber.
 */
enum class EControlPenetration : uint8
{
	/**
	 * No penetration: the hit's control faces the target's poise threshold
	 * normally (the legacy behavior of all four melee attacks).
	 */
	None = 0,

	/**
	 * Bypass the poise threshold only. This NEVER bypasses the target's
	 * explicit allow gates (FTargetReaction::bAllowStagger/bAllowLaunch/
	 * bAllowKnockdown), a blanket control immunity, or a get-up protection
	 * window; those still refuse the control part of the hit exactly as for
	 * EControlPenetration::None.
	 */
	BypassPoise = 1
};

/**
 * Parses a control_penetration token from a source table. Accepts exactly
 * "none" and "bypass_poise" (case-insensitive, matching the project's
 * enum-token parsing precedent); everything else - including the empty
 * string - is refused.
 */
inline bool ParseControlPenetration(const FString& Text, EControlPenetration& OutValue)
{
	if (Text.Equals(TEXT("none"), ESearchCase::IgnoreCase))
	{
		OutValue = EControlPenetration::None;
		return true;
	}
	if (Text.Equals(TEXT("bypass_poise"), ESearchCase::IgnoreCase))
	{
		OutValue = EControlPenetration::BypassPoise;
		return true;
	}
	return false;
}

/**
 * The attack side of the control negotiation: one row per attack reaction.
 * Deliberately thin - the numeric magnitudes (stun, knockback, launch, hit
 * stop) live on the attack's FDamageProfile, and this table carries only
 * the control-strength policy that has no numeric expression.
 */
struct FAttackReaction
{
	/** Unique business id: lowercase a-z, 0-9 and _, 1..64 characters. */
	FName ReactionId;

	/** How the attack pushes control through the target's poise threshold. */
	EControlPenetration ControlPenetration = EControlPenetration::None;
};

/** Local validation of one attack reaction (id syntax, enum range). */
inline bool ValidateAttackReaction(const FAttackReaction& Reaction, FString& OutErrors)
{
	TArray<FString> Problems;
	if (!IsValidDefinitionIdSyntax(Reaction.ReactionId.ToString()))
	{
		Problems.Add(FString::Printf(TEXT("ReactionId '%s' must be 1..64 characters of a-z, 0-9 and _"),
			*Reaction.ReactionId.ToString()));
	}
	if (Reaction.ControlPenetration != EControlPenetration::None
		&& Reaction.ControlPenetration != EControlPenetration::BypassPoise)
	{
		Problems.Add(TEXT("ControlPenetration must be None (0) or BypassPoise (1)"));
	}
	return FinishDefinitionValidation(Problems, OutErrors);
}

/**
 * The target side of the control negotiation: which controls the target
 * accepts, its air-combo and down-state timing, and its separate immunity
 * axes. The struct defaults are the legacy normal target (M5-001 baseline):
 * every control accepted, 2 launches per air cycle with 1.0/0.7 Z scales,
 * 0.45 s knockdown plus 0.25 s recovering, no poise pool and no immunity.
 *
 * Immunity axes stay separate on purpose (task acceptance): damage immunity
 * and control immunity are different flags; poise is a numeric pool that
 * blocks control, not an immunity; and death resistance is not damage
 * immunity (a death-resistant boss still takes damage).
 */
struct FTargetReaction
{
	/** Unique business id: lowercase a-z, 0-9 and _, 1..64 characters. */
	FName PolicyId;

	/** Whether the target may be staggered into hit stun at all. */
	bool bAllowStagger = true;

	/** Whether the target may be launched at all (false = launch immune). */
	bool bAllowLaunch = true;

	/** Whether the target may be knocked down at all. */
	bool bAllowKnockdown = true;

	/** Launches the target accepts per air cycle before further launch requests are refused. Not negative. */
	int32 MaxLaunchesPerAirCycle = 2;

	/**
	 * Z speed scale per launch inside one air cycle: entry i scales launch i
	 * (clamped to the last entry for later launches). At least one entry;
	 * every entry finite and greater than 0. Legacy default 1.0 then 0.7.
	 */
	TArray<float> LaunchZScales = {1.0f, 0.7f};

	/**
	 * Upper bound on one continuous airborne period in seconds; after it the
	 * target's extra air control is dropped (the target falls, it is never
	 * teleported to the ground). Finite and greater than 0 - a bounded
	 * positive number, so no sample can hang a target in the air forever.
	 */
	float MaxAirTimeSeconds = 2.5f;

	/**
	 * Poise pool size. 0 = no poise pool (the legacy normal target): poise
	 * blocks nothing. A value greater than 0 gives the target a poise
	 * threshold that control must deplete. Finite and not negative.
	 */
	float PoiseMax = 0.0f;

	/** Poise regeneration period in seconds. 0 = no regeneration. Finite and not negative. */
	float PoiseRegenSeconds = 0.0f;

	/** Knockdown duration in seconds once the target lands launched. Finite and not negative. */
	float KnockdownSeconds = 0.45f;

	/** Recovering duration in seconds after the knockdown ends. Finite and not negative. */
	float RecoveringSeconds = 0.25f;

	/** Whether the target takes no damage at all (separate from control immunity). */
	bool bImmuneDamage = false;

	/** Whether the target accepts no control at all (separate from damage immunity). */
	bool bImmuneControl = false;

	/** Whether the target refuses to die from damage (it still takes damage). */
	bool bDeathResistant = false;
};

/**
 * Local validation of one target reaction policy: id syntax, non-negative
 * counts and durations, a bounded positive max air time and at least one
 * finite positive launch scale. Collects every problem, joined with "; ".
 */
inline bool ValidateTargetReaction(const FTargetReaction& Policy, FString& OutErrors)
{
	TArray<FString> Problems;
	if (!IsValidDefinitionIdSyntax(Policy.PolicyId.ToString()))
	{
		Problems.Add(FString::Printf(TEXT("PolicyId '%s' must be 1..64 characters of a-z, 0-9 and _"),
			*Policy.PolicyId.ToString()));
	}
	if (Policy.MaxLaunchesPerAirCycle < 0)
	{
		Problems.Add(TEXT("MaxLaunchesPerAirCycle must be not negative"));
	}
	if (Policy.LaunchZScales.Num() < 1)
	{
		Problems.Add(TEXT("LaunchZScales must hold at least one launch scale"));
	}
	for (int32 Index = 0; Index < Policy.LaunchZScales.Num(); ++Index)
	{
		const float Scale = Policy.LaunchZScales[Index];
		if (!FMath::IsFinite(Scale) || Scale <= 0.0f)
		{
			Problems.Add(FString::Printf(TEXT("LaunchZScales[%d] must be finite and greater than 0"), Index));
		}
	}
	if (!FMath::IsFinite(Policy.MaxAirTimeSeconds) || Policy.MaxAirTimeSeconds <= 0.0f)
	{
		Problems.Add(TEXT("MaxAirTimeSeconds must be finite and greater than 0"));
	}
	if (!FMath::IsFinite(Policy.PoiseMax) || Policy.PoiseMax < 0.0f)
	{
		Problems.Add(TEXT("PoiseMax must be finite and not negative"));
	}
	if (!FMath::IsFinite(Policy.PoiseRegenSeconds) || Policy.PoiseRegenSeconds < 0.0f)
	{
		Problems.Add(TEXT("PoiseRegenSeconds must be finite and not negative"));
	}
	if (!FMath::IsFinite(Policy.KnockdownSeconds) || Policy.KnockdownSeconds < 0.0f)
	{
		Problems.Add(TEXT("KnockdownSeconds must be finite and not negative"));
	}
	if (!FMath::IsFinite(Policy.RecoveringSeconds) || Policy.RecoveringSeconds < 0.0f)
	{
		Problems.Add(TEXT("RecoveringSeconds must be finite and not negative"));
	}
	return FinishDefinitionValidation(Problems, OutErrors);
}

// Compile-time pins: reaction definitions are value snapshots, never
// World/Actor pointer holders (interface contract section 0.5).
static_assert(!std::is_pointer_v<decltype(FAttackReaction::ReactionId)>, "FAttackReaction::ReactionId must stay a value type");
static_assert(!std::is_pointer_v<decltype(FAttackReaction::ControlPenetration)>, "FAttackReaction::ControlPenetration must stay a value type");
static_assert(std::is_trivially_copyable_v<FAttackReaction>, "FAttackReaction must stay a trivially copyable value snapshot");

static_assert(!std::is_pointer_v<decltype(FTargetReaction::PolicyId)>, "FTargetReaction::PolicyId must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::bAllowStagger)>, "FTargetReaction::bAllowStagger must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::bAllowLaunch)>, "FTargetReaction::bAllowLaunch must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::bAllowKnockdown)>, "FTargetReaction::bAllowKnockdown must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::MaxLaunchesPerAirCycle)>, "FTargetReaction::MaxLaunchesPerAirCycle must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::LaunchZScales)>, "FTargetReaction::LaunchZScales must stay a value container");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::MaxAirTimeSeconds)>, "FTargetReaction::MaxAirTimeSeconds must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::PoiseMax)>, "FTargetReaction::PoiseMax must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::PoiseRegenSeconds)>, "FTargetReaction::PoiseRegenSeconds must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::KnockdownSeconds)>, "FTargetReaction::KnockdownSeconds must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::RecoveringSeconds)>, "FTargetReaction::RecoveringSeconds must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::bImmuneDamage)>, "FTargetReaction::bImmuneDamage must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::bImmuneControl)>, "FTargetReaction::bImmuneControl must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::bDeathResistant)>, "FTargetReaction::bDeathResistant must stay a value type");
static_assert(std::is_default_constructible_v<FTargetReaction>, "FTargetReaction must stay default constructible");
