#include "CombatComponent.h"

#include "AttackCatalog.h"
#include "AttackDefinition.h"
#include "CombatGeometry.h"
#include "CombatHitQuery.h"
#include "CombatPresentationComponent.h"
#include "HealthComponent.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Enemy/TrainingEnemy.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Logging/LogMacros.h"
#include "../Logging/OperationLogSubsystem.h"
#include "System/DamageTypes.h"
#include "System/ReactionTypes.h"
#include "System/UnifiedHitApplier.h"

#include <atomic>

namespace
{
	// M3-023: resolves the operation log through the owner (owner -> world ->
	// game instance). Null (bare component, no owner, no world, no game
	// instance) skips the log silently - the component keeps its pure-logic
	// behavior verbatim in every unwired context.
	UOperationLogSubsystem* M3_023_OpLog(const UCombatComponent& Component)
	{
		return UOperationLogSubsystem::FindForContext(Component.GetOwner());
	}

	// M3-023: one State row per explicit ActionState/dead-flag write point.
	// Only the discrete transitions log (start/cancel/stun/knockdown/
	// recovering/recovery end/finish/dead); the per-frame timeline never logs.
	void M3_023_LogActionState(const UCombatComponent& Component, const FString& Detail)
	{
		UOperationLogSubsystem* OpLog = M3_023_OpLog(Component);
		if (OpLog == nullptr)
		{
			return;
		}
		const AActor* OwnerActor = Component.GetOwner();
		OpLog->LogState(FString::Printf(TEXT("AttackState: %s owner=%s"),
			*Detail, OwnerActor != nullptr ? *OwnerActor->GetName() : TEXT("none")));
	}

	// Session-wide monotonic source of stable attacker ids (interface contract
	// sections 4 and 5): minted once per component, never a raw pointer value,
	// never reused within the session. Starts at 1 so 0 stays "no id".
	std::atomic<uint64> M1_019_NextInstigatorId{1};

	// M1-021 component-level constants: the fixed mapping from a buffered
	// input type to the attack id it drives. The Free state starts the base
	// attack of each ground chain (Light -> light_01, Launcher -> launcher);
	// a cancel window chains into the per-type follow-up (Light -> light_02,
	// Launcher -> launcher). Whether a mapped follow-up is legal is always
	// decided by the running attack's AllowedNextAttacks, never here.
	const FName M1_021_FreeLightAttackId(TEXT("light_01"));
	const FName M1_021_FreeLauncherAttackId(TEXT("launcher"));
	const FName M1_021_ChainLightAttackId(TEXT("light_02"));
	const FName M1_021_ChainLauncherAttackId(TEXT("launcher"));

	// M1-023: the only attack whose cancel window allows the jump cancel.
	// The cancel window itself is always read from the running attack's
	// definition (launcher: [18,32)); this constant pins the source attack
	// because the data schema carries no per-attack jump flag yet (the JSON
	// cancel_window_allows field is not surfaced into UAttackDefinition) and
	// the jump allowance is input semantics, not an AllowedNextAttacks entry.
	const FName M1_023_JumpCancelAttackId(TEXT("launcher"));

	// M1-024: the attack a buffered Light starts while the owner is airborne
	// (the interface contract section 3 row: aerial_01, launch 60 cm/s). The
	// grounded mapping stays M1_021_FreeLightAttackId (light_01).
	const FName M1_024_AerialLightAttackId(TEXT("aerial_01"));

	// M1-025/M5-014: the attack whose hits run the per-float-cycle launch
	// policy (the M5-014 ReactionResolver reads the scale from the target
	// policy). Only the launcher is capped and decayed; aerial_01's small
	// compensation stays under the separate M1-024 one-per-cycle gate.
	const FName M1_025_LauncherAttackId(TEXT("launcher"));

	// M1-025: reads the target's current float-cycle launcher count. M5-015:
	// the unified surface is the victim's combat component (its API reads
	// through to the TrainingEnemy legacy field, so those results stay
	// verbatim, and it owns the count for every other component-bearing
	// victim - wave enemies included). A componentless target falls back to
	// the legacy TrainingEnemy read; anything else carries no float cycle
	// and reads 0.
	int32 M1_025_ResolveLauncherCycleCount(const AActor* Target)
	{
		if (Target != nullptr)
		{
			if (const UCombatComponent* VictimCombat = Target->FindComponentByClass<UCombatComponent>())
			{
				return VictimCombat->GetLaunchCycleCount();
			}
			if (const ATrainingEnemy* EnemyTarget = Cast<const ATrainingEnemy>(Target))
			{
				return EnemyTarget->GetLauncherCycleCount();
			}
		}
		return 0;
	}

	// M1-026: the landing recovery durations (interface contract section 6):
	// a launched landing knocks the victim down for 0.45 s and then keeps it
	// in Recovering for 0.25 s before it is Free again - both deadlines are
	// fixed at the landing moment, so the process always totals 0.70 s
	// regardless of tick spacing. Pre-tuning-playtest initial values.
	constexpr double M1_026_KnockdownSeconds = 0.45;
	constexpr double M1_026_RecoveringSeconds = 0.25;

	// Buffered-input lifetime (interface contract section 2): an age of exactly
	// 150 ms is still valid, only a strictly greater age expires.
	constexpr double M1_021_InputLifetimeSeconds = 0.150;

	// M1-021: maps a buffered input type to the attack id a cancel-window
	// chain would start. Jump stays unconsumed (the jump cancel is M1-023).
	bool M1_021_ResolveChainAttackId(ECombatInput Action, FName& OutAttackId)
	{
		switch (Action)
		{
		case ECombatInput::Light:
			OutAttackId = M1_021_ChainLightAttackId;
			return true;
		case ECombatInput::Launcher:
			OutAttackId = M1_021_ChainLauncherAttackId;
			return true;
		default:
			return false;
		}
	}

	// M1-021: maps a buffered input type to the attack id a Free-state start
	// would begin. M1-024: the Light routes by the owner's air state -
	// aerial_01 while airborne, light_01 grounded - while the Launcher
	// mapping stays launcher in both states. Jump stays unconsumed (M1-023).
	bool M1_021_ResolveFreeStartAttackId(ECombatInput Action, bool bAirborne, FName& OutAttackId)
	{
		switch (Action)
		{
		case ECombatInput::Light:
			OutAttackId = bAirborne ? M1_024_AerialLightAttackId : M1_021_FreeLightAttackId;
			return true;
		case ECombatInput::Launcher:
			OutAttackId = M1_021_FreeLauncherAttackId;
			return true;
		default:
			return false;
		}
	}

	// M1-024: whether the target combatant stands on ground right now. This
	// matches ATrainingEnemy::GetAirState()==Grounded (the movement component
	// walks on ground, which is the same IsMovingOnGround check). A
	// non-character target cannot float, so it reads as grounded and the
	// per-float-cycle aerial gate can never block hits on it.
	bool M1_024_IsTargetGrounded(const AActor& Target)
	{
		if (const ACharacter* TargetCharacter = Cast<ACharacter>(&Target))
		{
			if (const UCharacterMovementComponent* Movement = TargetCharacter->GetCharacterMovement())
			{
				return Movement->IsMovingOnGround();
			}
		}
		return true;
	}

	// M1-021: display name of a buffered action for the switch/start logs.
	const TCHAR* M1_021_InputActionName(ECombatInput Action)
	{
		return Action == ECombatInput::Launcher ? TEXT("Launcher") : TEXT("Light");
	}

	// The design damage formula (Docs/01 section 8.2):
	// damage = max(1, round((baseDamage + AttackPower * coefficient)
	//                        * 100 / (100 + max(0, Defense))))
	// M3-010: AttackPower is the ATTACKER's CombatAttackPower (this component)
	// and Defense is the VICTIM's CombatDefense (read from the target's combat
	// component). An unwired combatant keeps the 0/0 defaults, so the
	// pre-M3-010 results stay verbatim (light_01 deducts exactly 10).
	// M5-013: the formula now lives only in the M5-011 pure resolver (the
	// unified entry's default calculator; byte-for-byte compatible and pinned
	// by M5DamageResolverTests/M5UnifiedHitTests) - the M1-019 helper and the
	// direct-to-health path it served are removed with the rewiring.

	// M5-013: derives the unified request's per-hit numeric definition from
	// the live attack definition the old pipeline already used (the catalog's
	// DA_* assets loaded through Config/DefaultGame.ini). The four legacy
	// attacks resolve to the exact old numbers (10/14/18/12 at the 0/0
	// attribute defaults). When the M5-008/009 data assets reach their runtime
	// wiring (M5-020A), the same fields can be read from
	// FCombatCatalog::FindDamageProfile without touching this call site.
	FDamageProfile M5_013_MakeDamageProfile(const UAttackDefinition& Definition)
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = Definition.AttackId;
		Profile.BaseDamage = Definition.BaseDamage;
		Profile.AttackCoefficient = Definition.AttackCoefficient;
		Profile.HitStunSeconds = Definition.HitStunSeconds;
		Profile.KnockbackCmPerSecond = Definition.KnockbackSpeed;
		Profile.LaunchCmPerSecond = Definition.LaunchSpeed;
		Profile.HitStopSeconds = Definition.HitStopSeconds;
		return Profile;
	}

	// M5-014: the M5-013 target-side adaptation (M5_013_MakeLegacyTargetReaction,
	// the hardcoded legacy normal target) moved to the resolver as
	// ReactionResolver::MakeLegacyNormalTargetReaction - the component's hit
	// policy member is initialized from it and every hit's request is now
	// resolved through the M5-014 ReactionResolver.
}

UCombatComponent::UCombatComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	CachedInstigatorId = M1_019_NextInstigatorId.fetch_add(1) + 1;
	// M5-013: the unified entry's dedup face verifies epochs and entities
	// against this component's own registry (the member address is stable for
	// the component's lifetime).
	UnifiedHitLedger.BindRegistry(&UnifiedEntityRegistry);
}

bool UCombatComponent::InitializeFromCatalog(UAttackCatalog* InCatalog)
{
	if (InCatalog == nullptr)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO UCombatComponent: InitializeFromCatalog rejected a null catalog; the previously attached catalog (if any) stays in place."));
		return false;
	}
	Catalog = InCatalog;
	LoggedMissingAttackIds.Reset();
	return true;
}

