#pragma once

#include "CoreMinimal.h"

#include "Engine/DataAsset.h"

#include "../System/DamageTypes.h"
#include "../System/ReactionTypes.h"
#include "../../Weapons/WeaponTypes.h"
#include "../../Weapons/AmmoTypes.h"
#include "../../Projectiles/ProjectileTypes.h"
#include "../../Vehicles/VehicleTypes.h"

#include "CombatCatalog.generated.h"

/**
 * M5-007: the read-only combat configuration catalog (M5 interface contract
 * section 1, owner 007; work order places it under Combat/Data/). The catalog
 * aggregates the frozen configuration value types of M5-003/004 plus the
 * vehicle segment of M5-037 and gives one immutable, World-free lookup
 * surface over them:
 *
 * - BuildFromParsed constructs a catalog from already-validated candidate
 *   rows. "Parsed" means the output side of the source-table parsing
 *   (M5-005/006/037): this card builds from legal value data and deliberately
 *   re-validates nothing except its own lookup integrity (empty ids, duplicate
 *   ids, positive schema version). Duplicate ids refuse the whole build with
 *   an error naming the table and the id - there is no last-row-wins and no
 *   silent overwrite.
 * - A failed build publishes nothing: the target catalog is only assigned
 *   after every table was accepted, so a refused build leaves the target (and
 *   any previously built catalog) completely unchanged.
 * - The Find* functions return const pointers into catalog-owned stable
 *   storage, or nullptr for unknown ids. There is no fallback definition and
 *   no default weapon: a missing id must fail loudly at the call site, never
 *   silently reroute to training equipment.
 * - GetConfigRevision returns a deterministic content signature (lowercase
 *   hex) over the schema version and the normalized rows, computed row-order
 *   independently: the same rows in a different order keep the revision, any
 *   value change, added/removed row or schema version change alters it. The
 *   revision lives only on built catalogs; a default-constructed catalog
 *   reports an empty string.
 * - There are no setters and no World/Actor/timer dependencies of any kind:
 *   the catalog is a plain immutable value container that downstream owners
 *   (011 damage resolution, 012 unified entry, 020A item/drop adapters)
 *   read concurrently on the game thread.
 *
 * M5-038: the vehicle segment joins the same surface. FParsedCombatConfig
 * carries the vehicles/vehicle_seats/vehicle_mounts candidate rows (the 037
 * value types), BuildFromParsed indexes them with the identical fill/refuse
 * semantics and the revision digest hashes them in fixed table order after
 * the projectile table - so the catalog revision now mirrors the whole
 * ten-table source directory. A config that carries no vehicle rows (every
 * pre-038 consumer copies the six A/B tables only) keeps indexing empty
 * vehicle maps: existing six-table consumers stay source-compatible, their
 * revision value changes once with the extended digest stream and must be
 * re-stamped by one regeneration run (create_combat_system_assets.py).
 *
 * Not in this card's surface: item/drop adapters and TestRoom reward/spawn
 * definitions (they need the normalized item source from M5-005) and the
 * target_filters/presentations tables (no frozen value type exists yet).
 */

/**
 * Candidate rows the catalog is built from - the parsed output side of the
 * source tables under Data/CombatSystem (one array per table, every row
 * already validated by M5-005/006/037). The parser owner fills this struct;
 * the catalog owns no parsing, no JSON and no file access.
 */
struct FParsedCombatConfig
{
	/** Source schema version stamped by the tables; must be positive. */
	int32 SchemaVersion = 0;

	/** Candidate rows of damage_profiles.json. */
	TArray<FDamageProfile> DamageProfiles;

	/** Candidate rows of attack_reactions.json. */
	TArray<FAttackReaction> AttackReactions;

	/** Candidate rows of target_reactions.json. */
	TArray<FTargetReaction> TargetReactions;

	/** Candidate rows of weapons.json. */
	TArray<FWeaponDefinition> Weapons;

	/** Candidate rows of ammo_types.json. */
	TArray<FAmmoType> AmmoTypes;

	/** Candidate rows of projectiles.json. */
	TArray<FProjectileDefinition> Projectiles;

	/** Candidate rows of vehicles.json (M5-037/038). */
	TArray<FVehicleDefinition> Vehicles;

	/** Candidate rows of vehicle_seats.json (M5-037/038). */
	TArray<FVehicleSeatDefinition> VehicleSeats;

	/** Candidate rows of vehicle_mounts.json (M5-037/038). */
	TArray<FVehicleMountDefinition> VehicleMounts;
};

