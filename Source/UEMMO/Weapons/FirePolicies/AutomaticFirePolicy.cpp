#include "AutomaticFirePolicy.h"

void FAutomaticFirePolicy::SetShotIntervalSeconds(double IntervalSeconds)
{
	ShotIntervalSeconds = (FMath::IsFinite(IntervalSeconds) && IntervalSeconds > 0.0) ? IntervalSeconds : 0.0;
}

FCapacityReservation FAutomaticFirePolicy::ReserveShotCapacity(const FShotContext& Candidate)
{
	FCapacityReservation Reservation;

	// The automatic policy owns the hitscan delivery only: projectile pacing
	// and capacity belong to the 024 policy.
	if (Candidate.FireMode != EWeaponFireMode::Hitscan)
	{
		Reservation.RejectDetail = FString::Printf(
			TEXT("the automatic-fire policy grants hitscan shots only (the candidate is mode %d)"),
			static_cast<int32>(Candidate.FireMode));
		return Reservation;
	}

	// One admission per frame at most: the candidate's commit time IS the
	// component's injected fire clock (GameClockSeconds semantics), so a second
	// poll inside the same frame reads the same value here - and a paused
	// clock reads the same value across frames, so the pause never advances
	// the cadence either.
	const double Now = Candidate.CommittedAtSeconds;
	if (LastAdmissionSeconds >= 0.0 && Now <= LastAdmissionSeconds)
	{
		Reservation.RejectDetail = FString::Printf(
			TEXT("at most one automatic shot per frame (now %.6f, the last admission %.6f)"),
			Now, LastAdmissionSeconds);
		return Reservation;
	}

	// The interval anchor laid at the last commit: a stall fires exactly ONE
	// shot when it ends - the skipped shots are dropped, never accumulated
	// ("卡顿不补射历史整串").
	if (Now < NextAdmissionSeconds)
	{
		Reservation.RejectDetail = FString::Printf(
			TEXT("the automatic interval runs until %.6f (now %.6f)"), NextAdmissionSeconds, Now);
		return Reservation;
	}

	// One shot is one raycast request: pre-claim exactly one ray slot.
	Reservation.bGranted = true;
	Reservation.RaycastSlots = 1;
	Reservation.ProjectileSlots = 0;
	return Reservation;
}

void FAutomaticFirePolicy::CommitShot(const FShotContext& Committed)
{
	// The admission anchor: the next shot opens one configured interval after
	// THIS commit (anchored on the actual commit time, so a long stall
	// restarts the cadence instead of catching up the skipped shots). The
	// same-frame dedup marker moves with it.
	LastAdmissionSeconds = Committed.CommittedAtSeconds;
	NextAdmissionSeconds = Committed.CommittedAtSeconds + ShotIntervalSeconds;
}

void FAutomaticFirePolicy::AbortReservation(const FShotContext& Aborted)
{
	// The reservation is transactional (the granted ray slot lives only inside
	// the TryFire transaction) and the anchor is laid at commit only, so an
	// aborted attempt leaves no pacing residue.
}

void FAutomaticFirePolicy::NotifyShotReleased(const FShotContext& Ended)
{
	// The release ends the shot's in-flight window and clears the same-frame
	// dedup marker. The interval anchor survives on purpose: the weapon's
	// cadence is not per-pull state, so a fast re-press stays paced by the
	// anchor plus the component cooldown - no stray shot, no residue.
	LastAdmissionSeconds = -1.0;
}
