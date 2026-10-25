#pragma once

#include "CoreMinimal.h"

#include "../Items/InventoryModel.h"
#include "../Items/ItemDefinition.h"
#include "../Weapons/AmmoTypes.h"

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
	 * SchemaVersion is not a value this build understands (anything other
	 * than 1 or 2). A version above CurrentSchemaVersion means the file was
	 * written by a NEWER build: the load is refused and the original file
	 * must be preserved untouched - wiping or overwriting a future-schema
	 * save is never acceptable. Schema version 1 is NOT an error: it loads
	 * through the explicit M5-046 v1->v2 migration (see MigrateV1ToV2).
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
	DanglingEquipReference,

	/**
	 * M5-046: a DefinitionId is neither a definition this build knows nor a
	 * registered legacy alias in the supplied FProfileItemManifest. Only
	 * enforced when a manifest is supplied (nullptr = no content gating, the
	 * M3-013 structural behavior). The message distinguishes the two cases:
	 * a registered legacy alias is HISTORY COMPATIBILITY (mapped to its
	 * current id and restored), an unknown id is CORRUPTION or
	 * newer-than-known content and is rejected with the save preserved.
	 */
	UnknownItemDefinition,

	/**
	 * M5-046: a WeaponMagazines/ReserveAmmoPools row fails its own field
	 * rules (invalid ammo id text, capacity outside 1..MaxAmmoMagazineSize,
	 * loaded rounds beyond the row's capacity, reserve rounds beyond the
	 * row's own cap or MaxAmmoReserve). Rejected, never clamped.
	 */
	InvalidWeaponState,

	/**
	 * M5-046: the same magazine InstanceId or the same reserve AmmoId
	 * appears in more than one row - set semantics must be restored, never
	 * silently deduplicated.
	 */
	DuplicateWeaponState,

	/** M5-046: a magazine's InstanceId does not exist among the INVENTORY instances. */
	DanglingMagazineReference
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
 * M5-046: one explicit legacy alias row of the legacy_item_aliases manifest
 * (interface contract section 7 / section 10.9 of the M5 baseline review).
 * The concrete case is frozen history: the M3-011 HUD debug staging minted
 * instances under the item id "accessory_training" (values 1/0/5) before the
 * authoritative Data/items.json table standardized the kind as
 * "charm_training" (0/0/20). Saves written through that path hold a LEGAL
 * historical id - not corruption - and the manifest is the explicit,
 * data-driven mapping that carries it forward. Extending means appending a
 * row; nothing else changes.
 */
struct FLegacyItemAliasEntry
{
	/** The historical id as it appears in old saves (e.g. "accessory_training"). */
	FName LegacyId;

	/** The authoritative current id the legacy kind resolves to (e.g. "charm_training"). */
	FName CurrentId;
};

/**
 * M5-046: the explicit, data-driven item manifest of the save contract.
 * Three responsibilities, all additive and all explicit (nothing is inferred
 * from live subsystems - the serializer stays pure):
 *
 * - KnownDefinitionIds: the closed vocabulary of CURRENT item definition ids
 *   a legitimate save of this build may reference (transcribed from
 *   Data/items.json). With a manifest supplied, any DefinitionId outside
 *   this set and the alias rows is rejected (UnknownItemDefinition) -
 *   distinguishing "historical compatibility" from "corrupt/unknown".
 * - LegacyAliases: the legacy_item_aliases rows (see FLegacyItemAliasEntry).
 *   Validated acyclic, self-alias-free and target-present; the target must
 *   be a known id. "Revision coverage": the alias is sanctioned content
 *   under the current config lineage - the migrated save records the then-
 *   current ConfigRevision, and the target being a known id IS the coverage
 *   check (a future config that removes the target makes the manifest
 *   invalid and the migration refuses; the alias never silently falls back
 *   to an unknown id).
 * - WeaponAmmoSeeds: the one-time v1 weapon-state initialization table. The
 *   migration must initialize the MISSING weapon state exactly once, but the
 *   serializer is pure and holds no catalog - so the manifest explicitly
 *   carries, per FIREARMS item definition, the reserve kind and magazine
 *   capacity its instances initialize with (values transcribed from
 *   Data/CombatSystem/weapons.json through the items.json
 *   weapon_definition_id mapping). Melee definitions carry NO row: a sword
 *   has no magazine, and an absent row is the explicit "no magazine"
 *   decision, not an omission.
 *
 * The default manifest (Get) mirrors the shipped content; tests may build
 * their own to pin the rejection branches.
 */
