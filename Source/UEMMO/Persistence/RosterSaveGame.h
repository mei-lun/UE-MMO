#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "Misc/Guid.h"

#include "RosterSaveGame.generated.h"

/**
 * M7-002: one character entry of the roster (02-多职业功能实施方案.md §5:
 * "记录 CharacterId、ProfessionId、显示名、存档前缀和活动标记"). Pure value
 * row; the identity is the persistent FGuid (never a UE asset path), and the
 * SavePrefix is deterministically derived from the CharacterId
 * (URosterService::DeriveSavePrefix) so a roster row can always locate its
 * character's own A/B/Index slots.
 */
USTRUCT()
struct FRosterEntrySaveRow
{
	GENERATED_BODY()

	/** Persistent character identity (the legacy import PRESERVES the legacy id). */
	UPROPERTY()
	FGuid CharacterId;

	/** The character's profession (a Data/Professions profession_id). */
	UPROPERTY()
	FName ProfessionId;

	/** Menu display name as authored. */
	UPROPERTY()
	FString DisplayName;

	/** The character's own A/B/Index slot prefix (Char_<32 hex digits>). */
	UPROPERTY()
	FString SavePrefix;

	/** The active character flag: exactly one row per committed roster. */
	UPROPERTY()
	bool bActive = false;
};

/**
 * M7-002: the versioned roster payload. SchemaVersion starts at 1; a version
 * above this build's is refused (never wiped) exactly like the profile save.
 */
UCLASS()
class UEMMO_API URosterSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	static constexpr int32 CurrentSchemaVersion = 1;

	UPROPERTY()
	int32 SchemaVersion = CurrentSchemaVersion;

	/** One row per character, in creation order (the active flag on exactly one). */
	UPROPERTY()
	TArray<FRosterEntrySaveRow> Entries;
};

/**
 * M7-002: the serialized content of ONE roster A/B slot (the transaction
 * envelope, mirroring UProfileSlotSaveGame): SlotGeneration plus a CRC32
 * EntryDigest over the rows and the row count, so a read-back that disagrees
 * is rejected and the index is never pointed at it.
 */
UCLASS()
class UEMMO_API URosterSlotSaveGame : public URosterSaveGame
{
	GENERATED_BODY()

public:
	/** Monotonic transaction counter stamped by URosterService (>= 1). */
	UPROPERTY()
	int32 SlotGeneration = 0;

	/** CRC32 over the roster rows at write time. */
	UPROPERTY()
	uint32 EntryDigest = 0;

	/** Integrity: the row count at write time. */
	UPROPERTY()
	int32 EntryCount = 0;
};

/**
 * M7-002: the small activity index SaveGame under RosterPrefix + "Index"
 * (the COMMIT POINT of the roster transaction, mirroring
 * UProfileIndexSaveGame): written only after the new roster slot was written
 * AND verified by read-back.
 */
UCLASS()
class UEMMO_API URosterIndexSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	/** ActiveSlotIndex value meaning "no roster committed yet" (fresh state). */
	static constexpr int32 NoActiveSlot = -1;

	/** Which roster slot is active: 0 = A, 1 = B, NoActiveSlot = none. */
	UPROPERTY()
	int32 ActiveSlotIndex = NoActiveSlot;

	/** Last committed generation per slot; exactly 2 entries (A, B). */
	UPROPERTY()
	TArray<int32> SlotGenerations;
};
