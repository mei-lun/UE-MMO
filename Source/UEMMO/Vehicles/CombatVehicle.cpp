#include "CombatVehicle.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/CombatEntityRegistry.h"
#include "../Combat/Data/CombatCatalog.h"
#include "../Combat/Data/CombatDataTableParser.h"
#include "../Combat/System/TestRoomConfigDriver.h"
#include "../Items/ItemDefinition.h"
#include "../Projectiles/CombatProjectile.h"
#include "../Projectiles/Policies/BallisticPolicy.h"
#include "../Projectiles/Policies/HomingPolicy.h"
#include "../Projectiles/Policies/PenetrationPolicy.h"
#include "../Projectiles/ProjectileWorldService.h"
#include "../Weapons/WeaponComponent.h"
#include "../Weapons/ShotPattern.h"
#include "VehicleMovementComponent.h"
#include "VehicleWeaponMountComponent.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "GameFramework/ProjectileMovementComponent.h"

/**
 * M5-039: implementation of the hittable vehicle hull. See the header for the
 * frozen contract: the shared component pool (HealthComponent owns life,
 * CombatComponent carries defense and the control-immune reaction policy),
 * every hit through the M5-012 unified entry, and the one idempotent
 * death/destroy sequence (seats -> registry -> destroy event).
 */

DEFINE_LOG_CATEGORY_STATIC(LogUEMMOVehicle, Log, All);

namespace
{
	// The fixed hull query body (see the HullBody member comment): hull
	// dimensions are not in the M5-037 schema, so the box stays a named
	// constant until a mesh card introduces a real hull size.
	const FVector HullBodyHalfExtentCm(90.0f, 160.0f, 80.0f);

	// The registry category the M5-010 header names for vehicle bodies.
	const FName VehicleEntityCategory(TEXT("vehicle"));

	// M5-044: the vehicle-local pellet spawner (the pawn's
	// FWeaponProjectileSpawner shape, hull-owned; the body stamping is
	// identical, the spawn owner is the hull). Spawned tracks the live
	// pellets of the LAST commit so the delivery binds one motion policy per
	// body (the service owns the cleanup).
	class FVehicleTurretPelletSpawner final : public IProjectileSpawner
	{
	public:
		explicit FVehicleTurretPelletSpawner(UWorld* InWorld, AActor* InOwner)
			: World(InWorld)
			, Owner(InOwner)
		{
		}

		ACombatProjectile* SpawnPellet(const FProjectileSpawnReservation& Reservation, const FShotPellet& Pellet) override
		{
			UWorld* SpawnWorld = World.Get();
			if (SpawnWorld == nullptr)
			{
				return nullptr;
			}
			FActorSpawnParameters Params;
			Params.Owner = Owner.Get();
			ACombatProjectile* Actor = SpawnWorld->SpawnActor<ACombatProjectile>(
				ACombatProjectile::StaticClass(), Reservation.Origin, FRotator::ZeroRotator, Params);
			if (Actor == nullptr)
			{
				return nullptr;
			}
			Actor->Context.Epoch = Reservation.Epoch;
			Actor->Context.SourceEntityId = Reservation.Shot.SourceEntityId;
			Actor->Context.ShotId = Reservation.Shot.ShotId;
			Actor->Context.PelletIndex = Pellet.PelletIndex;
			Actor->Context.WeaponInstanceId = Reservation.Shot.WeaponInstanceId;
			Actor->Context.ProjectileInstanceId = FGuid::NewGuid();
			Actor->Context.ConfigRevision = Reservation.ConfigRevision;
			Actor->Context.ProjectileId = Reservation.Definition.ProjectileId;
			Actor->LaunchSpeedCmS = Reservation.Definition.SpeedCmS;
			Actor->MotionGravityScale = Reservation.Definition.Motion == EProjectileMotion::Parabolic ? 1.0f : 0.0f;
			if (Actor->Movement != nullptr)
			{
				Actor->Movement->InitialSpeed = Reservation.Definition.SpeedCmS;
				Actor->Movement->MaxSpeed = Reservation.Definition.SpeedCmS;
				Actor->Movement->ProjectileGravityScale = Actor->MotionGravityScale;
			}
			Actor->SetLifeSpan(Reservation.Definition.LifetimeS);
			Actor->FaceFlightDirection(Pellet.Direction);
			Spawned.Add(Actor);
			return Actor;
		}

		void DestroyPellet(ACombatProjectile& PelletActor) override
		{
			PelletActor.Destroy();
			// The list stays honest: the retired body drops out (the service
			// owns the cleanup faces; this keeps Spawned = live pellets).
			Spawned.RemoveAllSwap([&PelletActor](const TWeakObjectPtr<ACombatProjectile>& Weak)
			{
				return !Weak.IsValid() || Weak.Get() == &PelletActor;
			});
		}

		TArray<TWeakObjectPtr<ACombatProjectile>> Spawned;

	private:
		TWeakObjectPtr<UWorld> World;
		TWeakObjectPtr<AActor> Owner;
	};
}

