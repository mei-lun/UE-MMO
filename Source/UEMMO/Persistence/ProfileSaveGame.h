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
 * CurrentSchemaVersion (= 1) by the serializer. A loader that finds an unknown
 * version (any value other than 1) must refuse the load AND refuse to wipe or
 * overwrite the underlying file - a future schema belongs to a newer game
 * build, so the safe behavior is to surface the error and keep the original
 * data (FProfileSerializer implements exactly that; the A/B slot service that
 * touches files is M3-014).
 *
 * Field notes (schema_version=1):
 * - SchemaVersion: serialized format version, currently always 1.
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
 */
UCLASS()
class UEMMO_API UProfileSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	/**
	 * The schema version this build writes and understands. Bump on any
	 * breaking field change; loaders must reject anything they do not know
	 * (currently anything != 1) without destroying the save file.
	 */
	static constexpr int32 CurrentSchemaVersion = 1;

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
};
