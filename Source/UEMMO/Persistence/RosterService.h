#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "Misc/Guid.h"

#include "../Profile/ProfileSubsystem.h"
#include "ProfileSaveGame.h"
#include "ProfileSaveService.h"
#include "ProfileSerializer.h"
#include "RosterSaveGame.h"

#include "RosterService.generated.h"

/**
 * M7-002: one in-memory roster entry (the validated form of
 * FRosterEntrySaveRow).
 */
struct FRosterEntry
{
	FGuid CharacterId;
	FName ProfessionId;
	FString DisplayName;
	FString SavePrefix;
	bool bActive = false;
};

/**
 * M7-002: result of LoadRoster. The semantics mirror the profile startup
 * chain: NoRoster ("nothing stored - the caller decides") is separate from
 * RecoveryError ("data exists but is unusable - surface the error, keep the
 * evidence, NEVER treat it as a fresh state and re-import"), because a
 * corrupt roster must never masquerade as a fresh install (双角色 guard).
 */
enum class ERosterLoadResult : uint8
{
	/** The index-active roster slot loaded exactly as the index points. */
	Recovered = 0,

	/** The active slot or the index was unusable; the other slot was recovered. */
	RecoveredFallback,

	/** Nothing stored at all (no index, no slot files) - a fresh state. */
	NoRoster,

	/** Data exists but no usable roster could be restored; the evidence stays on disk. */
	RecoveryError
};

/** Load result: enum + reason + the validated entries. */
struct FRosterLoadOutcome
{
	ERosterLoadResult Result = ERosterLoadResult::RecoveryError;
	FString Message;
	TArray<FRosterEntry> Entries;

	/** The active character id (invalid when no entry is active/loaded). */
	FGuid ActiveCharacterId;
};

/**
 * M7-002: result of the one-time legacy import. The distinctions the task
 * card demands: RosterAlreadyExists (a healthy roster is present - never
 * re-import, 双角色 guard), NoLegacyData (no roster AND no legacy saves - the
 * fresh-install path creates the default prototype character),
 * LegacyRecoveryError (the legacy data exists but is unusable - keep the
 * legacy files and the empty roster, retryable later) and Imported.
 */
enum class ERosterImportResult : uint8
{
	Imported = 0,
	FreshInstallCreated,
	RosterAlreadyExists,
	NoLegacyData,
	LegacyRecoveryError,
	Failed
};

struct FRosterImportOutcome
{
	ERosterImportResult Result = ERosterImportResult::Failed;
	FString Message;

	/** The imported/created character id (Invalid unless Imported/FreshInstallCreated). */
	FGuid CharacterId;

	/** The derived character save prefix (empty on failure). */
	FString SavePrefix;
};

/**
 * M7-002: result of CreateCharacter. The transaction contract: the roster
 * entry is written as a PENDING row into the staging slot first (recoverable
 * evidence), then the character's own A/B/Index save is written and verified
 * by read-back, and only then is the roster committed - any earlier failure
 * leaves the committed roster untouched.
 */
enum class ERosterCreateResult : uint8
{
	Created = 0,
	Failed
};

struct FRosterCreateOutcome
{
	ERosterCreateResult Result = ERosterCreateResult::Failed;
	FString Message;

	/** The minted character id (valid on Created). */
	FGuid CharacterId;

	/** The derived character save prefix (non-empty on Created). */
	FString SavePrefix;
};

/**
 * M7-002: the character roster service (02-多职业功能实施方案.md §5). A/B/Index
 * transaction over the roster itself (URosterSlotSaveGame/URosterIndexSaveGame
 * under RosterPrefix + "A"/"B"/"Index"), plus the per-character save flows it
 * drives through UProfileSaveService on each character's derived prefix:
 *
 * - LoadRoster: index-active slot first, the other slot as the corruption
 *   fallback, NoRoster vs RecoveryError kept distinct (双角色 guard).
 * - ImportLegacyProfile: the ONE-TIME legacy import. A healthy roster
 *   refuses (RosterAlreadyExists); no roster AND no legacy data is the fresh
 *   install (creates the default prototype character); a corrupt legacy save
 *   keeps the legacy files and the empty roster (LegacyRecoveryError,
 *   retryable); a healthy legacy save is copied into the new character's own
 *   prefix with the ORIGINAL CharacterId preserved (the legacy A/B/Index
 *   bytes stay read-only) and only then is the roster committed.
 * - CreateCharacter: mint the identity -> pending roster row into the
 *   staging slot -> the character's own A/B/Index save via
 *   UProfileSaveService -> read-back verification -> the roster commit.
 *   Any earlier failure leaves the committed roster untouched.
 * - SetActiveCharacter: verifies the target character's save loads before
 *   committing the active flag (the caller saves the CURRENT character
 *   first - the documented switching order).
 *
 * The service is synchronous and single-threaded (game thread); the storage
 * seam is the profile save's ISaveStorage (test-injectable).
 */
