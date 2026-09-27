#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "Misc/Guid.h"

#include "../Items/InventoryModel.h"
#include "../Items/ItemDefinition.h"
#include "../Profile/ProfileSubsystem.h"
#include "../Profile/RewardService.h"
#include "ProfileSaveGame.h"

#include "ProfileSaveService.generated.h"

/**
 * M3-014: the serialized content of ONE A/B profile slot. Subclasses
 * UProfileSaveGame (the M3-013 payload) so the payload travels as direct
 * serialized fields (a USaveGame cannot inline another USaveGame's contents),
 * and adds the transaction envelope fields the A/B slot protocol needs:
 *
 * - SlotGeneration: the monotonic transaction counter stamped by
 *   UProfileSaveService (starts at 1 for the first commit; every successful
 *   save increments). The index mirrors it; the slot payload self-carries it
 *   so a load can report the generation even when the index is gone.
 * - Integrity fields (interface contract section 8: "A/B slots carry
 *   Generation and integrity validation"): PayloadDigest (CRC32 over every
 *   base payload field, computed at write time), PayloadInstanceCount and
 *   CharacterIdSummary (the two field-level checks named by the task card).
 *   A read-back whose recomputed digest/stored fields disagree is rejected:
 *   the transaction aborts and the index is never pointed at it.
 *
 * The digest is a corruption checksum, not tamper-proof security: a rewrite
 * of payload AND stored digest together is outside this card's threat model
 * (documented in the task report as a known limitation).
 */
UCLASS()
class UEMMO_API UProfileSlotSaveGame : public UProfileSaveGame
{
	GENERATED_BODY()

public:
	/** Monotonic transaction counter stamped by UProfileSaveService (>= 1). */
	UPROPERTY()
	int32 SlotGeneration = 0;

	/** CRC32 over the base UProfileSaveGame payload at write time. */
	UPROPERTY()
	uint32 PayloadDigest = 0;

	/** Integrity: number of inventory snapshot strings at write time. */
	UPROPERTY()
	int32 PayloadInstanceCount = 0;

	/** Integrity: the CharacterId as its string form at write time. */
	UPROPERTY()
	FString CharacterIdSummary;
};

/**
 * M3-014: the small activity index SaveGame stored under SlotPrefix + "Index".
 * Records which A/B slot is active (0 = A, 1 = B, NoActiveSlot = nothing
 * committed yet) and each slot's last committed generation. The index is the
 * COMMIT POINT of the save transaction: it is written only after the new slot
 * payload was written AND verified by read-back, so any earlier failure
 * leaves the index (and therefore the old valid save) untouched. The index is
 * also the single source of truth for which slot to overwrite next (the
 * inactive one) and which slot LoadActiveProfile reads first.
 */
UCLASS()
class UEMMO_API UProfileIndexSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	/** ActiveSlotIndex value meaning "no slot committed yet" (fresh state). */
	static constexpr int32 NoActiveSlot = -1;

	/** Which slot is active: 0 = A, 1 = B, NoActiveSlot = none. */
	UPROPERTY()
	int32 ActiveSlotIndex = NoActiveSlot;

	/** Last committed generation per slot; exactly 2 entries (A, B). */
	UPROPERTY()
	TArray<int32> SlotGenerations;
};

/**
 * Storage seam behind the save transaction (interface contract section 8:
 * slots go through USaveGame write/read). The production implementation
 * (FGameplayStaticsSaveStorage) uses UGameplayStatics::SaveGameToSlot /
 * LoadGameFromSlot. Tests inject an in-memory implementation so the failure
 * paths (write failure, corrupted read-back, index write failure) can be
 * exercised without touching any real file, and one smoke test exercises the
 * real UGameplayStatics path on dedicated TestSlot_ slots.
 *
 * Plain C++ interface on purpose (no UINTERFACE): the implementations are
 * engine-free test doubles and a thin engine adapter, never reflected, never
 * replicated. Ownership: the storage does NOT own the passed USaveGame; a
 * ReadSlot result is a fresh object the caller uses within its call stack.
 */
class UEMMO_API ISaveStorage
{
public:
	virtual ~ISaveStorage() = default;

	/** Persists the save object under the slot name; false = storage refused/failed. */
	virtual bool WriteSlot(const FString& SlotName, USaveGame* Data) = 0;

	/** Loads the slot's save object; null when missing or unreadable. */
	virtual USaveGame* ReadSlot(const FString& SlotName) = 0;

	/** Removes the slot; false when the storage could not delete it. */
	virtual bool DeleteSlot(const FString& SlotName) = 0;

