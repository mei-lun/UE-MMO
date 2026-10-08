#include "BurstFirePolicy.h"

void FBurstFirePolicy::SetBurstCount(int32 InBurstCount)
{
	// M5-024 stub: the value is recorded for the red run; the real burst
	// state machine lands with the green implementation.
	BurstCount = FMath::Clamp(InBurstCount, 1, 64);
}

void FBurstFirePolicy::SetSubShotIntervalSeconds(double IntervalSeconds)
{
	// M5-024 stub: recorded only.
	SubShotIntervalSeconds = (FMath::IsFinite(IntervalSeconds) && IntervalSeconds > 0.0) ? IntervalSeconds : 0.0;
}

void FBurstFirePolicy::NotifyTriggerPressed()
{
	// M5-024 stub: no burst arming yet.
}

void FBurstFirePolicy::NotifyTriggerReleased()
{
	// M5-024 stub: no state to clear yet.
}

void FBurstFirePolicy::CancelPendingBurst()
{
	// M5-024 stub: no pending burst yet.
}

bool FBurstFirePolicy::HasPendingBurst() const
{
	// M5-024 stub: never pending.
	return false;
}

FCapacityReservation FBurstFirePolicy::ReserveShotCapacity(const FShotContext& Candidate)
{
	// M5-024 stub: every reservation is refused so the directed suite records
	// the red baseline; the real implementation replaces this body only.
	FCapacityReservation Refusal;
	Refusal.RejectDetail = TEXT("the burst-fire policy stub refuses every reservation");
	return Refusal;
}

void FBurstFirePolicy::CommitShot(const FShotContext& Committed)
{
	// M5-024 stub: no burst bookkeeping yet.
}

void FBurstFirePolicy::AbortReservation(const FShotContext& Aborted)
{
	// M5-024 stub: nothing to roll back yet.
}

void FBurstFirePolicy::NotifyShotReleased(const FShotContext& Ended)
{
	// M5-024 stub: no state to clear yet.
}
