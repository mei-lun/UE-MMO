#pragma once

#include "CoreMinimal.h"

/**
 * M5-004: weapon definition value type (M5 interface contract section 1,
 * first owner 004). A definition is the shared, immutable description of one
 * weapon kind; concrete owned occurrences (magazine state, reserve ammo) live
 * in the later WeaponComponent/AmmoModel layer. Plain value types on purpose:
 * validation stays pure C++ logic testable without a World; a definition never
 * references or holds a World/Actor pointer (contract section 0.5).
 *
 * Units (contract section 4: every quantity carries its unit):
 * - lengths in centimeters (RangeCm),
 * - angles in degrees (SpreadDegrees),
 * - rates in rounds per minute (FireRateRpm),
 * - the legacy four melee attacks keep their 60 Hz frame windows from
 *   combat-attacks.json; this header introduces no frame fields.
 *
 * Delivery mode vocabulary (FireMode): exactly melee, hitscan or projectile.
 * "single/automatic/burst" firing pacing of the design discussion is expressed
 * by BurstCount and FireRateRpm, not by an extra mode enum.
 *
 * Capability combinations that the first version does not support are
 * rejected by ValidateWeaponDefinition, never silently ignored:
 * - melee needs no ammo and no projectile; it also carries none of the firearm
 *   pacing/geometry fields (magazine, fire rate, burst, pellets, spread,
 *   weapon range). Melee timing and hit geometry stay owned by the legacy
 *   attack definitions.
 * - hitscan resolves hits by a swept query up to RangeCm and never spawns an
 *   actor ("the ray spawns no actor"), so it must not reference a projectile.
 * - projectile weapons must reference a projectile definition; their damage
 *   authority is FProjectileDefinition::DamageProfileId, so the weapon-level
 *   profile must stay empty to avoid two competing profiles.
 * - melee attack ids must be exactly the complete legacy four-attack set;
 *   unknown or incomplete sets are rejected at validation. Equipment changes
 *   keep the X/Z routing and cancel chain; swapping arbitrary melee attack
 *   sets is a separate future task (contract section 7).
 */

/** Delivery mode of a weapon; the three values are closed by design. */
enum class EWeaponFireMode : uint8
{
	Melee = 0,
	Hitscan = 1,
	Projectile = 2
};

/**
 * Upper bounds of the first-version weapon fields ("bounded positive
 * integers"). The bounds are sanity limits for the source tables, not tuning
 * targets; raising one is a deliberate schema change.
 */
constexpr int32 MaxWeaponPelletCount = 32;
constexpr int32 MaxWeaponBurstCount = 12;
constexpr int32 MaxWeaponMagazineSize = 999;
constexpr float MaxWeaponSpreadDegrees = 45.0f;
constexpr float MaxWeaponFireRateRpm = 3000.0f;
constexpr float MaxWeaponRangeCm = 1000000.0f;

/**
 * The complete legacy four-attack melee set (contract section 7): light_01,
 * light_02, launcher, aerial_01. Order inside the definition is not
 * load-bearing (the routing lives in the attack chain), but the set must be
 * exactly these four distinct ids.
 */
inline const TArray<FName>& GetLegacyMeleeAttackIds()
{
	static const TArray<FName> LegacyAttackIds = { TEXT("light_01"), TEXT("light_02"), TEXT("launcher"), TEXT("aerial_01") };
	return LegacyAttackIds;
}

/** Returns true only for the three closed fire-mode values. */
inline bool IsValidWeaponFireMode(EWeaponFireMode FireMode)
{
	switch (FireMode)
	{
	case EWeaponFireMode::Melee:
	case EWeaponFireMode::Hitscan:
	case EWeaponFireMode::Projectile:
		return true;
	default:
		return false;
	}
}

/**
 * Parses a fire mode from source-data text ("melee"/"hitscan"/"projectile",
 * case-insensitive). Returns false and leaves OutFireMode untouched for empty
 * or unknown text; the empty string must never map to a valid mode.
 */
inline bool ParseWeaponFireMode(const FString& Text, EWeaponFireMode& OutFireMode)
{
	if (Text.Equals(TEXT("melee"), ESearchCase::IgnoreCase))
	{
		OutFireMode = EWeaponFireMode::Melee;
		return true;
	}
	if (Text.Equals(TEXT("hitscan"), ESearchCase::IgnoreCase))
	{
		OutFireMode = EWeaponFireMode::Hitscan;
		return true;
	}
	if (Text.Equals(TEXT("projectile"), ESearchCase::IgnoreCase))
	{
		OutFireMode = EWeaponFireMode::Projectile;
		return true;
	}
	return false;
}

