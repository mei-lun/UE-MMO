#pragma once

#include "CoreMinimal.h"
#include "AIController.h"

#include "MeleeEnemyController.generated.h"

class AMeleeEnemy;

/**
 * M2-002 minimal enemy brain state (no behavior tree anywhere on this card:
 * the Tick-driven state machine replaces it deliberately).
 */
UENUM(BlueprintType)
enum class EMeleeEnemyState : uint8
{
	/** No valid target (none injected, destroyed or dead): stand still. */
	Idle = 0,

	/** Chasing the target: depth-first alignment, then X closing. */
	Approach = 1,

	/**
	 * M2-003: attack wind-up. Entered when the attack condition is met; the
	 * attack direction (facing) and the deadline are locked at entry, the
	 * position hold starts here (the target may walk away and the attack
	 * misses). The attack fires only when the deadline elapses.
	 */
	Telegraph = 2,

	/** M2-003: an attack instance is running on this enemy's combat component. */
	Attack = 3,

	/** M2-003: rest after a finished attack instance before approaching again. */
	Recover = 4
};

// ---------------------------------------------------------------------------
// Pure M2-002 rule functions (unit-testable without a world; the controller
// tick and the enemy facing entry are thin applications of these).
// ---------------------------------------------------------------------------

/**
 * M2-002 approach rule: depth first, then X. Returns a unit world-space move
 * intent (or zero):
 *  - while |TargetY - EnemyY| > AlignYTolerance the intent is pure Y toward
 *    the target (no matter how large the X offset is - never a diagonal);
 *  - once depth-aligned (|DeltaY| <= AlignYTolerance) the intent is pure X
 *    toward the target while |DeltaX| > AttackRangeX;
 *  - aligned and inside the attack range: zero intent (hold position; the
 *    attack itself belongs to M2-003).
 */
UEMMO_API FVector ComputeMeleeApproachIntent(const FVector& EnemyLocation, const FVector& TargetLocation, float AttackRangeX, float AlignYTolerance);

/**
 * M2-003 attack-condition rule: true when the target stands inside the melee
 * attack window - |DeltaX| <= AttackRangeX and |DeltaY| <= AlignYTolerance.
 * Boundaries count as satisfied, mirroring the hold band of
 * ComputeMeleeApproachIntent (the same geometry that makes the approach
 * intent zero is the geometry that starts the wind-up).
 */
UEMMO_API bool ComputeMeleeAttackReady(const FVector& EnemyLocation, const FVector& TargetLocation, float AttackRangeX, float AlignYTolerance);

/**
 * M2-002 separation rule: keep at least MinSeparation (cm, horizontal XY)
 * between melee enemies. Returns the summed positional correction (cm) that
 * pushes the self location away from every other enemy closer than
 * MinSeparation (each neighbor contributes a vector of magnitude
 * MinSeparation - Distance pointing away from it). Neighbors at or beyond the
 * minimum contribute nothing; an exact overlap contributes a deterministic
 * +X fallback of MinSeparation (documented limitation: two enemies spawned at
 * the very same spot receive the same fallback and cannot separate by
 * position alone - spawn points differ in practice).
 */
UEMMO_API FVector ComputeMeleeSeparationAdjustment(const FVector& SelfLocation, const TArray<FVector>& OtherEnemyLocations, float MinSeparation);

/**
 * M2-002 room-bound rule for a move intent: an invalid (unset) box passes the
 * intent through unchanged; a location outside the configured rectangle gets
 * a full stop (zero intent - "beyond the field boundary, stop"); inside, any
 * intent component that would cross a bound (1 cm lookahead on the unit
 * intent) is zeroed so the enemy never steers out of the field.
 */
UEMMO_API FVector FilterMeleeIntentByRoomBounds(const FVector& EnemyLocation, const FVector& MoveIntent, const FBox& RoomBounds);

