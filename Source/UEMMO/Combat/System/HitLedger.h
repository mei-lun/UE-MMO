#pragma once

#include "CoreMinimal.h"
#include <type_traits>

#include "CombatEventTypes.h"

class FCombatEntityRegistry;

/**
 * M5-010: the bounded idempotent hit ledger (interface contract section 1,
 * owner 010). A plain logic class - no UObject - that owns the dedup face of
 * the combat event key; the integration task owns one instance per World next
 * to its FCombatEntityRegistry (both are World-lifetime, section 0.5) and
 * binds it with BindRegistry.
 *
 * Rules frozen here:
 * - The five-tuple FCombatEventKey is the dedup surface: an exact re-send is
 *   refused with DuplicateEvent. One shot may damage one target through
 *   several pellets (distinct keys, distinct damage).
 * - The shot's launch/knockdown control is deduped once per
 *   (Epoch, Source, ShotId, Target) - the control key drops the pellet index -
 *   so a shotgun cannot bypass the once-per-shot control limit.
 * - The capacity bounds ACTIVE shot instances (hard cap). A full ledger
 *   refuses a new attack instance with CapacityFull and never evicts a live
 *   key to make room - evicting an active shot's keys would silently break
 *   idempotency and let the same hit apply twice. An instance already open
 *   keeps accepting its own pellet events and control.
 * - EndShot tombstones one shot instance: its event keys and control keys are
 *   released back to the pool (the ledger returns toward its baseline) while
 *   a late re-send of that shot's keys is still refused (ShotAlreadyEnded).
 *   Tombstones hold no keys, do not count against the capacity and are wiped
 *   by Reset.
 * - Requests naming an unknown source, an unregistered target or an old epoch
 *   are refused with an explicit reason before anything is recorded; a
 *   half-empty key never enters the ledger at all (UnusableKey). Every
 *   refusal carries its reason - never a bare bool.
 * - Reset (session end / world rebuild / unload) returns the ledger to its
 *   empty baseline; the world owner calls it alongside the registry's
 *   BeginNextWorldEpoch.
 */

/**
 * Why the ledger refused a hit event or a shot control. Append only - never
 * renumber, so log lines and tests keep their meaning. 0 always means
 * "accepted".
 */
enum class EHitLedgerRejectReason : uint8
{
	/** The request was accepted. */
	None = 0,
	/** The key is not a usable event key (a 64-bit id field is 0). */
	UnusableKey = 1,
	/** The key names an epoch other than the registry's current generation. */
	StaleEpoch = 2,
	/** The key's source entity is not a registered entity. */
	UnknownSource = 3,
	/** The key's target entity is not a registered entity. */
	UnknownTarget = 4,
	/** The exact event key was already recorded. */
	DuplicateEvent = 5,
	/** The shot's control on that target was already accepted (pellet-insensitive). */
	DuplicateControl = 6,
	/** The shot instance already ended; its keys stay refused past the end. */
	ShotAlreadyEnded = 7,
	/** The hard capacity was reached; the new attack instance was refused, active ones keep working. */
	CapacityFull = 8,
	/** No registry is bound; the ledger cannot verify epoch or entities. */
	UnboundRegistry = 9
};

/**
 * One shot instance inside the ledger, identified by (Epoch, Source, ShotId) -
 * the pellet-free prefix of the event key. Tracks the active/ended lifecycle
 * and the per-surface counts.
 */
struct FCombatLedgerShotKey
{
	FCombatEpoch Epoch = InvalidCombatEpoch;
	FEntityId SourceEntityId = InvalidCombatEntityId;
	FShotId ShotId = InvalidCombatShotId;

	bool operator==(const FCombatLedgerShotKey& Other) const
	{
		return Epoch == Other.Epoch
			&& SourceEntityId == Other.SourceEntityId
			&& ShotId == Other.ShotId;
	}
};

inline uint32 GetTypeHash(const FCombatLedgerShotKey& Key)
{
	uint32 Hash = ::GetTypeHash(Key.Epoch);
	Hash = HashCombine(Hash, ::GetTypeHash(Key.SourceEntityId));
	Hash = HashCombine(Hash, ::GetTypeHash(Key.ShotId));
	return Hash;
}