struct FProfileItemManifest
{
	/** One seed row value keyed by item definition (see WeaponAmmoSeeds). */
	struct FWeaponAmmoSeed
	{
		/** The reserve kind the magazine refills from (e.g. "ammo_cell"). */
		FName AmmoId;

		/** The magazine capacity (weapons.json magazine_size; 1..MaxAmmoMagazineSize). */
		int32 MagazineCapacity = 0;
	};

	/** Current item definition ids a legitimate save may reference. */
	TArray<FName> KnownDefinitionIds;

	/** Legacy alias rows (accessory_training -> charm_training in the default). */
	TArray<FLegacyItemAliasEntry> LegacyAliases;

	/** One-time weapon-state seeds for firearm item definitions (melee: absent). */
	TMap<FName, FWeaponAmmoSeed> WeaponAmmoSeeds;

	/**
	 * Structural validation of the manifest itself: every alias names two
	 * non-none distinct ids, no legacy id duplicates or doubles as a current
	 * id, every alias target is a known definition (目标存在), and the alias
	 * graph is acyclic (无环). Returns false and names the offending row.
	 */
	bool Validate(FString& OutProblem) const;

	/** True when Id is one of the known current definition ids. */
	bool IsKnownDefinition(FName Id) const;

	/**
	 * Resolves one saved DefinitionId through the manifest: known ids resolve
	 * to themselves, registered legacy aliases resolve to their current id;
	 * returns false for anything else (the UnknownItemDefinition case).
	 */
	bool ResolveDefinitionId(FName SavedId, FName& OutCurrentId) const;

	/**
	 * The build-wide default manifest: the shipped Data/items.json vocabulary,
	 * the frozen accessory_training -> charm_training alias (section 10.9)
	 * and the firearm seeds transcribed from the Data/CombatSystem tables.
	 * Static storage on purpose: the serializer and every load path resolve
	 * through the SAME explicit table.
	 */
	static const FProfileItemManifest& Get();
};

/**
 * M5-046: the optional weapon-state outputs of FromSaveGame. The base M3-013
 * outputs stay untouched in shape (existing call sites compile unchanged);
 * callers that care about the v2 weapon state pass this bundle and receive
 * the restored magazines, reserve pools, the one-time initialization marker
 * and the recorded config revision - every one of them exactly what the save
 * carried (a v1 input returns the MIGRATED state: seeded-empty magazines,
 * marker true). Never written on a failed load.
 */
struct FProfileWeaponStateOut
{
	/** Restored per-instance magazines (v1 input: the migration's seeded rows). */
	TArray<FWeaponMagazineSaveEntry> WeaponMagazines;

	/** Restored shared reserve pools (empty unless the save carried rounds). */
	TArray<FAmmoReserveSaveEntry> ReserveAmmoPools;

	/** The one-time marker (true for any migrated v1 save). */
	bool bWeaponStateInitialized = false;

	/** The recorded config revision provenance (empty when unknown). */
	FString ConfigRevision;
};