	/** True when a slot file/object exists under this name. */
	virtual bool DoesSlotExist(const FString& SlotName) = 0;
};

/**
 * Production storage: the real UGameplayStatics::SaveGameToSlot /
 * LoadGameFromSlot / DeleteGameInSlot / DoesSaveGameExist path (UserIndex 0).
 * Stateless; works in -nullrhi automation (slot files under the project's
 * saved directory), verified by the M3-014 smoke test on TestSlot_ slots.
 */
class UEMMO_API FGameplayStaticsSaveStorage : public ISaveStorage
{
public:
	virtual bool WriteSlot(const FString& SlotName, USaveGame* Data) override;
	virtual USaveGame* ReadSlot(const FString& SlotName) override;
	virtual bool DeleteSlot(const FString& SlotName) override;
	virtual bool DoesSlotExist(const FString& SlotName) override;
};

/**
 * One save request, assembled by the caller from the profile domain (interface
 * contract section 8 value types). UProfileSaveService copies it ONCE inside
 * BeginSave - the captured copy is the transaction's immutable snapshot, so
 * later mutations of the caller's live profile state can never leak into an
 * already-started save. Mirrors FProfileSerializer::ToSaveGame's inputs; the
 * applied-settlement set travels because UProfileSubsystem exposes membership
 * checks only, so the future wiring task assembles this request.
 */
struct FProfileSaveRequest
{
	/** Identity/progress snapshot (the level-formula base stat row). */
	FProfileSnapshot Snapshot;

	/** Authoritative item source at capture time (value copy). */
	FInventoryModel Inventory;

	/** Unclaimed reward drafts at capture time (value copy). */
	TArray<FPendingReward> PendingRewards;

	/** Settlement ids already claimed at capture time (value copy). */
	TSet<uint64> AppliedSettlementIds;

	/** Equipment slot bindings at capture time (value copy). */
	TMap<EItemSlot, FGuid> EquippedMap;
};

/**
 * Result of one save entry point: an explicit success/failure enum plus a
 * reason, so the UI can never present "saved" for a failed transaction (task
 * card: "report the operation result; the UI must not show saved on failure").
 * The disk effects of every Failed* value are identical: the activity index
 * was NOT moved, the previously active slot is untouched and still loadable,
 * and a partially written slot (FailedWriteSlot onwards) holds no committed
 * data and is simply overwritten by the next save.
 */
enum class EProfileSaveResult : uint8
{
	/** BeginSave: the consistent snapshot was captured; call ProcessPendingSave to commit it. */
	Queued = 0,

	/**
	 * BeginSave while another save was in progress: the NEWEST request replaced
	 * the queued snapshot (merge semantics chosen over queueing - the older
	 * superseded state would be stale the moment the newer state exists, and
	 * no second transaction is stacked). Still needs ProcessPendingSave.
	 */
	Merged,

	/** ProcessPendingSave: slot written, read-back verified, index committed. */
	Success,

	/** BeginSave: the request data was rejected BEFORE any disk write (invalid profile state). */
	FailedCapture,

	/** The inactive slot write failed (storage unavailable); old slot still active. */
	FailedWriteSlot,

	/** Read-back of the freshly written slot was missing/corrupt/failed validation; index untouched. */
	FailedReadBack,

	/** The index commit write failed; the new slot content exists but is NOT active (overwritten next time). */
	FailedWriteIndex,

	/** The existing index file is unreadable/corrupt: the save refuses to guess which slot is safe to overwrite. */
	FailedIndexState,

	/** ProcessPendingSave without a queued snapshot (defensive; nothing happened). */
	FailedNoPendingSave
};

/** Save entry-point result: enum + reason + where the data went. */
struct FProfileSaveOutcome
{
	/** What happened; Message carries the reason for every Failed* value. */
	EProfileSaveResult Result = EProfileSaveResult::FailedNoPendingSave;

	/** Committed generation (Success only; 0 otherwise). */
	int32 Generation = 0;

	/** Slot the transaction wrote: 0 = A, 1 = B (Success; -1 otherwise). */
	int32 WrittenSlotIndex = -1;

	/** Human-readable failure reason naming the failing step; empty on success. */
	FString Message;
};

/**
 * Result of LoadActiveProfile. Success means the index was healthy and the
 * active slot passed every check; SuccessFallback means the index was
 * missing/corrupt or the active slot was unusable and a valid slot was
 * recovered (the higher generation wins when scanning). Both failure values
 * restore NOTHING (the outcome carries default data only) - a failed load
 * must never surface as a half-restored profile.
 */
