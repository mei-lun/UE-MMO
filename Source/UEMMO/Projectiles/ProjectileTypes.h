#pragma once

#include "CoreMinimal.h"

#include "../Weapons/WeaponTypes.h"

/**
 * M5-004: projectile definition value type (M5 interface contract section 1,
 * first owner 004). A definition is the shared, immutable description of one
 * projectile kind; actors, spawning and motion integration belong to the
 * later Projectiles/* tasks (026..032). Plain value type on purpose:
 * validation stays pure C++ logic testable without a World and the definition
 * never holds a World/Actor pointer (contract section 0.5).
 *
 * Units (contract section 4: every quantity carries its unit):
 * - speeds in centimeters per second (SpeedCmS),
 * - times in seconds (LifetimeS) on the Pause-frozen World clock,
 * - lengths in centimeters (ExplosionRadiusCm),
 * - angles in degrees per second (HomingTurnRateDegS).
 *
 * Semantics frozen here (design section 4):
 * - straight/parabolic/homing are the only first-version motions; there is no
 *   bouncing trajectory and no arbitrary scripting.
 * - PierceCount is the number of extra targets the projectile may pass
 *   through after the first hit (0 = terminate on the first target).
 * - ExplosionRadiusCm 0 means "no explosion"; an explosion needs its own
 *   damage profile. The first version forbids combining an explosion with
 *   piercing so one projectile can never detonate more than once.
 * - Homing needs a positive turn rate; the other motions must not carry one.
 *   Target loss handling after a lost target belongs to the motion tasks.
 */

/** Motion type of a projectile; the three values are closed by design. */
enum class EProjectileMotion : uint8
{
	Straight = 0,
	Parabolic = 1,
	Homing = 2
};

/**
 * Upper bounds of the first-version projectile fields. The bounds are sanity
 * limits for the source tables, not tuning targets; raising one is a
 * deliberate schema change.
 */
constexpr float MaxProjectileSpeedCmS = 100000.0f;
constexpr float MaxProjectileLifetimeS = 60.0f;
constexpr int32 MaxProjectilePierceCount = 32;
constexpr float MaxProjectileExplosionRadiusCm = 10000.0f;
constexpr float MaxHomingTurnRateDegS = 720.0f;

/** Returns true only for the three closed motion values. */
inline bool IsValidProjectileMotion(EProjectileMotion Motion)
{
	switch (Motion)
	{
	case EProjectileMotion::Straight:
	case EProjectileMotion::Parabolic:
	case EProjectileMotion::Homing:
		return true;
	default:
		return false;
	}
}

/**
 * Parses a motion from source-data text ("straight"/"parabolic"/"homing",
 * case-insensitive). Returns false and leaves OutMotion untouched for empty
 * or unknown text; the empty string must never map to a valid motion.
 */
inline bool ParseProjectileMotion(const FString& Text, EProjectileMotion& OutMotion)
{
	if (Text.Equals(TEXT("straight"), ESearchCase::IgnoreCase))
	{
		OutMotion = EProjectileMotion::Straight;
		return true;
	}
	if (Text.Equals(TEXT("parabolic"), ESearchCase::IgnoreCase))
	{
		OutMotion = EProjectileMotion::Parabolic;
		return true;
	}
	if (Text.Equals(TEXT("homing"), ESearchCase::IgnoreCase))
	{
		OutMotion = EProjectileMotion::Homing;
		return true;
	}
	return false;
}

/** Immutable description of one projectile kind (interface contract section 1, owner 004). */
struct FProjectileDefinition
{
	/**
	 * Stable business identifier (e.g. "bullet_linear"); lowercase a-z0-9_ per
	 * IsValidDefinitionIdText (WeaponTypes.h), unique inside the projectiles
	 * table.
	 */
	FName ProjectileId;

	/** Motion type; must be one of the three closed enum values. */
	EProjectileMotion Motion = EProjectileMotion::Straight;

	/** Launch speed in centimeters per second; strictly positive. */
	float SpeedCmS = 0.0f;

	/** Lifetime in seconds on the Pause-frozen World clock; strictly positive. */
	float LifetimeS = 0.0f;

	/** Damage profile applied per hit; required for every motion. */
	FName DamageProfileId;

	/** Extra targets the projectile may pass through after the first hit (0 = none). */
	int32 PierceCount = 0;

	/** Explosion radius in centimeters; 0 means the projectile has no explosion. */
	float ExplosionRadiusCm = 0.0f;

	/**
	 * Damage profile of the explosion; required exactly when
	 * ExplosionRadiusCm is greater than 0.
	 */
	FName ExplosionDamageProfileId;

	/**
	 * Homing turn rate in degrees per second; required (positive) for the
	 * homing motion, must stay 0 for the other motions.
	 */
	float HomingTurnRateDegS = 0.0f;
};

/**
 * Validates a single projectile definition in isolation and returns true when
 * legal. Rules (M5-004): ProjectileId matches IsValidDefinitionIdText, Motion
 * one of the three closed values, SpeedCmS/LifetimeS finite and strictly
 * positive inside their bounds, PierceCount inside 0..MaxProjectilePierceCount,
 * explosion radius and explosion damage profile either both present or both
 * empty, explosion never combined with piercing, homing turn rate positive
 * exactly for the homing motion. Cross-table reference checks (damage profile
 * ids resolve) belong to M5-005/006.
 * On failure OutErrors joins every problem with "; " and each problem names
 * its field (e.g. "ProjectileId", "SpeedCmS", "PierceCount").
 */
