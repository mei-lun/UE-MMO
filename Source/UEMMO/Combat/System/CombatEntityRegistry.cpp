#include "CombatEntityRegistry.h"

#include "GameFramework/Actor.h"

// M5-010: implementation of the world-level combat entity registry. See the
// header for the frozen rules: epoch-carrying mutations, per-generation id
// spaces and one common ActionSequence per (Epoch, SourceEntity).

FCombatEntityRegistry::FCombatEntityRegistry()
	: CurrentEpoch(1) // a fresh registry models a live first world generation
	, NextEntityId(1)
{
}

FCombatEpoch FCombatEntityRegistry::GetCurrentEpoch() const
{
	return CurrentEpoch;
}

FCombatEpoch FCombatEntityRegistry::BeginNextWorldEpoch()
{
	++CurrentEpoch;
	Records.Empty();
	NextSequenceBySource.Empty();
	NextEntityId = 1;
	return CurrentEpoch;
}

FEntityId FCombatEntityRegistry::RegisterEntity(AActor* Entity, const FCombatEntityMetadata& Metadata, FCombatEpoch CallerEpoch, ECombatRegistryRejectReason* OutReason)
{
	const auto Refuse = [OutReason](ECombatRegistryRejectReason Reason) -> FEntityId
	{
		if (OutReason != nullptr)
		{
			*OutReason = Reason;
		}
		return InvalidCombatEntityId;
	};

	if (CallerEpoch != CurrentEpoch)
	{
		return Refuse(ECombatRegistryRejectReason::StaleEpoch);
	}

	// A live actor cannot claim a second identity in one generation. Null
	// actors carry no object identity, so logic-only records may repeat.
	if (Entity != nullptr)
	{
		for (const TPair<FEntityId, FCombatEntityRecord>& Pair : Records)
		{
			if (Pair.Value.Actor.Get() == Entity)
			{
				return Refuse(ECombatRegistryRejectReason::DuplicateActor);
			}
		}
	}

	const FEntityId EntityId = NextEntityId;
	++NextEntityId;

	FCombatEntityRecord Record;
	Record.EntityId = EntityId;
	Record.Epoch = CurrentEpoch;
	Record.Actor = Entity;
	Record.Metadata = Metadata;
	Records.Add(EntityId, Record);

	if (OutReason != nullptr)
	{
		*OutReason = ECombatRegistryRejectReason::None;
	}
	return EntityId;
}

bool FCombatEntityRegistry::UnregisterEntity(FEntityId EntityId, FCombatEpoch CallerEpoch, ECombatRegistryRejectReason* OutReason)
{
	if (OutReason != nullptr)
	{
		*OutReason = ECombatRegistryRejectReason::None;
	}

	if (CallerEpoch != CurrentEpoch)
	{
		// An old-generation request is refused even when a re-minted record
		// with the same numeric id exists right now.
		if (OutReason != nullptr)
		{
			*OutReason = ECombatRegistryRejectReason::StaleEpoch;
		}
		return false;
	}

	if (Records.Remove(EntityId) == 0)
	{
		if (OutReason != nullptr)
		{
			*OutReason = ECombatRegistryRejectReason::UnknownEntity;
		}
		return false;
	}
	return true;
}

FShotId FCombatEntityRegistry::AllocateActionSequence(FEntityId SourceEntityId, FCombatEpoch CallerEpoch, ECombatRegistryRejectReason* OutReason)
{
	if (OutReason != nullptr)
	{
		*OutReason = ECombatRegistryRejectReason::None;
	}

	if (CallerEpoch != CurrentEpoch)
	{
		if (OutReason != nullptr)
		{
			*OutReason = ECombatRegistryRejectReason::StaleEpoch;
		}
		return InvalidCombatShotId;
	}

	if (!Records.Contains(SourceEntityId))
	{
		if (OutReason != nullptr)
		{
			*OutReason = ECombatRegistryRejectReason::UnknownEntity;
		}
		return InvalidCombatShotId;
	}

	const FCombatSourceSequenceKey Key{CurrentEpoch, SourceEntityId};
	if (FShotId* Next = NextSequenceBySource.Find(Key))
	{
		const FShotId Allocated = *Next;
		++(*Next);
		return Allocated;
	}

	// The fresh allocator hands out 1 now and remembers 2 as the next value.
	NextSequenceBySource.Add(Key, static_cast<FShotId>(2));
	return static_cast<FShotId>(1);
}

TWeakObjectPtr<AActor> FCombatEntityRegistry::ResolveEntity(FEntityId EntityId) const
{
	const FCombatEntityRecord* Record = FindEntity(EntityId);
	return Record != nullptr ? Record->Actor : TWeakObjectPtr<AActor>();
}

const FCombatEntityRecord* FCombatEntityRegistry::FindEntity(FEntityId EntityId) const
{
	return Records.Find(EntityId);
}

bool FCombatEntityRegistry::IsKnownEntity(FEntityId EntityId) const
{
	return Records.Contains(EntityId);
}

bool FCombatEntityRegistry::IsKnownEntityInEpoch(FEntityId EntityId, FCombatEpoch Epoch) const
{
	return Epoch == CurrentEpoch && Records.Contains(EntityId);
}

int32 FCombatEntityRegistry::GetNumRegisteredEntities() const
{
	return Records.Num();
}
