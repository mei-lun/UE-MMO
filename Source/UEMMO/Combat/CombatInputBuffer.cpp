#include "CombatInputBuffer.h"

#include "Math/UnrealMathUtility.h"

bool FCombatInputBuffer::Push(FBufferedCombatInput Input)
{
	if (!FMath::IsFinite(Input.PressedAt))
	{
		// Non-finite timestamps (NaN/Inf) are rejected outright: no queue entry,
		// and the session sequence watermark is not advanced.
		return false;
	}
	if (Input.Sequence <= LastAcceptedSequence)
	{
		return false;
	}
	if (Entries.Num() >= Capacity)
	{
		Entries.RemoveAt(0);
	}
	Entries.Add(Input);
	LastAcceptedSequence = Input.Sequence;
	return true;
}

bool FCombatInputBuffer::ConsumeFirst(ECombatInput Action, FBufferedCombatInput& Out)
{
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		if (Entries[Index].Action == Action)
		{
			Out = Entries[Index];
			Entries.RemoveAt(Index);
			return true;
		}
	}
	return false;
}

int32 FCombatInputBuffer::Size() const
{
	return Entries.Num();
}

bool FCombatInputBuffer::PeekAt(int32 Index, FBufferedCombatInput& Out) const
{
	if (Index < 0 || Index >= Entries.Num())
	{
		return false;
	}
	Out = Entries[Index];
	return true;
}

void FCombatInputBuffer::PruneExpired(double Now, double Lifetime)
{
	// Walk backwards so removal keeps the relative order of surviving entries.
	for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
	{
		const FBufferedCombatInput& Entry = Entries[Index];
		const bool bNonFiniteTime = !FMath::IsFinite(Entry.PressedAt);
		const bool bInFuture = Entry.PressedAt > Now;
		const bool bExpired = (Now - Entry.PressedAt) > Lifetime;
		if (bNonFiniteTime || bInFuture || bExpired)
		{
			Entries.RemoveAt(Index);
		}
	}
}

bool FCombatInputBuffer::Consume(ECombatInput Action, double Now, double Lifetime, FBufferedCombatInput& Out)
{
	PruneExpired(Now, Lifetime);
	return ConsumeFirst(Action, Out);
}

void FCombatInputBuffer::PruneExpiredAnchored(double Now, double Lifetime, const TMap<uint64, double>& Anchors)
{
	// Walk backwards so removal keeps the relative order of surviving entries,
	// exactly like PruneExpired; only the judged press time differs (the
	// anchor overrides when it sits after the raw stamp).
	for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
	{
		const FBufferedCombatInput& Entry = Entries[Index];
		const double* Anchor = Anchors.Find(Entry.Sequence);
		const double EffectivePressedAt = (Anchor != nullptr && *Anchor > Entry.PressedAt) ? *Anchor : Entry.PressedAt;
		const bool bNonFiniteTime = !FMath::IsFinite(Entry.PressedAt) || !FMath::IsFinite(EffectivePressedAt);
		const bool bInFuture = EffectivePressedAt > Now;
		const bool bExpired = (Now - EffectivePressedAt) > Lifetime;
		if (bNonFiniteTime || bInFuture || bExpired)
		{
			Entries.RemoveAt(Index);
		}
	}
}

void FCombatInputBuffer::ShiftPressedTimes(double OffsetSeconds)
{
	if (OffsetSeconds == 0.0)
	{
		return;
	}
	for (FBufferedCombatInput& Entry : Entries)
	{
		if (FMath::IsFinite(Entry.PressedAt))
		{
			Entry.PressedAt += OffsetSeconds;
		}
	}
}

void FCombatInputBuffer::Reset()
{
	Entries.Reset();
	LastAcceptedSequence = 0;
}
