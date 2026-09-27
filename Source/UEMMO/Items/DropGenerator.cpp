#include "DropGenerator.h"

#include "Math/RandomStream.h"

// M3-007 red-phase stub: every call is rejected until the real weighted pick
// lands. The deterministic helpers below are already in their final shape so
// the green-phase change touches only GenerateReward's body.
namespace
{
	// 64-bit finalizer (murmur3 fmix): deterministic, platform-independent,
	// and spreads single-bit input differences across all output bits.
	uint64 DropMix64(uint64 X)
	{
		X ^= X >> 33;
		X *= 0xFF51AFD7ED558CCDull;
		X ^= X >> 33;
		X *= 0xC4CEB9FE1A85EC53ull;
		X ^= X >> 33;
		return X;
	}

	// Distinct purpose salts so the table selection stream, the recorded stat
	// roll seed and the instance identity never share one raw value.
	constexpr uint64 DropSaltSelectionStream = 0x9E3779B97F4A7C15ull;
	constexpr uint64 DropSaltRollSeed = 0xC2B2AE3D27D4EB4Full;
	constexpr uint64 DropSaltInstanceA = 0x243F6A8885A308D3ull;
	constexpr uint64 DropSaltInstanceB = 0x13198A2E03707344ull;

	// FRandomStream seeds are int32; fold the int64 RewardSeed into 32 bits.
	int32 DeriveSelectionStreamSeed(int64 RewardSeed)
	{
		const uint64 Mixed = DropMix64(static_cast<uint64>(RewardSeed) ^ DropSaltSelectionStream);
		return static_cast<int32>(Mixed ^ (Mixed >> 32));
	}

	int64 DeriveRollSeed(int64 RewardSeed)
	{
		const uint64 Mixed = DropMix64(static_cast<uint64>(RewardSeed) ^ DropSaltRollSeed);
		return static_cast<int64>(Mixed ^ (Mixed >> 32));
	}

	// InstanceId = f(SettlementId, RewardIndex); never seed- or clock-derived.
	FGuid DeriveInstanceGuid(uint64 SettlementId, int32 RewardIndex)
	{
		const uint64 IndexBits = static_cast<uint64>(static_cast<uint32>(RewardIndex));
		const uint64 A = DropMix64(SettlementId ^ DropSaltInstanceA ^ (IndexBits << 32));
		const uint64 B = DropMix64((SettlementId * DropSaltSelectionStream) ^ DropSaltInstanceB ^ IndexBits);
		FGuid Id(static_cast<uint32>(A), static_cast<uint32>(A >> 32), static_cast<uint32>(B), static_cast<uint32>(B >> 32));
		if (!Id.IsValid())
		{
			// The all-zero guid is never a legal identity; a single deterministic
			// bit keeps the mapping total (astronomically unlikely anyway).
			Id.A = 1;
		}
		return Id;
	}
}

