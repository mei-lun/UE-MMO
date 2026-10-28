// M5-043: the vehicle turret weapon mount (interface contract section 8
// "载具权限"). The suite drives the mount against the REAL shared weapon
// executor - no parallel weapon/ammo/damage stack anywhere:
//
//   1. Configuration: the shipped M5-037 mount row (mount_wheeled_turret ->
//      weapon_explosive_sample, pivot (0, 40, 120) cm, pitch -20..60) loads
//      the weapon definition and the ammo kind the id points to, straight
//      from the turret weapon component's own mounted catalog; the aim
//      summary reports the free yaw traverse plus the pitch clamp.
//   2. Locatable refusals: a null weapon component, an ownerless mount, an
//      invalid row, an unknown TurretWeaponId, a catalog-less weapon
//      component and an unresolvable ammo kind are all named refusals that
//      carry the offending id/field; every refused request changes nothing.
//   3. Turret clamp: pitch outside [-20, 60] and non-finite aims refuse with
//      zero side effects; the boundaries stay inclusive; the yaw traverse is
//      free (the M5-037 schema has no yaw field).
//   4. Operator gate: with no operator resolved, player fire is forbidden
//      outright (NoOperator, zero side effects); the seat model (M5-040) or
//      a direct provider injects the operator, and the fire-source
//      registration follows the operator faction (contract section 8).
//   5. Same-config delivery: the SAME weapon definition (a hitscan row on
//      the shared test round) bound to a handheld component and to the
//      turret commits identical shot contexts and delivers identical damage
//      (round(700/115) = 6) and identical ballistics through the ONE shared
//      hit ledger - the mount adds no dedicated damage path.
//   6. Attribution and epochs: the committed FShotContext source is the
//      VEHICLE fire source (the operator rides the mount's separate
//      record); after a world rebuild the stale source refuses (named,
//      side-effect free) and re-registration restores fire under the new
//      generation (the retry/leave-room face).
//   7. Cleanup: Finalize (and EndPlay on Destroy) cancels the in-flight
//      shot and the open reload window through the weapon component's own
//      cancel faces, bumps the binding generation and drops the injected
//      references; the mount is terminal afterwards.

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

