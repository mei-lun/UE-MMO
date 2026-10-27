#pragma once

#include "CoreMinimal.h"
#include "Delegates/DelegateCombinations.h"
#include "GameFramework/Pawn.h"

#include "../Combat/System/CombatEventTypes.h"
#include "../Combat/System/ReactionTypes.h"
#include "../Combat/System/HitLedger.h"
#include "../Combat/Data/CombatCatalog.h"
#include "../Items/ItemDefinition.h"
#include "../Projectiles/LinearProjectilePolicy.h"
#include "../Projectiles/ProjectileWorldService.h"
#include "SeatModel.h"
#include "VehicleTypes.h"

#include <type_traits>

#include "CombatVehicle.generated.h"

class ACombatVehicle;
class UBoxComponent;
class UCombatComponent;
class UHealthComponent;
class FCombatEntityRegistry;

/**
 * The destroy event payload: a value snapshot of the terminal fact (contract
 * section 0.5) - ids only plus the weak actor reference consumers already
 * resolve through the registry; no raw pointer, never a profile value.
 */
struct FVehicleDestroyedEvent
{
	/** The vehicles-table id of the destroyed hull. */
	FName VehicleId;

	/** The world entity id the hull held (InvalidCombatEntityId when it never registered). */
	FEntityId EntityId = InvalidCombatEntityId;

	/** The generation the hull was registered in (InvalidCombatEpoch when never registered). */
	FCombatEpoch Epoch = InvalidCombatEpoch;

	/** The destroyed hull (weak; stale-safe for late consumers). */
	TWeakObjectPtr<const ACombatVehicle> Vehicle;
};

/** The one-shot destroy broadcast of the death sequence. */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnVehicleDestroyed, const FVehicleDestroyedEvent&);

// Compile-time pins: the destroy event is a pointer-free trivially copyable
// value snapshot (interface contract section 0.5) - ids and one weak
// reference, never a raw actor address.
static_assert(std::is_trivially_copyable_v<FVehicleDestroyedEvent>, "FVehicleDestroyedEvent must stay a trivially copyable value snapshot");
static_assert(!std::is_pointer_v<decltype(FVehicleDestroyedEvent::VehicleId)>, "FVehicleDestroyedEvent::VehicleId must stay a value type");
static_assert(!std::is_pointer_v<decltype(FVehicleDestroyedEvent::EntityId)>, "FVehicleDestroyedEvent::EntityId must stay a value type");
static_assert(!std::is_pointer_v<decltype(FVehicleDestroyedEvent::Epoch)>, "FVehicleDestroyedEvent::Epoch must stay a value type");
static_assert(!std::is_pointer_v<decltype(FVehicleDestroyedEvent::Vehicle)>, "FVehicleDestroyedEvent::Vehicle must stay a weak reference, never a raw pointer");

