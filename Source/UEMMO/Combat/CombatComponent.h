#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Delegates/DelegateCombinations.h"
#include "Engine/EngineTypes.h"
#include "Templates/Function.h"

#include "AttackCatalog.h"
#include "CombatClock.h"
#include "CombatHitTypes.h"
#include "CombatInputBuffer.h"

#include "System/CombatEntityRegistry.h"
#include "System/HitLedger.h"
#include "System/ReactionResolver.h"

#include "CombatComponent.generated.h"

class UAttackCatalog;
class UAttackDefinition;

/**
 * Action state of one combatant (interface contract section 4). M1-011 ships
 * the lifecycle subset; later tasks append HitStun / Knockdown / Recovering
 * (append only, never renumber, so existing values keep meaning).
 */
UENUM(BlueprintType)
enum class ECombatActionState : uint8
{
	/** No action in flight; a new attack may start. */
	Free = 0,
	/** An attack instance is running its frame timeline. */
	Attacking = 1,
	/** M1-020: an accepted hit interrupted the combatant; attacks stay refused until the stun ends. */
	HitStun = 2,
	/**
	 * M1-026: a launched landing knocked the combatant down (interface
	 * contract section 6); every hit on it is refused until it recovered.
	 */
	Knockdown = 3,
	/** M1-026: the knockdown ended and the combatant is getting back up; hits stay refused. */
	Recovering = 4
};

/**
 * Read-only view of the combat component state (interface contract section 4).
 * InstanceId is a stable in-session monotonic id starting at 1, never a raw
 * pointer value. Frame is the current logic frame of the running attack:
 * -1 before the first step (the clock is reset on start, so the first
 * TickCombat after a successful start lands on Frame 0), and the attack ends
 * on the step that reaches DurationFrames - 1.
 */
USTRUCT(BlueprintType)
struct FCombatSnapshot
{
	GENERATED_BODY()

	/** AttackId of the running instance; NAME_None while Free. */
	UPROPERTY(BlueprintReadOnly, Category = "Combat")
	FName AttackId = NAME_None;

	/**
	 * In-session instance id of the running attack; 0 while Free. Kept as
	 * uint64 without Blueprint exposure: UHT does not support uint64 in
	 * Blueprint-visible properties, and the stable-id contract requires the
	 * full unsigned 64-bit range.
	 */
	UPROPERTY(Transient)
	uint64 InstanceId = 0;

	/** Current logic frame; -1 while Free or before the first tick. */
	UPROPERTY(BlueprintReadOnly, Category = "Combat")
	int32 Frame = -1;

	/** Facing passed to TryStartAttack (+1 / -1 mirrors the X axis). */
	UPROPERTY(BlueprintReadOnly, Category = "Combat")
	int32 Facing = 0;

	/** Coarse action state; death is tracked separately and has priority. */
	UPROPERTY(BlueprintReadOnly, Category = "Combat")
	ECombatActionState ActionState = ECombatActionState::Free;

	/** Size of the bounded combat input buffer (M1-011 keeps it empty). */
	UPROPERTY(BlueprintReadOnly, Category = "Combat")
	int32 BufferSize = 0;
};

/** Broadcast on every successful attack start with its AttackId and InstanceId. */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnCombatStarted, FName /*AttackId*/, uint64 /*InstanceId*/);

/** Broadcast exactly once when an attack instance reaches its final frame. */
DECLARE_MULTICAST_DELEGATE_TwoParams(FOnCombatFinished, FName /*AttackId*/, uint64 /*InstanceId*/);

/** Broadcast exactly once per accepted hit: ApplyDamage removed health (> 0). */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCombatHitConfirmed, const FCombatHit& /*Hit*/);

/**
 * M1-033: broadcast when this component's local hit stop starts (true) and
 * when it ends (false). Presentation components bind it to pause/resume their
 * mesh playback for the freeze duration.
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnCombatHitStopChanged, bool /*bFrozen*/);

/**
 * Injectable feet-origin source (interface contract section 5: the feet
 * location is the capsule center minus the capsule half height). The default
 * derivation reads the owner (capsule half height for characters, the actor
 * location as feet for any other root); tests can pin an explicit origin.
 */
using FCombatFeetLocationProvider = TFunction<FVector()>;

/**
 * M1-023: injectable jump request. The component owns no movement and cannot
 * call ACharacter::Jump itself, so every consumed Jump intent (the Free-state
 * jump and the launcher jump-cancel) asks the owner through this handler to
 * perform the real jump, synchronously in the consume path. Tests bind a
 * counting handler to capture the requests.
 */
using FCombatJumpRequestHandler = TFunction<void()>;

