#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "Misc/Guid.h"

#include "ProfileSaveGame.generated.h"

/**
 * Serialized form of one equipment slot binding (interface contract section 8:
 * "Slot->InstanceId, only references inventory instances"). Key/value pair of
 * the future TMap<EItemSlot, FGuid>, stored as an ARRAY of pairs on purpose:
 * the card warns that TMap ordering/serialization inside a USaveGame is
 * fragile, and EItemSlot (Items/ItemDefinition.h) is a plain, unreflected
 * enum class that UHT cannot place in a UPROPERTY - so the slot travels as
 * its underlying uint8 byte and is re-validated as a closed enum value on
 * load (FProfileSerializer rejects out-of-range slot bytes instead of
 * silently coercing them).
 */
USTRUCT()
struct FEquippedSlotSavePair
{
	GENERATED_BODY()

	/** EItemSlot value as its underlying byte (Weapon=0, Armor=1, Accessory=2). */
	UPROPERTY()
	uint8 Slot = 0;

	/** InstanceId of the equipped inventory item; must exist in the inventory set. */
	UPROPERTY()
	FGuid InstanceId;
};

/**
 * Serialized form of one pending reward draft (interface contract section 8,
 * M3-008). Mirrors the plain FPendingReward value struct (RewardService.h,
 * read-only for this card): SettlementId, XP and the pre-generated item
 * instances. The instances travel as their M3-001 ToStringSnapshot strings
 * (one string per FItemInstance, restored via FromStringSnapshot) because
 * FPendingReward and FItemInstance are deliberately engine-free value types
 * that UHT cannot embed in a UPROPERTY. The roll is already frozen inside the
 * snapshot strings, so a load never re-rolls anything.
 */
USTRUCT()
struct FPendingRewardSaveEntry
{
	GENERATED_BODY()

	/** Business identity of the settlement this draft belongs to. */
	UPROPERTY()
	uint64 SettlementId = 0;

	/** XP granted by the settlement (design: 50 per cleared room). */
	UPROPERTY()
	int32 XP = 0;

	/** One ToStringSnapshot() string per pre-generated item, in draft order. */
	UPROPERTY()
	TArray<FString> ItemSnapshots;
};

/**
 * Serialized form of one per-instance weapon magazine entry (interface
 * contract section 7: "WeaponStateByItemId 仅记录该实例弹匣"). Mirrors the
 * M5-021 FMagazineState slot as a plain value row: the magazine is keyed by
 * the weapon's ITEM instance identity (FGuid InstanceId - the same identity
 * the inventory and EquippedIds carry, never a copy of the item), names the
 * shared reserve kind it refills from, and stores the capacity plus the
 * rounds currently loaded. Stored as an ARRAY of rows on purpose (same
 * reasoning as FEquippedSlotSavePair: TMap ordering inside a USaveGame is
 * fragile); the loader validates uniqueness of InstanceId and the closed
 * numeric ranges instead of silently coercing them.
 *
 * Melee weapons carry NO row at all (a magazine that cannot hold a round is
 * not a state), and an entry whose InstanceId does not exist among the
 * INVENTORY instances is rejected on load as dangling - an orphaned magazine
 * must never masquerade as restored state.
 */
USTRUCT()
struct FWeaponMagazineSaveEntry
{
	GENERATED_BODY()

	/** Item instance identity this magazine belongs to (must exist in the inventory). */
	UPROPERTY()
	FGuid InstanceId;

	/** The shared reserve kind this magazine refills from (e.g. "ammo_cell"). */
	UPROPERTY()
	FName AmmoId;

	/** Maximum rounds the magazine holds; validated 1..MaxAmmoMagazineSize on load. */
	UPROPERTY()
	int32 Capacity = 0;

	/** Rounds currently in the magazine; validated 0..Capacity on load. */
	UPROPERTY()
	int32 LoadedRounds = 0;
};

/**
 * Serialized form of one shared reserve pool row (interface contract
 * section 7: "ReserveAmmoByType 记录共享备弹"). Mirrors the M5-021
 * FAmmoReservePool: the pool is keyed by the ammo kind id and holds the
 * configured cap plus the rounds currently in the shared stock. Two ammo
 * kinds never touch each other's pool; duplicate AmmoId rows are rejected on
 * load, and Rounds beyond the row's own MaxReserve is corrupt state.
 */
USTRUCT()
struct FAmmoReserveSaveEntry
{
	GENERATED_BODY()

	/** The shared reserve kind (e.g. "ammo_cell"); unique across rows. */
	UPROPERTY()
	FName AmmoId;

	/** Maximum shared stock of this ammo kind at save time; 0..MaxAmmoReserve. */
	UPROPERTY()
	int32 MaxReserve = 0;

	/** Rounds currently in the shared pool; validated 0..MaxReserve on load. */
	UPROPERTY()
	int32 Rounds = 0;
};

