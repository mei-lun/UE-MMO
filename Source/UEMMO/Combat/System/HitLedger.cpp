#include "HitLedger.h"

#include "CombatEntityRegistry.h"

// M5-010: implementation of the bounded idempotent hit ledger. See the header
// for the frozen rules: key-level event dedup, once-per-(shot,target) control,
// a hard capacity on active shot instances that never evicts, tombstoned shot
// endings and explicit refusal reasons for everything.

FHitLedger::FHitLedger(const FCombatEntityRegistry* InRegistry)
	: Registry(InRegistry)
{
}

void FHitLedger::BindRegistry(const FCombatEntityRegistry* InRegistry)
{
	Registry = InRegistry;
}

const FCombatEntityRegistry* FHitLedger::GetBoundRegistry() const
{
	return Registry;
}

void FHitLedger::SetCapacity(int32 NewCapacity)
{
	Capacity = NewCapacity < 1 ? 1 : NewCapacity;
}

int32 FHitLedger::GetCapacity() const
{
	return Capacity;
}

int32 FHitLedger::GetNumRecordedEvents() const
{
	return RecordedEvents.Num();
}

int32 FHitLedger::GetNumAcceptedControls() const
{
	return AcceptedControls.Num();
}

int32 FHitLedger::GetNumActiveShots() const
{
	return NumActiveShots;
}

int32 FHitLedger::GetNumTrackedShots() const
{
	return Shots.Num();
}

bool FHitLedger::ValidateRequest(const FCombatEventKey& Key, EHitLedgerRejectReason& OutReason) const
{
	if (!IsUsableCombatEventKey(Key))
	{
		OutReason = EHitLedgerRejectReason::UnusableKey;
		return false;
	}
	if (Registry == nullptr)
	{
		OutReason = EHitLedgerRejectReason::UnboundRegistry;
		return false;
	}
	if (Key.Epoch != Registry->GetCurrentEpoch())
	{
		OutReason = EHitLedgerRejectReason::StaleEpoch;
		return false;
	}
	if (!Registry->IsKnownEntity(Key.EntityId))
	{
		OutReason = EHitLedgerRejectReason::UnknownSource;
		return false;
	}
	if (!Registry->IsKnownEntity(Key.TargetId))
	{
		OutReason = EHitLedgerRejectReason::UnknownTarget;
		return false;
	}
	return true;
}

bool FHitLedger::TryRecordHitEvent(const FCombatEventKey& Key, EHitLedgerRejectReason& OutReason)
{
	OutReason = EHitLedgerRejectReason::None;

	if (!ValidateRequest(Key, OutReason))
	{
		return false;
	}

	const FCombatLedgerShotKey ShotKey = MakeCombatLedgerShotKey(Key);
	FCombatLedgerShotState* Existing = Shots.Find(ShotKey);
	if (Existing != nullptr && Existing->bEnded)
	{
		// The tombstone keeps the idempotency guarantee past the shot's end.
		OutReason = EHitLedgerRejectReason::ShotAlreadyEnded;
		return false;
	}

	if (RecordedEvents.Contains(Key))
	{
		OutReason = EHitLedgerRejectReason::DuplicateEvent;
		return false;
	}

	if (Existing == nullptr && NumActiveShots >= Capacity)
	{
		// Hard capacity: refuse the NEW attack instance instead of evicting a
		// live key - evicting would let the same hit apply twice.
		OutReason = EHitLedgerRejectReason::CapacityFull;
		return false;
	}

	if (Existing != nullptr)
	{
		++Existing->RecordedEventCount;
	}
	else
	{
		FCombatLedgerShotState NewState;
		NewState.RecordedEventCount = 1;
		Shots.Add(ShotKey, NewState);
		++NumActiveShots;
	}
	RecordedEvents.Add(Key);
	return true;
}

