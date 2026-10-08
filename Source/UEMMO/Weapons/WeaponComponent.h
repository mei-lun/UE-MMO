#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Misc/Guid.h"

#include "AmmoModel.h"
#include "WeaponBinding.h"
#include "WeaponComponent.generated.h"

class FCombatCatalog;
struct FItemDefinitionCatalog;
struct FItemInstance;

/**
 * Why one ApplyEquippedWeapon request was refused at the component level; only
 * grows, never renumbers (0 = success). Registry-level refusals surface as
 * BindingRefused with the named EWeaponBindingRejectReason carried in the
 * detail text - the component never swallows or re-maps a named reason.
 */
enum class EWeaponMountReject : uint8
{
	None = 0,
	/** No usable catalog pair is mounted (unmounted, unbuilt catalog or a previously failed mount). */
	CatalogUnavailable = 1,
	/** The binding registry refused (invalid instance, unknown definition, no mapping, ...). */
	BindingRefused = 2,
	/** The resolved melee weapon does not carry exactly the legacy four-attack set. */
	UnsupportedMeleeAttackSet = 3,
	/** The resolved ranged weapon references an ammo kind the catalog does not define. */
	AmmoUnavailable = 4,
};

/** Outcome of one ApplyEquippedWeapon request: explicit decision + reason. */
struct FWeaponMountOutcome
{
	/**
	 * True when the request was honored - either a weapon binding is now active
	 * (bWeaponBound) or the explicit "no weapon" state was applied. A refused
	 * request changes nothing: the previous state stays exactly as it was.
	 */
	bool bSucceeded = false;

	/** True only when a weapon binding is active after the call. */
	bool bWeaponBound = false;

	/** None when bSucceeded; otherwise the named refusal cause. */
	EWeaponMountReject Reject = EWeaponMountReject::None;

	/** Human-readable detail naming the offending id/field; empty on success. */
	FString RejectDetail;

	/** The binding record; only meaningful when bWeaponBound. */
	FWeaponBindingRecord Record;
};

/**
 * M5-020: the production weapon mount (M5 interface contract section 1,
 * owner 019..025 "WeaponComponent"; section 7 "存档与实例"). One component per
 * pawn; it mounts the session registries and the production catalogs and
 * resolves the equipped weapon-slot item into an active weapon binding:
 *
 *   equipment slot -> FItemInstance -> (M5-019 registry) -> FWeaponBindingRecord
 *                                    -> (M5-021 model)    -> magazine/reserve/reload
 *
 * Guarantees pinned by this card:
 * - Production catalog mounting: MountCatalogs accepts a built FCombatCatalog
 *   (non-empty ConfigRevision - a never-built catalog is refused) and an item
 *   definition catalog. A refused mount latches the explicit catalog-error
 *   state: firing stays disabled until a successful mount replaces it. There
 *   is no fallback weapon and no default catalog.
 * - Equipment load/bind: ApplyEquippedWeapon (BeginPlay, equipment change,
 *   revive re-entry) tears the previous binding down FIRST (unbind parks the
 *   magazine rounds, an open reload window is closed, the binding generation
 *   bumps) and then binds the new instance. A refused request after the
 *   teardown leaves the mount explicitly unbound - never half-bound.
 * - Explicit refusal, never a fake success: an unavailable catalog, a registry
 *   rejection, a melee weapon without exactly the legacy four-attack set and a
 *   ranged weapon whose ammo kind does not resolve are all named refusals.
 * - Rebind keeps the books: the same instance re-applied restores its parked
 *   magazine rounds (M5-019) and never re-initializes its magazine or its
 *   shared reserve pool (M5-021) - switching equipment adds no ammunition.
 * - Stale callbacks lose effect: every teardown bumps the binding generation;
 *   a callback captured before a switch/death can check IsBindingGenerationCurrent
 *   and must drop its work (the M5-022 fire scheduling will key on this).
 * - Melee stays legacy: binding a melee weapon never rewrites the X/Z attack
 *   chain - the resolved attack id set must be exactly the legacy four-attack
 *   set or the bind is refused ("配置不支持的AttackSet明确拒绝").
 * - No World/timer use of its own: the component is a state surface; clocks
 *   enter through the caller (M5-021 model semantics), persistence belongs to
 *   M5-046 and the fire scheduling to M5-022.
 */