ACombatVehicle::ACombatVehicle()
{
	PrimaryActorTick.bCanEverTick = false;

	HullBody = CreateDefaultSubobject<UBoxComponent>(TEXT("HullBody"));
	SetRootComponent(HullBody);
	HullBody->SetMobility(EComponentMobility::Movable);
	HullBody->SetBoxExtent(HullBodyHalfExtentCm);
	// QueryOnly: the hull is a passive target - melee overlaps and hitscan
	// object traces address it through the pawn body object type, projectile
	// sweeps resolve against its blocking responses; nothing simulates.
	HullBody->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	HullBody->SetCollisionObjectType(ECC_Pawn);
	HullBody->SetCollisionResponseToAllChannels(ECR_Block);

	HullHealth = CreateDefaultSubobject<UHealthComponent>(TEXT("HullHealth"));
	HullCombat = CreateDefaultSubobject<UCombatComponent>(TEXT("HullCombat"));
}

// M5-047-lesson: ACombatVehicle intentionally does NOT override
// PostInitializeComponents - the engine dispatches it only for worlds past
// actor initialization (Actor.cpp's AreActorsInitialized gate), and
// automation worlds created via UWorld::CreateWorld never pass that gate.
// The death-sequence binding lives in SetVehicleDefinition instead, which
// runs unconditionally wherever the hull is loaded.

bool ACombatVehicle::SetVehicleDefinition(const FVehicleDefinition& InDefinition, FString* OutErrors)
{
	if (bDestroyFinalized)
	{
		// The terminal state wins (first-terminal-wins idempotency): a dead or
		// destroyed hull never re-opens a health lifecycle.
		if (OutErrors != nullptr)
		{
			*OutErrors = TEXT("the hull is already destroyed; a destroyed vehicle refuses a new definition");
		}
		return false;
	}

	FString Errors;
	if (!ValidateVehicleDefinition(InDefinition, Errors))
	{
		// No partial state: an invalid definition touches nothing (the
		// M5-037 validator owns the field-level reasons).
		if (OutErrors != nullptr)
		{
			*OutErrors = Errors;
		}
		UE_LOG(LogUEMMOVehicle, Warning,
			TEXT("UEMMO ACombatVehicle: refused vehicle definition '%s' (%s)."),
			*InDefinition.VehicleId.ToString(), *Errors);
		return false;
	}

	Definition = InDefinition;
	bHasDefinition = true;

	// The one health owner (interface contract section 5): a lowered max
	// clamps the pool down, a raised max never heals, and ResetHealth opens
	// the fresh death lifecycle the definition implies.
	if (HullHealth != nullptr)
	{
		HullHealth->SetMaxHealth(Definition.MaxHealth);
		HullHealth->ResetHealth();
		// M5-047-lesson: the death-sequence binding lives HERE, not in
		// PostInitializeComponents - the engine dispatches PostInitialize-
		// Components only for worlds past actor initialization (Actor.cpp:
		// the AreActorsInitialized gate), and a world created via
		// UWorld::CreateWorld without InitializeActorsForPlay/BeginPlay never
		// passes that gate, so the binding would never happen in those
		// automation worlds. Every definition load opens a fresh death
		// lifecycle, so the binding refreshes with it; the RemoveAll keeps
		// repeated loads from stacking handlers.
		HullHealth->OnDied.RemoveAll(this);
		HullHealth->OnDied.AddUObject(this, &ACombatVehicle::HandleHealthDied);
	}

	// The shared carrier gets the definition's defense (the default unified
	// calculator reads exactly this component) and the control-immune hull
	// policy (the melee/hitscan pipelines read the overridden policy the same
	// way they read every combatant's).
	if (HullCombat != nullptr)
	{
		HullCombat->SetCombatStats(0.0f, Definition.Defense);
		HullCombat->SetTargetReactionPolicy(MakeHullTargetReaction(Definition));
	}

	UE_LOG(LogUEMMOVehicle, Log,
		TEXT("UEMMO ACombatVehicle: hull '%s' loaded (HP %f, defense %f)."),
		*Definition.VehicleId.ToString(), Definition.MaxHealth, Definition.Defense);
	return true;
}

FTargetReaction ACombatVehicle::MakeHullTargetReaction(const FVehicleDefinition& InDefinition)
{
	// A hull is a machine body: it takes full damage and dies, but no control
	// kind ever applies to it (contract section 5 - control immunity still
	// lets damage through, and a pure-control hit never fakes damage). The
	// per-kind gates stay closed alongside the blanket flag so a resolver that
	// only reads the gates refuses the same way. Poise stays 0 (the blanket
	// immunity already refuses everything; a poise pool would be dead state).
	FTargetReaction Policy;
	Policy.PolicyId = InDefinition.VehicleId;
	Policy.bAllowStagger = false;
	Policy.bAllowLaunch = false;
	Policy.bAllowKnockdown = false;
	Policy.bImmuneControl = true;
	Policy.bImmuneDamage = false;
	Policy.bDeathResistant = false;
	return Policy;
}

FEntityId ACombatVehicle::RegisterCombatEntity(FCombatEntityRegistry& Registry, FName Faction)
{
	// The ResolveUnifiedEntityId guard (M5-013): a cached id is reused only
	// while its record still resolves to this live actor - a record wiped by a
	// world rebuild (or a different registry) re-mints instead of inheriting
	// stale identity.
	if (BoundRegistry == &Registry && IsValidCombatEntityId(EntityId))
	{
		const FCombatEntityRecord* Record = Registry.FindEntity(EntityId);
		if (Record != nullptr && Record->Actor.Get() == this)
		{
			return EntityId;
		}
	}

	FCombatEntityMetadata Metadata;
	// Faction is the wiring's choice (empty = unaligned never trips the
	// friendly-fire filter); the category is the registry header's own
	// vehicle example.
	Metadata.Faction = Faction;
	Metadata.Category = VehicleEntityCategory;

	const FCombatEpoch CurrentEpoch = Registry.GetCurrentEpoch();
	const FEntityId MintedId = Registry.RegisterEntity(this, Metadata, CurrentEpoch);
	if (IsValidCombatEntityId(MintedId))
	{
		BoundRegistry = &Registry;
		EntityId = MintedId;
		RegisteredEpoch = CurrentEpoch;
	}
	else
	{
		UE_LOG(LogUEMMOVehicle, Warning,
			TEXT("UEMMO ACombatVehicle: the registry refused hull '%s' (id stays unset)."),
			*Definition.VehicleId.ToString());
	}
	return MintedId;
}

