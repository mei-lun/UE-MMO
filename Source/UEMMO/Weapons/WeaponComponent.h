#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Delegates/DelegateCombinations.h"
#include "Misc/Guid.h"
#include "Templates/Function.h"

#include "../Combat/System/CombatEntityRegistry.h"

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
 * M5-022: why one TryFire request was refused; only grows, never renumbers
 * (0 = success). Every refusal leaves the mount completely unchanged - no
 * round deducted, no sequence allocated, no cooldown started ("准入失败或生成
 * 预留失败不扣弹/不增sequence/不冷却").
 */
enum class EFireReject : uint8
{
	None = 0,
	/** No ranged weapon binding is active (unbound mount, or the bound weapon is melee). */
	NoWeaponBound = 1,
	/** No healthy catalog pair is mounted (the latched catalog error). */
	CatalogUnavailable = 2,
	/** The wielder is dead. */
	OwnerDead = 3,
	/** The injected menu-open gate reports an open menu. */
	MenuOpen = 4,
	/** The injected hit-stun gate reports a running stun. */
	HitStunned = 5,
	/** A reload window is open for the active weapon. */
	Reloading = 6,
	/** The fire-rate cooldown has not elapsed on the injected fire clock. */
	CooldownActive = 7,
	/** The fire book (the M5-019 magazine slot) holds no rounds. */
	MagazineEmpty = 8,
	/** The intent repeats an already committed local shot sequence. */
	DuplicateIntent = 9,
	/** A previously committed shot is still in flight (ReleaseFire not called). */
	ShotInProgress = 10,
	/** The intent does not match the active binding or the sequence book. */
	StaleIntent = 11,
	/** The fire policy refused the capacity reservation. */
	ReservationFailed = 12,
	/** The common ActionSequence allocation was refused by the fire registry. */
	SequenceAllocationFailed = 13,
	/** The guarded magazine write refused (defensive path, unreachable by construction). */
	AmmoWriteFailed = 14,
};

/**
 * M5-022: the caller-side fire intent. WeaponInstanceId must name the active
 * binding; LocalShotSequence is the caller's module-local monotonic counter
 * (association only - it never fills the public FCombatEventKey::ShotId,
 * which only the 010 allocator may mint). One commit per strictly increasing
 * local sequence per binding generation; a repeat is the named DuplicateIntent
 * refusal ("重复意图不多发").
 */
struct FFireIntent
{
	/** The weapon item instance the trigger pull addresses. */
	FGuid WeaponInstanceId;

	/** The caller's local monotonic shot counter; must be >= 1 and strictly increasing. */
	uint64 LocalShotSequence = 0;
};

/**
 * M5-022: the read-only snapshot of one committed shot, handed to the policy
 * and broadcast to the downstream consumers (projectile/027, hit pipeline).
 * Epoch/SourceEntityId/ShotId come from the component's fire registry (the
 * M5-010 allocator semantics); LocalShotSequence only associates.
 */
struct FShotContext
{
	/** The fire registry generation the shot was minted in. */
	FCombatEpoch Epoch = InvalidCombatEpoch;

	/** The registered fire source entity (the wielder's fire-lineage identity). */
	FEntityId SourceEntityId = InvalidCombatEntityId;

	/** The common ActionSequence value (M5-010 allocator); never a local counter. */
	FShotId ShotId = InvalidCombatShotId;

	/** The caller's local sequence, association only. */
	uint64 LocalShotSequence = 0;

	/** The weapon item instance that fired. */
	FGuid WeaponInstanceId;

	/** The resolved weapon definition. */
	FName WeaponDefinitionId;

	/** Delivery mode of the resolved weapon (hitscan/projectile fire here). */
	EWeaponFireMode FireMode = EWeaponFireMode::Melee;

	/** Damage profile the weapon itself applies (hitscan). */
	FName DamageProfileId;

	/** Shared reserve kind the shot consumed. */
	FName AmmoId;

	/** Pellets per shot (1 = single pellet). */
	int32 PelletCount = 1;

	/** Projectile definition per shot (projectile weapons). */
	FName ProjectileId;

	/** Hitscan maximum hit distance in centimeters. */
	float RangeCm = 0.0f;

	/** Maximum angular deviation of one pellet in degrees. */
	float SpreadDegrees = 0.0f;

	/** Fire-clock seconds at the commit (provenance only; the clock is injected). */
	double CommittedAtSeconds = 0.0;
};

/**
 * M5-022: the policy's capacity decision for one candidate shot. The
 * reservation pre-claims the downstream delivery capacity (projectile slots
 * and/or raycast requests) BEFORE the round is deducted; a refusal aborts the
 * transaction with zero side effects ("先预留全体弹丸或射线请求容量，再一次扣弹").
 */
struct FCapacityReservation
{
	/** True when the full capacity of the candidate shot is pre-claimed. */
	bool bGranted = false;

	/** Projectile actor slots pre-claimed (0 for hitscan). */
	int32 ProjectileSlots = 0;

	/** Raycast request slots pre-claimed (0 for projectile fire). */
	int32 RaycastSlots = 0;

	/** Human-readable refusal detail; empty when granted. */
	FString RejectDetail;
};

/**
 * M5-022: the fire policy mount point (the shared interface this card freezes;
 * 023/024/025 add their own policy classes in their own files and register
 * them through SetFirePolicy). The component calls the policy inside the
 * TryFire transaction: reserve (pre-claim capacity) -> commit (the shot is
 * real) or abort (roll the reservation back) -> release (the caller ended the
 * shot's in-flight window). Plain C++ interface: policies are logic, not
 * UObjects, and never own component state.
 */
class IFirePolicy
{
public:
	virtual ~IFirePolicy() = default;

	/** Pre-claims the delivery capacity of the candidate shot. */
	virtual FCapacityReservation ReserveShotCapacity(const FShotContext& Candidate) = 0;

	/** Confirms a reserved shot as committed (the reservation becomes bookkeeping). */
	virtual void CommitShot(const FShotContext& Committed) = 0;

	/** Rolls a reservation back (a post-reserve transaction step failed). */
	virtual void AbortReservation(const FShotContext& Aborted) = 0;

	/** Ends the committed shot's bookkeeping (the caller called ReleaseFire). */
	virtual void NotifyShotReleased(const FShotContext& Ended) = 0;
};

/** Outcome of one TryFire request: explicit decision + reason, never a bare bool. */
struct FFireOutcome
{
	/** True only when the shot committed (round deducted, sequence minted, cooldown started). */
	bool bFired = false;

	/** None when bFired; otherwise the named refusal cause. */
	EFireReject Reject = EFireReject::None;

	/** Human-readable detail naming the offending id/field; empty on success. */
	FString RejectDetail;

	/** The committed shot snapshot; only meaningful when bFired. */
	FShotContext Shot;
};

/** Broadcast exactly once per committed shot with its read-only context. */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnWeaponShotCommitted, const FShotContext& /*Shot*/);

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
 *
 * M5-022 additions (single-fire transaction): TryFire commits one shot through
 * the ordered gate chain (catalog, binding, death, menu, hit-stun, reload,
 * cooldown, in-flight, intent identity, magazine) -> policy capacity
 * reservation -> one guarded round deduction -> the common ActionSequence
 * allocation. Every refused step leaves the mount unchanged (a post-deduction
 * allocation failure rolls the round back). The fire identity space is this
 * component's own FCombatEntityRegistry ("fire registry"): the ShotId values
 * it mints are genuine M5-010 common ActionSequence allocations of the fire
 * lineage - monotonic per (Epoch, SourceEntity), never the caller's local
 * counter - while the future unified adjudication space (merging with the
 * CombatComponent's private world registry, M5-013 integration form) keeps
 * this an injection point: replacing/bridging the registry member is the
 * whole change, the transaction shape does not move. The fire book is the
 * M5-019 magazine slot (guarded SetInstanceRounds writes); the M5-021 model
 * stays the reload ledger and its refills are synced into the fire book by
 * the component-level reload wrappers below (the M5-021 report delegated
 * exactly this combination decision to the 020/022 integration).
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

	// ------------------------------------------------------------------
	// M5-022: the single-fire transaction surface (contract section 3).
	// ------------------------------------------------------------------

	/**
	 * Registers the component owner as the fire source of the fire registry
	 * (the actor may be null for a logic-only standalone mount - the record
	 * then resolves to null but still holds an entity id). The minted entity id
	 * feeds every FShotContext; TryFire without a registered source refuses
	 * with SequenceAllocationFailed (zero side effects). Returns the invalid id
	 * when the registry refuses (duplicate owner registration).
	 */
	FEntityId RegisterFireSource(const FCombatEntityMetadata& Metadata);

	/**
	 * Swaps the active fire policy (non-owning; the caller keeps the instance
	 * alive and unregisters before it dies). Null restores the component-owned
	 * M5-022 default (the semiauto single-ray FSingleFirePolicy). 023/024/025
	 * register their policies here.
	 */
	void SetFirePolicy(IFirePolicy* Policy);

	/**
	 * Injects the fire clock (GameClockSeconds semantics: the caller supplies
	 * the seconds; cooldown pacing reads it, the component never reads a World
	 * timer itself). Unbound, the default reads the owner world's
	 * GetTimeSeconds (0.0 without a world).
	 */
	void SetFireClockProvider(TFunction<double()> Provider);

	/** Injects the menu-open gate (true = fire refused with MenuOpen). Default: never open (production wiring belongs to M5-033/034). */
	void SetMenuOpenPredicate(TFunction<bool()> Predicate);

	/** Injects the hit-stun gate (true = fire refused with HitStunned). Default: never stunned (production wiring belongs to M5-033/034). */
	void SetHitStunPredicate(TFunction<bool()> Predicate);

	/**
	 * The single-shot transaction. Gate order: catalog, binding (no ranged
	 * weapon), death, menu, hit-stun, reload, cooldown, in-flight shot, intent
	 * identity (stale/duplicate), empty magazine, then the policy reservation,
	 * then one guarded round deduction, then the common ActionSequence
	 * allocation. Commit mints the FShotContext, starts the cooldown
	 * (60/FireRateRpm on the injected fire clock) and broadcasts
	 * OnShotCommitted exactly once. Every refusal is named and side-effect
	 * free ("失败零副作用").
	 */
	FFireOutcome TryFire(const FFireIntent& Intent);

	/**
	 * Ends the committed shot's in-flight window (the semiauto trigger
	 * release): the next TryFire is admissible once its gates pass. Idempotent
	 * no-op while no shot is in flight.
	 */
	void ReleaseFire();

	/**
	 * Component-level reload entry (contract section 3): opens the reload
	 * window of the active ranged weapon at caller-clock NowSeconds. The
	 * window lives on a fresh per-cycle model slot (the M5-021 model has no
	 * deduction API, so a repeated refill cycle needs a fresh slot identity
	 * per cycle; the stale slots are tiny and bounded by the reload count).
	 * Named refusals: no active ranged binding / dead owner (UnknownInstance),
	 * full fire book (MagazineFull), empty reserve (NoReserveAmmo), plus the
	 * model's own window refusals verbatim. Success moves nothing until
	 * CompleteReload. While a window is open, TryFire refuses (Reloading).
	 */
	FAmmoReloadOutcome BeginReload(double NowSeconds, double DurationSeconds);

	/**
	 * Closes the open reload window and atomically moves granted =
	 * min(capacity - loaded, reserve) rounds from the shared pool into the
	 * magazine - then syncs the fire book (the M5-019 slot) to the refilled
	 * count, the only path that ever refills it. Refused (named, unchanged)
	 * without an open window.
	 */
	FAmmoReloadOutcome CompleteReload();

	/**
	 * Component-level cancel (contract section 3): closes the open reload
	 * window without transferring (equipment switch / death semantics).
	 * Best-effort void per the contract sketch; the idempotent no-op case is
	 * an already-closed window.
	 */
	void CancelReload(FName Reason);

	/** The last committed shot snapshot; false before the first commit. */
	bool GetLastShotContext(FShotContext& OutShot) const;

	/** Number of committed shots this mount made (only successful commits count). */
	int32 GetCommittedShotCount() const { return CommittedShotCount; }

	/** True between a successful TryFire and its ReleaseFire. */
	bool IsShotInFlight() const { return bShotInFlight; }

	/** The fire-clock seconds the next shot becomes admissible (0 before the first commit). */
	double GetNextFireTimeSeconds() const { return NextFireTimeSeconds; }

	/** The fire registry's current generation (context provenance). */
	FCombatEpoch GetFireEpoch() const { return FireRegistry.GetCurrentEpoch(); }

	/** The registered fire source entity id; invalid before RegisterFireSource. */
	FEntityId GetFireSourceEntityId() const { return FireSourceEntityId; }

	/** The fire registry (read-only; the fire-lineage identity space of this mount). */
	const FCombatEntityRegistry& GetFireRegistry() const { return FireRegistry; }

	/** Broadcast exactly once per committed shot (downstream consumers: 027 projectiles, the hit pipeline). */
	FOnWeaponShotCommitted OnShotCommitted;

private:
	/**
	 * Tears the active binding down: unbind (rounds parked), close the open
	 * reload window (nothing transferred), generation bump. Idempotent on an
	 * already unbound mount. M5-022: also closes the per-cycle reload slot,
	 * voids the in-flight shot and resets the fire books.
	 */
	void TeardownActiveBinding();

	/** The injected fire clock seconds (provider, owner world, or 0.0). */
	double ResolveFireClockSeconds() const;

	/** Best-effort close of the per-cycle reload window (nothing transfers). */
	void CloseActiveReloadWindow();

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

	// -- M5-022: the fire transaction state --------------------------------

	/** The component-owned default policy (the semiauto single-ray FSingleFirePolicy). */
	TUniquePtr<IFirePolicy> OwnedDefaultPolicy;

	/** The active non-owning policy; never null after the constructor. */
	IFirePolicy* ActivePolicy = nullptr;

	/**
	 * The fire-lineage identity space (an M5-010 registry owned by this mount).
	 * The future unified adjudication space merges by replacing/bridging this
	 * member - the injection point the class comment documents.
	 */
	FCombatEntityRegistry FireRegistry;

	/** The registered fire source entity id; invalid until RegisterFireSource. */
	FEntityId FireSourceEntityId = InvalidCombatEntityId;

	/** The injected fire clock (GameClockSeconds semantics; empty = owner world / 0.0). */
	TFunction<double()> FireClockProvider;

	/** The injected menu-open gate; empty = never open. */
	TFunction<bool()> MenuOpenPredicate;

	/** The injected hit-stun gate; empty = never stunned. */
	TFunction<bool()> HitStunPredicate;

	/** True between a successful TryFire and its ReleaseFire. */
	bool bShotInFlight = false;

	/** True after the first commit of the current binding generation. */
	bool bHasCommittedShot = false;

	/** The last committed local sequence (DuplicateIntent keys on it). */
	uint64 LastCommittedLocalSequence = 0;

	/** Number of committed shots this mount made. */
	int32 CommittedShotCount = 0;

	/** The last committed shot snapshot (valid when bHasCommittedShot). */
	FShotContext LastShotContext;

	/** Fire-clock seconds the next shot becomes admissible (0 = admissible now). */
	double NextFireTimeSeconds = 0.0;

	/** Per-cycle reload counter; mints the fresh model slot identity per window. */
	uint64 ReloadCycle = 0;

	/** The open reload window's model slot identity; invalid while no window. */
	FGuid ActiveReloadSlotId;
};
