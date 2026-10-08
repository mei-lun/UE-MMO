#include "BurstFirePolicy.h"

void FBurstFirePolicy::SetBurstCount(int32 InBurstCount)
{
	BurstCount = FMath::Clamp(InBurstCount, 1, 64);
}

void FBurstFirePolicy::SetSubShotIntervalSeconds(double IntervalSeconds)
{
	SubShotIntervalSeconds = (FMath::IsFinite(IntervalSeconds) && IntervalSeconds > 0.0) ? IntervalSeconds : 0.0;
}

void FBurstFirePolicy::NotifyTriggerPressed()
{
	// One press edge arms one burst: a press while a burst is pending or
	// already armed is ignored ("重复press或长按不叠加两串"). Only a fresh
	// edge re-arms after the burst completes.
	if (PendingSubShots == 0 && !bPressArmed && !bFirstSubShotReserved)
	{
		bPressArmed = true;
	}
}

void FBurstFirePolicy::NotifyTriggerReleased()
{
	// A release before the first sub-shot disarms the burst; a started burst
	// runs to its end ("已开始点射松键可继续").
	if (PendingSubShots == 0 && !bFirstSubShotReserved)
	{
		bPressArmed = false;
	}
}

void FBurstFirePolicy::CancelPendingBurst()
{
	// Death/switch/reload duty of the caller: the stale remainder never
	// resumes ("中断不补发") - only a fresh press edge starts a new burst.
	bPressArmed = false;
	bFirstSubShotReserved = false;
	PendingSubShots = 0;
	NextSubShotSeconds = 0.0;
}

bool FBurstFirePolicy::HasPendingBurst() const
{
	return PendingSubShots > 0 || bFirstSubShotReserved || bPressArmed;
}

FCapacityReservation FBurstFirePolicy::ReserveShotCapacity(const FShotContext& Candidate)
{
	FCapacityReservation Reservation;

	// The burst policy owns the hitscan delivery only: projectile pacing and
	// capacity belong to the 025 multi-pellet policy.
	if (Candidate.FireMode != EWeaponFireMode::Hitscan)
	{
		Reservation.RejectDetail = FString::Printf(
			TEXT("the burst-fire policy grants hitscan shots only (the candidate is mode %d)"),
			static_cast<int32>(Candidate.FireMode));
		return Reservation;
	}

	// The candidate's commit time IS the component's injected fire clock
	// (GameClockSeconds semantics): a pause freezes the burst in place (no
	// catch-up) and a stalled frame admits at most the single due sub-shot.
	const double Now = Candidate.CommittedAtSeconds;

	// A started burst streams its remaining sub-shots, one per admission,
	// no earlier than the interval anchor laid at the previous commit (0
	// defers the sub-shot pacing to the component's fire-rate cooldown).
	if (PendingSubShots > 0)
	{
		if (Now < NextSubShotSeconds)
		{
			Reservation.RejectDetail = FString::Printf(
				TEXT("the next burst sub-shot is due at %.6f (now %.6f)"), NextSubShotSeconds, Now);
			return Reservation;
		}
		Reservation.bGranted = true;
		Reservation.RaycastSlots = 1;
		Reservation.ProjectileSlots = 0;
		return Reservation;
	}

	// The first sub-shot consumes the press edge: an admission without a
	// fresh press is refused - a hold never starts a burst on its own.
	if (!bPressArmed)
	{
		Reservation.RejectDetail = TEXT("no burst is armed (a burst needs a fresh trigger press)");
		return Reservation;
	}

	bPressArmed = false;
	bFirstSubShotReserved = true;
	Reservation.bGranted = true;
	Reservation.RaycastSlots = 1;
	Reservation.ProjectileSlots = 0;
	return Reservation;
}

void FBurstFirePolicy::CommitShot(const FShotContext& Committed)
{
	// The first commit turns the armed press into the bounded remainder and
	// lays the interval anchor from the actual commit time; every later commit
	// counts one sub-shot down and re-anchors. The anchor is laid at commit
	// only, so an aborted reservation leaves no pacing residue.
	if (bFirstSubShotReserved)
	{
		bFirstSubShotReserved = false;
		PendingSubShots = BurstCount - 1;
		NextSubShotSeconds = Committed.CommittedAtSeconds + SubShotIntervalSeconds;
		return;
	}
	if (PendingSubShots > 0)
	{
		--PendingSubShots;
		NextSubShotSeconds = Committed.CommittedAtSeconds + SubShotIntervalSeconds;
	}
}

void FBurstFirePolicy::AbortReservation(const FShotContext& Aborted)
{
	// The first sub-shot's reservation is transactional: a refused later gate
	// (rounds, sequence) rolls the armed edge back - the press is NOT consumed
	// by a shot that never committed.
	if (bFirstSubShotReserved)
	{
		bFirstSubShotReserved = false;
	}
}

void FBurstFirePolicy::NotifyShotReleased(const FShotContext& Ended)
{
	// The release ends the shot's in-flight window; burst bookkeeping is not
	// per-pull state, so nothing to clear here (the scheduled remainder keeps
	// its anchor and completes through the input loop's polling).
}
