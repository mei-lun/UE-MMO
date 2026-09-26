#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Delegates/DelegateCombinations.h"
#include "Templates/Function.h"

#include "CombatClock.h"
#include "CombatHitTypes.h"
#include "CombatInputBuffer.h"

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
	HitStun = 2
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
 * Injectable feet-origin source (interface contract section 5: the feet
 * location is the capsule center minus the capsule half height). The default
 * derivation reads the owner (capsule half height for characters, the actor
 * location as feet for any other root); tests can pin an explicit origin.
 */
using FCombatFeetLocationProvider = TFunction<FVector()>;

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
	 * (input-driven start; inert until the input clock was injected once).
	 * The frozen flag (hit stop and cutscene freezes belong to later tasks) is
	 * passed straight through to the clock: a frozen frame drops its delta and
	 * advances no frame.
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
	 * gone) marks the component dead instead of stunning it.
	 */
	void NotifyHitReceived(const FCombatHit& Hit);

	FCombatSnapshot GetSnapshot() const;

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

private:
	/**
	 * M1-014: when the running attack is inside its cancel window, prunes
	 * expired buffered inputs and switches into the earliest buffered entry
	 * whose action maps to a follow-up the running attack allows (M1-021
	 * generalized the M1-014 Light-only rule to the input type: a Light maps
	 * to light_02, a Launcher to launcher; Jump stays for M1-023). Inputs that
	 * cannot chain (other actions, follow-up not allowed, expired, missing
	 * follow-up definition) stay buffered. Returns true when the running
	 * instance changed, so the caller stops advancing the old timeline for
	 * this tick.
	 */
	bool TryChainFromBuffer();

	/**
	 * M1-021: while Free and alive, prunes expired buffered inputs and starts
	 * the earliest buffered Light/Launcher whose mapped Free attack id exists
	 * in the catalog (Light -> light_01, Launcher -> launcher; Jump stays for
	 * M1-023). Consumes exactly one entry per tick, so one press starts one
	 * attack and can never fire twice. Inert until the owner injected the
	 * input clock at least once (SetInputClockSeconds): without an injected
	 * clock the component has no valid "now" to judge input lifetimes with.
	 * Returns true when an attack started.
	 */
	bool TryStartFromBuffer();

	/** Clears the running instance (not the id counter) back to Free defaults. */
	void ClearInstance();

	/** M1-020: leaves HitStun back to Free (no-op unless currently stunned). */
	void EndHitStun();

	/** Ends the current attack: clears state first, then broadcasts OnFinished once. */
	void FinishCurrentAttack();

	/**
	 * M1-019: when the current frame sits inside the running attack's active
	 * window, queries the hit box targets and applies one deduplicated damage
	 * per target (dedup key = InstigatorId/AttackInstanceId/HitGroupId/
	 * TargetId, recorded only on an accepted ApplyDamage). Safe no-op without
	 * a catalog, definition, owner or world; stale and dead targets skip.
	 */
	void TryApplyActiveWindowHits();

	/** M1-019: resolves the current feet origin (provider first, owner second). */
	FVector ResolveFeetLocation() const;

	/** M1-019: applies the hit impulse to a surviving target, skipping bodies that cannot move. */
	void ApplyHitImpulse(AActor& Target, const FVector& Impulse) const;

	/** Injected catalog; UPROPERTY keeps it alive for the GC. */
	UPROPERTY(Transient)
	TObjectPtr<UAttackCatalog> Catalog;

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

	/** Missing ids already diagnosed; reset when a new catalog is attached. */
	TSet<FName> LoggedMissingAttackIds;

	/**
	 * M1-019: hit keys already accepted by the running attack instance. The
	 * set is cleared together with the instance (finish, chain switch, reset),
	 * so the next instance can hit the same target again.
	 */
	TSet<FCombatHitDedupKey> InstanceHitKeys;

	/**
	 * M1-019: injected feet-origin provider; empty means the default
	 * owner-based derivation (ResolveFeetLocation).
	 */
	FCombatFeetLocationProvider FeetLocationProvider;

	/** M1-019: stable in-session attacker id minted once at construction. */
	uint64 CachedInstigatorId = 0;
};