UCLASS(ClassGroup = (Combat), meta = (BlueprintSpawnableComponent))
class UEMMO_API UWeaponComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UWeaponComponent();

	/**
	 * M5-020: mounts the production catalogs (non-owning pointers; the mounter
	 * owns the values and must outlive the mount - the pawn owns its session
	 * catalog members). The weapon catalog must be built (non-empty revision);
	 * the item catalog may be empty (binds then refuse UnknownItemDefinition).
	 * Refusal latches the catalog-error state (firing disabled, named error)
	 * and never swaps the previously mounted pointers.
	 */
	bool MountCatalogs(const FCombatCatalog* InWeaponCatalog, const FItemDefinitionCatalog* InItemCatalog, FString* OutError = nullptr);

	/**
	 * The production load/bind entry (BeginPlay, equipment change, revive
	 * re-entry). WeaponInstance == nullptr applies the explicit "no weapon"
	 * state (previous binding torn down first). Refusal order: catalog
	 * unavailable, registry rejection, unsupported melee attack set, ammo
	 * unavailable. Every refused request leaves the mount unbound and the
	 * registry unchanged (the teardown side effects - parked rounds, closed
	 * reload window, bumped generation - are the intended switch semantics).
	 */
	FWeaponMountOutcome ApplyEquippedWeapon(const FItemInstance* WeaponInstance);

	/**
	 * Death teardown (the pawn's death lifecycle calls this): closes any open
	 * reload window without transferring rounds and invalidates pending
	 * callbacks (generation bump). The binding identity survives - the revive
	 * re-entry re-applies the equipment and restores the parked magazine.
	 */
	void NotifyOwnerDied();

	/** Revive re-entry: re-enables the mount after a death teardown. */
	void NotifyOwnerRevived();

	/** The active binding, or nullptr while no weapon is bound. */
	const FWeaponBindingRecord* GetActiveBinding() const;

	/** The session ammo model (M5-021); the M5-022 fire scheduling drives it. */
	FAmmoModel& GetAmmoModel() { return Ammo; }
	const FAmmoModel& GetAmmoModel() const { return Ammo; }

	/** The session binding registry (M5-019), read-only view. */
	const FWeaponBindingRegistry& GetRegistry() const { return Registry; }

	/** The mutable session registry view (the guarded M5-019 writes: round consumption, ammo bookkeeping). */
	FWeaponBindingRegistry& GetRegistry() { return Registry; }

	/**
	 * True only when a ranged weapon is bound, the wielder is alive and a
	 * healthy catalog pair is mounted. Melee weapons authorize the legacy X/Z
	 * chain (untouched) and never this flag; a catalog error, an unbound mount
	 * or a dead wielder all disable firing explicitly.
	 */
	bool IsFireAuthorized() const;

	/** The ConfigRevision of the mounted weapon catalog; empty while unmounted. */
	const FString& GetMountedRevision() const { return MountedRevision; }

	/** True after a refused mount (or before any mount); firing is disabled. */
	bool IsCatalogError() const { return bCatalogError; }

	/** The current binding generation (bumps on every teardown). */
	uint64 GetBindingGeneration() const { return BindingGeneration; }

	/**
	 * False for every generation captured before the last teardown: a stale
	 * fire/reload callback must drop its work instead of firing a residual
	 * bullet for an unbound weapon.
	 */
	bool IsBindingGenerationCurrent(uint64 Generation) const { return Generation == BindingGeneration; }

private:
	/**
	 * Tears the active binding down: unbind (rounds parked), close the open
	 * reload window (nothing transferred), generation bump. Idempotent on an
	 * already unbound mount.
	 */
	void TeardownActiveBinding();

	/** Session binding registry (M5-019 mount). */
	FWeaponBindingRegistry Registry;

	/** Session ammo account book (M5-021 mount). */
	FAmmoModel Ammo;

	/** Non-owning weapon catalog (the pawn owns the session value). */
	const FCombatCatalog* WeaponCatalog = nullptr;

	/** Non-owning item definition catalog (the pawn owns the session value). */
	const FItemDefinitionCatalog* ItemCatalog = nullptr;

	/** Mounted catalog revision (empty while unmounted). */
	FString MountedRevision;

	/** The explicit catalog-error latch (refused mount / catalog unavailable). */
	bool bCatalogError = true;

	/** True from spawn until NotifyOwnerDied; NotifyOwnerRevived restores it. */
	bool bOwnerAlive = true;

	/** The active bound instance identity; invalid while unbound. */
	FGuid ActiveInstanceId;

	/** Bumps on every teardown; stale callbacks key on it. */
	uint64 BindingGeneration = 0;
};
