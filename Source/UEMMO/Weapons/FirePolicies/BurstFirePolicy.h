#pragma once

#include "CoreMinimal.h"

#include "../WeaponComponent.h"

/**
 * M5-024: the burst-fire pacing policy - one press triggers one bounded burst
 * ("一次意图一串"); every sub-shot is individually admitted, deducted and
 * numbered through the component's single-fire transaction (each sub-shot is
 * its own TryFire commit with its own common ShotId).
 *
 * The state machine lives on the concrete class (the input layer M5-033 owns
 * the instance and registers it through SetFirePolicy):
 *
 * - NotifyTriggerPressed arms one burst while idle. A press edge while a
 *   burst is pending or already armed is ignored, and a hold never starts a
 *   second burst ("重复press或长按不叠加两串") - after the burst completes
 *   only a fresh press edge re-arms.
 * - ReserveShotCapacity grants the armed first sub-shot, then the scheduled
 *   follow-ups one sub-shot interval apart (0 defers the sub-shot pacing to
 *   the component's fire-rate cooldown). CommitShot counts the burst down
 *   from the actual commit time.
 * - NotifyTriggerReleased only disarms a burst that has not started yet; a
 *   started burst runs to its end ("已开始点射松键可继续").
 * - CancelPendingBurst kills the pending sub-shots - the caller's duty on
 *   death/equipment-switch/reload (the component's own gates refuse the
 *   stream there, and the policy must not resume the stale remainder:
 *   "死亡/切换/换弹取消剩余" and "中断不补发").
 * - HasPendingBurst tells the input loop to keep polling after the release
 *   until the burst completes.
 *
 * The clock is the component's injected fire clock (the candidate's
 * CommittedAtSeconds): a pause freezes the burst in place, a stalled frame
 * fires at most the single due sub-shot - never the accumulated rest.
 * Only hitscan candidates are granted: the 025 multi-pellet policy owns
 * pellet capacity.
 */
class FBurstFirePolicy : public IFirePolicy
{
public:
	/** Sets the rounds per burst (1..64; values outside clamp into range). */
	void SetBurstCount(int32 InBurstCount);

	/**
	 * Sets the interval between sub-shots in seconds (0 = the component's
	 * fire-rate cooldown paces the sub-shots). Non-finite and non-positive
	 * values clamp to 0.
	 */
	void SetSubShotIntervalSeconds(double IntervalSeconds);

	/** Arms one burst on a press edge (ignored while a burst is pending/armed). */
	void NotifyTriggerPressed();

	/** Disarms a burst that has not started yet; a started burst keeps running. */
	void NotifyTriggerReleased();

	/** Kills the pending sub-shots (death/switch/reload duty of the caller). */
	void CancelPendingBurst();

	/** True while sub-shots are still due (the input loop keeps polling). */
	bool HasPendingBurst() const;

	// IFirePolicy
	FCapacityReservation ReserveShotCapacity(const FShotContext& Candidate) override;
	void CommitShot(const FShotContext& Committed) override;
	void AbortReservation(const FShotContext& Aborted) override;
	void NotifyShotReleased(const FShotContext& Ended) override;

private:
	/** Rounds per burst. */
	int32 BurstCount = 3;

	/** The interval between sub-shots; 0 defers to the component cooldown. */
	double SubShotIntervalSeconds = 0.0;

	/** True while a press edge is armed and the first sub-shot is not reserved yet. */
	bool bPressArmed = false;

	/** True between the granted first sub-shot reservation and its commit/abort. */
	bool bFirstSubShotReserved = false;

	/** Sub-shots still due after the first (0 = idle). */
	int32 PendingSubShots = 0;

	/** Fire-clock seconds the next scheduled sub-shot opens. */
	double NextSubShotSeconds = 0.0;
};