FSeatOccupancyModel::FSeatEnterGate ACombatVehicle::MakeVehicleValidityGate() const
{
	// Weak capture: the gate outlives nothing - once the hull is gone the
	// predicate answers false instead of dereferencing a dangling actor.
	TWeakObjectPtr<const ACombatVehicle> WeakHull(this);
	return [WeakHull](const FVehicleSeatPolicyContext& Context)
	{
		const ACombatVehicle* Hull = WeakHull.Get();
		// This gate vouches only for its own hull (the wiring binds one gate
		// per vehicle); a request naming another vehicle id is not this
		// hull's business and refuses with the gate's own reason.
		if (Hull == nullptr || Context.Request.VehicleId != Hull->Definition.VehicleId)
		{
			return false;
		}
		return Hull->IsHullAlive() && !Hull->IsPendingKillPending();
	};
}

bool ACombatVehicle::IsHullAlive() const
{
	return HullHealth != nullptr && HullHealth->IsAlive();
}

bool ACombatVehicle::FinalizeDestruction()
{
	// First terminal wins (the HitLedger/scene evaluator idempotency
	// precedent): every later death, destroy or EndPlay lands here and is
	// rejected without touching any state.
	if (bDestroyFinalized)
	{
		return false;
	}
	bDestroyFinalized = true;

	// (a) Seats/mounts first (contract section 8 order): the occupants leave
	// before the entity identity disappears, so the wiring's ClearVehicle sees
	// the still-registered vehicle id. Unset seam = safe no-op (the pure
	// world-entity tests run without a seat model).
	if (SeatVacate)
	{
		SeatVacate(Definition.VehicleId);
	}

	// (b) Unregister from the entity registry, naming the generation that owns
	// the record: a rebuilt world already wiped it and refuses StaleEpoch -
	// the documented safe no-op, never an error path.
	if (BoundRegistry != nullptr && IsValidCombatEntityId(EntityId))
	{
		ECombatRegistryRejectReason Reason = ECombatRegistryRejectReason::None;
		if (!BoundRegistry->UnregisterEntity(EntityId, RegisteredEpoch, &Reason))
		{
			UE_LOG(LogUEMMOVehicle, Verbose,
				TEXT("UEMMO ACombatVehicle: unregister of hull '%s' refused (reason %d) - the record is already gone with its epoch."),
				*Definition.VehicleId.ToString(), static_cast<int32>(Reason));
		}
	}

	// (c) The one-shot destroy event. Observers (wreck presentation, the
	// M5-043 mount teardown, room accounting) resolve the hull through the
	// weak reference / registry id, never through a stored raw pointer.
	FVehicleDestroyedEvent Event;
	Event.VehicleId = Definition.VehicleId;
	Event.EntityId = EntityId;
	Event.Epoch = RegisteredEpoch;
	Event.Vehicle = this;
	OnVehicleDestroyed.Broadcast(Event);

	UE_LOG(LogUEMMOVehicle, Log,
		TEXT("UEMMO ACombatVehicle: hull '%s' destroyed (entity %llu, epoch %llu)."),
		*Definition.VehicleId.ToString(), EntityId, RegisteredEpoch);
	return true;
}

void ACombatVehicle::HandleHealthDied()
{
	// The health owner's at-most-once death signal is the only death trigger;
	// idempotency and ordering live in FinalizeDestruction.
	FinalizeDestruction();
}

void ACombatVehicle::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The destroy cleanup face (contract section 8): an actor destroyed while
	// alive runs the same one sequence; after a death it is the rejected
	// second terminal. Run before Super so the registry/model outlives the
	// observer teardown the base class performs.
	FinalizeDestruction();

	if (HullHealth != nullptr)
	{
		HullHealth->OnDied.RemoveAll(this);
	}

	Super::EndPlay(EndPlayReason);
}

