#pragma once

#include "CoreMinimal.h"

#include "../Weapons/WeaponTypes.h"

/**
 * Read-side item definition layer of the M3 equipment stack (interface
 * contract section 8). A definition is the shared, reusable description of one
 * item kind; concrete owned occurrences are FItemInstance (ItemInstance.h).
 * Plain value types on purpose: identity, validation and serialization stay
 * pure C++ logic testable without a World; nothing here reads assets.
 *
 * Stat name mapping to the existing M1 combat attributes (the card requires an
 * explicit, documented mapping; wiring itself belongs to later M3 tasks):
 * - FItemStats.Attack  -> attacker "AttackPower" entry point of the design
 *   damage formula M1_019_ComputeHitDamage (CombatComponent.cpp):
 *   damage = max(1, round((baseDamage + AttackPower * coefficient)
 *                          * 100 / (100 + max(0, Defense)))).
 * - FItemStats.Defense -> defender "Defense" entry point of the same formula.
 * - FItemStats.MaxHP   -> UHealthComponent::MaxHealth upper bound of the HP
 *   pool (HealthComponent.h).
 */

/** Equipment slot of a definition; the three values are closed by design. */
enum class EItemSlot : uint8
{
	Weapon = 0,
	Armor = 1,
	Accessory = 2
};

/** Rarity tier of a definition: 1 normal / 2 rare / 3 legendary (minimal). */
enum class EItemRarity : uint8
{
	Normal = 1,
	Rare = 2,
	Legendary = 3
};

/** The three combat attributes an item grants; all must stay finite and >= 0. */
struct FItemStats
{
	/** Feeds the M1 damage formula as the attacker's AttackPower. */
	float Attack = 0.0f;

	/** Feeds the M1 damage formula as the defender's Defense. */
	float Defense = 0.0f;

	/** Raises the UHealthComponent MaxHealth upper bound. */
	float MaxHP = 0.0f;
};

/** Shared description of one item kind (interface contract section 8). */
struct FItemDefinition
{
	/** Stable business identifier (e.g. "weapon_training"); never a UE asset path. */
	FName DefinitionId;

	/** Human readable label shown in UI; display-only, never used as identity. */
	FString DisplayName;

	/** Equipment slot; must be one of the three closed enum values. */
	EItemSlot Slot = EItemSlot::Weapon;

	/** Guaranteed stats of the kind; rolled instance stats start as a copy. */
	FItemStats BaseStats;

	/**
	 * Soft object path of the icon (e.g. "/Game/.../T_Sword"); deliberately a
	 * plain string, may be empty for this card (no icons shipped yet).
	 */
	FString IconPath;

	/** Rarity tier 1/2/3; must be one of the closed enum values. */
	EItemRarity Rarity = EItemRarity::Normal;

	/**
	 * M5-019: the weapon behavior bound to this item kind through the
	 * item_definition_id connection (M5 interface contract section 7). Empty
	 * (None) for every non-weapon item; on a Weapon-slot definition it names
	 * the FWeaponDefinition (CombatCatalog FindWeapon) this item's instances
	 * resolve to. The mapping never creates a second item instance - the
	 * FItemInstance identity (InstanceId/Level/RollSeed) is preserved and the
	 * per-instance magazine state lives in the M5-019 binding layer.
	 */
	FName WeaponDefinitionId;
};

/**
 * Returns true only for the three closed slot values; catches out-of-enum
 * values that reached the struct through casts or hand-written data.
 */
inline bool IsValidItemSlot(EItemSlot Slot)
{
	switch (Slot)
	{
	case EItemSlot::Weapon:
	case EItemSlot::Armor:
	case EItemSlot::Accessory:
		return true;
	default:
		return false;
	}
}

/**
 * Parses a slot from source-data text ("Weapon"/"Armor"/"Accessory",
 * case-insensitive). Returns false and leaves OutSlot untouched for empty or
 * unknown text; the empty string must never map to a valid slot.
 */
inline bool ParseItemSlot(const FString& Text, EItemSlot& OutSlot)
{
	if (Text.Equals(TEXT("Weapon"), ESearchCase::IgnoreCase))
	{
		OutSlot = EItemSlot::Weapon;
		return true;
	}
	if (Text.Equals(TEXT("Armor"), ESearchCase::IgnoreCase))
	{
		OutSlot = EItemSlot::Armor;
		return true;
	}
	if (Text.Equals(TEXT("Accessory"), ESearchCase::IgnoreCase))
	{
		OutSlot = EItemSlot::Accessory;
		return true;
	}
	return false;
}

/** Returns true only for the three closed rarity values (1/2/3). */
inline bool IsValidItemRarity(EItemRarity Rarity)
{
	switch (Rarity)
	{
	case EItemRarity::Normal:
	case EItemRarity::Rare:
	case EItemRarity::Legendary:
		return true;
	default:
		return false;
	}
}