/**
 * M2-002 facing rule: only an X intent component turns the enemy (yaw 0 for
 * +X, yaw 180 for -X); a pure Y (depth) or zero intent keeps CurrentYaw. The
 * enemy therefore never faces the camera direction (which looks along Y) or
 * the raw diagonal to its target.
 */
UEMMO_API float ComputeMeleeFacingYaw(const FVector& MoveIntent, float CurrentYaw);

/**
 * M2-002: Tick-driven melee enemy brain (AAIController subclass, but no
 * behavior tree and no path following: the minimal Tick state machine steers
 * its AMeleeEnemy pawn with AddMovementInput through the normal
 * CharacterMovement so the approach uses real walking physics and real
 * collision).
 *
 * States this card: Idle (no valid target) and Approach. The target is a
 * weak reference injected through SetTarget (tests now, the M2 spawner
 * later); a destroyed or dead target drops the controller back to Idle and
 * is never dereferenced. The approach rule is the pure
 * ComputeMeleeApproachIntent (depth first, then X), filtered by the
 * configurable rectangular room bounds, plus the pure separation rule
 * (ComputeMeleeSeparationAdjustment) so up to three enemies never stack on
 * the same point. Facing is applied to the pawn through
	 * AMeleeEnemy::ApplyFacingIntent (only +/-X while approaching).
 */
UCLASS()
class UEMMO_API AMeleeEnemyController : public AAIController
{
	GENERATED_BODY()

public:
	AMeleeEnemyController();

	/**
	 * Injects the chase target as a weak reference (tests / the later
	 * spawner). Passing nullptr clears the target (back to Idle on the next
	 * tick). The target is resolved fresh every tick and never dereferenced
	 * when the weak reference is stale.
	 */
	void SetTarget(AActor* InTarget);

	/**
	 * Configurable rectangular play field: while the pawn is inside, movement
	 * input is filtered so it never steers across a bound; a pawn outside
	 * stops (M2-002 "beyond the field boundary, stop"). An unset (invalid)
	 * box disables the rule entirely.
	 */
	void SetRoomBounds(const FBox& InRoomBounds) { RoomBounds = InRoomBounds; }

	/**
	 * Overrides the contract-default combat parameters used when the pawn
	 * carries no UEnemyDefinition. A definition applied to the pawn
	 * (AMeleeEnemy::SetEnemyDefinition) always wins over these.
	 */
	void SetCombatParameters(float InAttackRangeX, float InAlignYTolerance);

	/** Minimum horizontal distance kept to other melee enemies (cm). */
	void SetMinSeparation(float InMinSeparation) { MinSeparation = InMinSeparation; }

	/** Current brain state (Idle until a valid target exists). */
	EMeleeEnemyState GetState() const { return State; }

	/**
	 * Diagnostics/tests: the injected chase target, resolved out of the weak
	 * reference (null when none was injected or the target is stale). Never
	 * keeps the target alive.
	 */
	AActor* GetTargetActor() const { return TargetWeak.Get(); }

	/**
	 * M2-003 diagnostics/tests: the injected telegraph-clock value at which
	 * the current state was entered (the same clock the Telegraph/Recover
	 * deadlines are measured on). 0.0 before the first injection.
	 */
	double GetStateEnteredGameSeconds() const { return StateEnteredSeconds; }

	/**
	 * Diagnostics/tests: the approach move intent produced by the last tick
	 * (after room-bound filtering, before separation), zero in Idle.
	 */
	const FVector& GetLastMoveIntent() const { return LastMoveIntent; }

	virtual void Tick(float DeltaSeconds) override;

private:
	/**
	 * Resolves the weak target reference for this tick: returns true only
	 * when the reference is alive (not stale/pending-kill) and the target is
	 * not dead (its UCombatComponent, when present, reports dead). On true it
	 * fills OutTargetLocation; the stale object is never dereferenced.
	 */
	bool ResolveTarget(FVector& OutTargetLocation) const;

