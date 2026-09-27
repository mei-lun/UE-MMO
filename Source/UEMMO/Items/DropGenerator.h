#pragma once

#include "CoreMinimal.h"

#include "ItemDefinition.h"
#include "ItemInstance.h"
#include "DropTable.h"

/**
 * Seeded drop generation of the M3 reward stack (interface contract section 8,
 * M3-007). FDropGenerator is pure logic: no World, no asset IO, no global
 * random state. Given one RewardSeed and one SettlementId it picks exactly one
 * equipment definition from a FDropTable by weight and materializes it as a
 * fully determined FItemInstance, so a retry with the same inputs reproduces
 * the identical instance (same DefinitionId, same deterministic InstanceId,
 * same recorded RollSeed, same stats) while the persistence of that first
 * result belongs to the settlement layer (M3-008 FPendingReward).
 *
 * Determinism scheme (documented for M3-008/M3-009 consumers):
 * - The selection stream is an FRandomStream seeded with
 *   DeriveSelectionStreamSeed(RewardSeed), never the engine global stream and
 *   never the combat stream, so combat draws cannot shift drop outcomes and
 *   frame-rate-dependent call counts cannot either.
 * - Instance.InstanceId is derived deterministically from
 *   (SettlementId, RewardIndex) only - the same settlement reward slot always
 *   maps to the same FGuid, independent of the seed.
 * - Instance.RollSeed is derived from RewardSeed with a distinct salt so
 *   future stat rolls (M3-010 and later) are uncorrelated with the table
 *   selection stream while staying reproducible from the same RewardSeed.
 * - Instance.RolledStats copy Definition.BaseStats (M3-001 factory behavior);
 *   no extra variance is rolled in this card.
 * - Exactly one equipment instance is produced per clear; XP and the reward
 *   container (FPendingReward, inventory-full retention) belong to M3-008/009.
 */
struct FDropRewardResult
{
	/** True only when a reward was generated; check Error otherwise. */
	bool bSuccess = false;

	/** The generated instance; meaningful only when bSuccess is true. */
	FItemInstance Instance;

	/**
	 * Empty on success; on failure a message naming the offending field
	 * ("Entries", "DefinitionId", "Weight") or the invalid input.
	 */
	FString Error;
};

struct FDropGenerator
{
	/**
	 * Generates the reward of one cleared room from Table using Catalog.
	 *
	 * RewardSeed: the fixed seed for this settlement's roll (caller derives it
	 *   from the settlement data; the same RewardSeed must be reused on retry).
	 * SettlementId: the business identity of the settlement; InstanceId is a
	 *   deterministic function of it and RewardIndex (never of wall clocks).
	 * Table: weighted entries; rejected when empty, when any Weight is
	 *   negative, when a DefinitionId is listed twice, when an id is unknown
	 *   to Catalog, or when the total weight is 0. The weighted pick is a
	 *   single pass - there is no random retry loop.
	 * Catalog: read-only item definition lookup; unknown DefinitionIds fail.
	 * RewardIndex: slot of this reward within the settlement (one equipment
	 *   drop per clear, so the default 0 covers this card).
	 *
	 * Returns a result with bSuccess=false and a field-naming Error for every
	 * rejected input; success carries the fully determined FItemInstance.
	 */
	static FDropRewardResult GenerateReward(int64 RewardSeed, uint64 SettlementId,
		const FDropTable& Table, const FItemDefinitionCatalog& Catalog, int32 RewardIndex = 0);
};
