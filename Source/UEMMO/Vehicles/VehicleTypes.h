#pragma once

#include "CoreMinimal.h"

#include "../Combat/System/DamageTypes.h"

/**
 * M5-037: ground vehicle definition value types (M5 interface contract
 * section 1, first owner 037 for Vehicles/*). A definition is the shared,
 * immutable description of one vehicle kind; the entity actor (038), the seat
 * occupation state (039..041), the movement integration (042) and the turret
 * mounting (043) belong to the later vehicle tasks. Plain value types on
 * purpose: validation stays pure C++ logic testable without a World and no
 * struct here ever references or holds a World/Actor pointer (contract
 * section 0.5).
 *
 * Units (contract section 4: every quantity carries its unit - the field
 * names and/or the member comments state them):
 * - speeds in centimeters per second (MaxSpeedCmS),
 * - accelerations in centimeters per second squared (AccelerationCmS2),
 * - lengths and offsets in centimeters (LocationCm, ExitOffsetCm,
 *   CameraDistanceCm, CameraHeightCm),
 * - angles in degrees (CameraPitchDeg, MinPitchDegrees, MaxPitchDegrees),
 * - health in health points, defense in defense points of the shared
 *   damage formula (M5-011), both reusing the existing component pool at
 *   runtime (the vehicle hull owns a HealthComponent, M5-039).
 *
 * Axes (contract section 0.6): X lateral, Y depth, Z height. Every offset
 * below is relative to the vehicle origin in that frame.
 *
 * Drive-type vocabulary: the first version implements exactly one drive
 * type, Ground (an X/Y platform driven on the floor plane). Aircraft, boats
 * and multi-seat layouts are OUT of the implemented range and are expressed
 * as unsupported, never silently ignored:
 * - the closed enum carries only Ground, so no air/ship token can even be
 *   represented;
 * - ParseVehicleDriveType/ParseVehicleSeatRole accept only the implemented
 *   tokens ("ground"/"driver") and the parser reports everything else as an
 *   unsupported drive type / seat role;
 * - the cross-table gate refuses a second seat row for the same vehicle
 *   (one seat per vehicle; multi-seat layouts arrive with a later card).
 */

/** Drive type of a vehicle; closed by design until a card implements more. */
enum class EVehicleDriveType : uint8
{
	/** X/Y ground platform driven on the floor plane; the only implemented value. */
	Ground = 0
};

/** Role of one vehicle seat; closed by design until a card implements more. */
enum class EVehicleSeatRole : uint8
{
	/** The single driving seat; the first version supports exactly this one. */
	Driver = 0
};

/**
 * Upper bounds of the first-version vehicle fields. The bounds are sanity
 * limits for the source tables, not tuning targets; raising one is a
 * deliberate schema change. The body offsets bound seat/mount/exit offsets
 * relative to the vehicle origin (a hull of a few meters), the spawn bounds
 * bound room-local spawn locations the same way the test room does.
 */
constexpr float MaxVehicleHealth = 100000.0f;
constexpr float MaxVehicleDefense = 10000.0f;
constexpr float MaxVehicleSpeedCmS = 3000.0f;
constexpr float MaxVehicleAccelerationCmS2 = 10000.0f;
constexpr float MaxVehicleCameraDistanceCm = 2000.0f;
constexpr float MaxVehicleCameraHeightCm = 2000.0f;
constexpr float MaxVehicleCameraPitchDegrees = 89.0f;
constexpr float MaxTurretPitchDegrees = 89.0f;
constexpr float MaxVehicleBodyOffsetCm = 1000.0f;
constexpr float MaxVehicleSpawnHorizontalCm = 20000.0f;
constexpr float MaxVehicleSpawnHeightCm = 5000.0f;

/** Returns true only for the closed drive-type values. */
inline bool IsValidVehicleDriveType(EVehicleDriveType DriveType)
{
	switch (DriveType)
	{
	case EVehicleDriveType::Ground:
		return true;
	default:
		return false;
	}
}