/**
 * M1-024: injectable airborne predicate. Returns true while the owner is in
 * the air (the game owner binds ACharacter::IsFalling in BeginPlay; tests bind
 * a fixed lambda). The value routes the Free-state buffered Light start: an
 * airborne Light begins aerial_01, a grounded Light keeps light_01. An empty
 * provider reads as grounded, which keeps the pre-M1-024 bare-component
 * behavior verbatim.
 */
using FCombatAirStateProvider = TFunction<bool()>;

/**
 * M1-041: injectable facing source for the input-driven attack start (+1
 * right / -1 left, the X axis mirror convention of TryStartAttack). The game
 * owner binds it in BeginPlay (derived from the pawn yaw the M1-029 planar
 * flip writes); tests may pin a fixed lambda. While unbound, an input-driven
 * start carries the stored Facing (0 for a bare component), which keeps the
 * pre-M1-041 bare-component behavior verbatim.
 */
using FCombatFacingProvider = TFunction<int32()>;

/**
 * Component-level attack lifecycle (M1-011): start one attack, advance it on a
 * fixed 60 Hz FCombatClock and finish it exactly once. Pure logic: no keyboard
 * input, no hit detection, no animation and no character movement is wired
 * here (later tasks own those). The catalog is injected explicitly with
 * InitializeFromCatalog; the constructor reads nothing from ini.
 */
