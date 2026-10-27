#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Templates/Function.h"

#include "VehicleTypes.h"
#include "../Combat/System/CombatEventTypes.h"
#include "../Weapons/WeaponComponent.h"

#include <type_traits>

#include "VehicleWeaponMountComponent.generated.h"

class FSeatOccupancyModel;
class UWeaponComponent;

/**
 * Why one Configure request was refused; only grows, never renumbers
 * (0 = success). Every refusal names the offending id/field in the detail
 * text ("缺挂点/缺武器错误可定位") and leaves the mount completely
 * unconfigured - a refused request never half-loads a turret.
 */
enum class EVehicleMountConfigReject : uint8
{
	None = 0,
	/** No turret weapon component was handed over (null UWeaponComponent). */
	WeaponComponentMissing = 1,
	/** The mount definition failed ValidateVehicleMountDefinition (the detail names the fields). */
	MountDefinitionInvalid = 2,
	/** The mount component has no owner actor: a turret pivot without a vehicle body cannot aim. */
	OwnerActorMissing = 3,
	/** The turret weapon component carries no healthy mounted catalog pair. */
	CatalogUnavailable = 4,
	/** The mount's TurretWeaponId does not resolve in the mounted weapon catalog. */
	WeaponDefinitionMissing = 5,
	/** The resolved weapon is melee: a turret mount fires ranged weapons only. */
	MeleeTurretWeapon = 6,
	/** The resolved weapon's ammo kind does not resolve in the mounted catalog. */
	AmmoUnavailable = 7,
};

/**
 * Why one CanFire/TryFire request was refused; only grows, never renumbers
 * (0 = success). Every refusal is side-effect free: no round deducted, no
 * sequence allocated, no cooldown started (the M5-022 transaction semantics
 * this component forwards into).
 */
enum class EVehicleMountFireReject : uint8
{
	None = 0,
	/** Finalize ran (vehicle destroyed / room teardown); the mount is terminal. */
	MountFinalized = 1,
	/** Configure never succeeded (no turret weapon, no angles, nothing mounted). */
	NotConfigured = 2,
	/** The turret weapon holds no active ranged binding of the mount's TurretWeaponId. */
	TurretWeaponNotBound = 3,
	/** No vehicle fire source is registered (call RefreshVehicleFireSource first). */
	FireSourceMissing = 4,
	/** The registered fire source is not a live record of the current fire-registry epoch (world rebuild). */
	FireSourceStale = 5,
	/** No operator occupies the operator seat; player fire is forbidden ("无操作者禁止玩家开火"). */
	NoOperator = 6,
	/** The aim carries a non-finite yaw or pitch. */
	AimInvalid = 7,
	/** The pitch is outside [MinPitchDegrees, MaxPitchDegrees]. */
	PitchOutOfRange = 8,
	/**
	 * Reserved for a yaw-clamp schema change. The current M5-037 mount schema
	 * has no yaw field, so horizontal traverse is free and this reason can not
	 * fire today; the slot keeps future refusals append-only instead of
	 * overloading PitchOutOfRange when a yaw limit arrives.
	 */
	YawOutOfRange = 9,
	/** The forwarded WeaponComponent::TryFire refused; the named EFireReject rides the detail verbatim. */
	WeaponRefused = 10,
};

/**
 * The turret aim as the operator's module-local angles in DEGREES relative to
 * the vehicle body (0 yaw = vehicle forward; positive pitch = up, matching the
 * FRotator sign convention). Plain floats on purpose - an FRotator silently
 * normalizes/clamps, while the turret contract REFUSES an out-of-clamp aim
 * with a named reason instead of bending it.
 */
struct FVehicleMountAim
{
	/** Desired turret yaw in degrees relative to the vehicle body. */
	float YawDegrees = 0.0f;

	/** Desired turret pitch in degrees (positive = up). */
	float PitchDegrees = 0.0f;
};

/** Outcome of one Configure request: explicit decision + reason, never a bare bool. */
struct FVehicleMountConfigOutcome
{
	/** True when the mount loaded the weapon/ammo/angles and is ready to arm. */
	bool bConfigured = false;

	/** None when bConfigured; otherwise the named refusal cause. */
	EVehicleMountConfigReject Reject = EVehicleMountConfigReject::None;

	/** Human-readable detail naming the offending id/field; empty on success. */
	FString RejectDetail;
};

