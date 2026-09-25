#pragma once

#include "CoreMinimal.h"

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

	int32 Size() const;
	void Reset();

private:
	static constexpr int32 Capacity = 4;

	TArray<FBufferedCombatInput> Entries;
	uint64 LastAcceptedSequence = 0;
};
