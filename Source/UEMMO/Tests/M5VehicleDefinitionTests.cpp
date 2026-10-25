// M5-037: the single-seat ground vehicle source tables and their validation.
// Pins the vehicle extension of the shared parser/validator machinery in
// Combat/Data/CombatDataTableParser.h plus the 037 value validators in
// Vehicles/VehicleTypes.h:
// - strict per-row parsing of vehicles/vehicle_seats/vehicle_mounts (unknown
//   fields, missing fields, mistyped fields and bad ids are refused with
//   errors that name the field; only the "ground" drive type and the
//   "driver" seat role exist - aircraft, boats and multi-seat layouts are
//   reported as unsupported, never silently mapped or ignored);
// - the manifest.json registry: a manifest WITHOUT the vehicle tables loads
//   clean and produces no vehicle data, a manifest WITH them registers them
//   (the card acceptance "changing the manifest enables the vehicle
//   tables"), a supported table file outside the registry is refused and the
//   code-defined A/B required set still loads standalone without a manifest;
// - the cross-table gate: seats/mounts resolve their vehicle, mounts resolve
//   their turret weapon (a mount without a Weapon is refused), every vehicle
//   keeps at most one seat, and duplicate table/row ids carry the complete
//   error path;
// - the vehicle_spawns entry validation: an unknown VehicleId and an
//   out-of-range room-local location are each refused by name (the generic
//   validation 044 wires into the test room; no script branches on concrete
//   vehicle ids).
//
// Pure checks only: JSON text, plain structs and temporary directories under
// Saved/Temp; no UE assets, worlds or wall clocks (the M5_005 pattern).

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"

#include <limits>