/**
 * Parses a drive type from source-data text. Only "ground" exists; every
 * other token (air, heli, boat, ship, ...) returns false and leaves
 * OutDriveType untouched so the caller reports the unsupported type instead
 * of guessing one.
 */
inline bool ParseVehicleDriveType(const FString& Text, EVehicleDriveType& OutDriveType)
{
	if (Text.Equals(TEXT("ground"), ESearchCase::IgnoreCase))
	{
		OutDriveType = EVehicleDriveType::Ground;
		return true;
	}
	return false;
}

/** Returns true only for the closed seat-role values. */
inline bool IsValidVehicleSeatRole(EVehicleSeatRole SeatRole)
{
	switch (SeatRole)
	{
	case EVehicleSeatRole::Driver:
		return true;
	default:
		return false;
	}
}

/**
 * Parses a seat role from source-data text. Only "driver" exists; the
 * gunner/passenger roles of a future multi-seat card return false so the
 * caller reports the unsupported role.
 */
inline bool ParseVehicleSeatRole(const FString& Text, EVehicleSeatRole& OutSeatRole)
{
	if (Text.Equals(TEXT("driver"), ESearchCase::IgnoreCase))
	{
		OutSeatRole = EVehicleSeatRole::Driver;
		return true;
	}
	return false;
}

/** True when every component of the vector is finite (no NaN/Inf leaked in). */
inline bool IsFiniteVector(const FVector& Value)
{
	return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
}

/** True when every component is finite and its magnitude stays inside the bound. */
inline bool IsBoundedVector(const FVector& Value, float MaxAbsPerAxisCm)
{
	return IsFiniteVector(Value)
		&& FMath::Abs(Value.X) <= MaxAbsPerAxisCm
		&& FMath::Abs(Value.Y) <= MaxAbsPerAxisCm
		&& FMath::Abs(Value.Z) <= MaxAbsPerAxisCm;
}

/** Immutable description of one vehicle kind (interface contract section 1, owner 037). */
struct FVehicleDefinition
{
	/**
	 * Stable business identifier (e.g. "vehicle_wheeled_sample"); lowercase
	 * a-z0-9_ per the shared id rule, unique inside the vehicles table. The
	 * character-set check lives on the raw JSON string in the parser
	 * (M5-036); here only an absent id (NAME_None) is refused.
	 */
	FName VehicleId;

	/** Drive type; must be the one implemented value (Ground). */
	EVehicleDriveType DriveType = EVehicleDriveType::Ground;

	/**
	 * Hull health in health points; the vehicle's HealthComponent pool at
	 * runtime. Finite and greater than 0 (an invulnerable or dead-on-spawn
	 * hull is a configuration error).
	 */
	float MaxHealth = 0.0f;

	/**
	 * Hull defense in defense points of the shared damage formula. Finite
	 * and not negative.
	 */
	float Defense = 0.0f;

	/** Straight-line top speed in cm/s on the ground plane. Finite and greater than 0. */
	float MaxSpeedCmS = 0.0f;

	/** Throttle acceleration in cm/s^2. Finite and greater than 0. */
	float AccelerationCmS2 = 0.0f;

	/**
	 * Soft object path of the hull mesh (a /Game/... path). Required
	 * non-empty: a vehicle without a hull mesh has nothing to render and no
	 * socket to mount a seat or turret to. The cook-list wiring of the path
	 * belongs to the later catalog/generation cards; presence is the
	 * source-table gate here.
	 */
	FString MeshPath;

	/** Follow-camera boom length in centimeters from the vehicle origin. Finite and greater than 0. */
	float CameraDistanceCm = 0.0f;

	/** Follow-camera height in centimeters above the vehicle origin. Finite, magnitude bounded. */
	float CameraHeightCm = 0.0f;

	/** Follow-camera pitch in degrees (negative looks down). Finite, magnitude inside 89. */
	float CameraPitchDeg = 0.0f;
};

