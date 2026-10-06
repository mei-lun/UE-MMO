#pragma once

#include "CoreMinimal.h"

#include "../System/DamageTypes.h"
#include "../System/ReactionTypes.h"
#include "../../Weapons/WeaponTypes.h"
#include "../../Weapons/AmmoTypes.h"
#include "../../Projectiles/ProjectileTypes.h"

/**
 * M5-007: the read-only combat configuration catalog (M5 interface contract
 * section 1, owner 007; work order places it under Combat/Data/). The catalog
 * aggregates the six frozen configuration value types of M5-003/004 and gives
 * one immutable, World-free lookup surface over them:
 *
 * - BuildFromParsed constructs a catalog from already-validated candidate
 *   rows. "Parsed" means the output side of the source-table parsing
 *   (M5-005/006): this card builds from legal value data and deliberately
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
 * Not in this card's surface: item/drop adapters and TestRoom reward/spawn
 * definitions (they need the normalized item source from M5-005) and the
 * target_filters/presentations tables (no frozen value type exists yet).
 */

/**
 * Candidate rows the catalog is built from - the parsed output side of the
 * source tables under Data/CombatSystem (one array per table, every row
 * already validated by M5-005/006). The parser owner fills this struct; the
 * catalog owns no parsing, no JSON and no file access.
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
};

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
	 * duplicate id inside any of the six tables.
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

	/**
	 * Deterministic content signature (16 lowercase hex characters) over the
	 * schema version and the normalized rows of all six tables. Empty only on
	 * a never-built catalog. Stable across processes for the same content;
	 * suitable for save-data provenance and event tracing (contract section 6).
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

private:
	/** Computes the revision digest of a fully filled catalog. */
	static FString ComputeConfigRevision(int32 SchemaVersion, const FCombatCatalog& Catalog);

	TMap<FName, FDamageProfile> DamageProfiles;
	TMap<FName, FAttackReaction> AttackReactions;
	TMap<FName, FTargetReaction> TargetReactions;
	TMap<FName, FWeaponDefinition> Weapons;
	TMap<FName, FAmmoType> AmmoTypes;
	TMap<FName, FProjectileDefinition> Projectiles;

	FString ConfigRevision;
};
