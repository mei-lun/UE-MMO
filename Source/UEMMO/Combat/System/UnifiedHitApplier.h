#pragma once

#include "CoreMinimal.h"

#include "CombatEventTypes.h"
#include "DamageResolver.h"
#include "DamageTypes.h"
#include "HitLedger.h"
#include "ReactionTypes.h"

#include <type_traits>

class AActor;

/**
 * M5-012: the unified hit application entry (M5 interface contract section 1,
 * owner 012). One game-thread submission applies one hit exactly once: it
 * validates identity and context first, commits the FHitLedger dedup key,
 * resolves the damage through the injectable calculator (the M5-011 pure
 * resolver by default), applies it through the target's HealthComponent (the
 * only owner of life - no second pool is created here) and decides the hit's
 * control from the target reaction policy. Acceptance and refusal both flow
 * through this entry once and carry explicit, traceable reasons - never a
 * bare bool.
 *
 * Contract of ApplyUnifiedHit:
 * - Validation before any commit: an unusable key, a stale/null target actor
 *   weak reference, a target-actor/registry identity mismatch, a target
 *   without a health component, a dead target, a target inside its landing
 *   recovery (get-up protection, refused as Block_Invulnerable) and a same-
 *   faction teammate are all refused BEFORE the ledger key is consumed, so a
 *   refused request never poisons a later instance.
 * - The damage resolution is pure and precedes the ledger commit: an illegal
 *   damage context (non-finite, negative, overflowing) leaves no half dedup
 *   and no half health behind.
 * - The ledger commit is the idempotency face: a duplicate event key, a
 *   stale epoch, an unknown entity, a full capacity or an unbound registry
 *   refuses with the ledger's explicit reason and applies nothing.
 * - The health application goes through UHealthComponent::ApplyDamage only.
 *   Death resistance (FTargetReaction::bDeathResistant) caps the request so
 *   at least one health point survives; the target still takes damage.
 * - The control decision is read from the target reaction: the resolver's
 *   per-kind gate summary (bAllowStagger/bAllowLaunch plus the blanket
 *   control immunity) decides the stagger and the launch, and an accepted
 *   launch carries the knockdown exactly when the knockdown gate allows it.
 *   A super-armored target (control refused) takes its full damage with no
 *   control. The shot's control on one target is accepted exactly once per
 *   (Epoch, Source, Shot, Target) through TryAcceptShotControl: another
 *   pellet of the same shot keeps its own damage but skips the consumed
 *   control.
 * - The old CombatComponent path stays the only action state machine: an
 *   accepted hit (damage or control) reaches the victim through the legacy
 *   NotifyHitReceived entry, which keeps owning death marking and the attack
 *   interrupt. The stun request is forwarded only when the policy accepted
 *   the stagger. Nothing here writes action state directly.
 * - Bridging rules (interface contract section 6, explicit): only an actual
 *   damage > 0 bridges the legacy OnHitConfirmed presentation path
 *   (BridgesLegacyHitConfirmed); a hit with zero applied damage and accepted
 *   control takes the pure-control path instead (TakesPureControlPath) and
 *   never fakes a min-1 damage. The movement impulse is not applied here -
 *   the request carries no attacker actor or facing to mirror it with - the
 *   outcome's control decision plus the request magnitudes carry it to the
 *   production adapter.
 *
 * Pure logic with injectable seams: no UWorld access anywhere; the only
 * object state touched is the request's weakly referenced target actor, its
 * HealthComponent/CombatComponent (the legacy lookups) and the injected
 * ledger. The card's "stat calculator or equivalent" is the injectable
 * IUnifiedHitDamageCalculator; the default binds the M5-011 ResolveDamage
 * with the legacy target-side defense lookup.
 */

/**
 * Why the unified entry refused the request at its own face. Append only -
 * never renumber, so log lines and tests keep their meaning. 0 always means
 * "not refused by the applier". The ledger face (EHitLedgerRejectReason) and
 * the resolver face (EDamageBlockReason) carry their own detail fields on the
 * outcome.
 */
enum class EUnifiedHitRefuseReason : uint8
{
	/** The hit was not refused at the applier face. */
	None = 0,

	/** The request does not form a usable event key (an id field is 0, or the pellet index is the 255 sentinel). */
	UnusableKey = 1,

	/** The target actor weak reference is null/stale, or it does not match the entity the key names. */
	InvalidTargetActor = 2,