/**
 * Validates a single item definition in isolation and returns true when legal.
 * Rules (M3-001): DefinitionId non-empty, Slot one of Weapon/Armor/Accessory,
 * BaseStats.Attack/Defense/MaxHP finite and >= 0, Rarity one of 1/2/3.
 * M5-019 adds the weapon mapping rules: WeaponDefinitionId may only be set on
 * a Weapon-slot definition and, when set, must match the M5 definition id text
 * rule (1-64 characters of a-z0-9_, WeaponTypes.h IsValidDefinitionIdText).
 * On failure OutErrors joins every problem with "; " and each problem names
 * its field (e.g. "DefinitionId", "Slot", "BaseStats.Attack", "Rarity",
 * "WeaponDefinitionId").
 * IconPath and DisplayName are display-only and never rejected here.
 */
inline bool ValidateItemDefinition(const FItemDefinition& Definition, FString& OutErrors)
{
	TArray<FString> Problems;

	// Identity: a definition without a stable id cannot be referenced by
	// inventories, equipment or drop tables.
	if (Definition.DefinitionId.IsNone())
	{
		Problems.Add(TEXT("DefinitionId must be a non-empty identifier (got None)"));
	}

	// Slot: only the three closed values are legal.
	if (!IsValidItemSlot(Definition.Slot))
	{
		Problems.Add(FString::Printf(TEXT("Slot must be one of Weapon/Armor/Accessory (got enum value %d)"),
			static_cast<int32>(Definition.Slot)));
	}

	// Base stats: negative grants would silently weaken the character, and a
	// non-finite value would poison every later stat sum, so both are rejected.
	const float StatValues[3] = { Definition.BaseStats.Attack, Definition.BaseStats.Defense, Definition.BaseStats.MaxHP };
	const TCHAR* StatNames[3] = { TEXT("BaseStats.Attack"), TEXT("BaseStats.Defense"), TEXT("BaseStats.MaxHP") };
	for (int32 Index = 0; Index < 3; ++Index)
	{
		if (!FMath::IsFinite(StatValues[Index]))
		{
			Problems.Add(FString::Printf(TEXT("%s must be a finite number (got %s)"),
				StatNames[Index], *FString::SanitizeFloat(StatValues[Index])));
		}
		else if (StatValues[Index] < 0.0f)
		{
			Problems.Add(FString::Printf(TEXT("%s must be >= 0 (got %s)"),
				StatNames[Index], *FString::SanitizeFloat(StatValues[Index])));
		}
	}

	// Rarity: only the three closed values are legal.
	if (!IsValidItemRarity(Definition.Rarity))
	{
		Problems.Add(FString::Printf(TEXT("Rarity must be 1 (Normal), 2 (Rare) or 3 (Legendary) (got %d)"),
			static_cast<int32>(Definition.Rarity)));
	}

	// M5-019 weapon mapping: the field is empty for armor/accessory items and,
	// when set, must be a weapon-slot definition carrying a syntactically
	// valid M5 weapon id (the id resolving to a real row is checked at bind
	// time against the combat catalog, never silently rerouted).
	if (!Definition.WeaponDefinitionId.IsNone())
	{
		if (Definition.Slot != EItemSlot::Weapon)
		{
			Problems.Add(FString::Printf(TEXT("WeaponDefinitionId must be empty for non-Weapon slot items (item '%s' slot %d got weapon '%s')"),
				*Definition.DefinitionId.ToString(),
				static_cast<int32>(Definition.Slot),
				*Definition.WeaponDefinitionId.ToString()));
		}
		if (!IsValidDefinitionIdText(Definition.WeaponDefinitionId.ToString()))
		{
			Problems.Add(FString::Printf(TEXT("WeaponDefinitionId must be 1-64 characters of a-z0-9_ (got '%s')"),
				*Definition.WeaponDefinitionId.ToString()));
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

/**
 * Minimal in-memory catalog of item definitions keyed by DefinitionId
 * (interface contract section 8 refers to a definition collection; the full
 * read-only catalog asset belongs to a later task). AddDefinition rejects a
 * duplicate DefinitionId and an empty id; other per-definition field checks
 * stay with ValidateItemDefinition, so the catalog itself never validates
 * stats. Find returns nullptr for unknown ids and never falls back silently.
 */
struct FItemDefinitionCatalog
{
	/**
	 * Adds one definition. Returns false (and fills OutError when provided,
	 * naming "DefinitionId") when the id is empty or already registered;
	 * a rejected definition does not modify the catalog.
	 */
	bool AddDefinition(const FItemDefinition& Definition, FString* OutError = nullptr)
	{
		if (Definition.DefinitionId.IsNone())
		{
			if (OutError)
			{
				*OutError = TEXT("DefinitionId must be a non-empty identifier (got None)");
			}
			return false;
		}
		if (Definitions.Contains(Definition.DefinitionId))
		{
			if (OutError)
			{
				*OutError = FString::Printf(TEXT("DefinitionId '%s' is already registered; duplicate definitions are rejected"),
					*Definition.DefinitionId.ToString());
			}
			return false;
		}
		Definitions.Add(Definition.DefinitionId, Definition);
		return true;
	}

	/** Read-only lookup by DefinitionId; nullptr when the id is unknown. */
	const FItemDefinition* Find(FName DefinitionId) const
	{
		return Definitions.Find(DefinitionId);
	}

	/** Number of definitions currently registered. */
	int32 Num() const
	{
		return Definitions.Num();
	}

private:
	TMap<FName, FItemDefinition> Definitions;
};
