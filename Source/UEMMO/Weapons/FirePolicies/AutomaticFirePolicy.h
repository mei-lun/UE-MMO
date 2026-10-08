#pragma once

#include "CoreMinimal.h"

#include "../WeaponComponent.h"

/**
 * M5-023: the automatic-fire pacing policy - hold-to-fire at the configured
 * shot interval, one shot per frame at most, no catch-up after a stall
 * ("卡顿不补射历史整串"), and an explicit stop on release/reload/switch/death.
 *
 * The input layer (M5-033) drives the hold: while the trigger stays down it
 * polls TryFire once per frame and closes the per-shot in-flight window with
 * ReleaseFire the same frame (hitscan resolves instantly). This policy owns
 * the cadence on top of the component's fire-rate cooldown:
 *
 * - The clock is the component's injected fire clock (GameClockSeconds
 *   semantics, Pause-frozen): the candidate's CommittedAtSeconds IS the "now"
 *   here, so the policy keeps no second clock and a pause or a per-character
 *   HitStop can neither cheat nor extend the cadence ("暂停不推进冷却，单角色
 *   HitStop不改变枪冷却").
 * - CommitShot lays the admission anchor (commit time + interval);
 *   ReserveShotCapacity refuses anything earlier - at most one admission per
 *   frame, and a stall fires exactly ONE shot when it ends: the skipped shots
 *   are dropped, never accumulated.
 * - The anchor survives the release on purpose (the weapon's cadence is not
 *   per-pull state), so "松开后不残留" means: no stray shot and no held
 *   capacity after the release - the reservation is transactional (the granted
 *   ray slot lives only inside the TryFire transaction), and the release
 *   clears nothing but the same-frame dedup marker. Reload/switch/death
 *   terminate the stream through the component's own named gates (Reloading,
 *   NoWeaponBound, OwnerDead) before the policy is ever consulted.
 * - SetShotIntervalSeconds(0) defers the pacing entirely to the component's
 *   fire-rate cooldown (60/FireRateRpm); a positive interval makes this policy
 *   the stricter pacer. The per-frame guard stays active either way.
 *
 * Only hitscan candidates are granted: projectile delivery and its capacity
 * belong to the 024 policy.
 */
class FAutomaticFirePolicy : public IFirePolicy
{
public:
	/**
	 * Sets the policy-level shot interval in seconds (0 = the component's
	 * fire-rate cooldown paces alone). Non-finite and non-positive values
	 * clamp to 0.
	 */
	void SetShotIntervalSeconds(double IntervalSeconds);

	// IFirePolicy
	FCapacityReservation ReserveShotCapacity(const FShotContext& Candidate) override;
	void CommitShot(const FShotContext& Committed) override;
	void AbortReservation(const FShotContext& Aborted) override;
	void NotifyShotReleased(const FShotContext& Ended) override;

private:
	/** The configured shot interval; 0 defers the pacing to the component cooldown. */
	double ShotIntervalSeconds = 0.0;

	/** Fire-clock seconds the next admission opens (laid at commit; 0 = open). */
	double NextAdmissionSeconds = 0.0;

	/** Fire-clock seconds of the last admission (-1 = none since the last release). */
	double LastAdmissionSeconds = -1.0;
};
