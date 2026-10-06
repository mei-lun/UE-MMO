#include "UnifiedHitApplier.h"

#include "CombatEntityRegistry.h"

#include "../CombatComponent.h"
#include "../CombatHitTypes.h"
#include "../HealthComponent.h"

/**
 * M5-012: implementation of the unified hit application entry. See the
 * header for the frozen contract: validate everything first, commit the
 * ledger key, apply the damage through the one health owner, decide the
 * control from the target reaction and route the accepted hit through the
 * legacy victim path - refused submissions leave no half transaction.
 */

namespace
{
	/**
	 * The default damage computation of the unified entry: the M5-011 pure
	 * resolver over the request's attack profile, target reaction policy and
	 * attacker attack power. The target's defense is read the legacy M3-010
	 * way, from the target's combat component (0 without one), so the applied
	 * numbers reproduce M1-019 byte for byte. The seam itself is injectable -
	 * tests and the production adapter can replace the context resolution
	 * without touching this file.
	 */
	struct FDefaultUnifiedHitCalculator final : IUnifiedHitDamageCalculator
	{
		virtual FDamageOutcome ResolveDamage(const FUnifiedHitRequest& Request) const override
		{
			float Defense = 0.0f;
			const AActor* TargetActor = Request.TargetActor.Get();
			if (TargetActor != nullptr)
			{
				const UCombatComponent* VictimCombat = TargetActor->FindComponentByClass<UCombatComponent>();
				if (VictimCombat != nullptr)
				{
					Defense = VictimCombat->GetDefense();
				}
			}
			return ::ResolveDamage(Request.Attack, Request.TargetReaction, Request.AttackPower, Defense);
		}
	};
}

const IUnifiedHitDamageCalculator& GetDefaultUnifiedHitCalculator()
{
	static FDefaultUnifiedHitCalculator Default;
	return Default;
}