FDropRewardResult FDropGenerator::GenerateReward(int64 RewardSeed, uint64 SettlementId,
	const FDropTable& Table, const FItemDefinitionCatalog& Catalog, int32 RewardIndex)
{
	FDropRewardResult Result;
	Result.bSuccess = false;

	// Structural rejection pass (single walk over the entries, ascending).
	// Every error names its field so data bugs are directly attributable.
	if (Table.Entries.Num() == 0)
	{
		Result.Error = TEXT("Entries must contain at least one entry; a drop table without entries cannot produce a reward");
		return Result;
	}
	TSet<FName> SeenIds;
	for (int32 Index = 0; Index < Table.Entries.Num(); ++Index)
	{
		const FDropEntry& Entry = Table.Entries[Index];
		if (Entry.DefinitionId.IsNone())
		{
			Result.Error = FString::Printf(TEXT("Entries[%d].DefinitionId must be a non-empty identifier (got None)"), Index);
			return Result;
		}
		if (Entry.Weight < 0)
		{
			Result.Error = FString::Printf(TEXT("Entries[%d].Weight must be >= 0 (got %d)"), Index, Entry.Weight);
			return Result;
		}
		if (SeenIds.Contains(Entry.DefinitionId))
		{
			Result.Error = FString::Printf(TEXT("Entries[%d].DefinitionId '%s' is listed more than once; drop table entries must have unique DefinitionIds"),
				Index, *Entry.DefinitionId.ToString());
			return Result;
		}
		SeenIds.Add(Entry.DefinitionId);
	}

	// Catalog rejection pass: every referenced definition must be known, so a
	// broken data file can never silently shrink the reward pool.
	for (int32 Index = 0; Index < Table.Entries.Num(); ++Index)
	{
		const FDropEntry& Entry = Table.Entries[Index];
		if (Catalog.Find(Entry.DefinitionId) == nullptr)
		{
			Result.Error = FString::Printf(TEXT("Entries[%d].DefinitionId '%s' is not registered in the item definition catalog"),
				Index, *Entry.DefinitionId.ToString());
			return Result;
		}
	}

	// Zero total weight: the weighted pick has nothing to distribute.
	int32 TotalWeight = 0;
	for (const FDropEntry& Entry : Table.Entries)
	{
		TotalWeight += Entry.Weight;
	}
	if (TotalWeight <= 0)
	{
		Result.Error = FString::Printf(TEXT("Entries total Weight must be > 0 (got %d); a drop table with zero total weight cannot be rolled"),
			TotalWeight);
		return Result;
	}

	// Independent stream: seeded from RewardSeed only (never the global
	// stream, never a combat stream), so combat draws and frame-rate dependent
	// call counts cannot shift the outcome.
	FRandomStream SelectionStream(DeriveSelectionStreamSeed(RewardSeed));

	// Weighted pick in exactly one pass - no random retry loop. The point
	// falls in [0, TotalWeight); the first entry whose cumulative weight
	// passes the point wins.
	const float Point = SelectionStream.FRand() * static_cast<float>(TotalWeight);
	int32 Accumulated = 0;
	int32 PickedIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Table.Entries.Num(); ++Index)
	{
		Accumulated += Table.Entries[Index].Weight;
		if (Point < static_cast<float>(Accumulated))
		{
			PickedIndex = Index;
			break;
		}
	}
	if (PickedIndex == INDEX_NONE)
	{
		// Only reachable through float rounding at the exact upper edge
		// (Point == TotalWeight): fall back to the last positive-weight entry.
		// Deterministic and loop-free.
		for (int32 Index = Table.Entries.Num() - 1; Index >= 0; --Index)
		{
			if (Table.Entries[Index].Weight > 0)
			{
				PickedIndex = Index;
				break;
			}
		}
		if (PickedIndex == INDEX_NONE)
		{
			// Unreachable after the TotalWeight check; kept as a guard so the
			// function can never index out of bounds.
			Result.Error = TEXT("Entries contain no entry with a positive Weight");
			return Result;
		}
	}

	const FItemDefinition* Definition = Catalog.Find(Table.Entries[PickedIndex].DefinitionId);
	if (!Definition)
	{
		Result.Error = FString::Printf(TEXT("Entries[%d].DefinitionId '%s' is not registered in the item definition catalog"),
			PickedIndex, *Table.Entries[PickedIndex].DefinitionId.ToString());
		return Result;
	}

	// Materialize the instance: the M3-001 factory copies BaseStats and stamps
	// a fresh guid, which is then replaced by the deterministic identity
	// derived from (SettlementId, RewardIndex). The recorded RollSeed derives
	// from RewardSeed with a distinct salt, so the same RewardSeed always
	// rebuilds the same instance while future stat rolls stay uncorrelated
	// with the table selection stream.
	FItemInstance Instance = MakeItemInstance(*Definition, DeriveRollSeed(RewardSeed));
	Instance.InstanceId = DeriveInstanceGuid(SettlementId, RewardIndex);

	Result.bSuccess = true;
	Result.Error.Reset();
	Result.Instance = MoveTemp(Instance);
	return Result;
}
