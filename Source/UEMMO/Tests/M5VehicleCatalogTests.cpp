// M5-038: the vehicle segment joins the read-only combat configuration
// catalog. Pins the Combat/Data/CombatCatalog.h extension:
//
// - The shipped Data/CombatSystem directory (manifest-gated ten tables)
//   builds a catalog whose FindVehicle / FindVehicleSeat / FindVehicleMount
//   accessors mirror every source field, and the mounted turret's weapon id
//   resolves into the same catalog's weapons/ammo tables (the related
//   Weapon definition the card verification names).
// - The vehicle tables follow the exact six-table failure semantics: an
//   empty id, a duplicate id and a non-positive schema version refuse the
//   whole build with an error naming the table (and id); a refused build
//   leaves the target catalog completely untouched; unknown ids return
//   nullptr - there is no fallback vehicle and no temporary stand-in row.
// - The revision mirror: the digest covers the three vehicle tables in fixed
//   order after the projectiles, row-order independently (reordering rows or
//   double-building from freshly parsed tables keeps the revision), while
//   any field value change, added/removed row or schema version change
//   alters it. A vehicle-less config builds a different revision from the
//   same six A/B rows, so the segment is provably inside the digest.
// - Missing resources/refs refuse the directory before anything is built
//   (the M5-037 manifest/validation semantics): a required vehicle table
//   file that is missing, a vehicle row without a mesh path, a mount whose
//   turret weapon does not resolve, a seat on an unknown vehicle and a
//   second seat for one vehicle each refuse with named fields - no fake
//   vehicle row is ever published.
// - The generated vehicle assets (Scripts/Editor/create_combat_system_assets.py,
//   /Game/UEMMO/CombatSystem/Vehicles) reload from disk in a fresh process
//   (RF_WasLoaded), mirror the shipped source rows field by field, share one
//   ConfigRevision equal to the vehicle-less six-table revision the builder
//   stamps on this run, and the related weapon definition asset of the
//   mounted turret loads from the same generation.
//
// Process honesty: the reload suite needs the generated assets on disk; run
// the generation script before this suite (the card flow: generate, save,
// close the editor, reload in a new process). In a packaged build the dev
// content tree is absent and the suite skips by name.

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/Data/CombatCatalog.h"
#include "../Combat/Data/CombatDataTableParser.h"
#include "../Combat/Data/CombatDataAssetBuilder.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_038
{
	// ------------------------------------------------------------------
	// In-memory fixture rows (value mirrors of the shipped samples)
	// ------------------------------------------------------------------

	static FVehicleDefinition MakeSampleVehicle(FName Id = FName(TEXT("vehicle_wheeled_sample")))
	{
		FVehicleDefinition Vehicle;
		Vehicle.VehicleId = Id;
		Vehicle.DriveType = EVehicleDriveType::Ground;
		Vehicle.MaxHealth = 250.0f;
		Vehicle.Defense = 15.0f;
		Vehicle.MaxSpeedCmS = 700.0f;
		Vehicle.AccelerationCmS2 = 900.0f;
		Vehicle.MeshPath = TEXT("/Game/UEMMO/Vehicles/SM_Vehicle_Wheeled_Sample.SM_Vehicle_Wheeled_Sample");
		Vehicle.CameraDistanceCm = 650.0f;
		Vehicle.CameraHeightCm = 260.0f;
		Vehicle.CameraPitchDeg = -18.0f;
		return Vehicle;
	}

	static FVehicleSeatDefinition MakeSampleSeat(FName Id = FName(TEXT("seat_wheeled_driver")),
		FName VehicleId = FName(TEXT("vehicle_wheeled_sample")))
	{
		FVehicleSeatDefinition Seat;
		Seat.SeatId = Id;
		Seat.VehicleId = VehicleId;
		Seat.SeatRole = EVehicleSeatRole::Driver;
		Seat.SeatLocationCm = FVector(0.0, -40.0, 45.0);
		Seat.ExitOffsetCm = FVector(-180.0, 0.0, 0.0);
		return Seat;
	}

	static FVehicleMountDefinition MakeSampleMount(FName Id = FName(TEXT("mount_wheeled_turret")),
		FName VehicleId = FName(TEXT("vehicle_wheeled_sample")),
		FName WeaponId = FName(TEXT("weapon_explosive_sample")))
	{
		FVehicleMountDefinition Mount;
		Mount.MountId = Id;
		Mount.VehicleId = VehicleId;
		Mount.TurretWeaponId = WeaponId;
		Mount.LocationCm = FVector(0.0, 40.0, 120.0);
		Mount.MinPitchDegrees = -20.0f;
		Mount.MaxPitchDegrees = 60.0f;
		return Mount;
	}

	/** A minimal but legal six-table candidate set (one profile + one weapon). */
	static FParsedCombatConfig MakeBaseConfig()
	{
		FParsedCombatConfig Config;
		Config.SchemaVersion = 1;

		FDamageProfile Profile;
		Profile.DamageProfileId = TEXT("light_01");
		Profile.BaseDamage = 10.0f;
		Profile.AttackCoefficient = 1.0f;
		Profile.HitStunSeconds = 0.22f;
		Profile.KnockbackCmPerSecond = 90.0f;
		Profile.LaunchCmPerSecond = 0.0f;
		Profile.HitStopSeconds = 0.04f;
		Config.DamageProfiles.Add(Profile);

		FWeaponDefinition Weapon;
		Weapon.WeaponId = TEXT("weapon_test_sword");
		Weapon.FireMode = EWeaponFireMode::Melee;
		Weapon.DamageProfileId = TEXT("light_01");
		Weapon.BurstCount = 1;
		Weapon.PelletCount = 1;
		Config.Weapons.Add(Weapon);
		return Config;
	}

	/** The base config plus the sample vehicle segment. */
	static FParsedCombatConfig MakeVehicleConfig()
	{
		FParsedCombatConfig Config = MakeBaseConfig();
		Config.Vehicles.Add(MakeSampleVehicle());
		Config.VehicleSeats.Add(MakeSampleSeat());
		Config.VehicleMounts.Add(MakeSampleMount());
		return Config;
	}

	/** The deterministic revision of one config; empty means a refused build. */
	static FString RevisionOf(const FParsedCombatConfig& Config, FString* OutErrors = nullptr)
	{
		FCombatCatalog Catalog;
		FString Errors;
		const FString Revision = FCombatCatalog::BuildFromParsed(Config, Catalog, Errors)
			? Catalog.GetConfigRevision()
			: FString();
		if (OutErrors)
		{
			*OutErrors = Errors;
		}
		return Revision;
	}

	// ------------------------------------------------------------------
	// Shipped directory helpers
	// ------------------------------------------------------------------

	static bool LoadShippedTables(FAutomationTestBase& Test, FCombatDataTableSet& OutTables)
	{
		const FString DataDir = FPaths::ProjectDir() / TEXT("Data") / TEXT("CombatSystem");
		TArray<FString> Problems;
		if (!LoadCombatDataDirectory(DataDir, OutTables, Problems))
		{
			Test.AddError(FString::Printf(TEXT("the shipped combat data directory failed to load: %s"),
				*FString::Join(Problems, TEXT(" | "))));
			return false;
		}
		return true;
	}

	/**
	 * Assembles the candidate config the way the production readers do
	 * (field-by-field copy). bIncludeVehicles=false reproduces the pre-038
	 * six-table shape of the asset builder, which stamps the six-table
	 * revision the vehicle assets carry.
	 */
	static FParsedCombatConfig ToParsedConfig(const FCombatDataTableSet& Tables, bool bIncludeVehicles)
	{
		FParsedCombatConfig Config;
		Config.SchemaVersion = 1;
		for (const TPair<FName, FDamageProfile>& Row : Tables.DamageProfiles)
		{
			Config.DamageProfiles.Add(Row.Value);
		}
		for (const TPair<FName, FAttackReaction>& Row : Tables.AttackReactions)
		{
			Config.AttackReactions.Add(Row.Value);
		}
		for (const TPair<FName, FTargetReaction>& Row : Tables.TargetReactions)
		{
			Config.TargetReactions.Add(Row.Value);
		}
		for (const TPair<FName, FWeaponDefinition>& Row : Tables.Weapons)
		{
			Config.Weapons.Add(Row.Value);
		}
		for (const TPair<FName, FAmmoType>& Row : Tables.AmmoTypes)
		{
			Config.AmmoTypes.Add(Row.Value);
		}
		for (const TPair<FName, FProjectileDefinition>& Row : Tables.Projectiles)
		{
			Config.Projectiles.Add(Row.Value);
		}
		if (bIncludeVehicles)
		{
			for (const TPair<FName, FVehicleDefinition>& Row : Tables.Vehicles)
			{
				Config.Vehicles.Add(Row.Value);
			}
			for (const TPair<FName, FVehicleSeatDefinition>& Row : Tables.VehicleSeats)
			{
				Config.VehicleSeats.Add(Row.Value);
			}
			for (const TPair<FName, FVehicleMountDefinition>& Row : Tables.VehicleMounts)
			{
				Config.VehicleMounts.Add(Row.Value);
			}
		}
		return Config;
	}

	// ------------------------------------------------------------------
	// Generated vehicle asset paths (the editor script's naming contract)
	// ------------------------------------------------------------------

	static const TCHAR* const VehicleAssetDir = TEXT("/Game/UEMMO/CombatSystem/Vehicles");

	static FString VehicleAssetName(const TCHAR* Prefix, const FName Id)
	{
		return Prefix + Id.ToString();
	}

	static FString VehicleAssetDiskPath(const FString& AssetName)
	{
		return FPaths::ProjectContentDir() / TEXT("UEMMO/CombatSystem/Vehicles") / (AssetName + TEXT(".uasset"));
	}

	/**
	 * Reloads one generated asset by object path and asserts it was
	 * deserialized from the saved package (the M5-009 transient-illusion
	 * guard): an in-memory transient of a generating process never carries
	 * RF_WasLoaded.
	 */
	template <typename AssetClass>
	static AssetClass* ReloadGeneratedAsset(FAutomationTestBase& Test, const FString& AssetName)
	{
		const FString ObjectPath = FString(VehicleAssetDir) / AssetName + TEXT(".") + AssetName;
		AssetClass* Asset = LoadObject<AssetClass>(nullptr, *ObjectPath);
		if (Test.TestNotNull(*FString::Printf(TEXT("the generated asset '%s' reloads"), *AssetName), Asset))
		{
			Test.TestTrue(FString::Printf(TEXT("the asset '%s' was loaded from disk (RF_WasLoaded)"), *AssetName),
				Asset->HasAnyFlags(RF_WasLoaded));
			Test.TestFalse(FString::Printf(TEXT("the asset '%s' has no _1 duplicate package on disk"), *AssetName),
				IFileManager::Get().FileExists(*VehicleAssetDiskPath(AssetName + TEXT("_1"))));
			Test.TestFalse(FString::Printf(TEXT("the asset '%s' has no _2 duplicate package on disk"), *AssetName),
				IFileManager::Get().FileExists(*VehicleAssetDiskPath(AssetName + TEXT("_2"))));
		}
		return Asset;
	}

	// ------------------------------------------------------------------
	// Temporary directory helpers for the refusal cases (the M5_037 pattern)
	// ------------------------------------------------------------------

	static FString BuildTempDataDir(const TCHAR* CaseName)
	{
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("Temp") / TEXT("M5_038") / CaseName;
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		PlatformFile.DeleteDirectoryRecursively(*Dir);
		PlatformFile.CreateDirectoryTree(*Dir);
		return Dir;
	}

	static void RemoveTempDataDir(const FString& Dir)
	{
		FPlatformFileManager::Get().GetPlatformFile().DeleteDirectoryRecursively(*Dir);
	}

	static bool WriteTableFile(const FString& Dir, const TCHAR* FileName, const FString& Content)
	{
		return FFileHelper::SaveStringToFile(Content, *(Dir / FileName), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	static bool WriteManifestWithVehicles(const FString& Dir)
	{
		return WriteTableFile(Dir, TEXT("manifest.json"), TEXT(
			"{\"schema_version\": 1, \"required_tables\": [\"damage_profiles\", \"attack_reactions\", "
			"\"target_reactions\", \"presentations\", \"weapons\", \"ammo_types\", \"projectiles\", "
			"\"vehicles\", \"vehicle_seats\", \"vehicle_mounts\"]}"));
	}

	static bool WriteVehicleTables(const FString& Dir, const TCHAR* VehicleRows, const TCHAR* SeatRows, const TCHAR* MountRows)
	{
		bool bOk = WriteTableFile(Dir, TEXT("vehicles.json"), FString::Printf(TEXT(
			"{\n  \"schema_version\": 1,\n  \"table\": \"vehicles\",\n  \"rows\": [\n    %s\n  ]\n}\n"), VehicleRows));
		bOk = bOk && WriteTableFile(Dir, TEXT("vehicle_seats.json"), FString::Printf(TEXT(
			"{\n  \"schema_version\": 1,\n  \"table\": \"vehicle_seats\",\n  \"rows\": [\n    %s\n  ]\n}\n"), SeatRows));
		bOk = bOk && WriteTableFile(Dir, TEXT("vehicle_mounts.json"), FString::Printf(TEXT(
			"{\n  \"schema_version\": 1,\n  \"table\": \"vehicle_mounts\",\n  \"rows\": [\n    %s\n  ]\n}\n"), MountRows));
		return bOk;
	}

	/** A minimal but reference-consistent A-segment table set (the 037 fixture). */
	static bool WriteAbTableSet(const FString& Dir)
	{
		bool bOk = WriteTableFile(Dir, TEXT("damage_profiles.json"), TEXT(
			"{\n"
			"  \"schema_version\": 1,\n"
			"  \"table\": \"damage_profiles\",\n"
			"  \"rows\": [\n"
			"    {\"damage_profile_id\": \"light_01\", \"base_damage\": 10, \"attack_coefficient\": 1.0, \"hit_stun_s\": 0.22, \"knockback_cm_s\": 90, \"launch_cm_s\": 0, \"hit_stop_s\": 0.04}\n"
			"  ]\n"
			"}\n"));
		bOk = bOk && WriteTableFile(Dir, TEXT("attack_reactions.json"), TEXT(
			"{\n"
			"  \"schema_version\": 1,\n"
			"  \"table\": \"attack_reactions\",\n"
			"  \"rows\": [\n"
			"    {\"reaction_id\": \"light_01\", \"control_penetration\": \"none\"}\n"
			"  ]\n"
			"}\n"));
		bOk = bOk && WriteTableFile(Dir, TEXT("target_reactions.json"), TEXT(
			"{\n"
			"  \"schema_version\": 1,\n"
			"  \"table\": \"target_reactions\",\n"
			"  \"rows\": [\n"
			"    {\"policy_id\": \"normal\", \"allow_stagger\": true, \"allow_launch\": true, \"allow_knockdown\": true, \"max_launches_per_air_cycle\": 2, \"launch_z_scales\": [1.0, 0.7], \"max_air_time_s\": 2.5, \"poise_max\": 0, \"poise_regen_s\": 0, \"knockdown_s\": 0.45, \"recovering_s\": 0.25, \"immune_damage\": false, \"immune_control\": false, \"death_resistant\": false}\n"
			"  ]\n"
			"}\n"));
		bOk = bOk && WriteTableFile(Dir, TEXT("presentations.json"), TEXT(
			"{\n"
			"  \"schema_version\": 1,\n"
			"  \"table\": \"presentations\",\n"
			"  \"rows\": [\n"
			"    {\"presentation_id\": \"hit_light_01\", \"reaction_id\": \"light_01\", \"montage_path\": \"\", \"sound_path\": \"\", \"effect_path\": \"\", \"placeholder\": true}\n"
			"  ]\n"
			"}\n"));
		bOk = bOk && WriteTableFile(Dir, TEXT("weapons.json"), TEXT(
			"{\n"
			"  \"schema_version\": 1,\n"
			"  \"table\": \"weapons\",\n"
			"  \"rows\": [\n"
			"    {\"weapon_id\": \"weapon_explosive_sample\", \"mode\": \"projectile\", \"damage_profile_id\": \"\", \"ammo_id\": \"ammo_rocket\", \"magazine_size\": 3, \"fire_rate_rpm\": 45, \"burst_count\": 1, \"pellet_count\": 1, \"spread_degrees_deg\": 0, \"range_cm\": 0, \"projectile_id\": \"rocket_explosive\", \"melee_attack_ids\": []}\n"
			"  ]\n"
			"}\n"));
		bOk = bOk && WriteTableFile(Dir, TEXT("ammo_types.json"), TEXT(
			"{\n"
			"  \"schema_version\": 1,\n"
			"  \"table\": \"ammo_types\",\n"
			"  \"rows\": [\n"
			"    {\"ammo_id\": \"ammo_rocket\", \"max_reserve\": 12, \"magazine_size\": 3}\n"
			"  ]\n"
			"}\n"));
		bOk = bOk && WriteTableFile(Dir, TEXT("projectiles.json"), TEXT(
			"{\n"
			"  \"schema_version\": 1,\n"
			"  \"table\": \"projectiles\",\n"
			"  \"rows\": [\n"
			"    {\"projectile_id\": \"rocket_explosive\", \"motion\": \"straight\", \"speed_cm_s\": 6000, \"lifetime_s\": 2.0, \"damage_profile_id\": \"light_01\", \"pierce_count\": 0, \"explosion_radius_cm\": 0, \"explosion_damage_profile_id\": \"\", \"homing_turn_rate_deg_s\": 0}\n"
			"  ]\n"
			"}\n"));
		return bOk;
	}

	/** Collects whether any problem mentions the needle. */
	static bool ProblemsContain(const TArray<FString>& Problems, const TCHAR* Needle)
	{
		for (const FString& Problem : Problems)
		{
			if (Problem.Contains(Needle))
			{
				return true;
			}
		}
		return false;
	}
}

using namespace UE::UEMMO::Tasks::M5_038;

// ---------------------------------------------------------------------------
// VehicleCatalogMirrorsSourceRows
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_038VehicleCatalogMirrorsSourceRows,
	"UEMMO.Tasks.M5_038.VehicleCatalogMirrorsSourceRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_038VehicleCatalogMirrorsSourceRows::RunTest(const FString& Parameters)
{
	// The shipped manifest-gated directory (ten tables) builds a catalog that
	// carries the vehicle segment.
	FCombatDataTableSet Tables;
	if (!LoadShippedTables(*this, Tables))
	{
		return true;
	}
	TestEqual(TEXT("the shipped vehicles table holds the sample row"), Tables.Vehicles.Num(), 1);
	TestEqual(TEXT("the shipped vehicle_seats table holds the sample row"), Tables.VehicleSeats.Num(), 1);
	TestEqual(TEXT("the shipped vehicle_mounts table holds the sample row"), Tables.VehicleMounts.Num(), 1);

	FCombatCatalog Catalog;
	FString Errors;
	if (!TestTrue(TEXT("the shipped config including vehicles builds"),
		FCombatCatalog::BuildFromParsed(ToParsedConfig(Tables, true), Catalog, Errors)))
	{
		AddError(FString::Printf(TEXT("the catalog build refused: %s"), *Errors));
		return true;
	}
	TestEqual(TEXT("NumVehicles mirrors the table"), Catalog.NumVehicles(), 1);
	TestEqual(TEXT("NumVehicleSeats mirrors the table"), Catalog.NumVehicleSeats(), 1);
	TestEqual(TEXT("NumVehicleMounts mirrors the table"), Catalog.NumVehicleMounts(), 1);
	TestTrue(TEXT("the revision of a vehicle-inclusive build is non-empty"), !Catalog.GetConfigRevision().IsEmpty());

	// Every vehicle field survives the parse -> catalog round trip.
	const FVehicleDefinition* Vehicle = Catalog.FindVehicle(FName(TEXT("vehicle_wheeled_sample")));
	if (TestNotNull(TEXT("FindVehicle returns the sample vehicle"), Vehicle))
	{
		TestTrue(TEXT("the drive type is Ground"), Vehicle->DriveType == EVehicleDriveType::Ground);
		TestEqual(TEXT("MaxHealth mirrors the table"), Vehicle->MaxHealth, 250.0f);
		TestEqual(TEXT("Defense mirrors the table"), Vehicle->Defense, 15.0f);
		TestEqual(TEXT("MaxSpeedCmS mirrors the table"), Vehicle->MaxSpeedCmS, 700.0f);
		TestEqual(TEXT("AccelerationCmS2 mirrors the table"), Vehicle->AccelerationCmS2, 900.0f);
		TestTrue(TEXT("MeshPath mirrors the table"), Vehicle->MeshPath.Contains(TEXT("SM_Vehicle_Wheeled_Sample")));
		TestEqual(TEXT("CameraDistanceCm mirrors the table"), Vehicle->CameraDistanceCm, 650.0f);
		TestEqual(TEXT("CameraHeightCm mirrors the table"), Vehicle->CameraHeightCm, 260.0f);
		TestEqual(TEXT("CameraPitchDeg mirrors the table"), Vehicle->CameraPitchDeg, -18.0f);
	}

	const FVehicleSeatDefinition* Seat = Catalog.FindVehicleSeat(FName(TEXT("seat_wheeled_driver")));
	if (TestNotNull(TEXT("FindVehicleSeat returns the sample seat"), Seat))
	{
		TestTrue(TEXT("the seat role is Driver"), Seat->SeatRole == EVehicleSeatRole::Driver);
		TestEqual(TEXT("the seat's vehicle mirrors the table"), Seat->VehicleId, FName(TEXT("vehicle_wheeled_sample")));
		TestEqual(TEXT("the seat location x mirrors the table"), Seat->SeatLocationCm.X, 0.0);
		TestEqual(TEXT("the seat location y mirrors the table"), Seat->SeatLocationCm.Y, -40.0);
		TestEqual(TEXT("the seat location z mirrors the table"), Seat->SeatLocationCm.Z, 45.0);
		TestEqual(TEXT("the exit offset x mirrors the table"), Seat->ExitOffsetCm.X, -180.0);
		TestEqual(TEXT("the exit offset z mirrors the table"), Seat->ExitOffsetCm.Z, 0.0);
	}

	const FVehicleMountDefinition* Mount = Catalog.FindVehicleMount(FName(TEXT("mount_wheeled_turret")));
	if (TestNotNull(TEXT("FindVehicleMount returns the sample mount"), Mount))
	{
		TestEqual(TEXT("the mount's vehicle mirrors the table"), Mount->VehicleId, FName(TEXT("vehicle_wheeled_sample")));
		TestEqual(TEXT("the turret weapon mirrors the table"), Mount->TurretWeaponId, FName(TEXT("weapon_explosive_sample")));
		TestEqual(TEXT("the pivot x mirrors the table"), Mount->LocationCm.X, 120.0);
		TestEqual(TEXT("the pivot y mirrors the table"), Mount->LocationCm.Y, 40.0);
		TestEqual(TEXT("the pivot z mirrors the table"), Mount->LocationCm.Z, 40.0);
		TestEqual(TEXT("the pitch minimum mirrors the table"), Mount->MinPitchDegrees, -20.0f);
		TestEqual(TEXT("the pitch maximum mirrors the table"), Mount->MaxPitchDegrees, 60.0f);
	}

	// The related Weapon definition (card verification step): the mounted
	// turret weapon resolves through the same catalog into weapons and ammo.
	const FWeaponDefinition* Weapon = Mount ? Catalog.FindWeapon(Mount->TurretWeaponId) : nullptr;
	if (TestNotNull(TEXT("the mounted turret weapon resolves"), Weapon))
	{
		TestTrue(TEXT("the turret weapon fires projectiles"), Weapon->FireMode == EWeaponFireMode::Projectile);
		TestEqual(TEXT("the turret weapon magazine mirrors the table"), Weapon->MagazineSize, 3);
		TestEqual(TEXT("the turret weapon fire rate mirrors the table"), Weapon->FireRateRpm, 45.0f);
		TestEqual(TEXT("the turret weapon ammo mirrors the table"), Weapon->AmmoId, FName(TEXT("ammo_rocket")));
		TestEqual(TEXT("the turret weapon projectile mirrors the table"), Weapon->ProjectileId, FName(TEXT("rocket_explosive")));
		const FAmmoType* Ammo = Catalog.FindAmmoType(Weapon->AmmoId);
		if (TestNotNull(TEXT("the turret weapon ammo resolves"), Ammo))
		{
			TestEqual(TEXT("the rocket reserve mirrors the table"), Ammo->MaxReserve, 12);
			TestEqual(TEXT("the rocket magazine mirrors the table"), Ammo->MagazineSize, 3);
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// VehicleCatalogFailureSemantics
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_038VehicleCatalogFailureSemantics,
	"UEMMO.Tasks.M5_038.VehicleCatalogFailureSemantics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_038VehicleCatalogFailureSemantics::RunTest(const FString& Parameters)
{
	// The shared build target: every refusal case below must leave it either
	// untouched or (when the build succeeds) freshly published.
	FCombatCatalog Catalog;
	FString Errors;

	// Unknown ids return nullptr - no fallback vehicle, no stand-in row.
	{
		TestTrue(TEXT("the vehicle config builds"),
			FCombatCatalog::BuildFromParsed(MakeVehicleConfig(), Catalog, Errors));
		TestNull(TEXT("FindVehicle returns nullptr for an unknown id"),
			Catalog.FindVehicle(FName(TEXT("ghost_vehicle"))));
		TestNull(TEXT("FindVehicleSeat returns nullptr for an unknown id"),
			Catalog.FindVehicleSeat(FName(TEXT("ghost_seat"))));
		TestNull(TEXT("FindVehicleMount returns nullptr for an unknown id"),
			Catalog.FindVehicleMount(FName(TEXT("ghost_mount"))));
	}

	// An empty row id refuses the whole build and names the table.
	{
		FParsedCombatConfig NoId = MakeVehicleConfig();
		NoId.Vehicles[0].VehicleId = NAME_None;
		FString BuildErrors;
		TestFalse(TEXT("a vehicle row without an id is refused"),
			FCombatCatalog::BuildFromParsed(NoId, Catalog, BuildErrors));
		TestTrue(TEXT("the empty-id refusal names the vehicles table"), BuildErrors.Contains(TEXT("vehicles")));
		TestTrue(TEXT("the empty-id refusal says empty id"), BuildErrors.Contains(TEXT("empty id")));

		FParsedCombatConfig NoSeatId = MakeVehicleConfig();
		NoSeatId.VehicleSeats[0].SeatId = NAME_None;
		TestFalse(TEXT("a seat row without an id is refused"),
			FCombatCatalog::BuildFromParsed(NoSeatId, Catalog, BuildErrors));
		TestTrue(TEXT("the seat empty-id refusal names the vehicle_seats table"),
			BuildErrors.Contains(TEXT("vehicle_seats")));

		FParsedCombatConfig NoMountId = MakeVehicleConfig();
		NoMountId.VehicleMounts[0].MountId = NAME_None;
		TestFalse(TEXT("a mount row without an id is refused"),
			FCombatCatalog::BuildFromParsed(NoMountId, Catalog, BuildErrors));
		TestTrue(TEXT("the mount empty-id refusal names the vehicle_mounts table"),
			BuildErrors.Contains(TEXT("vehicle_mounts")));
	}

	// A duplicate id inside any vehicle table refuses with the full path.
	{
		FParsedCombatConfig DuplicateVehicle = MakeVehicleConfig();
		DuplicateVehicle.Vehicles.Add(MakeSampleVehicle(FName(TEXT("vehicle_wheeled_sample"))));
		FString DupErrors;
		TestFalse(TEXT("a duplicate vehicle id is refused"),
			FCombatCatalog::BuildFromParsed(DuplicateVehicle, Catalog, DupErrors));
		TestTrue(TEXT("the duplicate refusal says duplicate id"), DupErrors.Contains(TEXT("duplicate id")));
		TestTrue(TEXT("the duplicate refusal names the id"),
			DupErrors.Contains(TEXT("vehicle_wheeled_sample")));
		TestTrue(TEXT("the duplicate refusal names the vehicles table"), DupErrors.Contains(TEXT("vehicles")));

		FParsedCombatConfig DuplicateSeat = MakeVehicleConfig();
		DuplicateSeat.VehicleSeats.Add(MakeSampleSeat(FName(TEXT("seat_wheeled_driver"))));
		TestFalse(TEXT("a duplicate seat id is refused"),
			FCombatCatalog::BuildFromParsed(DuplicateSeat, Catalog, DupErrors));
		TestTrue(TEXT("the seat duplicate refusal names the vehicle_seats table"),
			DupErrors.Contains(TEXT("vehicle_seats")));

		FParsedCombatConfig DuplicateMount = MakeVehicleConfig();
		DuplicateMount.VehicleMounts.Add(MakeSampleMount(FName(TEXT("mount_wheeled_turret"))));
		TestFalse(TEXT("a duplicate mount id is refused"),
			FCombatCatalog::BuildFromParsed(DuplicateMount, Catalog, DupErrors));
		TestTrue(TEXT("the mount duplicate refusal names the vehicle_mounts table"),
			DupErrors.Contains(TEXT("vehicle_mounts")));
	}

	// A non-positive schema version refuses (the candidate shape guard).
	{
		FParsedCombatConfig ZeroSchema = MakeVehicleConfig();
		ZeroSchema.SchemaVersion = 0;
		FString SchemaErrors;
		TestFalse(TEXT("a zero schema version is refused"),
			FCombatCatalog::BuildFromParsed(ZeroSchema, Catalog, SchemaErrors));
		TestTrue(TEXT("the schema refusal names schema_version"), SchemaErrors.Contains(TEXT("schema_version")));
	}

	// A refused build leaves the target catalog completely untouched.
	{
		FCombatCatalog Good;
		FString GoodErrors;
		TestTrue(TEXT("the good vehicle config builds"),
			FCombatCatalog::BuildFromParsed(MakeVehicleConfig(), Good, GoodErrors));
		const FString GoodRevision = Good.GetConfigRevision();

		FParsedCombatConfig Bad = MakeVehicleConfig();
		Bad.Vehicles.Add(MakeSampleVehicle(FName(TEXT("vehicle_wheeled_sample"))));
		FString BadErrors;
		TestFalse(TEXT("a refused build returns false"),
			FCombatCatalog::BuildFromParsed(Bad, Good, BadErrors));
		TestEqual(TEXT("the refused target keeps its revision"), Good.GetConfigRevision(), GoodRevision);
		TestNotNull(TEXT("the refused target keeps its vehicle rows"),
			Good.FindVehicle(FName(TEXT("vehicle_wheeled_sample"))));
		TestNotNull(TEXT("the refused target keeps its seat rows"),
			Good.FindVehicleSeat(FName(TEXT("seat_wheeled_driver"))));
		TestNotNull(TEXT("the refused target keeps its mount rows"),
			Good.FindVehicleMount(FName(TEXT("mount_wheeled_turret"))));
	}

	// A never-built catalog reports an empty revision.
	{
		FCombatCatalog NeverBuilt;
		TestTrue(TEXT("a default-constructed catalog reports an empty revision"),
			NeverBuilt.GetConfigRevision().IsEmpty());
	}
	return true;
}

// ---------------------------------------------------------------------------
// VehicleCatalogRevisionMirror
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_038VehicleCatalogRevisionMirror,
	"UEMMO.Tasks.M5_038.VehicleCatalogRevisionMirror",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_038VehicleCatalogRevisionMirror::RunTest(const FString& Parameters)
{
	const FString Baseline = RevisionOf(MakeVehicleConfig());
	TestTrue(TEXT("the baseline revision is non-empty"), !Baseline.IsEmpty());

	// Double build from freshly assembled rows in a different order keeps
	// the revision: the repeated-generation path is stable.
	{
		FParsedCombatConfig Forward = MakeVehicleConfig();
		FParsedCombatConfig Backward = MakeBaseConfig();
		Backward.VehicleMounts.Add(MakeSampleMount());
		Backward.VehicleSeats.Add(MakeSampleSeat());
		Backward.Vehicles.Add(MakeSampleVehicle());
		TestEqual(TEXT("the same rows in a different order keep the revision"),
			RevisionOf(Backward), RevisionOf(Forward));
	}

	// Any vehicle field value change alters the revision.
	{
		FParsedCombatConfig ChangedSpeed = MakeVehicleConfig();
		ChangedSpeed.Vehicles[0].MaxSpeedCmS = 701.0f;
		TestTrue(TEXT("a vehicle speed change alters the revision"),
			RevisionOf(ChangedSpeed) != Baseline);

		FParsedCombatConfig ChangedMesh = MakeVehicleConfig();
		ChangedMesh.Vehicles[0].MeshPath = TEXT("/Game/UEMMO/Vehicles/SM_Vehicle_Other.SM_Vehicle_Other");
		TestTrue(TEXT("a mesh path change alters the revision"), RevisionOf(ChangedMesh) != Baseline);

		FParsedCombatConfig ChangedExit = MakeVehicleConfig();
		ChangedExit.VehicleSeats[0].ExitOffsetCm.X = -200.0;
		TestTrue(TEXT("a seat exit offset change alters the revision"),
			RevisionOf(ChangedExit) != Baseline);

		FParsedCombatConfig ChangedPitch = MakeVehicleConfig();
		ChangedPitch.VehicleMounts[0].MaxPitchDegrees = 61.0f;
		TestTrue(TEXT("a mount pitch clamp change alters the revision"),
			RevisionOf(ChangedPitch) != Baseline);
	}

	// An added or removed vehicle row alters the revision.
	{
		FParsedCombatConfig Added = MakeVehicleConfig();
		FVehicleDefinition Second = MakeSampleVehicle(FName(TEXT("vehicle_armored_extra")));
		Second.MaxSpeedCmS = 400.0f;
		Added.Vehicles.Add(Second);
		const FString AddedRevision = RevisionOf(Added);
		TestTrue(TEXT("an added vehicle row alters the revision"), AddedRevision != Baseline);

		FParsedCombatConfig Removed = MakeBaseConfig();
		Removed.VehicleSeats.Add(MakeSampleSeat());
		Removed.VehicleMounts.Add(MakeSampleMount());
		TestTrue(TEXT("a removed vehicle row alters the revision"), RevisionOf(Removed) != Baseline);
	}

	// The vehicle-less shape of the same six A/B rows builds a different
	// revision: the segment is provably inside the digest.
	{
		const FString SixTableOnly = RevisionOf(MakeBaseConfig());
		TestTrue(TEXT("the vehicle-less revision is non-empty"), !SixTableOnly.IsEmpty());
		TestTrue(TEXT("the vehicle-less build differs from the vehicle-inclusive build"),
			SixTableOnly != Baseline);
	}

	// A schema version change alters the revision.
	{
		FParsedCombatConfig OtherSchema = MakeVehicleConfig();
		OtherSchema.SchemaVersion = 2;
		TestTrue(TEXT("a schema version change alters the revision"),
			RevisionOf(OtherSchema) != Baseline);
	}
	return true;
}

// ---------------------------------------------------------------------------
// MissingVehicleResourcesRefuseDirectory
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_038MissingVehicleResourcesRefuseDirectory,
	"UEMMO.Tasks.M5_038.MissingVehicleResourcesRefuseDirectory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_038MissingVehicleResourcesRefuseDirectory::RunTest(const FString& Parameters)
{
	// (a) The manifest registers the vehicle tables but a file is missing:
	//     the required table is a missing file, never an accepted empty set.
	{
		const FString Dir = BuildTempDataDir(TEXT("MissingVehicleFile"));
		TestTrue(TEXT("the A/B set was written"), WriteAbTableSet(Dir));
		TestTrue(TEXT("the manifest was written"), WriteManifestWithVehicles(Dir));
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestFalse(TEXT("a missing vehicle table file refuses the directory"),
			LoadCombatDataDirectory(Dir, Tables, Problems));
		TestTrue(TEXT("the missing-file refusal names the vehicles table"),
			ProblemsContain(Problems, TEXT("vehicles")));
		TestTrue(TEXT("the missing-file refusal says missing"), ProblemsContain(Problems, TEXT("missing")));
		TestEqual(TEXT("no vehicle row was published for the refused directory"), Tables.Vehicles.Num(), 0);
		RemoveTempDataDir(Dir);
	}

	// (b) A vehicle row without a mesh path is the missing-resource refusal
	//     and the diagnostic set holds no fabricated vehicle row.
	{
		const FString Dir = BuildTempDataDir(TEXT("NoMesh"));
		const bool bWritten = WriteAbTableSet(Dir) && WriteManifestWithVehicles(Dir)
			&& WriteVehicleTables(Dir,
				TEXT("{\"vehicle_id\": \"vehicle_wheeled_sample\", \"drive_type\": \"ground\", \"max_health\": 250, \"defense\": 15, \"max_speed_cm_s\": 700, \"acceleration_cm_s2\": 900, \"mesh_path\": \"\", \"camera_distance_cm\": 650, \"camera_height_cm\": 260, \"camera_pitch_deg\": -18}"),
				TEXT("{\"seat_id\": \"seat_wheeled_driver\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"seat_role\": \"driver\", \"seat_location_cm\": {\"x\": 0, \"y\": -40, \"z\": 45}, \"exit_offset_cm\": {\"x\": -180, \"y\": 0, \"z\": 0}}"),
				TEXT("{\"mount_id\": \"mount_wheeled_turret\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"turret_weapon_id\": \"weapon_explosive_sample\", \"location_cm\": {\"x\": 0, \"y\": 40, \"z\": 120}, \"pitch_min_deg\": -20, \"pitch_max_deg\": 60}"));
		if (!TestTrue(TEXT("the no-mesh files were written"), bWritten))
		{
			RemoveTempDataDir(Dir);
			return true;
		}
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestFalse(TEXT("a vehicle row without a mesh path refuses the directory"),
			LoadCombatDataDirectory(Dir, Tables, Problems));
		TestTrue(TEXT("the no-mesh refusal names mesh_path"), ProblemsContain(Problems, TEXT("mesh_path")));
		TestEqual(TEXT("no fake vehicle row was published"), Tables.Vehicles.Num(), 0);
		RemoveTempDataDir(Dir);
	}

	// (c..e) Dangling references inside the vehicle edges refuse through the
	//        cross-table gate - the generation pipeline stops before building
	//        anything from a gated set.
	{
		struct FReferenceCase
		{
			const TCHAR* CaseName;
			const TCHAR* SeatRows;
			const TCHAR* MountRows;
			const TCHAR* ExpectedField;
			const TCHAR* ExpectedId;
		};
		const FReferenceCase Cases[] = {
			{TEXT("UnknownMountWeapon"),
				TEXT("{\"seat_id\": \"seat_wheeled_driver\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"seat_role\": \"driver\", \"seat_location_cm\": {\"x\": 0, \"y\": -40, \"z\": 45}, \"exit_offset_cm\": {\"x\": -180, \"y\": 0, \"z\": 0}}"),
				TEXT("{\"mount_id\": \"mount_wheeled_turret\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"turret_weapon_id\": \"weapon_missing\", \"location_cm\": {\"x\": 0, \"y\": 40, \"z\": 120}, \"pitch_min_deg\": -20, \"pitch_max_deg\": 60}"),
				TEXT("turret_weapon_id"), TEXT("weapon_missing")},
			{TEXT("SeatOnUnknownVehicle"),
				TEXT("{\"seat_id\": \"seat_wheeled_driver\", \"vehicle_id\": \"vehicle_missing\", \"seat_role\": \"driver\", \"seat_location_cm\": {\"x\": 0, \"y\": -40, \"z\": 45}, \"exit_offset_cm\": {\"x\": -180, \"y\": 0, \"z\": 0}}"),
				TEXT("{\"mount_id\": \"mount_wheeled_turret\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"turret_weapon_id\": \"weapon_explosive_sample\", \"location_cm\": {\"x\": 0, \"y\": 40, \"z\": 120}, \"pitch_min_deg\": -20, \"pitch_max_deg\": 60}"),
				TEXT("vehicle_id"), TEXT("vehicle_missing")},
			{TEXT("MountOnUnknownVehicle"),
				TEXT("{\"seat_id\": \"seat_wheeled_driver\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"seat_role\": \"driver\", \"seat_location_cm\": {\"x\": 0, \"y\": -40, \"z\": 45}, \"exit_offset_cm\": {\"x\": -180, \"y\": 0, \"z\": 0}}"),
				TEXT("{\"mount_id\": \"mount_wheeled_turret\", \"vehicle_id\": \"vehicle_missing\", \"turret_weapon_id\": \"weapon_explosive_sample\", \"location_cm\": {\"x\": 0, \"y\": 40, \"z\": 120}, \"pitch_min_deg\": -20, \"pitch_max_deg\": 60}"),
				TEXT("vehicle_id"), TEXT("vehicle_missing")}
		};
		for (const FReferenceCase& Case : Cases)
		{
			const FString Dir = BuildTempDataDir(Case.CaseName);
			const bool bWritten = WriteAbTableSet(Dir) && WriteManifestWithVehicles(Dir)
				&& WriteVehicleTables(Dir,
					TEXT("{\"vehicle_id\": \"vehicle_wheeled_sample\", \"drive_type\": \"ground\", \"max_health\": 250, \"defense\": 15, \"max_speed_cm_s\": 700, \"acceleration_cm_s2\": 900, \"mesh_path\": \"/Game/UEMMO/Vehicles/SM_Vehicle_Wheeled_Sample.SM_Vehicle_Wheeled_Sample\", \"camera_distance_cm\": 650, \"camera_height_cm\": 260, \"camera_pitch_deg\": -18}"),
					Case.SeatRows, Case.MountRows);
			if (!TestTrue(FString::Printf(TEXT("the %s files were written"), Case.CaseName), bWritten))
			{
				RemoveTempDataDir(Dir);
				continue;
			}
			FCombatDataTableSet Tables;
			TArray<FString> Problems;
			if (!TestTrue(FString::Printf(TEXT("the %s directory parses"), Case.CaseName),
				LoadCombatDataDirectory(Dir, Tables, Problems)))
			{
				for (const FString& Problem : Problems)
				{
					AddError(FString::Printf(TEXT("unexpected parse problem: %s"), *Problem));
				}
				RemoveTempDataDir(Dir);
				continue;
			}
			TArray<FString> ReferenceProblems;
			TestFalse(FString::Printf(TEXT("the %s reference is refused"), Case.CaseName),
				ValidateCombatDataReferences(Tables, ReferenceProblems));
			TestTrue(FString::Printf(TEXT("the %s refusal names the field %s"), Case.CaseName, Case.ExpectedField),
				ProblemsContain(ReferenceProblems, Case.ExpectedField));
			TestTrue(FString::Printf(TEXT("the %s refusal names the missing id %s"), Case.CaseName, Case.ExpectedId),
				ProblemsContain(ReferenceProblems, Case.ExpectedId));
			RemoveTempDataDir(Dir);
		}
	}

	// (f) A second seat for one vehicle refuses (one seat per vehicle).
	{
		const FString Dir = BuildTempDataDir(TEXT("MultiSeat"));
		const bool bWritten = WriteAbTableSet(Dir) && WriteManifestWithVehicles(Dir)
			&& WriteVehicleTables(Dir,
				TEXT("{\"vehicle_id\": \"vehicle_wheeled_sample\", \"drive_type\": \"ground\", \"max_health\": 250, \"defense\": 15, \"max_speed_cm_s\": 700, \"acceleration_cm_s2\": 900, \"mesh_path\": \"/Game/UEMMO/Vehicles/SM_Vehicle_Wheeled_Sample.SM_Vehicle_Wheeled_Sample\", \"camera_distance_cm\": 650, \"camera_height_cm\": 260, \"camera_pitch_deg\": -18}"),
				TEXT("{\"seat_id\": \"seat_wheeled_driver\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"seat_role\": \"driver\", \"seat_location_cm\": {\"x\": 0, \"y\": -40, \"z\": 45}, \"exit_offset_cm\": {\"x\": -180, \"y\": 0, \"z\": 0}}, "
					"{\"seat_id\": \"seat_wheeled_gunner\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"seat_role\": \"driver\", \"seat_location_cm\": {\"x\": 0, \"y\": 40, \"z\": 45}, \"exit_offset_cm\": {\"x\": -180, \"y\": 0, \"z\": 0}}"),
				TEXT("{\"mount_id\": \"mount_wheeled_turret\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"turret_weapon_id\": \"weapon_explosive_sample\", \"location_cm\": {\"x\": 0, \"y\": 40, \"z\": 120}, \"pitch_min_deg\": -20, \"pitch_max_deg\": 60}"));
		if (!TestTrue(TEXT("the multi-seat files were written"), bWritten))
		{
			RemoveTempDataDir(Dir);
			return true;
		}
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		if (!TestTrue(TEXT("the multi-seat directory parses"),
			LoadCombatDataDirectory(Dir, Tables, Problems)))
		{
			for (const FString& Problem : Problems)
			{
				AddError(FString::Printf(TEXT("unexpected parse problem: %s"), *Problem));
			}
			RemoveTempDataDir(Dir);
			return true;
		}
		TArray<FString> ReferenceProblems;
		TestFalse(TEXT("a second seat for the same vehicle is refused"),
			ValidateCombatDataReferences(Tables, ReferenceProblems));
		TestTrue(TEXT("the multi-seat refusal names one seat per vehicle"),
			ProblemsContain(ReferenceProblems, TEXT("one seat per vehicle")));
		RemoveTempDataDir(Dir);
	}
	return true;
}

// ---------------------------------------------------------------------------
// VehicleAssetsReloadFromDiskAndMirrorSource
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_038VehicleAssetsReloadFromDiskAndMirrorSource,
	"UEMMO.Tasks.M5_038.VehicleAssetsReloadFromDiskAndMirrorSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_038VehicleAssetsReloadFromDiskAndMirrorSource::RunTest(const FString& Parameters)
{
	// M5-009 pattern: the packaged build keeps its content inside the pak, so
	// the dev working-tree check skips by name there.
	if (!FPaths::DirectoryExists(FPaths::ProjectContentDir() / TEXT("UEMMO/Combat/Data")))
	{
		AddInfo(TEXT("Skipped in the packaged build: the repo working tree's saved .uasset files are dev-only."));
		return true;
	}
	if (!FPaths::DirectoryExists(FPaths::ProjectContentDir() / TEXT("UEMMO/CombatSystem/Vehicles")))
	{
		AddError(TEXT("the generated vehicle assets are missing under "
			"Content/UEMMO/CombatSystem/Vehicles; run "
			"Scripts/Editor/create_combat_system_assets.py (generate pass, then the fresh-process "
			"verify pass) before this suite"));
		return true;
	}

	// The shipped source tables are the comparison truth.
	FCombatDataTableSet Tables;
	if (!LoadShippedTables(*this, Tables))
	{
		return true;
	}
	const FVehicleDefinition* SourceVehicle = Tables.Vehicles.Find(FName(TEXT("vehicle_wheeled_sample")));
	const FVehicleSeatDefinition* SourceSeat = Tables.VehicleSeats.Find(FName(TEXT("seat_wheeled_driver")));
	const FVehicleMountDefinition* SourceMount = Tables.VehicleMounts.Find(FName(TEXT("mount_wheeled_turret")));
	if (!TestNotNull(TEXT("the shipped source carries the sample vehicle row"), SourceVehicle) ||
		!TestNotNull(TEXT("the shipped source carries the sample seat row"), SourceSeat) ||
		!TestNotNull(TEXT("the shipped source carries the sample mount row"), SourceMount))
	{
		return true;
	}

	// The saved assets reload from disk in this fresh process.
	UCombatVehicleAsset* VehicleAsset = ReloadGeneratedAsset<UCombatVehicleAsset>(*this,
		VehicleAssetName(TEXT("DA_Vehicle_"), SourceVehicle->VehicleId));
	UCombatVehicleSeatAsset* SeatAsset = ReloadGeneratedAsset<UCombatVehicleSeatAsset>(*this,
		VehicleAssetName(TEXT("DA_VehicleSeat_"), SourceSeat->SeatId));
	UCombatVehicleMountAsset* MountAsset = ReloadGeneratedAsset<UCombatVehicleMountAsset>(*this,
		VehicleAssetName(TEXT("DA_VehicleMount_"), SourceMount->MountId));
	if (!VehicleAsset || !SeatAsset || !MountAsset)
	{
		return true;
	}
	TestEqual(TEXT("the vehicle asset class mirrors the source row class"),
		VehicleAsset->GetClass(), UCombatVehicleAsset::StaticClass());
	TestEqual(TEXT("the seat asset class mirrors the source row class"),
		SeatAsset->GetClass(), UCombatVehicleSeatAsset::StaticClass());
	TestEqual(TEXT("the mount asset class mirrors the source row class"),
		MountAsset->GetClass(), UCombatVehicleMountAsset::StaticClass());

	// Every saved field mirrors its source row (the saved form is the source
	// form, not a defaulted copy).
	TestEqual(TEXT("the saved vehicle id mirrors the source"), VehicleAsset->VehicleId, SourceVehicle->VehicleId);
	TestEqual(TEXT("the saved drive type mirrors the source"),
		static_cast<int32>(VehicleAsset->DriveType), static_cast<int32>(SourceVehicle->DriveType));
	TestEqual(TEXT("the saved MaxHealth mirrors the source"), VehicleAsset->MaxHealth, SourceVehicle->MaxHealth);
	TestEqual(TEXT("the saved Defense mirrors the source"), VehicleAsset->Defense, SourceVehicle->Defense);
	TestEqual(TEXT("the saved MaxSpeedCmS mirrors the source"), VehicleAsset->MaxSpeedCmS, SourceVehicle->MaxSpeedCmS);
	TestEqual(TEXT("the saved AccelerationCmS2 mirrors the source"), VehicleAsset->AccelerationCmS2, SourceVehicle->AccelerationCmS2);
	TestEqual(TEXT("the saved MeshPath mirrors the source"), VehicleAsset->MeshPath, SourceVehicle->MeshPath);
	TestEqual(TEXT("the saved CameraDistanceCm mirrors the source"), VehicleAsset->CameraDistanceCm, SourceVehicle->CameraDistanceCm);
	TestEqual(TEXT("the saved CameraHeightCm mirrors the source"), VehicleAsset->CameraHeightCm, SourceVehicle->CameraHeightCm);
	TestEqual(TEXT("the saved CameraPitchDeg mirrors the source"), VehicleAsset->CameraPitchDeg, SourceVehicle->CameraPitchDeg);

	TestEqual(TEXT("the saved seat id mirrors the source"), SeatAsset->SeatId, SourceSeat->SeatId);
	TestEqual(TEXT("the saved seat vehicle mirrors the source"), SeatAsset->VehicleId, SourceSeat->VehicleId);
	TestEqual(TEXT("the saved seat role mirrors the source"),
		static_cast<int32>(SeatAsset->SeatRole), static_cast<int32>(SourceSeat->SeatRole));
	TestEqual(TEXT("the saved seat location x mirrors the source"), SeatAsset->SeatLocationCm.X, SourceSeat->SeatLocationCm.X);
	TestEqual(TEXT("the saved seat location y mirrors the source"), SeatAsset->SeatLocationCm.Y, SourceSeat->SeatLocationCm.Y);
	TestEqual(TEXT("the saved seat location z mirrors the source"), SeatAsset->SeatLocationCm.Z, SourceSeat->SeatLocationCm.Z);
	TestEqual(TEXT("the saved exit offset x mirrors the source"), SeatAsset->ExitOffsetCm.X, SourceSeat->ExitOffsetCm.X);
	TestEqual(TEXT("the saved exit offset y mirrors the source"), SeatAsset->ExitOffsetCm.Y, SourceSeat->ExitOffsetCm.Y);
	TestEqual(TEXT("the saved exit offset z mirrors the source"), SeatAsset->ExitOffsetCm.Z, SourceSeat->ExitOffsetCm.Z);

	TestEqual(TEXT("the saved mount id mirrors the source"), MountAsset->MountId, SourceMount->MountId);
	TestEqual(TEXT("the saved mount vehicle mirrors the source"), MountAsset->VehicleId, SourceMount->VehicleId);
	TestEqual(TEXT("the saved turret weapon mirrors the source"), MountAsset->TurretWeaponId, SourceMount->TurretWeaponId);
	TestEqual(TEXT("the saved pivot x mirrors the source"), MountAsset->LocationCm.X, SourceMount->LocationCm.X);
	TestEqual(TEXT("the saved pivot y mirrors the source"), MountAsset->LocationCm.Y, SourceMount->LocationCm.Y);
	TestEqual(TEXT("the saved pivot z mirrors the source"), MountAsset->LocationCm.Z, SourceMount->LocationCm.Z);
	TestEqual(TEXT("the saved pitch minimum mirrors the source"), MountAsset->MinPitchDegrees, SourceMount->MinPitchDegrees);
	TestEqual(TEXT("the saved pitch maximum mirrors the source"), MountAsset->MaxPitchDegrees, SourceMount->MaxPitchDegrees);

	// Revision provenance: one generation stamps one revision, the vehicle
	// assets carry the run's six-table revision (the shape the unmodified
	// asset builder assembles and stamps), and it is non-empty.
	TestTrue(TEXT("the vehicle asset carries a non-empty ConfigRevision"),
		!VehicleAsset->ConfigRevision.IsEmpty());
	TestEqual(TEXT("the seat asset shares the generation revision"),
		SeatAsset->ConfigRevision, VehicleAsset->ConfigRevision);
	TestEqual(TEXT("the mount asset shares the generation revision"),
		MountAsset->ConfigRevision, VehicleAsset->ConfigRevision);

	FParsedCombatConfig SixTableOnly = ToParsedConfig(Tables, false);
	FCombatCatalog SixCatalog;
	FString Errors;
	if (TestTrue(TEXT("the vehicle-less shipped config builds"),
		FCombatCatalog::BuildFromParsed(SixTableOnly, SixCatalog, Errors)))
	{
		TestEqual(TEXT("the saved revision equals the six-table generation revision"),
			VehicleAsset->ConfigRevision, SixCatalog.GetConfigRevision());
	}

	// The related Weapon definition asset of the mounted turret belongs to
	// the same generation and links the mount to its weapon row.
	UCombatWeaponAsset* WeaponAsset = LoadObject<UCombatWeaponAsset>(nullptr,
		TEXT("/Game/UEMMO/Combat/Data/DA_Weapon_weapon_explosive_sample.DA_Weapon_weapon_explosive_sample"));
	if (TestNotNull(TEXT("the related weapon definition asset reloads"), WeaponAsset))
	{
		TestTrue(TEXT("the weapon asset was loaded from disk (RF_WasLoaded)"),
			WeaponAsset->HasAnyFlags(RF_WasLoaded));
		TestEqual(TEXT("the weapon asset id matches the mounted turret weapon"),
			WeaponAsset->WeaponId, MountAsset->TurretWeaponId);
	}
	return true;
}

#endif
