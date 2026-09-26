#include "CombatComponent.h"

#include "AirComboPolicy.h"
#include "AttackCatalog.h"
#include "AttackDefinition.h"
#include "CombatGeometry.h"
#include "CombatHitQuery.h"
#include "HealthComponent.h"
#include "../Enemy/TrainingEnemy.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Logging/LogMacros.h"

#include <atomic>

namespace
{
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

	// M1-025: the attack whose hits run the per-float-cycle air-combo policy
	// (EvaluateAirCombo). Only the launcher is capped and decayed; aerial_01's
	// small compensation stays under the separate M1-024 one-per-cycle gate.
	const FName M1_025_LauncherAttackId(TEXT("launcher"));

	// M1-025: reads the target's current float-cycle launcher count. The count
	// lives target-side on ATrainingEnemy (cleared by ground contact, death
	// and ResetEnemy, see there); any other actor type carries no float cycle
	// yet and reads 0, which keeps every launcher hit allowed at full scale
	// for it (documented limitation until M2 enemies adopt the same surface).
	int32 M1_025_ResolveLauncherCycleCount(const AActor* Target)
	{
		if (const ATrainingEnemy* EnemyTarget = Cast<const ATrainingEnemy>(Target))
		{
			return EnemyTarget->GetLauncherCycleCount();
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
	// Attacker AttackPower and defender Defense have no growth source yet
	// (profile stats belong to M3), so both stay 0 here and the result is
	// max(1, round(BaseDamage)): light_01 deducts exactly 10 and stays
	// verifiable. The formula keeps the two attribute entry points named so
	// M3 can wire real stats without reshaping the call site.
	float M1_019_ComputeHitDamage(const UAttackDefinition& Definition)
	{
		constexpr float AttackPower = 0.0f;
		constexpr float Defense = 0.0f;
		const float RawDamage = (Definition.BaseDamage + AttackPower * Definition.AttackCoefficient)
			* 100.0f / (100.0f + FMath::Max(0.0f, Defense));
		return FMath::Max(1.0f, FMath::RoundToFloat(RawDamage));
	}
}

UCombatComponent::UCombatComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	CachedInstigatorId = M1_019_NextInstigatorId.fetch_add(1) + 1;
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
	OnStarted.Broadcast(ActiveAttackId, ActiveInstanceId);
	return true;
}

void UCombatComponent::TickCombat(float DeltaSeconds)
{
	// M1-026: the landing recovery runs on the injected input clock (the
	// M1-020 stun pattern). The owner keeps calling TickCombat; each branch
	// flips at most one state per tick, exactly like the per-frame game loop,
	// so a clock jump past both deadlines settles on the second tick.
	if (ActionState == ECombatActionState::Knockdown)
	{
		if (InputClockSeconds >= LandingKnockdownEndTimeSeconds)
		{
			ActionState = ECombatActionState::Recovering;
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
	// Reset is a teardown, not an attack ending: no OnFinished. The instance
	// id counter and the dead flag are intentionally left alone (the unified
	// reset semantics belong to M1-027).
	ClearInstance();
	InputBuffer.Reset();
	// M1-024: the reset also tears down the per-float-cycle aerial follow-up
	// records (landing or reset clear the aerial follow-up counts), so a
	// fresh life starts with every target's cycle unspent.
	AerialFollowUpTargetIds.Reset();
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
		EndHitStun();
		LandingKnockdownEndTimeSeconds = 0.0;
		LandingRecoveringEndTimeSeconds = 0.0;
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
	ClearInstance();
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
	ActionState = ECombatActionState::Knockdown;
	LandingKnockdownEndTimeSeconds = NowSeconds + M1_026_KnockdownSeconds;
	LandingRecoveringEndTimeSeconds = LandingKnockdownEndTimeSeconds + M1_026_RecoveringSeconds;
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
	// M1-026: the fourth float-cycle clear point (interface contract section
	// 6): the air-combo policy cycle reopens only when the recovery completed,
	// so the next launcher after the recovery rises at the full definition
	// launch speed again. The count lives target-side on ATrainingEnemy (the
	// M1-025 read pattern); other owner types carry no float cycle.
	if (ATrainingEnemy* EnemyOwner = Cast<ATrainingEnemy>(GetOwner()))
	{
		EnemyOwner->ClearLauncherCycle();
	}
}

void UCombatComponent::EndHitStun()
{
	if (ActionState != ECombatActionState::HitStun)
	{
		return;
	}
	ActionState = ECombatActionState::Free;
	HitStunEndTimeSeconds = 0.0;
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

void UCombatComponent::QueueInput(FBufferedCombatInput Input)
{
	// Push rejections (duplicate/regressing sequence, non-finite time) are
	// intentionally silent: QueueInput reports nothing (M1-012 contract).
	InputBuffer.Push(Input);
}

bool UCombatComponent::PeekInputBuffer(FBufferedCombatInput& Out, int32 Index) const
{
	return InputBuffer.PeekAt(Index, Out);
}

void UCombatComponent::SetInputClockSeconds(double NowSeconds)
{
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
		return false;
	}

	// Lifetime rule first (interface contract section 2): expired entries are
	// dropped before any consumption attempt, and an age of exactly 150 ms is
	// still valid (only a strictly greater age expires).
	InputBuffer.PruneExpired(InputClockSeconds, M1_021_InputLifetimeSeconds);

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
	if (bInputClockInjected)
	{
		InputBuffer.PruneExpired(InputClockSeconds, M1_021_InputLifetimeSeconds);
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

		// The start carries the component's stored Facing (0 until the
		// game-side start wiring passes a real facing; that wiring is a later
		// task). The new instance steps from its next TickCombat.
		TryStartAttack(StartAttackId, Facing);
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
	// still valid (only a strictly greater age expires).
	InputBuffer.PruneExpired(InputClockSeconds, M1_021_InputLifetimeSeconds);

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
	// M1-019: the instance's hit set dies with the instance (finish, chain
	// switch and reset all funnel through here), so the next instance can hit
	// the same target again.
	InstanceHitKeys.Reset();
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

	// Single hit group per attack instance this card: every current attack is
	// one hit, so all keys share HitGroupId 0. Multi-hit skills later define
	// one group per sub-hit (interface contract section 5).
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

		// M1-024: one aerial follow-up per target float cycle. A refused
		// follow-up is a miss in the fullest sense: no damage, no impulse,
		// no dedup key (nothing poisons a later instance) and no event.
		const uint64 TargetId = static_cast<uint64>(Target->GetUniqueID());
		// M1-026: a target inside its landing recovery (Knockdown or
		// Recovering) refuses every hit in the fullest sense: no damage, no
		// impulse, no dedup key and no event - the recovery period is
		// unhittable by contract. The state lives on the victim's own combat
		// component (the same lookup the victim-side notify uses below).
		if (const UCombatComponent* VictimCombat = Target->FindComponentByClass<UCombatComponent>())
		{
			if (VictimCombat->IsInLandingRecovery())
			{
				UE_LOG(LogTemp, Verbose,
					TEXT("UEMMO UCombatComponent: hit on target %llu refused (target is inside its landing recovery)."),
					TargetId);
				continue;
			}
		}
		if (ActiveAttackId == M1_024_AerialLightAttackId
			&& ShouldRefuseAerialFollowUp(TargetId, *Target))
		{
			UE_LOG(LogTemp, Verbose,
				TEXT("UEMMO UCombatComponent: aerial follow-up on target %llu refused (already spent in this float cycle)."),
				TargetId);
			continue;
		}

		// Dedup key of the contract shape. Only an accepted damage adds the
		// key, so a refused hit (dead target, zero result) never blocks a
		// later instance.
		FCombatHitDedupKey Key;
		Key.InstigatorId = CachedInstigatorId;
		Key.AttackInstanceId = ActiveInstanceId;
		Key.HitGroupId = HitGroupId;
		Key.TargetId = TargetId;
		if (InstanceHitKeys.Contains(Key))
		{
			continue;
		}

		UHealthComponent* TargetHealth = Target->FindComponentByClass<UHealthComponent>();
		if (TargetHealth == nullptr)
		{
			continue;
		}

		// The design damage formula (Docs/01 section 8.2) with the current
		// zero growth attributes; light_01 then deducts exactly 10.
		const float Damage = M1_019_ComputeHitDamage(*Definition);
		const float Applied = TargetHealth->ApplyDamage(Damage);
		if (Applied <= 0.0f)
		{
			continue;
		}
		InstanceHitKeys.Add(Key);
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
		Hit.Damage = Applied;
		Hit.WorldHitLocation = Box.Center;
		Hit.AttackId = ActiveAttackId;
		Hit.HitStopSeconds = Definition->HitStopSeconds;
		Hit.StunSeconds = Definition->HitStunSeconds;
		Hit.Impulse = FVector(Facing * Definition->KnockbackSpeed, 0.0f, Definition->LaunchSpeed);

		// M1-025: launcher hits run the per-float-cycle air-combo policy
		// before the impulse leaves the hit site (interface contract section
		// 6: at most two launcher launches per cycle, Z scale 1.0 then 0.7,
		// the third refuses the launch reaction while damage and dedup already
		// happened above). Allowed: the launch Z is the definition launch
		// speed times the policy scale (700 -> 490 on the second launch);
		// refused: the Z component is removed entirely so ApplyHitImpulse
		// takes the additive no-Z path - the X knockback stays on the hit,
		// the target's current vertical state is untouched. The scaled Z
		// flows through the unchanged M1-024 max(currentZ, Impulse.Z)
		// application, so the decay manifests once the target's rise has
		// fallen below the scaled launch - the only regime the real combo
		// timing (the second launcher cannot land before frame 18 + the
		// follow-up's active frame) can produce. Non-launcher attacks keep
		// their definition impulse verbatim.
		bool bLauncherLaunchAllowed = false;
		if (ActiveAttackId == M1_025_LauncherAttackId)
		{
			const int32 LauncherCycleCount = M1_025_ResolveLauncherCycleCount(Target);
			const FAirComboDecision AirComboDecision = EvaluateAirCombo(LauncherCycleCount);
			if (AirComboDecision.bAllowed)
			{
				Hit.Impulse.Z = Definition->LaunchSpeed * AirComboDecision.ZScale;
				bLauncherLaunchAllowed = true;
			}
			else
			{
				Hit.Impulse.Z = 0.0f;
			}
			UE_LOG(LogTemp, Verbose,
				TEXT("UEMMO UCombatComponent: launcher air-combo decision on target %llu (cycle count %d, allowed %d, z scale %.2f)"),
				TargetId, LauncherCycleCount,
				AirComboDecision.bAllowed ? 1 : 0, AirComboDecision.ZScale);
		}

		// A target that died from this hit receives no impulse: death has
		// priority (interface contract section 4).
		if (TargetHealth->IsAlive())
		{
			ApplyHitImpulse(*Target, Hit.Impulse);
			// M1-025: only a launch that was actually applied counts into the
			// target's float cycle - a refused launcher (no launch impulse)
			// and a lethal hit (no impulse at all) leave the count untouched,
			// so a still-further launcher keeps being refused in this cycle.
			if (bLauncherLaunchAllowed && Hit.Impulse.Z > 0.0f)
			{
				if (ATrainingEnemy* EnemyTarget = Cast<ATrainingEnemy>(Target))
				{
					EnemyTarget->RecordLauncherLaunch();
				}
			}
		}

		// M1-020: the accepted hit reaches the victim's combat component (when
		// it has one) before the attacker-side broadcast, so any observer of
		// OnHitConfirmed already sees the victim stunned or dead. The victim
		// entry owns the stun/death decision (death has priority there).
		if (UCombatComponent* VictimCombat = Target->FindComponentByClass<UCombatComponent>())
		{
			VictimCombat->NotifyHitReceived(Hit);
		}

		// Only now the hit exists for presentations and state tasks.
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
	OnFinished.Broadcast(FinishedAttackId, FinishedInstanceId);
}
