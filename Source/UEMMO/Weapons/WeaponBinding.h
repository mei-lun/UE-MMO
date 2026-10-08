#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"

#include "../Items/ItemInstance.h"
#include "../Combat/Data/CombatCatalog.h"
#include "WeaponTypes.h"

/**
 * M5-019: the session weapon binding layer (M5 interface contract section 1,
 * owner 019 "实例绑定"; section 7 "存档与实例"). It connects the existing M3
 * item identity to the M5 weapon behavior through the item_definition_id
 * chain - it never creates a second item instance:
 *
 *   FItemInstance --DefinitionId--> FItemDefinition --WeaponDefinitionId-->
 *   FWeaponDefinition (FCombatCatalog FindWeapon)
 *
 * Guarantees pinned by this card:
 * - Identity preservation: the binding record copies the instance's
 *   InstanceId (FGuid), Level and RollSeed verbatim; binding is a read-side
 *   resolution, the FItemInstance value is never mutated.
 * - Per-instance magazine separation: the magazine slot is keyed by
 *   ItemInstanceId, so two instances of the same weapon definition never
 *   share loaded rounds (contract section 7 "WeaponStateByItemId 仅记录该
 *   实例弹匣").
 * - Rebind keeps the magazine: unbinding parks the rounds and rebinding the
 *   same instance restores them - switching equipment does not initialize new
 *   ammo (contract section 7 "切装备不初始化新弹药"). A first bind starts the
 *   magazine full (the exact loaded/reserve/reload semantics belong to
 *   M5-021's AmmoModel; this card only owns the separated storage slot).
 * - Explicit rejection, never a fallback: an unknown item, a non-weapon slot,
 *   a missing mapping, an unknown weapon id or a duplicate active binding is
 *   refused with a named reason. The training sword is never used as a
 *   silent stand-in.
 * - Session scope only: the registry is a plain value container without any
 *   World/Actor/UObject dependency; persistence (v2 save) belongs to M5-046.
 */

/** Why a bind attempt was refused; only grows, never renumbers (0 = success). */
enum class EWeaponBindingRejectReason : uint8
{
	None = 0,
	/** The instance identity is unusable: all-zero InstanceId or empty DefinitionId. */
	InvalidInstance = 1,
	/** The item definition id is not registered in the item definition catalog. */
	UnknownItemDefinition = 2,
	/** The item definition exists but its slot is not Weapon (armor/accessory). */
	SlotNotWeapon = 3,
	/** The weapon-slot item definition carries no WeaponDefinitionId mapping. */
	NoWeaponMapping = 4,
	/** The mapped weapon id does not resolve in the combat catalog. */
	UnknownWeaponDefinition = 5,
	/** This item instance already holds an active binding (unbind first to rebind). */
	DuplicateInstanceBinding = 6,
};

/**
 * One active binding: the preserved instance identity plus the resolved weapon
 * kind and the per-instance magazine slot. LoadedRounds only moves through
 * SetInstanceRounds (guarded) or unbind/rebind parking; it starts at
 * MagazineCapacity on a first bind and stays 0 for melee weapons.
 */
struct FWeaponBindingRecord
{
	/** The original FItemInstance identity, copied verbatim. */
	FGuid InstanceId;

	/** The item kind this instance came from (the item_definition_id anchor). */
	FName ItemDefinitionId;

	/** The resolved weapon behavior. */
	FName WeaponDefinitionId;

	/** Delivery mode of the resolved weapon. */
	EWeaponFireMode FireMode = EWeaponFireMode::Melee;

	/** The original instance level, copied verbatim. */
	int32 Level = 1;

	/** The original instance roll seed, copied verbatim. */
	int64 RollSeed = 0;

	/** Magazine size of the weapon definition; 0 for melee. */
	int32 MagazineCapacity = 0;

	/** The per-instance magazine slot; 0 for melee weapons. */
	int32 LoadedRounds = 0;
};

/** Outcome of one bind attempt: explicit decision + reason, never a bare bool. */
struct FWeaponBindOutcome
{
	bool bBound = false;

	/** None when bBound; otherwise the named refusal cause. */
	EWeaponBindingRejectReason RejectReason = EWeaponBindingRejectReason::None;

	/** Human-readable detail naming the offending id/field; empty on success. */
	FString RejectDetail;

	/** The binding record; only meaningful when bBound. */
	FWeaponBindingRecord Record;
};

/**
 * The session-scoped binding registry. One registry per session (M5-020 mounts
 * it from the character); it holds the active bindings and the parked magazine
 * slots of unbound instances. Plain value container: no World, no Actor, no
 * timers, no serialization (M5-046 owns the v2 save).
 */
class FWeaponBindingRegistry
{
public:
	/**
	 * Resolves and records the binding of one item instance. Refusal order:
	 * invalid instance, duplicate active binding, unknown item definition,
	 * non-weapon slot, missing weapon mapping, unknown weapon id - every
	 * refusal leaves the registry completely unchanged (no half bindings, no
	 * state writes). A successful first bind starts the magazine full (melee
	 * stays 0); a rebind after UnbindInstance restores the parked rounds.
	 */
	FWeaponBindOutcome BindInstance(const FItemInstance& Instance, const FItemDefinitionCatalog& ItemDefinitions, const FCombatCatalog& WeaponCatalog);

	/**
	 * Removes the active binding of this instance and parks its magazine
	 * rounds so a later rebind does not re-initialize ammo. Returns false
	 * (registry unchanged) when the instance holds no active binding.
	 */
	bool UnbindInstance(const FGuid& InstanceId);

	/** The active binding of this instance, or nullptr when unbound. */
	const FWeaponBindingRecord* FindBinding(const FGuid& InstanceId) const;

	/** Number of active bindings. */
	int32 NumBindings() const;

	/**
	 * Guarded mutation of the per-instance magazine slot (the storage the
	 * M5-021 AmmoModel writes into). Refuses (returns false, registry
	 * unchanged) an unbound instance, a melee weapon (no magazine) and values
	 * outside 0..MagazineCapacity; OutError names the reason when provided.
	 */
	bool SetInstanceRounds(const FGuid& InstanceId, int32 NewLoadedRounds, FString* OutError = nullptr);

	/** Drops every binding and parked state (session teardown / test isolation). */
	void Reset();

private:
	/** Active bindings keyed by the preserved instance identity. */
	TMap<FGuid, FWeaponBindingRecord> Bindings;

	/** Magazine rounds parked by UnbindInstance; keyed by the same identity. */
	TMap<FGuid, int32> DetachedMagazines;
};