// ===========================================================================
// M5-038: vehicle segment data assets
// ===========================================================================

/**
 * One vehicle definition row of Data/CombatSystem/vehicles.json as a UE
 * asset. Pure data mirror of the frozen FVehicleDefinition value type
 * (Vehicles/VehicleTypes.h), in the UCombatDamageProfileAsset style of the
 * M5-008 builder: every source field is mirrored, the closed enum values are
 * stored as their numeric value (uint8) because the vehicle value types are
 * deliberately non-reflected plain enums, and the asset carries the
 * ConfigRevision of the config it was generated from.
 *
 * These three classes live next to the catalog (not in
 * CombatDataAssetBuilder.h) because M5-038's file scope owns the catalog
 * files and not the builder; the one-header-per-value-family-with-its-asset-
 * mirrors shape follows the builder header itself (six asset UCLASSes plus
 * the plain builder class in one header). The catalog class below stays a
 * plain value container - no UObject state, no World.
 *
 * MeshPath mirrors the source row's FString soft object path verbatim. The
 * hull mesh asset does not exist in this baseline, so there is no typed
 * TSoftObjectPtr here yet: guessing the mesh class (static vs skeletal)
 * would silently refuse the real asset when it arrives. Introducing the
 * typed cook reference is a deliberate follow-up once the mesh home lands.
 */
UCLASS(BlueprintType)
class UCombatVehicleAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Business id of the source row (lowercase a-z, 0-9 and _). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Identity")
	FName VehicleId;

	/** Numeric EVehicleDriveType value: 0 = Ground (the only implemented value). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Identity")
	uint8 DriveType = 0;

	/** Hull health in health points of the shared HealthComponent pool. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Vehicle")
	float MaxHealth = 0.0f;

	/** Hull defense in defense points of the shared damage formula. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Vehicle")
	float Defense = 0.0f;

	/** Straight-line top speed on the ground plane in cm/s. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Vehicle")
	float MaxSpeedCmS = 0.0f;

	/** Throttle acceleration in cm/s^2. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Vehicle")
	float AccelerationCmS2 = 0.0f;

	/** Soft object path of the hull mesh (a /Game/... path, non-empty). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Vehicle")
	FString MeshPath;

	/** Follow-camera boom length in centimeters from the vehicle origin. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Camera")
	float CameraDistanceCm = 0.0f;

	/** Follow-camera height in centimeters above the vehicle origin. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Camera")
	float CameraHeightCm = 0.0f;

	/** Follow-camera pitch in degrees (negative looks down). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Camera")
	float CameraPitchDeg = 0.0f;

	/** Deterministic content revision of the config that generated this asset. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Provenance")
	FString ConfigRevision;
};

/**
 * One vehicle seat row of vehicle_seats.json as a UE asset. Mirror of the
 * frozen FVehicleSeatDefinition (Vehicles/VehicleTypes.h); the seat and exit
 * offsets are centimeter vectors relative to the vehicle origin.
 */
UCLASS(BlueprintType)
class UCombatVehicleSeatAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Business id of the source row. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Identity")
	FName SeatId;

	/** The vehicles-table id this seat is installed in. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Identity")
	FName VehicleId;

	/** Numeric EVehicleSeatRole value: 0 = Driver (the only implemented value). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Identity")
	uint8 SeatRole = 0;

	/** Occupant seat offset in centimeters relative to the vehicle origin. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Seat")
	FVector SeatLocationCm = FVector::ZeroVector;

	/** Exit offset in centimeters: where the occupant drops when leaving. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Seat")
	FVector ExitOffsetCm = FVector::ZeroVector;

	/** Deterministic content revision of the config that generated this asset. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Provenance")
	FString ConfigRevision;
};

/**
 * One vehicle turret mount row of vehicle_mounts.json as a UE asset. Mirror
 * of the frozen FVehicleMountDefinition (Vehicles/VehicleTypes.h); the pivot
 * is a centimeter vector relative to the vehicle origin and the pitch clamp
 * is in degrees with MinPitchDegrees <= MaxPitchDegrees.
 */