/**
 * One seat row of vehicle_seats.json: the vehicle the seat belongs to, its
 * role, where the occupant sits and where the occupant is dropped when
 * leaving - all offsets in centimeters relative to the vehicle origin.
 */
struct FVehicleSeatDefinition
{
	/** Stable business identifier (e.g. "seat_wheeled_driver"); unique inside the seats table. */
	FName SeatId;

	/** The vehicle this seat is installed in; must resolve into vehicles (cross-table gate). */
	FName VehicleId;

	/** Seat role; must be the one implemented value (Driver). */
	EVehicleSeatRole SeatRole = EVehicleSeatRole::Driver;

	/** Occupant seat offset in centimeters relative to the vehicle origin. */
	FVector SeatLocationCm = FVector::ZeroVector;

	/**
	 * Exit offset in centimeters relative to the vehicle origin: where the
	 * occupant is dropped when leaving the vehicle (the driver's feet land
	 * at vehicle origin + ExitOffsetCm).
	 */
	FVector ExitOffsetCm = FVector::ZeroVector;
};

/**
 * One turret mount row of vehicle_mounts.json: the vehicle carrying the
 * mount, the weapon the turret fires, the pivot offset and the pitch clamp.
 * The first version sets no yaw limit (free horizontal traverse); adding a
 * yaw clamp is a deliberate schema change.
 */
struct FVehicleMountDefinition
{
	/** Stable business identifier (e.g. "mount_wheeled_turret"); unique inside the mounts table. */
	FName MountId;

	/** The vehicle this mount is installed on; must resolve into vehicles (cross-table gate). */
	FName VehicleId;

	/**
	 * The weapons-table id the turret fires. Required non-empty (a mount
	 * without a weapon is refused) and must resolve into weapons (cross-table
	 * gate, "missing Weapon refuses").
	 */
	FName TurretWeaponId;

	/** Turret pivot offset in centimeters relative to the vehicle origin. */
	FVector LocationCm = FVector::ZeroVector;

	/**
	 * Turret pitch clamp in degrees; both values finite inside
	 * +/-MaxTurretPitchDegrees and MinPitchDegrees <= MaxPitchDegrees (an
	 * inverted or unbounded clamp is an illegal pitch configuration).
	 */
	float MinPitchDegrees = 0.0f;
	float MaxPitchDegrees = 0.0f;
};

/**
 * One vehicle_spawns entry of the test_room structures (Data/test_room.json
 * and the target_filters.json test_room manifest): which vehicle to place
 * and where, room-local. New vehicles join a room by adding a list entry -
 * the validation resolves ids against the vehicles table, no script ever
 * branches on a concrete vehicle id.
 */
struct FVehicleSpawnEntry
{
	/** The vehicles-table id to spawn; must resolve into vehicles (cross-table gate). */
	FName VehicleId;

	/**
	 * Room-local spawn location in centimeters (X lateral, Y depth, Z
	 * height). Finite, |X|/|Y| inside MaxVehicleSpawnHorizontalCm and Z
	 * inside 0..MaxVehicleSpawnHeightCm (an underground or absurdly remote
	 * spawn is a configuration error).
	 */
	FVector LocationCm = FVector::ZeroVector;
};

/**
 * Validates a single vehicle definition in isolation and returns true when
 * legal. Rules (M5-037): VehicleId present (NAME_None refused; the
 * character-set rule lives on the raw JSON string in the parser, M5-036),
 * DriveType the one implemented value, MaxHealth/MaxSpeedCmS/AccelerationCmS2
 * finite and strictly positive inside their bounds, Defense finite and not
 * negative, MeshPath non-empty, camera values finite and inside their
 * bounds. Cross-table reference checks belong to the parser gate.
 * On failure OutErrors joins every problem with "; " and each problem names
 * its field (e.g. "VehicleId", "MaxSpeedCmS", "MeshPath").
 */