enum class EProfileLoadResult : uint8
{
	/** Active slot loaded exactly as the index points. */
	Success = 0,

	/** Recovered from a valid slot after index loss/corruption or an unusable active slot. */
	SuccessFallback,

	/** Nothing is stored at all (no index, no slot files) - a fresh state, not corruption. */
	FailedNoData,

	/** Data exists but every candidate slot failed integrity/validation - explicit failure. */
	FailedCorrupt
};

/** Load result: enum + restored data + where it came from. */
struct FProfileLoadOutcome
{
	/** What happened; Message carries the recovery/failure reason. */
	EProfileLoadResult Result = EProfileLoadResult::FailedNoData;

	/** Generation of the slot the data came from (0 on failure). */
	int32 Generation = 0;

	/** Slot the data came from: 0 = A, 1 = B (-1 on failure). */
	int32 SlotIndex = -1;

	/** Human-readable reason (which step failed / where the data was recovered from). */
	FString Message;

	/** Restored profile state; default (empty) values on any failure. */
	FProfileSnapshot Snapshot;
	FInventoryModel Inventory;
	TArray<FPendingReward> PendingRewards;
	TSet<uint64> AppliedSettlementIds;
	TMap<EItemSlot, FGuid> EquippedMap;
};

/**
 * M3-014: A/B dual-slot profile save service with index commit (interface
 * contract section 8). Owns exactly three slot names derived from the prefix
 * given to Initialize: SlotPrefix + "A", SlotPrefix + "B" and SlotPrefix +
 * "Index" - it NEVER constructs or touches any other slot name, so a service
 * configured with the automation prefix "TestSlot_" cannot modify a player's
 * "Profile_*" saves by construction (pinned by a dedicated isolation test).
 *
 * Save transaction (SaveProfile = BeginSave + ProcessPendingSave):
 * 1. BeginSave copies the request ONCE into the pending transaction (the
 *    consistent snapshot) and validates the payload with the M3-013
 *    serializer BEFORE any disk write (FailedCapture refuses invalid data).
 *    A second BeginSave while a save is in progress MERGES: the newest
 *    request replaces the queued snapshot (one transaction stays queued -
 *    bSaveInProgress guard, no stacking). Merge-over-queue is the documented
 *    choice: the superseded older state is stale the moment the newer state
 *    exists.
 * 2. ProcessPendingSave reads the index fresh from storage, writes the new
 *    payload (generation = max(indexed generations) + 1) into the INACTIVE
 *    slot, reads it back and verifies (integrity fields + M3-013
 *    FromSaveGame validation) and only then commits the index to point at
 *    the new slot/generation. ANY failure leaves the index untouched: the
 *    old active slot stays loadable and the failed slot is overwritten by
 *    the next save. An unreadable EXISTING index refuses the save entirely
 *    (FailedIndexState) instead of guessing which slot holds the only good
 *    copy; a MISSING index is the legal fresh state (first save goes to A).
 * 3. LoadActiveProfile reads the index, then the active slot (integrity +
 *    serializer validation); on index loss/corruption or an unusable active
 *    slot it falls back to the other slot (higher generation wins when
 *    scanning); when nothing valid exists it fails explicitly (FailedNoData
 *    for a truly fresh state, FailedCorrupt when data exists but is bad).
 *
 * The service is synchronous and single-threaded (game thread); the
 * Begin/Process split exists so a frame-driven game loop can park a
 * transaction between frames, and so the in-progress guard is observable.
 * DeleteTestSlots removes exactly this service's three slot names (test
 * cleanup; it can never reach a slot outside its own prefix).
 */