bool UCombatComponent::TryStartAttack(FName AttackId, int32 NewFacing)
{
	// Death has the highest priority (interface contract section 4).
	if (bDead)
	{
		return false;
	}
	// One action at a time: an in-flight attack rejects unconditional reentry.
	if (ActionState != ECombatActionState::Free)
	{
		return false;
	}

	const UAttackDefinition* Definition = (Catalog != nullptr) ? Catalog->Find(AttackId) : nullptr;
	if (Definition == nullptr)
	{
		// Missing definition (or no catalog attached): reject with a once-per-id
		// diagnostic so mashing the same unknown input cannot spam the log.
		bool bAlreadyLogged = false;
		LoggedMissingAttackIds.Add(AttackId, &bAlreadyLogged);
		if (!bAlreadyLogged)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("UEMMO UCombatComponent: no attack definition for '%s' in the attached catalog; start rejected."),
				*AttackId.ToString());
		}
		return false;
	}

	// Every successful start mints a fresh stable in-session instance id
	// (monotonic from 1, never a raw pointer value, never reused).
	ActiveInstanceId = NextInstanceId++;
	ActiveAttackId = AttackId;
	ActiveDurationFrames = Definition->DurationFrames;
	Facing = NewFacing;
	CurrentFrame = -1;
	Clock.Reset();
	ActionState = ECombatActionState::Attacking;
	// M3-023: the discrete state transition row (the Started event rows follow
	// from the broadcast below).
	M3_023_LogActionState(*this, FString::Printf(TEXT("Attacking (%s #%llu)"),
		*AttackId.ToString(), ActiveInstanceId));
	OnStarted.Broadcast(ActiveAttackId, ActiveInstanceId);
	return true;
}

void UCombatComponent::TickCombat(float DeltaSeconds)
{
	// M3-031: keep the carried-anchor map in step with the buffer (entries
	// consumed or pruned since the last tick lose their anchors).
	CompactCarriedAnchors();
	// M1-033: a local hit stop counts down on real game advancement. Owners
	// that inject the input clock per frame (the contract's per-frame order)
	// drive the remainder through those injections; this tick's delta only
	// feeds the countdown when no injection advanced it this frame (owners
	// without input-clock injection, today's production wiring included). The
	// frozen action-clock deltas stay dropped either way: the freeze consumes
	// the real time but the action timeline never back-fills it (interface
	// contract section 2: no catch-up after a freeze lifts).
	if (bHitStopActive)
	{
		if (!bHitStopConsumedInjectedAdvance && FMath::IsFinite(DeltaSeconds) && static_cast<double>(DeltaSeconds) > 0.0)
		{
			HitStopRemainingSeconds -= static_cast<double>(DeltaSeconds);
			// M3-031: the delta path measures the same frozen span the
			// injection path does; accumulate it for the resume compensation.
			HitStopClockJumpSeconds += static_cast<double>(DeltaSeconds);
			if (HitStopRemainingSeconds <= 0.0)
			{
				EndHitStop();
			}
		}
		bHitStopConsumedInjectedAdvance = false;
	}

	// M5-015: the bounded air-control window enforcement. An open window
	// whose max air time passed on the injected input clock forces the
	// descent: the extra air control is revoked (the window stays expired
	// until the next ground contact/landing, so the resolver refuses every
	// further launch) and the owner's upward velocity is clamped once - a
	// fall, never a teleport. The check runs before the state branches so a
	// stunned or knocked-down owner still settles its expired window.
	if (bAirControlWindowOpen && FMath::IsFinite(InputClockSeconds)
		&& InputClockSeconds >= AirControlExpiryInputClockSeconds)
	{
		bAirControlWindowOpen = false;
		bAirControlExpiredUntilLanding = true;
		if (ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner()))
		{
			if (UCharacterMovementComponent* OwnerMovement = OwnerCharacter->GetCharacterMovement())
			{
				OwnerMovement->Velocity.Z = FMath::Min(OwnerMovement->Velocity.Z, 0.0f);
			}
		}
	}

	// M1-026: the landing recovery runs on the injected input clock (the
	// M1-020 stun pattern). The owner keeps calling TickCombat; each branch
	// flips at most one state per tick, exactly like the per-frame game loop,
	// so a clock jump past both deadlines settles on the second tick.
	if (ActionState == ECombatActionState::Knockdown)
	{
		if (InputClockSeconds >= LandingKnockdownEndTimeSeconds)
		{
			ActionState = ECombatActionState::Recovering;
			// M3-023: the knockdown -> recovering transition row.
			M3_023_LogActionState(*this, FString(TEXT("Recovering")));
		}
		return;
	}
	if (ActionState == ECombatActionState::Recovering)
	{
		if (InputClockSeconds >= LandingRecoveringEndTimeSeconds)
		{
			EndLandingRecovery();
		}
		return;
	}

	// M1-020: a stun runs on the injected input clock (the same explicitly
	// injected "now" as the buffered input lifetimes). The owner keeps calling
	// TickCombat; on the first tick whose clock reached the stun end the state
	// returns to Free. The action-clock delta plays no role while stunned.
	if (ActionState == ECombatActionState::HitStun)
	{
		if (InputClockSeconds >= HitStunEndTimeSeconds)
		{
			EndHitStun();
		}
		return;
	}

	if (ActionState != ECombatActionState::Attacking)
	{
		// M1-021: Free and alive - the earliest valid buffered Light/Launcher
		// starts its mapped attack here (the input-driven start; game-side
		// TryStartAttack callers still do not exist, so the buffered intents
		// are the only start source). The started instance steps from its next
		// TickCombat, so this tick returns without advancing any timeline.
		if (ActionState == ECombatActionState::Free)
		{
			TryStartFromBuffer();
		}
		return;
	}

	const int32 Steps = Clock.Advance(DeltaSeconds, bClockFrozen);
	for (int32 Step = 0; Step < Steps; ++Step)
	{
		++CurrentFrame;
		// M1-019: the current frame's active-window hits run before the finish
		// check, so a definition whose active window includes the final frame
		// still lands its hit exactly once. The stub applies nothing yet.
		TryApplyActiveWindowHits();
		// Half-open timeline: DurationFrames - 1 is the last covered frame;
		// the step that lands on it ends the attack (26 steps for light_01).
		if (CurrentFrame >= ActiveDurationFrames - 1)
		{
			FinishCurrentAttack();
			return;
		}

		// M1-014 (M1-021 generalized it): inside the cancel window the attack
		// can switch into the earliest valid buffered Light/Launcher
		// follow-up. A successful switch retires the old instance and starts
		// the next one; this tick then stops advancing the old timeline (the
		// new instance steps from its next TickCombat, so a chain never skips
		// frames inside a single tick).
		if (TryChainFromBuffer())
		{
			return;
		}
		// M1-023: after the attack-type chain, a buffered Jump inside the
		// launcher cancel window cancels the running attack and requests the
		// owner's jump. A successful cancel retires the instance (no Finished,
		// hit set cleared), so this tick stops advancing the old timeline.
		if (TryJumpCancelFromBuffer())
		{
			return;
		}
	}
}

void UCombatComponent::ResetCombat()
{
	// M1-033: a reset tears down a running local hit stop first (the clocks
	// resume, the saved movement state and the animation restore) before the
	// instance teardown; a stop never survives into the next life.
	EndHitStop();
	// Reset is a teardown, not an attack ending: no OnFinished. The instance
	// id counter and the dead flag are intentionally left alone (the unified
	// reset semantics belong to M1-027).
	ClearInstance();
	InputBuffer.Reset();
	// M1-024: the reset also tears down the per-float-cycle aerial follow-up
	// records (landing or reset clear the aerial follow-up counts), so a
	// fresh life starts with every target's cycle unspent.
	AerialFollowUpTargetIds.Reset();
	// M3-031: the carried anchors die with the buffer they tracked.
	CarriedInputAnchors.Reset();
	// M5-015: the reset clear point - the execution state (poise pool, air
	// control window, float cycle) is cleared with the rest of the combat
	// bookkeeping, so a fresh life starts from the policy defaults.
	PoiseCurrent = HitTargetReactionPolicy.PoiseMax;
	LastPoisePressureInputClockSeconds = -1.0;
	bAirControlWindowOpen = false;
	bAirControlExpiredUntilLanding = false;
	ResetLaunchCycle();
}

void UCombatComponent::SetDead(bool bNewDead)
{
	if (bNewDead && !bDead)
	{
		// M1-026: death has priority over every non-attacking action state -
		// death, the landing recovery and the hit stun are mutually exclusive,
		// so a combatant that dies mid-recovery or mid-stun drops that state
		// immediately (an in-flight attack keeps the documented SetDead
		// contract untouched; the NotifyHitReceived death path cancels it
		// explicitly).
		if (ActionState == ECombatActionState::Knockdown || ActionState == ECombatActionState::Recovering)
		{
			ActionState = ECombatActionState::Free;
		}
		// M1-033: death clears a running local hit stop (the freeze never
		// outlives the body; the cleanup also restores the saved movement and
		// resumes the animation through the end broadcast).
		EndHitStop();
		EndHitStun();
		LandingKnockdownEndTimeSeconds = 0.0;
		LandingRecoveringEndTimeSeconds = 0.0;
		// M3-023: the dead-flag transition row (EndHitStun above may already
		// have logged a stun-end Free row; the Dead row records the death).
		M3_023_LogActionState(*this, FString(TEXT("Dead")));
		// M5-015: the death clear point - the execution state (poise pool,
		// air-control window, float cycle) never survives into a corpse; a
		// revived combatant starts from the policy defaults.
		PoiseCurrent = HitTargetReactionPolicy.PoiseMax;
		LastPoisePressureInputClockSeconds = -1.0;
		bAirControlWindowOpen = false;
		bAirControlExpiredUntilLanding = false;
		ResetLaunchCycle();
	}
	else if (!bNewDead && bDead)
	{
		// M3-023: the revive half (the unified reset drops the dead flag).
		M3_023_LogActionState(*this, FString(TEXT("Free (revived)")));
	}
	bDead = bNewDead;
}

bool UCombatComponent::IsDead() const
{
	return bDead;
}