bool FHitLedger::TryAcceptShotControl(const FCombatEventKey& Key, EHitLedgerRejectReason& OutReason)
{
	OutReason = EHitLedgerRejectReason::None;

	// Usability ignores the pellet index by contract, so any pellet of the
	// shot may carry the control request.
	if (!ValidateRequest(Key, OutReason))
	{
		return false;
	}

	const FCombatLedgerShotKey ShotKey = MakeCombatLedgerShotKey(Key);
	FCombatLedgerShotState* Existing = Shots.Find(ShotKey);
	if (Existing != nullptr && Existing->bEnded)
	{
		OutReason = EHitLedgerRejectReason::ShotAlreadyEnded;
		return false;
	}

	const FCombatShotControlKey ControlKey = MakeCombatShotControlKey(Key);
	if (AcceptedControls.Contains(ControlKey))
	{
		// One shot accepts control on one target exactly once, whichever
		// pellet delivered it.
		OutReason = EHitLedgerRejectReason::DuplicateControl;
		return false;
	}

	if (Existing == nullptr && NumActiveShots >= Capacity)
	{
		// A pure-control request can be the first touch of an attack
		// instance, so it counts against the same hard capacity.
		OutReason = EHitLedgerRejectReason::CapacityFull;
		return false;
	}

	if (Existing != nullptr)
	{
		++Existing->AcceptedControlCount;
	}
	else
	{
		FCombatLedgerShotState NewState;
		NewState.AcceptedControlCount = 1;
		Shots.Add(ShotKey, NewState);
		++NumActiveShots;
	}
	AcceptedControls.Add(ControlKey);
	return true;
}

bool FHitLedger::HasRecordedEvent(const FCombatEventKey& Key) const
{
	return RecordedEvents.Contains(Key);
}

bool FHitLedger::HasAcceptedControl(const FCombatEventKey& Key) const
{
	return AcceptedControls.Contains(MakeCombatShotControlKey(Key));
}

bool FHitLedger::EndShot(FCombatEpoch Epoch, FEntityId SourceEntityId, FShotId ShotId)
{
	if (Registry == nullptr || Epoch != Registry->GetCurrentEpoch())
	{
		return false;
	}

	const FCombatLedgerShotKey ShotKey{Epoch, SourceEntityId, ShotId};
	FCombatLedgerShotState* State = Shots.Find(ShotKey);
	if (State == nullptr || State->bEnded)
	{
		return false;
	}
	State->bEnded = true;
	--NumActiveShots;

	// Release the shot's keys back to the pool: the ledger returns toward its
	// baseline while the tombstone keeps refusing late re-sends.
	TArray<FCombatEventKey> DroppedEvents;
	for (const FCombatEventKey& Recorded : RecordedEvents)
	{
		if (MakeCombatLedgerShotKey(Recorded) == ShotKey)
		{
			DroppedEvents.Add(Recorded);
		}
	}
	for (const FCombatEventKey& Dropped : DroppedEvents)
	{
		RecordedEvents.Remove(Dropped);
	}

	TArray<FCombatShotControlKey> DroppedControls;
	for (const FCombatShotControlKey& Accepted : AcceptedControls)
	{
		if (Accepted.Epoch == ShotKey.Epoch
			&& Accepted.SourceEntityId == ShotKey.SourceEntityId
			&& Accepted.ShotId == ShotKey.ShotId)
		{
			DroppedControls.Add(Accepted);
		}
	}
	for (const FCombatShotControlKey& Dropped : DroppedControls)
	{
		AcceptedControls.Remove(Dropped);
	}
	return true;
}

bool FHitLedger::FindTrackedShot(FCombatEpoch Epoch, FEntityId SourceEntityId, FShotId ShotId, FCombatLedgerShotState& OutState) const
{
	const FCombatLedgerShotState* State = Shots.Find(FCombatLedgerShotKey{Epoch, SourceEntityId, ShotId});
	if (State == nullptr)
	{
		return false;
	}
	OutState = *State;
	return true;
}

void FHitLedger::Reset()
{
	Shots.Empty();
	RecordedEvents.Empty();
	AcceptedControls.Empty();
	NumActiveShots = 0;
}
