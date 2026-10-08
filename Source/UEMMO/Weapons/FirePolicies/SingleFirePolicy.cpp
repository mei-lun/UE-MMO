#include "SingleFirePolicy.h"

FCapacityReservation FSingleFirePolicy::ReserveShotCapacity(const FShotContext& Candidate)
{
	FCapacityReservation Reservation;

	// Semiauto pacing: the single ray slot is held from the reservation until
	// the shot's release; an overlap attempt is the named refusal.
	if (bRaySlotHeld)
	{
		Reservation.RejectDetail = TEXT("the single-fire ray slot is still held (the previous shot is not released)");
		return Reservation;
	}

	// The first policy owns the hitscan delivery only: projectile pacing and
	// capacity belong to the later policies (024).
	if (Candidate.FireMode != EWeaponFireMode::Hitscan)
	{
		Reservation.RejectDetail = FString::Printf(
			TEXT("the single-fire policy grants hitscan shots only (the candidate is mode %d)"),
			static_cast<int32>(Candidate.FireMode));
		return Reservation;
	}

	// One shot is one raycast request: pre-claim exactly one ray slot.
	Reservation.bGranted = true;
	Reservation.RaycastSlots = 1;
	Reservation.ProjectileSlots = 0;
	bRaySlotHeld = true;
	return Reservation;
}

void FSingleFirePolicy::CommitShot(const FShotContext& Committed)
{
	// The granted reservation turns into the in-flight bookkeeping: the slot
	// stays held until NotifyShotReleased.
}

void FSingleFirePolicy::AbortReservation(const FShotContext& Aborted)
{
	// A post-reserve transaction failure releases the pre-claimed slot.
	bRaySlotHeld = false;
}

void FSingleFirePolicy::NotifyShotReleased(const FShotContext& Ended)
{
	// The trigger release ends the shot's in-flight window.
	bRaySlotHeld = false;
}