void UCombatComponent::NotifyHitReceived(const FCombatHit& Hit)
{
	// A hit that does not name this owner is not this owner's business
	// (defense in depth for the public victim-side entry).
	if (Hit.Target.Get() != GetOwner())
	{
		return;
	}
	// Death has the highest priority (interface contract section 4): a dead
	// combatant neither stuns nor re-runs any death handling.
	if (bDead)
	{
		return;
	}

	// A lethal hit (the attacker already applied the damage before this call,
	// so the owner's health pool is gone) marks the component dead instead of
	// stunning it: a dead combatant never comes back when a stun timer ends.
	const UHealthComponent* OwnerHealth = GetOwner() ? GetOwner()->FindComponentByClass<UHealthComponent>() : nullptr;
	if (OwnerHealth != nullptr && !OwnerHealth->IsAlive())
	{
		SetDead(true);
		CancelCurrentAttack(FName(TEXT("Death")));
		EndHitStun();
		return;
	}

	// M1-026: the landing recovery refuses hits entirely (the death >
	// knockdown/recovering > hit stun priority): no stun is applied and the
	// running process is never restarted, extended or cut short by a hit.
	if (ActionState == ECombatActionState::Knockdown || ActionState == ECombatActionState::Recovering)
	{
		return;
	}

	// Capture the remaining stun before the cancel (the cancel itself never
	// touches an existing stun, but ClearInstance resets the bookkeeping).
	const double RemainingSeconds = (ActionState == ECombatActionState::HitStun)
		? FMath::Max(0.0, HitStunEndTimeSeconds - InputClockSeconds)
		: 0.0;

	// Interrupt first: the running attack dies with the accepted hit and its
	// instance hit set (and any pending active-window damage with it) is
	// cleared, so the interrupted instance can never hit again.
	CancelCurrentAttack(FName(TEXT("HitStun")));

	// New stun = max(remaining, new): a hit during a stun refreshes to the
	// longer of the two and never stacks additively. Non-finite requests are
	// treated as no stun; a zero total leaves the component simply Free.
	const double RequestedSeconds = FMath::IsFinite(Hit.StunSeconds) ? static_cast<double>(Hit.StunSeconds) : 0.0;
	const double StunSeconds = FMath::Max(RemainingSeconds, RequestedSeconds);
	if (StunSeconds > 0.0)
	{
		ActionState = ECombatActionState::HitStun;
		HitStunEndTimeSeconds = InputClockSeconds + StunSeconds;
		// M3-023: the accepted hit's stun transition row.
		M3_023_LogActionState(*this, FString::Printf(TEXT("HitStun (%.2fs)"), StunSeconds));
	}
}

void UCombatComponent::CancelCurrentAttack(FName Reason)
{
	// Idempotent: only a running attack can be cancelled; Free and HitStun
	// already have no instance to tear down.
	if (ActionState != ECombatActionState::Attacking)
	{
		return;
	}
	// A cancel is an interruption, not the attack reaching its final frame:
	// no OnFinished (that delegate's contract is the natural timeline end).
	// ClearInstance drops the timeline, the facing and the instance hit set,
	// so the interrupted instance can never land its pending damage.
	const FName CancelledAttackId = ActiveAttackId;
	const uint64 CancelledInstanceId = ActiveInstanceId;
	ClearInstance();
	// M3-023: the interruption row (reason names the caller: HitStun,
	// JumpCancel, LandingRecovery, Death, PlayerDied, RoomRunFailed).
	M3_023_LogActionState(*this, FString::Printf(TEXT("Free (cancelled %s #%llu reason=%s)"),
		*CancelledAttackId.ToString(), CancelledInstanceId, *Reason.ToString()));
	UE_LOG(LogTemp, Verbose, TEXT("UEMMO UCombatComponent: attack cancelled (reason: %s)"), *Reason.ToString());
}

bool UCombatComponent::BeginLandingRecovery(double NowSeconds)
{
	// The entry needs a finite "now": a non-finite landing time could only
	// produce non-finite deadlines that no clock ever reaches (a permanently
	// unhittable combatant), so it is refused like the stun's non-finite rule.
	if (!FMath::IsFinite(NowSeconds))
	{
		return false;
	}
	// Death has the highest priority (interface contract section 4): a dead
	// combatant never recovers.
	if (bDead)
	{
		return false;
	}
	// One landing event builds exactly one process: an already running
	// recovery (Knockdown or Recovering) is never restarted or extended.
	if (ActionState == ECombatActionState::Knockdown || ActionState == ECombatActionState::Recovering)
	{
		return false;
	}
	// The landing interrupts any in-flight attack (an interruption - no
	// Finished broadcast) and any pending stun bookkeeping: the knockdown
	// takes over both (death > landing recovery > hit stun priority).
	CancelCurrentAttack(FName(TEXT("LandingRecovery")));
	HitStunEndTimeSeconds = 0.0;
	// M5-015: the landing closes the air-control window (the airborne period
	// is over regardless of the down-state that follows).
	bAirControlWindowOpen = false;
	bAirControlExpiredUntilLanding = false;
	ActionState = ECombatActionState::Knockdown;
	LandingKnockdownEndTimeSeconds = NowSeconds + M1_026_KnockdownSeconds;
	LandingRecoveringEndTimeSeconds = LandingKnockdownEndTimeSeconds + M1_026_RecoveringSeconds;
	// M3-023: the landing transition row (the 0.45 s knockdown opens here).
	M3_023_LogActionState(*this, FString(TEXT("Knockdown")));
	return true;
}

bool UCombatComponent::IsInLandingRecovery() const
{
	return ActionState == ECombatActionState::Knockdown || ActionState == ECombatActionState::Recovering;
}

void UCombatComponent::EndLandingRecovery()
{
	if (ActionState != ECombatActionState::Recovering)
	{
		return;
	}
	ActionState = ECombatActionState::Free;
	LandingKnockdownEndTimeSeconds = 0.0;
	LandingRecoveringEndTimeSeconds = 0.0;
	// M3-023: the recovery completion row (Recovering -> Free).
	M3_023_LogActionState(*this, FString(TEXT("Free (recovery end)")));
	// M1-026: the fourth float-cycle clear point (interface contract section
	// 6): the air-combo policy cycle reopens only when the recovery completed,
	// so the next launcher after the recovery rises at the full definition
	// launch speed again. M5-015: the unified victim-component surface (the
	// TrainingEnemy legacy field through the write-through; every other
	// owner's component count) plus the air-control window close.
	ResetLaunchCycle();
	bAirControlWindowOpen = false;
	bAirControlExpiredUntilLanding = false;
}

void UCombatComponent::EndHitStun()
{
	if (ActionState != ECombatActionState::HitStun)
	{
		return;
	}
	ActionState = ECombatActionState::Free;
	HitStunEndTimeSeconds = 0.0;
	// M3-023: the stun completion row (HitStun -> Free).
	M3_023_LogActionState(*this, FString(TEXT("Free (stun end)")));
}

FCombatSnapshot UCombatComponent::GetSnapshot() const
{
	FCombatSnapshot Snapshot;
	Snapshot.AttackId = ActiveAttackId;
	Snapshot.InstanceId = ActiveInstanceId;
	Snapshot.Frame = CurrentFrame;
	Snapshot.Facing = Facing;
	Snapshot.ActionState = ActionState;
	Snapshot.BufferSize = InputBuffer.Size();
	return Snapshot;
}

bool UCombatComponent::CanAcceptMovement() const
{
	return !bDead && ActionState == ECombatActionState::Free;
}

bool UCombatComponent::CanTurn() const
{
	return !bDead && ActionState == ECombatActionState::Free;
}

void UCombatComponent::SetClockFrozen(bool bNewFrozen)
{
	bClockFrozen = bNewFrozen;
}

bool UCombatComponent::IsClockFrozen() const
{
	return bClockFrozen;
}

void UCombatComponent::SetCombatStats(float InAttackPower, float InDefense)
{
	// M3-010: the hardening mirrors FStatCalculator's equipped-row rules: a
	// non-finite field reads as 0 and a negative field clamps to 0, so a
	// poisoned stat row can never poison the formula through this entry. The
	// values are stored only; the damage formula consumes them from the green
	// implementation on.
	CombatAttackPower = (FMath::IsFinite(InAttackPower) && InAttackPower > 0.0f) ? InAttackPower : 0.0f;
	CombatDefense = (FMath::IsFinite(InDefense) && InDefense > 0.0f) ? InDefense : 0.0f;
}

float UCombatComponent::GetAttackPower() const
{
	return CombatAttackPower;
}

float UCombatComponent::GetDefense() const
{
	return CombatDefense;
}

void UCombatComponent::SetTargetReactionPolicy(FTargetReaction InPolicy)
{
	// M5-014: stored verbatim - the policy is configuration the resolver
	// reads (the gate flags, the float-cycle launch policy, the immunity
	// axes); the resolver defends the request against illegal magnitudes.
	HitTargetReactionPolicy = InPolicy;
	// M5-015: an explicit override marks this component as a policy-bearing
	// VICTIM (the hit pipeline resolves against its policy before the
	// attacker's injected member) and initializes its pool state fresh.
	bTargetReactionPolicyOverridden = true;
	PoiseCurrent = InPolicy.PoiseMax;
	LastPoisePressureInputClockSeconds = -1.0;
}

FTargetReaction UCombatComponent::GetTargetReactionPolicy() const
{
	return HitTargetReactionPolicy;
}

bool UCombatComponent::HasOverriddenTargetReactionPolicy() const
{
	return bTargetReactionPolicyOverridden;
}

float UCombatComponent::GetEffectivePoiseCurrent()
{
	const float PoolMax = HitTargetReactionPolicy.PoiseMax;
	if (!(PoolMax > 0.0f) || !FMath::IsFinite(PoolMax))
	{
		return 0.0f;
	}
	// A configured pool that was never pressured reads full (the
	// initialization guard; SetTargetReactionPolicy normally pre-fills it).
	if (PoiseCurrent <= 0.0f && LastPoisePressureInputClockSeconds < 0.0)
	{
		PoiseCurrent = PoolMax;
		return PoiseCurrent;
	}
	// The lazy regeneration: a read after the configured period since the
	// last pressure reports (and settles) the full pool. No tick dependency;
	// the injected input clock is the same one the stun deadlines run on.
	if (PoiseCurrent < PoolMax
		&& HitTargetReactionPolicy.PoiseRegenSeconds > 0.0f
		&& FMath::IsFinite(HitTargetReactionPolicy.PoiseRegenSeconds)
		&& LastPoisePressureInputClockSeconds >= 0.0
		&& FMath::IsFinite(InputClockSeconds)
		&& InputClockSeconds - LastPoisePressureInputClockSeconds >= static_cast<double>(HitTargetReactionPolicy.PoiseRegenSeconds))
	{
		PoiseCurrent = PoolMax;
	}
	return PoiseCurrent;
}

void UCombatComponent::RecordPoisePressure(float Amount)
{
	if (!FMath::IsFinite(Amount) || !(Amount > 0.0f))
	{
		return;
	}
	const float PoolMax = HitTargetReactionPolicy.PoiseMax;
	if (!(PoolMax > 0.0f) || !FMath::IsFinite(PoolMax))
	{
		return;
	}
	// The initialization guard (see GetEffectivePoiseCurrent).
	if (PoiseCurrent <= 0.0f && LastPoisePressureInputClockSeconds < 0.0)
	{
		PoiseCurrent = PoolMax;
	}
	PoiseCurrent = FMath::Max(0.0f, PoiseCurrent - Amount);
	LastPoisePressureInputClockSeconds = InputClockSeconds;
	// The break: a depleted pool resets to full - the granted control is the
	// break's breathing room, so the next hit faces the pool from the top.
	if (PoiseCurrent <= 0.0f)
	{
		PoiseCurrent = PoolMax;
	}
}