/**
 * M5-039: the single-seat ground vehicle hull as a hittable world entity
 * (M5 interface contract section 8 "载具权限", first owner 039 for the shared
 * VehicleActor). The hull REUSES the shared component pool instead of building
 * a second combat stack:
 * - UHealthComponent stays the one owner of life (contract section 5) - the
 *   vehicle never applies damage to itself or to anything else, and there is
 *   no direct-to-health function on this class. Every hit arrives through the
 *   M5-012 unified entry (ApplyUnifiedHit), exactly like the hitscan executor
 *   (M5-026) and the melee pipeline submit them; the entry reads the hull's
 *   HealthComponent, its defense (the shared UCombatComponent carrier, the
 *   same lookup the default calculator performs on every combatant) and its
 *   overridden FTargetReaction policy.
 * - The hull's target reaction is blanket control-immune (bImmuneControl,
 *   every bAllow* gate closed) while taking full damage: a hull never
 *   staggers, launches or gets knocked down, and control immunity still lets
 *   damage through (contract section 5). Death resistance and damage immunity
 *   stay off - hulls die.
 *
 * Base class APawn: the vehicle is the possessable body the M5-041 input
 * handover drives (the same ladder as every other combatant body -
 * APrototypeCharacter/ATrainingEnemy are pawns) and melee/hitscan queries
 * address combatants through the pawn body channel. The hull mesh itself is
 * NOT loaded here: the M5-038 catalog keeps MeshPath as a soft path and the
 * mesh asset does not exist yet (M5-038 limitation), so the query body is a
 * fixed-size box root; a hull-size schema change arrives with the mesh card.
 *
 * Faction/TargetPolicy: the hull registers into the world's FCombatEntityRegistry
 * (M5-010 semantics) and becomes selectable/hittable by bullets and melee
 * through the two existing faces - the hitscan executor's injected
 * IHitscanTargetIdentity resolves the hull's registered EntityId, and the
 * melee QueryTargets overlap picks any live-HealthComponent body. The faction
 * is a wiring choice (FVehicleDefinition carries none, matching the
 * UCombatComponent precedent where no production faction source exists): the
 * default is empty = unaligned, which never trips the friendly-fire filter;
 * the M5-041 operator wiring sets the concrete faction.
 *
 * Death sequence (exactly once, contract section 8 "清理必须覆盖死亡、销毁、
 * LeaveRoom、Retry和World EndPlay"): HealthComponent OnDied ->
 *   (a) the seat/mount vacate seam runs first (M5-040 FSeatOccupancyModel::
 *       ClearVehicle through the injected callback; the M5-043 turret mount
 *       cancel rides the same one-shot sequence/destroy event),
 *   (b) the hull unregisters from the entity registry (its own generation's
 *       epoch - a rebuilt world's wiped record refuses as StaleEpoch, which is
 *       the safe no-op),
 *   (c) the one-shot destroy event broadcasts (FOnVehicleDestroyed).
 * The sequence is idempotent first-terminal-wins (the HitLedger/scene
 * evaluator precedent): repeated deaths, a second destroy or actor EndPlay
 * after death are rejected with no side effects (no second event, no second
 * unregister, no second vacate).
 *
 * Lifetime and persistence: World/Session level (contract section 0.5) - the
 * vehicle is an APawn, its instance state dies with the World and is NEVER
 * written into the player profile/save (Profile = GameInstance level, the
 * M5-040 SeatModel precedent; vehicle instances are not persistent business
 * values, only FVehicleDefinition kinds are shared configuration). The
 * definition and the death bookkeeping below are deliberately NOT UPROPERTYs
 * so no save/serialization path can pick them up.
 */
UCLASS(ClassGroup = (Vehicles))
class UEMMO_API ACombatVehicle : public APawn
{
	GENERATED_BODY()

public:
	ACombatVehicle();

	/**
	 * Initializes the hull from a real vehicle definition (M5-037 value types):
	 * validates it first (an invalid definition is refused, no partial state),
	 * then loads MaxHealth into the one health owner (fresh death lifecycle)
	 * and Defense plus the control-immune hull policy into the shared combat
	 * component. A finalized (dead/destroyed) hull refuses a new definition -
	 * the terminal state wins. OutErrors (when provided) carries the validator
	 * problems on refusal.
	 */
	bool SetVehicleDefinition(const FVehicleDefinition& InDefinition, FString* OutErrors = nullptr);

	/** The loaded definition (only meaningful when HasVehicleDefinition()). */
	const FVehicleDefinition& GetVehicleDefinition() const { return Definition; }
	bool HasVehicleDefinition() const { return bHasDefinition; }

	/**
	 * World identity: registers the hull into the entity registry (M5-010) with
	 * category "vehicle" and the given faction (empty = unaligned). Idempotent
	 * with the ResolveUnifiedEntityId guard: a cached id is reused only while
	 * its record still resolves to this live actor, so a repeated call never
	 * mints a second record. Returns InvalidCombatEntityId when the registry
	 * refuses (stale epoch, duplicate actor) - the refusal reason stays on the
	 * registry face.
	 */
	FEntityId RegisterCombatEntity(FCombatEntityRegistry& Registry, FName Faction);

	/** The registered world entity id (InvalidCombatEntityId while unregistered). */
	FEntityId GetEntityId() const { return EntityId; }

	/** The generation that minted the registration (InvalidCombatEpoch while unregistered). */
	FCombatEpoch GetRegisteredEpoch() const { return RegisteredEpoch; }

	/**
	 * Builds the M5-040 SeatModel VehicleValidityGate predicate for this hull
	 * (the wiring point SeatModel.h names): the gate admits only a request for
	 * this hull's own vehicle id whose hull is alive and not pending kill -
	 * a dead or destroyed hull refuses entry with VehicleInvalid. The gate
	 * captures a weak reference, so it answers false once the hull is gone.
	 */
	FSeatOccupancyModel::FSeatEnterGate MakeVehicleValidityGate() const;

	/**
	 * The M5-041 wiring seam of the death sequence's first step: called at most
	 * once with the vehicle id when the hull dies or is destroyed (the wiring
	 * forwards to FSeatOccupancyModel::ClearVehicle; the M5-043 mount cancel
	 * joins here). Unset = no-op. The wiring must keep the bound callback
	 * callable for as long as vehicles can die (it owns both lifetimes) and
	 * clear it before its own teardown - the vehicle stores the function, it
	 * does not own the model.
	 */
	void SetSeatVacateCallback(TFunction<void(FName)> Callback) { SeatVacate = MoveTemp(Callback); }