	/** Other AMeleeEnemy locations in the world (for the separation rule). */
	TArray<FVector> CollectOtherMeleeEnemyLocations(const AMeleeEnemy* Self) const;

	// ------------------------------------------------------------------
	// M2-003 telegraph/recover state machine helpers.
	// ------------------------------------------------------------------

	/**
	 * M2-003: explicit game-clock injection for the Telegraph/Recover
	 * countdown (interface contract section 2 semantics, the M1 per-frame
	 * injection pattern). The controller injects the World GetTimeSeconds
	 * game clock once per tick, BEFORE the state machine reads it; no wall
	 * clock or real-time timer is read anywhere in the countdown.
	 */
	void InjectTelegraphClockSeconds(double NowSeconds) { TelegraphClockSeconds = NowSeconds; }

	/** M2-003: switches the brain state and records the entry clock value. */
	void EnterState(EMeleeEnemyState NewState, double NowSeconds);

	/**
	 * M2-003: collapses any timed state (Telegraph wind-up, Attack wait,
	 * Recover rest) to Idle and restores the telegraph presentation. Called
	 * when the self is dead or the target is gone: the wind-up timer is
	 * dropped so no delayed attack can ever fire from it.
	 */
	void CancelTimedState(AMeleeEnemy& Enemy, double NowSeconds);

	/**
	 * M2-003: drives the Telegraph wind-up (deadline check, interrupt
	 * cancel, attack start through the enemy's own combat component).
	 */
	void TickTelegraphState(AMeleeEnemy& Enemy, double NowSeconds, bool bSelfCombatBusy, FName EffectiveAttackId);

	/**
	 * M2-003: drives the Attack wait (hold while the instance runs, recover
	 * when it finishes, plain resume when it was interrupted).
	 */
	void TickAttackState(double NowSeconds, bool bSelfCombatBusy, bool bSelfFree);

	/**
	 * M2-003: drives the Recover rest (hold until the recover end, then
	 * back to the approach loop).
	 */
	void TickRecoverState(double NowSeconds, bool bSelfCombatBusy);

	/**
	 * M2-003: applies/clears the telegraph presentation on the pawn, deduped
	 * so repeated ticks never re-issue the same request.
	 */
	void SetTelegraphVisual(AMeleeEnemy& Enemy, bool bActive);

	/** Weak target reference injected by SetTarget. */
	TWeakObjectPtr<AActor> TargetWeak;

	EMeleeEnemyState State = EMeleeEnemyState::Idle;
	FVector LastMoveIntent = FVector::ZeroVector;

	/** Contract defaults (UEnemyDefinition field defaults, M2-001). */
	float AttackRangeX = 160.0f;
	float AlignYTolerance = 35.0f;
	float MinSeparation = 80.0f;

	/** Configurable rectangular play field; invalid box = rule disabled. */
	FBox RoomBounds = FBox(ForceInit);

	// ---------------- M2-003 telegraph/recover bookkeeping ----------------

	/** Telegraph duration default when the pawn carries no UEnemyDefinition. */
	static constexpr float DefaultTelegraphSeconds = 0.35f;

	/** Recovery rest duration after a finished attack instance (contract). */
	static constexpr float RecoverSeconds = 0.5f;

	/** Injected game clock ("now") the Telegraph/Recover deadlines use. */
	double TelegraphClockSeconds = 0.0;

	/** Injected-clock value at which the current state was entered. */
	double StateEnteredSeconds = 0.0;

	/** Telegraph deadline: the attack fires at/after this clock value. */
	double TelegraphDeadlineSeconds = 0.0;

	/** Recover end: the approach loop resumes at/after this clock value. */
	double RecoverEndTimeSeconds = 0.0;

	/** Attack facing locked at the telegraph start (+1 right / -1 left). */
	int32 LockedFacing = 0;

	/** Current telegraph presentation state (dedups the visual requests). */
	bool bTelegraphVisualActive = false;
};
