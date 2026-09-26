#include "AirComboPolicy.h"

namespace
{
	// Interface contract section 6 (unified ambiguity resolution): at most two
	// launcher launches per target float cycle. The first rises at the full
	// definition launch speed, the second at 70 percent, and from the third on
	// the launch impulse is refused while damage and dedup still apply. Both
	// scales are design initial values pending playtest tuning.
	constexpr int32 M1_025_MaxLauncherLaunchesPerCycle = 2;
	constexpr float M1_025_FirstLaunchZScale = 1.0f;
	constexpr float M1_025_SecondLaunchZScale = 0.7f;
}

FAirComboDecision EvaluateAirCombo(const int32 LaunchCount)
{
	FAirComboDecision Decision;
	if (LaunchCount >= M1_025_MaxLauncherLaunchesPerCycle)
	{
		// Third (and later) launcher of the cycle: the launch reaction is
		// refused; damage and dedup stay with the hit site. Default values
		// already carry bAllowed=false / ZScale=0.0.
		return Decision;
	}
	// A negative count has no producer; defensively read as a fresh cycle.
	Decision.bAllowed = true;
	Decision.ZScale = (LaunchCount >= 1) ? M1_025_SecondLaunchZScale : M1_025_FirstLaunchZScale;
	return Decision;
}