UCLASS()
class UEMMO_API URosterService : public UObject
{
	GENERATED_BODY()

public:
	/** Builds the roster slot names from the prefix; refuses an empty prefix. */
	bool Initialize(const FString& InRosterPrefix);

	/** Replaces the storage implementation (the profile save's test seam). */
	void SetStorage(ISaveStorage* InStorage);

	/** The configured roster prefix (empty until Initialize succeeded). */
	const FString& GetRosterPrefix() const;

	/** True when a roster slot file exists on disk (the startup probe). */
	bool HasRosterFiles() const;

	/** The structured roster load (see ERosterLoadResult). */
	FRosterLoadOutcome LoadRoster();

	/**
	 * The one-time legacy import / fresh-install bootstrap (see
	 * ERosterImportResult). LegacyService must be initialized on the legacy
	 * prefix ("Profile_"); Profile is the live profile subsystem the fresh
	 * install mints into. The legacy slots are never written.
	 */
	FRosterImportOutcome ImportLegacyProfile(UProfileSaveService& LegacyService,
		UProfileSubsystem* Profile);

	/**
	 * Creates a character: mints the identity, derives the prefix, writes the
	 * pending roster row into the staging slot, saves InitialState through
	 * the character's own A/B/Index transaction (with ProfessionId/DisplayName
	 * stamped into the profession state), verifies by read-back and commits
	 * the roster. InitialState.Snapshot.CharacterId must be INVALID - the
	 * service mints it (the caller's live profile state is not trusted for
	 * identity here; NewProfile runs against the caller's profile AFTER the
	 * commit via the returned identity).
	 */
	FRosterCreateOutcome CreateCharacter(FName ProfessionId, const FString& DisplayName,
		const FProfileSaveRequest& InitialState);

	/**
	 * Commits the active flag to TargetCharacterId after verifying that the
	 * target's own prefix still loads (the switching order: the caller saved
	 * the current character first). Returns false and changes nothing when
	 * the target is unknown or its save is unusable.
	 */
	bool SetActiveCharacter(const FGuid& TargetCharacterId);

	/**
	 * The deterministic save prefix of one character: "Char_" + the
	 * CharacterId's 32 hex digits (stable, filename-safe, collision-free).
	 */
	static FString DeriveSavePrefix(const FGuid& CharacterId);

	/** Deletes exactly this service's three roster slot names (test cleanup). */
	int32 DeleteTestSlots();

private:
	/** The minting-optional core of CreateCharacter (the import passes the legacy id). */
	bool CreateCharacterInternal(const FGuid& CharacterId, FName ProfessionId,
		const FString& DisplayName, const FProfileSaveRequest& InitialState,
		FRosterCreateOutcome& Out);

	/** Reads the roster index (missing/corrupt classification). */
	bool M2_ReadIndex(int32& OutActiveSlot, int32 (&OutGenerations)[2],
		bool& bOutMissing, FString& OutProblem);

	/** The slot integrity digest over the canonical row text. */
	static uint32 ComputeEntryDigestImpl(const TArray<FRosterEntrySaveRow>& Entries);

	/** Default storage resolution (lazy engine-backed storage creation). */
	ISaveStorage* ResolveStorage();

	/** The committed entries of the index-active roster slot (with fallback). */
	bool ReadCommittedEntries(TArray<FRosterEntrySaveRow>& OutEntries, FString& OutProblem, bool& bOutNoRoster);

private:
	FString RosterPrefix;
	TArray<FString> SlotNames;

	/** Storage override (test seam); null resolves the engine-backed storage. */
	ISaveStorage* StorageOverride = nullptr;

	/** The full roster A/B transaction: staging write -> verify -> index commit. */
	bool CommitRoster(const TArray<FRosterEntrySaveRow>& Entries, FString& OutProblem);

	/** Writes one roster slot + read-back verification; returns false on any problem. */
	bool WriteRosterSlotAndVerify(int32 SlotIndex, const TArray<FRosterEntrySaveRow>& Entries,
		int32 Generation, FString& OutProblem);

	/** Reads one roster slot (integrity + schema checks); null-safe. */
	bool TryReadRosterSlot(int32 SlotIndex, TArray<FRosterEntrySaveRow>& OutEntries,
		int32& OutGeneration, FString& OutProblem);

	/** Saves the initial character state under Prefix and verifies the read-back. */
	bool SaveInitialCharacterState(const FString& Prefix, const FProfileSaveRequest& InitialState,
		FString& OutProblem);


};