inline bool ValidateVehicleDefinition(const FVehicleDefinition& Definition, FString& OutErrors)
{
	TArray<FString> Problems;

	// Identity: an absent id can never be referenced by seats, mounts or the
	// spawn lists ("None" displays as the legal-looking "None", so only the
	// parser sees the real character set).
	if (Definition.VehicleId == NAME_None)
	{
		Problems.Add(FString::Printf(TEXT("VehicleId '%s' must be 1..64 characters of a-z, 0-9 and _"),
			*Definition.VehicleId.ToString()));
	}

	// Drive type: only Ground is implemented; an aircraft or boat value can
	// not even be represented in the closed enum, but a corrupted value must
	// still be reported instead of silently treated as ground.
	if (!IsValidVehicleDriveType(Definition.DriveType))
	{
		Problems.Add(FString::Printf(TEXT("DriveType must be Ground (the only implemented drive type; aircraft and boat drive types are not supported; got enum value %d)"),
			static_cast<int32>(Definition.DriveType)));
	}

	// Hull health: zero or negative dies instantly, non-finite poisons the
	// shared damage path.
	if (!FMath::IsFinite(Definition.MaxHealth) || Definition.MaxHealth <= 0.0f || Definition.MaxHealth > MaxVehicleHealth)
	{
		Problems.Add(FString::Printf(TEXT("MaxHealth must be a finite value in (0..%f) (got %s)"),
			MaxVehicleHealth, *FString::SanitizeFloat(Definition.MaxHealth)));
	}

	// Defense: negative would amplify damage through the shared formula.
	if (!FMath::IsFinite(Definition.Defense) || Definition.Defense < 0.0f || Definition.Defense > MaxVehicleDefense)
	{
		Problems.Add(FString::Printf(TEXT("Defense must be a finite value in 0..%f (got %s)"),
			MaxVehicleDefense, *FString::SanitizeFloat(Definition.Defense)));
	}

	// Speed/acceleration: a non-positive top speed or acceleration can never
	// drive the hull anywhere.
	if (!FMath::IsFinite(Definition.MaxSpeedCmS) || Definition.MaxSpeedCmS <= 0.0f || Definition.MaxSpeedCmS > MaxVehicleSpeedCmS)
	{
		Problems.Add(FString::Printf(TEXT("MaxSpeedCmS must be a finite value in (0..%f) cm/s (got %s)"),
			MaxVehicleSpeedCmS, *FString::SanitizeFloat(Definition.MaxSpeedCmS)));
	}
	if (!FMath::IsFinite(Definition.AccelerationCmS2) || Definition.AccelerationCmS2 <= 0.0f || Definition.AccelerationCmS2 > MaxVehicleAccelerationCmS2)
	{
		Problems.Add(FString::Printf(TEXT("AccelerationCmS2 must be a finite value in (0..%f) cm/s^2 (got %s)"),
			MaxVehicleAccelerationCmS2, *FString::SanitizeFloat(Definition.AccelerationCmS2)));
	}

	// Hull mesh: a vehicle without a mesh reference has nothing to render.
	if (Definition.MeshPath.IsEmpty())
	{
		Problems.Add(TEXT("MeshPath must be a non-empty soft object path (the hull mesh reference)"));
	}

	// Camera: a non-positive boom length puts the camera inside the hull; the
	// pitch stays inside the gimbal limit.
	if (!FMath::IsFinite(Definition.CameraDistanceCm) || Definition.CameraDistanceCm <= 0.0f || Definition.CameraDistanceCm > MaxVehicleCameraDistanceCm)
	{
		Problems.Add(FString::Printf(TEXT("CameraDistanceCm must be a finite value in (0..%f) cm (got %s)"),
			MaxVehicleCameraDistanceCm, *FString::SanitizeFloat(Definition.CameraDistanceCm)));
	}
	if (!FMath::IsFinite(Definition.CameraHeightCm) || FMath::Abs(Definition.CameraHeightCm) > MaxVehicleCameraHeightCm)
	{
		Problems.Add(FString::Printf(TEXT("CameraHeightCm must be a finite value within +/-%f cm (got %s)"),
			MaxVehicleCameraHeightCm, *FString::SanitizeFloat(Definition.CameraHeightCm)));
	}
	if (!FMath::IsFinite(Definition.CameraPitchDeg) || FMath::Abs(Definition.CameraPitchDeg) > MaxVehicleCameraPitchDegrees)
	{
		Problems.Add(FString::Printf(TEXT("CameraPitchDeg must be a finite value within +/-%f degrees (got %s)"),
			MaxVehicleCameraPitchDegrees, *FString::SanitizeFloat(Definition.CameraPitchDeg)));
	}

	return FinishDefinitionValidation(Problems, OutErrors);
}