	UHealthComponent* GetHealthComponent() const { return HullHealth; }
	UCombatComponent* GetCombatComponent() const { return HullCombat; }

	/** True while the health owner is alive (the hull's enterable state). */
	bool IsHullAlive() const;

	/** True once the death/destroy sequence ran (the terminal state). */
	bool IsDestroyFinalized() const { return bDestroyFinalized; }

	/**
	 * Runs the death sequence once (seats -> unregister -> destroy event).
	 * Returns true exactly when this call performed it; a repeated call is the
	 * rejected second terminal (false, no side effects). HealthComponent::OnDied
	 * and EndPlay both funnel here, so death, destroy and the EndPlay cleanup
	 * faces share the one idempotent sequence.
	 */
	bool FinalizeDestruction();

	/**
	 * M5-044: the production rigging entry (the vehicle-segment integration).
	 * Ensures the ground drive (M5-042) and the turret weapon + mount
	 * (M5-043) exist from the PRODUCTION sources - the same Data/CombatSystem
	 * tables and items.json loaders the pawn's weapon mount uses - and wires
	 * the operator seam: the injected FSeatOccupancyModel + driver seat id
	 * (the M5-040 model the interaction wiring owns), the operator faction
	 * (contract section 8: the friendly-fire filter reads the operator while
	 * seated) and the vehicle fire-source registration. Idempotent: an
	 * already-rigged hull refreshes the operator wiring in place. Refuses
	 * (false + OutError) on a terminal hull or an unavailable/invalid
	 * production source; a hull whose vehicle id has NO mount row stays
	 * drivable but unarmed (true - a turret is optional rigging; the note is
	 * logged, never faked).
	 */
	bool EnsureProductionRigging(FSeatOccupancyModel* Seats, FName DriverSeatId, FName OperatorFaction, FString* OutError = nullptr);

	/** The one-shot destroy event of the death sequence (contract section 8). */
	FOnVehicleDestroyed OnVehicleDestroyed;

	// AActor lifecycle: only the destroy cleanup face. M5-047-lesson: the
	// death-sequence binding deliberately does NOT live in
	// PostInitializeComponents - the engine dispatches it only for worlds
	// past actor initialization (Actor.cpp's AreActorsInitialized gate) and
	// automation worlds created via UWorld::CreateWorld never pass that gate;
	// it lives in SetVehicleDefinition instead.
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** The health owner's at-most-once death signal; funnels into FinalizeDestruction. */
	void HandleHealthDied();

	/**
	 * The hull's target reaction policy: blanket control immunity with every
	 * per-kind gate closed, full damage taken, no death resistance. A static
	 * member (not a global helper) - the M5-037 namespace lesson keeps helpers
	 * out of the global namespace.
	 */
	static FTargetReaction MakeHullTargetReaction(const FVehicleDefinition& InDefinition);

	/**
	 * The query body combatants trace and overlap against: object type ECC_Pawn
	 * (the combatant body channel the melee overlap and the hitscan object
	 * query use) blocking every channel (the projectile sweeps resolve against
	 * it like a solid body). The extent is a constant - hull dimensions are not
	 * in the M5-037 schema; the real mesh arrives with a later card.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Vehicles")
	TObjectPtr<UBoxComponent> HullBody;

	/** The one owner of life (interface contract section 5) - no second pool exists. */
	UPROPERTY(VisibleAnywhere, Category = "Vehicles")
	TObjectPtr<UHealthComponent> HullHealth;

	/**
	 * The shared defense/reaction-policy carrier the unified entry reads (the
	 * default calculator's GetDefense lookup and the melee/hitscan policy
	 * lookup). Attached inert: the vehicle never starts attacks, so the action
	 * state machine stays Free except for the death marking.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Vehicles")
	TObjectPtr<UCombatComponent> HullCombat;

	/**
	 * The loaded vehicle kind (World-transient; deliberately NOT a UPROPERTY -
	 * nothing here may reach a save/profile path).
	 */
	FVehicleDefinition Definition;
	bool bHasDefinition = false;

	/**
	 * M5-044: the production rigging's own catalogs (owned VALUES). The
	 * turret weapon component holds NON-OWNING pointers into them (the same
	 * shape as the pawn's WeaponCatalogValue session pair), so they must live
	 * as long as the rigging exists - stack locals would dangle the first
	 * fire. World/session state, never a save value.
	 */
	FCombatCatalog RiggingCatalog;
	FItemDefinitionCatalog RiggingItems;

