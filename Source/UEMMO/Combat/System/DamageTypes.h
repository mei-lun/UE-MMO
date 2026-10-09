#pragma once

#include "CoreMinimal.h"

#include <type_traits>

/**
 * M5-003: the damage profile value type (M5 interface contract section 1,
 * first owner 003). One FDamageProfile is the immutable numeric definition
 * of a single attack hit: how hard it hits and which reaction magnitudes
 * (stun, knockback, launch, hit stop) ride on the hit. The magnitudes are
 * requests that the target's reaction policy (ReactionTypes.h) may still
 * refuse or gate; they are not outcomes.
 *
 * Pure value type: no .cpp, no UObject members, no World/Actor pointers and
 * no behavior. Validation is a local, World-free computation over the struct
 * (interface contract: local validation must not depend on cross-table
 * lookups and pure computation must not read the World). Downstream owners
 * (005/006 parsing, 011 damage resolution) implement strategies in their own
 * files and never edit this header.
 *
 * Units (interface contract section 4): damage in health points, times in
 * seconds, speeds in cm/s. X is lateral, Y depth, Z height, so
 * LaunchCmPerSecond is the vertical (Z) launch impulse magnitude.
 */

/**
 * Business id syntax shared by every combat system source table (design
 * section 4): 1..64 characters of lowercase ASCII a-z, 0-9 and underscore.
 * M5-036: this contract is enforced on the RAW JSON string inside
 * CombatDataTableParser (the FName display string cannot be trusted for
 * casing in the packaged game build, which compiles without
 * case-preserving FNames - FName(*RawId) adopts the name pool's existing
 * display entry, so a legal lowercase 'normal' can surface as 'Normal').
 * The 003 validators therefore only guard against NAME_None and rely on
 * the parser for the character-set rule.
 * This single helper is the one definition site used by every 003 validator
 * call site that holds a raw string (presentations, test-room ids).
 */
inline bool IsValidDefinitionIdSyntax(const FString& Id)
{
	if (Id.IsEmpty() || Id.Len() > 64)
	{
		return false;
	}
	for (int32 Index = 0; Index < Id.Len(); ++Index)
	{
		const TCHAR Character = Id[Index];
		const bool bLowerCaseLetter = (Character >= TEXT('a') && Character <= TEXT('z'));
		const bool bDigit = (Character >= TEXT('0') && Character <= TEXT('9'));
		const bool bUnderscore = (Character == TEXT('_'));
		if (!bLowerCaseLetter && !bDigit && !bUnderscore)
		{
			return false;
		}
	}
	return true;
}

/**
 * Joins the collected problem strings with "; " into OutErrors. Returns true
 * exactly when Problems is empty (the validated value is acceptable), so the
 * validators below can end with a single shared call.
 */
inline bool FinishDefinitionValidation(const TArray<FString>& Problems, FString& OutErrors)
{
	OutErrors.Reset();
	for (int32 Index = 0; Index < Problems.Num(); ++Index)
	{
		if (Index > 0)
		{
			OutErrors += TEXT("; ");
		}
		OutErrors += Problems[Index];
	}
	return Problems.Num() == 0;
}

/**
 * The per-hit numeric definition of one attack. Defaults are intentionally
 * NOT a usable attack (BaseDamage 0 is invalid and the id is empty): every
 * profile must come from validated configuration, never from a silent
 * fallback value.
 */
struct FDamageProfile
{
	/** Unique business id: lowercase a-z, 0-9 and _, 1..64 characters. */
	FName DamageProfileId;

	/** Base damage in health points before attack power and defense. Finite and greater than 0. */
	float BaseDamage = 0.0f;

	/**
	 * Multiplier applied to the attacker's attack power inside the legacy
	 * damage formula. Finite and not negative (0 = pure base damage hit).
	 */
	float AttackCoefficient = 1.0f;

	/** Hit stun duration the hit requests, in seconds. Finite and not negative (0 = no stagger requested). */
	float HitStunSeconds = 0.0f;

	/** Horizontal knockback speed the hit requests, in cm/s. Finite and not negative. */
	float KnockbackCmPerSecond = 0.0f;

	/** Vertical (Z) launch speed the hit requests, in cm/s. Finite and not negative (0 = no launch requested). */
	float LaunchCmPerSecond = 0.0f;

	/** Hit stop duration the hit requests, in seconds. Finite and not negative. */
	float HitStopSeconds = 0.0f;
};

/**
 * Local validation of one damage profile: finite values, no negative times
 * or speeds, positive base damage and an id that is present (the raw
 * character-set rule lives in the parser, M5-036).
 * Collects every problem instead of stopping at the first, joined with "; "
 * so one bad row reports all of its bad fields at once.
 */
inline bool ValidateDamageProfile(const FDamageProfile& Profile, FString& OutErrors)
{
	TArray<FString> Problems;
	// The character-set rule lives on the raw JSON string in the parser
	// (M5-036: FName display casing is not trustworthy in-package); here we
	// only refuse an absent id (NAME_None displays as the legal-looking
	// "None", so a syntax check alone would let it through).
	if (Profile.DamageProfileId == NAME_None)
	{
		Problems.Add(FString::Printf(TEXT("DamageProfileId '%s' must be 1..64 characters of a-z, 0-9 and _"),
			*Profile.DamageProfileId.ToString()));
	}
	if (!FMath::IsFinite(Profile.BaseDamage) || Profile.BaseDamage <= 0.0f)
	{
		Problems.Add(TEXT("BaseDamage must be finite and greater than 0"));
	}
	if (!FMath::IsFinite(Profile.AttackCoefficient) || Profile.AttackCoefficient < 0.0f)
	{
		Problems.Add(TEXT("AttackCoefficient must be finite and not negative"));
	}
	if (!FMath::IsFinite(Profile.HitStunSeconds) || Profile.HitStunSeconds < 0.0f)
	{
		Problems.Add(TEXT("HitStunSeconds must be finite and not negative"));
	}
	if (!FMath::IsFinite(Profile.KnockbackCmPerSecond) || Profile.KnockbackCmPerSecond < 0.0f)
	{
		Problems.Add(TEXT("KnockbackCmPerSecond must be finite and not negative"));
	}
	if (!FMath::IsFinite(Profile.LaunchCmPerSecond) || Profile.LaunchCmPerSecond < 0.0f)
	{
		Problems.Add(TEXT("LaunchCmPerSecond must be finite and not negative"));
	}
	if (!FMath::IsFinite(Profile.HitStopSeconds) || Profile.HitStopSeconds < 0.0f)
	{
		Problems.Add(TEXT("HitStopSeconds must be finite and not negative"));
	}
	return FinishDefinitionValidation(Problems, OutErrors);
}

// Compile-time pins: a damage profile is a value snapshot, never a
// World/Actor pointer holder (interface contract section 0.5).
static_assert(!std::is_pointer_v<decltype(FDamageProfile::DamageProfileId)>, "FDamageProfile::DamageProfileId must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageProfile::BaseDamage)>, "FDamageProfile::BaseDamage must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageProfile::AttackCoefficient)>, "FDamageProfile::AttackCoefficient must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageProfile::HitStunSeconds)>, "FDamageProfile::HitStunSeconds must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageProfile::KnockbackCmPerSecond)>, "FDamageProfile::KnockbackCmPerSecond must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageProfile::LaunchCmPerSecond)>, "FDamageProfile::LaunchCmPerSecond must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageProfile::HitStopSeconds)>, "FDamageProfile::HitStopSeconds must stay a value type");
static_assert(std::is_trivially_copyable_v<FDamageProfile>, "FDamageProfile must stay a trivially copyable value snapshot");
