#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"
#include "Engine/DataAsset.h"

#include "EnemyDefinition.generated.h"

/**
 * Data-driven definition of one enemy kind (interface contract section 7).
 * Pure data: units are part of the field names (HP, cm/s speeds, cm distances,
 * seconds) and seconds are never mixed with frames. This card (M2-001) ships
 * the definition, its JSON source data and the single-entry validation only;
 * enemy AI behavior, room wiring and the new visual representation belong to
 * later M2 tasks. Melee attacks reuse the M1 pipeline: MeleeAttackId names an
 * attack_id of Data/combat-attacks.json and the attack flows through the M1
 * combat components, never a second direct health-removal path.
 */
UCLASS(BlueprintType)
class UEnemyDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Stable identifier of this enemy kind (e.g. "melee_grunt"). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Identity")
	FName EnemyId;

	/** Health pool handed to the enemy's UHealthComponent; finite and > 0. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Stats")
	float MaxHP = 60.0f;

	/** Attack power fed into the M1-019 damage formula; finite and >= 0. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Stats")
	float AttackPower = 0.0f;

	/** Approach speed in cm/s; finite and > 0 (a melee enemy must close in). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Movement")
	float MoveSpeed = 220.0f;

	/** X distance in cm close enough to start an attack; finite and > 0. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Combat")
	float AttackRangeX = 160.0f;

	/** Y depth alignment tolerance in cm; finite and > 0 (zero can never align). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Combat")
	float AlignYTolerance = 35.0f;

	/** Attack wind-up (telegraph) duration in seconds; finite and >= 0. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Combat")
	float TelegraphSeconds = 0.35f;

	/** Attack ban window after spawn in seconds; finite and >= 0. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Combat")
	float SpawnGraceSeconds = 0.5f;

	/** Attack id inside the M1 catalog (Data/combat-attacks.json); must be non-empty. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Combat")
	FName MeleeAttackId;

	/**
	 * Game clock value of the most recent spawn of this definition instance,
	 * injected by callers through MarkSpawned (interface contract section 2:
	 * tests and early systems pass explicit times; no wall clock is read).
	 * Runtime state, not source data: it never participates in validation.
	 * 0.0 until the first mark, which behaves like a spawn at time zero.
	 */
	double SpawnedAtGameSeconds = 0.0;

	/** Records the spawn instant on the caller's injected game clock. */
	void MarkSpawned(double NowSeconds) { SpawnedAtGameSeconds = NowSeconds; }

	/**
	 * Remaining spawn-grace seconds at the injected game time NowSeconds:
	 * SpawnGraceSeconds minus the time passed since MarkSpawned, clamped at 0.
	 * Pure read: no wall clock, no mutation. Callers pass a monotonic game
	 * clock; a NowSeconds before the spawn instant keeps the full grace.
	 */
	double GetSpawnGraceRemaining(double NowSeconds) const;

	/**
	 * Attack permission from the spawn grace alone at the injected game time:
	 * true only when the grace window [SpawnedAt, SpawnedAt + SpawnGraceSeconds)
	 * has fully elapsed, i.e. GetSpawnGraceRemaining(NowSeconds) == 0. All other
	 * attack conditions (range, Y alignment, telegraph, cooldowns) stay with the
	 * caller and are deliberately not part of this check.
	 */
	bool CanAttack(double NowSeconds) const;
};

/**
 * Validates a single enemy definition in isolation and returns true when legal.
 * On failure OutErrors joins every problem with "; " and each problem names its
 * field (e.g. "EnemyId", "MaxHP", "AlignYTolerance", "MeleeAttackId"). Pure
 * check: it never creates or mutates assets. Cross-entry validation (whether
 * MeleeAttackId resolves to an existing attack of the M1 catalog) is
 * intentionally out of scope here; the later M2 room/catalog tasks can call
 * this per entry and add the existence check on top, mirroring how the M1
 * catalog tasks extend ValidateAttackDefinition.
 */
bool ValidateEnemyDefinition(const UEnemyDefinition& Enemy, FText& OutErrors);
