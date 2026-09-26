#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "TrainingResetService.generated.h"

class APrototypeCharacter;
class ATrainingEnemy;

/**
 * M1-027: the unified one-key training reset (interface contract section 6:
 * the session reset clears inputs, events, timers, velocity, HP, facing, the
 * airborne counters and the hit sets of every participant, without reloading
 * the map). The service owns the reset ORDER: phase 1 stops every action and
 * timer on all participants first, phase 2 restores the physics and values -
 * so no participant is ever teleported while another one still runs an action
 * that could observe the half-reset room.
 *
 * Participant collection source: explicit registration (RegisterPlayer /
 * RegisterEnemy). Registration keeps the set deterministic for the automation
 * tests and avoids a world scan that would drag in enemies from unrelated
 * spaces of the same world; the production room wiring registers its own
 * participants (the R-key owner injects this service into the pawn through
 * APrototypeCharacter::SetTrainingResetService).
 *
 * Idempotency: every reset step is a teardown or a setter, so calling
 * ResetTrainingSession any number of times in a row leaves a fresh room and
 * never duplicates registrations, timers or actors. The attack instance id
 * counters are deliberately NOT rewound (M1-011: ids stay unique per session).
 */
UCLASS()
class UEMMO_API UTrainingResetService : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Binds the player pawn whose R-key reset routes into this service (and
	 * whose physics state this service resets in phase 2). Registering another
	 * player replaces the previous binding; registering null clears it.
	 */
	void RegisterPlayer(APrototypeCharacter* InPlayer);

	/**
	 * Registers one training enemy into the reset set. Registering the same
	 * enemy twice is ignored (idempotent - repeated setup never duplicates a
	 * participant); destroyed registrations are pruned lazily.
	 */
	void RegisterEnemy(ATrainingEnemy* InEnemy);

	/** Live registered enemy count (stale destroyed registrations pruned). */
	int32 GetRegisteredEnemyCount() const;

	/** True while a live player pawn is registered. */
	bool HasRegisteredPlayer() const;

	/**
	 * How many times ResetTrainingSession actually ran (diagnostics/tests:
	 * one R press is exactly one session reset, never two).
	 */
	int32 GetResetCount() const { return ResetCount; }

	/**
	 * The unified reset entry. Fixed order (interface contract section 6):
	 * phase 1 - stop actions and timers on EVERY participant first (both
	 * combat components: ResetCombat cancels the running attack / hit stun /
	 * knockdown / recovery, drops the instance hit set, clears the buffered
	 * input and zeroes every combat deadline); phase 2 - restore physics and
	 * values (player: spawn position, zero velocity, spawn facing, movement
	 * mode; enemies: ResetEnemy = full HP, spawn anchor transform, zero
	 * velocity, airborne counters cleared, dead flag dropped). No map reload;
	 * safe to call repeatedly.
	 */
	void ResetTrainingSession();

private:
	/** Drops registrations whose actors were destroyed. */
	void PruneStaleEnemies() const;

	/** The R-key owner; weak so a destroyed pawn never dangles the service. */
	mutable TWeakObjectPtr<APrototypeCharacter> Player;

	/** Registered enemies; weak, pruned of stale entries on every access. */
	mutable TArray<TWeakObjectPtr<ATrainingEnemy>> Enemies;

	/** Completed ResetTrainingSession runs (see GetResetCount). */
	int32 ResetCount = 0;
};
