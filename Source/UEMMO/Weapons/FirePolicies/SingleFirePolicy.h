#pragma once

#include "CoreMinimal.h"

#include "../WeaponComponent.h"

/**
 * M5-022: the first fire policy - semiauto, one raycast request per shot.
 * The reservation pre-claims exactly one ray slot while no shot is in flight
 * (an overlap attempt is refused with the named detail); the commit keeps the
 * slot held until the caller releases the shot (ReleaseFire ->
 * NotifyShotReleased). AbortReservation releases a pre-claimed slot whose
 * transaction failed after the reservation. Only hitscan candidates are
 * granted: projectile pacing belongs to the later policies (024).
 */
class FSingleFirePolicy : public IFirePolicy
{
public:
	// IFirePolicy
	FCapacityReservation ReserveShotCapacity(const FShotContext& Candidate) override;
	void CommitShot(const FShotContext& Committed) override;
	void AbortReservation(const FShotContext& Aborted) override;
	void NotifyShotReleased(const FShotContext& Ended) override;

private:
	/** The single ray slot: held from a granted reservation until release/abort. */
	bool bRaySlotHeld = false;
};