FUnifiedHitOutcome ApplyUnifiedHit(const FUnifiedHitRequest& Request, FHitLedger& Ledger,
	const IUnifiedHitDamageCalculator& Calculator)
{
	FUnifiedHitOutcome Outcome;

	// =======================================================================
	// Stage 1: identity and context validation. Nothing is committed (no
	// ledger key, no health, no control) before every reference the hit names
	// is verified; a refused request must leave no half transaction behind.
	// =======================================================================

	// 1a. The request must form a usable event key: every id field populated
	// and a real pellet index. A half-empty key never reaches the ledger.
	const FCombatEventKey Key = Request.MakeEventKey();
	if (!IsUsableCombatEventKey(Key) || !IsValidCombatPelletIndex(Request.PelletIndex))
	{
		Outcome.bWasBlocked = true;
		Outcome.Decision = EHitDecision::Block_Ignore;
		Outcome.RefuseReason = EUnifiedHitRefuseReason::UnusableKey;
		return Outcome;
	}

	// 1b. The target actor weak reference: a stale or null reference skips
	// safely - no crash, no record, no damage (interface contract: consumers
	// resolve actors through weak references only).
	AActor* TargetActor = Request.TargetActor.Get();
	if (TargetActor == nullptr)
	{
		Outcome.bWasBlocked = true;
		Outcome.Decision = EHitDecision::Block_Ignore;
		Outcome.RefuseReason = EUnifiedHitRefuseReason::InvalidTargetActor;
		return Outcome;
	}

	// 1c. Identity cross-check: when a registry is bound and knows the key's
	// target id, the id must resolve to exactly the actor the request carries.
	// A different live actor behind the weak reference is a context mismatch,
	// not a hit.
	const FCombatEntityRegistry* Registry = Ledger.GetBoundRegistry();
	if (Registry != nullptr && Registry->IsKnownEntity(Request.TargetEntityId))
	{
		const TWeakObjectPtr<AActor> Registered = Registry->ResolveEntity(Request.TargetEntityId);
		if (Registered.IsValid() && Registered.Get() != TargetActor)
		{
			Outcome.bWasBlocked = true;
			Outcome.Decision = EHitDecision::Block_Ignore;
			Outcome.RefuseReason = EUnifiedHitRefuseReason::InvalidTargetActor;
			return Outcome;
		}
	}

	// 1d. HealthComponent stays the only owner of life (interface contract
	// section 5): without one there is no health to apply to and no hit.
	UHealthComponent* TargetHealth = TargetActor->FindComponentByClass<UHealthComponent>();
	if (TargetHealth == nullptr)
	{
		Outcome.bWasBlocked = true;
		Outcome.Decision = EHitDecision::Block_Ignore;
		Outcome.RefuseReason = EUnifiedHitRefuseReason::MissingHealthComponent;
		return Outcome;
	}

	// 1e. Death has the highest priority (interface contract section 4): a
	// dead target refuses the whole hit - no damage, no control, no ledger
	// record, no presentation.
	if (!TargetHealth->IsAlive())
	{
		Outcome.bWasBlocked = true;
		Outcome.Decision = EHitDecision::Block_Ignore;
		Outcome.RefuseReason = EUnifiedHitRefuseReason::TargetDead;
		return Outcome;
	}

	// The victim's combat component gates the get-up protection below and is
	// the legacy victim path for the accepted hit later.
	UCombatComponent* VictimCombat = TargetActor->FindComponentByClass<UCombatComponent>();

	// 1f. Get-up protection: a target inside its landing recovery (Knockdown
	// or Recovering) is unhittable - the whole hit is refused (the legacy
	// attacker rule), expressed as Block_Invulnerable.
	if (VictimCombat != nullptr && VictimCombat->IsInLandingRecovery())
	{
		Outcome.bWasBlocked = true;
		Outcome.Decision = EHitDecision::Block_Invulnerable;
		Outcome.RefuseReason = EUnifiedHitRefuseReason::GetUpProtection;
		return Outcome;
	}

	// 1g. Friendly fire filter: a shared non-empty faction refuses the whole
	// hit before anything is recorded (the registry arrives bound on the
	// ledger; an unbound ledger is refused by the ledger itself below).
	if (Registry != nullptr)
	{
		const FCombatEntityRecord* AttackerRecord = Registry->FindEntity(Request.AttackerEntityId);
		const FCombatEntityRecord* TargetRecord = Registry->FindEntity(Request.TargetEntityId);
		if (AttackerRecord != nullptr && TargetRecord != nullptr
			&& !AttackerRecord->Metadata.Faction.IsNone()
			&& AttackerRecord->Metadata.Faction == TargetRecord->Metadata.Faction)
		{
			Outcome.bWasBlocked = true;
			Outcome.Decision = EHitDecision::Block_Ignore;
			Outcome.RefuseReason = EUnifiedHitRefuseReason::SameFaction;
			return Outcome;
		}
	}

	// =======================================================================
	// Stage 2: the damage resolution is pure and precedes the ledger commit,
	// so an illegal context leaves no half dedup and no half health behind.
	// =======================================================================
	const FDamageOutcome Resolution = Calculator.ResolveDamage(Request);
	if (Resolution.bWasBlocked)
	{
		Outcome.bWasBlocked = true;
		Outcome.Decision = EHitDecision::Block_Ignore;
		Outcome.RefuseReason = EUnifiedHitRefuseReason::DamageContextRefused;
		Outcome.DamageBlockReason = Resolution.BlockReason;
		return Outcome;
	}
	Outcome.bWasImmune = Resolution.bWasImmune;

	// =======================================================================
	// Stage 3: the ledger dedup commit. A duplicate event key, a stale epoch,
	// an unknown entity, a full capacity or an unbound registry refuses here
	// with the ledger's explicit reason; nothing has been applied yet.
	// =======================================================================
	EHitLedgerRejectReason LedgerReason = EHitLedgerRejectReason::None;
	if (!Ledger.TryRecordHitEvent(Key, LedgerReason))
	{
		Outcome.bWasBlocked = true;
		Outcome.Decision = EHitDecision::Block_Ignore;
		Outcome.RefuseReason = EUnifiedHitRefuseReason::LedgerRefused;
		Outcome.LedgerReason = LedgerReason;
		return Outcome;
	}

	// =======================================================================
	// Stage 4: the health application through the one health owner. A
	// death-resistant target still takes damage but never dies from it: the
	// request is capped so at least one health point survives.
	// =======================================================================
	float RequestedDamage = Resolution.FinalDamage;
	if (Request.TargetReaction.bDeathResistant && RequestedDamage > 0.0f)
	{
		RequestedDamage = FMath::Min(RequestedDamage, FMath::Max(0.0f, TargetHealth->GetHealth() - 1.0f));
	}
	Outcome.DamageApplied = TargetHealth->ApplyDamage(RequestedDamage);
	Outcome.bTargetDied = Outcome.DamageApplied > 0.0f && !TargetHealth->IsAlive();

	// =======================================================================
	// Stage 5: the control decision, read from the target reaction through
	// the resolver's per-kind gate summary. Damage immunity and control
	// acceptance stay separate axes; a super-armored target takes its full
	// damage with every control kind refused.
	// =======================================================================
	Outcome.Control.bStagger = Resolution.bStaggerAccepted;
	Outcome.Control.bLaunch = Resolution.bLaunchAccepted;
	Outcome.Control.bKnockdown = Outcome.Control.bLaunch
		&& Request.TargetReaction.bAllowKnockdown
		&& !Request.TargetReaction.bImmuneControl;

	// The shot's control on one target is accepted exactly once per
	// (Epoch, Source, Shot, Target): another pellet of the same shot keeps
	// its own damage event but skips the already-consumed control.
	if (Outcome.Control.AnyAccepted())
	{
		EHitLedgerRejectReason ControlReason = EHitLedgerRejectReason::None;
		if (!Ledger.TryAcceptShotControl(Key, ControlReason))
		{
			Outcome.Control = FUnifiedHitControlOutcome();
		}
	}

	// =======================================================================
	// Stage 6: the legacy victim path stays the only action state machine.
	// An accepted hit (damage or control) reaches the victim through the
	// unchanged M1-020 NotifyHitReceived entry, which keeps owning the death
	// marking and the attack interrupt; the stun request is forwarded only
	// when the target's policy accepted the stagger, so a super-armored
	// target never takes a stun through this path. The movement impulse is
	// NOT applied here: the request carries no attacker actor or facing to
	// mirror it with - the control decision plus the request magnitudes
	// carry it to the production adapter (M5-013).
	// =======================================================================
	if (VictimCombat != nullptr && (Outcome.DamageApplied > 0.0f || Outcome.Control.AnyAccepted()))
	{
		FCombatHit Hit;
		Hit.Target = TargetActor;
		Hit.InstigatorId = Request.AttackerEntityId;
		Hit.AttackInstanceId = Request.ShotId;
		Hit.HitGroupId = 0;
		Hit.Damage = Outcome.DamageApplied;
		Hit.WorldHitLocation = Request.HitLocation;
		Hit.AttackId = Request.Attack.DamageProfileId;
		Hit.HitStopSeconds = Request.Attack.HitStopSeconds;
		Hit.StunSeconds = Outcome.Control.bStagger ? Request.Attack.HitStunSeconds : 0.0f;
		VictimCombat->NotifyHitReceived(Hit);
	}

	Outcome.Decision = EHitDecision::Apply;
	return Outcome;
}

FUnifiedHitOutcome ApplyUnifiedHit(const FUnifiedHitRequest& Request, FHitLedger& Ledger)
{
	return ApplyUnifiedHit(Request, Ledger, GetDefaultUnifiedHitCalculator());
}
