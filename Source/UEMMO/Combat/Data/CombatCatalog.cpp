#include "CombatCatalog.h"

/**
 * M5-007 implementation notes (extended by M5-038):
 *
 * - The catalog never re-validates row values (M5-005/006/037 own that); it
 *   only guards what would break its own lookup surface: a positive schema
 *   version, non-empty ids and one definition per id per table.
 * - Revision digest: FNV-1a over a canonical byte stream - a domain tag, the
 *   schema version, then per table (fixed table order, rows sorted by id
 *   text) the row count and every field in a fixed order. Floats hash their
 *   IEEE-754 bits with negative zero normalized to positive zero, so the
 *   digest is value-based, not text-based: reordering rows or re-encoding a
 *   number keeps the revision, changing any value changes it. The M5-038
 *   vehicle tables hash in fixed order after the projectile table; their
 *   centimeter offsets are FVector (double) components and hash their
 *   IEEE-754 double bits through the same negative-zero normalization.
 * - Nothing here touches a World, a timer, an asset or a file; the whole
 *   class is plain value container code. The vehicle data asset UCLASSes
 *   declared in the header are inert mirrors (M5-038); the catalog class
 *   itself never references them.
 */

namespace
{
	// FNV-1a 64-bit constants.
	constexpr uint64 RevisionHashOffsetBasis = 14695981039346656037ull;
	constexpr uint64 RevisionHashPrime = 1099511628211ull;

	uint64 RevisionHashBytes(uint64 Hash, const uint8* Data, int64 Size)
	{
		for (int64 Index = 0; Index < Size; ++Index)
		{
			Hash ^= static_cast<uint64>(Data[Index]);
			Hash *= RevisionHashPrime;
		}
		return Hash;
	}

	/** Hashes a string's raw character storage; display text only feeds the digest, it is never parsed back. */
	uint64 RevisionHashText(uint64 Hash, const FString& Text)
	{
		return RevisionHashBytes(Hash, reinterpret_cast<const uint8*>(*Text),
			static_cast<int64>(Text.Len()) * static_cast<int64>(sizeof(TCHAR)));
	}

	uint64 RevisionHashInt(uint64 Hash, int32 Value)
	{
		return RevisionHashBytes(Hash, reinterpret_cast<const uint8*>(&Value), sizeof(Value));
	}

	uint64 RevisionHashBool(uint64 Hash, bool Value)
	{
		const uint8 Byte = Value ? 1u : 0u;
		return RevisionHashBytes(Hash, &Byte, sizeof(Byte));
	}

	/**
	 * Hashes a float's IEEE-754 bits. Negative zero normalizes to positive
	 * zero so value-equal inputs hash equal; NaN cannot appear here because
	 * the upstream validators refuse non-finite values.
	 */
	uint64 RevisionHashFloat(uint64 Hash, float Value)
	{
		if (Value == 0.0f)
		{
			Value = 0.0f; // collapse -0.0 onto +0.0
		}
		static_assert(sizeof(float) == sizeof(uint32), "the revision digest assumes 32-bit IEEE-754 floats");
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		return RevisionHashBytes(Hash, reinterpret_cast<const uint8*>(&Bits), sizeof(Bits));
	}

	/**
	 * Hashes a double's IEEE-754 bits with the same negative-zero
	 * normalization as the float variant. The vehicle segment's centimeter
	 * offsets are FVector (double) components, so the digest keeps
	 * distinguishing value changes that a float truncation would swallow.
	 */
	uint64 RevisionHashDouble(uint64 Hash, double Value)
	{
		if (Value == 0.0)
		{
			Value = 0.0; // collapse -0.0 onto +0.0
		}
		static_assert(sizeof(double) == sizeof(uint64), "the revision digest assumes 64-bit IEEE-754 doubles");
		uint64 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
		return RevisionHashBytes(Hash, reinterpret_cast<const uint8*>(&Bits), sizeof(Bits));
	}

	/** Hashes a vector's X, Y, Z components in that fixed order. */
	uint64 RevisionHashFVector(uint64 Hash, const FVector& Value)
	{
		Hash = RevisionHashDouble(Hash, static_cast<double>(Value.X));
		Hash = RevisionHashDouble(Hash, static_cast<double>(Value.Y));
		Hash = RevisionHashDouble(Hash, static_cast<double>(Value.Z));
		return Hash;
	}

