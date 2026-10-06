#include "ReactionResolver.h"

#include "DamageResolver.h"

/**
 * M5-014: implementation of the hit-reaction resolver. See the header for
 * the frozen contract: a pure, stateless request computed from the attack
 * reaction, the target policy and the state facts - the CombatComponent
 * stays the only timing/state source and decides whether and how to execute
 * the request. No World, no Actor, no timer, no mutable state anywhere.
 */

float ResolveLaunchZScale(const FTargetReaction& TargetPolicy, int32 LaunchIndex)
{
	// A negative count has no producer; defensively read as a fresh cycle.
	const int32 SafeIndex = FMath::Max(0, LaunchIndex);
	if (SafeIndex >= TargetPolicy.MaxLaunchesPerAirCycle)
	{
		// The cycle's launch budget is spent: the launch reaction is refused
		// (0.0) while damage and dedup still apply - the M1-025 semantics,
		// now read from the target policy instead of the attacker-side
		// constants (the legacy default policy carries the same numbers).
		return 0.0f;
	}
	if (TargetPolicy.LaunchZScales.Num() < 1)
	{
		// An illegal policy shape (ValidateTargetReaction refuses it);
		// refuse the launch instead of poisoning the impulse.
		return 0.0f;
	}
	// The scale clamps to the last entry for later launches (the contract).
	const int32 ClampedIndex = FMath::Min(SafeIndex, TargetPolicy.LaunchZScales.Num() - 1);
	const float Scale = TargetPolicy.LaunchZScales[ClampedIndex];
	return (FMath::IsFinite(Scale) && Scale > 0.0f) ? Scale : 0.0f;
}