/** Builds the shot key (pellet-free prefix) of an event key. */
inline FCombatLedgerShotKey MakeCombatLedgerShotKey(const FCombatEventKey& Key)
{
	FCombatLedgerShotKey ShotKey;
	ShotKey.Epoch = Key.Epoch;
	ShotKey.SourceEntityId = Key.EntityId;
	ShotKey.ShotId = Key.ShotId;
	return ShotKey;
}

/**
 * One shot's control on one target: (Epoch, Source, ShotId, Target). The
 * pellet index is deliberately absent - every pellet of a shot maps to the
 * same control decision for the same target.
 */
struct FCombatShotControlKey
{
	FCombatEpoch Epoch = InvalidCombatEpoch;
	FEntityId SourceEntityId = InvalidCombatEntityId;
	FShotId ShotId = InvalidCombatShotId;
	FTargetId TargetId = InvalidCombatTargetId;

	bool operator==(const FCombatShotControlKey& Other) const
	{
		return Epoch == Other.Epoch
			&& SourceEntityId == Other.SourceEntityId
			&& ShotId == Other.ShotId
			&& TargetId == Other.TargetId;
	}
};

inline uint32 GetTypeHash(const FCombatShotControlKey& Key)
{
	uint32 Hash = ::GetTypeHash(Key.Epoch);
	Hash = HashCombine(Hash, ::GetTypeHash(Key.SourceEntityId));
	Hash = HashCombine(Hash, ::GetTypeHash(Key.ShotId));
	Hash = HashCombine(Hash, ::GetTypeHash(Key.TargetId));
	return Hash;
}

/** Builds the control key of an event key (the pellet index is dropped). */
inline FCombatShotControlKey MakeCombatShotControlKey(const FCombatEventKey& Key)
{
	FCombatShotControlKey ControlKey;
	ControlKey.Epoch = Key.Epoch;
	ControlKey.SourceEntityId = Key.EntityId;
	ControlKey.ShotId = Key.ShotId;
	ControlKey.TargetId = Key.TargetId;
	return ControlKey;
}

/**
 * The shot instance's ledger state: lifecycle flag plus the per-surface
 * counts (bookkeeping for logs and tests - the dedup sets are the authority).
 */
struct FCombatLedgerShotState
{
	/** True once EndShot tombstoned the instance. */
	bool bEnded = false;

	/** Hit events recorded for this shot while it was active. */
	int32 RecordedEventCount = 0;

	/** Shot controls accepted for this shot while it was active. */
	int32 AcceptedControlCount = 0;
};

/**
 * The stable EventId of a hit event, derived from the COMPLETE key - never
 * from a pointer value (interface contract section 2). FNV-1a 64-bit over the
 * five key fields in member declaration order with fixed byte order, so the
 * derivation is deterministic across runs and platforms. Owned by 010.
 */
constexpr uint64 DeriveCombatEventId(const FCombatEventKey& Key)
{
	const uint64 Fields[5] = {
		static_cast<uint64>(Key.Epoch),
		static_cast<uint64>(Key.EntityId),
		static_cast<uint64>(Key.ShotId),
		static_cast<uint64>(Key.PelletIndex),
		static_cast<uint64>(Key.TargetId)
	};
	uint64 Hash = 0xcbf29ce484222325ULL; // FNV-1a 64-bit offset basis
	for (const uint64 Field : Fields)
	{
		for (int32 Byte = 0; Byte < 8; ++Byte)
		{
			Hash ^= (Field >> (Byte * 8)) & 0xFFULL;
			Hash *= 0x100000001b3ULL; // FNV-1a 64-bit prime
		}
	}
	return Hash;
}

class FHitLedger
{
public:
	/** Default hard capacity of active shot instances. */
	static constexpr int32 DefaultCapacity = 256;

	FHitLedger() = default;
	explicit FHitLedger(const FCombatEntityRegistry* InRegistry);

	/** Binds the registry used to verify epochs and source/target entities. */
	void BindRegistry(const FCombatEntityRegistry* InRegistry);

	/** The bound registry, or null while unbound (every request is refused). */
	const FCombatEntityRegistry* GetBoundRegistry() const;

	/** Sets the hard capacity of active shot instances (clamped to at least 1). */
	void SetCapacity(int32 NewCapacity);