	uint64 RevisionHashFName(uint64 Hash, FName Value)
	{
		return RevisionHashText(Hash, Value.ToString());
	}

	uint64 RevisionHashFloatArray(uint64 Hash, const TArray<float>& Values)
	{
		Hash = RevisionHashInt(Hash, Values.Num());
		for (const float Value : Values)
		{
			Hash = RevisionHashFloat(Hash, Value);
		}
		return Hash;
	}

	uint64 RevisionHashNameArray(uint64 Hash, const TArray<FName>& Values)
	{
		Hash = RevisionHashInt(Hash, Values.Num());
		for (const FName Value : Values)
		{
			Hash = RevisionHashFName(Hash, Value);
		}
		return Hash;
	}

	uint64 RevisionHashDamageProfile(uint64 Hash, const FDamageProfile& Profile)
	{
		Hash = RevisionHashFName(Hash, Profile.DamageProfileId);
		Hash = RevisionHashFloat(Hash, Profile.BaseDamage);
		Hash = RevisionHashFloat(Hash, Profile.AttackCoefficient);
		Hash = RevisionHashFloat(Hash, Profile.HitStunSeconds);
		Hash = RevisionHashFloat(Hash, Profile.KnockbackCmPerSecond);
		Hash = RevisionHashFloat(Hash, Profile.LaunchCmPerSecond);
		Hash = RevisionHashFloat(Hash, Profile.HitStopSeconds);
		return Hash;
	}

	uint64 RevisionHashAttackReaction(uint64 Hash, const FAttackReaction& Reaction)
	{
		Hash = RevisionHashFName(Hash, Reaction.ReactionId);
		Hash = RevisionHashInt(Hash, static_cast<int32>(Reaction.ControlPenetration));
		return Hash;
	}

	uint64 RevisionHashTargetReaction(uint64 Hash, const FTargetReaction& Policy)
	{
		Hash = RevisionHashFName(Hash, Policy.PolicyId);
		Hash = RevisionHashBool(Hash, Policy.bAllowStagger);
		Hash = RevisionHashBool(Hash, Policy.bAllowLaunch);
		Hash = RevisionHashBool(Hash, Policy.bAllowKnockdown);
		Hash = RevisionHashInt(Hash, Policy.MaxLaunchesPerAirCycle);
		Hash = RevisionHashFloatArray(Hash, Policy.LaunchZScales);
		Hash = RevisionHashFloat(Hash, Policy.MaxAirTimeSeconds);
		Hash = RevisionHashFloat(Hash, Policy.PoiseMax);
		Hash = RevisionHashFloat(Hash, Policy.PoiseRegenSeconds);
		Hash = RevisionHashFloat(Hash, Policy.KnockdownSeconds);
		Hash = RevisionHashFloat(Hash, Policy.RecoveringSeconds);
		Hash = RevisionHashBool(Hash, Policy.bImmuneDamage);
		Hash = RevisionHashBool(Hash, Policy.bImmuneControl);
		Hash = RevisionHashBool(Hash, Policy.bDeathResistant);
		return Hash;
	}

	uint64 RevisionHashWeapon(uint64 Hash, const FWeaponDefinition& Weapon)
	{
		Hash = RevisionHashFName(Hash, Weapon.WeaponId);
		Hash = RevisionHashInt(Hash, static_cast<int32>(Weapon.FireMode));
		Hash = RevisionHashFName(Hash, Weapon.DamageProfileId);
		Hash = RevisionHashFName(Hash, Weapon.AmmoId);
		Hash = RevisionHashInt(Hash, Weapon.MagazineSize);
		Hash = RevisionHashFloat(Hash, Weapon.FireRateRpm);
		Hash = RevisionHashInt(Hash, Weapon.BurstCount);
		Hash = RevisionHashInt(Hash, Weapon.PelletCount);
		Hash = RevisionHashFloat(Hash, Weapon.SpreadDegrees);
		Hash = RevisionHashFloat(Hash, Weapon.RangeCm);
		Hash = RevisionHashFName(Hash, Weapon.ProjectileId);
		Hash = RevisionHashNameArray(Hash, Weapon.MeleeAttackIds);
		return Hash;
	}