/**
 * Business id text rule of the M5 source tables: lowercase ASCII letters,
 * digits and underscores, 1-64 characters (design section 4). Shared text
 * form for every table owned by 004; FName equality is case-insensitive, so
 * the display string decides whether a definition may enter the catalog.
 */
inline bool IsValidDefinitionIdText(const FString& Text)
{
	if (Text.IsEmpty() || Text.Len() > 64)
	{
		return false;
	}
	for (const TCHAR Character : Text)
	{
		const bool bLowercaseLetter = Character >= TEXT('a') && Character <= TEXT('z');
		const bool bDigit = Character >= TEXT('0') && Character <= TEXT('9');
		const bool bUnderscore = Character == TEXT('_');
		if (!bLowercaseLetter && !bDigit && !bUnderscore)
		{
			return false;
		}
	}
	return true;
}

/** Immutable description of one weapon kind (interface contract section 1, owner 004). */
struct FWeaponDefinition
{
	/**
	 * Stable business identifier (e.g. "weapon_training_sword"); lowercase
	 * a-z0-9_ per IsValidDefinitionIdText, unique inside the weapons table.
	 */
	FName WeaponId;

	/** Delivery mode; must be one of the three closed enum values. */
	EWeaponFireMode FireMode = EWeaponFireMode::Melee;

	/**
	 * Damage profile applied by the weapon itself: required for melee and
	 * hitscan, must stay empty for projectile weapons (the referenced
	 * FProjectileDefinition owns the damage there).
	 */
	FName DamageProfileId;

	/** Shared reserve ammo type consumed per shot; melee must leave it empty. */
	FName AmmoId;

	/** Rounds of one magazine of this weapon instance; 0 for melee. */
	int32 MagazineSize = 0;

	/** Maximum sustained pacing in rounds per minute; 0 for melee. */
	float FireRateRpm = 0.0f;

	/** Rounds per triggered burst (point fire); 1 means single shots. */
	int32 BurstCount = 1;

	/** Pellets per shot (multi-pellet semantics); 1 means a single pellet. */
	int32 PelletCount = 1;

	/** Maximum angular deviation of one pellet/direction in degrees (0 = exact). */
	float SpreadDegrees = 0.0f;

	/**
	 * Hitscan maximum hit distance in centimeters. Meaningless for melee (the
	 * attack hit boxes own the reach) and for projectile mode (the projectile
	 * lifetime and speed own the reach), so both must keep it 0.
	 */
	float RangeCm = 0.0f;

	/** Projectile definition used per shot; required for projectile mode, empty otherwise. */
	FName ProjectileId;

	/**
	 * Melee attack ids of the legacy chain; melee weapons must carry exactly
	 * GetLegacyMeleeAttackIds() and every other mode must leave it empty.
	 */
	TArray<FName> MeleeAttackIds;
};

/**
 * Validates a single weapon definition in isolation and returns true when
 * legal. Rules (M5-004): WeaponId matches IsValidDefinitionIdText, FireMode
 * one of the three closed values, per-mode ammo/projectile/profile/range
 * requirements as documented on the struct, all physical quantities finite
 * and inside their documented bounds, melee attack ids exactly the legacy
 * four-attack set (any order, no duplicates). Cross-table reference checks
 * (ammo/projectile/profile ids resolve) belong to M5-005/006.
 * On failure OutErrors joins every problem with "; " and each problem names
 * its field (e.g. "WeaponId", "AmmoId", "MeleeAttackIds").
 */