	/** The target actor carries no HealthComponent, so there is no life to apply to. */
	MissingHealthComponent = 3,

	/** The target is dead: the whole hit is refused (no damage, no control, no presentation). */
	TargetDead = 4,

	/** The target is inside its landing recovery (get-up protection): the whole hit is refused. */
	GetUpProtection = 5,

	/** Attacker and target share a non-empty faction: friendly fire is filtered before application. */
	SameFaction = 6,

	/** The ledger refused the key; see FUnifiedHitOutcome::LedgerReason for the ledger's own reason. */
	LedgerRefused = 7,

	/** The damage context was refused by the resolver; see FUnifiedHitOutcome::DamageBlockReason. */
	DamageContextRefused = 8
};

/**
 * One hit submission: everything the entry needs to validate, dedup, resolve
 * and apply exactly one hit. A value snapshot plus exactly one weak target
 * reference - never a World/Actor raw pointer (interface contract 0.5).
 */
struct FUnifiedHitRequest
{
	/** The combat generation that minted this hit (a stale epoch is refused by the ledger). */
	FCombatEpoch Epoch = InvalidCombatEpoch;

	/** World-unique id of the attacking entity (010's registry). */
	FEntityId AttackerEntityId = InvalidCombatEntityId;

	/** The shot's common ActionSequence slot (010's registry allocation, never a module-local counter). */
	FShotId ShotId = InvalidCombatShotId;

	/** Pellet index inside the shot (0 for single-pellet attacks; 255 is the invalid sentinel). */
	FPelletIndex PelletIndex = 0;

	/** World-unique id of the target entity (the key's TargetId face). */
	FEntityId TargetEntityId = InvalidCombatTargetId;

	/**
	 * Weak reference to the target actor the hit applies to. A null or stale
	 * reference skips safely; when the bound registry resolves the target id
	 * to a different live actor, the request is refused as a context mismatch.
	 */
	TWeakObjectPtr<AActor> TargetActor;

	/** The attack side: per-hit numeric definition (damage and control request magnitudes). */
	FDamageProfile Attack;

	/** The target side: which controls/immunities the target policy grants. */
	FTargetReaction TargetReaction;

	/**
	 * The attacker's attack power, already resolved by the caller (the legacy
	 * formula context; 0 keeps every pre-growth result verbatim). The final
	 * damage is always computed here - never taken from the request.
	 */
	float AttackPower = 0.0f;

	/** World-space location the hit is reported at (X lateral, Y depth, Z height). */
	FVector HitLocation = FVector::ZeroVector;

	/** Builds the public dedup key of this hit (the five-tuple, M5-002). */
	FCombatEventKey MakeEventKey() const
	{
		FCombatEventKey Key;
		Key.Epoch = Epoch;
		Key.EntityId = AttackerEntityId;
		Key.ShotId = ShotId;
		Key.PelletIndex = PelletIndex;
		Key.TargetId = TargetEntityId;
		return Key;
	}
};

/**
 * The control part of one outcome: which control kinds the hit applies to
 * the target after the policy gates. All false for a super-armored target
 * that still took its damage.
 */
struct FUnifiedHitControlOutcome
{
	/** The hit's stagger (hit stun) request passed the target's stagger gate and applies. */
	bool bStagger = false;

	/** The hit's launch request passed the target's launch gate and applies. */
	bool bLaunch = false;

	/** An accepted launch lands into a knockdown exactly when the knockdown gate allows it. */
	bool bKnockdown = false;

	/** True when at least one control kind applies. */
	bool AnyAccepted() const
	{
		return bStagger || bLaunch || bKnockdown;
	}
};

/**
 * The complete result of one unified hit submission. A pointer-free,
 * trivially copyable value snapshot: the applied damage, the immunity and
 * refusal faces with their explicit reasons, the target death fact, the
 * control decision and the bridging verdicts.
 */
struct FUnifiedHitOutcome
{
	/**
	 * The coarse public verdict (M5-002): Apply when the hit was accepted
	 * (damage and/or control decided), Block_Invulnerable for a get-up
	 * protected target, Block_Ignore for every other refusal. Damage immunity
	 * is NOT a whole-hit refusal (M5-011 semantics): an immune hit still
	 * decides Apply with exactly zero damage and its control standing.
	 */
	EHitDecision Decision = EHitDecision::Block_Ignore;