	uint64 RevisionHashAmmoType(uint64 Hash, const FAmmoType& Ammo)
	{
		Hash = RevisionHashFName(Hash, Ammo.AmmoId);
		Hash = RevisionHashInt(Hash, Ammo.MaxReserve);
		Hash = RevisionHashInt(Hash, Ammo.MagazineSize);
		return Hash;
	}

	uint64 RevisionHashProjectile(uint64 Hash, const FProjectileDefinition& Projectile)
	{
		Hash = RevisionHashFName(Hash, Projectile.ProjectileId);
		Hash = RevisionHashInt(Hash, static_cast<int32>(Projectile.Motion));
		Hash = RevisionHashFloat(Hash, Projectile.SpeedCmS);
		Hash = RevisionHashFloat(Hash, Projectile.LifetimeS);
		Hash = RevisionHashFName(Hash, Projectile.DamageProfileId);
		Hash = RevisionHashInt(Hash, Projectile.PierceCount);
		Hash = RevisionHashFloat(Hash, Projectile.ExplosionRadiusCm);
		Hash = RevisionHashFName(Hash, Projectile.ExplosionDamageProfileId);
		Hash = RevisionHashFloat(Hash, Projectile.HomingTurnRateDegS);
		return Hash;
	}

	/** M5-038: every field of one vehicle definition in a fixed order. */
	uint64 RevisionHashVehicle(uint64 Hash, const FVehicleDefinition& Vehicle)
	{
		Hash = RevisionHashFName(Hash, Vehicle.VehicleId);
		Hash = RevisionHashInt(Hash, static_cast<int32>(Vehicle.DriveType));
		Hash = RevisionHashFloat(Hash, Vehicle.MaxHealth);
		Hash = RevisionHashFloat(Hash, Vehicle.Defense);
		Hash = RevisionHashFloat(Hash, Vehicle.MaxSpeedCmS);
		Hash = RevisionHashFloat(Hash, Vehicle.AccelerationCmS2);
		Hash = RevisionHashText(Hash, Vehicle.MeshPath);
		Hash = RevisionHashFloat(Hash, Vehicle.CameraDistanceCm);
		Hash = RevisionHashFloat(Hash, Vehicle.CameraHeightCm);
		Hash = RevisionHashFloat(Hash, Vehicle.CameraPitchDeg);
		return Hash;
	}

	/** M5-038: every field of one vehicle seat in a fixed order. */
	uint64 RevisionHashVehicleSeat(uint64 Hash, const FVehicleSeatDefinition& Seat)
	{
		Hash = RevisionHashFName(Hash, Seat.SeatId);
		Hash = RevisionHashFName(Hash, Seat.VehicleId);
		Hash = RevisionHashInt(Hash, static_cast<int32>(Seat.SeatRole));
		Hash = RevisionHashFVector(Hash, Seat.SeatLocationCm);
		Hash = RevisionHashFVector(Hash, Seat.ExitOffsetCm);
		return Hash;
	}

	/** M5-038: every field of one vehicle turret mount in a fixed order. */
	uint64 RevisionHashVehicleMount(uint64 Hash, const FVehicleMountDefinition& Mount)
	{
		Hash = RevisionHashFName(Hash, Mount.MountId);
		Hash = RevisionHashFName(Hash, Mount.VehicleId);
		Hash = RevisionHashFName(Hash, Mount.TurretWeaponId);
		Hash = RevisionHashFVector(Hash, Mount.LocationCm);
		Hash = RevisionHashFloat(Hash, Mount.MinPitchDegrees);
		Hash = RevisionHashFloat(Hash, Mount.MaxPitchDegrees);
		return Hash;
	}

	/**
	 * Fills one catalog table from candidate rows, refusing empty ids and
	 * duplicate ids with an error naming the table, the id and the row
	 * index. A refused table leaves OutMap unchanged (never half-filled).
	 */
	template <typename RowType>
	bool FillCatalogTable(const TArray<RowType>& Rows, const TCHAR* TableName, FName RowType::* IdMember,
		TMap<FName, RowType>& OutMap, FString& OutErrors)
	{
		for (int32 Index = 0; Index < Rows.Num(); ++Index)
		{
			const FName RowId = Rows[Index].*IdMember;
			if (RowId.IsNone())
			{
				OutErrors = FString::Printf(TEXT("%s: row %d has an empty id"), TableName, Index);
				return false;
			}
			if (OutMap.Contains(RowId))
			{
				OutErrors = FString::Printf(TEXT("%s: duplicate id '%s' (row %d)"), TableName, *RowId.ToString(), Index);
				return false;
			}
			OutMap.Add(RowId, Rows[Index]);
		}
		return true;
	}