/**
 * Serialized form of one per-room clear record (schema_version=3, M6-003).
 * Pure value row: the room id (a Data/rooms.json room_id), how many CLEARED
 * runs finished there and the unix-seconds timestamp of the last one. The
 * record is bookkeeping only - nothing in the game reads it as a gate (the
 * free-select decision), and the menu display is the M6-002 card.
 */
USTRUCT()
struct FRoomClearSaveEntry
{
	GENERATED_BODY()

	/** Business identity of the cleared room (e.g. "room_training_01"). */
	UPROPERTY()
	FName RoomId;

	/** How many cleared runs finished in this room (>= 1 in any valid row). */
	UPROPERTY()
	int32 ClearCount = 0;

	/** Unix-seconds timestamp of the most recent clear (>= 0). */
	UPROPERTY()
	int64 LastClearUnixSeconds = 0;
};

/**
 * M3-013: the versioned profile save structure (interface contract section 8).
 * A USaveGame whose every field is a plain VALUE type - no Actor pointers, no
 * resource object addresses, no World references, no TObjectPtr of any kind.
 * The structural guarantee is enforced twice: by construction (this header
 * contains zero pointer/reference members) and by an automation test that
 * walks the reflected property tree and rejects any object-reference property
 * (ProfileSerializationTests.cpp). The identity of items is the persistent
 * FGuid InstanceId / FGuid CharacterId, never a UE asset path.
 *
 * Schema versioning: SchemaVersion is a serialized field stamped with
 * CurrentSchemaVersion (= 2 since M5-046) by the serializer. M5-046 opened the
 * FIRST migration channel: a loader that finds schema_version 1 restores it
 * through the explicit FProfileSerializer::MigrateV1ToV2 (one-time weapon-
 * state initialization, legacy alias rewrite; the input is never modified),
 * and anything else it does not know must refuse the load AND refuse to wipe
 * or overwrite the underlying file - a future schema belongs to a newer game
 * build, so the safe behavior is to surface the error and keep the original
 * data (FProfileSerializer implements exactly that; the A/B slot service that
 * touches files is M3-014).
 *
 * Field notes (schema_version=1 - the M3-013 base, fully preserved):
 * - SchemaVersion: serialized format version; 1 = pre-M5-046 legacy layout.
 * - CharacterId: persistent business identity of the character (FGuid).
 * - Level/XP: progression. The derived stats (MaxHP/Attack/Defense) are
 *   deliberately NOT stored: they are recomputed from the level formulas
 *   (and, after equipment is re-attached, the full FStatCalculator row) so a
 *   save can never fossilize stale or in-combat values.
 * - InventoryInstanceSnapshots: one M3-001 ToStringSnapshot() string per
 *   inventory instance, in insertion order; InstanceIds travel inside the
 *   strings and are validated for uniqueness and finiteness on load.
 * - EquippedIds: array of slot pairs (see FEquippedSlotSavePair); every
 *   InstanceId must exist in the inventory set on load (dangling references
 *   are rejected, never silently dropped).
 * - PendingRewards: unclaimed settlement drafts (see FPendingRewardSaveEntry).
 * - AppliedSettlementIds: settlement ids already claimed (the TSet<uint64> of
 *   UProfileSubsystem travels as a plain array; TSet is not directly
 *   serializable). Set semantics are restored on load.
 * - MasterVolume: settings placeholder so the schema carries a settings slot
 *   from day one; no settings domain exists yet, the serializer always keeps
 *   it at its default (1.0) and reports this as a known placeholder.
 *
 * Field notes (schema_version=2 - the M5-046 additions; interface contract
 * section 7 "v2新增这些值类型和revision，A/B完整性计算覆盖新字段"):
 * - WeaponMagazines: per-INSTANCE magazines (see FWeaponMagazineSaveEntry);
 *   a v1 save has no rows, and the one-time v1->v2 migration seeds an
 *   explicit EMPTY magazine for every firearm item instance (LoadedRounds 0 -
 *   mounting never mints ammunition) so nothing stays implicit.
 * - ReserveAmmoPools: shared reserve stock per ammo kind (see
 *   FAmmoReserveSaveEntry); provisioned lazily and EMPTY by the session
 *   layer, so a v1 save (and a migrated one) carries no rows until rounds
 *   actually exist.
 * - bWeaponStateInitialized: the one-time marker that the weapon-state domain
 *   of this profile has been settled (true after the v1->v2 migration and
 *   after the new-game weapon flow first stores state). A restore path reads
 *   it as "the containers are authoritative - never run the initial-ammo
 *   grant against this profile again"; without it an all-empty container set
 *   would be indistinguishable from "never initialized".
 * - ConfigRevision: provenance value - the FCombatCatalog::GetConfigRevision
 *   content signature of the config this state was last saved under (empty
 *   when unknown, e.g. right after a migration). It never gates a load by
 *   itself (interface contract section 7: "ConfigRevision是溯源值，不自动拒绝
 *   所有旧档"); the compatibility decision compares the SAVED weapon state
 *   against the CURRENT config (FProfileSerializer::
 *   IsWeaponStateCompatibleWithAmmoConfig), refusing only changes that would
 *   destroy state (ammo kind removed, magazine capacity shrunk below the
 *   stored load) and keeping the original file untouched.
 */
