#pragma once

#include "CoreMinimal.h"

/**
 * Fixed 60 Hz action clock for combat logic.
 * Advance(DeltaSeconds, bFrozen) accumulates the passed delta into a double
 * accumulator and returns how many integer logic steps (1/60 s each) elapsed
 * during this call (0 or more).
 * At most 8 steps run per Advance call; unconsumed backlog is kept in the
 * accumulator and survives until later calls drain it, so no time is lost.
 * A frozen frame discards its delta entirely: nothing is accumulated and the
 * backlog is not drained, so a freeze is never caught up after it lifts.
 * Negative and non-finite (NaN/Inf) deltas are ignored: no accumulation,
 * no steps, no crash; the accumulator is left untouched.
 * This clock only drives combat action logic; it makes no promise of physics
 * determinism. All time comes from the explicit delta passed by callers;
 * wall clocks are never read internally.
 */
class FCombatClock
{
public:
	/**
	 * Adds DeltaSeconds to the accumulator and runs up to 8 whole 60 Hz steps,
	 * returning the number of steps executed by this call (0 when frozen, or
	 * when the delta is negative or non-finite).
	 */
	int32 Advance(double DeltaSeconds, bool bFrozen);

	/** Clears the accumulated backlog. */
	void Reset();

private:
	static constexpr double StepSeconds = 1.0 / 60.0;
	static constexpr int32 MaxStepsPerAdvance = 8;

	double AccumulatedSeconds = 0.0;
};