	/**
	 * Collects a table's rows sorted by id text so the revision digest is
	 * independent of the incoming row order.
	 */
	template <typename RowType>
	TArray<const RowType*> SortedRowsById(const TMap<FName, RowType>& Map, FName RowType::* IdMember)
	{
		TArray<const RowType*> Rows;
		Rows.Reserve(Map.Num());
		for (const TPair<FName, RowType>& Pair : Map)
		{
			Rows.Add(&Pair.Value);
		}
		Rows.Sort([IdMember](const RowType& Left, const RowType& Right)
		{
			return (Left.*IdMember).LexicalLess(Right.*IdMember);
		});
		return Rows;
	}
}

bool FCombatCatalog::BuildFromParsed(const FParsedCombatConfig& Parsed, FCombatCatalog& OutCatalog, FString& OutErrors)
{
	OutErrors.Reset();

	// The schema version stamps the digest and the parsed shape; a missing
	// one means the caller did not receive validated parser output.
	if (Parsed.SchemaVersion <= 0)
	{
		OutErrors = FString::Printf(TEXT("schema_version must be a positive integer (got %d)"), Parsed.SchemaVersion);
		return false;
	}

	FCombatCatalog Built;
	if (!FillCatalogTable(Parsed.DamageProfiles, TEXT("damage_profiles"), &FDamageProfile::DamageProfileId, Built.DamageProfiles, OutErrors) ||
		!FillCatalogTable(Parsed.AttackReactions, TEXT("attack_reactions"), &FAttackReaction::ReactionId, Built.AttackReactions, OutErrors) ||
		!FillCatalogTable(Parsed.TargetReactions, TEXT("target_reactions"), &FTargetReaction::PolicyId, Built.TargetReactions, OutErrors) ||
		!FillCatalogTable(Parsed.Weapons, TEXT("weapons"), &FWeaponDefinition::WeaponId, Built.Weapons, OutErrors) ||
		!FillCatalogTable(Parsed.AmmoTypes, TEXT("ammo_types"), &FAmmoType::AmmoId, Built.AmmoTypes, OutErrors) ||
		!FillCatalogTable(Parsed.Projectiles, TEXT("projectiles"), &FProjectileDefinition::ProjectileId, Built.Projectiles, OutErrors) ||
		!FillCatalogTable(Parsed.Vehicles, TEXT("vehicles"), &FVehicleDefinition::VehicleId, Built.Vehicles, OutErrors) ||
		!FillCatalogTable(Parsed.VehicleSeats, TEXT("vehicle_seats"), &FVehicleSeatDefinition::SeatId, Built.VehicleSeats, OutErrors) ||
		!FillCatalogTable(Parsed.VehicleMounts, TEXT("vehicle_mounts"), &FVehicleMountDefinition::MountId, Built.VehicleMounts, OutErrors))
	{
		return false; // OutCatalog untouched: nothing was published
	}

	Built.ConfigRevision = ComputeConfigRevision(Parsed.SchemaVersion, Built);
	OutCatalog = MoveTemp(Built);
	return true;
}

const FDamageProfile* FCombatCatalog::FindDamageProfile(FName Id) const
{
	return DamageProfiles.Find(Id);
}

const FAttackReaction* FCombatCatalog::FindAttackReaction(FName Id) const
{
	return AttackReactions.Find(Id);
}

const FTargetReaction* FCombatCatalog::FindTargetReaction(FName Id) const
{
	return TargetReactions.Find(Id);
}

const FWeaponDefinition* FCombatCatalog::FindWeapon(FName Id) const
{
	return Weapons.Find(Id);
}

const FAmmoType* FCombatCatalog::FindAmmoType(FName Id) const
{
	return AmmoTypes.Find(Id);
}

const FProjectileDefinition* FCombatCatalog::FindProjectile(FName Id) const
{
	return Projectiles.Find(Id);
}

const FVehicleDefinition* FCombatCatalog::FindVehicle(FName Id) const
{
	return Vehicles.Find(Id);
}

const FVehicleSeatDefinition* FCombatCatalog::FindVehicleSeat(FName Id) const
{
	return VehicleSeats.Find(Id);
}

