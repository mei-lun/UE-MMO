#pragma once

#include "CoreMinimal.h"

#include <type_traits>

#include "CombatEventTypes.generated.h"

/**
 * M5-002: frozen public contracts of the data-driven combat expansion
 * (M5 interface contract section 0). This header is the single definition
 * site of the shared combat event key, the id aliases, the hit decision
 * enum and the local tick type. It is pure value types, constants and
 * enums: no .cpp, no UObject members, no World/Actor pointers, no behavior.
 *
 * Downstream owners (interface contract section 1) implement strategies in
 * their own new files and never edit this header:
 * - 003/004 freeze damage/weapon/projectile value types on top of these.
 * - 010 owns the world entity registry: epochs, entity ids, dedup ledger and
 *   the common ActionSequence allocation per (Epoch, SourceEntity).
 * - 012 owns the single hit application entry that consumes the key and the
 *   decision enum.
 */

// ===========================================================================
// Key types (section 0.1)
//
// All ids are value-semantic integers: comparable, hashable, trivially
// copyable, safe to persist in a ledger or a log line. A raw pointer value is
// never an id.
// ===========================================================================

/**
 * Combat generation epoch: a globally increasing counter minted exactly once
 * per combat component lifetime (the formalized successor of the existing
 * session-level M1_019_NextInstigatorId pattern). Old epochs are invalidated
 * when the owning world tears down (EndPlay / leave room / retry); a key
 * carrying a stale epoch is rejected as a whole and never merged into the
 * live generation. Starts at 1; 0 is reserved for InvalidCombatEpoch.
 */
using FCombatEpoch = uint64;

/**
 * World-unique id of one registered actor entity (010's registry, interface
 * contract section 2). Compatible with the legacy usage of
 * UObject::GetUniqueID as a session-unique object number until 010 replaces
 * the source; a registry id is never a pointer value. Starts at 1; 0 is
 * reserved for InvalidCombatEntityId.
 */
using FEntityId = uint64;

/**
 * Shot/action sequence identifier inside one epoch. Firearms increment their
 * shot counter per shot, but the value stored in FCombatEventKey::ShotId must
 * come from the COMMON ActionSequence allocated per (Epoch, SourceEntity) by
 * the 010 registry (melee, firearms and vehicles share one monotonic sequence
 * per source). A module-local counter - the legacy melee AttackInstanceId or
 * a firearm-local ShotSequence - is an association value only and never fills
 * the public key slot directly; that keeps two capabilities whose local
 * sequences both equal 1 on different public keys. Starts at 1; 0 is reserved
 * for InvalidCombatShotId.
 */
using FShotId = uint64;

/**
 * Index of one pellet inside a multi-pellet shot: 0..PelletCount-1 (0 for
 * every single-pellet attack). 255 is reserved for InvalidCombatPelletIndex.
 */
using FPelletIndex = uint8;

/**
 * Unique id of the damaged target. Compatible with the legacy
 * `static_cast<uint64>(Target->GetUniqueID())` usage until 010's registry
 * replaces the source; never a pointer value. Starts at 1; 0 is reserved for
 * InvalidCombatTargetId.
 */
using FTargetId = uint64;

/**
 * Value type of the local EpochTick counter owned by one combat component
 * (section 0.4). Monotonically increasing per component; not a global clock,
 * not seconds, not comparable across components and never comparable with
 * GameClockSeconds.
 */
using FCombatTick = uint32;

/**
 * The public combat event key (interface contract sections 2 and 6): five
 * equal fields mean "the same hit", which is exactly the dedup surface.
 * Different sources or epochs can never collide because Epoch and EntityId
 * are part of the key. One shot may damage one target through several pellets
 * (distinct keys, distinct damage), while the shot's launch/knockdown control
 * is deduped once per (Epoch, EntityId, ShotId, TargetId) upstream. The
 * stable EventId is derived from the complete key - never from a pointer
 * value (derivation owned by 010).
 */
struct FCombatEventKey
{
	/** Combat generation that minted the event (stale epochs are rejected). */
	FCombatEpoch Epoch = 0;

	/** World-unique id of the attacking entity (the key's source). */
	FEntityId EntityId = 0;

	/** Common per-source action/shot sequence inside the epoch (starts at 1). */
	FShotId ShotId = 0;

	/** Pellet index inside the shot (0 for single-pellet attacks). */
	FPelletIndex PelletIndex = 0;

	/** Unique id of the damaged target. */
	FTargetId TargetId = 0;

	bool operator==(const FCombatEventKey& Other) const
	{
		return Epoch == Other.Epoch
			&& EntityId == Other.EntityId
			&& ShotId == Other.ShotId
			&& PelletIndex == Other.PelletIndex
			&& TargetId == Other.TargetId;
	}
};

inline uint32 GetTypeHash(const FCombatEventKey& Key)
{
	uint32 Hash = ::GetTypeHash(Key.Epoch);
	Hash = HashCombine(Hash, ::GetTypeHash(Key.EntityId));
	Hash = HashCombine(Hash, ::GetTypeHash(Key.ShotId));
	Hash = HashCombine(Hash, ::GetTypeHash(Key.PelletIndex));
	Hash = HashCombine(Hash, ::GetTypeHash(Key.TargetId));
	return Hash;
}

/**
 * Invalid sentinels: 0 for every 64-bit id (sequences start at 1 so a default
 * value can never be mistaken for a real event) and 255 for the 8-bit pellet
 * index (valid pellets are 0..254).
 */