UCLASS(ClassGroup = (Combat), meta = (BlueprintSpawnableComponent))
class UCombatComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCombatComponent();

	/**
	 * Attaches the read-only attack catalog used by TryStartAttack lookups.
	 * Returns false and keeps the previous catalog when InCatalog is null.
	 */
	bool InitializeFromCatalog(UAttackCatalog* InCatalog);

	/**
	 * Starts a new attack instance when Free, alive and the catalog holds the
	 * AttackId. Every successful start mints a fresh InstanceId (monotonic
	 * from 1, never reused in this session) and broadcasts OnStarted with the
	 * AttackId and the new InstanceId. Returns false (no state change, no
	 * event) when dead, already attacking, stunned (HitStun, M1-020), or the
	 * definition is missing (a diagnostic is logged once per missing id).
	 */
	bool TryStartAttack(FName AttackId, int32 Facing);

	/**
	 * Advances the running attack on the fixed 60 Hz clock. The first step
	 * after a start lands on Frame 0; the step that reaches DurationFrames - 1
	 * ends the attack: OnFinished is broadcast exactly once and the state
	 * returns to Free. While HitStun (M1-020) the tick only checks the
	 * injected input clock: on the first tick whose clock reached the stun end
	 * the state returns to Free. While Free (M1-021) the tick consumes the
	 * earliest valid buffered Light/Launcher and starts its mapped attack
	 * (input-driven start; inert until the input clock was injected once);
	 * M1-024 routes the mapped Light by the injected air state (aerial_01
	 * while the owner reports airborne, light_01 grounded; the Launcher
	 * mapping never changes); M1-023 adds the earliest buffered Jump to the
	 * same walk (one owner jump request per consumed press when a handler is
	 * bound). The frozen flag (hit stop and cutscene freezes belong to later
	 * tasks) is passed straight through to the clock: a frozen frame drops its
	 * delta and advances no frame.
	 */
	void TickCombat(float DeltaSeconds);

	/**
	 * Tears down the running action, the input buffer and the clock, and
	 * returns to Free without broadcasting OnFinished (reset is a teardown,
	 * not an attack ending). Does not touch the dead flag or the InstanceId
	 * counter: the next successful start still mints a new, larger id.
	 */
	void ResetCombat();

	/**
	 * Minimal death hook for M1-011: a dead component refuses new attacks.
	 * It does not interrupt an attack that is already in flight and is not
	 * connected to UHealthComponent (M1-016/M1-019 own that wiring).
	 */
	void SetDead(bool bNewDead);

	bool IsDead() const;

	/**
	 * M1-020: tears down the running attack without broadcasting OnFinished
	 * (the delegate means the timeline reached its final frame; a cancel is an
	 * interruption). Clears the instance state and the instance hit set, so
	 * the cancelled instance can never hit again. Idempotent: no-op unless an
	 * attack is currently running.
	 */
	void CancelCurrentAttack(FName Reason);

	/**
	 * M1-020: victim-side entry of one accepted hit (the attacker's damage
	 * application calls this on the target's component after ApplyDamage
	 * removed health). While alive, the hit interrupts the running attack
	 * (CancelCurrentAttack("HitStun")) and puts the component into HitStun for
	 * Hit.StunSeconds on the injected input clock; a new stun is
	 * max(remaining, new), never additive. Death has priority: an already dead
	 * component ignores the hit, and a lethal hit (the owner's health pool is
	 * gone) marks the component dead instead of stunning it. M1-026: a
	 * combatant inside its landing recovery (Knockdown or Recovering) refuses
	 * the hit entirely - the stun request never takes over the recovery
	 * process (death > landing recovery > hit stun priority).
	 */
	void NotifyHitReceived(const FCombatHit& Hit);

	/**
	 * M1-026: victim-side entry of one landed-after-launch recovery process
	 * (interface contract section 6: a launched landing builds exactly one
	 * Knockdown 0.45 s -> Recovering 0.25 s -> Free sequence). The two
	 * deadlines are fixed at the landing moment on the injected input clock,
	 * so the whole process always totals 0.70 s regardless of tick spacing.
	 * Returns false without any state change when the combatant is dead
	 * (death has priority; the dead never recover) or when a recovery process
	 * is already running (one landing event builds exactly one process;
	 * duplicate Landed notifies never restart the timers).
	 */
	bool BeginLandingRecovery(double NowSeconds);

	/**
	 * M1-026: true while this combatant is inside its landing recovery
	 * process (Knockdown or Recovering). The attacker-side hit application
	 * refuses such targets entirely (no damage, no impulse, no dedup key, no
	 * event) and the victim-side NotifyHitReceived refuses stun requests.
	 */
	bool IsInLandingRecovery() const;

	FCombatSnapshot GetSnapshot() const;

	/**
	 * M1-028: read-only debug view of the definition the running attack
	 * instance queries - the exact pair (Attacking guard + Catalog::Find on
	 * ActiveAttackId) TryApplyActiveWindowHits uses, so the HUD's debug box
	 * can never diverge from the real query. Null while Free/stunned/dead or
	 * without an attached catalog: the overlay draws no box then. Inline here
	 * because the M1-028 file scope extends this header only; the include of
	 * AttackCatalog.h below makes the inline Find call compile.
	 */
	const UAttackDefinition* GetCurrentDefinition() const
	{
		return (ActionState == ECombatActionState::Attacking && Catalog != nullptr)
			? Catalog->Find(ActiveAttackId)
			: nullptr;
	}

	/**
	 * M1-028: the feet origin the real hit query resolves (provider first,
	 * owner second - ResolveFeetLocation), so the debug box shares the query
	 * origin exactly. Inline for the same file-scope reason.
	 */
	FVector GetDebugFeetLocation() const
	{
		return ResolveFeetLocation();
	}

	/**
	 * Snapshot-driven minimal rule consumed by M1-013: movement is accepted
	 * only while Free and alive; death has priority (contract section 4).
	 */
	bool CanAcceptMovement() const;

	/** Same rule as CanAcceptMovement; Facing mirroring stays character-side. */
	bool CanTurn() const;

	/** Broadcast on every successful start with the new AttackId/InstanceId. */
	FOnCombatStarted OnStarted;

	/** Broadcast exactly once per completed attack instance. */
	FOnCombatFinished OnFinished;

	/**
	 * M1-019: broadcast once per accepted hit while the attack's active window
	 * advances. Only a hit whose ApplyDamage call removed health (> 0) is
	 * broadcast; an overlap that was refused (dead target, zero result) or
	 * already deduplicated never fires.
	 */
	FOnCombatHitConfirmed OnHitConfirmed;

	/**
	 * M1-019: stable in-session id of this component as an attacker, minted
	 * once at construction from a session-wide counter (monotonic, never a
	 * raw pointer value; interface contract sections 4 and 5).
	 */
	uint64 GetInstigatorId() const;

	/**
	 * M1-019: overrides the feet-origin derivation with an explicit provider
	 * (tests pin a fixed origin). Passing an empty function restores the
	 * default owner-based derivation.
	 */
	void SetFeetLocationProvider(FCombatFeetLocationProvider InProvider);

	/** Freezes/unfreezes the action clock passthrough (default: not frozen). */
	void SetClockFrozen(bool bNewFrozen);

	bool IsClockFrozen() const;

	/**
	 * M3-010: injects this combatant's growth attributes into the M1-019
	 * damage formula. AttackPower is read when this component attacks; Defense
	 * is read from this component when it is a hit VICTIM (the attacker's
	 * pipeline looks the value up on the target's combat component). Defaults
	 * are 0/0, which keeps every pre-M3-010 result verbatim (the M1-019
	 * constants that stood in before any growth source existed). Non-finite
	 * fields are stored as 0 and negative fields are clamped to 0 (the same
	 * hardening rule FStatCalculator applies to equipped stat rows).
	 */
	void SetCombatStats(float InAttackPower, float InDefense);

	/** M3-010: attack power this component feeds the damage formula as an attacker. */
	float GetAttackPower() const;

	/** M3-010: defense this component contributes when it is a hit victim. */
	float GetDefense() const;

	/**
	 * M5-014: overrides the target-side reaction policy (M5-003) this
	 * component's hits resolve against. The policy is configuration, not
	 * action state: it feeds the M5-014 ReactionResolver, which computes the
	 * hit-received request (stun gate, launch scaling, impulse, immunities)
	 * for every hit this component applies. The default is the legacy normal
	 * target minted by ReactionResolver::MakeLegacyNormalTargetReaction - the
	 * exact value the M5-013 adaptation hardcoded - so every pre-M5-014
	 * result stays verbatim until a caller overrides it.
	 */
	void SetTargetReactionPolicy(FTargetReaction InPolicy);

	/** M5-014: the target-side reaction policy this component's hits resolve against (see SetTargetReactionPolicy). */
	FTargetReaction GetTargetReactionPolicy() const;

	/**
	 * M5-014: overrides the attack-side reaction (M5-003) this component's
	 * hits carry into the resolver. The default is default-constructed:
	 * EControlPenetration::None, the legacy behavior of every melee attack
	 * (control faces the target's poise threshold normally; a poise-free
	 * target never notices, and BypassPoise never bypasses the explicit
	 * allow gates or a blanket control immunity either way).
	 */
	void SetAttackReaction(FAttackReaction InReaction);

	/**
	 * M5-013: diagnostic count of the hit events this component's unified hit
	 * ledger (M5-010) currently records as live keys. Every melee hit this
	 * component applies flows through the M5-012 unified entry, which commits
	 * exactly one event key per (epoch, attacker, shot, pellet, target); a
	 * shot that ended (attack finished/cancelled/reset) releases its keys back
	 * to the pool, so a torn-down instance reads 0 while a running one carries
	 * its accepted hits.
	 */
	int32 GetUnifiedLedgerRecordedEventCount() const;

	/** M5-013: diagnostic count of the shot controls the unified ledger accepted (live keys only). */
	int32 GetUnifiedLedgerAcceptedControlCount() const;

	/**
	 * M1-033: requests a local hit stop of DurationSeconds on this component
	 * (the card's 40 ms rides in on FCombatHit::HitStopSeconds from the
	 * definition). While the stop runs: the action clock freezes (the
	 * bClockFrozen passthrough), the injected input game clock pins at the
	 * frozen value (interface contract section 2: Pause/HitStop does not
	 * advance it; presses still queue), and a character owner's movement saves
	 * its velocity/mode once and integrates nothing until the stop ends
	 * restores them. Reentry takes max(remaining, requested), never a sum.
	 * ResetCombat and death clear a running stop. Non-finite and non-positive
	 * requests are ignored; a dead component refuses. The trigger is the hit
	 * presentation path (a bound UCombatPresentationComponent observing
	 * OnHitConfirmed for the attacker and the hit target); a bare component
	 * without a presenter keeps the pre-M1-033 behavior verbatim.
	 */
	void RequestHitStop(float DurationSeconds);

	/** M1-033: true while a local hit stop freezes this component. */
	bool IsHitStopActive() const;

	/** M1-033: broadcast on the local hit stop start (true) and end (false). */
	FOnCombatHitStopChanged OnHitStopChanged;

	/**
	 * M1-012: buffers one combat intent into the component's input buffer
	 * (interface contract section 4). The buffer rejects duplicate or
	 * regressing sequence numbers and non-finite timestamps; a rejected push
	 * is silently ignored here (no state change, no event).
	 */
	void QueueInput(FBufferedCombatInput Input);

	/**
	 * M1-012: copies one buffered input without consuming or reordering
	 * anything (Index 0 = earliest entry). Returns false when the index is out
	 * of range. Read-only observation for diagnostics and tests; consumption
	 * wiring stays with M1-014.
	 */
	bool PeekInputBuffer(FBufferedCombatInput& Out, int32 Index = 0) const;

	/**
	 * M1-014: overrides the component's input game clock, the "now" the
	 * cancel-window chaining compares buffered PressedAt ages against. The
	 * owner injects it explicitly (from the pause-aware game clock, one call
	 * per game frame before TickCombat); tests inject fixed times (interface
	 * contract section 2: early tests use explicit times). TickCombat never
	 * advances this clock itself, and QueueInput carries PressedAt in the
	 * same time base, so a set value is used verbatim at the next window step.
	 * The first injection also activates the M1-021 input-driven Free start
	 * (TryStartFromBuffer).
	 */
	void SetInputClockSeconds(double NowSeconds);

	/** Current input game clock value (0.0 until the first override). */
	double GetInputClockSeconds() const;

	/**
	 * M1-023: binds the jump request handler (see FCombatJumpRequestHandler).
	 * Passing an empty function unbinds it: while unbound, buffered Jump
	 * intents are never consumed (they stay buffered), which keeps the
	 * pre-M1-023 observation-only behavior for bare components verbatim.
	 */
	void SetJumpRequestHandler(FCombatJumpRequestHandler InHandler);

	/**
	 * M1-024: binds the airborne predicate (see FCombatAirStateProvider).
	 * Passing an empty function unbinds it: while unbound the component reads
	 * as grounded and every buffered Light keeps starting light_01.
	 */
	void SetAirStateProvider(FCombatAirStateProvider InProvider);

	/**
	 * M1-041: binds the facing source (see FCombatFacingProvider) the
	 * input-driven Free start passes to TryStartAttack. Passing an empty
	 * function unbinds it: while unbound an input-driven start keeps the
	 * stored Facing (0 for a bare component).
	 */
	void SetFacingProvider(FCombatFacingProvider InProvider);