bool ACombatVehicle::EnsureProductionRigging(FSeatOccupancyModel* Seats, FName DriverSeatId, FName OperatorFaction, FString* OutError)
{
	const auto Refuse = [OutError](const FString& Why)
	{
		if (OutError != nullptr)
		{
			*OutError = Why;
		}
		return false;
	};

	// A terminal hull never re-opens a lifecycle (the M5-039 first-terminal-
	// wins face): no drive, no turret, no fire source.
	if (IsDestroyFinalized() || IsPendingKillPending() || !HasVehicleDefinition())
	{
		return Refuse(TEXT("the hull is destroyed or has no definition: the production rigging refuses"));
	}

	// The production sources: the same loaders the pawn's weapon mount uses
	// (the M5-020 EnsureWeaponCatalogsMounted pattern), built per call - the
	// sources are small and the rigging runs once per hull.
	FCombatDataTableSet Tables;
	TArray<FString> LoadProblems;
	if (!LoadCombatDataDirectory(FPaths::ProjectDir() / TEXT("Data") / TEXT("CombatSystem"), Tables, LoadProblems))
	{
		return Refuse(FString::Printf(TEXT("the combat source tables are unavailable: %s"), *FString::Join(LoadProblems, TEXT(" | "))));
	}
	FParsedCombatConfig Parsed;
	Parsed.SchemaVersion = 1;
	for (const TPair<FName, FDamageProfile>& Row : Tables.DamageProfiles) { Parsed.DamageProfiles.Add(Row.Value); }
	for (const TPair<FName, FAttackReaction>& Row : Tables.AttackReactions) { Parsed.AttackReactions.Add(Row.Value); }
	for (const TPair<FName, FTargetReaction>& Row : Tables.TargetReactions) { Parsed.TargetReactions.Add(Row.Value); }
	for (const TPair<FName, FWeaponDefinition>& Row : Tables.Weapons) { Parsed.Weapons.Add(Row.Value); }
	for (const TPair<FName, FAmmoType>& Row : Tables.AmmoTypes) { Parsed.AmmoTypes.Add(Row.Value); }
	for (const TPair<FName, FProjectileDefinition>& Row : Tables.Projectiles) { Parsed.Projectiles.Add(Row.Value); }
	for (const TPair<FName, FVehicleDefinition>& Row : Tables.Vehicles) { Parsed.Vehicles.Add(Row.Value); }
	for (const TPair<FName, FVehicleSeatDefinition>& Row : Tables.VehicleSeats) { Parsed.VehicleSeats.Add(Row.Value); }
	for (const TPair<FName, FVehicleMountDefinition>& Row : Tables.VehicleMounts) { Parsed.VehicleMounts.Add(Row.Value); }
	FCombatCatalog BuiltCatalog;
	FString BuildErrors;
	if (!FCombatCatalog::BuildFromParsed(Parsed, BuiltCatalog, BuildErrors))
	{
		return Refuse(FString::Printf(TEXT("the combat catalog build refused: %s"), *BuildErrors));
	}

	// The items source (the M5-020A loader): the turret weapon binds through
	// the same item->weapon chain as any handheld weapon, so the item row
	// mapping this mount's turret_weapon_id must exist. Both catalogs land in
	// the hull's OWNED member values below - the turret weapon component
	// holds non-owning pointers into them (the pawn's session pair shape),
	// so stack locals would dangle the first fire.
	FItemDefinitionCatalog BuiltItems;
	TArray<FTestRoomItemRow> ItemRows;
	FString ItemsError;
	if (!ACombatTestRoomDriver::LoadProductionItems(ItemRows, ItemsError))
	{
		return Refuse(FString::Printf(TEXT("the items source is unavailable: %s"), *ItemsError));
	}
	for (const FTestRoomItemRow& Row : ItemRows)
	{
		FItemDefinition ItemDefinition;
		ItemDefinition.DefinitionId = Row.DefinitionId;
		ItemDefinition.DisplayName = Row.DisplayName;
		ItemDefinition.Slot = Row.Slot == TEXT("Weapon") ? EItemSlot::Weapon
			: Row.Slot == TEXT("Armor") ? EItemSlot::Armor
			: Row.Slot == TEXT("Accessory") ? EItemSlot::Accessory
			: EItemSlot::Weapon;
		ItemDefinition.BaseStats.Attack = Row.Attack;
		ItemDefinition.BaseStats.Defense = Row.Defense;
		ItemDefinition.BaseStats.MaxHP = Row.MaxHP;
		ItemDefinition.Rarity = static_cast<EItemRarity>(FMath::Clamp(Row.Rarity, 1, 3));
		ItemDefinition.WeaponDefinitionId = Row.WeaponDefinitionId;
		FString AddError;
		if (!BuiltItems.AddDefinition(ItemDefinition, &AddError))
		{
			UE_LOG(LogUEMMOVehicle, Warning, TEXT("UEMMO M5-044: item definition '%s' refused (%s)."),
				*Row.DefinitionId.ToString(), *AddError);
		}
	}
	// The ownership transfer: the members live as long as the hull, so the
	// turret weapon's catalog pointers stay valid for its whole life.
	RiggingCatalog = MoveTemp(BuiltCatalog);
	RiggingItems = MoveTemp(BuiltItems);
	FCombatCatalog& Catalog = RiggingCatalog;

	// 1. The ground drive (M5-042): ensured once, configured from the hull's
	// own definition (the same id the spawner loaded through SetVehicleDefinition).
	UVehicleMovementComponent* Drive = FindComponentByClass<UVehicleMovementComponent>();
	if (Drive == nullptr)
	{
		Drive = NewObject<UVehicleMovementComponent>(this, TEXT("M5_044_VehicleDrive"));
		AddInstanceComponent(Drive);
		Drive->RegisterComponent();
	}
	FString DriveError;
	if (!Drive->Configure(&DriveError))
	{
		return Refuse(FString::Printf(TEXT("the ground drive refused its configuration: %s"), *DriveError));
	}

	// 2. The turret (M5-043): optional rigging - a vehicle id without a mount
	// row stays drivable but unarmed (logged, never faked). The table maps
	// key by MOUNT id, so the hull's row resolves by scanning for its
	// vehicle_id (exactly one turret per vehicle; the validator enforces it).
	const FVehicleMountDefinition* MountRow = nullptr;
	for (const TPair<FName, FVehicleMountDefinition>& Row : Tables.VehicleMounts)
	{
		if (Row.Value.VehicleId == Definition.VehicleId)
		{
			MountRow = &Row.Value;
			break;
		}
	}
	UVehicleWeaponMountComponent* Mount = FindComponentByClass<UVehicleWeaponMountComponent>();
	if (MountRow == nullptr)
	{
		if (Mount != nullptr)
		{
			Mount->Finalize();
			Mount->DestroyComponent();
			Mount = nullptr;
		}
		UE_LOG(LogUEMMOVehicle, Log,
			TEXT("UEMMO M5-044: vehicle '%s' has no vehicle_mounts row: the hull stays drivable but unarmed."),
			*Definition.VehicleId.ToString());
	}
	else
	{
		UWeaponComponent* TurretWeapon = FindComponentByClass<UWeaponComponent>();
		if (TurretWeapon == nullptr)
		{
			TurretWeapon = NewObject<UWeaponComponent>(this, TEXT("M5_044_TurretWeapon"));
			AddInstanceComponent(TurretWeapon);
			TurretWeapon->RegisterComponent();
		}
		FString MountCatalogError;
		if (!TurretWeapon->MountCatalogs(&RiggingCatalog, &RiggingItems, &MountCatalogError))
		{
			return Refuse(FString::Printf(TEXT("the turret weapon refused the production catalog pair: %s"), *MountCatalogError));
		}
		// The reserve: the supply decision of this card (the 043 limit named
		// 044): four full magazines, registered explicitly - the mount never
		// mints ammunition on its own.
		const FWeaponDefinition* TurretDefinition = Catalog.FindWeapon(MountRow->TurretWeaponId);
		const FAmmoType* AmmoKind = TurretDefinition != nullptr ? Catalog.FindAmmoType(TurretDefinition->AmmoId) : nullptr;
		if (TurretDefinition == nullptr || AmmoKind == nullptr)
		{
			return Refuse(FString::Printf(TEXT("the turret weapon row '%s' or its ammo kind did not resolve"),
				*MountRow->TurretWeaponId.ToString()));
		}
		const int32 ReserveRounds = FMath::Max(1, TurretDefinition->MagazineSize) * 4;
		FString ReserveError;
		if (!TurretWeapon->GetAmmoModel().RegisterAmmoType(AmmoKind->AmmoId, ReserveRounds, ReserveRounds, &ReserveError)
			&& !ReserveError.IsEmpty())
		{
			// An already-registered type re-registers as a refresh; a real
			// refusal surfaces here.
			UE_LOG(LogUEMMOVehicle, Warning, TEXT("UEMMO M5-044: the turret reserve registration noted: %s"), *ReserveError);
		}
		// The fire clock: the world's own time (the turret has no pawn-side
		// input clock; the pacing cooldown and reload window run on it).
		TWeakObjectPtr<ACombatVehicle> WeakHull(this);
		TurretWeapon->SetFireClockProvider([WeakHull]()
		{
			const ACombatVehicle* Hull = WeakHull.Get();
			return Hull != nullptr && Hull->GetWorld() != nullptr ? Hull->GetWorld()->GetTimeSeconds() : 0.0;
		});

		// The bind: the item row whose WeaponDefinitionId names the turret
		// weapon (the production item->weapon chain; no stand-in instance).
		FName TurretItemId;
		for (const FTestRoomItemRow& Row : ItemRows)
		{
			if (Row.WeaponDefinitionId == MountRow->TurretWeaponId)
			{
				TurretItemId = Row.DefinitionId;
				break;
			}
		}
		if (TurretItemId == NAME_None)
		{
			return Refuse(FString::Printf(TEXT("no items.json row maps weapon '%s': the turret stays unbound"),
				*MountRow->TurretWeaponId.ToString()));
		}
		FItemInstance TurretInstance;
		TurretInstance.InstanceId = FGuid::NewGuid();
		TurretInstance.DefinitionId = TurretItemId;
		TurretInstance.RollSeed = static_cast<int64>(GetTypeHash(TurretItemId));
		TurretInstance.Level = 1;
		if (!TurretWeapon->ApplyEquippedWeapon(&TurretInstance).bWeaponBound)
		{
			return Refuse(FString::Printf(TEXT("the turret weapon did not bind item '%s'"), *TurretItemId.ToString()));
		}

		if (Mount == nullptr)
		{
			Mount = NewObject<UVehicleWeaponMountComponent>(this, TEXT("M5_044_VehicleMount"));
			AddInstanceComponent(Mount);
			Mount->RegisterComponent();
		}
		const FVehicleMountConfigOutcome Configured = Mount->Configure(*MountRow, TurretWeapon);
		if (!Configured.bConfigured)
		{
			return Refuse(FString::Printf(TEXT("the mount row '%s' refused to configure (%d): %s"),
				*MountRow->MountId.ToString(), static_cast<int32>(Configured.Reject), *Configured.RejectDetail));
		}

		// The operator seam (contract section 8): the seat model the
		// interaction wiring owns + the driver seat + the operator faction.
		Mount->SetOperatorSeatSource(Seats, Definition.VehicleId, DriverSeatId);
		Mount->SetOperatorFaction(OperatorFaction);
		Mount->NotifyOperatorChanged();
		FString FireSourceError;
		if (!Mount->RefreshVehicleFireSource(&FireSourceError))
		{
			return Refuse(FString::Printf(TEXT("the vehicle fire source registration refused: %s"), *FireSourceError));
		}

		// The delivery tail (this card): the commit broadcast spawns and flies
		// the pellets from the turret pivot through the shared pipeline.
		UVehicleTurretDeliveryComponent* Delivery = FindComponentByClass<UVehicleTurretDeliveryComponent>();
		if (Delivery == nullptr)
		{
			Delivery = NewObject<UVehicleTurretDeliveryComponent>(this, TEXT("M5_044_TurretDelivery"));
			AddInstanceComponent(Delivery);
			Delivery->RegisterComponent();
		}
		Delivery->BindTurretWeapon(*TurretWeapon);
	}

	UE_LOG(LogUEMMOVehicle, Log, TEXT("UEMMO M5-044: vehicle '%s' production rigging ensured (drive + %s)."),
		*Definition.VehicleId.ToString(), MountRow != nullptr ? TEXT("turret") : TEXT("no mount row"));
	return true;
}

