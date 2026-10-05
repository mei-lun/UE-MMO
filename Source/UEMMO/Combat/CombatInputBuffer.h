#pragma once

#include "CoreMinimal.h"
#include "Containers/Map.h"

enum class ECombatInput : uint8
{
	Light,
	Launcher,
	Jump
};

struct FBufferedCombatInput
{
	uint64 Sequence = 0;
	ECombatInput Action = ECombatInput::Light;
	double PressedAt = 0.0;
};

/**
 * Bounded FIFO of combat inputs (capacity 4).
 * Sequence numbers must strictly increase within a session; Reset() starts a new session.
 * All times come from the explicit monotonic input game clock passed by callers;
 * wall clocks (FPlatformTime, FDateTime::Now, ...) are never read internally.
 */
class FCombatInputBuffer
{
public:
	bool Push(FBufferedCombatInput Input);
	bool ConsumeFirst(ECombatInput Action, FBufferedCombatInput& Out);

	/**
	 * Removes entries whose age (Now - PressedAt) is strictly greater than Lifetime,
	 * plus entries whose PressedAt is non-finite or clearly in the future (PressedAt > Now).
	 * Surviving entries keep their relative order. Default lifetime is 0.150 seconds.
	 */
	void PruneExpired(double Now, double Lifetime = 0.150);

	/**
	 * Prunes expired entries first (PruneExpired(Now, Lifetime)), then consumes the
	 * earliest surviving entry matching Action, if any.
	 */
	bool Consume(ECombatInput Action, double Now, double Lifetime, FBufferedCombatInput& Out);

	/**
	 * M3-031: PruneExpired with a per-entry lifetime anchor override. Each
	 * entry's age is judged from its EFFECTIVE press time:
	 * max(PressedAt, Anchors.FindRef(Sequence)) when an anchor exists for the
	 * entry's sequence, PressedAt otherwise. Without anchors the judgment is
	 * exactly PruneExpired(Now, Lifetime). The component raises anchors for
	 * CARRIED presses (entries queued behind an earlier intent) while no
	 * consumption opportunity exists, so a queued press survives into the
	 * follow-up attack's cancel window instead of expiring mid-wait; the
	 * 150 ms lifetime itself never changes.
	 */
	void PruneExpiredAnchored(double Now, double Lifetime, const TMap<uint64, double>& Anchors);

	/**
	 * M3-031: shifts every buffered entry's PressedAt by OffsetSeconds. The
	 * hit stop compensation: a local stop pins the injected input clock and
	 * the resume applies the accumulated world jump in one step, so presses
	 * buffered across the stop would otherwise pay the frozen span out of
	 * their 150 ms lifetime. Shifting the press times by exactly the frozen
	 * span keeps every lifetime measured on unfrozen input-clock time only.
	 * No-op for a zero offset; non-finite stamps are left alone (they are
	 * dropped by the next prune anyway).
	 */
	void ShiftPressedTimes(double OffsetSeconds);

	int32 Size() const;

	/**
	 * M1-012: copies the entry at Index (0 = earliest) without consuming or
	 * reordering anything. Purely additive read-only accessor; returns false
	 * when the index is out of range.
	 */
	bool PeekAt(int32 Index, FBufferedCombatInput& Out) const;

	void Reset();

private:
	static constexpr int32 Capacity = 4;

	TArray<FBufferedCombatInput> Entries;
	uint64 LastAcceptedSequence = 0;
};
