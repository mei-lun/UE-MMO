#include "CombatClock.h"

#include "Math/UnrealMathUtility.h"
#include "Logging/LogMacros.h"

int32 FCombatClock::Advance(double DeltaSeconds, bool bFrozen)
{
	if (bFrozen)
	{
		// Frozen frames drop their delta entirely: nothing is accumulated,
		// the backlog is not drained, and there is no catch-up once the
		// freeze lifts.
		return 0;
	}
	if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds < 0.0)
	{
		// Negative and non-finite (NaN/Inf) deltas are ignored outright:
		// no accumulation, no steps, and any existing backlog is left
		// untouched for the next valid Advance call.
		return 0;
	}

	AccumulatedSeconds += DeltaSeconds;

	int32 Steps = 0;
	while (Steps < MaxStepsPerAdvance && AccumulatedSeconds >= StepSeconds)
	{
		AccumulatedSeconds -= StepSeconds;
		++Steps;
	}

	if (Steps == MaxStepsPerAdvance && AccumulatedSeconds >= StepSeconds)
	{
		// Long-frame diagnostic: the backlog was truncated by the step cap
		// and carries over to the next Advance call.
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO FCombatClock: long frame capped at %d steps; %.6fs of backlog carried over"),
			Steps, AccumulatedSeconds);
	}

	return Steps;
}

void FCombatClock::Reset()
{
	AccumulatedSeconds = 0.0;
}
