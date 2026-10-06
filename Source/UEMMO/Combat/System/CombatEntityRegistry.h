#pragma once

#include "CoreMinimal.h"
#include <type_traits>

#include "CombatEventTypes.h"

class AActor;

/**
 * M5-010: the world-level combat entity registry (interface contract section 1,
 * owner 010). This is a plain logic class - no UObject, no subsystem - that
 * implements the identity rules; the integration task owns one instance per
 * World alongside the World's own lifetime (ownership ladder, section 0.5:
 * registry and epochs die with the World, never with a pawn or a profile).
 *
 * Rules frozen here:
 * - Registration mints the next FEntityId of the current generation for a
 *   weakly referenced actor plus its faction/category metadata. Ids restart
 *   at 1 in every new epoch: a rebuilt world (EndPlay / leave room / retry)
 *   is a fresh identity space, and callers that carry an (old epoch, id) pair
 *   are refused by the explicit epoch checks instead of silently colliding
 *   with a re-minted id. 0 is never handed out (InvalidCombatEntityId).
 * - Every mutating request carries the caller's epoch. Requests that do not
 *   name the current generation are refused with ECombatRegistryRejectReason::
 *   StaleEpoch - this is what makes "unregister refuses old-epoch requests"
 *   (and its register/allocate siblings) a real, testable rule instead of an
 *   accident of the records having been wiped.
 * - One common monotonic ActionSequence per (Epoch, SourceEntity): melee,
 *   firearms and vehicle weapons all draw the same allocator, so a melee
 *   AttackInstanceId of 1 and a firearm ShotSequence of 1 still land on two
 *   different public sequences (M5-002's FCombatEventKey::ShotId contract).
 *   Module-local counters are association values only and never fill the key.
 * - Resolving an id returns the registered weak reference; a destroyed actor
 *   (or a logic-only null-actor record) resolves to null. The registry never
 *   stores a raw actor pointer and never writes the registry into a save.
 *
 * Signature note versus the interface contract sketch (section 2/3): the
 * sketch showed FGuid ids and no epoch parameters. Section 0 of the frozen
 * contract already converged the FGuid ids to the native uint64 aliases of
 * CombatEventTypes.h, and the contract leaves signature completion to the
 * owning task ("the marked owner completes and freezes the signatures"); the
 * added CallerEpoch parameters are that completion - they are what enforces
 * the stale-epoch refusal semantics of sections 0.2 and 2.
 */

/**
 * Registration payload: faction and category of the registered entity.
 * Value type, no behavior; the hit pipeline uses the faction for friendly-fire
 * filtering, the category is an informational label (for example "player",
 * "enemy", "vehicle"). Owned by 010 - it is the registry's own registration
 * metadata and deliberately not one of 003's target-policy value types.
 */
struct FCombatEntityMetadata
{
	/** Faction tag used for same-team / friendly-fire filtering (empty = unaligned). */
	FName Faction;

	/** Informational entity category label (for example "player", "enemy", "vehicle"). */
	FName Category;

	bool operator==(const FCombatEntityMetadata& Other) const
	{
		return Faction == Other.Faction && Category == Other.Category;
	}
};

/**
 * One registered entity: the minted id, the epoch it was minted in (always the
 * current generation - a world rebuild drops every record), the weak actor
 * reference and the registration metadata. A value record: no raw pointer is
 * stored and consumers resolve actors only through ResolveEntity.
 */
struct FCombatEntityRecord
{
	/** The minted world entity id (starts at 1 in every epoch). */
	FEntityId EntityId = InvalidCombatEntityId;

	/** The generation that minted this record. */
	FCombatEpoch Epoch = InvalidCombatEpoch;

	/** Weak reference to the registered actor (may be null for logic-only records). */
	TWeakObjectPtr<AActor> Actor;

	/** Faction/category registration payload. */
	FCombatEntityMetadata Metadata;
};

/**
 * Why the registry refused a request. Append only - never renumber, so log
 * lines and tests keep their meaning. 0 always means "accepted".
 */
enum class ECombatRegistryRejectReason : uint8
{
	/** The request was accepted. */
	None = 0,
	/** The request named an epoch other than the current world generation. */
	StaleEpoch = 1,
	/** A live record of the current generation already holds this non-null actor. */
	DuplicateActor = 2,
	/** The named id is not a live record of the current generation (never registered or already unregistered). */
	UnknownEntity = 3
};