// ===========================================================================
// M5-044: UVehicleTurretDeliveryComponent - the vehicle-side delivery tail.
// Rules live in the header; the implementation mirrors the pawn's delivery
// face (PrototypeCharacter HandleWeaponShotCommitted + the advance/retire
// loop) with vehicle geometry: the origin rides the mount pivot (the turret
// socket this card owns), the facing is the hull yaw, the ledger is the
// vehicle's own (bound to the turret weapon's fire registry).
// ===========================================================================

namespace
{
	// The lazily registered hostile-target metadata (the M5-033 shooter's
	// constants; the vehicle file carries its own copies so the pawn's
	// anonymous namespace stays private).
	const FName VehicleTargetFaction(TEXT("enemy"));
	const FName VehicleTargetCategory(TEXT("target"));

	// The vehicle-local motion-policy dispatch (the production dispatch shape
	// of the M5-035 card; exactly one policy per pellet - never two advancers
	// on one body).
	TUniquePtr<IProjectileMotionPolicy> MakeVehicleTurretMotionPolicy(
		const FProjectileDefinition& Definition, FEntityId HomingTargetId)
	{
		if (Definition.Motion == EProjectileMotion::Parabolic)
		{
			return MakeUnique<FBallisticProjectilePolicy>();
		}
		if (Definition.Motion == EProjectileMotion::Homing)
		{
			if (IsValidCombatTargetId(HomingTargetId))
			{
				auto Homing = MakeUnique<FHomingProjectilePolicy>();
				Homing->Configure(Definition.HomingTurnRateDegS, EHomingLostTargetAction::KeepStraight);
				if (Homing->BindTarget(HomingTargetId))
				{
					return MoveTemp(Homing);
				}
			}
			// The honest downgrade: without a tracked target the shot flies
			// the straight reference motion (the pawn's shape verbatim).
			return MakeUnique<FLinearProjectilePolicy>();
		}
		if (Definition.PierceCount > 0)
		{
			auto Pierce = MakeUnique<FPenetrationProjectilePolicy>();
			FPenetrationPolicyConfig Config;
			Config.ExplosionRadiusCm = Definition.ExplosionRadiusCm;
			Pierce->Configure(Config);
			return MoveTemp(Pierce);
		}
		return MakeUnique<FLinearProjectilePolicy>();
	}
}