inline bool ValidateWeaponDefinition(const FWeaponDefinition& Definition, FString& OutErrors)
{
	TArray<FString> Problems;

	// Identity: a definition without a stable lowercase id cannot be
	// referenced by items, catalogs or save data.
	if (!IsValidDefinitionIdText(Definition.WeaponId.ToString()))
	{
		Problems.Add(FString::Printf(TEXT("WeaponId must be 1-64 characters of a-z0-9_ (got '%s')"),
			*Definition.WeaponId.ToString()));
	}

	// Fire mode: only the three closed values are legal; the per-mode rule
	// blocks below compare against the three valid values, so an unknown mode
	// is reported together with every other independent problem.
	if (!IsValidWeaponFireMode(Definition.FireMode))
	{
		Problems.Add(FString::Printf(TEXT("FireMode must be one of Melee/Hitscan/Projectile (got enum value %d)"),
			static_cast<int32>(Definition.FireMode)));
	}

	// Shared quantity checks: finite, non-negative, bounded.
	if (Definition.MagazineSize < 0 || Definition.MagazineSize > MaxWeaponMagazineSize)
	{
		Problems.Add(FString::Printf(TEXT("MagazineSize must be 0..%d (got %d)"), MaxWeaponMagazineSize, Definition.MagazineSize));
	}
	if (!FMath::IsFinite(Definition.FireRateRpm) || Definition.FireRateRpm < 0.0f || Definition.FireRateRpm > MaxWeaponFireRateRpm)
	{
		Problems.Add(FString::Printf(TEXT("FireRateRpm must be a finite value in 0..%f (got %s)"),
			MaxWeaponFireRateRpm, *FString::SanitizeFloat(Definition.FireRateRpm)));
	}
	if (Definition.BurstCount < 1 || Definition.BurstCount > MaxWeaponBurstCount)
	{
		Problems.Add(FString::Printf(TEXT("BurstCount must be a positive value in 1..%d (got %d)"), MaxWeaponBurstCount, Definition.BurstCount));
	}
	if (Definition.PelletCount < 1 || Definition.PelletCount > MaxWeaponPelletCount)
	{
		Problems.Add(FString::Printf(TEXT("PelletCount must be a positive value in 1..%d (got %d)"), MaxWeaponPelletCount, Definition.PelletCount));
	}
	if (!FMath::IsFinite(Definition.SpreadDegrees) || Definition.SpreadDegrees < 0.0f || Definition.SpreadDegrees > MaxWeaponSpreadDegrees)
	{
		Problems.Add(FString::Printf(TEXT("SpreadDegrees must be a finite value in 0..%f (got %s)"),
			MaxWeaponSpreadDegrees, *FString::SanitizeFloat(Definition.SpreadDegrees)));
	}
	if (!FMath::IsFinite(Definition.RangeCm) || Definition.RangeCm < 0.0f || Definition.RangeCm > MaxWeaponRangeCm)
	{
		Problems.Add(FString::Printf(TEXT("RangeCm must be a finite value in 0..%f (got %s)"),
			MaxWeaponRangeCm, *FString::SanitizeFloat(Definition.RangeCm)));
	}

	// Melee: no ammo, no projectile, no firearm pacing or geometry; the legacy
	// four-attack set is the only legal attack id set.
	if (Definition.FireMode == EWeaponFireMode::Melee)
	{
		if (!Definition.AmmoId.IsNone())
		{
			Problems.Add(FString::Printf(TEXT("AmmoId must be empty for melee weapons (got '%s')"), *Definition.AmmoId.ToString()));
		}
		if (!Definition.ProjectileId.IsNone())
		{
			Problems.Add(FString::Printf(TEXT("ProjectileId must be empty for melee weapons (got '%s')"), *Definition.ProjectileId.ToString()));
		}
		if (Definition.MagazineSize != 0)
		{
			Problems.Add(TEXT("MagazineSize must be 0 for melee weapons"));
		}
		if (Definition.FireRateRpm != 0.0f)
		{
			Problems.Add(TEXT("FireRateRpm must be 0 for melee weapons (attack timing lives in the attack definitions)"));
		}
		if (Definition.BurstCount != 1)
		{
			Problems.Add(TEXT("BurstCount must be 1 for melee weapons"));
		}
		if (Definition.PelletCount != 1)
		{
			Problems.Add(TEXT("PelletCount must be 1 for melee weapons"));
		}
		if (Definition.SpreadDegrees != 0.0f)
		{
			Problems.Add(TEXT("SpreadDegrees must be 0 for melee weapons"));
		}
		if (Definition.RangeCm != 0.0f)
		{
			Problems.Add(TEXT("RangeCm must be 0 for melee weapons (the attack hit boxes own the reach)"));
		}
		if (Definition.DamageProfileId.IsNone())
		{
			Problems.Add(TEXT("DamageProfileId must be a non-empty identifier for melee weapons"));
		}

		const TArray<FName>& LegacySet = GetLegacyMeleeAttackIds();
		if (Definition.MeleeAttackIds.Num() != LegacySet.Num())
		{
			Problems.Add(FString::Printf(TEXT("MeleeAttackIds must be exactly the complete legacy four-attack set (got %d ids)"),
				Definition.MeleeAttackIds.Num()));
		}
		else
		{
			for (const FName LegacyId : LegacySet)
			{
				if (Definition.MeleeAttackIds.Contains(LegacyId) == false)
				{
					Problems.Add(FString::Printf(TEXT("MeleeAttackIds must be exactly the complete legacy four-attack set (missing '%s')"),
						*LegacyId.ToString()));
					break;
				}
			}
			TSet<FName> UniqueIds;
			for (const FName AttackId : Definition.MeleeAttackIds)
			{
				if (UniqueIds.Contains(AttackId))
				{
					Problems.Add(FString::Printf(TEXT("MeleeAttackIds must be exactly the complete legacy four-attack set (duplicate '%s')"),
						*AttackId.ToString()));
					break;
				}
				UniqueIds.Add(AttackId);
			}
		}
	}

	// Hitscan: needs ammo and a weapon damage profile; the ray spawns no actor
	// and never references a projectile.
	if (Definition.FireMode == EWeaponFireMode::Hitscan)
	{
		if (Definition.AmmoId.IsNone())
		{
			Problems.Add(TEXT("AmmoId must be a non-empty identifier for ranged weapons"));
		}
		if (Definition.DamageProfileId.IsNone())
		{
			Problems.Add(TEXT("DamageProfileId must be a non-empty identifier for hitscan weapons"));
		}
		if (!Definition.ProjectileId.IsNone())
		{
			Problems.Add(FString::Printf(TEXT("ProjectileId must be empty for hitscan weapons (got '%s')"),
				*Definition.ProjectileId.ToString()));
		}
		if (Definition.MagazineSize < 1)
		{
			Problems.Add(TEXT("MagazineSize must be at least 1 for ranged weapons"));
		}
		if (Definition.FireRateRpm <= 0.0f)
		{
			Problems.Add(TEXT("FireRateRpm must be positive for ranged weapons"));
		}
		if (Definition.RangeCm <= 0.0f)
		{
			Problems.Add(TEXT("RangeCm must be positive for hitscan weapons"));
		}
		if (Definition.MeleeAttackIds.Num() != 0)
		{
			Problems.Add(TEXT("MeleeAttackIds must be empty for ranged weapons"));
		}
	}

	// Projectile: needs ammo and a projectile definition; the damage and
	// range authority is the projectile definition, so the weapon keeps both
	// of its own fields empty.
	if (Definition.FireMode == EWeaponFireMode::Projectile)
	{
		if (Definition.AmmoId.IsNone())
		{
			Problems.Add(TEXT("AmmoId must be a non-empty identifier for ranged weapons"));
		}
		if (Definition.ProjectileId.IsNone())
		{
			Problems.Add(TEXT("ProjectileId must be a non-empty identifier for projectile weapons"));
		}
		if (!Definition.DamageProfileId.IsNone())
		{
			Problems.Add(FString::Printf(TEXT("DamageProfileId must be empty for projectile weapons (got '%s'; the projectile definition owns the damage profile)"),
				*Definition.DamageProfileId.ToString()));
		}
		if (Definition.MagazineSize < 1)
		{
			Problems.Add(TEXT("MagazineSize must be at least 1 for ranged weapons"));
		}
		if (Definition.FireRateRpm <= 0.0f)
		{
			Problems.Add(TEXT("FireRateRpm must be positive for ranged weapons"));
		}
		if (Definition.RangeCm != 0.0f)
		{
			Problems.Add(TEXT("RangeCm must be 0 for projectile weapons (the projectile lifetime and speed own the range)"));
		}
		if (Definition.MeleeAttackIds.Num() != 0)
		{
			Problems.Add(TEXT("MeleeAttackIds must be empty for ranged weapons"));
		}
	}

	if (Problems.Num() == 0)
	{
		OutErrors.Reset();
		return true;
	}
	OutErrors = FString::Join(Problems, TEXT("; "));
	return false;
}