/**
 * Validates a single vehicle seat in isolation. Rules (M5-037): SeatId and
 * VehicleId present, SeatRole the one implemented value (Driver), seat and
 * exit offsets finite and inside the per-axis body bound. Whether the seat's
 * vehicle exists and whether it is the vehicle's only seat is the
 * cross-table gate's rule.
 */
inline bool ValidateVehicleSeatDefinition(const FVehicleSeatDefinition& Seat, FString& OutErrors)
{
	TArray<FString> Problems;

	if (Seat.SeatId == NAME_None)
	{
		Problems.Add(FString::Printf(TEXT("SeatId '%s' must be 1..64 characters of a-z, 0-9 and _"),
			*Seat.SeatId.ToString()));
	}
	if (Seat.VehicleId == NAME_None)
	{
		Problems.Add(FString::Printf(TEXT("VehicleId '%s' must be 1..64 characters of a-z, 0-9 and _"),
			*Seat.VehicleId.ToString()));
	}
	if (!IsValidVehicleSeatRole(Seat.SeatRole))
	{
		Problems.Add(FString::Printf(TEXT("SeatRole must be Driver (the only implemented seat role; gunner and passenger roles of multi-seat layouts are not supported; got enum value %d)"),
			static_cast<int32>(Seat.SeatRole)));
	}
	if (!IsBoundedVector(Seat.SeatLocationCm, MaxVehicleBodyOffsetCm))
	{
		Problems.Add(FString::Printf(TEXT("SeatLocationCm must be a finite vector with every axis within +/-%f cm (got %s)"),
			MaxVehicleBodyOffsetCm, *Seat.SeatLocationCm.ToString()));
	}
	if (!IsBoundedVector(Seat.ExitOffsetCm, MaxVehicleBodyOffsetCm))
	{
		Problems.Add(FString::Printf(TEXT("ExitOffsetCm must be a finite vector with every axis within +/-%f cm (got %s)"),
			MaxVehicleBodyOffsetCm, *Seat.ExitOffsetCm.ToString()));
	}

	return FinishDefinitionValidation(Problems, OutErrors);
}

/**
 * Validates a single vehicle turret mount in isolation. Rules (M5-037):
 * MountId, VehicleId and TurretWeaponId present (a mount without a weapon is
 * refused here; whether the weapon id resolves is the cross-table gate),
 * pivot offset finite and bounded, pitch clamp finite inside
 * +/-MaxTurretPitchDegrees with MinPitchDegrees <= MaxPitchDegrees.
 */