UVehicleTurretDeliveryComponent::UVehicleTurretDeliveryComponent()
{
	// The advance/retire loop is the vehicle's only per-frame duty (the hull
	// actor cannot tick; the delivery carries its own).
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
}

void UVehicleTurretDeliveryComponent::BindTurretWeapon(UWeaponComponent& Weapon)
{
	if (BoundTurretWeapon.Get() == &Weapon)
	{
		return;
	}
	if (UWeaponComponent* Previous = BoundTurretWeapon.Get())
	{
		Previous->OnShotCommitted.RemoveAll(this);
	}
	BoundTurretWeapon = &Weapon;
	TurretLedger.BindRegistry(&Weapon.GetFireRegistry());
	Weapon.OnShotCommitted.AddUObject(this, &UVehicleTurretDeliveryComponent::HandleShotCommitted);
	UE_LOG(LogUEMMOVehicle, Log, TEXT("UEMMO M5-044: the turret delivery bound (fire registry epoch %d)."),
		static_cast<int32>(Weapon.GetFireEpoch()));
}

FEntityId UVehicleTurretDeliveryComponent::ResolveTargetEntityId(const AActor& HitActor) const
{
	if (const FEntityId* Known = TargetEntityIds.Find(&HitActor))
	{
		return *Known;
	}
	UWeaponComponent* Weapon = BoundTurretWeapon.Get();
	// Walls and bodies without a health pool stay unregistered environment
	// (the unified entry ignores them; the trace filter blocks the shot).
	if (Weapon == nullptr || HitActor.FindComponentByClass<UHealthComponent>() == nullptr)
	{
		return InvalidCombatTargetId;
	}
	FCombatEntityMetadata Metadata;
	Metadata.Faction = VehicleTargetFaction;
	Metadata.Category = VehicleTargetCategory;
	const FEntityId TargetId = Weapon->GetFireRegistry().RegisterEntity(
		const_cast<AActor*>(&HitActor), Metadata, Weapon->GetFireEpoch());
	if (TargetId != InvalidCombatTargetId)
	{
		TargetEntityIds.Add(&HitActor, TargetId);
	}
	return TargetId;
}