UCLASS()
class UEMMO_API UProfileSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	/**
	 * The schema version this build writes and understands. M5-046 bumped it
	 * from 1 to 2 (the first migration channel), M6-003 from 2 to 3: loaders
	 * accept 3 (the current layout), 2 (restored through the explicit
	 * FProfileSerializer::MigrateV2ToV3 - a verbatim copy with an explicitly
	 * EMPTY RoomClears set) and 1 (restored through FProfileSerializer::
	 * MigrateV1ToV2, which preserves every id/stat/claim and initializes the
	 * missing weapon state exactly once); anything else is refused without
	 * destroying the save file.
	 */
	static constexpr int32 CurrentSchemaVersion = 3;

	/** The pre-M5-046 legacy schema version; the migration channel's input. */
	static constexpr int32 LegacySchemaVersion = 1;

	/** The M5-046 pre-M6-003 layout; loads through the v2->v3 migration. */
	static constexpr int32 SchemaVersion2 = 2;

	/** Serialized schema version; ToSaveGame stamps CurrentSchemaVersion. */
	UPROPERTY()
	int32 SchemaVersion = CurrentSchemaVersion;

	/** Persistent character identity; never a UE asset path. */
	UPROPERTY()
	FGuid CharacterId;

	/** Character level (1..10 for a real profile; validated on load). */
	UPROPERTY()
	int32 Level = 0;

	/** XP toward the next level. */
	UPROPERTY()
	int32 XP = 0;

	/** One ToStringSnapshot() string per inventory instance, insertion order. */
	UPROPERTY()
	TArray<FString> InventoryInstanceSnapshots;

	/** Equipment slot bindings; every InstanceId must exist in the inventory. */
	UPROPERTY()
	TArray<FEquippedSlotSavePair> EquippedIds;

	/** Unclaimed settlement reward drafts, in draft order. */
	UPROPERTY()
	TArray<FPendingRewardSaveEntry> PendingRewards;

	/** Settlement ids whose rewards were already claimed (TSet travels as array). */
	UPROPERTY()
	TArray<uint64> AppliedSettlementIds;

	/** Settings placeholder (master volume 0..1); see class comment. */
	UPROPERTY()
	float MasterVolume = 1.0f;

	// -- schema_version=2 additions (M5-046) ------------------------------------

	/**
	 * Per-instance weapon magazines (interface contract section 7:
	 * "WeaponStateByItemId 仅记录该实例弹匣"). Melee weapons carry no row; a
	 * v1 save carries no rows at all until its one-time migration seeds them.
	 */
	UPROPERTY()
	TArray<FWeaponMagazineSaveEntry> WeaponMagazines;

	/**
	 * Shared reserve stock per ammo kind (interface contract section 7:
	 * "ReserveAmmoByType 记录共享备弹"). The session layer provisions pools
	 * lazily and empty, so rows appear only once rounds actually exist.
	 */
	UPROPERTY()
	TArray<FAmmoReserveSaveEntry> ReserveAmmoPools;

	/**
	 * One-time weapon-state marker: true once the weapon-state domain of this
	 * profile was settled (by the v1->v2 migration or the new-game weapon
	 * flow). A restore path must never run the initial-ammo grant against a
	 * profile whose containers are already authoritative. See class comment.
	 */
	UPROPERTY()
	bool bWeaponStateInitialized = false;

	/**
	 * Config revision provenance (FCombatCatalog::GetConfigRevision of the
	 * config this state was last saved under); empty when unknown. Never a
	 * load gate by itself - see the class comment for the compatibility rule.
	 */
	UPROPERTY()
	FString ConfigRevision;

	// -- schema_version=3 additions (M6-003) --------------------------------------

	/**
	 * Per-room clear records of the multi-level profile (M6-003; the user's
	 * 2026-10-29 free-select decision: records only, no gating anywhere). One
	 * row per CLEARED room id - the row appears with the first cleared run and
	 * counts every later clear. A v2 (and a migrated v1) save carries no rows;
	 * the one-time v2->v3 migration copies the rest of the state verbatim and
	 * seeds an explicitly EMPTY record set. The A/B payload digest covers the
	 * rows like every other field (the M5-046 digest-coverage rule).
	 */
	UPROPERTY()
	TArray<FRoomClearSaveEntry> RoomClears;
};