constexpr FCombatEpoch InvalidCombatEpoch = 0;
constexpr FEntityId InvalidCombatEntityId = 0;
constexpr FShotId InvalidCombatShotId = 0;
constexpr FTargetId InvalidCombatTargetId = 0;
constexpr FPelletIndex InvalidCombatPelletIndex = 255;

constexpr bool IsValidCombatEpoch(FCombatEpoch Epoch) { return Epoch != InvalidCombatEpoch; }
constexpr bool IsValidCombatEntityId(FEntityId Id) { return Id != InvalidCombatEntityId; }
constexpr bool IsValidCombatShotId(FShotId Id) { return Id != InvalidCombatShotId; }
constexpr bool IsValidCombatTargetId(FTargetId Id) { return Id != InvalidCombatTargetId; }
constexpr bool IsValidCombatPelletIndex(FPelletIndex Index) { return Index != InvalidCombatPelletIndex; }

/**
 * True only when every id field of the key is populated. Ledgers and runtime
 * submission entries reject anything else: a late re-sent event, an unknown
 * entity or an unregistered target arrives as an unusable or stale key and
 * must be refused without damage, impulse or presentation.
 */
constexpr bool IsUsableCombatEventKey(const FCombatEventKey& Key)
{
	return IsValidCombatEpoch(Key.Epoch)
		&& IsValidCombatEntityId(Key.EntityId)
		&& IsValidCombatShotId(Key.ShotId)
		&& IsValidCombatTargetId(Key.TargetId);
}

// Compile-time pins: the key stays a pointer-free value snapshot. The event
// key persists no World bare address - a World/Actor pointer member fails the
// build right here.
static_assert(!std::is_pointer_v<decltype(FCombatEventKey::Epoch)>, "FCombatEventKey::Epoch must stay a value type");
static_assert(!std::is_pointer_v<decltype(FCombatEventKey::EntityId)>, "FCombatEventKey::EntityId must stay a value type");
static_assert(!std::is_pointer_v<decltype(FCombatEventKey::ShotId)>, "FCombatEventKey::ShotId must stay a value type");
static_assert(!std::is_pointer_v<decltype(FCombatEventKey::PelletIndex)>, "FCombatEventKey::PelletIndex must stay a value type");
static_assert(!std::is_pointer_v<decltype(FCombatEventKey::TargetId)>, "FCombatEventKey::TargetId must stay a value type");
static_assert(std::is_trivially_copyable_v<FCombatEventKey>, "FCombatEventKey must stay a trivially copyable value snapshot");

// ===========================================================================
// Hit decision (section 0.3)
// ===========================================================================

/**
 * What the unified hit application entry (012) decided about one hit request.
 * Extends the M1-019 hit/refuse semantics: a refusal now carries its reason
 * as an explicit enumerator instead of a bare bool, so callers can log and
 * present rejections without losing information. Append only - never
 * renumber, so persisted logs and ledger entries keep their meaning; a new
 * decision must also extend the coverage list of the
 * UEMMO.Tasks.M5_002.HitDecisionCoverage test.
 */
UENUM(BlueprintType)
enum class EHitDecision : uint8
{
	/** The hit is accepted; damage and control are applied exactly once through the single entry. */
	Apply = 0,
	/** Refused: the target is in an invulnerable window (for example get-up protection refuses the whole hit). */
	Block_Invulnerable = 1,
	/** Refused: the target's policy grants immunity against this damage or control (immune tags, poise). */
	Block_Immune = 2,
	/** Refused before evaluation: stale epoch, unknown entity, duplicate key or a filtered source. */
	Block_Ignore = 3
};

// ===========================================================================
// Ownership ladder, clocks, space and state (sections 0.4-0.6) - frozen here
// ===========================================================================

/**
 * Ownership ladder:
 * - Profile (save data, business values): GameInstance lifetime. Survives
 *   worlds; never references a World actor or transient combat state.
 * - World / session (entity registry, epochs, projectiles, vehicles):
 *   UWorld lifetime. EndPlay, leaving the room and a retry invalidate old
 *   epochs and clear weak references, timers and inputs.
 * - CombatComponent (action state, timers, local tick): Pawn lifetime.
 *   HealthComponent stays the only owner of life.
 *
 * Pointer ban: every event/key type in this header is a value snapshot and
 * none of them may hold a World/Actor raw pointer (static_asserts pin the
 * key above). Consumers resolve actors only through 010's registry weak
 * references. Definitions (003/004) are immutable configuration values and
 * hold no Actor either.
 *
 * Two clocks - never mixed:
 * - GameClockSeconds: UWorld::GetTimeSeconds(), frozen by Pause. Attack
 *   frames/input deadlines, firearm cooldowns, projectile lifetimes and
 *   tracking keep using it exactly as the M5 interface contract section 4
 *   states today.
 * - EpochTick: the owning component's own FCombatTick counter, driven by its
 *   logic tick. Not a global clock, not seconds, not comparable across
 *   components and never compared or converted against GameClockSeconds.
 *   HitStop freezes the action clock and the pinned injected clock exactly as
 *   today (M3 semantics) but does not stop the EpochTick counter; Pause stops
 *   the component tick with the world. FHitRequest::SimulationTick (frozen by
 *   012) is sampled per this rule: take the source component's FCombatTick
 *   value - a local monotonic event label, never a deadline or a server clock.
 *
 * Single state source: CombatComponent owns action state and timers,
 * HealthComponent owns life; acceptance and refusal flow once through the
 * unified entry (012), which bridges the legacy OnHitConfirmed event. Immune
 * and super-armor stay policy flags, never new action states.
 *
 * Space: X lateral, Y depth, Z height; facing is +-1 and mirrors X only.
 */