inline bool ValidateVehicleMountDefinition(const FVehicleMountDefinition& Mount, FString& OutErrors)
{
	TArray<FString> Problems;

	if (Mount.MountId == NAME_None)
	{
		Problems.Add(FString::Printf(TEXT("MountId '%s' must be 1..64 characters of a-z, 0-9 and _"),
			*Mount.MountId.ToString()));
	}
	if (Mount.VehicleId == NAME_None)
	{
		Problems.Add(FString::Printf(TEXT("VehicleId '%s' must be 1..64 characters of a-z, 0-9 and _"),
			*Mount.VehicleId.ToString()));
	}
	if (Mount.TurretWeaponId == NAME_None)
	{
		Problems.Add(FString::Printf(TEXT("TurretWeaponId '%s' must be a non-empty weapons id (a turret mount without a weapon is refused)"),
			*Mount.TurretWeaponId.ToString()));
	}
	if (!IsBoundedVector(Mount.LocationCm, MaxVehicleBodyOffsetCm))
	{
		Problems.Add(FString::Printf(TEXT("LocationCm must be a finite vector with every axis within +/-%f cm (got %s)"),
			MaxVehicleBodyOffsetCm, *Mount.LocationCm.ToString()));
	}

	// Pitch clamp: an unbounded or inverted clamp would let the turret flip
	// through the hull; non-finite angles poison every rotation query.
	if (!FMath::IsFinite(Mount.MinPitchDegrees) || FMath::Abs(Mount.MinPitchDegrees) > MaxTurretPitchDegrees)
	{
		Problems.Add(FString::Printf(TEXT("MinPitchDegrees must be a finite value within +/-%f degrees (got %s)"),
			MaxTurretPitchDegrees, *FString::SanitizeFloat(Mount.MinPitchDegrees)));
	}
	if (!FMath::IsFinite(Mount.MaxPitchDegrees) || FMath::Abs(Mount.MaxPitchDegrees) > MaxTurretPitchDegrees)
	{
		Problems.Add(FString::Printf(TEXT("MaxPitchDegrees must be a finite value within +/-%f degrees (got %s)"),
			MaxTurretPitchDegrees, *FString::SanitizeFloat(Mount.MaxPitchDegrees)));
	}
	if (FMath::IsFinite(Mount.MinPitchDegrees) && FMath::IsFinite(Mount.MaxPitchDegrees) && Mount.MinPitchDegrees > Mount.MaxPitchDegrees)
	{
		Problems.Add(FString::Printf(TEXT("MinPitchDegrees must not exceed MaxPitchDegrees (got %s > %s)"),
			*FString::SanitizeFloat(Mount.MinPitchDegrees), *FString::SanitizeFloat(Mount.MaxPitchDegrees)));
	}

	return FinishDefinitionValidation(Problems, OutErrors);
}

/**
 * Validates a single vehicle spawn entry in isolation. Rules (M5-037):
 * VehicleId present (whether it resolves is the cross-table gate), the
 * room-local location finite with |X|/|Y| inside
 * MaxVehicleSpawnHorizontalCm and Z inside 0..MaxVehicleSpawnHeightCm (a
 * spawn below the floor or absurdly remote is refused).
 */
inline bool ValidateVehicleSpawnEntry(const FVehicleSpawnEntry& Spawn, FString& OutErrors)
{
	TArray<FString> Problems;

	if (Spawn.VehicleId == NAME_None)
	{
		Problems.Add(FString::Printf(TEXT("VehicleId '%s' must be 1..64 characters of a-z, 0-9 and _"),
			*Spawn.VehicleId.ToString()));
	}
	if (!IsFiniteVector(Spawn.LocationCm))
	{
		Problems.Add(TEXT("LocationCm must be a finite vector"));
	}
	else
	{
		if (FMath::Abs(Spawn.LocationCm.X) > MaxVehicleSpawnHorizontalCm || FMath::Abs(Spawn.LocationCm.Y) > MaxVehicleSpawnHorizontalCm)
		{
			Problems.Add(FString::Printf(TEXT("LocationCm.X/Y must stay within +/-%f cm (got X=%s, Y=%s)"),
				MaxVehicleSpawnHorizontalCm,
				*FString::SanitizeFloat(Spawn.LocationCm.X), *FString::SanitizeFloat(Spawn.LocationCm.Y)));
		}
		if (Spawn.LocationCm.Z < 0.0f || Spawn.LocationCm.Z > MaxVehicleSpawnHeightCm)
		{
			Problems.Add(FString::Printf(TEXT("LocationCm.Z must be a finite value in 0..%f cm (got %s; an underground spawn is refused)"),
				MaxVehicleSpawnHeightCm, *FString::SanitizeFloat(Spawn.LocationCm.Z)));
		}
	}

	return FinishDefinitionValidation(Problems, OutErrors);
}
