#pragma once

#include "CoreMinimal.h"

/**
 * Weighted drop table layer of the M3 reward stack (interface contract
 * section 8, M3-007). A table lists the DefinitionIds that can drop together
 * with relative weights; the seeded pick itself lives in FDropGenerator
 * (DropGenerator.h). Plain value types on purpose: identity, validation and
 * the pinned starter values stay pure C++ logic testable without a World.
 * Source of truth for the starter table is Data/drops.json
 * (schema_version=1, table_id="starter"); MakeStarterDropTable mirrors the
 * same values inline so logic tests never depend on file IO.
 */

/** One weighted entry of a drop table. */
struct FDropEntry
{
	/**
	 * Which item kind can drop; must resolve in the item definition catalog
	 * (FItemDefinitionCatalog, ItemDefinition.h) at generation time.
	 */
	FName DefinitionId;

	/**
	 * Relative probability weight; must be >= 0. A total weight of 0 makes
	 * the whole table unrollable and is rejected by the generator.
	 */
	int32 Weight = 0;
};

/** A named set of weighted drop entries (e.g. the "starter" clear reward). */
struct FDropTable
{
	/** Stable business identifier of the table (e.g. "starter"); not an asset path. */
	FName TableId;

	/** Weighted entries; duplicate DefinitionIds inside one table are rejected. */
	TArray<FDropEntry> Entries;
};

/**
 * Validates a drop table in isolation and returns true when legal. Rules
 * (M3-007): TableId non-empty, Entries non-empty, every entry has a non-empty
 * DefinitionId, every Weight >= 0, and no DefinitionId listed twice. On
 * failure OutErrors joins every problem with "; " and each problem names its
 * field ("TableId", "Entries", "DefinitionId", "Weight") so callers can pin
 * the offending data. Whether the referenced definitions exist in a catalog
 * is a generator-level check (needs the FItemDefinitionCatalog).
 */
inline bool ValidateDropTable(const FDropTable& Table, FString& OutErrors)
{
	TArray<FString> Problems;

	// Identity: a table without a stable id cannot be referenced by the
	// settlement flow or future data-driven lookup.
	if (Table.TableId.IsNone())
	{
		Problems.Add(TEXT("TableId must be a non-empty identifier (got None)"));
	}

	// Shape: an empty table can never produce a reward.
	if (Table.Entries.Num() == 0)
	{
		Problems.Add(TEXT("Entries must contain at least one entry"));
	}

	TSet<FName> SeenIds;
	for (int32 Index = 0; Index < Table.Entries.Num(); ++Index)
	{
		const FDropEntry& Entry = Table.Entries[Index];

		// Identity of the entry: an empty id cannot resolve in any catalog.
		if (Entry.DefinitionId.IsNone())
		{
			Problems.Add(FString::Printf(TEXT("Entries[%d].DefinitionId must be a non-empty identifier (got None)"), Index));
		}
		else if (SeenIds.Contains(Entry.DefinitionId))
		{
			// Duplicates would make the weight split ambiguous.
			Problems.Add(FString::Printf(TEXT("Entries[%d].DefinitionId '%s' is listed more than once; drop table entries must have unique DefinitionIds"),
				Index, *Entry.DefinitionId.ToString()));
		}
		else
		{
			SeenIds.Add(Entry.DefinitionId);
		}

		// Weight: negative weights are meaningless and a sign of broken data.
		if (Entry.Weight < 0)
		{
			Problems.Add(FString::Printf(TEXT("Entries[%d].Weight must be >= 0 (got %d)"),
				Index, Entry.Weight));
		}
	}

	if (Problems.Num() == 0)
	{
		OutErrors.Reset();
		return true;
	}
	OutErrors = FString::Join(Problems, TEXT("; "));
	return false;
}

/**
 * The pinned starter clear-reward table: one guaranteed equipment drop per
 * room clear with the design weights 50/30/20 (weapon_training first,
 * armor_training second, charm_training third). Mirrors Data/drops.json
 * (schema_version=1, table_id="starter"); keep both in sync.
 */
inline FDropTable MakeStarterDropTable()
{
	FDropTable Table;
	Table.TableId = TEXT("starter");
	Table.Entries.Add(FDropEntry{ FName(TEXT("weapon_training")), 50 });
	Table.Entries.Add(FDropEntry{ FName(TEXT("armor_training")), 30 });
	Table.Entries.Add(FDropEntry{ FName(TEXT("charm_training")), 20 });
	return Table;
}