inline bool ValidateProjectileDefinition(const FProjectileDefinition& Definition, FString& OutErrors)
{
	TArray<FString> Problems;

	// Identity: a definition without a stable lowercase id cannot be
	// referenced by weapon definitions or catalogs.
	if (!IsValidDefinitionIdText(Definition.ProjectileId.ToString()))
	{
		Problems.Add(FString::Printf(TEXT("ProjectileId must be 1-64 characters of a-z0-9_ (got '%s')"),
			*Definition.ProjectileId.ToString()));
	}

	// Motion: only the three closed values are legal; the motion rule block
	// below compares against Homing, so an unknown motion is reported together
	// with every other independent problem.
	if (!IsValidProjectileMotion(Definition.Motion))
	{
		Problems.Add(FString::Printf(TEXT("Motion must be one of Straight/Parabolic/Homing (got enum value %d)"),
			static_cast<int32>(Definition.Motion)));
	}

	// Speed: negative would invert the trajectory, zero never arrives, and a
	// non-finite value would poison every sweep query.
	if (!FMath::IsFinite(Definition.SpeedCmS) || Definition.SpeedCmS <= 0.0f || Definition.SpeedCmS > MaxProjectileSpeedCmS)
	{
		Problems.Add(FString::Printf(TEXT("SpeedCmS must be a finite value in (0..%f] (got %s)"),
			MaxProjectileSpeedCmS, *FString::SanitizeFloat(Definition.SpeedCmS)));
	}

	// Lifetime: negative is a time contradiction, zero despawns instantly.
	if (!FMath::IsFinite(Definition.LifetimeS) || Definition.LifetimeS <= 0.0f || Definition.LifetimeS > MaxProjectileLifetimeS)
	{
		Problems.Add(FString::Printf(TEXT("LifetimeS must be a finite value in (0..%f] (got %s)"),
			MaxProjectileLifetimeS, *FString::SanitizeFloat(Definition.LifetimeS)));
	}

	// Damage profile: every hit needs one.
	if (Definition.DamageProfileId.IsNone())
	{
		Problems.Add(TEXT("DamageProfileId must be a non-empty identifier"));
	}

	// Pierce count: negative is meaningless, the upper bound keeps source-table
	// typos visible (the pellet/hit ledger carries one entry per pierced hit).
	if (Definition.PierceCount < 0 || Definition.PierceCount > MaxProjectilePierceCount)
	{
		Problems.Add(FString::Printf(TEXT("PierceCount must be a value in 0..%d (got %d)"),
			MaxProjectilePierceCount, Definition.PierceCount));
	}

	// Explosion semantics: radius and profile appear together or not at all,
	// and an explosion never combines with piercing.
	if (!FMath::IsFinite(Definition.ExplosionRadiusCm) || Definition.ExplosionRadiusCm < 0.0f
		|| Definition.ExplosionRadiusCm > MaxProjectileExplosionRadiusCm)
	{
		Problems.Add(FString::Printf(TEXT("ExplosionRadiusCm must be a finite value in 0..%f (got %s)"),
			MaxProjectileExplosionRadiusCm, *FString::SanitizeFloat(Definition.ExplosionRadiusCm)));
	}
	if (Definition.ExplosionRadiusCm > 0.0f && Definition.ExplosionDamageProfileId.IsNone())
	{
		Problems.Add(TEXT("ExplosionDamageProfileId must be a non-empty identifier when ExplosionRadiusCm is greater than 0"));
	}
	if (Definition.ExplosionRadiusCm == 0.0f && !Definition.ExplosionDamageProfileId.IsNone())
	{
		Problems.Add(FString::Printf(TEXT("ExplosionDamageProfileId must be empty when ExplosionRadiusCm is 0 (got '%s')"),
			*Definition.ExplosionDamageProfileId.ToString()));
	}
	if (Definition.ExplosionRadiusCm > 0.0f && Definition.PierceCount > 0)
	{
		Problems.Add(TEXT("PierceCount must be 0 when the projectile has an explosion (one projectile must never detonate more than once)"));
	}

	// Homing semantics: a homing projectile needs a positive turn rate; the
	// other motions must not carry one. An unknown motion is never classified
	// as non-homing here - it was already reported above.
	if (IsValidProjectileMotion(Definition.Motion))
	{
		if (Definition.Motion == EProjectileMotion::Homing)
		{
			if (!FMath::IsFinite(Definition.HomingTurnRateDegS) || Definition.HomingTurnRateDegS <= 0.0f
				|| Definition.HomingTurnRateDegS > MaxHomingTurnRateDegS)
			{
				Problems.Add(FString::Printf(TEXT("HomingTurnRateDegS must be a finite value in (0..%f] for homing projectiles (got %s)"),
					MaxHomingTurnRateDegS, *FString::SanitizeFloat(Definition.HomingTurnRateDegS)));
			}
		}
		else if (Definition.HomingTurnRateDegS != 0.0f)
		{
			Problems.Add(TEXT("HomingTurnRateDegS must be 0 for non-homing projectiles"));
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