	/**
	 * The world entity identity: the registry is NOT owned (the world wiring
	 * owns registry and vehicles together, like the ledger's bound-registry
	 * precedent); ids/epoch are value snapshots of the registration.
	 */
	FCombatEntityRegistry* BoundRegistry = nullptr;
	FEntityId EntityId = InvalidCombatEntityId;
	FCombatEpoch RegisteredEpoch = InvalidCombatEpoch;

	/** The terminal-state flag: the death sequence ran exactly once. */
	bool bDestroyFinalized = false;

	/** The injected seat/mount vacate seam (see SetSeatVacateCallback). */
	TFunction<void(FName VehicleId)> SeatVacate;
};

/** One in-flight turret pellet of the vehicle delivery (value entry; the pawn's FLiveWeaponProjectile shape, vehicle-owned). */
struct FVehicleLiveTurretPellet
{
	TWeakObjectPtr<ACombatProjectile> Actor;
	TUniquePtr<IProjectileMotionPolicy> Policy;
	FCombatEpoch ShotEpoch = InvalidCombatEpoch;
	FEntityId ShotSource = InvalidCombatEntityId;
	FShotId ShotId = InvalidCombatShotId;
};

/**
 * M5-044: the turret delivery component - the vehicle-side tail of the shared
 * fire pipeline. The mount commits the shot (gates, ammo, attribution), THIS
 * component delivers it: the shot plan from the mount pivot (the physical
 * turret socket this card owns) along the hull facing, the projectile
 * reservation + spawn through the world service, one motion policy per pellet
 * (the production dispatch - straight/ballistic/homing/pierce exactly like the
 * pawn), and the per-frame advance/hit/retire loop (the pawn advances its own
 * pellets from its tick; the vehicle actor cannot tick, so the delivery
 * carries its own). Every hit lands through the M5-012 unified entry via the
 * policy's hit context - no second damage stack. The M5-077 ledger face is
 * the vehicle's own FHitLedger bound to the turret weapon's fire registry;
 * a shot ends exactly once when its last pellet retires. A hitscan-mode
 * turret weapon is refused with a named log line (the shipped tables carry
 * no such row; the hitscan delivery face would mirror the pawn's when one
 * arrives).
 */
UCLASS(ClassGroup = (Vehicles), meta = (BlueprintSpawnableComponent))
class UEMMO_API UVehicleTurretDeliveryComponent : public UActorComponent, public IHitscanTargetIdentity
{
	GENERATED_BODY()

public:
	UVehicleTurretDeliveryComponent();

	// The per-frame advance/retire loop (the vehicle actor itself cannot tick).
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// The teardown face: the live list clears; held shots' ledger ends run here.
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/**
	 * Binds the turret weapon: subscribes its commit broadcast and binds the
	 * delivery's ledger to the weapon's fire registry. Idempotent (a re-bind
	 * detaches the previous subscription first).
	 */
	void BindTurretWeapon(UWeaponComponent& Weapon);

	// The IHitscanTargetIdentity seam: the hit actor's registered entity id
	// (walls and health-less bodies stay unregistered environment).
	virtual FEntityId ResolveTargetEntityId(const AActor& HitActor) const override;

private:
	/** The commit tail: plan -> reserve -> spawn -> bind policies. */
	void HandleShotCommitted(const FShotContext& Shot);

	/** The shared retirement helper (a destroyed body still counts against its shot). */
	void RetirePellet(int32 LiveIndex);

	/** The bound turret weapon (the commit broadcast source; the registry owner). */
	TWeakObjectPtr<UWeaponComponent> BoundTurretWeapon;

	/** The delivery's own world service + spawner + ledger (the pawn's delivery trio, vehicle-owned). */
	TUniquePtr<FProjectileWorldService> ProjectileService;
	TUniquePtr<IProjectileSpawner> ProjectileSpawner;
	FHitLedger TurretLedger;

	TArray<FVehicleLiveTurretPellet> LivePellets;

	/** The M5-077 shot-end tracking (the last pellet's retirement releases the shot). */
	struct FVehiclePendingShotEnd
	{
		FCombatEpoch Epoch = InvalidCombatEpoch;
		FEntityId Source = InvalidCombatEntityId;
		FShotId Shot = InvalidCombatShotId;
		int32 LivePellets = 0;
	};
	TArray<FVehiclePendingShotEnd> PendingShotEnds;

	/** The lazy target registration cache (the pawn's ShotTargetEntityIds shape). */
	mutable TMap<const AActor*, FEntityId> TargetEntityIds;
};