const FVehicleMountDefinition* FCombatCatalog::FindVehicleMount(FName Id) const
{
	return VehicleMounts.Find(Id);
}

FString FCombatCatalog::GetConfigRevision() const
{
	return ConfigRevision;
}

int32 FCombatCatalog::NumDamageProfiles() const
{
	return DamageProfiles.Num();
}

int32 FCombatCatalog::NumAttackReactions() const
{
	return AttackReactions.Num();
}

int32 FCombatCatalog::NumTargetReactions() const
{
	return TargetReactions.Num();
}

int32 FCombatCatalog::NumWeapons() const
{
	return Weapons.Num();
}

int32 FCombatCatalog::NumAmmoTypes() const
{
	return AmmoTypes.Num();
}

int32 FCombatCatalog::NumProjectiles() const
{
	return Projectiles.Num();
}

int32 FCombatCatalog::NumVehicles() const
{
	return Vehicles.Num();
}

int32 FCombatCatalog::NumVehicleSeats() const
{
	return VehicleSeats.Num();
}

int32 FCombatCatalog::NumVehicleMounts() const
{
	return VehicleMounts.Num();
}

FString FCombatCatalog::ComputeConfigRevision(int32 SchemaVersion, const FCombatCatalog& Catalog)
{
	// Domain tag keeps digests from other subsystems (or later catalog format
	// changes) from ever colliding with this one.
	uint64 Hash = RevisionHashText(RevisionHashOffsetBasis, TEXT("UEMMO.M5.CombatCatalog.v1"));
	Hash = RevisionHashInt(Hash, SchemaVersion);

	// Fixed table order; rows sorted by id so the incoming row order never
	// reaches the digest.
	for (const FDamageProfile* Row : SortedRowsById(Catalog.DamageProfiles, &FDamageProfile::DamageProfileId))
	{
		Hash = RevisionHashDamageProfile(Hash, *Row);
	}
	for (const FAttackReaction* Row : SortedRowsById(Catalog.AttackReactions, &FAttackReaction::ReactionId))
	{
		Hash = RevisionHashAttackReaction(Hash, *Row);
	}
	for (const FTargetReaction* Row : SortedRowsById(Catalog.TargetReactions, &FTargetReaction::PolicyId))
	{
		Hash = RevisionHashTargetReaction(Hash, *Row);
	}
	for (const FWeaponDefinition* Row : SortedRowsById(Catalog.Weapons, &FWeaponDefinition::WeaponId))
	{
		Hash = RevisionHashWeapon(Hash, *Row);
	}
	for (const FAmmoType* Row : SortedRowsById(Catalog.AmmoTypes, &FAmmoType::AmmoId))
	{
		Hash = RevisionHashAmmoType(Hash, *Row);
	}
	for (const FProjectileDefinition* Row : SortedRowsById(Catalog.Projectiles, &FProjectileDefinition::ProjectileId))
	{
		Hash = RevisionHashProjectile(Hash, *Row);
	}
	// M5-038: the vehicle segment joins the digest in fixed order after the
	// projectiles, so the revision mirrors the whole source directory. A
	// config without vehicle rows hashes three empty tables here - the six
	// A/B consumers keep one shared revision among themselves, and a
	// vehicle-inclusive build differs from it by exactly this segment.
	for (const FVehicleDefinition* Row : SortedRowsById(Catalog.Vehicles, &FVehicleDefinition::VehicleId))
	{
		Hash = RevisionHashVehicle(Hash, *Row);
	}
	for (const FVehicleSeatDefinition* Row : SortedRowsById(Catalog.VehicleSeats, &FVehicleSeatDefinition::SeatId))
	{
		Hash = RevisionHashVehicleSeat(Hash, *Row);
	}
	for (const FVehicleMountDefinition* Row : SortedRowsById(Catalog.VehicleMounts, &FVehicleMountDefinition::MountId))
	{
		Hash = RevisionHashVehicleMount(Hash, *Row);
	}

	// Lowercase 16-digit hex, built digit by digit (no printf size-modifier
	// dependency for 64-bit values).
	static const TCHAR HexDigits[] = TEXT("0123456789abcdef");
	FString Digest;
	Digest.Reserve(16);
	for (int32 Shift = 60; Shift >= 0; Shift -= 4)
	{
		Digest.AppendChar(HexDigits[(Hash >> Shift) & 0xF]);
	}
	return Digest;
}