int32 UCombatComponent::GetLaunchCycleCount() const
{
	// M5-015: the TrainingEnemy owner keeps its legacy actor field as the
	// storage (the M1-025/M1-026 deferred-restore dance lives there); the
	// component API reads through to it so both surfaces stay one number.
	if (const ATrainingEnemy* EnemyOwner = Cast<const ATrainingEnemy>(GetOwner()))
	{
		return EnemyOwner->GetLauncherCycleCount();
	}
	return LaunchCycleCount;
}

void UCombatComponent::RecordLaunchAdmitted()
{
	if (ATrainingEnemy* EnemyOwner = Cast<ATrainingEnemy>(GetOwner()))
	{
		EnemyOwner->RecordLauncherLaunch();
		return;
	}
	++LaunchCycleCount;
}

void UCombatComponent::ResetLaunchCycle()
{
	if (ATrainingEnemy* EnemyOwner = Cast<ATrainingEnemy>(GetOwner()))
	{
		EnemyOwner->ClearLauncherCycle();
		return;
	}
	LaunchCycleCount = 0;
}

void UCombatComponent::OnGroundContact()
{
	// M5-015: the ground contact reopens the float cycle (the M1-025 clear
	// point; TrainingEnemy keeps its legacy field through the write-through)
	// and closes the air-control window (the airborne period is over).
	ResetLaunchCycle();
	bAirControlWindowOpen = false;
	bAirControlExpiredUntilLanding = false;
}

void UCombatComponent::OpenAirControlWindow(float MaxAirTimeSeconds)
{
	// M5-015: the window is set once per airborne period - a second launch
	// inside it never extends the bound, so the total air time stays bounded.
	if (!FMath::IsFinite(MaxAirTimeSeconds) || !(MaxAirTimeSeconds > 0.0f))
	{
		return;
	}
	if (bAirControlWindowOpen || bAirControlExpiredUntilLanding)
	{
		return;
	}
	bAirControlWindowOpen = true;
	AirControlExpiryInputClockSeconds = InputClockSeconds + static_cast<double>(MaxAirTimeSeconds);
}

bool UCombatComponent::IsAirControlExpired() const
{
	if (bAirControlExpiredUntilLanding)
	{
		return true;
	}
	return bAirControlWindowOpen
		&& FMath::IsFinite(InputClockSeconds)
		&& InputClockSeconds >= AirControlExpiryInputClockSeconds;
}

void UCombatComponent::SetAirborneDamageScale(float InScale)
{
	// M5-015: configuration, not state; non-finite and non-positive scales
	// are refused (a zero scale would silently zero every airborne hit).
	if (!FMath::IsFinite(InScale) || !(InScale > 0.0f))
	{
		return;
	}
	AirborneDamageScale = InScale;
}

float UCombatComponent::GetAirborneDamageScale() const
{
	return AirborneDamageScale;
}

void UCombatComponent::SetAttackReaction(FAttackReaction InReaction)
{
	// M5-014: stored verbatim (see SetTargetReactionPolicy for the
	// configuration-not-state rationale).
	HitAttackReaction = InReaction;
}

int32 UCombatComponent::GetUnifiedLedgerRecordedEventCount() const
{
	return UnifiedHitLedger.GetNumRecordedEvents();
}

int32 UCombatComponent::GetUnifiedLedgerAcceptedControlCount() const
{
	return UnifiedHitLedger.GetNumAcceptedControls();
}

void UCombatComponent::RequestHitStop(float DurationSeconds)
{
	// Death owns the body: a dead component never freezes (a lethal hit's own
	// presentation dispatch is refused here, and the death path already
	// cleared any stop a previous hit had opened).
	if (bDead)
	{
		return;
	}
	const double RequestedSeconds = (FMath::IsFinite(DurationSeconds) && DurationSeconds > 0.0)
		? static_cast<double>(DurationSeconds)
		: 0.0;
	if (RequestedSeconds <= 0.0)
	{
		// A zero or broken request stops nothing (a refused hit carries 0).
		return;
	}

	// Reentry takes max(remaining, requested), never a sum: a second hit while
	// 30 ms remain extends the stop to the new 40 ms, and a 40 ms hit during a
	// 50 ms stop keeps the longer remainder.
	const double RemainingSeconds = bHitStopActive ? HitStopRemainingSeconds : 0.0;
	HitStopRemainingSeconds = FMath::Max(RemainingSeconds, RequestedSeconds);
	const bool bWasActive = bHitStopActive;
	bHitStopActive = true;
	bHitStopConsumedInjectedAdvance = false;
	// The action clock freezes through the existing passthrough; the injected
	// input clock pins through the SetInputClockSeconds hold while this flag
	// is set.
	bClockFrozen = true;
	if (!bWasActive)
	{
		// M3-031: a fresh stop measures its own frozen span from zero.
		HitStopClockJumpSeconds = 0.0;
		// A fresh stop freezes the victim-side movement once (the first save
		// wins; a reentry keeps the pre-freeze state) and pauses a
		// presenter-less owner's mesh animation.
		FreezeOwnerMovementForHitStop();
		ApplyOwnerHitStopAnimationPause(true);
		OnHitStopChanged.Broadcast(true);
	}
}

bool UCombatComponent::IsHitStopActive() const
{
	return bHitStopActive;
}

void UCombatComponent::EndHitStop()
{
	if (!bHitStopActive)
	{
		return;
	}
	bHitStopActive = false;
	HitStopRemainingSeconds = 0.0;
	bHitStopConsumedInjectedAdvance = false;
	// M3-031: the resume applies the accumulated frozen span to the input
	// clock in one jump (the M1-033 fall-through). Buffered presses would pay
	// that span out of their 150 ms lifetimes (the M3-031 red evidence: an
	// X pressed 83 ms into light_01 aged 183 ms by the window step across one
	// 40 ms stop), so every buffered press time and carried anchor shifts by
	// exactly the frozen span: lifetimes stay measured on unfrozen
	// input-clock time only. This runs before the resume, so the first
	// post-stop prune judges the shifted values.
	InputBuffer.ShiftPressedTimes(HitStopClockJumpSeconds);
	ShiftCarriedAnchors(HitStopClockJumpSeconds);
	HitStopClockJumpSeconds = 0.0;
	// The action clock passthrough unfreezes here. A debug freeze set through
	// SetClockFrozen(true) is lifted by a hit stop ending too (a documented
	// debug-tool interaction: both share one freeze flag).
	bClockFrozen = false;
	RestoreOwnerMovementFromHitStop();
	ApplyOwnerHitStopAnimationPause(false);
	// Broadcast last: observers see the fully restored component.
	OnHitStopChanged.Broadcast(false);
}

void UCombatComponent::FreezeOwnerMovementForHitStop()
{
	// One save per stop: a reentry keeps the original pre-freeze state instead
	// of stacking a second save over the frozen one.
	if (bOwnerMovementFrozenByHitStop)
	{
		return;
	}
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	UCharacterMovementComponent* Movement = OwnerCharacter ? OwnerCharacter->GetCharacterMovement() : nullptr;
	if (Movement == nullptr)
	{
		// Non-character owners (test bodies) carry no movement to freeze.
		return;
	}
	// Fold any pending impulse/force (the hit's knockback arrives as a pending
	// impulse one tick before this call) into the velocity first: the engine's
	// MOVE_None transition runs ClearAccumulatedForces internally and would
	// otherwise wipe it. Folding is the same step a real movement update runs
	// (ApplyAccumulatedForces), so the knockback applies at the freeze start
	// instead of 40 ms later - indistinguishable, because nothing integrates
	// while the stop lasts.
	Movement->ApplyAccumulatedForces(1.0f / 60.0f);
	// Save exactly once (after the fold, so the knockback resumes with it).
	HitStopSavedVelocity = Movement->Velocity;
	HitStopSavedPendingLaunchVelocity = Movement->PendingLaunchVelocity;
	HitStopSavedMovementMode = Movement->MovementMode;
	bOwnerMovementFrozenByHitStop = true;
	// MOVE_None integrates nothing (StartNewPhysics early-outs on it), so an
	// airborne target neither falls nor drifts while the stop lasts. The
	// engine's own MOVE_None transition zeroes the velocity and clears the
	// accumulated forces internally, so the saved values are written back
	// right after the mode change: the frozen body keeps its velocity and any
	// still-pending launch observable, ready for the restore.
	Movement->SetMovementMode(MOVE_None);
	Movement->Velocity = HitStopSavedVelocity;
	Movement->PendingLaunchVelocity = HitStopSavedPendingLaunchVelocity;
}

void UCombatComponent::RestoreOwnerMovementFromHitStop()
{
	if (!bOwnerMovementFrozenByHitStop)
	{
		return;
	}
	bOwnerMovementFrozenByHitStop = false;
	ACharacter* OwnerCharacter = Cast<ACharacter>(GetOwner());
	UCharacterMovementComponent* Movement = OwnerCharacter ? OwnerCharacter->GetCharacterMovement() : nullptr;
	if (Movement == nullptr)
	{
		return;
	}
	// Exactly one restore: the saved velocity (including the folded hit
	// knockback and any pending launch) comes back with the saved mode, so the
	// original motion continues.
	Movement->Velocity = HitStopSavedVelocity;
	Movement->PendingLaunchVelocity = HitStopSavedPendingLaunchVelocity;
	Movement->SetMovementMode(HitStopSavedMovementMode);
}

void UCombatComponent::ApplyOwnerHitStopAnimationPause(bool bPause)
{
	// Owners with a presentation component pause through that component (its
	// OnHitStopChanged binding); presenter-less owners (the training enemy)
	// pause their own mesh here, so a local stop always stops the animation
	// while the two paths never double-drive one mesh.
	AActor* OwnerActor = GetOwner();
	if (OwnerActor == nullptr || OwnerActor->FindComponentByClass<UCombatPresentationComponent>() != nullptr)
	{
		return;
	}
	ACharacter* OwnerCharacter = Cast<ACharacter>(OwnerActor);
	USkeletalMeshComponent* Mesh = OwnerCharacter ? OwnerCharacter->GetMesh() : nullptr;
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
	UCombatPresentationComponent::SetAnimInstancePausedForHitStop(AnimInstance, bPause);
}

