#pragma once

#include "CoreMinimal.h"

/**
 * Half-open frame window [StartFrame, EndFrame) used by combat actions.
 * Crosses inspects the integer frames advanced over, i.e. the range (PreviousFrame, CurrentFrame].
 * The initial PreviousFrame is -1; re-checking the same frame must not re-trigger.
 * The window stays half-open by contract; do not turn it into a closed interval.
 */
struct FCombatWindow
{
	int32 StartFrame = 0;
	int32 EndFrame = 0;

	/** True when the window is non-empty and fits inside [0, Duration]. */
	bool IsValid(int32 Duration) const
	{
		return StartFrame >= 0 && EndFrame > StartFrame && EndFrame <= Duration;
	}

	/** True when Frame lies inside the half-open window [StartFrame, EndFrame). */
	bool Contains(int32 Frame) const
	{
		return StartFrame <= Frame && Frame < EndFrame;
	}

	/**
	 * True when at least one integer frame F with PreviousFrame < F <= CurrentFrame
	 * lies inside the window. Non-forward progress (CurrentFrame <= PreviousFrame)
	 * never triggers. The check is O(1): the advanced frames intersect the window
	 * exactly when max(StartFrame, PreviousFrame + 1) <= min(CurrentFrame, EndFrame - 1).
	 */
	bool Crosses(int32 PreviousFrame, int32 CurrentFrame) const
	{
		if (CurrentFrame <= PreviousFrame)
		{
			return false;
		}
		const int32 First = FMath::Max(StartFrame, PreviousFrame + 1);
		const int32 Last = FMath::Min(CurrentFrame, EndFrame - 1);
		return First <= Last;
	}
};
