#pragma once

#include "CoreMinimal.h"

/**
 * M1-035: pure display model behind the debug HUD's hit feedback (health bars,
 * floating damage numbers, combo counter). No world, no ticking, no rendering:
 * every value is pushed in from outside (events or explicit snapshots) and the
 * only time source is the explicitly injected clock (interface contract
 * section 2), so the whole model is testable without a world.
 */

/**
 * One floating damage number. Damage is the health the target actually lost
 * (the ApplyDamage return value carried by FCombatHit::Damage) - never the
 * configured base damage, so the HUD can never display an invented number.
 */
struct FDamageNumberEntry
{
	/** Health actually removed by the accepted hit (> 0; from FCombatHit::Damage). */
	float Damage = 0.0f;

	/** Injected-clock value at which the number entered the pool. */
	double SpawnTimeSeconds = 0.0;

	/**
	 * World-space anchor drawn from (FCombatHit::WorldHitLocation, the active
	 * hit box center). Stored in world space and projected at draw time so a
	 * moving camera keeps the number attached to the hit.
	 */
	FVector Location = FVector::ZeroVector;

	/** Attack definition id that landed the hit (NAME_None when unknown). */
	FName AttackId = NAME_None;
};

/**
 * Bounded pool of floating damage numbers. Entries expire after
 * LifeTimeSeconds (measured on the injected clock); adding past MaxEntries
 * evicts the earliest entry, so the pool never grows unbounded and no stored
 * entry can dangle: the pool owns plain data copies, never object references.
 */
class FDamageNumberPool
{
public:
	/** Hard pool cap: adding past it evicts the earliest entry first. */
	static constexpr int32 MaxEntries = 32;

	/** Number lifetime in seconds; an entry at least this old is expired. */
	static constexpr double LifeTimeSeconds = 0.6;

	/** Explicit clock injection (interface contract section 2). Never advances internally. */
	void SetNowSeconds(double NowSeconds);

	double GetNowSeconds() const { return NowSeconds; }

	/**
	 * Records one accepted hit as a floating number stamped with the injected
	 * clock. Prunes expired entries first, then evicts the earliest entry
	 * while the pool is full, then appends.
	 */
	void Add(float Damage, const FVector& Location, FName AttackId = NAME_None);

	/** Removes expired entries. The HUD calls this once per drawn frame. */
	void PruneExpired();

	/** Live entry count (prunes expired entries first). */
	int32 Num();

	/**
	 * Raw view of the stored entries. Call PruneExpired (or Num) first for the
	 * live set; the draw path prunes once per frame before reading this.
	 */
	const TArray<FDamageNumberEntry>& GetEntries() const { return Entries; }

	/** Clears everything (reset flow: numbers start empty). */
	void Clear();

private:
	double NowSeconds = 0.0;
	TArray<FDamageNumberEntry> Entries;
};

/**
 * One health bar's display data (current / max / alive). The HUD feeds it per
 * drawn frame from a real UHealthComponent; the player side has no health
 * component in M1 (M1-016 granted one to enemies only), so it keeps the
 * default bHasData = false and the HUD draws an explicit placeholder instead
 * of invented numbers.
 */
struct FHealthBarData
{
	float CurrentHP = 0.0f;
	float MaxHP = 0.0f;
	bool bAlive = false;
	/** False until the first Set; the HUD draws the no-data placeholder while false. */
	bool bHasData = false;

	/**
	 * Feeds one snapshot. CurrentHP is clamped into [0, MaxHP] when MaxHP is
	 * positive so a racing write can never push the bar outside its frame.
	 */
	void Set(float InCurrentHP, float InMaxHP, bool bInAlive);

	/** Fill ratio in [0, 1]; 0 without data or with a non-positive max. */
	float GetRatio() const;
};

/**
 * Consecutive-hit counter for one attacker. Hits inside TimeoutSeconds of the
 * previous hit increment the combo; once the timeout elapsed the next hit
 * starts a fresh count at 1. Pure logic on the injected clock.
 */
class FComboCounter
{
public:
	explicit FComboCounter(double InTimeoutSeconds = 1.5);

	/**
	 * Records one hit at NowSeconds and returns the running combo: the previous
	 * count + 1 while the hit lands inside the timeout window, 1 otherwise.
	 */
	int32 NotifyHit(double NowSeconds);

	/**
	 * Combo value at NowSeconds: 0 once TimeoutSeconds elapsed since the last
	 * hit (exactly at the boundary counts as timed out), otherwise the count.
	 */
	int32 EvaluateCombo(double NowSeconds) const;

	/** Clears the count and the last-hit time (reset flow). */
	void Reset();

	double GetTimeoutSeconds() const { return TimeoutSeconds; }

private:
	double TimeoutSeconds = 1.5;
	double LastHitTimeSeconds = 0.0;
	int32 ComboCount = 0;
	bool bHasHit = false;
};