private:
	/**
	 * M1-014: when the running attack is inside its cancel window, prunes
	 * expired buffered inputs and switches into the earliest buffered entry
	 * whose action maps to a follow-up the running attack allows (M1-021
	 * generalized the M1-014 Light-only rule to the input type: a Light maps
	 * to light_02, a Launcher to launcher; the Jump is not a chain input and
	 * is handled by the M1-023 TryJumpCancelFromBuffer step that follows this
	 * one in the frame loop). Inputs that
	 * cannot chain (other actions, follow-up not allowed, expired, missing
	 * follow-up definition) stay buffered. Returns true when the running
	 * instance changed, so the caller stops advancing the old timeline for
	 * this tick.
	 */
	bool TryChainFromBuffer();

	/**
	 * M1-021: while Free and alive, prunes expired buffered inputs and starts
	 * the earliest buffered Light/Launcher whose mapped Free attack id exists
	 * in the catalog (grounded: Light -> light_01, Launcher -> launcher).
	 * M1-024 routes the Light by the injected air state: while the owner
	 * reports airborne the same press starts aerial_01 instead (the Launcher
	 * mapping never changes). M1-023 extends the same earliest-Sequence walk
	 * to a buffered Jump: with a bound jump request handler the Jump is
	 * consumed into exactly one owner jump request (no attack starts);
	 * unbound, the Jump stays buffered. Consumes exactly one entry per tick,
	 * so one press starts one attack or requests one jump and can never fire
	 * twice. The attack-start half stays gated on the injected input clock;
	 * the jump half is gated on the bound handler only (see
	 * SetJumpRequestHandler for the rationale).
	 * Returns true when an attack started or a jump was requested.
	 */
	bool TryStartFromBuffer();

	/**
	 * M1-023: while the launcher attack runs inside its definition's cancel
	 * window, consumes the earliest buffered Jump and cancels the running
	 * attack (no Finished, hit set cleared) before requesting exactly one
	 * owner jump through the bound handler. The window is read from the
	 * definition (never hardcoded) and the launcher-only allowance is input
	 * semantics, not an AllowedNextAttacks entry; other attacks keep the
	 * Jump buffered. Without a bound handler nothing is consumed. Returns
	 * true when the running instance was cancelled, so the caller stops
	 * advancing the old timeline for this tick.
	 */
	bool TryJumpCancelFromBuffer();

	/** Clears the running instance (not the id counter) back to Free defaults. */
	void ClearInstance();

	/** M1-020: leaves HitStun back to Free (no-op unless currently stunned). */
	void EndHitStun();

	/**
	 * M1-026: leaves Recovering back to Free and reopens the owner's float
	 * cycle (the fourth LauncherCycleCount clear point: the policy cycle
	 * reopens only when the recovery completed, so the next launcher after
	 * the recovery rises at full definition speed again). No-op unless
	 * currently Recovering.
	 */
	void EndLandingRecovery();

	/** Ends the current attack: clears state first, then broadcasts OnFinished once. */
	void FinishCurrentAttack();

	/**
	 * M1-033: clears a running local hit stop: the clock freeze lifts, the
	 * saved movement state restores exactly once, a presenter-less owner's
	 * animation resumes and OnHitStopChanged broadcasts false. No-op without a
	 * running stop.
	 */
	void EndHitStop();

	/**
	 * M1-033: saves the owner character's movement state exactly once (the
	 * first save wins; a reentry keeps the pre-freeze state) and puts the
	 * movement component into MOVE_None, which integrates nothing (an airborne
	 * target neither falls nor drifts while the stop lasts). Non-character
	 * owners skip (no movement to freeze).
	 */
	void FreezeOwnerMovementForHitStop();

	/** M1-033: restores the saved movement state exactly once (velocity + mode). */
	void RestoreOwnerMovementFromHitStop();

	/**
	 * M1-033: pauses/resumes a presenter-less owner's mesh animation directly
	 * (the training enemy's single-node idle). Owners with a presentation
	 * component pause through that component's OnHitStopChanged binding
	 * instead, so the two paths never double-drive one mesh.
	 */
	void ApplyOwnerHitStopAnimationPause(bool bPause);

	/**
	 * M1-019: when the current frame sits inside the running attack's active
	 * window, queries the hit box targets and applies one deduplicated damage
	 * per target (dedup key = InstigatorId/AttackInstanceId/HitGroupId/
	 * TargetId, recorded only on an accepted ApplyDamage). Safe no-op without
	 * a catalog, definition, owner or world; stale and dead targets skip.
	 * M5-013: the per-target application goes through the M5-012 unified hit
	 * entry (ApplyUnifiedHit) - the M5-010 ledger is the dedup face and the
	 * M5-011 resolver the damage face, so there is no second direct-to-health
	 * path left in this component.
	 */
	void TryApplyActiveWindowHits();

	/**
	 * M5-013: resolves (and lazily mints) the unified world entity id of one
	 * actor in this component's M5-010 registry. The first registration wins
	 * and is cached per actor; a cached id is reused only while the registry
	 * record still resolves to the same live actor. Faction stays empty (no
	 * production faction source exists yet), the category is an informational
	 * label ("attacker" for the owner, "target" for hit targets). Returns
	 * InvalidCombatEntityId when the registry refused.
	 */
	FEntityId ResolveUnifiedEntityId(AActor& Entity, FName Category);

	/**
	 * M5-013: tombstones the running instance's shot in the unified ledger
	 * (its keys release back to the pool while late re-sends stay refused) and
	 * drops the instance's public ActionSequence slot. Runs from the instance
	 * teardown (finish, cancel, reset), so attack instances never leak active
	 * ledger capacity.
	 */
	void EndUnifiedShot();

	/** M1-019: resolves the current feet origin (provider first, owner second). */
	FVector ResolveFeetLocation() const;

	/**
	 * M3-031: the buffered-input lifetime prune shared by every consumer
	 * (interface contract section 2: expired entries drop before any
	 * consumption attempt; exactly 150 ms is still valid). Judged per entry
	 * from its effective press time (carried anchors may move the judgment
	 * forward); an empty anchor map reduces to the plain PruneExpired.
	 */
	void PruneBufferedInputs();

	/**
	 * M3-031: raises every carried press's lifetime anchor to NowSeconds
	 * (never lowers one). Called from the attack-step branches that offer no
	 * consumption opportunity, so a queued press does not age while it
	 * physically cannot be consumed.
	 */
	void RaiseCarriedAnchors(double NowSeconds);

	/** M3-031: shifts every carried anchor by OffsetSeconds (hit stop compensation). */
	void ShiftCarriedAnchors(double OffsetSeconds);

	/**
	 * M3-031: drops carried anchors whose sequence is no longer buffered
	 * (consumed, pruned or reset). Runs once per TickCombat so the map stays
	 * bounded without coupling the consumers to the bookkeeping.
	 */
	void CompactCarriedAnchors();

	/**
	 * M1-019: applies the hit impulse to a surviving target, skipping bodies
	 * that cannot move. M1-022: a hit that carries a launch component
	 * (Impulse.Z > 0) launches a character target through
	 * ACharacter::LaunchCharacter with bXYOverride=false (the horizontal
	 * knockback stays additive on top of the target's current velocity) and a
	 * character-vertical override; hits without a launch component keep the
	 * additive AddImpulse path, so a floating target's vertical speed is
	 * never zeroed by a ground-level hit. M1-024: the vertical component is a
	 * max with the target's current vertical speed, not a blind replacement:
	 * the launch Z is max(currentZ, Impulse.Z) where currentZ also covers a
	 * launch still pending on the movement component - a launcher's 700 cm/s
	 * stays 700 (700 >= any current Z in practice, unchanged behavior) while
	 * the aerial follow-up (60 cm/s) never demotes a faster floating target
	 * and only lifts slower ones up to 60. M1-025: the launcher's Z can now
	 * arrive decayed (490 on a float cycle's second launch, 0 on a refused
	 * third one); the same max floor applies, so the decay manifests once the
	 * target's rise has fallen below the scaled launch.
	 */
	void ApplyHitImpulse(AActor& Target, const FVector& Impulse) const;

	/**
	 * M1-024: one aerial follow-up per target float cycle. Returns true when
	 * the incoming aerial hit on this target must be refused (already landed
	 * an aerial hit in the current leave-ground-to-landing cycle and the
	 * target still floats). The cycle is read lazily from the target: a
	 * grounded target's record belongs to a closed cycle and is dropped here,
	 * so the incoming hit opens a fresh cycle. The refusal is a miss in the
	 * fullest sense: no damage, no impulse, no dedup key, no event.
	 */
	bool ShouldRefuseAerialFollowUp(uint64 TargetId, const AActor& Target);

	/** Injected catalog; UPROPERTY keeps it alive for the GC. */
	UPROPERTY(Transient)
	TObjectPtr<UAttackCatalog> Catalog;

	/**
	 * M3-010: growth attributes injected via SetCombatStats (profile snapshot
	 * final stats in the game owner). Defaults 0/0 keep the pre-M3-010 damage
	 * results verbatim.
	 */
	float CombatAttackPower = 0.0f;
	float CombatDefense = 0.0f;

	/**
	 * M5-014: the attack-side reaction (M5-003) this component's hits carry
	 * into the M5-014 resolver. Configuration, not action state. The
	 * default-constructed value is EControlPenetration::None - the legacy
	 * behavior of every melee attack.
	 */
	FAttackReaction HitAttackReaction;

	/**
	 * M5-014: the target-side reaction policy (M5-003) this component's hits
	 * resolve against. Configuration, not action state. The default is the
	 * legacy normal target (ReactionResolver::MakeLegacyNormalTargetReaction,
	 * the value the M5-013 adaptation hardcoded), so the pre-M5-014 results
	 * stay verbatim.
	 */
	FTargetReaction HitTargetReactionPolicy = MakeLegacyNormalTargetReaction();

	ECombatActionState ActionState = ECombatActionState::Free;

	FName ActiveAttackId = NAME_None;
	uint64 ActiveInstanceId = 0;
	int32 ActiveDurationFrames = 0;
	int32 CurrentFrame = -1;
	int32 Facing = 0;

	/** Next InstanceId minted by a successful start; never rewound mid-session. */
	uint64 NextInstanceId = 1;

	bool bDead = false;
	bool bClockFrozen = false;

	/**
	 * M1-020: input-clock time at which the current HitStun ends (the moment
	 * of the accepted hit plus the stun duration). Only meaningful while the
	 * action state is HitStun; measured on the same explicitly injected input
	 * clock as the buffered input lifetimes.
	 */
	double HitStunEndTimeSeconds = 0.0;

	/**
	 * M1-026: input-clock time at which the current Knockdown flips to
	 * Recovering (the landing moment plus 0.45 s). Only meaningful while the
	 * action state is Knockdown; measured on the same explicitly injected
	 * input clock as the HitStun deadline.
	 */
	double LandingKnockdownEndTimeSeconds = 0.0;

	/**
	 * M1-026: input-clock time at which the current Recovering returns to
	 * Free (the landing moment plus 0.45 s + 0.25 s = 0.70 s). Only
	 * meaningful while the action state is Knockdown or Recovering; fixed at
	 * BeginLandingRecovery so the process always totals 0.70 s.
	 */
	double LandingRecoveringEndTimeSeconds = 0.0;

	/**
	 * M3-031: lifetime anchors for CARRIED buffered inputs - entries pushed
	 * while the buffer already held an earlier intent (the player queued a
	 * press behind one that is still pending). A carried press's 150 ms
	 * lifetime is judged from its anchor instead of its raw press time
	 * (PruneExpiredAnchored), and the anchor is raised on every attack step
	 * that offers the entry no consumption opportunity (RaiseCarriedAnchors,
	 * reached from the closed-window branch of the cancel-window consumers),
	 * so a queued press survives the follow-up attack's pre-window frames and
	 * is judged fresh at the first window step that can actually consume it.
	 * Keyed by the buffered sequence; entries removed from the buffer have
	 * their anchors compacted away; ResetCombat clears the map. The 150 ms
	 * lifetime value itself never changes.
	 */
	TMap<uint64, double> CarriedInputAnchors;

	/**
	 * M3-031: the total injected-clock advancement the running local hit stop
	 * has consumed (each frozen injection's measured advancement plus the
	 * delta path's contribution). The stop's resume applies that span to the
	 * input clock in one jump (the M1-033 fall-through), so presses buffered
	 * across the stop would pay the frozen span out of their lifetimes;
	 * EndHitStop shifts buffered press times and carried anchors by exactly
	 * this span, keeping every lifetime on unfrozen input-clock time only.
	 * Reset to zero when a fresh stop starts and when the stop ends.
	 */
	double HitStopClockJumpSeconds = 0.0;

	FCombatClock Clock;
	FCombatInputBuffer InputBuffer;

	/**
	 * M1-014: input game clock ("now") used for the buffered input lifetime
	 * checks of the cancel-window chaining. Injected via SetInputClockSeconds;
	 * never advanced internally.
	 */
	double InputClockSeconds = 0.0;

	/**
	 * M1-021: true after the first SetInputClockSeconds injection. Gates the
	 * input-driven Free start (TryStartFromBuffer): buffered-input lifetimes
	 * are only judged once the owner supplies the input game clock.
	 */
	bool bInputClockInjected = false;

	/**
	 * M1-023: bound jump request (empty = unbound). The component never jumps
	 * itself; consuming a Jump intent (Free jump or launcher jump-cancel)
	 * invokes this exactly once per consumed entry.
	 */
	FCombatJumpRequestHandler JumpRequestHandler;

	/**
	 * M1-024: bound airborne predicate (empty = grounded). Read at the
	 * Free-state input-driven start to route a buffered Light to aerial_01
	 * (airborne) or light_01 (grounded); the Launcher mapping never changes.
	 */
	FCombatAirStateProvider AirStateProvider;

	/**
	 * M1-041: bound facing source (empty = the stored Facing, 0 for a bare
	 * component). Read at the Free-state input-driven start only; the
	 * cancel-window chaining keeps the running instance's Facing (the game
	 * facing is locked while attacking, so both agree in the game owner).
	 */
	FCombatFacingProvider FacingProvider;

	/** Missing ids already diagnosed; reset when a new catalog is attached. */
	TSet<FName> LoggedMissingAttackIds;

	/**
	 * M5-013: the unified hit identity space of this component (M5-010
	 * ownership, M5-013 adaptation): the registry mints the owner's world
	 * entity id, every hit target's id and the per-instance public
	 * ActionSequence (FShotId); the bound ledger is the dedup face of the
	 * unified entry. Per-attacker instances are a deliberate M5-013 placement
	 * (the World-level home of the registry/ledger pair belongs to the later
	 * integration task): dedup keys never cross ledgers, so the per-instance
	 * once-per-target semantics of the retired M1-019 instance hit set are
	 * preserved exactly.
	 */
	FCombatEntityRegistry UnifiedEntityRegistry;

	/** M5-013: the M5-010 hit ledger bound to UnifiedEntityRegistry (the unified entry's dedup face). */
	FHitLedger UnifiedHitLedger;

	/**
	 * M5-013: the running instance's public ActionSequence slot and the source
	 * entity it was allocated for. Allocated lazily on the first active window
	 * frame that reaches the unified entry (the attack start can happen on a
	 * bare component without a world, where no registry source exists yet);
	 * cleared by EndUnifiedShot with the instance teardown.
	 */
	FShotId ActiveShotId = InvalidCombatShotId;
	FEntityId ActiveShotSourceEntityId = InvalidCombatEntityId;

	/**
	 * M5-013: unified entity id cache per registered actor (the owner on its
	 * first unified hit attempt, every hit target on its first hit attempt).
	 * Entries are verified against the registry record before reuse; entries
	 * of destroyed actors stay until their address is reused (a bounded,
	 * session-lifetime bookkeeping map).
	 */
	TMap<AActor*, FEntityId> UnifiedEntityIdCache;

	/**
	 * M1-024: targets that already took an aerial follow-up in their current
	 * float cycle (keys are the target actors' session-unique object ids).
	 * An entry blocks further aerial hits on that target until it reads
	 * grounded again (dropped lazily at the next aerial hit attempt) or the
	 * component resets. Deliberately NOT part of ClearInstance: the cycle
	 * spans attack instances.
	 */
	TSet<uint64> AerialFollowUpTargetIds;

	/**
	 * M1-019: injected feet-origin provider; empty means the default
	 * owner-based derivation (ResolveFeetLocation).
	 */
	FCombatFeetLocationProvider FeetLocationProvider;

	/** M1-019: stable in-session attacker id minted once at construction. */
	uint64 CachedInstigatorId = 0;

	/** M1-033: true while a local hit stop owns this component's freeze state. */
	bool bHitStopActive = false;

	/** M1-033: real-time remainder of the running hit stop. */
	double HitStopRemainingSeconds = 0.0;

	/**
	 * M1-033: per-frame dedup between the two advancement sources (an injected
	 * input clock value and the TickCombat delta measure the same game frame):
	 * set by a freeze-window injection, cleared by the next TickCombat, so one
	 * frame consumes the remainder exactly once.
	 */
	bool bHitStopConsumedInjectedAdvance = false;

	/** M1-033: last injected input clock value (the freeze-window delta base). */
	double LastInjectedInputClockSeconds = 0.0;

	/** M1-033: true while the owner character's movement is frozen by the stop. */
	bool bOwnerMovementFrozenByHitStop = false;

	/** M1-033: movement state saved once at the freeze start, restored at the end. */
	FVector HitStopSavedVelocity = FVector::ZeroVector;
	FVector HitStopSavedPendingLaunchVelocity = FVector::ZeroVector;
	TEnumAsByte<EMovementMode> HitStopSavedMovementMode = MOVE_None;
};
