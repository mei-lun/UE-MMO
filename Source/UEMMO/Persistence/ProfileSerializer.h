#pragma once

#include "CoreMinimal.h"

#include "../Items/InventoryModel.h"
#include "../Items/ItemDefinition.h"

#include "ProfileSaveGame.h"

class UProfileSaveGame;
struct FProfileSnapshot;
struct FPendingReward;

/**
 * Error classification of FProfileSerializer::Validate/FromSaveGame. Every
 * failure mode of the load path has its own code, and the accompanying text
 * (FProfileLoadError.Message) always names the offending field, so a rejected
 * load is diagnosable instead of a silent partial restore.
 */
enum class EProfileLoadError : uint8
{
	/** No error (load succeeded). */
	None = 0,

	/** The UProfileSaveGame pointer was null. */
	NullSaveGame,

	/**
	 * SchemaVersion is not a value this build understands (currently anything
	 * other than 1). Version > 1 means the file was written by a NEWER build:
	 * the load is refused and the original file must be preserved untouched -
	 * wiping or overwriting a future-schema save is never acceptable.
	 */
	UnsupportedSchema,

	/** Level is outside the closed 1..10 range (field "Level"). */
	InvalidLevel,

	/** The settings placeholder float is not finite (field "MasterVolume"). */
	NonFiniteSetting,

	/**
	 * An item snapshot string failed to parse (fields "InventoryInstances[i]"
	 * or "PendingRewards[i].Items[j]"); the message carries the parser's own
	 * field-naming reason, which includes non-finite (NaN/Inf) stat rejection.
	 */
	BadInstanceSnapshot,

	/** The same InstanceId appears more than once across stored instances. */
	DuplicateInstanceId,

	/** More inventory snapshot strings than FInventoryModel::Capacity. */
	InventoryOverfull,

	/** An equipped slot byte is not one of the three closed EItemSlot values. */
	InvalidEquipSlot,

	/** The same equipment slot is bound more than once. */
	DuplicateEquipSlot,

	/** An equipped InstanceId does not exist in the inventory set (dangling). */
	DanglingEquipReference
};

/** Error code plus a human-readable reason that names the offending field. */
struct FProfileLoadError
{
	/** Machine-readable classification; None on success. */
	EProfileLoadError Code = EProfileLoadError::None;

	/** Empty on success; otherwise names the field and the rejection reason. */
	FString Message;
};

/**
 * M3-013: pure, World-free profile save serializer (interface contract
 * section 8). Both directions are plain functions over value data: no World,
 * no subsystem, no ticking, no file I/O (the A/B slot file service that owns
 * USaveGame <-> disk is M3-014; it can call these two functions unchanged).
 *
 * ToSaveGame(...) maps trusted in-memory profile state into a fresh
 * UProfileSaveGame (schema_version=1). It performs no validation: the live
 * state is the trusted source. The Snapshot carries identity/progress; the
 * Inventory parameter is the authoritative item source (the snapshot's own
 * inventory copy is ignored on purpose - one source of truth per save).
 *
 * FromSaveGame(...) restores the state only if Validate(...) accepts the save:
 * - schema_version must be known (currently 1); an unknown version (e.g. a
 *   version 2 written by a future build) is REJECTED and the save object is
 *   never modified - the underlying file must be preserved, never auto-wiped;
 * - Level must be inside 1..10;
 * - every instance snapshot must parse (M3-001 FromStringSnapshot, which also
 *   rejects non-finite NaN/Inf stats) and InstanceIds must be unique across
 *   inventory AND pending drafts;
 * - the inventory must fit FInventoryModel::Capacity (a save claiming more is
 *   corrupt; the overflow is never silently dropped);
 * - every equipped slot must be a closed EItemSlot value, bound at most once,
 *   and its InstanceId must exist in the inventory set (dangling references
 *   are rejected, never silently dropped).
 *
 * On ANY failure the Out* parameters are left completely untouched and the
 * error names the offending field - a rejected load can never surface as a
 * half-restored profile or a silently-deleted item masquerading as success.
 */
struct FProfileSerializer
{
	/**
	 * Maps profile state into a fresh UProfileSaveGame (schema_version stamped
	 * with UProfileSaveGame::CurrentSchemaVersion). Outer may be null (the
	 * transient package is used); no World is needed, so pure-logic tests can
	 * call this directly. Item instances become their M3-001 ToStringSnapshot
	 * strings, the TSet<uint64> becomes a plain array, the TMap<EItemSlot,
	 * FGuid> becomes an array of slot/InstanceId pairs.
	 */
	static UProfileSaveGame* ToSaveGame(const FProfileSnapshot& Snapshot,
		const FInventoryModel& Inventory,
		const TArray<FPendingReward>& PendingRewards,
		const TSet<uint64>& AppliedSettlementIds,
		const TMap<EItemSlot, FGuid>& EquippedMap,
		UObject* Outer = nullptr);

	/**
	 * Validates the save standalone (no restore, no side effects). Returns
	 * true when the save is loadable by this build; otherwise fills OutError
	 * with a code and a field-naming reason.
	 */
	static bool Validate(const UProfileSaveGame* Save, FProfileLoadError& OutError);

	/**
	 * Restores profile state from a validated save. Returns false (and fills
	 * OutError) without touching ANY Out* parameter when the save is null or
	 * fails Validate. On success the Out* parameters are fully overwritten:
	 * the snapshot carries identity, Level/XP, the level-formula base stats
	 * (derived values are never stored in the save - equipment stats are
	 * re-derived by the gameplay layer once EquippedIds are re-applied) and a
	 * copy of the restored inventory; the remaining containers mirror the
	 * saved arrays/sets.
	 */
	static bool FromSaveGame(const UProfileSaveGame* Save,
		FProfileSnapshot& OutSnapshot,
		FInventoryModel& OutInventory,
		TArray<FPendingReward>& OutPendingRewards,
		TSet<uint64>& OutAppliedSettlementIds,
		TMap<EItemSlot, FGuid>& OutEquippedMap,
		FProfileLoadError& OutError);
};