UCLASS()
class UEMMO_API UProfileSaveService : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Builds the slot names from the prefix. The prefix must be non-empty
	 * (an empty prefix would produce generic "A"/"B"/"Index" slot names that
	 * could collide with unrelated saves); returns false and stays
	 * uninitialized when rejected. Production uses "Profile_", automation
	 * uses "TestSlot_".
	 */
	bool Initialize(const FString& InSlotPrefix);

	/**
	 * Replaces the storage implementation (test seam). Null restores the
	 * default FGameplayStaticsSaveStorage (created lazily on first use).
	 * The service never owns an injected storage; the caller keeps it alive.
	 */
	void SetStorage(ISaveStorage* InStorage);

	/** True between a BeginSave (or merging BeginSave) and its ProcessPendingSave. */
	bool IsSaveInProgress() const;

	/** The configured prefix (empty until Initialize succeeded). */
	const FString& GetSlotPrefix() const;

	/**
	 * Captures the request as the transaction's consistent snapshot and
	 * validates the resulting payload BEFORE any disk write. Returns Queued
	 * (first request), Merged (a save was in progress: newest snapshot
	 * replaced the queued one) or FailedCapture (invalid data; nothing
	 * queued). Does not touch storage.
	 */
	FProfileSaveOutcome BeginSave(const FProfileSaveRequest& Request);

	/**
	 * Runs the disk transaction for the queued snapshot: write inactive slot
	 * -> read-back verification -> index commit. Consumes the queued
	 * snapshot on every exit; failures leave the index and the old active
	 * slot untouched (see EProfileSaveResult).
	 */
	FProfileSaveOutcome ProcessPendingSave();

	/**
	 * Convenience wrapper for the synchronous path: BeginSave followed by
	 * ProcessPendingSave. The returned outcome is the transaction result
	 * (or the FailedCapture rejection).
	 */
	FProfileSaveOutcome SaveProfile(const FProfileSaveRequest& Request);

	/**
	 * Reads the index, then the active slot; falls back per the class
	 * comment. Never restores partial state: every Failed* outcome carries
	 * default (empty) data.
	 */
	FProfileLoadOutcome LoadActiveProfile();

	/**
	 * Deletes exactly this service's three slot names (A, B, Index under its
	 * own prefix) - test cleanup. Returns how many slots actually existed
	 * and were deleted; a slot outside the prefix is unreachable by
	 * construction. Named for the automation flow: with the production
	 * prefix this deletes the production slots, so only ever call it for
	 * slots the card explicitly owns (TestSlot_ in automation).
	 */
	int32 DeleteTestSlots();

private:
	/** Default storage resolution (lazy engine-backed storage creation). */
	ISaveStorage* ResolveStorage();

	/**
	 * CRC32 over every base UProfileSaveGame field in a canonical order (the
	 * integrity digest written into UProfileSlotSaveGame::PayloadDigest and
	 * recomputed on every read).
	 */
	static uint32 M3_014_ComputePayloadDigest(const UProfileSaveGame& Save);

	/**
	 * Builds the slot payload for the request: the M3-013 serializer maps the
	 * snapshot, then every reflected UProfileSaveGame property is copied into
	 * the UProfileSlotSaveGame and the generation/integrity envelope is
	 * stamped. Returns null when the serializer produced no payload.
	 */
	static UProfileSlotSaveGame* M3_014_BuildSlotSave(const FProfileSaveRequest& Request, int32 Generation);

	/**
	 * Reads the index fresh from storage. Returns false ONLY for an existing
	 * but unreadable/corrupt index (OutProblem names it); a MISSING index is
	 * the legal fresh state (returns true, bOutMissing = true, no active
	 * slot, generations 0/0).
	 */
	bool M3_014_ReadIndex(int32& OutActiveSlot, int32 (&OutGenerations)[2], bool& bOutMissing, FString& OutProblem);

	/**
	 * Recovers the effective transaction state from the slot PAYLOADS when
	 * the index file is missing: every valid slot save self-describes with
	 * its integrity digest and generation, so the highest-generation valid
	 * slot is treated as the active one (its content must never be
	 * overwritten by the transaction) and the generations seed the next
	 * increment. With no valid slot payload the state is truly fresh.
	 */
	void M3_014_RecoverStateFromSlots(int32& OutEffectiveActive, int32 (&OutGenerations)[2]);

	/**
	 * Read-back gate for a freshly written slot: class, generation, the
	 * three integrity fields and the M3-013 FromSaveGame validation must all
	 * pass, and the digest must match the just-written source payload.
	 * OutProblem names the failing check.
	 */
	bool M3_014_VerifyReadBack(const USaveGame* RawLoaded, int32 ExpectedGeneration,
		const UProfileSaveGame& SourcePayload, FString& OutProblem);

	/** Prefix from Initialize ("" until then). */
	FString SlotPrefix;

	/** [0] = SlotPrefix + "A", [1] = SlotPrefix + "B" (built by Initialize). */
	FString SlotNames[2];

	/** SlotPrefix + "Index" (built by Initialize). */
	FString IndexSlotName;

	/** bSaveInProgress guard: true from BeginSave until ProcessPendingSave consumes the queue. */
	bool bSaveInProgress = false;

	/** The queued consistent snapshot (merged over by newer BeginSave calls). */
	FProfileSaveRequest PendingRequest;

	/** Lazily created default storage (only when SetStorage was never used). */
	TUniquePtr<ISaveStorage> OwnedStorage;

	/** Active storage: injected pointer or OwnedStorage.Get(). */
	ISaveStorage* Storage = nullptr;
};