/** The read-only verdict of one CanFire request (the TryFire gate chain without side effects). */
struct FVehicleMountFireCheck
{
	/** True only when TryFire with the same state would reach the weapon transaction. */
	bool bCanFire = false;

	/** None when bCanFire; otherwise the named refusal cause. */
	EVehicleMountFireReject Reject = EVehicleMountFireReject::None;

	/** Human-readable detail naming the offending id/field; empty when clear. */
	FString RejectDetail;
};

/** Outcome of one TryFire request: explicit decision + reason, never a bare bool. */
struct FVehicleMountFireOutcome
{
	/** True only when the forwarded weapon transaction committed one shot. */
	bool bFired = false;

	/** None when bFired; otherwise the named refusal cause. */
	EVehicleMountFireReject Reject = EVehicleMountFireReject::None;

	/** Human-readable detail naming the offending id/field; empty on success. */
	FString RejectDetail;

	/** The committed shot snapshot (the M5-022 context, source = the vehicle); only meaningful when bFired. */
	FShotContext Shot;
};

/**
 * The operator snapshot the mount resolves from the injected seat state (or
 * from the direct presence/faction providers): WHO currently counts as the
 * gunner. A value snapshot - ids and a faction tag, never an actor pointer
 * (interface contract section 0.5); the wiring (M5-044) owns the live objects
 * behind the ids.
 */
struct FVehicleMountOperatorState
{
	/** True only while an operator holds the operator seat (or the override provider says so). */
	bool bPresent = false;

	/** The seat model's occupant identity (invalid when the operator came from the override provider). */
	FGuid OccupantId;

	/** The operator faction the friendly-fire filter uses (empty = unaligned). */
	FName Faction;
};

// Compile-time pins: the aim request stays a trivially copyable value
// snapshot (interface contract section 0.5 pointer ban).
static_assert(std::is_trivially_copyable_v<FVehicleMountAim>, "FVehicleMountAim must stay a trivially copyable value snapshot");
static_assert(!std::is_pointer_v<decltype(FVehicleMountAim::YawDegrees)>, "FVehicleMountAim must stay pointer-free");

/**
 * M5-043: the vehicle turret weapon mount (M5 interface contract section 1,
 * owner 037..043 "Vehicles/*"; section 8 "载具权限"). One component per
 * vehicle; it turns an FVehicleMountDefinition row (M5-037: turret weapon
 * reference, pivot offset, pitch clamp) into a firing surface that REUSES the
 * existing weapon executor - there is no second damage, ammo or shot stack:
 *
 *   FVehicleMountDefinition -> (this component) -> UWeaponComponent::TryFire
 *                                                    -> the M5-019/021/022 chain
 *                                                    -> the M5-026 delivery (M5-044)
 *
 * Responsibilities and guarantees pinned by this card:
 * - Configuration: Configure validates the mount row (the M5-037 validator),
 *   resolves TurretWeaponId and its ammo kind in the turret weapon
 *   component's OWN mounted catalog and refuses every gap with a named,
 *   id-carrying error (a missing weapon or ammo kind is never a fake
 *   success). The pivot offset (LocationCm) and the pitch clamp are parsed
 *   and exposed; the physical socket/attach work belongs to M5-044.
 * - One executor: firing goes through the injected UWeaponComponent::TryFire
 *   transaction verbatim (the same binding, magazine, reserve and
 *   ActionSequence allocator as a handheld weapon). This component adds NO
 *   dedicated damage path - the shot's damage rides the M5-026 delivery and
 *   the unified entry exactly like a handheld shot.
 * - Shot attribution (contract section 8): the committed FShotContext source
 *   is the VEHICLE (the turret weapon component's fire registry records the
 *   vehicle actor; the vehicle is the firing body and its fire-lineage owns
 *   the sequence space). The operator is recorded SEPARATELY on this
 *   component (occupant id + faction from the injected seat state), and the
 *   friendly-fire filter faces the OPERATOR's faction: the fire-source
 *   registration carries the operator faction while an operator holds the
 *   seat and the vehicle faction when nobody does (contract section 8
 *   "友伤过滤用操作者阵营，无操作者用车辆阵营") - so the existing M5-026
 *   faction filter reads the operator faction without any new filter code.
 * - No operator, no player fire: with no operator resolved, TryFire refuses
 *   with NoOperator and zero side effects ("无操作者禁止玩家开火").
 * - Turret clamp: the M5-037 mount schema has NO yaw field, so horizontal
 *   traverse is free (full circle); pitch must stay inside
 *   [MinPitchDegrees, MaxPitchDegrees]. Out-of-clamp or non-finite aims are
 *   REFUSED with named reasons, never clamped into range (GetAimClampSummary
 *   reports the effective clamp for diagnostics and the M5-044 wiring).
 * - Cleanup: Finalize (and EndPlay) closes the turret reload window, voids an
 *   in-flight shot and drops the injected references (the WeaponComponent
 *   cancel faces: CancelReload + NotifyOwnerDied). After Finalize the mount is
 *   terminal - a destroyed vehicle never fires again through it.
 * - No input, no ticking, no World services of its own: the component is an
 *   API surface (Configure / CanFire / TryFire / RefreshVehicleFireSource /
 *   NotifyOperatorChanged / Finalize); the actual seat/input/delivery wiring
 *   belongs to M5-044 and drives these calls.
 *
 * Lifetime: World/Session level (contract section 0.5) - the mount is a
 * vehicle subobject, its wiring state dies with the vehicle and is never
 * serialized (the injected pointers/providers below are deliberately NOT
 * UPROPERTYs, so no save path can reach them).
 */