	/** Health the hit actually removed (the HealthComponent return; 0 unless damage applied). */
	float DamageApplied = 0.0f;

	/** True when the target policy refused all damage: the hit connected but applied exactly 0. */
	bool bWasImmune = false;

	/** True when the submission was refused at any face and nothing applied. */
	bool bWasBlocked = false;

	/** True when this hit's damage took a living target to zero health (death has priority). */
	bool bTargetDied = false;

	/** The control decision after the target reaction gates. */
	FUnifiedHitControlOutcome Control;

	/** The resolver face's refusal reason; None unless RefuseReason == DamageContextRefused. */
	EDamageBlockReason DamageBlockReason = EDamageBlockReason::None;

	/** The ledger face's refusal reason; None unless RefuseReason == LedgerRefused. */
	EHitLedgerRejectReason LedgerReason = EHitLedgerRejectReason::None;

	/** The applier face's refusal reason; None unless bWasBlocked. */
	EUnifiedHitRefuseReason RefuseReason = EUnifiedHitRefuseReason::None;

	/** The applied verdict: not refused anywhere. */
	bool WasApplied() const
	{
		return !bWasBlocked;
	}

	/**
	 * Bridging rule (interface contract section 6): only an actual damage
	 * > 0 bridges the legacy OnHitConfirmed presentation path.
	 */
	bool BridgesLegacyHitConfirmed() const
	{
		return DamageApplied > 0.0f;
	}

	/**
	 * Bridging rule (interface contract section 6): a hit with zero applied
	 * damage and accepted control takes the pure-control confirmation path
	 * instead of faking a damage event.
	 */
	bool TakesPureControlPath() const
	{
		return !bWasBlocked && DamageApplied <= 0.0f && Control.AnyAccepted();
	}
};

/**
 * The injectable damage computation seam (the card's "stat calculator or
 * equivalent"). The entry owns every other step; only this computation is
 * replaceable, so tests and the production adapter can resolve the context
 * (attack power, defense, penetration) without touching the entry.
 */
struct IUnifiedHitDamageCalculator
{
	virtual ~IUnifiedHitDamageCalculator() = default;

	/** Resolves the damage part of the request; a refusal carries its explicit reason. */
	virtual FDamageOutcome ResolveDamage(const FUnifiedHitRequest& Request) const = 0;
};

/** The default calculator: the M5-011 pure resolver with the legacy target-side defense lookup. */
const IUnifiedHitDamageCalculator& GetDefaultUnifiedHitCalculator();

/**
 * Applies one hit through the unified entry (see the header contract above).
 * Game thread only; the ledger must outlive the call.
 */
FUnifiedHitOutcome ApplyUnifiedHit(const FUnifiedHitRequest& Request, FHitLedger& Ledger,
	const IUnifiedHitDamageCalculator& Calculator);

/** The default-calculator form of ApplyUnifiedHit. */
FUnifiedHitOutcome ApplyUnifiedHit(const FUnifiedHitRequest& Request, FHitLedger& Ledger);

// Compile-time pins: the outcome and the control part are pointer-free
// trivially copyable value snapshots; the request carries only a weak
// reference (interface contract section 0.5).
static_assert(std::is_trivially_copyable_v<FUnifiedHitControlOutcome>, "FUnifiedHitControlOutcome must stay a trivially copyable value snapshot");
static_assert(std::is_trivially_copyable_v<FUnifiedHitOutcome>, "FUnifiedHitOutcome must stay a trivially copyable value snapshot");
static_assert(std::is_default_constructible_v<FUnifiedHitOutcome>, "FUnifiedHitOutcome must stay default constructible");
static_assert(!std::is_pointer_v<decltype(FUnifiedHitRequest::TargetActor)>, "FUnifiedHitRequest::TargetActor must stay a weak reference, never a raw pointer");
static_assert(!std::is_pointer_v<decltype(FUnifiedHitRequest::Epoch)>, "FUnifiedHitRequest::Epoch must stay a value type");
static_assert(!std::is_pointer_v<decltype(FUnifiedHitRequest::TargetEntityId)>, "FUnifiedHitRequest::TargetEntityId must stay a value type");
static_assert(std::is_same_v<std::underlying_type_t<EUnifiedHitRefuseReason>, uint8>, "EUnifiedHitRefuseReason stays an append-only uint8 enum");