#include "../Combat/Data/CombatCatalog.h"
#include "../Combat/System/CombatEntityRegistry.h"
#include "../Combat/System/HitLedger.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../Projectiles/HitscanExecutor.h"
#include "../Vehicles/CombatVehicle.h"
#include "../Combat/HealthComponent.h"
#include "../Vehicles/SeatModel.h"
#include "../Vehicles/VehicleTypes.h"
#include "../Vehicles/VehicleWeaponMountComponent.h"
#include "../Weapons/ShotPattern.h"
#include "../Weapons/WeaponComponent.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_043
{
	// Scene placement: a remote base so no other test's world content can
	// collide; X lateral, Y depth, Z height.
	const FVector M5_043_SceneBase(74000.0, 84000.0, 800.0);

	// The shipped M5-037/M5-038 ids (vehicles.json / vehicle_seats.json /
	// vehicle_mounts.json / weapons.json): the real production configuration,
	// not test slang - the mount row and the weapon kind are the shipped rows.
	const FName VehicleIdShipped = TEXT("vehicle_wheeled_sample");
	const FName SeatDriverShipped = TEXT("seat_wheeled_driver");
	const FName MountIdShipped = TEXT("mount_wheeled_turret");
	const FName WeaponExplosiveSample = TEXT("weapon_explosive_sample");
	const FName AmmoRocket = TEXT("ammo_rocket");
	const FName ProjectileRocketExplosive = TEXT("rocket_explosive");

	// The shared-config weapon of the damage/ballistics face: a hitscan row
	// both faces bind, plus its item mappings and the shared test round.
	const FName WeaponHitscanTest = TEXT("weapon_hitscan_vehicle");
	const FName AmmoCell = TEXT("ammo_cell");
	const FName DamageVehicleRound = TEXT("vehicle_test_round");
	const FName ItemTurretRocket = TEXT("item_turret_rocket");
	const FName ItemVehicleHitscan = TEXT("item_vehicle_hitscan");

	const FName FactionPlayer = TEXT("player");
	const FName FactionEnemy = TEXT("enemy");
	const FName FactionVehicle = TEXT("vehicle_test_faction");

	/** The bullet face: the M5-026 test round (base 7, control ride-along) - the same profile M5-039 pinned hull damage with. */
	FDamageProfile MakeVehicleTestRound()
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = DamageVehicleRound;
		Profile.BaseDamage = 7.0f;
		Profile.AttackCoefficient = 1.0f;
		Profile.HitStunSeconds = 1.0f;
		Profile.KnockbackCmPerSecond = 70.0f;
		Profile.LaunchCmPerSecond = 700.0f;
		Profile.HitStopSeconds = 0.04f;
		return Profile;
	}

	/**
	 * The fixture recipe (the M5-022 fixture shape extended with the vehicle
	 * segment): the shipped rocket turret row + ammo + projectile, the
	 * shared-config hitscan row, the shipped vehicle/seat/mount rows and the
	 * item mappings every weapon binds through (one item->weapon chain, no
	 * parallel mounting path).
	 */
	bool MakeCombatCatalog(FCombatCatalog& OutCatalog, FString& OutError)
	{
		FParsedCombatConfig Parsed;
		Parsed.SchemaVersion = 1;

		// The shared test round (both faces deliver through it) and the two
		// shipped profiles the rocket projectile references.
		Parsed.DamageProfiles.Add(MakeVehicleTestRound());

		FDamageProfile Physical10;
		Physical10.DamageProfileId = TEXT("physical_10");
		Physical10.BaseDamage = 10.0f;
		Physical10.AttackCoefficient = 1.0f;
		Physical10.HitStunSeconds = 0.2f;
		Physical10.KnockbackCmPerSecond = 90.0f;
		Physical10.HitStopSeconds = 0.04f;
		Parsed.DamageProfiles.Add(Physical10);

		FDamageProfile Explosive40 = Physical10;
		Explosive40.DamageProfileId = TEXT("explosive_40");
		Explosive40.BaseDamage = 40.0f;
		Parsed.DamageProfiles.Add(Explosive40);

		// The shared-config hitscan row: 12 rounds on ammo_cell, 240 rpm,
		// zero spread and the 5000 cm range - exact straight ballistics for
		// the endpoint equality assertion.
		FWeaponDefinition Hitscan;
		Hitscan.WeaponId = WeaponHitscanTest;
		Hitscan.FireMode = EWeaponFireMode::Hitscan;
		Hitscan.DamageProfileId = DamageVehicleRound;
		Hitscan.AmmoId = AmmoCell;
		Hitscan.MagazineSize = 12;
		Hitscan.FireRateRpm = 240.0f;
		Hitscan.BurstCount = 1;
		Hitscan.PelletCount = 1;
		Hitscan.SpreadDegrees = 0.0f;
		Hitscan.RangeCm = 5000.0f;
		Parsed.Weapons.Add(Hitscan);

		// The shipped turret row (weapons.json): projectile fire on
		// ammo_rocket, 3-round magazine, 45 rpm, rocket_explosive delivery.
		FWeaponDefinition Rocket;
		Rocket.WeaponId = WeaponExplosiveSample;
		Rocket.FireMode = EWeaponFireMode::Projectile;
		Rocket.AmmoId = AmmoRocket;
		Rocket.MagazineSize = 3;
		Rocket.FireRateRpm = 45.0f;
		Rocket.BurstCount = 1;
		Rocket.PelletCount = 1;
		Rocket.SpreadDegrees = 0.0f;
		Rocket.ProjectileId = ProjectileRocketExplosive;
		Parsed.Weapons.Add(Rocket);

		FAmmoType Cell;
		Cell.AmmoId = AmmoCell;
		Cell.MaxReserve = 120;
		Cell.MagazineSize = 12;
		Parsed.AmmoTypes.Add(Cell);

		FAmmoType RocketAmmo;
		RocketAmmo.AmmoId = AmmoRocket;
		RocketAmmo.MaxReserve = 12;
		RocketAmmo.MagazineSize = 3;
		Parsed.AmmoTypes.Add(RocketAmmo);

		// The shipped projectile row (projectiles.json): straight 1400 cm/s,
		// 4 s life, 300 cm explosion on explosive_40.
		FProjectileDefinition RocketProjectile;
		RocketProjectile.ProjectileId = ProjectileRocketExplosive;
		RocketProjectile.Motion = EProjectileMotion::Straight;
		RocketProjectile.SpeedCmS = 1400.0f;
		RocketProjectile.LifetimeS = 4.0f;
		RocketProjectile.DamageProfileId = TEXT("physical_10");
		RocketProjectile.PierceCount = 0;
		RocketProjectile.ExplosionRadiusCm = 300.0f;
		RocketProjectile.ExplosionDamageProfileId = TEXT("explosive_40");
		RocketProjectile.HomingTurnRateDegS = 0.0f;
		Parsed.Projectiles.Add(RocketProjectile);

		// The shipped vehicle/seat/mount rows (M5-037/M5-038).
		FVehicleDefinition Vehicle;
		Vehicle.VehicleId = VehicleIdShipped;
		Vehicle.DriveType = EVehicleDriveType::Ground;
		Vehicle.MaxHealth = 250.0f;
		Vehicle.Defense = 15.0f;
		Vehicle.MaxSpeedCmS = 700.0f;
		Vehicle.AccelerationCmS2 = 900.0f;
		Vehicle.MeshPath = TEXT("/Game/UEMMO/Vehicles/SM_Vehicle_Wheeled_Sample.SM_Vehicle_Wheeled_Sample");
		Vehicle.CameraDistanceCm = 650.0f;
		Vehicle.CameraHeightCm = 260.0f;
		Vehicle.CameraPitchDeg = -18.0f;
		Parsed.Vehicles.Add(Vehicle);

		FVehicleSeatDefinition Seat;
		Seat.SeatId = SeatDriverShipped;
		Seat.VehicleId = VehicleIdShipped;
		Seat.SeatRole = EVehicleSeatRole::Driver;
		Seat.SeatLocationCm = FVector(0.0, -40.0, 45.0);
		Seat.ExitOffsetCm = FVector(-180.0, 0.0, 0.0);
		Parsed.VehicleSeats.Add(Seat);

		FVehicleMountDefinition Mount;
		Mount.MountId = MountIdShipped;
		Mount.VehicleId = VehicleIdShipped;
		Mount.TurretWeaponId = WeaponExplosiveSample;
		Mount.LocationCm = FVector(0.0, 40.0, 120.0);
		Mount.MinPitchDegrees = -20.0f;
		Mount.MaxPitchDegrees = 60.0f;
		Parsed.VehicleMounts.Add(Mount);

		return FCombatCatalog::BuildFromParsed(Parsed, OutCatalog, OutError);
	}

	/** The item catalog fixture: the two weapon-slot items every bind goes through. */
	FItemDefinitionCatalog MakeItemCatalog()
	{
		FItemDefinitionCatalog Catalog;

		FItemDefinition RocketItem;
		RocketItem.DefinitionId = ItemTurretRocket;
		RocketItem.DisplayName = TEXT("Turret Rocket");
		RocketItem.Slot = EItemSlot::Weapon;
		RocketItem.BaseStats.Attack = 0.0f;
		RocketItem.Rarity = EItemRarity::Normal;
		RocketItem.WeaponDefinitionId = WeaponExplosiveSample;
		Catalog.AddDefinition(RocketItem);

		FItemDefinition HitscanItem;
		HitscanItem.DefinitionId = ItemVehicleHitscan;
		HitscanItem.DisplayName = TEXT("Vehicle Hitscan");
		HitscanItem.Slot = EItemSlot::Weapon;
		HitscanItem.BaseStats.Attack = 0.0f;
		HitscanItem.Rarity = EItemRarity::Normal;
		HitscanItem.WeaponDefinitionId = WeaponHitscanTest;
		Catalog.AddDefinition(HitscanItem);

		return Catalog;
	}

	/** Deterministic item instance (the M3 fixture style). */
	FItemInstance MakeInstance(uint32 Seed, const FName DefinitionId)
	{
		FItemInstance Instance;
		Instance.InstanceId = FGuid(0x05430000u + Seed, 0x0043u, Seed + 20u, Seed * 7u + 5u);
		Instance.DefinitionId = DefinitionId;
		Instance.RollSeed = static_cast<int64>(0x543 + Seed);
		Instance.Level = 1;
		return Instance;
	}

	/**
	 * World acquisition, the M5_012 pattern PLUS the engine FTestWorldWrapper's
	 * full world shape (the M5-047 lesson M5-039 recorded): a GameInstance, a
	 * registered world context and SetGameMode - without them the engine
	 * skips PostInitializeComponents/BeginPlay dispatch and EndPlay routing
	 * on Destroy(), which would keep the mount's EndPlay cleanup face from
	 * ever running.
	 */
	static UWorld* M5_043_AcquireWorld()
	{
		UGameInstance* TestGameInstance = NewObject<UGameInstance>(GEngine);
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M5_043_TestWorld")));
		if (TempWorld != nullptr)
		{
			TempWorld->SetShouldTick(false);
			FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
			WorldContext.OwningGameInstance = TestGameInstance;
			WorldContext.SetCurrentWorld(TempWorld);
			TempWorld->SetGameInstance(TestGameInstance);
			return TempWorld;
		}
		UE_LOG(LogTemp, Log, TEXT("M5_043 world source: GWorld fallback (temp world unavailable)"));
		return GWorld;
	}

	/** Destroys the temp world again; each test owns exactly one. */
	struct FM5_043_WorldScope
	{
		UWorld* World = nullptr;

		~FM5_043_WorldScope()
		{
			if (World != nullptr && World != GWorld && GEngine != nullptr)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			}
		}
	};

	/** Test seam for the hit-to-entity face (the M5_026/M5_039 pattern): the scene's id map. */
	struct FM5_043_TestIdentity : IHitscanTargetIdentity
	{
		TMap<const AActor*, FEntityId> IdsByActor;

		void Record(const AActor& Actor, FEntityId Id)
		{
			IdsByActor.Add(&Actor, Id);
		}

		virtual FEntityId ResolveTargetEntityId(const AActor& HitActor) const override
		{
			const FEntityId* Id = IdsByActor.Find(&HitActor);
			return Id != nullptr ? *Id : InvalidCombatTargetId;
		}
	};

	// Forward declaration: the rig's Build is defined after the full scene
	// (the rig needs the fixture pair and the fire clock it owns).
	struct FM5_043_Scene;

	/**
	 * One turret rig on one hull: the vehicle body, its turret weapon
	 * component (the SAME WeaponComponent class a handheld pawn carries -
	 * bound through the standard item->weapon chain) and the mount
	 * component configured with the shipped mount row. DECLARATION ORDER:
	 * rigs live inside the scene struct declared BEFORE the world scope, so
	 * they outlive the world's actor teardown (the M5-039 discipline).
	 */
	struct FM5_043_TurretRig
	{
		ACombatVehicle* Vehicle = nullptr;
		UWeaponComponent* Weapon = nullptr;
		UVehicleWeaponMountComponent* Mount = nullptr;
		FItemInstance TurretInstance;

		bool Build(FAutomationTestBase& Test, UWorld& World, FM5_043_Scene& Scene, const FVector& Location);
	};

	/**
	 * One scene: the fixture pair, the seat model, the ONE shared hit ledger
	 * (both faces deliver through it), the identity seam, the handheld face
	 * (shooter body + its own weapon component + registered fire source) and
	 * the manually advanced fire clock both weapon components read.
	 */
	struct FM5_043_Scene
	{
		FCombatCatalog Catalog;
		FItemDefinitionCatalog Items;
		FSeatOccupancyModel Seats;
		FHitLedger Ledger;
		FM5_043_TestIdentity Identity;
		double Now = 100.0;

		AActor* Shooter = nullptr;
		UWeaponComponent* HandheldWeapon = nullptr;
		FItemInstance HandheldInstance;
		FEntityId HandheldSourceId = InvalidCombatEntityId;

		bool Build(FAutomationTestBase& Test, UWorld& World)
		{
			// The full world shape (the M5-047 lesson): the engine dispatches
			// BeginPlay and routes EndPlay on Destroy only for worlds past
			// actor initialization - the mount's EndPlay cleanup face needs
			// this exactly like M5-039's death sequence did.
			FURL Url;
			World.SetGameMode(Url);
			if (!Test.TestTrue(TEXT("the world's game mode initialized"),
					World.GetAuthGameMode() != nullptr && World.GetWorldSettings() != nullptr))
			{
				return false;
			}
			World.InitializeActorsForPlay(Url);
			World.BeginPlay();

			if (!MakeCombatCatalog(Catalog, CatalogError))
			{
				Test.AddError(FString::Printf(TEXT("the fixture catalog refused to build: %s"), *CatalogError));
				return false;
			}
			Items = MakeItemCatalog();

			// The handheld face: a bare body (no collision - traces start
			// past it) with its own weapon component bound to the SAME
			// hitscan definition the turret face binds.
			Shooter = World.SpawnActor<AActor>(AActor::StaticClass(), M5_043_SceneBase + FVector(-400.0, 40.0, 120.0), FRotator::ZeroRotator);
			if (Shooter == nullptr)
			{
				Test.AddError(TEXT("the shooter actor could not spawn"));
				return false;
			}
			HandheldWeapon = NewObject<UWeaponComponent>(GetTransientPackage());
			if (!HandheldWeapon->MountCatalogs(&Catalog, &Items))
			{
				Test.AddError(TEXT("the handheld weapon refused the fixture pair"));
				return false;
			}
			if (!HandheldWeapon->GetAmmoModel().RegisterAmmoType(AmmoCell, 120, 60))
			{
				Test.AddError(TEXT("the handheld reserve pool registration refused"));
				return false;
			}
			HandheldInstance = MakeInstance(0x41, ItemVehicleHitscan);
			if (!HandheldWeapon->ApplyEquippedWeapon(&HandheldInstance).bWeaponBound)
			{
				Test.AddError(TEXT("the handheld weapon did not bind"));
				return false;
			}
			FCombatEntityMetadata SourceMetadata;
			SourceMetadata.Faction = FactionPlayer;
			SourceMetadata.Category = FName(TEXT("player"));
			HandheldSourceId = HandheldWeapon->RegisterFireSource(SourceMetadata);
			if (!IsValidCombatEntityId(HandheldSourceId))
			{
				Test.AddError(TEXT("the handheld fire source could not register"));
				return false;
			}
			HandheldWeapon->SetFireClockProvider([this]() { return Now; });
			return true;
		}

		void AdvanceClock(double Seconds)
		{
			Now += Seconds;
		}

		/** Spawns a hull from the shipped vehicle definition (HP 250 / defense 15). */
		ACombatVehicle* SpawnHull(FAutomationTestBase& Test, UWorld& World, const FVector& Location)
		{
			ACombatVehicle* Hull = World.SpawnActor<ACombatVehicle>(ACombatVehicle::StaticClass(), Location, FRotator::ZeroRotator);
			if (Hull == nullptr)
			{
				Test.AddError(TEXT("the hull actor could not spawn"));
				return nullptr;
			}
			const FVehicleDefinition* Definition = Catalog.FindVehicle(VehicleIdShipped);
			FString Errors;
			if (Definition == nullptr || !Hull->SetVehicleDefinition(*Definition, &Errors))
			{
				Test.AddError(FString::Printf(TEXT("the hull definition was refused: %s"),
					Definition != nullptr ? *Errors : TEXT("the shipped vehicle row did not resolve")));
				return nullptr;
			}
			return Hull;
		}

		/** Registers a hull as a hit target in the given face's fire registry. */
		FEntityId RegisterTarget(FCombatEntityRegistry& Registry, ACombatVehicle& Hull, FName Faction)
		{
			FCombatEntityMetadata Metadata;
			Metadata.Faction = Faction;
			Metadata.Category = FName(TEXT("vehicle"));
			const FEntityId Id = Registry.RegisterEntity(&Hull, Metadata, Registry.GetCurrentEpoch());
			if (IsValidCombatEntityId(Id))
			{
				Identity.Record(Hull, Id);
			}
			return Id;
		}

		/** Registers the shipped driver seat (idempotent) and seats the occupant. */
		bool SeatOperator(FAutomationTestBase& Test, uint32 OccupantSeed)
		{
			if (!Seats.IsSeatRegistered(VehicleIdShipped, SeatDriverShipped))
			{
				EVehicleSeatRejectReason Reason = EVehicleSeatRejectReason::None;
				if (!Seats.RegisterSeat(VehicleIdShipped, SeatDriverShipped, Reason))
				{
					Test.AddError(TEXT("the driver seat could not register in the occupancy model"));
					return false;
				}
			}
			FVehicleSeatEnterRequest Enter;
			Enter.VehicleId = VehicleIdShipped;
			Enter.SeatId = SeatDriverShipped;
			Enter.OccupantId = FGuid(OccupantSeed, OccupantSeed + 1, OccupantSeed + 2, OccupantSeed + 3);
			EVehicleSeatRejectReason Reason = EVehicleSeatRejectReason::None;
			if (!Seats.TryEnterSeat(Enter, Reason))
			{
				Test.AddError(FString::Printf(TEXT("the operator could not enter the seat (reason %d)"), static_cast<int32>(Reason)));
				return false;
			}
			return true;
		}

		/** The seat model's current occupant of the shipped driver seat (invalid GUID while free). */
		FGuid DriverOccupant() const
		{
			FGuid OccupantId;
			Seats.FindSeatOccupant(VehicleIdShipped, SeatDriverShipped, OccupantId);
			return OccupantId;
		}

		FString CatalogError;
	};

	bool FM5_043_TurretRig::Build(FAutomationTestBase& Test, UWorld& World, FM5_043_Scene& Scene, const FVector& Location)
	{
		Vehicle = Scene.SpawnHull(Test, World, Location);
		if (Vehicle == nullptr)
		{
			return false;
		}

		// The turret weapon: a real UWeaponComponent owned by the vehicle
		// (the fire-source registration enters the vehicle actor), bound
		// through the standard ApplyEquippedWeapon chain with a
		// pre-provisioned ammo_rocket reserve (supply belongs to the supply
		// lineage - the mount never mints ammunition).
		Weapon = NewObject<UWeaponComponent>(Vehicle);
		Vehicle->AddInstanceComponent(Weapon);
		Weapon->RegisterComponent();
		if (!Weapon->MountCatalogs(&Scene.Catalog, &Scene.Items))
		{
			Test.AddError(TEXT("the turret weapon refused the fixture pair"));
			return false;
		}
		if (!Weapon->GetAmmoModel().RegisterAmmoType(AmmoRocket, 12, 12))
		{
			Test.AddError(TEXT("the turret reserve pool registration refused"));
			return false;
		}
		TurretInstance = MakeInstance(0x42, ItemTurretRocket);
		if (!Weapon->ApplyEquippedWeapon(&TurretInstance).bWeaponBound)
		{
			Test.AddError(TEXT("the turret weapon did not bind"));
			return false;
		}
		Weapon->SetFireClockProvider([&Scene]() { return Scene.Now; });

		// The mount: a real component on the vehicle, configured with the
		// shipped row straight from the fixture catalog.
		Mount = NewObject<UVehicleWeaponMountComponent>(Vehicle);
		Vehicle->AddInstanceComponent(Mount);
		Mount->RegisterComponent();
		const FVehicleMountDefinition* MountRow = Scene.Catalog.FindVehicleMount(MountIdShipped);
		if (MountRow == nullptr)
		{
			Test.AddError(TEXT("the shipped mount row did not resolve in the fixture catalog"));
			return false;
		}
		const FVehicleMountConfigOutcome Configured = Mount->Configure(*MountRow, Weapon);
		if (!Configured.bConfigured)
		{
			Test.AddError(FString::Printf(TEXT("the shipped mount row refused to configure (%d): %s"),
				static_cast<int32>(Configured.Reject), *Configured.RejectDetail));
			return false;
		}
		return true;
	}

	// ------------------------------------------------------------------
	// 1. The shipped mount row loads the weapon/ammo/angles it points to
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_043ConfiguresShippedMountRowAndSurfacesAngles,
		"UEMMO.Tasks.M5_043.ConfiguresShippedMountRowAndSurfacesAngles",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_043ConfiguresShippedMountRowAndSurfacesAngles::RunTest(const FString& Parameters)
	{
		UWorld* World = M5_043_AcquireWorld();
		FM5_043_WorldScope Scope{ World };
		FM5_043_Scene Scene;
		if (!Scene.Build(*this, *World) || !World)
		{
			return false;
		}
		FM5_043_TurretRig Rig;
		if (!Rig.Build(*this, *World, Scene, M5_043_SceneBase + FVector(0.0, 2000.0, 0.0)))
		{
			return false;
		}

		// The parsed row: ids, the pivot offset and the pitch clamp arrived
		// verbatim from the shipped M5-037 values.
		UVehicleWeaponMountComponent* Mount = Rig.Mount;
		TestTrue(TEXT("the shipped mount row configured"), Mount->IsConfigured());
		TestEqual(TEXT("the mount id is the shipped one"), Mount->GetMountedDefinition().MountId, MountIdShipped);
		TestEqual(TEXT("the turret weapon id is the shipped one"), Mount->GetMountedDefinition().TurretWeaponId, WeaponExplosiveSample);
		TestTrue(TEXT("the pivot offset is the fixture (0, 40, 120) cm"),
			Mount->GetMountedDefinition().LocationCm.Equals(FVector(0.0, 40.0, 120.0)));
		TestEqual(TEXT("the min pitch is the shipped -20 degrees"), Mount->GetMountedDefinition().MinPitchDegrees, -20.0f);
		TestEqual(TEXT("the max pitch is the shipped 60 degrees"), Mount->GetMountedDefinition().MaxPitchDegrees, 60.0f);

		// The loaded weapon definition + ammo (the id's targets), read from
		// the catalog the turret weapon component actually mounted.
		const FWeaponDefinition* TurretWeapon = Mount->GetTurretWeaponDefinition();
		TestNotNull(TEXT("the turret weapon definition loaded"), TurretWeapon);
		if (TurretWeapon != nullptr)
		{
			TestEqual(TEXT("the loaded weapon is the shipped rocket"), TurretWeapon->WeaponId, WeaponExplosiveSample);
			TestEqual(TEXT("the loaded weapon fires projectiles"),
				static_cast<int32>(TurretWeapon->FireMode), static_cast<int32>(EWeaponFireMode::Projectile));
			TestEqual(TEXT("the loaded weapon carries the shipped 3-round magazine"), TurretWeapon->MagazineSize, 3);
			TestEqual(TEXT("the loaded weapon references the shipped rocket projectile"), TurretWeapon->ProjectileId, ProjectileRocketExplosive);
			TestEqual(TEXT("the loaded weapon consumes the shipped rocket ammo"), TurretWeapon->AmmoId, AmmoRocket);
		}
		TestTrue(TEXT("the ammo kind resolved in the same catalog"),
			Rig.Weapon->GetMountedWeaponCatalog()->FindAmmoType(AmmoRocket) != nullptr);
		TestEqual(TEXT("the shared reserve pool is the pre-provisioned supply"),
			Rig.Weapon->GetAmmoModel().GetReserveRounds(AmmoRocket), 12);

		// The clamp summary: free yaw traverse + the pitch clamp, readable
		// for diagnostics and the M5-044 wiring.
		const FString Summary = Mount->GetAimClampSummary();
		TestTrue(TEXT("the summary reports the free yaw traverse"), Summary.Contains(TEXT("yaw free")));
		TestTrue(TEXT("the summary reports the shipped pitch clamp"), Summary.Contains(FString::Printf(TEXT("[%s, %s] degrees"), *FString::SanitizeFloat(-20.0f), *FString::SanitizeFloat(60.0f))));

		// The fire lineage: the vehicle registers as the turret's fire source
		// (the weapon component's owner actor), unaligned until an operator
		// or a vehicle faction exists.
		TestTrue(TEXT("the vehicle fire source registered"), Mount->RefreshVehicleFireSource());
		const FEntityId SourceId = Mount->GetVehicleFireSourceEntityId();
		TestTrue(TEXT("the fire source id is valid"), IsValidCombatEntityId(SourceId));
		TestTrue(TEXT("the fire source resolves to the vehicle actor"),
			Rig.Weapon->GetFireRegistry().ResolveEntity(SourceId).Get() == static_cast<AActor*>(Rig.Vehicle));
		const FCombatEntityRecord* SourceRecord = Rig.Weapon->GetFireRegistry().FindEntity(SourceId);
		TestNotNull(TEXT("the fire source record exists"), SourceRecord);
		if (SourceRecord != nullptr)
		{
			TestEqual(TEXT("the fire source carries the vehicle_turret category"), SourceRecord->Metadata.Category, FName(TEXT("vehicle_turret")));
			TestTrue(TEXT("the fire source starts unaligned (no operator, no vehicle faction)"), SourceRecord->Metadata.Faction.IsNone());
		}
		return true;
	}

	// ------------------------------------------------------------------
	// 2. Configure refusals are named and id-carrying (locatable errors)
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_043ConfigureRefusesWithLocatableErrors,
		"UEMMO.Tasks.M5_043.ConfigureRefusesWithLocatableErrors",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_043ConfigureRefusesWithLocatableErrors::RunTest(const FString& Parameters)
	{
		UWorld* World = M5_043_AcquireWorld();
		FM5_043_WorldScope Scope{ World };
		FM5_043_Scene Scene;
		if (!Scene.Build(*this, *World) || !World)
		{
			return false;
		}
		FM5_043_TurretRig Rig;
		if (!Rig.Build(*this, *World, Scene, M5_043_SceneBase + FVector(0.0, 2000.0, 0.0)))
		{
			return false;
		}
		UVehicleWeaponMountComponent* Mount = Rig.Mount;
		const FVehicleMountDefinition& ShippedRow = Mount->GetMountedDefinition();

		// (a) A null turret weapon component: named, and the previously
		// loaded configuration survives untouched (a refused request is
		// never a teardown).
		const FVehicleMountConfigOutcome NullWeapon = Mount->Configure(ShippedRow, nullptr);
		TestFalse(TEXT("the null-weapon configure refused"), NullWeapon.bConfigured);
		TestEqual(TEXT("the null-weapon refusal is named"),
			static_cast<int32>(NullWeapon.Reject), static_cast<int32>(EVehicleMountConfigReject::WeaponComponentMissing));
		TestTrue(TEXT("the null-weapon refusal changed nothing"), Mount->IsConfigured());

		// (b) An ownerless mount: the pivot and the fire source need the
		// vehicle body.
		UVehicleWeaponMountComponent* Ownerless = NewObject<UVehicleWeaponMountComponent>(GetTransientPackage());
		const FVehicleMountConfigOutcome NoOwner = Ownerless->Configure(ShippedRow, Scene.HandheldWeapon);
		TestFalse(TEXT("the ownerless configure refused"), NoOwner.bConfigured);
		TestEqual(TEXT("the ownerless refusal is named"),
			static_cast<int32>(NoOwner.Reject), static_cast<int32>(EVehicleMountConfigReject::OwnerActorMissing));

		// (c) An invalid row (absent turret weapon id + inverted pitch): the
		// M5-037 validator problems surface with the named fields.
		FVehicleMountDefinition InvalidRow = ShippedRow;
		InvalidRow.TurretWeaponId = NAME_None;
		InvalidRow.MinPitchDegrees = 89.0f;
		InvalidRow.MaxPitchDegrees = -89.0f;
		const FVehicleMountConfigOutcome Invalid = Mount->Configure(InvalidRow, Rig.Weapon);
		TestFalse(TEXT("the invalid-row configure refused"), Invalid.bConfigured);
		TestEqual(TEXT("the invalid-row refusal is named"),
			static_cast<int32>(Invalid.Reject), static_cast<int32>(EVehicleMountConfigReject::MountDefinitionInvalid));
		TestTrue(TEXT("the invalid-row detail names TurretWeaponId"), Invalid.RejectDetail.Contains(TEXT("TurretWeaponId")));
		TestTrue(TEXT("the invalid-row detail names the inverted pitch fields"), Invalid.RejectDetail.Contains(TEXT("MinPitchDegrees")));
		TestTrue(TEXT("the invalid-row configure changed nothing"), Mount->IsConfigured());

		// (d) An unknown TurretWeaponId: the missing weapon is a named,
		// id-carrying refusal ("缺武器" is never a fake success).
		FVehicleMountDefinition UnknownWeaponRow = ShippedRow;
		UnknownWeaponRow.TurretWeaponId = FName(TEXT("weapon_missing_sample"));
		const FVehicleMountConfigOutcome Missing = Mount->Configure(UnknownWeaponRow, Rig.Weapon);
		TestFalse(TEXT("the missing-weapon configure refused"), Missing.bConfigured);
		TestEqual(TEXT("the missing-weapon refusal is named"),
			static_cast<int32>(Missing.Reject), static_cast<int32>(EVehicleMountConfigReject::WeaponDefinitionMissing));
		TestTrue(TEXT("the missing-weapon detail names the unresolved id"),
			Missing.RejectDetail.Contains(TEXT("weapon_missing_sample")));

		// (e) A catalog-less weapon component: the M5-020 latch holds.
		UWeaponComponent* Unmounted = NewObject<UWeaponComponent>(GetTransientPackage());
		const FVehicleMountConfigOutcome NoCatalog = Mount->Configure(ShippedRow, Unmounted);
		TestFalse(TEXT("the catalog-less configure refused"), NoCatalog.bConfigured);
		TestEqual(TEXT("the catalog-less refusal is named"),
			static_cast<int32>(NoCatalog.Reject), static_cast<int32>(EVehicleMountConfigReject::CatalogUnavailable));

		// (f) An unresolvable ammo kind (a catalog is allowed to carry a
		// weapon whose ammo row is absent; the bind path would refuse first,
		// this gate protects the direct-Configure path).
		FParsedCombatConfig AmmolessParsed;
		AmmolessParsed.SchemaVersion = 1;
		AmmolessParsed.DamageProfiles.Add(MakeVehicleTestRound());
		FWeaponDefinition Ammoless;
		Ammoless.WeaponId = TEXT("weapon_ammoless_test");
		Ammoless.FireMode = EWeaponFireMode::Hitscan;
		Ammoless.DamageProfileId = DamageVehicleRound;
		Ammoless.AmmoId = TEXT("ammo_missing");
		Ammoless.MagazineSize = 4;
		Ammoless.FireRateRpm = 60.0f;
		Ammoless.RangeCm = 3000.0f;
		AmmolessParsed.Weapons.Add(Ammoless);
		FCombatCatalog AmmolessCatalog;
		FString CatalogError;
		if (!TestTrue(TEXT("the ammoless fixture catalog built"), FCombatCatalog::BuildFromParsed(AmmolessParsed, AmmolessCatalog, CatalogError)))
		{
			AddError(FString::Printf(TEXT("fixture catalog error: %s"), *CatalogError));
			return false;
		}
		UWeaponComponent* AmmolessWeapon = NewObject<UWeaponComponent>(GetTransientPackage());
		if (!TestTrue(TEXT("the ammoless weapon mounted its catalog"), AmmolessWeapon->MountCatalogs(&AmmolessCatalog, &Scene.Items)))
		{
			return false;
		}
		FVehicleMountDefinition AmmolessRow = ShippedRow;
		AmmolessRow.TurretWeaponId = TEXT("weapon_ammoless_test");
		const FVehicleMountConfigOutcome NoAmmo = Mount->Configure(AmmolessRow, AmmolessWeapon);
		TestFalse(TEXT("the ammoless configure refused"), NoAmmo.bConfigured);
		TestEqual(TEXT("the ammoless refusal is named"),
			static_cast<int32>(NoAmmo.Reject), static_cast<int32>(EVehicleMountConfigReject::AmmoUnavailable));
		TestTrue(TEXT("the ammoless detail names the missing ammo kind"), NoAmmo.RejectDetail.Contains(TEXT("ammo_missing")));

		// (g) Firing before the fire lineage exists: the mount refuses with
		// the locatable reason before the transaction, zero side effects.
		const FVehicleMountAim Aim;
		const FVehicleMountFireCheck MissingSource = Mount->CanFire(Aim);
		TestFalse(TEXT("the unregistered fire source cannot fire"), MissingSource.bCanFire);
		TestEqual(TEXT("the missing-source refusal is named"),
			static_cast<int32>(MissingSource.Reject), static_cast<int32>(EVehicleMountFireReject::FireSourceMissing));
		TestTrue(TEXT("the missing-source detail names the refresh call"), MissingSource.RejectDetail.Contains(TEXT("RefreshVehicleFireSource")));
		const FVehicleMountFireOutcome RefusedFire = Mount->TryFire(Aim);
		TestFalse(TEXT("the missing-source fire refused"), RefusedFire.bFired);
		TestEqual(TEXT("the missing-source fire deducted nothing"),
			Rig.Weapon->GetActiveBinding()->LoadedRounds, 3);
		TestEqual(TEXT("the missing-source fire minted no shot"), Mount->GetMountedShotCount(), 0);
		return true;
	}

	// ------------------------------------------------------------------
	// 3. The turret clamp: pitch refuses, boundaries stay inclusive, yaw free
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_043PitchClampRefusesAndYawTraverseIsFree,
		"UEMMO.Tasks.M5_043.PitchClampRefusesAndYawTraverseIsFree",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_043PitchClampRefusesAndYawTraverseIsFree::RunTest(const FString& Parameters)
	{
		UWorld* World = M5_043_AcquireWorld();
		FM5_043_WorldScope Scope{ World };
		FM5_043_Scene Scene;
		if (!Scene.Build(*this, *World) || !World)
		{
			return false;
		}
		FM5_043_TurretRig Rig;
		if (!Rig.Build(*this, *World, Scene, M5_043_SceneBase + FVector(0.0, 2000.0, 0.0)))
		{
			return false;
		}
		UVehicleWeaponMountComponent* Mount = Rig.Mount;

		// The operator and the fire lineage are in place; only the aim varies.
		if (!Scene.SeatOperator(*this, 43))
		{
			return false;
		}
		Mount->SetOperatorSeatSource(&Scene.Seats, VehicleIdShipped, SeatDriverShipped);
		Mount->SetOperatorFaction(FactionPlayer);
		Mount->NotifyOperatorChanged();
		TestTrue(TEXT("the turret armed after the operator entered"), Mount->CanFire(FVehicleMountAim()).bCanFire);

		// Out-of-clamp pitches refuse with the named reason and ZERO side
		// effects: no round deducted, no sequence minted, no cooldown started.
		const FVehicleMountFireOutcome HighPitch = Mount->TryFire(FVehicleMountAim{0.0f, 61.0f});
		TestFalse(TEXT("the +61 degree pitch refused"), HighPitch.bFired);
		TestEqual(TEXT("the high-pitch refusal is named"),
			static_cast<int32>(HighPitch.Reject), static_cast<int32>(EVehicleMountFireReject::PitchOutOfRange));
		const FVehicleMountFireOutcome LowPitch = Mount->TryFire(FVehicleMountAim{0.0f, -21.0f});
		TestFalse(TEXT("the -21 degree pitch refused"), LowPitch.bFired);
		TestEqual(TEXT("the low-pitch refusal is named"),
			static_cast<int32>(LowPitch.Reject), static_cast<int32>(EVehicleMountFireReject::PitchOutOfRange));
		TestTrue(TEXT("the out-of-clamp refusals name the clamp bounds"), LowPitch.RejectDetail.Contains(FString::Printf(TEXT("[%s, %s]"), *FString::SanitizeFloat(-20.0f), *FString::SanitizeFloat(60.0f))));
		TestEqual(TEXT("the refused attempts deducted nothing"),
			Rig.Weapon->GetActiveBinding()->LoadedRounds, 3);
		TestEqual(TEXT("the refused attempts minted no shot"), Mount->GetMountedShotCount(), 0);
		TestEqual(TEXT("the refused attempts started no cooldown"), Rig.Weapon->GetNextFireTimeSeconds(), 0.0);
		const FVehicleMountFireCheck HighCheck = Mount->CanFire(FVehicleMountAim{0.0f, 61.0f});
		TestFalse(TEXT("CanFire agrees with TryFire out of clamp"), HighCheck.bCanFire);
		TestEqual(TEXT("CanFire names the same reason"),
			static_cast<int32>(HighCheck.Reject), static_cast<int32>(EVehicleMountFireReject::PitchOutOfRange));

		// Non-finite aims refuse before the range gate.
		const float QuietNaN = std::numeric_limits<float>::quiet_NaN();
		const FVehicleMountFireOutcome NaNPitch = Mount->TryFire(FVehicleMountAim{0.0f, QuietNaN});
		TestEqual(TEXT("a NaN pitch refuses as an invalid aim"),
			static_cast<int32>(NaNPitch.Reject), static_cast<int32>(EVehicleMountFireReject::AimInvalid));
		const FVehicleMountFireOutcome NaNYaw = Mount->TryFire(FVehicleMountAim{QuietNaN, 0.0f});
		TestEqual(TEXT("a NaN yaw refuses as an invalid aim"),
			static_cast<int32>(NaNYaw.Reject), static_cast<int32>(EVehicleMountFireReject::AimInvalid));
		const FVehicleMountFireOutcome InfYaw = Mount->TryFire(FVehicleMountAim{std::numeric_limits<float>::infinity(), 0.0f});
		TestEqual(TEXT("an infinite yaw refuses as an invalid aim"),
			static_cast<int32>(InfYaw.Reject), static_cast<int32>(EVehicleMountFireReject::AimInvalid));

		// The boundaries are inclusive: exactly +60 and exactly -20 fire.
		const FVehicleMountFireOutcome MaxPitch = Mount->TryFire(FVehicleMountAim{0.0f, 60.0f});
		TestTrue(TEXT("the +60 degree boundary pitch fired"), MaxPitch.bFired);
		Mount->ReleaseMountShot();
		Scene.AdvanceClock(1.4); // the 45 rpm cooldown is 60/45 = 1.33 s
		const FVehicleMountFireOutcome MinPitch = Mount->TryFire(FVehicleMountAim{0.0f, -20.0f});
		TestTrue(TEXT("the -20 degree boundary pitch fired"), MinPitch.bFired);
		Mount->ReleaseMountShot();

		// The yaw traverse is free: the M5-037 schema has no yaw field, so no
		// yaw angle can be out of range (YawOutOfRange stays reserved for the
		// schema change) - only finiteness is policed.
		Scene.AdvanceClock(1.4);
		TestTrue(TEXT("a 359.5 degree yaw is admissible"), Mount->CanFire(FVehicleMountAim{359.5f, 0.0f}).bCanFire);
		TestTrue(TEXT("a -180 degree yaw is admissible"), Mount->CanFire(FVehicleMountAim{-180.0f, 10.0f}).bCanFire);
		TestTrue(TEXT("a 180 degree yaw is admissible"), Mount->CanFire(FVehicleMountAim{180.0f, 0.0f}).bCanFire);

		// The last committed aim is the delivery's aim snapshot.
		FVehicleMountAim LastAim;
		TestTrue(TEXT("the last committed aim is readable"), Mount->GetLastCommittedAim(LastAim));
		TestEqual(TEXT("the last committed aim is the -20 degree boundary shot"), LastAim.PitchDegrees, -20.0f);
		return true;
	}

	// ------------------------------------------------------------------
	// 4. No operator, no player fire; the operator state is injectable
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_043NoOperatorForbidsPlayerFire,
		"UEMMO.Tasks.M5_043.NoOperatorForbidsPlayerFire",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_043NoOperatorForbidsPlayerFire::RunTest(const FString& Parameters)
	{
		UWorld* World = M5_043_AcquireWorld();
		FM5_043_WorldScope Scope{ World };
		FM5_043_Scene Scene;
		if (!Scene.Build(*this, *World) || !World)
		{
			return false;
		}
		FM5_043_TurretRig Rig;
		if (!Rig.Build(*this, *World, Scene, M5_043_SceneBase + FVector(0.0, 2000.0, 0.0)))
		{
			return false;
		}
		UVehicleWeaponMountComponent* Mount = Rig.Mount;
		TestTrue(TEXT("the vehicle fire source registered"), Mount->RefreshVehicleFireSource());

		// (a) Without an operator source the operator is absent and player
		// fire is forbidden outright ("无操作者禁止玩家开火") - zero side
		// effects: no round, no shot, no cooldown.
		const FVehicleMountOperatorState Absent = Mount->ResolveOperator();
		TestFalse(TEXT("the operator is absent without an injected source"), Absent.bPresent);
		const FVehicleMountFireCheck OperatorCheck = Mount->CanFire(FVehicleMountAim());
		TestFalse(TEXT("the operator-less mount cannot fire"), OperatorCheck.bCanFire);
		TestEqual(TEXT("the no-operator refusal is named"),
			static_cast<int32>(OperatorCheck.Reject), static_cast<int32>(EVehicleMountFireReject::NoOperator));
		const FVehicleMountFireOutcome OperatorFire = Mount->TryFire(FVehicleMountAim());
		TestFalse(TEXT("the operator-less fire refused"), OperatorFire.bFired);
		TestEqual(TEXT("the operator-less fire deducted nothing"),
			Rig.Weapon->GetActiveBinding()->LoadedRounds, 3);
		TestEqual(TEXT("the operator-less fire minted no shot"), Mount->GetMountedShotCount(), 0);
		TestEqual(TEXT("the operator-less fire started no cooldown"), Rig.Weapon->GetNextFireTimeSeconds(), 0.0);

		// (b) The no-operator filter faction is the vehicle faction (contract
		// section 8's fallback face).
		Mount->SetVehicleFaction(FactionVehicle);
		TestEqual(TEXT("the vehicle faction filters without an operator"),
			Mount->ResolveFilterFaction(), FactionVehicle);

		// (c) The frozen seat-state seam: the M5-040 model supplies presence,
		// a fixed faction supplies the operator faction; the fire-source
		// registration follows the operator faction so the existing M5-026
		// filter reads it.
		if (!Scene.SeatOperator(*this, 43))
		{
			return false;
		}
		Mount->SetOperatorSeatSource(&Scene.Seats, VehicleIdShipped, SeatDriverShipped);
		Mount->SetOperatorFaction(FactionPlayer);
		Mount->NotifyOperatorChanged();
		const FVehicleMountOperatorState Present = Mount->ResolveOperator();
		TestTrue(TEXT("the seat model resolves the operator"), Present.bPresent);
		TestTrue(TEXT("the operator identity is the seat occupant"), Present.OccupantId == Scene.DriverOccupant());
		TestEqual(TEXT("the operator faction is the injected one"), Present.Faction, FactionPlayer);
		TestEqual(TEXT("the operator faction outranks the vehicle faction while seated"),
			Mount->ResolveFilterFaction(), FactionPlayer);
		const FCombatEntityRecord* SourceRecord = Rig.Weapon->GetFireRegistry().FindEntity(Mount->GetVehicleFireSourceEntityId());
		TestNotNull(TEXT("the refreshed fire source record exists"), SourceRecord);
		if (SourceRecord != nullptr)
		{
			TestEqual(TEXT("the fire source now carries the operator faction"), SourceRecord->Metadata.Faction, FactionPlayer);
		}

		// (d) With the operator present the turret fires.
		const FVehicleMountFireOutcome MannedFire = Mount->TryFire(FVehicleMountAim());
		TestTrue(TEXT("the manned turret fired"), MannedFire.bFired);
		Mount->ReleaseMountShot();
		Scene.AdvanceClock(1.4);

		// (e) The operator leaves: presence drops, fire forbids again, the
		// filter faction falls back to the vehicle.
		Scene.Seats.LeaveSeat(Scene.DriverOccupant());
		Mount->NotifyOperatorChanged();
		TestFalse(TEXT("the operator is absent after leaving the seat"), Mount->ResolveOperator().bPresent);
		const FVehicleMountFireOutcome VacatedFire = Mount->TryFire(FVehicleMountAim());
		TestFalse(TEXT("the vacated turret refuses fire"), VacatedFire.bFired);
		TestEqual(TEXT("the vacated refusal is the operator gate"),
			static_cast<int32>(VacatedFire.Reject), static_cast<int32>(EVehicleMountFireReject::NoOperator));
		TestEqual(TEXT("the vehicle faction filters again after the operator left"),
			Mount->ResolveFilterFaction(), FactionVehicle);

		// (f) The direct-provider seam (wiring without a seat model):
		// presence and faction injected directly, fire restored.
		Mount->SetOperatorSeatSource(nullptr, NAME_None, NAME_None);
		Mount->SetOperatorPresenceProvider([]() { return true; });
		Mount->SetOperatorFaction(FactionPlayer);
		Mount->NotifyOperatorChanged();
		TestTrue(TEXT("the direct provider resolves the operator"), Mount->ResolveOperator().bPresent);
		const FVehicleMountFireOutcome OverrideFire = Mount->TryFire(FVehicleMountAim());
		TestTrue(TEXT("the override-operated turret fired"), OverrideFire.bFired);
		Mount->ReleaseMountShot();
		return true;
	}

	// ------------------------------------------------------------------
	// 5. Same weapon on handheld and turret: same context, damage, ballistics
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_043SameWeaponDeliversSameDamageAndBallisticsOnHandheldAndTurret,
		"UEMMO.Tasks.M5_043.SameWeaponDeliversSameDamageAndBallisticsOnHandheldAndTurret",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_043SameWeaponDeliversSameDamageAndBallisticsOnHandheldAndTurret::RunTest(const FString& Parameters)
	{
		UWorld* World = M5_043_AcquireWorld();
		FM5_043_WorldScope Scope{ World };
		FM5_043_Scene Scene;
		if (!Scene.Build(*this, *World) || !World)
		{
			return false;
		}
		FM5_043_TurretRig Rig;
		if (!Rig.Build(*this, *World, Scene, M5_043_SceneBase + FVector(0.0, 2000.0, 0.0)))
		{
			return false;
		}

		// This proof compares SAME-CONFIG damage and ballistics: re-configure
		// the mount onto the vehicle hitscan weapon (a local row whose
		// TurretWeaponId names it) and bind the matching instance, so both
		// faces fire the one definition. The hitscan binding also exercises
		// the mount's binding-applied face restoring the component's own
		// single-fire policy (the projectile delivery policy from the rig's
		// rocket bind is replaced).
		FVehicleMountDefinition HitscanMountRow = Rig.Mount->GetMountedDefinition();
		HitscanMountRow.MountId = FName(TEXT("mount_test_hitscan"));
		HitscanMountRow.TurretWeaponId = WeaponHitscanTest;
		const FVehicleMountConfigOutcome HitscanConfigured = Rig.Mount->Configure(HitscanMountRow, Rig.Weapon);
		if (!TestTrue(TEXT("the hitscan mount row re-configures"), HitscanConfigured.bConfigured))
		{
			return false;
		}
		if (!TestTrue(TEXT("the turret ammo_cell reserve registers"),
			Rig.Weapon->GetAmmoModel().RegisterAmmoType(AmmoCell, 120, 30)))
		{
			return false;
		}
		const FItemInstance HitscanTurretInstance = MakeInstance(0x43, ItemVehicleHitscan);
		if (!TestTrue(TEXT("the turret weapon binds the hitscan definition"),
			Rig.Weapon->ApplyEquippedWeapon(&HitscanTurretInstance).bWeaponBound))
		{
			return false;
		}
		UVehicleWeaponMountComponent* Mount = Rig.Mount;

		// The mirrored geometry: both faces trace from an origin 1000 cm in
		// front of a hull of the SAME shipped kind (HP 250 / defense 15) -
		// the turret from the shipped pivot (0, 40, 120) cm.
		const FVector HandheldOrigin = M5_043_SceneBase + FVector(0.0, 40.0, 120.0);
		ACombatVehicle* HandheldTarget = Scene.SpawnHull(*this, *World, M5_043_SceneBase + FVector(1000.0, 40.0, 120.0));
		if (HandheldTarget == nullptr)
		{
			return false;
		}
		const FVector TurretOrigin = Rig.Vehicle->GetActorLocation() + FVector(0.0, 40.0, 120.0);
		ACombatVehicle* TurretTarget = Scene.SpawnHull(*this, *World, Rig.Vehicle->GetActorLocation() + FVector(1000.0, 40.0, 120.0));
		if (TurretTarget == nullptr)
		{
			return false;
		}

		// The turret face's key space: the between-faces world rebuild keeps
		// the two fire-lineage spaces from reusing the same event keys inside
		// the one shared ledger (both spaces start at epoch 1 otherwise).
		(void)Rig.Weapon->GetFireRegistry().BeginNextWorldEpoch();

		// The operator drives the turret face's filter faction.
		if (!Scene.SeatOperator(*this, 43))
		{
			return false;
		}
		Mount->SetOperatorSeatSource(&Scene.Seats, VehicleIdShipped, SeatDriverShipped);
		Mount->SetOperatorFaction(FactionPlayer);
		Mount->NotifyOperatorChanged();

		// The targets register in their own face's fire registry (the
		// handheld face's shooter already registered in Build).
		const FEntityId HandheldTargetId = Scene.RegisterTarget(Scene.HandheldWeapon->GetFireRegistry(), *HandheldTarget, FactionEnemy);
		TestTrue(TEXT("the handheld target registered"), IsValidCombatEntityId(HandheldTargetId));
		const FEntityId TurretTargetId = Scene.RegisterTarget(Rig.Weapon->GetFireRegistry(), *TurretTarget, FactionEnemy);
		TestTrue(TEXT("the turret target registered"), IsValidCombatEntityId(TurretTargetId));

		// FACE 1 - the handheld component fires the SAME hitscan definition.
		FFireIntent HandheldIntent;
		HandheldIntent.WeaponInstanceId = Scene.HandheldInstance.InstanceId;
		HandheldIntent.LocalShotSequence = 1;
		const FFireOutcome HandheldFire = Scene.HandheldWeapon->TryFire(HandheldIntent);
		TestTrue(TEXT("the handheld weapon fired"), HandheldFire.bFired);
		const FDamageProfile* HandheldProfile = Scene.Catalog.FindDamageProfile(HandheldFire.Shot.DamageProfileId);
		TestNotNull(TEXT("the handheld damage profile resolved"), HandheldProfile);
		if (!HandheldFire.bFired || HandheldProfile == nullptr)
		{
			return false;
		}
		FHitscanRequest HandheldDelivery;
		HandheldDelivery.Shot = HandheldFire.Shot;
		HandheldDelivery.Origin = HandheldOrigin;
		FShotPellet HandheldPellet;
		HandheldPellet.PelletIndex = 0;
		HandheldPellet.Direction = FVector(1.0, 0.0, 0.0);
		HandheldDelivery.Pellets.Add(HandheldPellet);
		HandheldDelivery.AttackProfile = *HandheldProfile;
		Scene.Ledger.BindRegistry(&Scene.HandheldWeapon->GetFireRegistry());
		const FHitscanExecutionOutcome HandheldOutcome = ExecuteHitscan(World, HandheldDelivery,
			&Scene.HandheldWeapon->GetFireRegistry(), &Scene.Ledger, &Scene.Identity);
		Scene.Ledger.EndShot(HandheldFire.Shot.Epoch, HandheldFire.Shot.SourceEntityId, HandheldFire.Shot.ShotId);

		TestTrue(TEXT("the handheld delivery executed"), HandheldOutcome.bExecuted);
		TestEqual(TEXT("the handheld face applied one hit"), HandheldOutcome.HitsApplied, 1);
		TestEqual(TEXT("the handheld round removed round(700/115)=6 from the hull"),
			HandheldTarget->GetHealthComponent()->GetHealth(), 244.0f);

		// FACE 2 - the turret commits through the SAME transaction chain (the
		// mount forwards into the vehicle's weapon component).
		const FVehicleMountFireOutcome TurretFire = Mount->TryFire(FVehicleMountAim());
		TestTrue(TEXT("the turret fired the same-config weapon"), TurretFire.bFired);
		Mount->ReleaseMountShot();
		// Both faces fire the hitscan vehicle weapon: the shot carries its
		// damage profile id directly (the projectile-row profile detour only
		// applies to projectile-mode turrets).
		const FDamageProfile* TurretProfile = Scene.Catalog.FindDamageProfile(TurretFire.Shot.DamageProfileId);
		TestNotNull(TEXT("the turret damage profile resolved"), TurretProfile);
		if (!TurretFire.bFired || TurretProfile == nullptr)
		{
			return false;
		}

		// The committed contexts agree on every definition-level field (the
		// instance ids differ by design: two separate weapon instances).
		TestEqual(TEXT("both faces fired the same weapon definition"),
			TurretFire.Shot.WeaponDefinitionId, HandheldFire.Shot.WeaponDefinitionId);
		TestEqual(TEXT("both faces used the same delivery mode"),
			static_cast<int32>(TurretFire.Shot.FireMode), static_cast<int32>(HandheldFire.Shot.FireMode));
		TestEqual(TEXT("both faces applied the same damage profile"),
			TurretFire.Shot.DamageProfileId, HandheldFire.Shot.DamageProfileId);
		TestEqual(TEXT("both faces consumed the same ammo kind"), TurretFire.Shot.AmmoId, HandheldFire.Shot.AmmoId);
		TestEqual(TEXT("both faces planned the same pellet count"), TurretFire.Shot.PelletCount, HandheldFire.Shot.PelletCount);
		TestEqual(TEXT("both faces resolved the same projectile id"), TurretFire.Shot.ProjectileId, HandheldFire.Shot.ProjectileId);
		TestEqual(TEXT("both faces carried the same range"), TurretFire.Shot.RangeCm, HandheldFire.Shot.RangeCm);
		TestEqual(TEXT("both faces carried the same spread"), TurretFire.Shot.SpreadDegrees, HandheldFire.Shot.SpreadDegrees);

		// The turret's delivery: the executor reads the filter faction from
		// the fire registry - the OPERATOR faction rides the fire-source
		// registration (the vehicle faction was never set here), so an
		// enemy-faction hull takes the hit like the handheld face's did.
		FHitscanRequest TurretDelivery;
		TurretDelivery.Shot = TurretFire.Shot;
		TurretDelivery.Origin = TurretOrigin;
		FShotPellet TurretPellet;
		TurretPellet.PelletIndex = 0;
		TurretPellet.Direction = FVector(1.0, 0.0, 0.0);
		TurretDelivery.Pellets.Add(TurretPellet);
		TurretDelivery.AttackProfile = *TurretProfile;
		Scene.Ledger.BindRegistry(&Rig.Weapon->GetFireRegistry());
		const FHitscanExecutionOutcome TurretOutcome = ExecuteHitscan(World, TurretDelivery,
			&Rig.Weapon->GetFireRegistry(), &Scene.Ledger, &Scene.Identity);
		Scene.Ledger.EndShot(TurretFire.Shot.Epoch, TurretFire.Shot.SourceEntityId, TurretFire.Shot.ShotId);

		TestTrue(TEXT("the turret delivery executed"), TurretOutcome.bExecuted);
		TestEqual(TEXT("the turret face applied one hit"), TurretOutcome.HitsApplied, 1);
		TestEqual(TEXT("the turret round removed the SAME round(700/115)=6 from the hull"),
			TurretTarget->GetHealthComponent()->GetHealth(), 244.0f);

		// Identical ballistics: the same straight pellet over the same range
		// reaches the same impact offset on both faces.
		if (HandheldOutcome.Hits.Num() == 1 && TurretOutcome.Hits.Num() == 1)
		{
			const FVector HandheldOffset = HandheldOutcome.Hits[0].HitLocation - HandheldOrigin;
			const FVector TurretOffset = TurretOutcome.Hits[0].HitLocation - TurretOrigin;
			TestTrue(TEXT("both faces delivered identical impact offsets (same ballistics)"),
				HandheldOffset.Equals(TurretOffset, 0.01));
		}

		// The one shared ledger collected both faces through the unified entry
		// (the damage assertions above are the proof). M5-077 semantics: each
		// EndShot RELEASES the shot's event keys — so zero recorded events
		// with two tombstoned shots is the healthy post-cleanup state, not a
		// missed recording.
		TestEqual(TEXT("the shared ledger recorded no live events after both EndShots"),
			Scene.Ledger.GetNumRecordedEvents(), 0);
		TestEqual(TEXT("the shared ledger keeps both shots as tombstones"),
			Scene.Ledger.GetNumTrackedShots(), 2);
		TestEqual(TEXT("the shared ledger holds no active shots"),
			Scene.Ledger.GetNumActiveShots(), 0);

		// The friendly-fire face: an operator-faction hull is refused by the
		// EXISTING M5-026 filter (friendlies block) - proof the operator
		// faction, not a vehicle-local rule, governs the turret's hits.
		ACombatVehicle* FriendlyTarget = Scene.SpawnHull(*this, *World, Rig.Vehicle->GetActorLocation() + FVector(0.0, 1100.0, 120.0));
		if (FriendlyTarget == nullptr)
		{
			return false;
		}
		const FEntityId FriendlyTargetId = Scene.RegisterTarget(Rig.Weapon->GetFireRegistry(), *FriendlyTarget, FactionPlayer);
		TestTrue(TEXT("the friendly target registered"), IsValidCombatEntityId(FriendlyTargetId));
		Scene.AdvanceClock(1.4); // past the turret's 45 rpm pacing cooldown
		const FVehicleMountFireOutcome FriendlyFire = Mount->TryFire(FVehicleMountAim{0.0f, 0.0f});
		TestTrue(TEXT("the friendly-direction shot committed (filtering happens at delivery)"), FriendlyFire.bFired);
		Mount->ReleaseMountShot();
		FHitscanRequest FriendlyDelivery;
		FriendlyDelivery.Shot = FriendlyFire.Shot;
		FriendlyDelivery.Origin = TurretOrigin;
		FShotPellet FriendlyPellet;
		FriendlyPellet.PelletIndex = 0;
		FriendlyPellet.Direction = FVector(0.0, 1.0, 0.0);
		FriendlyDelivery.Pellets.Add(FriendlyPellet);
		FriendlyDelivery.AttackProfile = *TurretProfile;
		const FHitscanExecutionOutcome FriendlyOutcome = ExecuteHitscan(World, FriendlyDelivery,
			&Rig.Weapon->GetFireRegistry(), &Scene.Ledger, &Scene.Identity);
		Scene.Ledger.EndShot(FriendlyFire.Shot.Epoch, FriendlyFire.Shot.SourceEntityId, FriendlyFire.Shot.ShotId);
		TestTrue(TEXT("the friendly-direction delivery executed"), FriendlyOutcome.bExecuted);
		TestEqual(TEXT("the friendly hull refused the hit"), FriendlyOutcome.HitsApplied, 0);
		TestEqual(TEXT("the friendly hull blocked the pellet (default friendly filter)"), FriendlyOutcome.PelletsBlocked, 1);
		TestEqual(TEXT("the friendly hull took no damage"), FriendlyTarget->GetHealthComponent()->GetHealth(), 250.0f);
		return true;
	}

	// ------------------------------------------------------------------
	// 6. Shot attribution (vehicle source, operator separate) and old epochs
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_043ShotAttributionVehicleSourceAndOldEpochRefusal,
		"UEMMO.Tasks.M5_043.ShotAttributionVehicleSourceAndOldEpochRefusal",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_043ShotAttributionVehicleSourceAndOldEpochRefusal::RunTest(const FString& Parameters)
	{
		UWorld* World = M5_043_AcquireWorld();
		FM5_043_WorldScope Scope{ World };
		FM5_043_Scene Scene;
		if (!Scene.Build(*this, *World) || !World)
		{
			return false;
		}
		FM5_043_TurretRig Rig;
		if (!Rig.Build(*this, *World, Scene, M5_043_SceneBase + FVector(0.0, 2000.0, 0.0)))
		{
			return false;
		}
		UVehicleWeaponMountComponent* Mount = Rig.Mount;
		if (!Scene.SeatOperator(*this, 43))
		{
			return false;
		}
		Mount->SetOperatorSeatSource(&Scene.Seats, VehicleIdShipped, SeatDriverShipped);
		Mount->SetOperatorFaction(FactionPlayer);
		Mount->NotifyOperatorChanged();

		// Attribution: the committed context's source is the VEHICLE fire
		// source (the firing body); the operator is recorded separately on
		// the mount (contract section 8 "当前操作者与发射车辆分别在ShotContext
		// 记录").
		const FEntityId SourceId = Mount->GetVehicleFireSourceEntityId();
		TestTrue(TEXT("the vehicle fire source is valid"), IsValidCombatEntityId(SourceId));
		TestTrue(TEXT("the fire source resolves to the vehicle actor"),
			Rig.Weapon->GetFireRegistry().ResolveEntity(SourceId).Get() == static_cast<AActor*>(Rig.Vehicle));
		const FVehicleMountFireOutcome First = Mount->TryFire(FVehicleMountAim());
		TestTrue(TEXT("the manned turret fired"), First.bFired);
		Mount->ReleaseMountShot();
		TestEqual(TEXT("the committed shot attributes the vehicle as its source"),
			First.Shot.SourceEntityId, SourceId);
		TestEqual(TEXT("the committed shot used the weapon component's own fire source"),
			First.Shot.SourceEntityId, Rig.Weapon->GetFireSourceEntityId());
		const FVehicleMountOperatorState Operator = Mount->ResolveOperator();
		TestTrue(TEXT("the operator is present"), Operator.bPresent);
		TestTrue(TEXT("the operator is the seat occupant, recorded separately from the shot source"),
			Operator.OccupantId == Scene.DriverOccupant() && Operator.OccupantId.IsValid());
		const FCombatEntityRecord* SourceRecord = Rig.Weapon->GetFireRegistry().FindEntity(SourceId);
		TestNotNull(TEXT("the source record exists"), SourceRecord);
		if (SourceRecord != nullptr)
		{
			TestEqual(TEXT("the source record carries the operator faction (the filter face)"),
				SourceRecord->Metadata.Faction, FactionPlayer);
		}

		// Old epoch: the world rebuild (leave room / retry) wipes the
		// fire-lineage records; the mount refuses with the named stale reason
		// and ZERO side effects (the existing registry semantics, surfaced).
		const FCombatEpoch OldEpoch = Rig.Weapon->GetFireEpoch();
		const int32 ShotsBefore = Mount->GetMountedShotCount();
		const int32 RoundsBefore = Rig.Weapon->GetActiveBinding()->LoadedRounds;
		const double CooldownBefore = Rig.Weapon->GetNextFireTimeSeconds();
		const FCombatEpoch NewEpoch = Rig.Weapon->GetFireRegistry().BeginNextWorldEpoch();
		TestEqual(TEXT("the rebuild advanced the generation"), NewEpoch, OldEpoch + 1);
		const FVehicleMountFireOutcome StaleFire = Mount->TryFire(FVehicleMountAim());
		TestFalse(TEXT("the stale-epoch fire refused"), StaleFire.bFired);
		TestEqual(TEXT("the stale-epoch refusal is named"),
			static_cast<int32>(StaleFire.Reject), static_cast<int32>(EVehicleMountFireReject::FireSourceStale));
		TestEqual(TEXT("the stale refusal deducted nothing"), Rig.Weapon->GetActiveBinding()->LoadedRounds, RoundsBefore);
		TestEqual(TEXT("the stale refusal minted no shot"), Mount->GetMountedShotCount(), ShotsBefore);
		TestEqual(TEXT("the stale refusal left the cooldown untouched"), Rig.Weapon->GetNextFireTimeSeconds(), CooldownBefore);

		// Re-registration (the retry/leave-room face): the living vehicle
		// re-enters the new generation and keeps firing under it.
		TestTrue(TEXT("the fire source re-registered in the new generation"), Mount->RefreshVehicleFireSource());
		const FEntityId RenewedSourceId = Mount->GetVehicleFireSourceEntityId();
		TestTrue(TEXT("the renewed source is a live record of the new generation"),
			Rig.Weapon->GetFireRegistry().IsKnownEntityInEpoch(RenewedSourceId, NewEpoch));
		Scene.AdvanceClock(1.4); // past the first shot's pacing cooldown
		const FVehicleMountFireOutcome RenewedFire = Mount->TryFire(FVehicleMountAim());
		TestTrue(TEXT("the renewed turret fired under the new generation"), RenewedFire.bFired);
		Mount->ReleaseMountShot();
		TestEqual(TEXT("the renewed shot carries the new epoch"), RenewedFire.Shot.Epoch, NewEpoch);
		TestEqual(TEXT("the renewed shot attributes the renewed vehicle source"),
			RenewedFire.Shot.SourceEntityId, RenewedSourceId);
		return true;
	}

	// ------------------------------------------------------------------
	// 7. Cleanup: Finalize/EndPlay cancel fire and reload, drop references
	// ------------------------------------------------------------------
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_043FinalizeCancelsFireReloadAndDropsReferences,
		"UEMMO.Tasks.M5_043.FinalizeCancelsFireReloadAndDropsReferences",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_043FinalizeCancelsFireReloadAndDropsReferences::RunTest(const FString& Parameters)
	{
		UWorld* World = M5_043_AcquireWorld();
		FM5_043_WorldScope Scope{ World };
		FM5_043_Scene Scene;
		if (!Scene.Build(*this, *World) || !World)
		{
			return false;
		}
		FM5_043_TurretRig Rig;
		if (!Rig.Build(*this, *World, Scene, M5_043_SceneBase + FVector(0.0, 2000.0, 0.0)))
		{
			return false;
		}
		UVehicleWeaponMountComponent* Mount = Rig.Mount;
		if (!Scene.SeatOperator(*this, 43))
		{
			return false;
		}
		Mount->SetOperatorSeatSource(&Scene.Seats, VehicleIdShipped, SeatDriverShipped);
		Mount->SetOperatorFaction(FactionPlayer);
		Mount->NotifyOperatorChanged();

		// Two committed shots (one released) plus an open reload window: the
		// full live state the cleanup must cancel.
		const FVehicleMountFireOutcome First = Mount->TryFire(FVehicleMountAim());
		TestTrue(TEXT("the first shot committed"), First.bFired);
		if (!First.bFired) { AddError(FString::Printf(TEXT("first shot reject %d: %s"), static_cast<int32>(First.Reject), *First.RejectDetail)); }
		Mount->ReleaseMountShot();
		Scene.AdvanceClock(1.4);
		const FVehicleMountFireOutcome Second = Mount->TryFire(FVehicleMountAim());
		TestTrue(TEXT("the second shot committed"), Second.bFired);
		TestTrue(TEXT("the second shot is in flight (no release yet)"), Rig.Weapon->IsShotInFlight());
		Scene.AdvanceClock(1.4);
		const FAmmoReloadOutcome Reload = Rig.Weapon->BeginReload(Scene.Now, 5.0);
		TestTrue(TEXT("the reload window opened"), Reload.bSuccess);
		TestTrue(TEXT("the reload window is pending"), Rig.Weapon->GetRemainingReloadSeconds(Scene.Now) > 0.0);

		const uint64 GenerationBefore = Rig.Weapon->GetBindingGeneration();
		const int32 RoundsBefore = Rig.Weapon->GetActiveBinding()->LoadedRounds;

		// Finalize: the vehicle-destroy cleanup through the weapon
		// component's own cancel faces.
		TestTrue(TEXT("the first Finalize performed the cleanup"), Mount->Finalize());
		TestTrue(TEXT("the mount reports the terminal state"), Mount->IsFinalized());
		TestFalse(TEXT("the in-flight shot was canceled"), Rig.Weapon->IsShotInFlight());
		TestEqual(TEXT("the open reload window was canceled"), Rig.Weapon->GetRemainingReloadSeconds(Scene.Now), 0.0);
		TestEqual(TEXT("the binding generation bumped (stale callbacks drop)"),
			Rig.Weapon->GetBindingGeneration(), GenerationBefore + 1);
		TestNotNull(TEXT("the binding identity survives (the revive re-entry semantics)"), Rig.Weapon->GetActiveBinding());
		if (Rig.Weapon->GetActiveBinding() != nullptr)
		{
			TestEqual(TEXT("the fire book rounds survive the death teardown"),
				Rig.Weapon->GetActiveBinding()->LoadedRounds, RoundsBefore);
		}

		// The terminal state: every fire face refuses; the second Finalize is
		// the rejected no-op.
		const FVehicleMountFireCheck TerminalCheck = Mount->CanFire(FVehicleMountAim());
		TestFalse(TEXT("the finalized mount cannot fire"), TerminalCheck.bCanFire);
		TestEqual(TEXT("the terminal refusal is named"),
			static_cast<int32>(TerminalCheck.Reject), static_cast<int32>(EVehicleMountFireReject::MountFinalized));
		const FVehicleMountFireOutcome TerminalFire = Mount->TryFire(FVehicleMountAim());
		TestFalse(TEXT("the finalized mount refuses fire"), TerminalFire.bFired);
		TestFalse(TEXT("the second Finalize is the rejected no-op"), Mount->Finalize());

		// The weak-reference drop: nothing injected survives the cleanup -
		// the seat model pointer and every provider are gone, and even the
		// still-occupied seat no longer reaches the mount.
		TestTrue(TEXT("the turret weapon reference was dropped"), Mount->GetTurretWeapon() == nullptr);
		TestFalse(TEXT("the dropped seat source resolves no operator"), Mount->ResolveOperator().bPresent);
		Scene.Seats.LeaveSeat(Scene.DriverOccupant());

		// The EndPlay route: a fresh rig's vehicle destroy funnels through
		// EndPlay into the same cleanup face (the full world shape routes it).
		FM5_043_TurretRig DestroyedRig;
		if (!DestroyedRig.Build(*this, *World, Scene, M5_043_SceneBase + FVector(0.0, 6000.0, 0.0)))
		{
			return false;
		}
		DestroyedRig.Vehicle->Destroy();
		TestTrue(TEXT("the actor destroy finalized its mount through EndPlay"), DestroyedRig.Mount->IsFinalized());
		TestTrue(TEXT("the destroyed rig dropped its weapon reference too"),
			DestroyedRig.Mount->GetTurretWeapon() == nullptr);
		return true;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