UCLASS(BlueprintType)
class UCombatVehicleMountAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Business id of the source row. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Identity")
	FName MountId;

	/** The vehicles-table id this mount is installed on. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Identity")
	FName VehicleId;

	/** The weapons-table id the turret fires. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Identity")
	FName TurretWeaponId;

	/** Turret pivot offset in centimeters relative to the vehicle origin. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Mount")
	FVector LocationCm = FVector::ZeroVector;

	/** Turret pitch clamp lower bound in degrees. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Mount")
	float MinPitchDegrees = 0.0f;

	/** Turret pitch clamp upper bound in degrees. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Mount")
	float MaxPitchDegrees = 0.0f;

	/** Deterministic content revision of the config that generated this asset. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Provenance")
	FString ConfigRevision;
};

// ===========================================================================
// The catalog
// ===========================================================================

/**
 * The immutable combat configuration catalog. Build once via
 * FCombatCatalog::BuildFromParsed, then read-only for the whole session; the
 * catalog is only swapped by building a fresh instance in a new session, never
 * by mutating a live one.
 */
class FCombatCatalog
{
public:
	/**
	 * Builds a catalog from validated candidate rows. Returns true and fills
	 * OutCatalog only when every table was accepted; on failure returns false
	 * with OutErrors naming the offending table (and id where applicable),
	 * and OutCatalog is left completely untouched. Refused without building:
	 * a non-positive SchemaVersion, an empty (NAME_None) row id and a
	 * duplicate id inside any of the nine tables (six A/B plus the three
	 * vehicle tables, M5-038).
	 */
	static bool BuildFromParsed(const FParsedCombatConfig& Parsed, FCombatCatalog& OutCatalog, FString& OutErrors);

	/** The damage profile with this id, or nullptr when the id is unknown. */
	const FDamageProfile* FindDamageProfile(FName Id) const;

	/** The attack reaction with this id, or nullptr when the id is unknown. */
	const FAttackReaction* FindAttackReaction(FName Id) const;

	/** The target reaction policy with this id, or nullptr when unknown. */
	const FTargetReaction* FindTargetReaction(FName Id) const;

	/** The weapon definition with this id, or nullptr when the id is unknown. */
	const FWeaponDefinition* FindWeapon(FName Id) const;

	/** The ammo type with this id, or nullptr when the id is unknown. */
	const FAmmoType* FindAmmoType(FName Id) const;

	/** The projectile definition with this id, or nullptr when the id is unknown. */
	const FProjectileDefinition* FindProjectile(FName Id) const;

	/** The vehicle definition with this id, or nullptr when the id is unknown (M5-038). */
	const FVehicleDefinition* FindVehicle(FName Id) const;

	/** The vehicle seat with this id, or nullptr when the id is unknown (M5-038). */
	const FVehicleSeatDefinition* FindVehicleSeat(FName Id) const;

	/** The vehicle turret mount with this id, or nullptr when the id is unknown (M5-038). */
	const FVehicleMountDefinition* FindVehicleMount(FName Id) const;

	/**
	 * Deterministic content signature (16 lowercase hex characters) over the
	 * schema version and the normalized rows of all nine tables (six A/B plus
	 * the three vehicle tables in fixed order after the projectiles, M5-038).
	 * Empty only on a never-built catalog. Stable across processes for the
	 * same content; suitable for save-data provenance and event tracing
	 * (contract section 6).
	 */
	FString GetConfigRevision() const;

	/** Number of stored damage profiles. */
	int32 NumDamageProfiles() const;

	/** Number of stored attack reactions. */
	int32 NumAttackReactions() const;

	/** Number of stored target reaction policies. */
	int32 NumTargetReactions() const;

	/** Number of stored weapon definitions. */
	int32 NumWeapons() const;

	/** Number of stored ammo types. */
	int32 NumAmmoTypes() const;

	/** Number of stored projectile definitions. */
	int32 NumProjectiles() const;

	/** Number of stored vehicle definitions (M5-038). */
	int32 NumVehicles() const;

	/** Number of stored vehicle seats (M5-038). */
	int32 NumVehicleSeats() const;

	/** Number of stored vehicle turret mounts (M5-038). */
	int32 NumVehicleMounts() const;

private:
	/** Computes the revision digest of a fully filled catalog. */
	static FString ComputeConfigRevision(int32 SchemaVersion, const FCombatCatalog& Catalog);

	TMap<FName, FDamageProfile> DamageProfiles;
	TMap<FName, FAttackReaction> AttackReactions;
	TMap<FName, FTargetReaction> TargetReactions;
	TMap<FName, FWeaponDefinition> Weapons;
	TMap<FName, FAmmoType> AmmoTypes;
	TMap<FName, FProjectileDefinition> Projectiles;
	TMap<FName, FVehicleDefinition> Vehicles;
	TMap<FName, FVehicleSeatDefinition> VehicleSeats;
	TMap<FName, FVehicleMountDefinition> VehicleMounts;

	FString ConfigRevision;
};