void UCombatComponent::PruneBufferedInputs()
{
	// M3-031: the shared lifetime prune. Judged per entry from its effective
	// press time: a carried anchor (when present and after the raw stamp)
	// replaces the press time in the age computation; an empty anchor map
	// reduces to the plain PruneExpired semantics verbatim.
	InputBuffer.PruneExpiredAnchored(InputClockSeconds, M1_021_InputLifetimeSeconds, CarriedInputAnchors);
}

void UCombatComponent::RaiseCarriedAnchors(double NowSeconds)
{
	for (TPair<uint64, double>& Anchor : CarriedInputAnchors)
	{
		// Raise, never lower: an anchor at or after Now (a press stamped
		// during a freeze) keeps its future-safe value.
		Anchor.Value = FMath::Max(Anchor.Value, NowSeconds);
	}
}

void UCombatComponent::ShiftCarriedAnchors(double OffsetSeconds)
{
	if (OffsetSeconds == 0.0)
	{
		return;
	}
	for (TPair<uint64, double>& Anchor : CarriedInputAnchors)
	{
		if (FMath::IsFinite(Anchor.Value))
		{
			Anchor.Value += OffsetSeconds;
		}
	}
}

void UCombatComponent::CompactCarriedAnchors()
{
	if (CarriedInputAnchors.Num() == 0)
	{
		return;
	}
	TSet<uint64> BufferedSequences;
	FBufferedCombatInput Entry;
	for (int32 Index = 0; InputBuffer.PeekAt(Index, Entry); ++Index)
	{
		BufferedSequences.Add(Entry.Sequence);
	}
	TArray<uint64> StaleSequences;
	for (TPair<uint64, double>& Anchor : CarriedInputAnchors)
	{
		if (!BufferedSequences.Contains(Anchor.Key))
		{
			StaleSequences.Add(Anchor.Key);
		}
	}
	for (uint64 StaleSequence : StaleSequences)
	{
		CarriedInputAnchors.Remove(StaleSequence);
	}
}

void UCombatComponent::QueueInput(FBufferedCombatInput Input)
{
	// M3-031: a push that lands behind an earlier buffered intent is a
	// CARRIED press (the player queued it while a chain was still pending);
	// its lifetime is judged from the carried anchor instead of the raw press
	// time so it can survive into the follow-up attack's cancel window.
	// Rejected pushes (duplicate/regressing sequence, non-finite time) anchor
	// nothing.
	const bool bBufferWasNonEmpty = InputBuffer.Size() > 0;
	const bool bAccepted = InputBuffer.Push(Input);
	if (bAccepted && bBufferWasNonEmpty)
	{
		CarriedInputAnchors.Add(Input.Sequence, Input.PressedAt);
	}
}

bool UCombatComponent::PeekInputBuffer(FBufferedCombatInput& Out, int32 Index) const
{
	return InputBuffer.PeekAt(Index, Out);
}

void UCombatComponent::SetInputClockSeconds(double NowSeconds)
{
	// M1-033: the input game clock is pinned while a local hit stop freezes
	// this component (interface contract section 2: Pause/HitStop does not
	// advance it, so PressedAt values recorded during the freeze are judged
	// against the frozen value and stun/landing deadlines stop moving).
	// Injections during the freeze still measure real advancement and consume
	// the stop remainder; a still-active freeze returns with the pinned value,
	// while the injection that ends the freeze falls through and applies (the
	// clock resumes advancing with that very injection).
	if (bHitStopActive)
	{
		if (bInputClockInjected && FMath::IsFinite(NowSeconds) && FMath::IsFinite(LastInjectedInputClockSeconds))
		{
			const double Advanced = NowSeconds - LastInjectedInputClockSeconds;
			if (Advanced > 0.0)
			{
				HitStopRemainingSeconds -= Advanced;
				// M3-031: the frozen span the resume will jump the clock over
				// accumulates here; EndHitStop compensates buffered lifetimes
				// by exactly this span.
				HitStopClockJumpSeconds += Advanced;
				bHitStopConsumedInjectedAdvance = true;
				if (HitStopRemainingSeconds <= 0.0)
				{
					EndHitStop();
				}
			}
		}
		if (FMath::IsFinite(NowSeconds))
		{
			LastInjectedInputClockSeconds = NowSeconds;
		}
		if (bHitStopActive)
		{
			// Still frozen: the injected value is dropped, the clock stays at
			// the frozen moment (no state change, no event).
			return;
		}
	}
	else
	{
		if (FMath::IsFinite(NowSeconds))
		{
			LastInjectedInputClockSeconds = NowSeconds;
		}
	}
	InputClockSeconds = NowSeconds;
	// M1-021: the first injection activates the input-driven Free start (see
	// TryStartFromBuffer for the rationale).
	bInputClockInjected = true;
}

double UCombatComponent::GetInputClockSeconds() const
{
	return InputClockSeconds;
}

void UCombatComponent::SetJumpRequestHandler(FCombatJumpRequestHandler InHandler)
{
	// RED-phase stub (M1-023): the handler is stored so tests can bind a
	// counting double, but nothing consumes a Jump intent or invokes it yet.
	// The green implementation owns the Free-state consumption and the
	// launcher jump-cancel.
	JumpRequestHandler = MoveTempIfPossible(InHandler);
}

void UCombatComponent::SetAirStateProvider(FCombatAirStateProvider InProvider)
{
	// The provider is the air/ground routing source (see FCombatAirStateProvider):
	// the game owner binds its own ACharacter::IsFalling in BeginPlay; tests
	// pin a fixed lambda. An empty provider reads as grounded everywhere.
	AirStateProvider = MoveTempIfPossible(InProvider);
}

void UCombatComponent::SetFacingProvider(FCombatFacingProvider InProvider)
{
	// M1-041: the provider is the facing source of the input-driven start (see
	// FCombatFacingProvider): the game owner derives it from the pawn yaw in
	// BeginPlay (the M1-029 flip writes only 0/180); tests pin a fixed lambda.
	// An empty provider keeps the stored Facing (0 for a bare component).
	FacingProvider = MoveTempIfPossible(InProvider);
}

bool UCombatComponent::TryChainFromBuffer()
{
	// The chaining only reads definitions of the running attack and never
	// hardcodes windows or id lists: the cancel window and the allowed
	// follow-ups both come from the injected catalog's definition.
	const UAttackDefinition* Definition = (Catalog != nullptr) ? Catalog->Find(ActiveAttackId) : nullptr;
	if (Definition == nullptr)
	{
		return false;
	}
	if (!Definition->CancelWindow.Contains(CurrentFrame))
	{
		// Outside the cancel window the buffer is deliberately untouched:
		// presses wait there until the window opens (or expire there).
		// M3-031: carried presses re-anchor their lifetime here - a step that
		// offers no consumption opportunity does not age a press that was
		// queued behind an earlier intent, so the queued Z of a rapid X X Z
		// survives light_02's pre-window frames and is judged fresh at the
		// first light_02 window step. Non-carried presses keep the plain
		// press-time lifetime (the M1-014/M1-021 expiry semantics unchanged).
		RaiseCarriedAnchors(InputClockSeconds);
		return false;
	}

	// Lifetime rule first (interface contract section 2): expired entries are
	// dropped before any consumption attempt, and an age of exactly 150 ms is
	// still valid (only a strictly greater age expires). M3-031: judged per
	// entry from its effective press time (carried anchors included).
	PruneBufferedInputs();

	// M1-021: walk the buffer from the earliest entry and take the first one
	// whose action maps to a follow-up the running attack allows (earliest
	// Sequence wins, first come first served; the buffer order is the Sequence
	// order because Push rejects duplicate or regressing sequences). Entries
	// that cannot chain (other actions, follow-up not allowed, missing
	// follow-up definition, dead) stay buffered: the consume happens only
	// after the switch is confirmed, so one step never consumes more than one
	// entry and never double-switches.
	const int32 BufferedCount = InputBuffer.Size();
	for (int32 Index = 0; Index < BufferedCount; ++Index)
	{
		FBufferedCombatInput Entry;
		if (!InputBuffer.PeekAt(Index, Entry))
		{
			continue;
		}
		FName FollowUpAttackId;
		if (!M1_021_ResolveChainAttackId(Entry.Action, FollowUpAttackId))
		{
			continue;
		}
		if (!Definition->AllowedNextAttacks.Contains(FollowUpAttackId))
		{
			// Kept for later consumers; deliberately not consumed here.
			continue;
		}
		// Belt-and-braces before consuming: the follow-up must exist in the
		// catalog and the component must be able to start it (TryStartAttack
		// refuses for a dead component).
		if (bDead || Catalog->Find(FollowUpAttackId) == nullptr)
		{
			continue;
		}

		// Nothing mutates the buffer between the peek and the consume, so
		// ConsumeFirst removes exactly this earliest matching entry.
		FBufferedCombatInput Consumed;
		if (!InputBuffer.ConsumeFirst(Entry.Action, Consumed))
		{
			continue;
		}

		// Switch semantics (M1-014, kept verbatim): the old instance ends
		// (exactly one Finished broadcast, handlers observe a Free snapshot in
		// between, like a natural end) and the follow-up starts immediately
		// with a fresh InstanceId and the same Facing. The caller stops
		// advancing the old timeline, so a frame never skips through the
		// follow-up. The M1-021 record of the switch (cancel reason plus the
		// retired/new instance ids and the consumed sequence) is the Verbose
		// log line below.
		const int32 ChainedFacing = Facing;
		const FName RetiredAttackId = ActiveAttackId;
		const uint64 RetiredInstanceId = ActiveInstanceId;
		FinishCurrentAttack();
		const bool bFollowUpStarted = TryStartAttack(FollowUpAttackId, ChainedFacing);
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO UCombatComponent: cancel-window switch (CancelWindow:%s) retired %s#%llu, started %s#%llu (input sequence %llu)"),
			M1_021_InputActionName(Entry.Action),
			*RetiredAttackId.ToString(), RetiredInstanceId,
			*FollowUpAttackId.ToString(), ActiveInstanceId,
			Consumed.Sequence);
		if (!bFollowUpStarted)
		{
			// The follow-up was verified above, so this is a diagnostic guard,
			// not an expected path: the consumed press is gone, the component
			// stays Free.
			UE_LOG(LogTemp, Warning,
				TEXT("UEMMO UCombatComponent: the consumed %s chain input could not start '%s'; the component stays Free."),
				M1_021_InputActionName(Entry.Action), *FollowUpAttackId.ToString());
		}
		return true;
	}
	return false;
}

