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
 * Time-based expiry is added by task M1-004.
 */
class FCombatInputBuffer
{
public:
	bool Push(FBufferedCombatInput Input);
	bool ConsumeFirst(ECombatInput Action, FBufferedCombatInput& Out);
	int32 Size() const;
	void Reset();

private:
	static constexpr int32 Capacity = 4;

	TArray<FBufferedCombatInput> Entries;
	uint64 LastAcceptedSequence = 0;
};
