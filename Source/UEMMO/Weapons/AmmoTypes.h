#pragma once

#include "CoreMinimal.h"

#include "WeaponTypes.h"

/**
 * M5-004: ammo type definition value type (M5 interface contract section 1,
 * first owner 004). An ammo type is the shared description of one reserve
 * pool kind: reserve ammo is shared per ammo id across the profile, while
 * magazine state stays per weapon instance (design section 9) - the magazine
 * size here describes the standard capacity of one magazine of this ammo
 * kind, the per-instance magazine lives in the later WeaponComponent/AmmoModel
 * layer. Plain value type on purpose: validation stays pure C++ logic
 * testable without a World and the definition never holds a World/Actor
 * pointer (contract section 0.5).
 */

/**
 * Upper bounds of the first-version ammo fields. The bounds are sanity limits
 * for the source tables, not tuning targets; raising one is a deliberate
 * schema change.
 */
constexpr int32 MaxAmmoReserve = 99999;
constexpr int32 MaxAmmoMagazineSize = 999;

/** Immutable description of one reserve ammo kind (interface contract section 1, owner 004). */
struct FAmmoType
{
	/**
	 * Stable business identifier (e.g. "ammo_cell"); lowercase a-z0-9_ per
	 * IsValidDefinitionIdText (WeaponTypes.h), unique inside the ammo table.
	 */
	FName AmmoId;

	/**
	 * Maximum shared reserve stock of this ammo kind. 0 is legal (an ammo
	 * kind that starts fully consumed), negatives are not.
	 */
	int32 MaxReserve = 0;

	/** Standard capacity of one magazine of this ammo kind; at least 1. */
	int32 MagazineSize = 1;
};

/**
 * Validates a single ammo type in isolation and returns true when legal.
 * Rules (M5-004): AmmoId matches IsValidDefinitionIdText, MaxReserve inside
 * 0..MaxAmmoReserve, MagazineSize inside 1..MaxAmmoMagazineSize.
 * On failure OutErrors joins every problem with "; " and each problem names
 * its field (e.g. "AmmoId", "MaxReserve").
 */
inline bool ValidateAmmoType(const FAmmoType& Ammo, FString& OutErrors)
{
	TArray<FString> Problems;

	// Identity: a definition without a stable lowercase id cannot be
	// referenced by weapon definitions or save data.
	if (!IsValidDefinitionIdText(Ammo.AmmoId.ToString()))
	{
		Problems.Add(FString::Printf(TEXT("AmmoId must be 1-64 characters of a-z0-9_ (got '%s')"),
			*Ammo.AmmoId.ToString()));
	}

	// Reserve: a negative cap would silently mint ammunition, so it is
	// rejected; the upper bound keeps source-table typos visible.
	if (Ammo.MaxReserve < 0 || Ammo.MaxReserve > MaxAmmoReserve)
	{
		Problems.Add(FString::Printf(TEXT("MaxReserve must be 0..%d (got %d)"), MaxAmmoReserve, Ammo.MaxReserve));
	}

	// Magazine: an ammo kind whose magazine cannot hold a round is meaningless.
	if (Ammo.MagazineSize < 1 || Ammo.MagazineSize > MaxAmmoMagazineSize)
	{
		Problems.Add(FString::Printf(TEXT("MagazineSize must be a positive value in 1..%d (got %d)"),
			MaxAmmoMagazineSize, Ammo.MagazineSize));
	}

	if (Problems.Num() == 0)
	{
		OutErrors.Reset();
		return true;
	}
	OutErrors = FString::Join(Problems, TEXT("; "));
	return false;
}