bool UCombatComponent::TryStartFromBuffer()
{
	// Death has the highest priority (interface contract section 4): a dead
	// component refuses everything and does not even judge the buffer (the
	// unified dead-side teardown belongs to M1-027).
	if (bDead)
	{
		return false;
	}

	// M1-023: a Free-state Jump intent is actionable only when a jump request
	// handler is bound (the component cannot jump itself; while unbound the
	// entry stays buffered, which keeps the pre-M1-023 observation-only
	// behavior for bare components verbatim). Unlike the attack-start path
	// below, the jump request needs no injected input clock: the game owner
	// binds the handler in BeginPlay while the per-frame clock injection
	// still belongs to a later wiring task, and judging lifetimes against the
	// 0.0 default would misjudge every real press as future-dated. The
	// staleness a clock-less consume can let through is bounded by the
	// window-step prune (entries pressed outside the current window are
	// dropped as future-dated at the first cancel-window step, M1-014
	// semantics) and is a documented limitation, not a new lifetime rule.
	const bool bJumpRequestBound = static_cast<bool>(JumpRequestHandler);

	// The attack-start path judges buffered lifetimes on the injected input
	// game clock (interface contract section 2). Until the owner injects it
	// once (contract: one call per game frame before TickCombat) the component
	// has no valid "now" - treating the 0.0 default as now would misjudge
	// every positive PressedAt as future-dated - so with no clock and no
	// bound jump handler the Free path stays observation-only (the
	// pre-M1-023 game behavior is unchanged).
	if (!bInputClockInjected && !bJumpRequestBound)
	{
		return false;
	}

	// Lifetime rule first (only with a valid clock): expired entries are
	// dropped before any consumption attempt (exactly 150 ms is still valid).
	// M3-031: judged per entry from its effective press time.
	if (bInputClockInjected)
	{
		PruneBufferedInputs();
	}

	// Walk the buffer from the earliest entry and act on the first actionable
	// one (earliest Sequence wins, one consumption per tick, so one press can
	// never fire twice): a Jump with a bound handler requests the owner's
	// jump (M1-023), a mapped Light/Launcher starts its Free attack (M1-021).
	// Entries that cannot be acted on stay buffered: the consume happens only
	// after the action is confirmed.
	// M1-024: the air state is read once per walk and routes the Light start
	// (aerial_01 while the owner reports airborne, light_01 grounded); an
	// unbound provider reads grounded, which keeps the pre-M1-024 bare
	// component behavior verbatim.
	const bool bAirborne = AirStateProvider ? AirStateProvider() : false;
	const int32 BufferedCount = InputBuffer.Size();
	for (int32 Index = 0; Index < BufferedCount; ++Index)
	{
		FBufferedCombatInput Entry;
		if (!InputBuffer.PeekAt(Index, Entry))
		{
			continue;
		}
		if (Entry.Action == ECombatInput::Jump)
		{
			if (!bJumpRequestBound)
			{
				continue;
			}
			// Nothing mutates the buffer between the peek and the consume, so
			// ConsumeFirst removes exactly this earliest Jump entry (the same
			// Sequence can never request a second jump).
			FBufferedCombatInput Consumed;
			if (!InputBuffer.ConsumeFirst(ECombatInput::Jump, Consumed))
			{
				continue;
			}
			UE_LOG(LogTemp, Verbose,
				TEXT("UEMMO UCombatComponent: Free-state jump consumed (input sequence %llu); requesting the owner jump."),
				Consumed.Sequence);
			JumpRequestHandler();
			return true;
		}

		FName StartAttackId;
		if (!M1_021_ResolveFreeStartAttackId(Entry.Action, bAirborne, StartAttackId))
		{
			continue;
		}
		// Attack starts keep their M1-021 clock gate: without an injected
		// clock the lifetime judgment has no basis, so the entry stays.
		if (!bInputClockInjected)
		{
			continue;
		}
		if (Catalog == nullptr || Catalog->Find(StartAttackId) == nullptr)
		{
			continue;
		}

		FBufferedCombatInput Consumed;
		if (!InputBuffer.ConsumeFirst(Entry.Action, Consumed))
		{
			continue;
		}

		// M1-041: the start facing comes from the injected provider (the game
		// owner derives it from the pawn yaw); without a provider the stored
		// Facing (0 for a bare component) keeps the pre-M1-041 behavior
		// verbatim. The new instance steps from its next TickCombat.
		const int32 StartFacing = FacingProvider ? FacingProvider() : Facing;
		TryStartAttack(StartAttackId, StartFacing);
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO UCombatComponent: input-driven start from Free (FreeInput:%s) started %s#%llu (input sequence %llu)"),
			M1_021_InputActionName(Entry.Action),
			*StartAttackId.ToString(), ActiveInstanceId, Consumed.Sequence);
		return true;
	}
	return false;
}

bool UCombatComponent::TryJumpCancelFromBuffer()
{
	// Death has the highest priority (interface contract section 4): a dead
	// component neither cancels into a jump nor consumes anything (M1-027
	// owns the unified dead-side teardown).
	if (bDead)
	{
		return false;
	}
	// No bound handler means no jump to cancel into: keep the intent buffered
	// (this preserves the pre-M1-023 keep-behavior for bare components).
	if (!JumpRequestHandler)
	{
		return false;
	}
	const UAttackDefinition* Definition = (Catalog != nullptr) ? Catalog->Find(ActiveAttackId) : nullptr;
	if (Definition == nullptr || ActiveAttackId != M1_023_JumpCancelAttackId)
	{
		// The jump cancel is launcher-only input semantics (launcher.next
		// holds aerial_01 and the allowance is not an AllowedNextAttacks
		// entry): every other running attack keeps the Jump buffered.
		return false;
	}
	// The cancel window comes from the definition (launcher: [18,32), read
	// through FCombatWindow::Contains); no frame number is hardcoded here.
	if (!Definition->CancelWindow.Contains(CurrentFrame))
	{
		return false;
	}

	// Lifetime rule first (interface contract section 2): expired entries are
	// dropped before any consumption attempt, and an age of exactly 150 ms is
	// still valid (only a strictly greater age expires). M3-031: judged per
	// entry from its effective press time (carried anchors included).
	PruneBufferedInputs();

	// Consume the earliest buffered Jump; consumption removes the entry, so
	// the same Sequence can never cancel twice.
	FBufferedCombatInput Consumed;
	if (!InputBuffer.ConsumeFirst(ECombatInput::Jump, Consumed))
	{
		return false;
	}

	// Cancel semantics (M1-020 path, kept verbatim): an interruption, not the
	// timeline reaching its final frame - no OnFinished - and the instance
	// hit set dies with the instance, so the cancelled launcher can never
	// land its pending damage. CancelCurrentAttack is idempotent and clears
	// the timeline back to Free.
	const int32 CancelledFrame = CurrentFrame;
	const FName RetiredAttackId = ActiveAttackId;
	const uint64 RetiredInstanceId = ActiveInstanceId;
	CancelCurrentAttack(FName(TEXT("JumpCancel")));
	UE_LOG(LogTemp, Verbose,
		TEXT("UEMMO UCombatComponent: jump cancel retired %s#%llu at frame %d (input sequence %llu); requesting the owner jump."),
		*RetiredAttackId.ToString(), RetiredInstanceId, CancelledFrame, Consumed.Sequence);
	// Exactly one jump request per consumed intent, synchronously in the
	// consume path: the owner performs the real ACharacter::Jump here.
	JumpRequestHandler();
	return true;
}

void UCombatComponent::ClearInstance()
{
	ActionState = ECombatActionState::Free;
	ActiveAttackId = NAME_None;
	ActiveInstanceId = 0;
	ActiveDurationFrames = 0;
	CurrentFrame = -1;
	Facing = 0;
	Clock.Reset();
	// M1-020: torn-down instances also drop the stun deadline bookkeeping; a
	// real stun is (re)applied right after the cancel that lands here.
	HitStunEndTimeSeconds = 0.0;
	// M1-026: a teardown also drops the landing-recovery deadlines. ResetCombat
	// funnels here, so a room reset never carries a half-finished recovery
	// (the unified reset semantics belong to M1-027).
	LandingKnockdownEndTimeSeconds = 0.0;
	LandingRecoveringEndTimeSeconds = 0.0;
	// M1-019: the instance's hit set died with the instance (finish, chain
	// switch and reset all funnel through here), so the next instance can hit
	// the same target again. M5-013: the dedup face itself moved into the
	// unified ledger - EndUnifiedShot tombstones the instance's shot, which
	// releases its keys (a fresh instance hits again) while keeping late
	// re-sends of the old shot refused.
	EndUnifiedShot();
}

FEntityId UCombatComponent::ResolveUnifiedEntityId(AActor& Entity, FName Category)
{
	// A cached id is reused only while its registry record still resolves to
	// the same live actor (a destroyed actor's record carries a dead weak
	// reference; address reuse then re-mints instead of inheriting the stale
	// identity).
	if (const FEntityId* Existing = UnifiedEntityIdCache.Find(&Entity))
	{
		const FCombatEntityRecord* Record = UnifiedEntityRegistry.FindEntity(*Existing);
		if (Record != nullptr && Record->Actor.Get() == &Entity)
		{
			return *Existing;
		}
	}

	FCombatEntityMetadata Metadata;
	// The faction stays empty: no production faction source exists yet (the
	// M1-018 team placeholder), and an unaligned entity can never trip the
	// unified entry's friendly-fire filter - the M2-004 same-kind exclusion
	// stays the explicit attacker-side rule below.
	Metadata.Category = Category;
	const FEntityId EntityId = UnifiedEntityRegistry.RegisterEntity(&Entity, Metadata, UnifiedEntityRegistry.GetCurrentEpoch());
	if (IsValidCombatEntityId(EntityId))
	{
		UnifiedEntityIdCache.Add(&Entity, EntityId);
	}
	return EntityId;
}

void UCombatComponent::EndUnifiedShot()
{
	if (!IsValidCombatShotId(ActiveShotId))
	{
		return;
	}
	if (IsValidCombatEntityId(ActiveShotSourceEntityId))
	{
		UnifiedHitLedger.EndShot(UnifiedEntityRegistry.GetCurrentEpoch(), ActiveShotSourceEntityId, ActiveShotId);
	}
	ActiveShotId = InvalidCombatShotId;
	ActiveShotSourceEntityId = InvalidCombatEntityId;
}

uint64 UCombatComponent::GetInstigatorId() const
{
	return CachedInstigatorId;
}

void UCombatComponent::SetFeetLocationProvider(FCombatFeetLocationProvider InProvider)
{
	FeetLocationProvider = MoveTempIfPossible(InProvider);
}