/**
 * Per-(Epoch, Source) allocator key: the sequence space of one source inside
 * one generation. A file-scope value key (like the ledger's keys) so its
 * GetTypeHash overload stays well-formed.
 */
struct FCombatSourceSequenceKey
{
	FCombatEpoch Epoch = InvalidCombatEpoch;
	FEntityId SourceEntityId = InvalidCombatEntityId;

	bool operator==(const FCombatSourceSequenceKey& Other) const
	{
		return Epoch == Other.Epoch && SourceEntityId == Other.SourceEntityId;
	}
};

inline uint32 GetTypeHash(const FCombatSourceSequenceKey& Key)
{
	uint32 Hash = ::GetTypeHash(Key.Epoch);
	Hash = HashCombine(Hash, ::GetTypeHash(Key.SourceEntityId));
	return Hash;
}

class FCombatEntityRegistry
{
public:
	FCombatEntityRegistry();

	/** The current world generation. A fresh registry starts in epoch 1. */
	FCombatEpoch GetCurrentEpoch() const;

	/**
	 * World rebuild: mints the next epoch and drops every record and every
	 * per-source sequence counter of the old generation (EndPlay / leave room
	 * / retry). Returns the new current epoch.
	 */
	FCombatEpoch BeginNextWorldEpoch();

	/**
	 * Registers an entity of the current generation and mints its FEntityId.
	 * Entity may be null (a logic-only record whose resolve answers null).
	 * Returns InvalidCombatEntityId and fills OutReason when the caller's
	 * epoch is stale or the non-null actor already holds a live record.
	 */
	FEntityId RegisterEntity(AActor* Entity, const FCombatEntityMetadata& Metadata, FCombatEpoch CallerEpoch, ECombatRegistryRejectReason* OutReason = nullptr);

	/**
	 * Unregisters the entity in the current generation. Refused (false plus an
	 * explicit OutReason) for a stale caller epoch or an unknown id.
	 */
	bool UnregisterEntity(FEntityId EntityId, FCombatEpoch CallerEpoch, ECombatRegistryRejectReason* OutReason = nullptr);

	/**
	 * Allocates the next value of the one common ActionSequence of
	 * (CallerEpoch, SourceEntityId) - the only value that may fill
	 * FCombatEventKey::ShotId for that source. Melee, firearm and vehicle
	 * callers share the sequence; their module-local counters never touch it.
	 * Returns InvalidCombatShotId for a stale epoch or an unknown source.
	 */
	FShotId AllocateActionSequence(FEntityId SourceEntityId, FCombatEpoch CallerEpoch, ECombatRegistryRejectReason* OutReason = nullptr);

	/** Resolves the id to the registered weak reference (null when unknown). */
	TWeakObjectPtr<AActor> ResolveEntity(FEntityId EntityId) const;

	/** Returns the live record of the id, or null. */
	const FCombatEntityRecord* FindEntity(FEntityId EntityId) const;

	/** True when the id is a live record of the current generation. */
	bool IsKnownEntity(FEntityId EntityId) const;

	/** True when the id is a live record AND the epoch names the current generation. */
	bool IsKnownEntityInEpoch(FEntityId EntityId, FCombatEpoch Epoch) const;

	/** Number of live records of the current generation. */
	int32 GetNumRegisteredEntities() const;

private:
	/** Live records of the current generation, keyed by the minted entity id. */
	TMap<FEntityId, FCombatEntityRecord> Records;

	/** Next common ActionSequence value per (Epoch, Source); a fresh allocation starts at 1. */
	TMap<FCombatSourceSequenceKey, FShotId> NextSequenceBySource;

	/** Current world generation; a fresh registry models a live epoch 1. */
	FCombatEpoch CurrentEpoch;

	/** Next entity id to mint inside the current generation (restarts at 1 per epoch). */
	FEntityId NextEntityId;
};

// Compile-time pins: the registry stores weak references and value ids, never
// a raw actor address, and the record stays a trivially copyable snapshot.
static_assert(!std::is_pointer_v<decltype(FCombatEntityRecord::Actor)>, "FCombatEntityRecord::Actor must stay a weak reference, never a raw pointer");
static_assert(std::is_trivially_copyable_v<FCombatEntityRecord>, "FCombatEntityRecord must stay a trivially copyable value snapshot");