FHitReactionRequest ResolveHitReaction(const FReactionHitContext& Context)
{
	FHitReactionRequest Request;

	// 1. The state gate: death has the highest priority, then the get-up
	//    protection (a target inside its landing recovery is unhittable).
	//    The caller executes nothing on a refused request; the unified entry
	//    enforces the same gate again for every request that reaches it.
	if (Context.bTargetDead)
	{
		Request.bRefuseHit = true;
		Request.RefuseReason = EReactionRefuseReason::TargetDead;
		return Request;
	}
	if (Context.bTargetInLandingRecovery)
	{
		Request.bRefuseHit = true;
		Request.RefuseReason = EReactionRefuseReason::GetUpProtection;
		return Request;
	}

	// 2. The float-cycle launch policy: the caller decides which attacks
	//    participate (M5-014: the launcher only); the target policy grants
	//    the per-launch scale. The admitted magnitude feeds both the unified
	//    request and the impulse; a refused launch admits 0 while the rest
	//    of the hit (damage, stun, dedup) still stands. M5-015: an expired
	//    air-control window revokes the launch request up front (the extra
	//    air control drops; damage, dedup and the remaining controls stand).
	FDamageProfile Attack = Context.Attack;
	if (Context.bTargetAirControlExpired)
	{
		Attack.LaunchCmPerSecond = 0.0f;
	}
	if (Context.bPerCycleLaunchScaling && Attack.LaunchCmPerSecond > 0.0f)
	{
		const float Scale = ResolveLaunchZScale(Context.TargetPolicy, Context.LaunchesUsedThisCycle);
		Attack.LaunchCmPerSecond = (Scale > 0.0f) ? Attack.LaunchCmPerSecond * Scale : 0.0f;
		Request.bLaunchAdmitted = Scale > 0.0f;
	}
	Request.LaunchCmPerSecond = Attack.LaunchCmPerSecond;

	// 3. The damage and control-gate summary through the M5-011 pure
	//    resolver - the exact computation the unified entry applies, so the
	//    request and the application can never disagree.
	const FDamageOutcome Resolution = ResolveDamage(Attack, Context.TargetPolicy, Context.AttackPower, Context.Defense);
	if (Resolution.bWasBlocked)
	{
		// An illegal context is not a state refusal: the request stays empty
		// (no faces, no magnitudes) and the unified entry owns the refusal
		// with its explicit reason.
		Request.StunOverride = EHitStunOverride::Max;
		return Request;
	}

	// 4. The M5-015 poise gate (pure decision over the caller-read pool
	//    value): a control-requesting hit against a pool target either holds
	//    (every control kind of the hit is refused; the damage face and the
	//    knockback stand), breaks (the control is accepted and the pressure
	//    is reported for the caller to record), or is skipped entirely by a
	//    BypassPoise penetration (which also grinds nothing). The depletion
	//    measure is the hit's resolved damage; a damage-immune hit carries
	//    no pressure (its resolved damage is exactly 0). A policy with a
	//    pool but no pool value on the context (no component home) never
	//    gates - the pool is stateful and lives on the victim component.
	float PoiseDepletion = 0.0f;
	bool bPoiseHoldsControl = false;
	const bool bControlRequested = Resolution.bStaggerAccepted || Resolution.bLaunchAccepted;
	const float PoolMax = Context.TargetPolicy.PoiseMax;
	if (bControlRequested
		&& FMath::IsFinite(PoolMax) && PoolMax > 0.0f
		&& Context.AttackReaction.ControlPenetration != EControlPenetration::BypassPoise)
	{
		PoiseDepletion = Resolution.bWasImmune ? 0.0f : Resolution.FinalDamage;
		// The break condition: the hit depletes the pool to zero or below.
		// A pool value of 0 with an active policy means an uninitialized or
		// just-broken pool - the caller-side guards settle both to full, so
		// the gate reads it as a break (the control goes through).
		bPoiseHoldsControl = Context.TargetPoiseCurrent - PoiseDepletion > 0.0f;
	}
	Request.PoiseDepletion = PoiseDepletion;

	// 5. The two independent faces (never merged into one bool): damage
	//    immunity and control acceptance are separate axes, so a
	//    super-armored target keeps its damage with every control refused
	//    and a damage-immune target keeps its control at exactly 0 damage.
	//    The poise gate refines the control face without touching the
	//    damage face.
	Request.bDamageApplies = !Resolution.bWasImmune;
	Request.bControlAccepted = (Resolution.bStaggerAccepted && !bPoiseHoldsControl)
		|| (Resolution.bLaunchAccepted && !bPoiseHoldsControl);
	Request.bStagger = Resolution.bStaggerAccepted && !bPoiseHoldsControl;
	Request.bLaunch = Resolution.bLaunchAccepted && !bPoiseHoldsControl;
	Request.bKnockdown = Request.bLaunch
		&& Context.TargetPolicy.bAllowKnockdown
		&& !Context.TargetPolicy.bImmuneControl;

	// 6. The stun request with the legacy max override: the victim's
	//    component executes max(remaining, requested), never additive.
	Request.StunOverride = EHitStunOverride::Max;
	Request.StunSeconds = Request.bStagger ? Context.Attack.HitStunSeconds : 0.0f;

	// 7. The interrupt request: exactly the hits that reach the victim's
	//    NotifyHitReceived bridge (damage applied or accepted control)
	//    interrupt the running attack; a refused hit interrupts nothing.
	Request.bInterruptAttack = Request.bDamageApplies || Request.bControlAccepted;

	// 8. The impulse request: X is the knockback mirrored by the facing (the
	//    X axis only), Y is always 0 (the depth axis is never locked - the
	//    execution keeps the target's current horizontal velocity) and Z is
	//    the admitted launch when the launch control applies. A poise-held
	//    launch reports no admitted magnitude.
	Request.LaunchCmPerSecond = bPoiseHoldsControl ? 0.0f : Request.LaunchCmPerSecond;
	Request.ImpulseVelocity = FVector(Context.Facing * Context.Attack.KnockbackCmPerSecond, 0.0f,
		Request.bLaunch ? Request.LaunchCmPerSecond : 0.0f);

	// 9. The down-state durations ride the request from the policy (the
	//    landing executor keeps the M1-026 constants until M5-016
	//    parameterizes the trigger and the durations).
	Request.KnockdownSeconds = Context.TargetPolicy.KnockdownSeconds;
	Request.RecoveringSeconds = Context.TargetPolicy.RecoveringSeconds;

	return Request;
}