void UCombatComponent::TryApplyActiveWindowHits()
{
	// Guards: no running instance, no catalog entry, no owner or no world
	// means no hits. The definition lookup repeats the one TryStartAttack
	// used, so a swapped catalog can never crash the tick path.
	if (ActionState != ECombatActionState::Attacking)
	{
		return;
	}
	const UAttackDefinition* Definition = (Catalog != nullptr) ? Catalog->Find(ActiveAttackId) : nullptr;
	if (Definition == nullptr)
	{
		return;
	}
	AActor* OwnerActor = GetOwner();
	if (OwnerActor == nullptr || OwnerActor->GetWorld() == nullptr)
	{
		return;
	}
	// Active window test through FCombatWindow::Contains (the definition's
	// window struct delegates there, one place owns the half-open rules).
	if (!Definition->ActiveWindow.Contains(CurrentFrame))
	{
		return;
	}

	// Feet origin first (interface contract section 5), then the pure
	// geometry step, then the read-only world query.
	const FVector FeetLocation = ResolveFeetLocation();
	const FCombatHitBox Box = ComputeHitBox(FeetLocation, Facing, *Definition);
	const TArray<TWeakObjectPtr<AActor>> Targets = QueryTargets(OwnerActor->GetWorld(), Box, OwnerActor, /*AttackerTeam*/ 0);

	// M5-013: the unified identity of this application. The owner's world
	// entity id and the instance's public ActionSequence (the M5-010 registry
	// allocation, never the module-local InstanceId) are minted lazily on the
	// first active frame that reaches here - an attack start on a bare
	// component without an owner has no registry source yet, and such a
	// component never lands hits either.
	const FCombatEpoch Epoch = UnifiedEntityRegistry.GetCurrentEpoch();
	const FEntityId AttackerEntityId = ResolveUnifiedEntityId(*OwnerActor, FName(TEXT("attacker")));
	if (!IsValidCombatEntityId(AttackerEntityId))
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO UCombatComponent: active-window hits skipped (the owner entity id could not be minted)."));
		return;
	}
	if (!IsValidCombatShotId(ActiveShotId))
	{
		ActiveShotId = UnifiedEntityRegistry.AllocateActionSequence(AttackerEntityId, Epoch);
		ActiveShotSourceEntityId = AttackerEntityId;
	}
	if (!IsValidCombatShotId(ActiveShotId))
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO UCombatComponent: active-window hits skipped (the public action sequence could not be allocated)."));
		return;
	}

	// Single hit group per attack instance this card: every current attack is
	// one hit, so all confirmed hits share HitGroupId 0. Multi-hit skills
	// later define one group per sub-hit (interface contract section 5).
	constexpr int32 HitGroupId = 0;

	for (const TWeakObjectPtr<AActor>& WeakTarget : Targets)
	{
		// Stale weak references (an actor destroyed between query and use)
		// are skipped safely; the query filters destroyed actors upstream,
		// this guard is defense in depth.
		AActor* Target = WeakTarget.Get();
		if (Target == nullptr)
		{
			continue;
		}

		// The attacker-side float-cycle bookkeeping id (M1-024). It stays the
		// session object id: this set is per-attacker state, not the unified
		// dedup surface (the ledger key carries the registry entity ids).
		const uint64 TargetId = static_cast<uint64>(Target->GetUniqueID());
		// M2-004: same faction never hurts itself (interface contract sections
		// 4 and 5). The query's AttackerTeam parameter is still a placeholder
		// (M1-018 - no team source exists on actors), so the minimal
		// same-kind exclusion lands at this single call site instead of the
		// query layer: a melee enemy's attack refuses a target of its own kind
		// in the fullest sense (no damage, no impulse, no dedup key, no
		// event; the self exclusion already lives in the query). The
		// APrototypeCharacter player and every non-enemy attacker (including
		// the bare-AActor M1 test scaffolding) stay untouched.
		if (const AMeleeEnemy* MeleeAttacker = Cast<AMeleeEnemy>(OwnerActor))
		{
			if (Target->IsA(MeleeAttacker->GetClass()))
			{
				UE_LOG(LogTemp, Verbose,
					TEXT("UEMMO UCombatComponent: hit on target %llu refused (same-kind enemy, no friendly fire)."),
					TargetId);
				continue;
			}
		}
		// M1-024: one aerial follow-up per target float cycle. A refused
		// follow-up is a miss in the fullest sense: no damage, no impulse,
		// no dedup key (nothing poisons a later instance) and no event.
		if (ActiveAttackId == M1_024_AerialLightAttackId
			&& ShouldRefuseAerialFollowUp(TargetId, *Target))
		{
			UE_LOG(LogTemp, Verbose,
				TEXT("UEMMO UCombatComponent: aerial follow-up on target %llu refused (already spent in this float cycle)."),
				TargetId);
			continue;
		}

		// M5-013: the target's world entity id in this attacker's registry
		// (the unified ledger refuses an unregistered target, so the hit
		// cannot bypass the identity check).
		const FEntityId TargetEntityId = ResolveUnifiedEntityId(*Target, FName(TEXT("target")));
		if (!IsValidCombatEntityId(TargetEntityId))
		{
			UE_LOG(LogTemp, Verbose,
				TEXT("UEMMO UCombatComponent: hit on target %llu refused (the target entity id could not be minted)."),
				TargetId);
			continue;
		}

		// M5-013: the attack side of the unified request - the per-hit numeric
		// definition read from the live attack definition (the exact values
		// the old direct-to-health path consumed).
		UHealthComponent* TargetHealth = Target->FindComponentByClass<UHealthComponent>();
		UCombatComponent* VictimCombat = Target->FindComponentByClass<UCombatComponent>();
		FDamageProfile Profile = M5_013_MakeDamageProfile(*Definition);

		// M5-015: the effective target policy - the victim's explicitly
		// overridden policy wins (per-target heavy/boss configuration through
		// its own combat component); without one the attacker's injected
		// member applies (the M5-014 surface, the default normal target).
		const FTargetReaction EffectiveTargetPolicy =
			(VictimCombat != nullptr && VictimCombat->HasOverriddenTargetReactionPolicy())
				? VictimCombat->GetTargetReactionPolicy()
				: HitTargetReactionPolicy;

		// M5-015: the airborne damage scale hook - an airborne victim takes
		// its configured multiplier (the design's air_damage_scale; the
		// frozen contract carries no such field, so the value lives on the
		// victim component). The default 1.0 keeps every legacy number byte
		// for byte; the scaling happens before both the reaction resolver
		// and the unified entry, so the request and the application agree.
		if (VictimCombat != nullptr)
		{
			const float AirScale = VictimCombat->GetAirborneDamageScale();
			if (AirScale != 1.0f && FMath::IsFinite(AirScale) && AirScale > 0.0f
				&& !M1_024_IsTargetGrounded(*Target))
			{
				Profile.BaseDamage *= AirScale;
			}
		}

		// M5-014: the hit-received request comes from the pure ReactionResolver
		// (the attack reaction + the target policy + the target's state
		// facts). The facts are read here from the only state owners (the
		// victim's health/combat components) - this component's pipeline stays
		// the only HitStun/Knockdown timing and action-state source, and the
		// resolver owns no state, no World and no timer.
		FReactionHitContext ReactionContext;
		ReactionContext.Attack = Profile;
		ReactionContext.AttackReaction = HitAttackReaction;
		ReactionContext.TargetPolicy = EffectiveTargetPolicy;
		ReactionContext.bTargetDead = TargetHealth != nullptr && !TargetHealth->IsAlive();
		ReactionContext.bTargetInLandingRecovery = VictimCombat != nullptr && VictimCombat->IsInLandingRecovery();
		ReactionContext.Facing = Facing;
		ReactionContext.LaunchesUsedThisCycle = M1_025_ResolveLauncherCycleCount(Target);
		// M1-025/M5-014: only the launcher participates in the target's
		// float-cycle launch policy (the per-cycle Z scaling now reads the
		// target policy - 1.0 then 0.7, the third launch refused - whose
		// legacy default carries the exact M1-025 numbers). aerial_01's small
		// compensation stays under the separate M1-024 one-per-cycle gate and
		// every other attack keeps its definition launch verbatim.
		ReactionContext.bPerCycleLaunchScaling = ActiveAttackId == M1_025_LauncherAttackId;
		ReactionContext.AttackPower = CombatAttackPower;
		ReactionContext.Defense = VictimCombat != nullptr ? VictimCombat->GetDefense() : 0.0f;
		// M5-015: the victim's stateful facts for the resolver's poise gate
		// and air-control expiry (the pool owner is the victim component;
		// the resolver itself stays pure).
		ReactionContext.TargetPoiseCurrent = VictimCombat != nullptr ? VictimCombat->GetEffectivePoiseCurrent() : 0.0f;
		ReactionContext.bTargetAirControlExpired = VictimCombat != nullptr && VictimCombat->IsAirControlExpired();
		const FHitReactionRequest Reaction = ResolveHitReaction(ReactionContext);

		if (Reaction.bRefuseHit)
		{
			// The resolver's state gate refused the whole hit (death has
			// priority, then the get-up protection): nothing applies, nothing
			// records - a refused hit never poisons a later instance (the
			// M1-019 semantics; the unified entry enforces the same gate
			// again for every request that does reach it).
			UE_LOG(LogTemp, Verbose,
				TEXT("UEMMO UCombatComponent: hit on target %llu refused by the reaction policy (reason %d)."),
				TargetId, static_cast<int32>(Reaction.RefuseReason));
			continue;
		}
		if (ActiveAttackId == M1_025_LauncherAttackId)
		{
			UE_LOG(LogTemp, Verbose,
				TEXT("UEMMO UCombatComponent: launcher air-combo decision on target %llu (cycle count %d, allowed %d, admitted launch %.1f)"),
				TargetId, ReactionContext.LaunchesUsedThisCycle,
				Reaction.bLaunchAdmitted ? 1 : 0, Reaction.LaunchCmPerSecond);
		}

		// M5-013: the one submission per (attacker, shot, pellet, target). The
		// unified entry validates identity and context first, resolves the
		// damage through the M5-011 resolver (the M1-019 formula), commits the
		// M5-010 ledger dedup key, applies the health through the one health
		// owner and routes the accepted hit through the victim's
		// NotifyHitReceived - there is no second direct-to-health path here.
		FUnifiedHitRequest Request;
		Request.Epoch = Epoch;
		Request.AttackerEntityId = AttackerEntityId;
		Request.ShotId = ActiveShotId;
		Request.PelletIndex = 0;
		Request.TargetEntityId = TargetEntityId;
		Request.TargetActor = Target;
		// M5-014: the launch magnitude the resolver admitted (the policy-scaled
		// value; 0 when the float-cycle policy refused the launch) and the
		// target policy the request was resolved against - the M5-013
		// hardcoded adaptation value became the resolver's legacy default
		// factory feeding the injectable policy member above.
		Request.Attack = Profile;
		Request.Attack.LaunchCmPerSecond = Reaction.LaunchCmPerSecond;
		// M5-015: the stun magnitude the resolver admitted (0 when a gate or
		// the poise pool refused it), so the unified entry's own control
		// summary can never disagree with the request's decision - the
		// victim-side bridge forwards exactly the admitted control.
		Request.Attack.HitStunSeconds = Reaction.StunSeconds;
		Request.TargetReaction = EffectiveTargetPolicy;
		Request.AttackPower = CombatAttackPower;
		Request.HitLocation = Box.Center;

		const FUnifiedHitOutcome Outcome = ApplyUnifiedHit(Request, UnifiedHitLedger);

		if (Outcome.bWasBlocked)
		{
			// A refused submission is a miss in the fullest sense (dead
			// target, get-up protection, same event key, full ledger): nothing
			// applied, nothing recorded - a refused hit never poisons a later
			// instance (the M1-019 semantics, carried by the ledger face).
			UE_LOG(LogTemp, Verbose,
				TEXT("UEMMO UCombatComponent: unified hit on target %llu refused (reason %d, ledger %d, damage %d)."),
				TargetId, static_cast<int32>(Outcome.RefuseReason),
				static_cast<int32>(Outcome.LedgerReason), static_cast<int32>(Outcome.DamageBlockReason));
			continue;
		}
		// The M5 pure-control shape (zero damage with accepted control)
		// applies no impulse and no broadcast: the M1-019 path never paid
		// either for a zero-damage result, and no legacy target policy
		// produces one (the victim-side control already went through the
		// unified entry's NotifyHitReceived bridge).
		if (Outcome.DamageApplied <= 0.0f)
		{
			continue;
		}

		// M1-024: the accepted aerial hit spends the target's float cycle -
		// further aerial hits on it are refused until it lands (or the
		// component resets).
		if (ActiveAttackId == M1_024_AerialLightAttackId)
		{
			AerialFollowUpTargetIds.Add(TargetId);
		}

		FCombatHit Hit;
		Hit.Instigator = OwnerActor;
		Hit.InstigatorId = CachedInstigatorId;
		Hit.AttackInstanceId = ActiveInstanceId;
		Hit.HitGroupId = HitGroupId;
		Hit.Target = Target;
		Hit.Damage = Outcome.DamageApplied;
		Hit.WorldHitLocation = Box.Center;
		Hit.AttackId = ActiveAttackId;
		Hit.HitStopSeconds = Definition->HitStopSeconds;
		// M5-015: the payload's stun request is the admitted one (the value
		// the victim actually received through the bridge), not the raw
		// definition magnitude a gate or the poise pool may have refused.
		Hit.StunSeconds = Outcome.Control.bStagger ? Definition->HitStunSeconds : 0.0f;
		// M5-014: the impulse is the resolver's facing-mirrored request (the X
		// axis only; Y stays 0 - the depth axis is never locked) with the
		// admitted launch magnitude, gated by the unified entry's control fact.
		Hit.Impulse = FVector(Reaction.ImpulseVelocity.X, 0.0f,
			Outcome.Control.bLaunch ? Reaction.LaunchCmPerSecond : 0.0f);

		// A target that died from this hit receives no impulse: death has
		// priority (interface contract section 4).
		if (TargetHealth != nullptr && TargetHealth->IsAlive())
		{
			ApplyHitImpulse(*Target, Hit.Impulse);
			// M1-025/M5-014: only a launch that was actually applied counts
			// into the target's float cycle - a refused launcher (no launch
			// impulse) and a lethal hit (no impulse at all) leave the count
			// untouched, so a still-further launcher keeps being refused in
			// this cycle. M5-015: the unified victim-component surface also
			// opens the bounded air-control window with the effective
			// policy's max air time (set once per airborne period).
			if (Reaction.bLaunchAdmitted && Hit.Impulse.Z > 0.0f)
			{
				if (VictimCombat != nullptr)
				{
					VictimCombat->RecordLaunchAdmitted();
					VictimCombat->OpenAirControlWindow(EffectiveTargetPolicy.MaxAirTimeSeconds);
				}
				else if (ATrainingEnemy* EnemyTarget = Cast<ATrainingEnemy>(Target))
				{
					EnemyTarget->RecordLauncherLaunch();
				}
			}
		}

		// M5-015: the poise pressure rides the resolver's request and is
		// recorded exactly once per accepted submission (a refused submission
		// never reaches this line - the M5-010 ledger face), so a duplicate
		// shot can never deplete the pool twice. The pool owner is the
		// victim component; the break reset and the regen live there too.
		if (VictimCombat != nullptr && Reaction.PoiseDepletion > 0.0f)
		{
			VictimCombat->RecordPoisePressure(Reaction.PoiseDepletion);
		}

		// Only now the hit exists for presentations and state tasks (only a
		// health-removing hit reaches this broadcast; the victim-side stun
		// already ran through the unified entry's NotifyHitReceived bridge,
		// so every OnHitConfirmed observer sees the victim stunned or dead).
		OnHitConfirmed.Broadcast(Hit);
	}
}