#include "../Combat/Data/CombatDataTableParser.h"
#include "../Vehicles/VehicleTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_037
{
	// ------------------------------------------------------------------
	// Fixture rows (mirrors of the shipped Data/CombatSystem samples)
	// ------------------------------------------------------------------

	static const TCHAR* const SampleVehicleRowJson = TEXT(
		"{\"vehicle_id\": \"vehicle_wheeled_sample\", \"drive_type\": \"ground\", \"max_health\": 250, \"defense\": 15, "
		"\"max_speed_cm_s\": 700, \"acceleration_cm_s2\": 900, "
		"\"mesh_path\": \"/Game/UEMMO/Vehicles/SM_Vehicle_Wheeled_Sample.SM_Vehicle_Wheeled_Sample\", "
		"\"camera_distance_cm\": 650, \"camera_height_cm\": 260, \"camera_pitch_deg\": -18}");

	static const TCHAR* const SampleSeatRowJson = TEXT(
		"{\"seat_id\": \"seat_wheeled_driver\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"seat_role\": \"driver\", "
		"\"seat_location_cm\": {\"x\": 0, \"y\": -40, \"z\": 45}, \"exit_offset_cm\": {\"x\": -180, \"y\": 0, \"z\": 0}}");

	static const TCHAR* const SampleMountRowJson = TEXT(
		"{\"mount_id\": \"mount_wheeled_turret\", \"vehicle_id\": \"vehicle_wheeled_sample\", "
		"\"turret_weapon_id\": \"weapon_explosive_sample\", \"location_cm\": {\"x\": 0, \"y\": 40, \"z\": 120}, "
		"\"pitch_min_deg\": -20, \"pitch_max_deg\": 60}");

	static const TCHAR* const SampleSpawnRowJson = TEXT(
		"{\"vehicle_id\": \"vehicle_wheeled_sample\", \"location_cm\": {\"x\": 300, \"y\": 0, \"z\": 88}}");

	// The A/B-only and full required-table registries.
	static const TCHAR* const ManifestAbOnlyJson = TEXT(
		"{\"schema_version\": 1, \"required_tables\": [\"damage_profiles\", \"attack_reactions\", \"target_reactions\", "
		"\"presentations\", \"weapons\", \"ammo_types\", \"projectiles\"]}");

	static const TCHAR* const ManifestWithVehiclesJson = TEXT(
		"{\"schema_version\": 1, \"required_tables\": [\"damage_profiles\", \"attack_reactions\", \"target_reactions\", "
		"\"presentations\", \"weapons\", \"ammo_types\", \"projectiles\", \"vehicles\", \"vehicle_seats\", \"vehicle_mounts\"]}");

	// ------------------------------------------------------------------
	// Temporary directory helpers for the loader cases (the M5_005 pattern)
	// ------------------------------------------------------------------

	static FString BuildTempDataDir(const TCHAR* CaseName)
	{
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("Temp") / TEXT("M5_037") / CaseName;
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

	static bool WriteVehicleTables(const FString& Dir)
	{
		bool bOk = WriteTableFile(Dir, TEXT("vehicles.json"), FString::Printf(TEXT(
			"{\n  \"schema_version\": 1,\n  \"table\": \"vehicles\",\n  \"rows\": [\n    %s\n  ]\n}\n"), SampleVehicleRowJson));
		bOk = bOk && WriteTableFile(Dir, TEXT("vehicle_seats.json"), FString::Printf(TEXT(
			"{\n  \"schema_version\": 1,\n  \"table\": \"vehicle_seats\",\n  \"rows\": [\n    %s\n  ]\n}\n"), SampleSeatRowJson));
		bOk = bOk && WriteTableFile(Dir, TEXT("vehicle_mounts.json"), FString::Printf(TEXT(
			"{\n  \"schema_version\": 1,\n  \"table\": \"vehicle_mounts\",\n  \"rows\": [\n    %s\n  ]\n}\n"), SampleMountRowJson));
		return bOk;
	}

	/**
	 * A minimal but reference-consistent A-segment table set (the M5_005
	 * minimal set: the weapon/projectile reference the light_01 damage
	 * profile, so the reference gate comes back clean for the A/B edges).
	 */
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
			"    {\"weapon_id\": \"weapon_test_sword\", \"mode\": \"melee\", \"damage_profile_id\": \"light_01\", \"ammo_id\": \"\", \"magazine_size\": 0, \"fire_rate_rpm\": 0, \"burst_count\": 1, \"pellet_count\": 1, \"spread_degrees_deg\": 0, \"range_cm\": 0, \"projectile_id\": \"\", \"melee_attack_ids\": [\"light_01\", \"light_02\", \"launcher\", \"aerial_01\"]}\n"
			"  ]\n"
			"}\n"));
		bOk = bOk && WriteTableFile(Dir, TEXT("ammo_types.json"), TEXT(
			"{\n"
			"  \"schema_version\": 1,\n"
			"  \"table\": \"ammo_types\",\n"
			"  \"rows\": [\n"
			"    {\"ammo_id\": \"ammo_cell\", \"max_reserve\": 120, \"magazine_size\": 12}\n"
			"  ]\n"
			"}\n"));
		bOk = bOk && WriteTableFile(Dir, TEXT("projectiles.json"), TEXT(
			"{\n"
			"  \"schema_version\": 1,\n"
			"  \"table\": \"projectiles\",\n"
			"  \"rows\": [\n"
			"    {\"projectile_id\": \"bullet_test\", \"motion\": \"straight\", \"speed_cm_s\": 6000, \"lifetime_s\": 2.0, \"damage_profile_id\": \"light_01\", \"pierce_count\": 0, \"explosion_radius_cm\": 0, \"explosion_damage_profile_id\": \"\", \"homing_turn_rate_deg_s\": 0}\n"
			"  ]\n"
			"}\n"));
		return bOk;
	}

	/** The full ten-table set: the A/B set plus the three vehicle tables. */
	static bool WriteFullTableSet(const FString& Dir)
	{
		return WriteAbTableSet(Dir) && WriteVehicleTables(Dir);
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

using namespace UE::UEMMO::Tasks::M5_037;

// ---------------------------------------------------------------------------
// VehicleRowParsing
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_037VehicleRowParsing,
	"UEMMO.Tasks.M5_037.VehicleRowParsing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_037VehicleRowParsing::RunTest(const FString& Parameters)
{
	// The shipped sample row parses with every unit-bearing field intact.
	FVehicleDefinition Vehicle;
	FString Error;
	TestTrue(TEXT("the sample ground vehicle row parses"), ParseVehicleDefinition(SampleVehicleRowJson, Vehicle, Error));
	if (TestTrue(TEXT("the sample row holds values"), Error.IsEmpty()))
	{
		TestEqual(TEXT("the id is parsed"), Vehicle.VehicleId, FName(TEXT("vehicle_wheeled_sample")));
		TestTrue(TEXT("the drive type is Ground"), Vehicle.DriveType == EVehicleDriveType::Ground);
		TestEqual(TEXT("MaxHealth is parsed"), Vehicle.MaxHealth, 250.0f);
		TestEqual(TEXT("Defense is parsed"), Vehicle.Defense, 15.0f);
		TestEqual(TEXT("MaxSpeedCmS is parsed"), Vehicle.MaxSpeedCmS, 700.0f);
		TestEqual(TEXT("AccelerationCmS2 is parsed"), Vehicle.AccelerationCmS2, 900.0f);
		TestTrue(TEXT("MeshPath is parsed"), Vehicle.MeshPath.Contains(TEXT("SM_Vehicle_Wheeled_Sample")));
		TestEqual(TEXT("CameraDistanceCm is parsed"), Vehicle.CameraDistanceCm, 650.0f);
		TestEqual(TEXT("CameraHeightCm is parsed"), Vehicle.CameraHeightCm, 260.0f);
		TestEqual(TEXT("CameraPitchDeg is parsed"), Vehicle.CameraPitchDeg, -18.0f);
	}

	// Missing fields are refused and name the field.
	struct FMissingFieldCase
	{
		const TCHAR* RowJson;
		const TCHAR* FieldName;
	};
	const FMissingFieldCase MissingCases[] = {
		{TEXT("{\"drive_type\": \"ground\", \"max_health\": 250, \"defense\": 15, \"max_speed_cm_s\": 700, \"acceleration_cm_s2\": 900, \"mesh_path\": \"/Game/M\", \"camera_distance_cm\": 650, \"camera_height_cm\": 260, \"camera_pitch_deg\": -18}"), TEXT("vehicle_id")},
		{TEXT("{\"vehicle_id\": \"v\", \"max_health\": 250, \"defense\": 15, \"max_speed_cm_s\": 700, \"acceleration_cm_s2\": 900, \"mesh_path\": \"/Game/M\", \"camera_distance_cm\": 650, \"camera_height_cm\": 260, \"camera_pitch_deg\": -18}"), TEXT("drive_type")},
		{TEXT("{\"vehicle_id\": \"v\", \"drive_type\": \"ground\", \"defense\": 15, \"max_speed_cm_s\": 700, \"acceleration_cm_s2\": 900, \"mesh_path\": \"/Game/M\", \"camera_distance_cm\": 650, \"camera_height_cm\": 260, \"camera_pitch_deg\": -18}"), TEXT("max_health")},
		{TEXT("{\"vehicle_id\": \"v\", \"drive_type\": \"ground\", \"max_health\": 250, \"defense\": 15, \"max_speed_cm_s\": 700, \"acceleration_cm_s2\": 900, \"camera_distance_cm\": 650, \"camera_height_cm\": 260, \"camera_pitch_deg\": -18}"), TEXT("mesh_path")},
		{TEXT("{\"vehicle_id\": \"v\", \"drive_type\": \"ground\", \"max_health\": 250, \"defense\": 15, \"max_speed_cm_s\": 700, \"acceleration_cm_s2\": 900, \"mesh_path\": \"/Game/M\", \"camera_height_cm\": 260, \"camera_pitch_deg\": -18}"), TEXT("camera_distance_cm")}
	};
	for (const FMissingFieldCase& Case : MissingCases)
	{
		FVehicleDefinition Missing;
		TestFalse(FString::Printf(TEXT("a vehicle row without '%s' is refused"), Case.FieldName),
			ParseVehicleDefinition(Case.RowJson, Missing, Error));
		TestTrue(FString::Printf(TEXT("the missing '%s' error names the field"), Case.FieldName),
			Error.Contains(Case.FieldName));
	}

	// Unknown fields are strictly refused.
	FVehicleDefinition UnknownField;
	TestFalse(TEXT("an unknown vehicle field is refused"),
		ParseVehicleDefinition(TEXT("{\"vehicle_id\": \"v\", \"drive_type\": \"ground\", \"max_health\": 250, \"defense\": 15, \"max_speed_cm_s\": 700, \"acceleration_cm_s2\": 900, \"mesh_path\": \"/Game/M\", \"camera_distance_cm\": 650, \"camera_height_cm\": 260, \"camera_pitch_deg\": -18, \"crew\": 3}"), UnknownField, Error));
	TestTrue(TEXT("the unknown-field error names crew"), Error.Contains(TEXT("crew")));

	// Aircraft/boat drive types are unsupported and refused by name: the
	// closed enum cannot even represent them.
	const TCHAR* const UnsupportedDriveTypes[] = { TEXT("air"), TEXT("airplane"), TEXT("heli"), TEXT("boat"), TEXT("ship"), TEXT("") };
	for (const TCHAR* const DriveType : UnsupportedDriveTypes)
	{
		FVehicleDefinition Unsupported;
		TestTrue(FString::Printf(TEXT("the drive type '%s' was refused"), DriveType),
			!ParseVehicleDefinition(FString::Printf(TEXT("{\"vehicle_id\": \"v\", \"drive_type\": \"%s\", \"max_health\": 250, \"defense\": 15, \"max_speed_cm_s\": 700, \"acceleration_cm_s2\": 900, \"mesh_path\": \"/Game/M\", \"camera_distance_cm\": 650, \"camera_height_cm\": 260, \"camera_pitch_deg\": -18}"), DriveType), Unsupported, Error));
		TestTrue(FString::Printf(TEXT("the '%s' refusal names drive_type"), DriveType), Error.Contains(TEXT("drive_type")));
		TestTrue(FString::Printf(TEXT("the '%s' refusal says unsupported"), DriveType), Error.Contains(TEXT("not supported")));
	}

	// Bad ids are refused on the raw string (M5-036: before the FName move).
	FVehicleDefinition BadId;
	TestFalse(TEXT("an uppercase vehicle id is refused"),
		ParseVehicleDefinition(TEXT("{\"vehicle_id\": \"Vehicle_One\", \"drive_type\": \"ground\", \"max_health\": 250, \"defense\": 15, \"max_speed_cm_s\": 700, \"acceleration_cm_s2\": 900, \"mesh_path\": \"/Game/M\", \"camera_distance_cm\": 650, \"camera_height_cm\": 260, \"camera_pitch_deg\": -18}"), BadId, Error));
	TestTrue(TEXT("the bad-id error names VehicleId"), Error.Contains(TEXT("VehicleId")));

	// An empty mesh path is the missing-resource refusal.
	FVehicleDefinition NoMeshParse;
	TestFalse(TEXT("a vehicle without a mesh path is refused"),
		ParseVehicleDefinition(TEXT("{\"vehicle_id\": \"v\", \"drive_type\": \"ground\", \"max_health\": 250, \"defense\": 15, \"max_speed_cm_s\": 700, \"acceleration_cm_s2\": 900, \"mesh_path\": \"\", \"camera_distance_cm\": 650, \"camera_height_cm\": 260, \"camera_pitch_deg\": -18}"), NoMeshParse, Error));
	TestTrue(TEXT("the missing-mesh error names mesh_path"), Error.Contains(TEXT("mesh_path")));

	// Value rules through the 037 validator: each case isolates one illegal
	// field on an otherwise valid definition and the refusal names it.
	FVehicleDefinition Valid;
	Valid.VehicleId = FName(TEXT("v"));
	Valid.MaxHealth = 250.0f;
	Valid.Defense = 15.0f;
	Valid.MaxSpeedCmS = 700.0f;
	Valid.AccelerationCmS2 = 900.0f;
	Valid.MeshPath = TEXT("/Game/M");
	Valid.CameraDistanceCm = 650.0f;
	Valid.CameraHeightCm = 260.0f;
	Valid.CameraPitchDeg = -18.0f;

	FVehicleDefinition NoId = Valid; NoId.VehicleId = NAME_None;
	FVehicleDefinition ZeroSpeed = Valid; ZeroSpeed.MaxSpeedCmS = 0.0f;
	FVehicleDefinition Overspeed = Valid; Overspeed.MaxSpeedCmS = MaxVehicleSpeedCmS + 1.0f;
	FVehicleDefinition NegativeDefense = Valid; NegativeDefense.Defense = -1.0f;
	FVehicleDefinition NoMesh = Valid; NoMesh.MeshPath = TEXT("");
	FVehicleDefinition SteepCamera = Valid; SteepCamera.CameraPitchDeg = 120.0f;

	struct FVehicleValueCase
	{
		const FVehicleDefinition* Definition;
		const TCHAR* ExpectedField;
	};
	const FVehicleValueCase ValueCases[] = {
		{&NoId, TEXT("VehicleId")},
		{&ZeroSpeed, TEXT("MaxSpeedCmS")},
		{&Overspeed, TEXT("MaxSpeedCmS")},
		{&NegativeDefense, TEXT("Defense")},
		{&NoMesh, TEXT("MeshPath")},
		{&SteepCamera, TEXT("CameraPitchDeg")}
	};
	for (const FVehicleValueCase& Case : ValueCases)
	{
		FString ValidationErrors;
		TestFalse(FString::Printf(TEXT("the %s value case is refused"), Case.ExpectedField),
			ValidateVehicleDefinition(*Case.Definition, ValidationErrors));
		TestTrue(FString::Printf(TEXT("the %s value case names its field"), Case.ExpectedField),
			ValidationErrors.Contains(Case.ExpectedField));
	}

	// A non-finite speed is refused by the validator (the parser refuses the
	// literal already, so the struct-level rule is pinned directly).
	FVehicleDefinition NaNVehicle;
	NaNVehicle.VehicleId = FName(TEXT("v"));
	NaNVehicle.MaxHealth = 250.0f;
	NaNVehicle.MaxSpeedCmS = std::numeric_limits<float>::quiet_NaN();
	NaNVehicle.AccelerationCmS2 = 900.0f;
	NaNVehicle.MeshPath = TEXT("/Game/M");
	FString NaNErrors;
	TestFalse(TEXT("a NaN MaxSpeedCmS is refused"), ValidateVehicleDefinition(NaNVehicle, NaNErrors));
	TestTrue(TEXT("the NaN error names MaxSpeedCmS"), NaNErrors.Contains(TEXT("MaxSpeedCmS")));

	// A failed parse leaves the out struct untouched (no half-filled row).
	TestTrue(TEXT("the failed parse reset the struct"), BadId.VehicleId.IsNone());
	return true;
}

// ---------------------------------------------------------------------------
// SeatAndMountRowParsing
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_037SeatAndMountRowParsing,
	"UEMMO.Tasks.M5_037.SeatAndMountRowParsing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_037SeatAndMountRowParsing::RunTest(const FString& Parameters)
{
	// The shipped sample seat parses with its offsets intact.
	FVehicleSeatDefinition Seat;
	FString Error;
	TestTrue(TEXT("the sample driver seat row parses"), ParseVehicleSeat(SampleSeatRowJson, Seat, Error));
	if (TestTrue(TEXT("the seat row holds values"), Error.IsEmpty()))
	{
		TestEqual(TEXT("the seat id is parsed"), Seat.SeatId, FName(TEXT("seat_wheeled_driver")));
		TestEqual(TEXT("the seat's vehicle is parsed"), Seat.VehicleId, FName(TEXT("vehicle_wheeled_sample")));
		TestTrue(TEXT("the seat role is Driver"), Seat.SeatRole == EVehicleSeatRole::Driver);
		TestEqual(TEXT("the seat location x parses"), Seat.SeatLocationCm.X, 0.0);
		TestEqual(TEXT("the seat location z parses"), Seat.SeatLocationCm.Z, 45.0);
		TestEqual(TEXT("the exit offset x parses"), Seat.ExitOffsetCm.X, -180.0);
	}

	// The gunner/passenger roles of multi-seat layouts are unsupported.
	const TCHAR* const UnsupportedRoles[] = { TEXT("gunner"), TEXT("passenger"), TEXT("commander"), TEXT("") };
	for (const TCHAR* const Role : UnsupportedRoles)
	{
		FVehicleSeatDefinition Unsupported;
		TestTrue(FString::Printf(TEXT("the seat role '%s' was refused"), Role),
			!ParseVehicleSeat(FString::Printf(TEXT("{\"seat_id\": \"s\", \"vehicle_id\": \"v\", \"seat_role\": \"%s\", \"seat_location_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}, \"exit_offset_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}}"), Role), Unsupported, Error));
		TestTrue(FString::Printf(TEXT("the '%s' role refusal names seat_role"), Role), Error.Contains(TEXT("seat_role")));
		TestTrue(FString::Printf(TEXT("the '%s' role refusal says unsupported"), Role), Error.Contains(TEXT("not supported")));
	}

	// Missing and mistyped location objects name the field.
	FVehicleSeatDefinition MissingLocation;
	TestFalse(TEXT("a seat without seat_location_cm is refused"),
		ParseVehicleSeat(TEXT("{\"seat_id\": \"s\", \"vehicle_id\": \"v\", \"seat_role\": \"driver\", \"exit_offset_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}}"), MissingLocation, Error));
	TestTrue(TEXT("the missing-location error names seat_location_cm"), Error.Contains(TEXT("seat_location_cm")));

	FVehicleSeatDefinition BadLocation;
	TestFalse(TEXT("a seat location with an unknown member is refused"),
		ParseVehicleSeat(TEXT("{\"seat_id\": \"s\", \"vehicle_id\": \"v\", \"seat_role\": \"driver\", \"seat_location_cm\": {\"x\": 0, \"y\": 0, \"z\": 0, \"w\": 1}, \"exit_offset_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}}"), BadLocation, Error));
	TestTrue(TEXT("the unknown-member error names w"), Error.Contains(TEXT("w")));

	FVehicleSeatDefinition NumberLocation;
	TestFalse(TEXT("a numeric seat location is refused"),
		ParseVehicleSeat(TEXT("{\"seat_id\": \"s\", \"vehicle_id\": \"v\", \"seat_role\": \"driver\", \"seat_location_cm\": 5, \"exit_offset_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}}"), NumberLocation, Error));
	TestTrue(TEXT("the mistyped-location error names seat_location_cm"), Error.Contains(TEXT("seat_location_cm")));

	// The shipped sample mount parses with its weapon and pitch clamp.
	FVehicleMountDefinition Mount;
	TestTrue(TEXT("the sample turret mount row parses"), ParseVehicleMount(SampleMountRowJson, Mount, Error));
	if (TestTrue(TEXT("the mount row holds values"), Error.IsEmpty()))
	{
		TestEqual(TEXT("the mount id is parsed"), Mount.MountId, FName(TEXT("mount_wheeled_turret")));
		TestEqual(TEXT("the turret weapon is parsed"), Mount.TurretWeaponId, FName(TEXT("weapon_explosive_sample")));
		TestEqual(TEXT("the pivot z parses"), Mount.LocationCm.Z, 120.0);
		TestEqual(TEXT("the pitch minimum parses"), Mount.MinPitchDegrees, -20.0f);
		TestEqual(TEXT("the pitch maximum parses"), Mount.MaxPitchDegrees, 60.0f);
	}

	// An illegal pitch configuration is refused and names the clamp fields.
	FVehicleMountDefinition InvertedPitch;
	TestFalse(TEXT("an inverted pitch clamp is refused"),
		ParseVehicleMount(TEXT("{\"mount_id\": \"m\", \"vehicle_id\": \"v\", \"turret_weapon_id\": \"w\", \"location_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}, \"pitch_min_deg\": 60, \"pitch_max_deg\": -20}"), InvertedPitch, Error));
	TestTrue(TEXT("the inverted clamp error names MinPitchDegrees"), Error.Contains(TEXT("MinPitchDegrees")));

	FVehicleMountDefinition UnboundedPitch;
	TestFalse(TEXT("an unbounded pitch clamp is refused"),
		ParseVehicleMount(TEXT("{\"mount_id\": \"m\", \"vehicle_id\": \"v\", \"turret_weapon_id\": \"w\", \"location_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}, \"pitch_min_deg\": -95, \"pitch_max_deg\": 60}"), UnboundedPitch, Error));
	TestTrue(TEXT("the unbounded clamp error names MinPitchDegrees"), Error.Contains(TEXT("MinPitchDegrees")));

	// A mount without a weapon is refused (the cross-table gate resolves the
	// id; the local rule refuses the empty one). The parser path names the
	// field in its snake_case source spelling, the value validator in its
	// C++ member spelling - the assertion accepts either.
	FVehicleMountDefinition NoWeapon;
	TestFalse(TEXT("a mount without a turret weapon is refused"),
		ParseVehicleMount(TEXT("{\"mount_id\": \"m\", \"vehicle_id\": \"v\", \"turret_weapon_id\": \"\", \"location_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}, \"pitch_min_deg\": -20, \"pitch_max_deg\": 60}"), NoWeapon, Error));
	TestTrue(TEXT("the missing-weapon error names the turret weapon field"),
		Error.Contains(TEXT("turret_weapon_id")) || Error.Contains(TEXT("TurretWeaponId")));

	// An out-of-body exit offset is refused by the seat validator.
	FVehicleSeatDefinition FarExit;
	FarExit.SeatId = FName(TEXT("s"));
	FarExit.VehicleId = FName(TEXT("v"));
	FarExit.SeatLocationCm = FVector::ZeroVector;
	FarExit.ExitOffsetCm = FVector(5000.0f, 0.0f, 0.0f);
	FString SeatErrors;
	TestFalse(TEXT("an out-of-body exit offset is refused"), ValidateVehicleSeatDefinition(FarExit, SeatErrors));
	TestTrue(TEXT("the far-exit error names ExitOffsetCm"), SeatErrors.Contains(TEXT("ExitOffsetCm")));
	return true;
}

// ---------------------------------------------------------------------------
// DirectoryRegistersVehicleTables (the shipped Data/CombatSystem directory)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_037DirectoryRegistersVehicleTables,
	"UEMMO.Tasks.M5_037.DirectoryRegistersVehicleTables",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_037DirectoryRegistersVehicleTables::RunTest(const FString& Parameters)
{
	// The shipped directory now carries the manifest plus the three vehicle
	// tables; the manifest registers them, so the load stays clean.
	const FString DataDir = FPaths::ProjectDir() / TEXT("Data") / TEXT("CombatSystem");
	FCombatDataTableSet Tables;
	TArray<FString> Problems;
	TestTrue(TEXT("the shipped Data/CombatSystem directory loads"),
		LoadCombatDataDirectory(DataDir, Tables, Problems));
	if (!TestTrue(TEXT("the shipped directory load reported no problems"), Problems.Num() == 0))
	{
		for (const FString& Problem : Problems)
		{
			AddError(FString::Printf(TEXT("load problem: %s"), *Problem));
		}
		return true;
	}

	// The vehicle tables are registered and hold the sample rows.
	TestEqual(TEXT("vehicles holds 1 row"), Tables.Vehicles.Num(), 1);
	TestEqual(TEXT("vehicle_seats holds 1 row"), Tables.VehicleSeats.Num(), 1);
	TestEqual(TEXT("vehicle_mounts holds 1 row"), Tables.VehicleMounts.Num(), 1);

	const FVehicleDefinition* Vehicle = Tables.Vehicles.Find(FName(TEXT("vehicle_wheeled_sample")));
	if (TestNotNull(TEXT("vehicle_wheeled_sample is loaded"), Vehicle))
	{
		TestTrue(TEXT("the sample vehicle is a ground vehicle"), Vehicle->DriveType == EVehicleDriveType::Ground);
		TestEqual(TEXT("the sample vehicle keeps speed 700"), Vehicle->MaxSpeedCmS, 700.0f);
		TestEqual(TEXT("the sample vehicle keeps health 250"), Vehicle->MaxHealth, 250.0f);
	}
	const FVehicleSeatDefinition* Seat = Tables.VehicleSeats.Find(FName(TEXT("seat_wheeled_driver")));
	if (TestNotNull(TEXT("seat_wheeled_driver is loaded"), Seat))
	{
		TestTrue(TEXT("the sample seat is the driver seat"), Seat->SeatRole == EVehicleSeatRole::Driver);
		TestEqual(TEXT("the sample seat keeps its exit offset"), Seat->ExitOffsetCm.X, -180.0);
	}
	const FVehicleMountDefinition* Mount = Tables.VehicleMounts.Find(FName(TEXT("mount_wheeled_turret")));
	if (TestNotNull(TEXT("mount_wheeled_turret is loaded"), Mount))
	{
		TestEqual(TEXT("the sample mount fires weapon_explosive_sample"), Mount->TurretWeaponId, FName(TEXT("weapon_explosive_sample")));
		TestEqual(TEXT("the sample mount clamps pitch at -20"), Mount->MinPitchDegrees, -20.0f);
		TestEqual(TEXT("the sample mount clamps pitch at 60"), Mount->MaxPitchDegrees, 60.0f);
	}

	// The vehicle reference edges stay clean on the shipped set (the A/B
	// damage-profile gap is documented M5-005 history and not asserted here).
	TArray<FString> ReferenceProblems;
	ValidateCombatDataReferences(Tables, ReferenceProblems);
	TestFalse(TEXT("no vehicle_seats reference problem on the shipped set"),
		ProblemsContain(ReferenceProblems, TEXT("vehicle_seats")));
	TestFalse(TEXT("no vehicle_mounts reference problem on the shipped set"),
		ProblemsContain(ReferenceProblems, TEXT("vehicle_mounts")));
	return true;
}

// ---------------------------------------------------------------------------
// ManifestGatesVehicleTables (the card acceptance: the manifest switches the
// vehicle tables on and off; the A/B required set still passes standalone)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_037ManifestGatesVehicleTables,
	"UEMMO.Tasks.M5_037.ManifestGatesVehicleTables",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_037ManifestGatesVehicleTables::RunTest(const FString& Parameters)
{
	// (a) A manifest without the vehicle tables parses clean and produces NO
	//     vehicle data - the "changing the manifest disables the vehicle
	//     tables" side of the acceptance.
	{
		const FString Dir = BuildTempDataDir(TEXT("ManifestAbOnly"));
		TestTrue(TEXT("the A/B set was written"), WriteAbTableSet(Dir));
		TestTrue(TEXT("the A/B manifest was written"), WriteTableFile(Dir, TEXT("manifest.json"), ManifestAbOnlyJson));
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestTrue(TEXT("a manifest without the vehicle tables loads"), LoadCombatDataDirectory(Dir, Tables, Problems));
		if (!TestTrue(TEXT("the A/B-only manifest load reported no problems"), Problems.Num() == 0))
		{
			for (const FString& Problem : Problems)
			{
				AddError(FString::Printf(TEXT("A/B-only manifest problem: %s"), *Problem));
			}
		}
		else
		{
			TestEqual(TEXT("no vehicle data without registration"), Tables.Vehicles.Num(), 0);
			TestEqual(TEXT("no seat data without registration"), Tables.VehicleSeats.Num(), 0);
			TestEqual(TEXT("no mount data without registration"), Tables.VehicleMounts.Num(), 0);
			TestEqual(TEXT("the A/B tables still loaded"), Tables.Weapons.Num(), 1);
		}
		RemoveTempDataDir(Dir);
	}

	// (b) The manifest with the three vehicle tables registers them: the
	//     "changing the manifest enables the vehicle tables" side.
	{
		const FString Dir = BuildTempDataDir(TEXT("ManifestWithVehicles"));
		TestTrue(TEXT("the full set was written"), WriteFullTableSet(Dir));
		TestTrue(TEXT("the full manifest was written"), WriteTableFile(Dir, TEXT("manifest.json"), ManifestWithVehiclesJson));
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestTrue(TEXT("a manifest with the vehicle tables loads"), LoadCombatDataDirectory(Dir, Tables, Problems));
		if (!TestTrue(TEXT("the full manifest load reported no problems"), Problems.Num() == 0))
		{
			for (const FString& Problem : Problems)
			{
				AddError(FString::Printf(TEXT("full manifest problem: %s"), *Problem));
			}
		}
		else
		{
			TestEqual(TEXT("the vehicle table registered"), Tables.Vehicles.Num(), 1);
			TestEqual(TEXT("the seat table registered"), Tables.VehicleSeats.Num(), 1);
			TestEqual(TEXT("the mount table registered"), Tables.VehicleMounts.Num(), 1);
		}
		RemoveTempDataDir(Dir);
	}

	// (c) A supported table file outside the registry is refused (never
	//     silently loaded or ignored), and the refusal names the file and
	//     the registry field.
	{
		const FString Dir = BuildTempDataDir(TEXT("StrayVehicleFile"));
		TestTrue(TEXT("the A/B set was written"), WriteAbTableSet(Dir));
		TestTrue(TEXT("the stray vehicle file was written"), WriteVehicleTables(Dir));
		TestTrue(TEXT("the A/B manifest was written"), WriteTableFile(Dir, TEXT("manifest.json"), ManifestAbOnlyJson));
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestFalse(TEXT("an unregistered vehicle table file is refused"), LoadCombatDataDirectory(Dir, Tables, Problems));
		TestTrue(TEXT("the stray-file refusal names vehicles.json"), ProblemsContain(Problems, TEXT("vehicles.json")));
		TestTrue(TEXT("the stray-file refusal names required_tables"), ProblemsContain(Problems, TEXT("required_tables")));
		TestEqual(TEXT("the stray file loaded no vehicle data"), Tables.Vehicles.Num(), 0);
		RemoveTempDataDir(Dir);
	}

	// (d) A manifest requiring an unsupported table is refused.
	{
		const FString Dir = BuildTempDataDir(TEXT("ManifestUnknownTable"));
		TestTrue(TEXT("the A/B set was written"), WriteAbTableSet(Dir));
		TestTrue(TEXT("the bad manifest was written"), WriteTableFile(Dir, TEXT("manifest.json"), TEXT(
			"{\"schema_version\": 1, \"required_tables\": [\"weapons\", \"target_filters\"]}")));
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestFalse(TEXT("a manifest requiring an unsupported table is refused"), LoadCombatDataDirectory(Dir, Tables, Problems));
		TestTrue(TEXT("the unsupported-table refusal names required_tables"), ProblemsContain(Problems, TEXT("required_tables")));
		TestTrue(TEXT("the unsupported-table refusal names the table"), ProblemsContain(Problems, TEXT("target_filters")));
		RemoveTempDataDir(Dir);
	}

	// (e) A manifest with a duplicate required entry is refused.
	{
		const FString Dir = BuildTempDataDir(TEXT("ManifestDuplicateEntry"));
		TestTrue(TEXT("the A/B set was written"), WriteAbTableSet(Dir));
		TestTrue(TEXT("the duplicate manifest was written"), WriteTableFile(Dir, TEXT("manifest.json"), TEXT(
			"{\"schema_version\": 1, \"required_tables\": [\"weapons\", \"weapons\"]}")));
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestFalse(TEXT("a manifest with a duplicate entry is refused"), LoadCombatDataDirectory(Dir, Tables, Problems));
		TestTrue(TEXT("the duplicate-entry refusal says more than once"), ProblemsContain(Problems, TEXT("more than once")));
		RemoveTempDataDir(Dir);
	}

	// (f) The old A/B required set still passes standalone: no manifest file
	//     at all falls back to the code-defined A-segment registry.
	{
		const FString Dir = BuildTempDataDir(TEXT("NoManifestAbOnly"));
		TestTrue(TEXT("the A/B set was written"), WriteAbTableSet(Dir));
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestTrue(TEXT("a manifest-less A/B directory loads"), LoadCombatDataDirectory(Dir, Tables, Problems));
		if (!TestTrue(TEXT("the manifest-less load reported no problems"), Problems.Num() == 0))
		{
			for (const FString& Problem : Problems)
			{
				AddError(FString::Printf(TEXT("manifest-less problem: %s"), *Problem));
			}
		}
		else
		{
			TestEqual(TEXT("the manifest-less set loaded the A/B weapon"), Tables.Weapons.Num(), 1);
			TestEqual(TEXT("the manifest-less set has no vehicle data"), Tables.Vehicles.Num(), 0);
		}
		RemoveTempDataDir(Dir);
	}
	return true;
}

// ---------------------------------------------------------------------------
// VehicleCrossTableGate (duplicate ids, dangling references, multi-seat)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_037VehicleCrossTableGate,
	"UEMMO.Tasks.M5_037.VehicleCrossTableGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_037VehicleCrossTableGate::RunTest(const FString& Parameters)
{
	// Duplicate row ids inside one vehicle table are refused with the same
	// complete error path as every other table.
	const TCHAR* const DuplicateVehicleRows = TEXT(
		"{\"vehicle_id\": \"vehicle_wheeled_sample\", \"drive_type\": \"ground\", \"max_health\": 250, \"defense\": 15, \"max_speed_cm_s\": 700, \"acceleration_cm_s2\": 900, \"mesh_path\": \"/Game/M\", \"camera_distance_cm\": 650, \"camera_height_cm\": 260, \"camera_pitch_deg\": -18}, "
		"{\"vehicle_id\": \"vehicle_wheeled_sample\", \"drive_type\": \"ground\", \"max_health\": 300, \"defense\": 15, \"max_speed_cm_s\": 700, \"acceleration_cm_s2\": 900, \"mesh_path\": \"/Game/M\", \"camera_distance_cm\": 650, \"camera_height_cm\": 260, \"camera_pitch_deg\": -18}");
	const TCHAR* const DuplicateSeatRows = TEXT(
		"{\"seat_id\": \"seat_wheeled_driver\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"seat_role\": \"driver\", \"seat_location_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}, \"exit_offset_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}}, "
		"{\"seat_id\": \"seat_wheeled_driver\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"seat_role\": \"driver\", \"seat_location_cm\": {\"x\": 1, \"y\": 0, \"z\": 0}, \"exit_offset_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}}");
	const TCHAR* const DuplicateMountRows = TEXT(
		"{\"mount_id\": \"mount_wheeled_turret\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"turret_weapon_id\": \"weapon_explosive_sample\", \"location_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}, \"pitch_min_deg\": -20, \"pitch_max_deg\": 60}, "
		"{\"mount_id\": \"mount_wheeled_turret\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"turret_weapon_id\": \"weapon_explosive_sample\", \"location_cm\": {\"x\": 1, \"y\": 0, \"z\": 0}, \"pitch_min_deg\": -20, \"pitch_max_deg\": 60}");

	struct FDuplicateCase
	{
		const TCHAR* FileName;
		const TCHAR* TableName;
		const TCHAR* IdField;
		const TCHAR* Rows;
	};
	const FDuplicateCase DuplicateCases[] = {
		{TEXT("vehicles.json"), TEXT("vehicles"), TEXT("vehicle_id"), DuplicateVehicleRows},
		{TEXT("vehicle_seats.json"), TEXT("vehicle_seats"), TEXT("seat_id"), DuplicateSeatRows},
		{TEXT("vehicle_mounts.json"), TEXT("vehicle_mounts"), TEXT("mount_id"), DuplicateMountRows}
	};
	for (const FDuplicateCase& Case : DuplicateCases)
	{
		const FString Dir = BuildTempDataDir(TEXT("DuplicateIds"));
		// The manifest registers the vehicle tables (M5-078-active gating:
		// without it the files are refused as present-but-unregistered and
		// the duplicate path would never run).
		bool bWritten = WriteTableFile(Dir, TEXT("manifest.json"), ManifestWithVehiclesJson);
		bWritten = bWritten && WriteFullTableSet(Dir);
		bWritten = bWritten && WriteTableFile(Dir, Case.FileName, FString::Printf(TEXT(
			"{\n  \"schema_version\": 1,\n  \"table\": \"%s\",\n  \"rows\": [\n    %s\n  ]\n}\n"),
			Case.TableName, Case.Rows));
		if (!TestTrue(FString::Printf(TEXT("the duplicate case files were written (%s)"), Case.FileName), bWritten))
		{
			RemoveTempDataDir(Dir);
			continue;
		}
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestFalse(FString::Printf(TEXT("a duplicate id in %s is refused"), Case.FileName),
			LoadCombatDataDirectory(Dir, Tables, Problems));
		TestTrue(FString::Printf(TEXT("the %s duplicate refusal says duplicate"), Case.FileName),
			ProblemsContain(Problems, TEXT("duplicate")));
		TestTrue(FString::Printf(TEXT("the %s duplicate refusal names the file"), Case.FileName),
			ProblemsContain(Problems, Case.FileName));
		RemoveTempDataDir(Dir);
	}

	// A dangling reference inside the vehicle edges is refused by the
	// cross-table gate and names the source row, the field and the id.
	struct FReferenceCase
	{
		const TCHAR* Description;
		const TCHAR* SeatRows;
		const TCHAR* MountRows;
		const TCHAR* ExpectedField;
		const TCHAR* ExpectedId;
	};
	const FReferenceCase ReferenceCases[] = {
		{TEXT("a seat on an unknown vehicle"),
			TEXT("{\"seat_id\": \"seat_ghost\", \"vehicle_id\": \"vehicle_missing\", \"seat_role\": \"driver\", \"seat_location_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}, \"exit_offset_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}}"),
			TEXT("{\"mount_id\": \"mount_wheeled_turret\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"turret_weapon_id\": \"weapon_explosive_sample\", \"location_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}, \"pitch_min_deg\": -20, \"pitch_max_deg\": 60}"),
			TEXT("vehicle_id"), TEXT("vehicle_missing")},
		{TEXT("a mount on an unknown vehicle"),
			TEXT("{\"seat_id\": \"seat_wheeled_driver\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"seat_role\": \"driver\", \"seat_location_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}, \"exit_offset_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}}"),
			TEXT("{\"mount_id\": \"mount_ghost\", \"vehicle_id\": \"vehicle_missing\", \"turret_weapon_id\": \"weapon_explosive_sample\", \"location_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}, \"pitch_min_deg\": -20, \"pitch_max_deg\": 60}"),
			TEXT("vehicle_id"), TEXT("vehicle_missing")},
		{TEXT("a mount with a missing weapon"),
			TEXT("{\"seat_id\": \"seat_wheeled_driver\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"seat_role\": \"driver\", \"seat_location_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}, \"exit_offset_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}}"),
			TEXT("{\"mount_id\": \"mount_wheeled_turret\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"turret_weapon_id\": \"weapon_missing\", \"location_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}, \"pitch_min_deg\": -20, \"pitch_max_deg\": 60}"),
			TEXT("turret_weapon_id"), TEXT("weapon_missing")}
	};
	for (const FReferenceCase& Case : ReferenceCases)
	{
		const FString Dir = BuildTempDataDir(TEXT("VehicleRefCase"));
		// The manifest registers the vehicle tables so the cross-table gate
		// actually runs (see the DuplicateIds note).
		bool bWritten = WriteTableFile(Dir, TEXT("manifest.json"), ManifestWithVehiclesJson);
		bWritten = bWritten && WriteFullTableSet(Dir);
		bWritten = bWritten && WriteTableFile(Dir, TEXT("vehicle_seats.json"), FString::Printf(TEXT(
			"{\n  \"schema_version\": 1,\n  \"table\": \"vehicle_seats\",\n  \"rows\": [\n    %s\n  ]\n}\n"), Case.SeatRows));
		bWritten = bWritten && WriteTableFile(Dir, TEXT("vehicle_mounts.json"), FString::Printf(TEXT(
			"{\n  \"schema_version\": 1,\n  \"table\": \"vehicle_mounts\",\n  \"rows\": [\n    %s\n  ]\n}\n"), Case.MountRows));
		if (!TestTrue(FString::Printf(TEXT("the reference case files were written (%s)"), Case.Description), bWritten))
		{
			RemoveTempDataDir(Dir);
			continue;
		}
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		if (!TestTrue(FString::Printf(TEXT("the reference case directory loads (%s)"), Case.Description),
			LoadCombatDataDirectory(Dir, Tables, Problems)))
		{
			for (const FString& Problem : Problems)
			{
				AddError(FString::Printf(TEXT("unexpected load problem: %s"), *Problem));
			}
			RemoveTempDataDir(Dir);
			continue;
		}
		TArray<FString> ReferenceProblems;
		TestFalse(FString::Printf(TEXT("%s is refused"), Case.Description),
			ValidateCombatDataReferences(Tables, ReferenceProblems));
		TestTrue(FString::Printf(TEXT("the %s refusal names the field %s"), Case.Description, Case.ExpectedField),
			ProblemsContain(ReferenceProblems, Case.ExpectedField));
		TestTrue(FString::Printf(TEXT("the %s refusal names the missing id %s"), Case.Description, Case.ExpectedId),
			ProblemsContain(ReferenceProblems, Case.ExpectedId));
		RemoveTempDataDir(Dir);
	}

	// Multi-seat layouts are refused: a second seat row for the same vehicle
	// fails the one-seat-per-vehicle rule (explicit "not supported", never a
	// silent second occupant).
	{
		const FString Dir = BuildTempDataDir(TEXT("MultiSeat"));
		const bool bWritten = WriteTableFile(Dir, TEXT("manifest.json"), ManifestWithVehiclesJson)
			&& WriteAbTableSet(Dir)
			&& WriteVehicleTables(Dir)
			&& WriteTableFile(Dir, TEXT("vehicle_seats.json"), TEXT(
				"{\n  \"schema_version\": 1,\n  \"table\": \"vehicle_seats\",\n  \"rows\": [\n"
				"    {\"seat_id\": \"seat_wheeled_driver\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"seat_role\": \"driver\", \"seat_location_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}, \"exit_offset_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}},\n"
				"    {\"seat_id\": \"seat_wheeled_gunner\", \"vehicle_id\": \"vehicle_wheeled_sample\", \"seat_role\": \"driver\", \"seat_location_cm\": {\"x\": 0, \"y\": 40, \"z\": 0}, \"exit_offset_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}}\n"
				"  ]\n}\n"));
		if (!TestTrue(TEXT("the multi-seat files were written"), bWritten))
		{
			RemoveTempDataDir(Dir);
			return true;
		}
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		if (!TestTrue(TEXT("the multi-seat directory loads"), LoadCombatDataDirectory(Dir, Tables, Problems)))
		{
			for (const FString& Problem : Problems)
			{
				AddError(FString::Printf(TEXT("unexpected load problem: %s"), *Problem));
			}
			RemoveTempDataDir(Dir);
			return true;
		}
		TArray<FString> ReferenceProblems;
		TestFalse(TEXT("a second seat for the same vehicle is refused"),
			ValidateCombatDataReferences(Tables, ReferenceProblems));
		TestTrue(TEXT("the multi-seat refusal names one seat per vehicle"),
			ProblemsContain(ReferenceProblems, TEXT("one seat per vehicle")));
		TestTrue(TEXT("the multi-seat refusal names the second seat"),
			ProblemsContain(ReferenceProblems, TEXT("seat_wheeled_gunner")));
		RemoveTempDataDir(Dir);
	}
	return true;
}

// ---------------------------------------------------------------------------
// VehicleSpawnEntries (the generic test_room.vehicle_spawns validation)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_037VehicleSpawnEntries,
	"UEMMO.Tasks.M5_037.VehicleSpawnEntries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_037VehicleSpawnEntries::RunTest(const FString& Parameters)
{
	// The sample spawn entry parses with its room-local location.
	FVehicleSpawnEntry Spawn;
	FString Error;
	TestTrue(TEXT("the sample spawn entry parses"), ParseVehicleSpawnEntry(SampleSpawnRowJson, Spawn, Error));
	if (TestTrue(TEXT("the spawn entry holds values"), Error.IsEmpty()))
	{
		TestEqual(TEXT("the spawn vehicle is parsed"), Spawn.VehicleId, FName(TEXT("vehicle_wheeled_sample")));
		TestEqual(TEXT("the spawn x parses"), Spawn.LocationCm.X, 300.0);
		TestEqual(TEXT("the spawn z parses"), Spawn.LocationCm.Z, 88.0);
	}

	// Missing/mistyped fields name the field.
	FVehicleSpawnEntry MissingId;
	TestFalse(TEXT("a spawn entry without vehicle_id is refused"),
		ParseVehicleSpawnEntry(TEXT("{\"location_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}}"), MissingId, Error));
	TestTrue(TEXT("the missing-vehicle error names vehicle_id"), Error.Contains(TEXT("vehicle_id")));

	FVehicleSpawnEntry MissingLocation;
	TestFalse(TEXT("a spawn entry without location_cm is refused"),
		ParseVehicleSpawnEntry(TEXT("{\"vehicle_id\": \"v\"}"), MissingLocation, Error));
	TestTrue(TEXT("the missing-location error names location_cm"), Error.Contains(TEXT("location_cm")));

	FVehicleSpawnEntry UnknownField;
	TestFalse(TEXT("a spawn entry with an unknown field is refused"),
		ParseVehicleSpawnEntry(TEXT("{\"vehicle_id\": \"v\", \"location_cm\": {\"x\": 0, \"y\": 0, \"z\": 0}, \"rotation\": 90}"), UnknownField, Error));
	TestTrue(TEXT("the unknown-field error names rotation"), Error.Contains(TEXT("rotation")));

	// Out-of-range locations are refused and name the location axis: an
	// underground spawn and an absurdly remote spawn both fail.
	FVehicleSpawnEntry Underground;
	TestFalse(TEXT("an underground spawn is refused"),
		ParseVehicleSpawnEntry(TEXT("{\"vehicle_id\": \"v\", \"location_cm\": {\"x\": 300, \"y\": 0, \"z\": -5}}"), Underground, Error));
	TestTrue(TEXT("the underground refusal names LocationCm.Z"), Error.Contains(TEXT("LocationCm.Z")));

	FVehicleSpawnEntry Remote;
	TestFalse(TEXT("an absurdly remote spawn is refused"),
		ParseVehicleSpawnEntry(TEXT("{\"vehicle_id\": \"v\", \"location_cm\": {\"x\": 30000, \"y\": 0, \"z\": 88}}"), Remote, Error));
	TestTrue(TEXT("the remote refusal names LocationCm.X"), Error.Contains(TEXT("LocationCm.X")));

	// The spawn gate: entries resolve against the vehicles table (no script
	// ever branches on a concrete vehicle id) and re-run the value rules.
	FCombatDataTableSet Tables;
	FVehicleDefinition Vehicle;
	if (!TestTrue(TEXT("the gate fixture vehicle row parses"),
		ParseVehicleDefinition(SampleVehicleRowJson, Vehicle, Error)))
	{
		AddError(Error);
		return true;
	}
	Tables.Vehicles.Add(Vehicle.VehicleId, Vehicle);

	TArray<FVehicleSpawnEntry> Spawns;
	Spawns.Add(Spawn);
	TArray<FString> GateProblems;
	TestTrue(TEXT("a registered vehicle spawn passes the gate"),
		ValidateVehicleSpawnReferences(Tables, Spawns, GateProblems));
	TestEqual(TEXT("the clean gate reported no problems"), GateProblems.Num(), 0);

	// An unknown VehicleId is refused by name.
	TArray<FVehicleSpawnEntry> GhostSpawns;
	FVehicleSpawnEntry Ghost;
	Ghost.VehicleId = FName(TEXT("ghost_vehicle"));
	Ghost.LocationCm = FVector(300.0f, 0.0f, 88.0f);
	GhostSpawns.Add(Ghost);
	TestFalse(TEXT("a spawn for an unknown vehicle is refused"),
		ValidateVehicleSpawnReferences(Tables, GhostSpawns, GateProblems));
	TestTrue(TEXT("the ghost refusal names vehicle_id"), ProblemsContain(GateProblems, TEXT("vehicle_id")));
	TestTrue(TEXT("the ghost refusal names the id"), ProblemsContain(GateProblems, TEXT("ghost_vehicle")));
	TestTrue(TEXT("the ghost refusal names vehicles"), ProblemsContain(GateProblems, TEXT("vehicles")));

	// A hand-assembled out-of-range entry cannot bypass the value rules: the
	// gate re-runs them and names the axis and the entry index.
	TArray<FVehicleSpawnEntry> BrokenSpawns;
	FVehicleSpawnEntry Broken;
	Broken.VehicleId = FName(TEXT("vehicle_wheeled_sample"));
	Broken.LocationCm = FVector(0.0f, 0.0f, -50.0f);
	BrokenSpawns.Add(Broken);
	TestFalse(TEXT("a hand-built out-of-range spawn is refused"),
		ValidateVehicleSpawnReferences(Tables, BrokenSpawns, GateProblems));
	TestTrue(TEXT("the hand-built refusal names the entry index"), ProblemsContain(GateProblems, TEXT("entry 0")));
	TestTrue(TEXT("the hand-built refusal names LocationCm.Z"), ProblemsContain(GateProblems, TEXT("LocationCm.Z")));
	return true;
}

#endif
