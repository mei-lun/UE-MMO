#pragma once

#include "CoreMinimal.h"

/**
 * M1-025: decision for one launcher hit against a target's current float
 * cycle (interface contract section 6: the same enemy accepts at most two
 * launcher launches per float cycle - the first rises at the full launch
 * speed, the second at 70 percent, and from the third on the launch impulse
 * is refused while damage and dedup still apply; landing or reset reopens
 * the cycle). Pure value type with no World dependency.
 */
struct FAirComboDecision
{
	/** False when the launch impulse must not be applied at all. */
	bool bAllowed = false;

	/** Multiplier for the attack definition's launch speed; 0.0 when refused. */
	float ZScale = 0.0f;
};

/**
 * Pure policy, no World and no state (the count source stays target-side):
 * LaunchCount is the number of launcher launches already applied to the
 * target in its current float cycle. 0 -> allowed at full scale (1.0),
 * 1 -> allowed at 0.7, 2 or more -> refused (0.0). A negative count is
 * defensively treated as a fresh cycle (no caller produces one).
 */
FAirComboDecision EvaluateAirCombo(int32 LaunchCount);