bool UCombatComponent::ShouldRefuseAerialFollowUp(uint64 TargetId, const AActor& Target)
{
	// A grounded target's float cycle is closed: any recorded aerial
	// follow-up belongs to a finished leave-ground-to-landing cycle and is
	// dropped here, so the incoming hit opens a fresh cycle. The landing is
	// read lazily from the target itself (its movement component walking on
	// ground, the same check ATrainingEnemy::GetAirState uses) at the only
	// moment the attacker cares: the next aerial hit attempt.
	if (M1_024_IsTargetGrounded(Target))
	{
		AerialFollowUpTargetIds.Remove(TargetId);
		return false;
	}
	// The target still floats and already took an aerial follow-up in this
	// cycle: refuse (the caller treats this as a full miss).
	return AerialFollowUpTargetIds.Contains(TargetId);
}

FVector UCombatComponent::ResolveFeetLocation() const
{
	// The injected provider wins (tests pin an explicit feet origin).
	if (FeetLocationProvider)
	{
		return FeetLocationProvider();
	}
	const AActor* OwnerActor = GetOwner();
	if (OwnerActor == nullptr)
	{
		return FVector::ZeroVector;
	}
	// A character roots on its capsule whose center sits at the actor
	// location: the feet origin is the capsule center minus the half height
	// (interface contract section 5). This matches APrototypeCharacter, whose
	// root capsule carries the standard ACharacter alignment.
	if (const ACharacter* Character = Cast<ACharacter>(OwnerActor))
	{
		if (const UCapsuleComponent* Capsule = Character->GetCapsuleComponent())
		{
			return OwnerActor->GetActorLocation() - FVector(0.0f, 0.0f, Capsule->GetScaledCapsuleHalfHeight());
		}
	}
	// Any other owner (test actors, non-character prototypes): the actor
	// location itself is treated as the feet origin.
	return OwnerActor->GetActorLocation();
}

void UCombatComponent::ApplyHitImpulse(AActor& Target, const FVector& Impulse) const
{
	// Characters take the impulse through their movement component as a
	// velocity change (mass independent; applied on its next movement update).
	if (ACharacter* TargetCharacter = Cast<ACharacter>(&Target))
	{
		if (UCharacterMovementComponent* Movement = TargetCharacter->GetCharacterMovement())
		{
			// M1-022: a hit that carries a launch component (LaunchSpeed > 0:
			// the launcher's 700 cm/s, aerial_01's 60 cm/s) launches the target
			// through ACharacter::LaunchCharacter instead of accumulating an
			// impulse. With bXYOverride=false the horizontal knockback stays
			// additive on top of the target's current velocity; the vertical
			// component is replaced by the definition's launch speed - the
			// engine stores it as the pending launch velocity, which a later
			// launch overwrites and a real movement update applies
			// absolutely, so repeated launcher hits can never stack Z towards
			// 1400, 2100, ... - Launch is velocity injection (no teleport, no
			// ragdoll): the flight resolves through the normal collision
			// integration, also next to walls. Non-launching hits (light:
			// Z == 0) stay on the additive impulse path, which leaves a
			// floating target's vertical speed untouched (a floating hit
			// state is never zeroed by a ground-level hit).
			// M1-024: the vertical replacement became a max with the target's
			// current vertical speed - max(currentZ, Impulse.Z), where
			// currentZ also covers a launch still pending on the movement
			// component (never applied by a movement update yet). The
			// launcher's 700 keeps its behavior (700 >= every current Z the
			// pre-M1-025 flow can produce), while the aerial follow-up's 60
			// never demotes a faster floating target (max(100, 60) = 100) and
			// only lifts slower ones up to exactly 60 (max(30, 60) = 60) -
			// one small compensation per accepted hit, never a per-frame
			// acceleration.
			if (Impulse.Z > 0.0f)
			{
				const float CurrentZ = FMath::Max(Movement->Velocity.Z, Movement->PendingLaunchVelocity.Z);
				const float LaunchZ = FMath::Max(CurrentZ, Impulse.Z);
				TargetCharacter->LaunchCharacter(FVector(Impulse.X, Impulse.Y, LaunchZ),
					/*bXYOverride*/ false, /*bZOverride*/ true);
				return;
			}
			Movement->AddImpulse(Impulse, /*bVelocityChange*/ true);
			return;
		}
	}
	// Simulating rigid bodies get a velocity-change impulse; query-only or
	// static bodies (the test actors of the temp worlds, walls) cannot move
	// and are skipped, which the task report records. The launch override is
	// a CharacterMovement semantic and stays character-only.
	if (UPrimitiveComponent* RootPrimitive = Cast<UPrimitiveComponent>(Target.GetRootComponent()))
	{
		if (RootPrimitive->IsAnySimulatingPhysics())
		{
			RootPrimitive->AddImpulse(Impulse, NAME_None, /*bVelChange*/ true);
		}
	}
}

void UCombatComponent::FinishCurrentAttack()
{
	const FName FinishedAttackId = ActiveAttackId;
	const uint64 FinishedInstanceId = ActiveInstanceId;
	// Clear first: a Finished handler observes a Free snapshot and can already
	// chain the next attack from inside the callback (combo wiring belongs to
	// a later task). Exactly one broadcast per completed instance.
	ClearInstance();
	// M3-023: the natural-end state row (the Finished event rows follow from
	// the broadcast below).
	M3_023_LogActionState(*this, FString::Printf(TEXT("Free (finished %s #%llu)"),
		*FinishedAttackId.ToString(), FinishedInstanceId));
	OnFinished.Broadcast(FinishedAttackId, FinishedInstanceId);
}