	int32 GetCapacity() const;

	/** Number of recorded hit event keys (the dedup face). */
	int32 GetNumRecordedEvents() const;

	/** Number of accepted shot controls. */
	int32 GetNumAcceptedControls() const;

	/** Number of active (not yet ended) shot instances - what the capacity bounds. */
	int32 GetNumActiveShots() const;

	/** Active instances plus ended tombstones (tombstones do not consume capacity). */
	int32 GetNumTrackedShots() const;

	/**
	 * Records one hit event: true when the key is new and accepted, false with
	 * an explicit OutReason otherwise (unusable key, unbound registry, stale
	 * epoch, unknown source/target, ended shot, duplicate key, full capacity).
	 */
	bool TryRecordHitEvent(const FCombatEventKey& Key, EHitLedgerRejectReason& OutReason);

	/**
	 * Accepts the shot's control on the key's target: true exactly once per
	 * (Epoch, Source, ShotId, Target) - the pellet index of the carrying event
	 * key is ignored. Refusal reasons mirror TryRecordHitEvent, with
	 * DuplicateControl for an already-accepted control.
	 */
	bool TryAcceptShotControl(const FCombatEventKey& Key, EHitLedgerRejectReason& OutReason);

	/** True when the exact event key is recorded. */
	bool HasRecordedEvent(const FCombatEventKey& Key) const;

	/** True when the shot's control on the key's target was accepted (pellet-insensitive). */
	bool HasAcceptedControl(const FCombatEventKey& Key) const;

	/**
	 * Ends one shot instance: its event keys and control keys are released
	 * back to the pool and the instance is tombstoned (late re-sends of its
	 * keys stay refused). Returns false for an unknown shot, an already ended
	 * shot, a stale epoch or an unbound registry.
	 */
	bool EndShot(FCombatEpoch Epoch, FEntityId SourceEntityId, FShotId ShotId);

	/** Returns the tracked state of a shot instance, if present (active or tombstone). */
	bool FindTrackedShot(FCombatEpoch Epoch, FEntityId SourceEntityId, FShotId ShotId, FCombatLedgerShotState& OutState) const;

	/**
	 * Unload: drops every event, control and tombstone - the full empty
	 * baseline. Called on session end / world rebuild (next to the registry's
	 * BeginNextWorldEpoch) / unload.
	 */
	void Reset();

private:
	/** Shared gate: usability, bound registry, current epoch, known source and target. */
	bool ValidateRequest(const FCombatEventKey& Key, EHitLedgerRejectReason& OutReason) const;

	/** Live instances plus tombstones, keyed by the pellet-free shot identity. */
	TMap<FCombatLedgerShotKey, FCombatLedgerShotState> Shots;

	/** The dedup face: every accepted hit event key. */
	TSet<FCombatEventKey> RecordedEvents;

	/** Every accepted shot control (pellet-free per target). */
	TSet<FCombatShotControlKey> AcceptedControls;

	/** Hard capacity of active instances; a full ledger refuses new instances, never evicts. */
	int32 Capacity = DefaultCapacity;

	/** Number of tracked instances that are not ended. */
	int32 NumActiveShots = 0;

	/** The registry used for epoch/entity verification (not owned). */
	const FCombatEntityRegistry* Registry = nullptr;
};

// Compile-time pins: the ledger's keys stay pointer-free trivially copyable
// value snapshots, safe for TSet/TMap and for persisted log lines.
static_assert(std::is_trivially_copyable_v<FCombatLedgerShotKey>, "FCombatLedgerShotKey must stay a trivially copyable value snapshot");
static_assert(std::is_trivially_copyable_v<FCombatShotControlKey>, "FCombatShotControlKey must stay a trivially copyable value snapshot");
static_assert(std::is_trivially_copyable_v<FCombatLedgerShotState>, "FCombatLedgerShotState must stay a trivially copyable value snapshot");
static_assert(!std::is_pointer_v<decltype(FCombatLedgerShotKey::Epoch)>, "FCombatLedgerShotKey must stay pointer-free");
static_assert(!std::is_pointer_v<decltype(FCombatShotControlKey::TargetId)>, "FCombatShotControlKey must stay pointer-free");
