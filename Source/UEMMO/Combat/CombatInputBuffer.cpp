#include "CombatInputBuffer.h"

bool FCombatInputBuffer::Push(FBufferedCombatInput Input)
{
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

void FCombatInputBuffer::Reset()
{
	Entries.Reset();
	LastAcceptedSequence = 0;
}