/**
 * M3-013 (extended by M5-046): pure, World-free profile save serializer
 * (interface contract section 8). Both directions are plain functions over
 * value data: no World, no subsystem, no ticking, no file I/O (the A/B slot
 * file service that owns USaveGame <-> disk is M3-014; it can call these two
 * functions unchanged).
 *
 * ToSaveGame(...) maps trusted in-memory profile state into a fresh
 * UProfileSaveGame (schema_version stamped with CurrentSchemaVersion = 2).
 * It performs no validation: the live state is the trusted source. The
 * Snapshot carries identity/progress; the Inventory parameter is the
 * authoritative item source (the snapshot's own inventory copy is ignored on
 * purpose - one source of truth per save). The M5-046 weapon-state inputs
 * (magazines, reserve pools, the one-time marker, the config revision
 * provenance) default to their "unset" values so every pre-existing call
 * site compiles and behaves identically.
 *
 * FromSaveGame(...) restores the state only if Validate(...) accepts the
 * save:
 * - schema_version must be known: 2 (this build's layout) or 1 (the legacy
 *   layout, restored through the explicit M5-046 migration below); an
 *   unknown version (e.g. a version 3 written by a future build) is
 *   REJECTED and the save object is never modified - the underlying file
 *   must be preserved, never auto-wiped;
 * - Level must be inside 1..10;
 * - every instance snapshot must parse (M3-001 FromStringSnapshot, which also
 *   rejects non-finite NaN/Inf stats) and InstanceIds must be unique across
 *   inventory AND pending drafts;
 * - the inventory must fit FInventoryModel::Capacity (a save claiming more is
 *   corrupt; the overflow is never silently dropped);
 * - every equipped slot must be a closed EItemSlot value, bound at most once,
 *   and its InstanceId must exist in the inventory set (dangling references
 *   are rejected, never silently dropped);
 * - M5-046: every v2 weapon-state row must satisfy its closed ranges, carry
 *   unique keys and reference an INVENTORY instance (dangling magazine rows
 *   are rejected); with a manifest supplied, every DefinitionId must resolve
 *   (known id or registered legacy alias - anything else is rejected).
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
	 * FGuid> becomes an array of slot/InstanceId pairs. M5-046: the weapon
	 * state travels verbatim from the given rows (defaults = the pre-wiring
	 * "unset" state: no magazines, no pools, marker false, no revision).
	 */
	static UProfileSaveGame* ToSaveGame(const FProfileSnapshot& Snapshot,
		const FInventoryModel& Inventory,
		const TArray<FPendingReward>& PendingRewards,
		const TSet<uint64>& AppliedSettlementIds,
		const TMap<EItemSlot, FGuid>& EquippedMap,
		const TArray<FWeaponMagazineSaveEntry>& WeaponMagazines = TArray<FWeaponMagazineSaveEntry>(),
		const TArray<FAmmoReserveSaveEntry>& ReserveAmmoPools = TArray<FAmmoReserveSaveEntry>(),
		bool bInWeaponStateInitialized = false,
		const FString& InConfigRevision = FString(),
		UObject* Outer = nullptr);

	/**
	 * Validates the save standalone (no restore, no side effects). Returns
	 * true when the save is loadable by this build; otherwise fills OutError
	 * with a code and a field-naming reason. Accepts schema versions 1 and 2;
	 * the optional ItemManifest additionally gates every DefinitionId against
	 * the known-vocabulary + legacy-alias resolution (nullptr = no content
	 * gating, the pure structural behavior).
	 */
	static bool Validate(const UProfileSaveGame* Save, FProfileLoadError& OutError,
		const FProfileItemManifest* ItemManifest = nullptr);

	/**
	 * Restores profile state from a validated save. Returns false (and fills
	 * OutError) without touching ANY Out* parameter when the save is null or
	 * fails Validate. On success the Out* parameters are fully overwritten:
	 * the snapshot carries identity, Level/XP, the level-formula base stats
	 * (derived values are never stored in the save - equipment stats are
	 * re-derived by the gameplay layer once EquippedIds are re-applied) and a
	 * copy of the restored inventory; the remaining containers mirror the
	 * saved arrays/sets.
	 *
	 * M5-046 extensions (all optional trailing parameters - every existing
	 * call site compiles unchanged):
	 * - a schema_version 1 save is restored through the explicit v1->v2
	 *   migration (ItemManifest supplied, or the build-wide default when
	 *   null) - the input save object is never modified;
	 * - ItemManifest additionally gates every DefinitionId (known id or
	 *   registered legacy alias; legacy ids are rewritten to their current id
	 *   inside the restored instances, everything else byte-preserved);
	 * - CurrentAmmoTypes, when supplied, runs the ConfigRevision
	 *   compatibility gate (IsWeaponStateCompatibleWithAmmoConfig): an ammo
	 *   kind the saved weapon state references but the current config no
	 *   longer defines, a magazine capacity shrunk below the stored load or a
	 *   reserve cap shrunk below the stored rounds REFUSE the load (the save
	 *   is provenance, the state would be destroyed) while a purely additive
	 *   config change loads and lets the caller rebind;
	 * - OutWeaponState, when supplied, receives the restored magazines,
	 *   reserve pools, the one-time marker and the recorded revision.
	 */
	static bool FromSaveGame(const UProfileSaveGame* Save,
		FProfileSnapshot& OutSnapshot,
		FInventoryModel& OutInventory,
		TArray<FPendingReward>& OutPendingRewards,
		TSet<uint64>& OutAppliedSettlementIds,
		TMap<EItemSlot, FGuid>& OutEquippedMap,
		FProfileLoadError& OutError,
		const FProfileItemManifest* ItemManifest = nullptr,
		const TMap<FName, FAmmoType>* CurrentAmmoTypes = nullptr,
		FProfileWeaponStateOut* OutWeaponState = nullptr);

	/**
	 * M5-046: the explicit one-time v1 -> v2 migration (interface contract
	 * section 7: the first migration channel). Refuses anything but a valid
	 * schema_version 1 input (a corrupt v1 save is refused by the same field
	 * rules a load applies; a v2 input is a caller bug). On success returns a
	 * FRESH UProfileSaveGame (the caller owns it) with:
	 * - every M3-013 base field preserved verbatim (identity, Level/XP,
	 *   inventory, equipment references, pending drafts, applied ids - no
	 *   re-forge, no re-roll, no silent deletion);
	 * - every registered legacy alias (accessory_training) rewritten to its
	 *   current id (charm_training) inside the instance snapshots, with
	 *   InstanceId/RolledStats/Level/RollSeed and every equipment reference
	 *   preserved byte-for-byte; non-aliased snapshots stay byte-identical;
	 * - the weapon state initialized EXACTLY ONCE: an explicit magazine row
	 *   (LoadedRounds 0 - mounting never mints ammunition) per inventory
	 *   instance whose definition carries a manifest seed, no rows for melee
	 *   definitions, no reserve rows (pools provision lazily and empty), and
	 *   bWeaponStateInitialized stamped true so no later path mistakes the
	 *   state for "never initialized" and re-runs an initial-ammo grant;
	 * - ConfigRevision set to MigrationConfigRevision (the caller stamps the
	 *   then-current revision; empty when unknown).
	 * Returns null on any refusal (OutError names the reason) and NEVER
	 * touches LegacySave - the original save stays byte-identical either way.
	 */
	static UProfileSaveGame* MigrateV1ToV2(const UProfileSaveGame& LegacySave,
		const FProfileItemManifest& ItemManifest,
		FProfileLoadError& OutError,
		const FString& MigrationConfigRevision = FString(),
		UObject* Outer = nullptr);

	/**
	 * M5-046: the ConfigRevision compatibility classifier (interface contract
	 * section 7: "ConfigRevision是溯源值，不自动拒绝所有旧档" - the revision
	 * STRINGS are never compared; the decision compares the SAVED weapon
	 * state against the CURRENT ammo config). "New config only added rows and
	 * the old constraints still hold" loads and lets the caller rebind:
	 * - every ammo kind the saved magazines/pools reference must still exist
	 *   in CurrentAmmoTypes (a removed kind orphans rounds - refuse);
	 * - every current magazine capacity must still hold the stored
	 *   LoadedRounds (a shrunk magazine must not silently spill rounds);
	 * - every current reserve cap must still hold the stored Rounds.
	 * Any violation refuses (OutError names the row and the field); an empty
	 * weapon state is trivially compatible. This is a pure query - neither
	 * input is modified.
	 */
	static bool IsWeaponStateCompatibleWithAmmoConfig(const UProfileSaveGame& Save,
		const TMap<FName, FAmmoType>& CurrentAmmoTypes,
		FProfileLoadError& OutError);
};