UCLASS(ClassGroup = (Vehicles), meta = (BlueprintSpawnableComponent))
class UEMMO_API UVehicleWeaponMountComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UVehicleWeaponMountComponent();

	// ------------------------------------------------------------------
	// Configuration (the M5-037 mount row -> the armed turret state)
	// ------------------------------------------------------------------

	/**
	 * Loads the turret from a real mount row and a real turret weapon
	 * component. Refusal order: null weapon component, no owner actor, an
	 * invalid mount row (the M5-037 validator problems verbatim), no healthy
	 * mounted catalog pair on the weapon component, TurretWeaponId not
	 * resolving, a melee turret weapon, the weapon's ammo kind not resolving.
	 * A refused request leaves the mount unconfigured (never half-armed); a
	 * successful request replaces the previous configuration in place (the
	 * fire-source registration stays until RefreshVehicleFireSource re-evaluates
	 * it). The turret weapon itself is BOUND through the same item->weapon
	 * chain as any handheld weapon (ApplyEquippedWeapon on the injected
	 * component - the M5-044 wiring owns the call); this gate only verifies
	 * the catalog the component actually mounted resolves the row's ids.
	 */
	FVehicleMountConfigOutcome Configure(const FVehicleMountDefinition& MountDefinition, UWeaponComponent* InTurretWeapon);

	/** True after a successful Configure (and before Finalize). */
	bool IsConfigured() const { return bConfigured; }

	/** The configured mount row (only meaningful while IsConfigured()). */
	const FVehicleMountDefinition& GetMountedDefinition() const { return ConfiguredMount; }

	/** The configured turret weapon definition snapshot (null while unconfigured). */
	const FWeaponDefinition* GetTurretWeaponDefinition() const { return bConfigured ? &ConfiguredWeapon : nullptr; }

	/**
	 * The effective aim clamp as human-readable text (diagnostics and the
	 * M5-044 wiring): the M5-037 schema has no yaw field, so the summary
	 * reports a free full-circle yaw plus the pitch clamp; while
	 * unconfigured it reports that instead.
	 */
	FString GetAimClampSummary() const;

	// ------------------------------------------------------------------
	// Operator state (the frozen seat-state seam; M5-044 supplies the live inputs)
	// ------------------------------------------------------------------

	/**
	 * Injects the seat occupancy source: the mount polls the model's
	 * (vehicle, seat) pair for the current occupant. Non-owning - the wiring
	 * keeps the model alive; null clears the source. The seat model is the
	 * presence authority; factions ride the separate faction provider below.
	 */
	void SetOperatorSeatSource(const FSeatOccupancyModel* SeatModel, FName VehicleId, FName SeatId);

	/** Direct presence override (true = an operator is at the gun); takes precedence over the seat source. Unset = seat source. */
	void SetOperatorPresenceProvider(TFunction<bool()> Provider);

	/** Injects the operator faction provider (read fresh on every resolve). Unset = no operator faction. */
	void SetOperatorFactionProvider(TFunction<FName()> Provider);

	/** Fixed-faction convenience over SetOperatorFactionProvider (the common production case: one faction while seated). */
	void SetOperatorFaction(FName Faction);

	/** The no-operator filter faction fallback (contract section 8: without an operator the vehicle faction filters). */
	void SetVehicleFaction(FName Faction);

	/**
	 * The wiring calls this after any seat entry/exit or faction change: the
	 * fire-source registration is re-evaluated so the friendly-fire filter
	 * keeps reading the CURRENT operator faction (skipped while a shot is in
	 * flight - re-register after ReleaseMountShot; an operator change without
	 * a faction change is a no-op).
	 */
	void NotifyOperatorChanged();

	/**
	 * Resolves the current operator snapshot: presence comes from the
	 * override provider (when set) or the injected seat model (when set);
	 * with neither source the operator is absent (fire stays forbidden - the
	 * safe default). The faction comes from the faction provider.
	 */
	FVehicleMountOperatorState ResolveOperator() const;

	/**
	 * The faction the turret's hits face the friendly-fire filter with: the
	 * operator faction while an operator is present, else the vehicle faction
	 * (contract section 8). Empty on both counts = unaligned (never trips the
	 * existing filter).
	 */
	FName ResolveFilterFaction() const;

	// ------------------------------------------------------------------
	// Fire lineage (contract section 8: the vehicle is the firing source)
	// ------------------------------------------------------------------

	/**
	 * Registers (or re-registers) the vehicle as the turret weapon's fire
	 * source with the CURRENT filter faction (operator when present, else
	 * vehicle). The registration enters the turret weapon component's fire
	 * registry with the weapon component's OWNER as the actor - the turret
	 * weapon component must be owned by the vehicle actor. Re-registration
	 * after a world rebuild (old epoch) or an operator faction change
	 * mints a fresh fire-lineage id (the sequence space restarts; the keys
	 * stay collision-free because the source id is part of every key).
	 * Returns false with OutError when the registry refuses; a no-op when the
	 * current registration already matches. Skipped (returns true unchanged)
	 * while a shot is in flight - the in-flight shot keeps its attribution.
	 */
	bool RefreshVehicleFireSource(FString* OutError = nullptr);

	/** The registered vehicle fire-source entity id (invalid before the first RefreshVehicleFireSource). */
	FEntityId GetVehicleFireSourceEntityId() const { return VehicleFireSourceEntityId; }

	// ------------------------------------------------------------------
	// The fire face (gates here, transaction in the injected WeaponComponent)
	// ------------------------------------------------------------------

	/**
	 * The full gate chain without side effects: finalized, configured, the
	 * turret binding names TurretWeaponId (ranged), the vehicle fire source
	 * is a live record of the current fire-registry epoch, an operator is
	 * present, the aim is finite and inside the pitch clamp. The forwarded
	 * WeaponComponent gates (catalog, cooldown, magazine, in-flight shot,
	 * intent identity) stay inside its own TryFire - CanFire true means the
	 * transaction was REACHED, not that it must commit.
	 */
	FVehicleMountFireCheck CanFire(const FVehicleMountAim& Aim) const;

	/**
	 * One trigger transaction: the CanFire gate chain, then the forwarded
	 * WeaponComponent::TryFire with the mount's own monotonic local shot
	 * sequence (association only - the public ShotId stays the fire
	 * registry's allocation). The committed FShotContext.SourceEntityId is
	 * the VEHICLE fire source (shot attribution, see the class comment); the
	 * operator rides ResolveOperator/ResolveFilterFaction. Every refusal is
	 * named and side-effect free; a WeaponRefused surfaces the underlying
	 * EFireReject reason and detail verbatim (never swallowed, never
	 * re-mapped).
	 */
	FVehicleMountFireOutcome TryFire(const FVehicleMountAim& Aim);

	/**
	 * Ends the committed shot's in-flight window (forwards the weapon
	 * component's ReleaseFire). The M5-044 wiring owns the trigger semantics
	 * (held trigger = release after every commit so the pacing cooldown
	 * alone gates the next round). Idempotent no-op without a shot.
	 */
	void ReleaseMountShot();

	/** The mount's committed shot count (only successful commits count). */
	int32 GetMountedShotCount() const { return MountedShotCount; }

	/** The aim the last committed shot fired at; false before the first commit. */
	bool GetLastCommittedAim(FVehicleMountAim& OutAim) const;

	/** The injected turret weapon component (null while unconfigured/finalized); the M5-044 delivery reads its fire registry and reload wrappers. */
	UWeaponComponent* GetTurretWeapon() const { return TurretWeapon; }

	// ------------------------------------------------------------------
	// Cleanup (contract section 8: death, destroy, LeaveRoom, Retry, EndPlay)
	// ------------------------------------------------------------------

	/**
	 * The vehicle-destroy cleanup face: cancels the turret reload window
	 * (nothing transfers) and the in-flight shot, runs the weapon component's
	 * death teardown (NotifyOwnerDied - generation bump, cooldown reset) and
	 * drops every injected reference (seat model pointer, providers, weapon
	 * component pointer). Idempotent; returns true exactly when this call
	 * performed it. After Finalize the mount is terminal: TryFire/CanFire
	 * refuse with MountFinalized and only the configuration snapshot stays
	 * readable for diagnostics.
	 */
	bool Finalize();

	/** True once Finalize ran (the terminal state). */
	bool IsFinalized() const { return bFinalized; }

	// AActor lifecycle: only the destroy cleanup face - EndPlay funnels into
	// Finalize so actor destruction (room teardown, world EndPlay) cancels
	// fire/reload/references exactly like an explicit vehicle-destroy call.
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** The shared CanFire/TryFire gate chain (state gates only; no side effects). */
	bool EvaluateFireGates(const FVehicleMountAim& Aim, EVehicleMountFireReject& OutReject, FString& OutDetail) const;

	/**
	 * M5-048-lesson handler: the turret weapon's mode-appropriate fire policy
	 * (projectile weapons refuse the component's default single-fire policy,
	 * which grants hitscan shots only). Subscribed once in Configure; re-reads
	 * the bound definition on every successful ApplyEquippedWeapon.
	 */
	void HandleTurretBindingApplied();

	/**
	 * The configured mount row (value copy - the terminal state and the
	 * diagnostics keep reading it after the catalogs/wiring are gone).
	 */
	FVehicleMountDefinition ConfiguredMount;

	/** The configured turret weapon definition (value copy, same lifetime note). */
	FWeaponDefinition ConfiguredWeapon;

	/** True after a successful Configure; a refused Configure never sets it. */
	bool bConfigured = false;

	/** True after Finalize; terminal (a destroyed vehicle never fires again). */
	bool bFinalized = false;

	/**
	 * The injected turret weapon component (non-owning; the vehicle owns it).
	 * Deliberately NOT a UPROPERTY: World-transient wiring, never serialized.
	 * Cleared by Finalize (the weak-reference drop).
	 */
	UWeaponComponent* TurretWeapon = nullptr;

	/** The registered vehicle fire-source entity id (invalid until RefreshVehicleFireSource). */
	FEntityId VehicleFireSourceEntityId = InvalidCombatEntityId;

	/** The mount's own monotonic local shot sequence (never reset: strictly increasing across binding generations is always admissible). */
	uint64 NextLocalShotSequence = 1;

	/** Number of committed shots this mount made. */
	int32 MountedShotCount = 0;

	/** The aim the last committed shot fired at (valid when bHasCommittedAim). */
	FVehicleMountAim LastCommittedAim;
	bool bHasCommittedAim = false;

	// -- The injected operator state (all non-owning; cleared by Finalize) ----

	/** The seat occupancy model the operator presence is polled from (null = unset). */
	const FSeatOccupancyModel* OperatorSeatModel = nullptr;

	/** The (vehicle, seat) pair polled in OperatorSeatModel. */
	FName OperatorVehicleId;
	FName OperatorSeatId;

	/** Direct presence override (takes precedence over the seat source; empty = unset). */
	TFunction<bool()> OperatorPresenceProvider;

	/** The operator faction provider (empty = no operator faction). */
	TFunction<FName()> OperatorFactionProvider;

	/** The no-operator filter faction fallback (contract section 8). */
	FName VehicleFaction;

	// -- M5-048-lesson: the turret weapon fire policy (projectile weapons
	// refuse the component's default single-fire policy, which grants hitscan
	// shots only) --

	/** The mode-appropriate delivery policy set while a projectile weapon is bound. */
	TUniquePtr<IFirePolicy> TurretDeliveryPolicy;

	/** The binding-applied subscription handle (removed in Finalize). */
	FDelegateHandle TurretBindingAppliedHandle;
};
