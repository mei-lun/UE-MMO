#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"

#include "ItemDefinition.h"

#include "../Profile/ExperienceCurve.h"

/**
 * M3-005: final stat recalculation from base + equipped gear (interface
 * contract section 8: "FStatCalculator sums base and equipment completely" -
 * never incremental add/remove bookkeeping). This type was MIGRATED here from
 * Profile/ExperienceCurve.h, where M3-006 had seeded it with the
 * FLevelBaseStats overload below; that overload moved verbatim (same plain
 * component sum) so the unmodifiable M3-006 tests keep their meaning. The old
 * definition in ExperienceCurve.h was deleted in the same change - there is
 * exactly ONE FStatCalculator in the module (no ODR twin).
 *
 * The M3-005 entry point is the FItemStats overload: it recomputes the WHOLE
 * final row from scratch on every call (base row + the sum of every equipped
 * entry), so repeated equip/unequip cycles can never accumulate drift.
 *
 * Pure logic only: no World, no UObject, no clock. It never writes a
 * HealthComponent; the CurrentHP clamp on a MaxHP change is offered as the
 * separate pure ClampHealthOnMaxChange and its CALL timing belongs to the
 * future wiring task (M3-010) - recalculating stats must never auto-heal.
 *
 * Hardening rules (owned by this card):
 * - An equipped entry with ANY non-finite field (NaN or +/-Inf) is rejected
 *   as a whole: a poisoned entry has no trustworthy magnitude, so instead of
 *   guessing it contributes nothing.
 * - A finite NEGATIVE field is clamped to 0 per field (a negative grant is
 *   treated as corrupted data of that one field, never as a stat drain).
 * - The base row is never rejected (a final answer must exist); its
 *   non-finite/negative fields are sanitized to 0 per field, same rule.
 * - Sums accumulate in double and clamp into float range, so large inputs
 *   saturate at MAX_flt instead of overflowing to Inf.
 * - The final MaxHP has a floor of 1: even an all-zero row yields a living
 *   character's one-hit pool, never a zero or negative max HP.
 */
struct FStatCalculator
{
	/**
	 * M3-005 complete recalculation: FinalStats = BaseStats + the sum of every
	 * usable equipped entry, recomputed whole on every call. See the struct
	 * comment for the hardening rules; the final MaxHP is floored at 1.
	 */
	static FItemStats Recalculate(const FItemStats& BaseStats,
		const TArray<FItemStats>& EquippedStats);

	/**
	 * M3-006 overload, migrated verbatim from Profile/ExperienceCurve.h: the
	 * plain component sum of the integer level-curve row plus an equipment
	 * bonus (default zero). Kept exactly as seeded - no clamping, no floor -
	 * because the level curve's rows are always valid design-table values and
	 * the unmodifiable M3-006 tests pin this exact behavior.
	 */
	static FLevelBaseStats Recalculate(const FLevelBaseStats& LevelBase,
		const FLevelBaseStats& EquipmentBonus = FLevelBaseStats());

	/**
	 * CurrentHP semantics on a MaxHP change (card requirement): returns
	 * min(CurrentHP, NewMax). LOWERING the max correctly clamps the pool;
	 * RAISING the max never restores already-lost health (no free heal).
	 * Hardening: a non-finite current pool reads as 0 (NaN comparisons are
	 * always false, so a raw min() would leak NaN through); a non-finite or
	 * negative NewMax falls back to the old max while that is itself a legal
	 * positive number, else to 0 - garbage max data can never raise the pool.
	 */
	static float ClampHealthOnMaxChange(float CurrentHP, float OldMax, float NewMax);

	/**
	 * Change detection between two final rows (pure). Exact float comparison
	 * for finite fields: a complete recalculation from identical inputs is
	 * deterministic, so bit-equal rows mean "nothing to notify". Any NaN
	 * reports as changed (NaN never compares equal), so a poisoned value can
	 * never be silently swallowed as "unchanged".
	 */
	static bool StatsChanged(const FItemStats& Previous, const FItemStats& Current);
};

/**
 * Native single-cast delegate for the one-shot stats-changed notification
 * (carries the new final row). Bound by UI/gameplay listeners; executed by
 * FStatChangeTracker at most once per ACTUAL change, so a repeated identical
 * recalculation can never storm the UI with events.
 */
DECLARE_DELEGATE_OneParam(FOnFinalStatsChanged, const FItemStats& /*NewFinal*/);

/**
 * M3-005 change-tracking front end: the "notify at most once" owner. The
 * future wiring (M3-010) feeds every fresh final row into ApplyRecalculation;
 * the tracker compares it against the last committed row (FStatCalculator::
 * StatsChanged), commits it either way, and only when it really differs -
 * including the very first report, which has no previous row - executes the
 * bound FOnFinalStatsChanged once and reports true. The caller broadcasts
 * ONLY through this return/delegate path, which is what keeps one logical
 * change to one notification (no UI event storm).
 */
struct FStatChangeTracker
{
	/** Bindable listener; executed at most once per actual change. */
	FOnFinalStatsChanged OnStatsChanged;

	/**
	 * Commits FreshFinalStats as the new tracked row. Returns true (and
	 * executes OnStatsChanged once, when bound) only when the row differs
	 * from the previously committed one or no row was committed yet; returns
	 * false - and stays silent - for an unchanged repeat recalculation.
	 */
	bool ApplyRecalculation(const FItemStats& FreshFinalStats)
	{
		const bool bChanged = !bHasLast || FStatCalculator::StatsChanged(LastFinal, FreshFinalStats);
		LastFinal = FreshFinalStats;
		bHasLast = true;
		if (bChanged && OnStatsChanged.IsBound())
		{
			OnStatsChanged.Execute(FreshFinalStats);
		}
		return bChanged;
	}

	/** The most recently committed row; meaningless until the first apply. */
	const FItemStats& GetLastFinal() const
	{
		return LastFinal;
	}

	/** True once at least one row was committed (first report always notifies). */
	bool HasSnapshot() const
	{
		return bHasLast;
	}

	/** Forgets the tracked row; the next ApplyRecalculation reports as changed. */
	void Reset()
	{
		LastFinal = FItemStats();
		bHasLast = false;
	}

private:
	/** Last committed final row (the "previous" side of the comparison). */
	FItemStats LastFinal;

	/** False until the first ApplyRecalculation committed a row. */
	bool bHasLast = false;
};