void UVehicleTurretDeliveryComponent::HandleShotCommitted(const FShotContext& Shot)
{
	ACombatVehicle* Hull = Cast<ACombatVehicle>(GetOwner());
	UWeaponComponent* Weapon = BoundTurretWeapon.Get();
	UWorld* World = GetWorld();
	if (Hull == nullptr || Weapon == nullptr || World == nullptr)
	{
		return;
	}
	const FCombatCatalog* Catalog = Weapon->GetMountedWeaponCatalog();
	if (Catalog == nullptr)
	{
		UE_LOG(LogUEMMOVehicle, Warning, TEXT("UEMMO M5-044: the turret shot delivered nothing (no mounted catalog)."));
		return;
	}
	if (Shot.FireMode != EWeaponFireMode::Projectile)
	{
		// The shipped tables carry no hitscan turret row; the face refuses
		// with a named log line instead of faking a delivery.
		UE_LOG(LogUEMMOVehicle, Warning,
			TEXT("UEMMO M5-044: the hitscan turret delivery is not implemented yet (weapon '%s'); the shot delivered nothing."),
			*Shot.WeaponDefinitionId.ToString());
		return;
	}
	const FProjectileDefinition* Definition = Catalog->FindProjectile(Shot.ProjectileId);
	// M5-035: an explosive pellet damages through its explosion profile (the
	// blast is the only damage event it ever delivers); every other motion
	// hits with the projectile's own profile.
	const FName HitProfileId = (Definition != nullptr)
		? (Definition->ExplosionRadiusCm > 0.0f ? Definition->ExplosionDamageProfileId : Definition->DamageProfileId)
		: FName(NAME_None);
	const FDamageProfile* Profile = (Definition != nullptr) ? Catalog->FindDamageProfile(HitProfileId) : nullptr;
	if (Definition == nullptr || Profile == nullptr)
	{
		UE_LOG(LogUEMMOVehicle, Warning,
			TEXT("UEMMO M5-044: the projectile '%s' or its damage profile did not resolve; the shot delivered nothing."),
			*Shot.ProjectileId.ToString());
		return;
	}

	// The shot plan: the origin is the turret pivot (the mount row's socket
	// this card owns) and the facing is the hull yaw (the aim yaw 0 rides the
	// body; the M5-037 schema has no yaw field).
	UVehicleWeaponMountComponent* Mount = Hull->FindComponentByClass<UVehicleWeaponMountComponent>();
	if (Mount == nullptr)
	{
		UE_LOG(LogUEMMOVehicle, Warning, TEXT("UEMMO M5-044: the hull carries no mount; the shot delivered nothing."));
		return;
	}
	const FVector MuzzlePivot = Hull->GetActorTransform().TransformPosition(Mount->GetMountedDefinition().LocationCm);
	FShotPatternRequest Request;
	Request.PelletCount = Shot.PelletCount;
	Request.SpreadDegrees = Shot.SpreadDegrees;
	Request.Seed = static_cast<uint32>(Shot.ShotId) ^ static_cast<uint32>(Shot.ShotId >> 32)
		^ static_cast<uint32>(Shot.LocalShotSequence);
	Request.AvailablePelletSlots = Shot.PelletCount;
	Request.FeetWorldLocation = Hull->GetActorLocation();
	const float HullYaw = Hull->GetActorRotation().Yaw;
	Request.Facing = (HullYaw > -90.0f && HullYaw < 90.0f) ? 1.0f : -1.0f;
	Request.MuzzleSocketWorldLocation = &MuzzlePivot;
	const FShotPatternPlan Plan = PlanShotPattern(Request);
	if (!Plan.bPlanned)
	{
		UE_LOG(LogUEMMOVehicle, Warning, TEXT("UEMMO M5-044: the shot pattern plan refused (%d): %s; the shot delivers nothing."),
			static_cast<int32>(Plan.Reject), *Plan.RejectDetail);
		return;
	}

	if (ProjectileService == nullptr)
	{
		ProjectileService = MakeUnique<FProjectileWorldService>(World);
	}
	FProjectileSpawnReservation Reservation;
	const FProjectileReserveOutcome ReserveOutcome = ProjectileService->ReserveProjectiles(Shot, *Definition,
		Plan.Origin.WorldLocation, Plan.Pellets, Catalog->GetConfigRevision(), Reservation);
	if (!ReserveOutcome.bReserved)
	{
		UE_LOG(LogUEMMOVehicle, Warning, TEXT("UEMMO M5-044: the projectile reservation refused (%d): %s"),
			static_cast<int32>(ReserveOutcome.Reject), *ReserveOutcome.RejectDetail);
		return;
	}
	if (ProjectileSpawner == nullptr)
	{
		ProjectileSpawner = MakeUnique<FVehicleTurretPelletSpawner>(World, Hull);
	}
	static_cast<FVehicleTurretPelletSpawner*>(ProjectileSpawner.Get())->Spawned.Reset();
	if (!ProjectileService->CommitSpawn(Reservation, *ProjectileSpawner))
	{
		UE_LOG(LogUEMMOVehicle, Warning, TEXT("UEMMO M5-044: the projectile commit failed: %s"), *Reservation.RejectDetail);
		return;
	}

	// One motion policy per spawned pellet; the hit context rides the
	// vehicle's own ledger and the hull as the shot source.
	FVehicleTurretPelletSpawner* Spawner = static_cast<FVehicleTurretPelletSpawner*>(ProjectileSpawner.Get());
	const int32 PelletTotal = FMath::Min(Plan.Pellets.Num(), Spawner->Spawned.Num());
	for (int32 PelletAt = 0; PelletAt < PelletTotal; ++PelletAt)
	{
		ACombatProjectile* Actor = Spawner->Spawned[PelletAt].Get();
		if (Actor == nullptr)
		{
			continue;
		}
		FProjectileHitContext HitContext;
		HitContext.Registry = &Weapon->GetFireRegistry();
		HitContext.Ledger = &TurretLedger;
		HitContext.Identity = this;
		HitContext.AttackProfile = *Profile;
		HitContext.PierceCount = Definition->PierceCount;
		HitContext.SourceActor = Hull;
		FVehicleLiveTurretPellet Entry;
		Entry.Actor = Actor;
		// The shipped turret weapon has no homing row; the target seam stays
		// untracked (invalid id) and a future homing row would inject the
		// provider here.
		Entry.Policy = MakeVehicleTurretMotionPolicy(*Definition, InvalidCombatTargetId);
		Entry.ShotEpoch = Shot.Epoch;
		Entry.ShotSource = Shot.SourceEntityId;
		Entry.ShotId = Shot.ShotId;
		if (Entry.Policy.IsValid() && Entry.Policy->Begin(*Actor, Plan.Pellets[PelletAt].Direction, HitContext))
		{
			LivePellets.Add(MoveTemp(Entry));
		}
		else
		{
			Actor->Destroy();
			UE_LOG(LogUEMMOVehicle, Warning, TEXT("UEMMO M5-044: failed projectile policy initialization; actor retired."));
		}
	}
	// M5-077: the shot is fully spawned - arm its ledger end tracking against
	// the actually bound pellets; a zero-pellet shot ends right here.
	if (PelletTotal <= 0)
	{
		TurretLedger.EndShot(Shot.Epoch, Shot.SourceEntityId, Shot.ShotId);
		return;
	}
	FVehiclePendingShotEnd Pending;
	Pending.Epoch = Shot.Epoch;
	Pending.Source = Shot.SourceEntityId;
	Pending.Shot = Shot.ShotId;
	Pending.LivePellets = PelletTotal;
	PendingShotEnds.Add(Pending);
	UE_LOG(LogUEMMOVehicle, Log, TEXT("UEMMO M5-044: the turret shot delivered (%d pellets from the turret pivot)."), PelletTotal);
}

void UVehicleTurretDeliveryComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	// The live pellets advance one logical step per frame; finished or
	// destroyed pellets drop out of the list (the pawn's loop verbatim).
	for (int32 Index = LivePellets.Num() - 1; Index >= 0; --Index)
	{
		FVehicleLiveTurretPellet& Entry = LivePellets[Index];
		ACombatProjectile* Actor = Entry.Actor.Get();
		if (Actor == nullptr)
		{
			RetirePellet(Index);
			continue;
		}
		const FVector PreviousPosition = Actor->GetActorLocation();
		Entry.Policy->AdvanceMotion(DeltaTime);
		Actor->FaceFlightDirection(Actor->GetActorLocation() - PreviousPosition);
		if (Entry.Policy->IsFinished() || Actor->IsPendingKillPending())
		{
			if (Entry.Policy->IsFinished() && !Actor->IsPendingKillPending())
			{
				Actor->Destroy();
			}
			RetirePellet(Index);
		}
	}
}

void UVehicleTurretDeliveryComponent::RetirePellet(int32 LiveIndex)
{
	if (!LivePellets.IsValidIndex(LiveIndex))
	{
		return;
	}
	const FCombatEpoch PelletEpoch = LivePellets[LiveIndex].ShotEpoch;
	const FEntityId PelletSource = LivePellets[LiveIndex].ShotSource;
	const FShotId PelletShotId = LivePellets[LiveIndex].ShotId;
	LivePellets.RemoveAtSwap(LiveIndex);

	for (int32 PendingIndex = PendingShotEnds.Num() - 1; PendingIndex >= 0; --PendingIndex)
	{
		FVehiclePendingShotEnd& Pending = PendingShotEnds[PendingIndex];
		if (Pending.Epoch != PelletEpoch || Pending.Source != PelletSource || Pending.Shot != PelletShotId)
		{
			continue;
		}
		--Pending.LivePellets;
		if (Pending.LivePellets <= 0)
		{
			// The LAST pellet of the shot retired: release its event/control
			// keys exactly once (the M5-077 semantics; the tombstone keeps
			// rejecting late resends).
			TurretLedger.EndShot(Pending.Epoch, Pending.Source, Pending.Shot);
			PendingShotEnds.RemoveAtSwap(PendingIndex);
		}
		break;
	}
}

void UVehicleTurretDeliveryComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The teardown: the live list clears (the actors die with the world) and
	// every held shot's ledger end runs here - the delivery's destruction
	// must not strand active ledger keys (the M5-077 cleanup face).
	for (const FVehiclePendingShotEnd& Pending : PendingShotEnds)
	{
		TurretLedger.EndShot(Pending.Epoch, Pending.Source, Pending.Shot);
	}
	PendingShotEnds.Reset();
	LivePellets.Reset();
	if (UWeaponComponent* Weapon = BoundTurretWeapon.Get())
	{
		Weapon->OnShotCommitted.RemoveAll(this);
	}
	BoundTurretWeapon = nullptr;
	Super::EndPlay(EndPlayReason);
}
