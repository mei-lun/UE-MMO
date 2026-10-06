#pragma once

#include "CoreMinimal.h"

#include "../System/DamageTypes.h"
#include "../System/ReactionTypes.h"
#include "../../Weapons/WeaponTypes.h"
#include "../../Weapons/AmmoTypes.h"
#include "../../Projectiles/ProjectileTypes.h"

/**
 * M5-005: structured source-table parsing and field validation for the
 * Data/CombatSystem JSON tables (M5 interface contract section 1, first
 * owners 005 together with 006). One place turns the raw JSON text into
 * candidate data (the 003/004 value types plus the presentation row) and
 * refuses broken structure with errors that name the table, the row id and
 * the field, so a typo in a source table never becomes a silently defaulted
 * runtime value.
 *
 * Scope split against the neighboring cards:
 * - This parser enforces the per-row schema (exact field sets, JSON kinds,
 *   integral numbers, finite values, id syntax, enum tokens), the 003/004
 *   local value validators on every parsed row, the in-table rules
 *   (duplicate ids, the one-presentation-per-reaction mapping), the
 *   required-table manifest and the rejection of unknown table file names.
 * - It does NOT resolve cross-table references inside LoadCombatDataDirectory
 *   and does not cook assets or generate actors. Reference checking is
 *   provided as the explicit ValidateCombatDataReferences entry (tested here,
 *   both directions) so M5-006 owns the release-gate wiring; the shipped
 *   A-segment samples deliberately reference damage profiles that arrive with
 *   that card, so a hard reference gate inside the loader would reject the
 *   current source tables.
 * - Data/items.json and Data/drops.json stay the single source of item and
 *   drop definitions; nothing here copies them (normalizing import belongs
 *   to the catalog card 007).
 *
 * Pure logic: no UObject, no World, no timers. Everything is plain C++ on
 * Core types and testable without a World (interface contract section 0.5).
 *
 * JSON note (same lesson as M1-008/M3-001/M5-003/M5-004): UnrealBuildTool
 * does not put the engine Json module import library on the UEMMO link line,
 * so the parser carries its own small hand-rolled RFC 8259 subset parser.
 * Unlike the earlier per-test-file copies this one is the shared definition
 * site (FCombatJsonValue/FCombatJsonParser) that the test files reuse.
 * On top of the JSON grammar it rejects duplicate keys inside one object and
 * numbers that decode to non-finite doubles.
 */

// ===========================================================================
// JSON value model and parser
// ===========================================================================

/**
 * One parsed JSON value: just enough shape for the combat config tables.
 * Object member order is not preserved (source tables are unordered maps),
 * but duplicate keys never reach the map - the parser refuses them.
 */
class FCombatJsonValue
{
public:
	enum class EKind
	{
		Null,
		Boolean,
		Number,
		String,
		Array,
		Object
	};

	EKind Kind = EKind::Null;
	bool Boolean = false;
	double Number = 0.0;
	FString String;
	TArray<TSharedPtr<FCombatJsonValue>> Array;
	TMap<FString, TSharedPtr<FCombatJsonValue>> Object;
};

/**
 * Recursive descent JSON parser (RFC 8259 subset: objects, arrays, strings
 * with escapes, numbers, booleans, null). Beyond the grammar it refuses:
 * - duplicate keys inside one object (M5-005 acceptance: duplicate keys);
 * - numbers that decode to a non-finite double (overflow such as 1e999);
 * - the NaN/Infinity literals (not valid JSON numbers).
 * Errors carry the failing offset so the source location survives.
 */
class FCombatJsonParser
{
public:
	static bool Parse(const FString& Text, TSharedPtr<FCombatJsonValue>& OutRoot, FString& OutError);
};

// ===========================================================================
// Presentation row value type
// ===========================================================================

/**
 * One hit presentation row of Data/CombatSystem/presentations.json: the
 * reaction a hit presentation is attached to plus the optional soft paths
 * (montage, sound, effect). This value type is owned by the parsing card
 * because no 003 header carries it; the presentation resources themselves
 * stay owned by the later presentation tasks (M5-017) and a missing optional
 * resource falls back to a text log (interface contract section 6).
 */
struct FCombatPresentation
{
	/** Unique business id: lowercase a-z, 0-9 and _, 1..64 characters. */
	FName PresentationId;

	/** The attack reaction this presentation is attached to. */
	FName ReactionId;

	/** Soft object path of the hit montage; empty when none. */
	FString MontagePath;

	/** Soft object path of the impact sound; empty when none. */
	FString SoundPath;

	/** Soft object path of the impact effect; empty when none. */
	FString EffectPath;

	/**
	 * Placeholder rows declare empty paths on purpose (the shipped sample
	 * rows); a non-placeholder row must declare at least one non-empty path.
	 */
	bool bPlaceholder = true;
};

// ===========================================================================
// Row parsers
// ===========================================================================

/**
 * Parses one damage_profiles row into FDamageProfile. Strict schema: exactly
 * the seven known fields (damage_profile_id, base_damage, attack_coefficient,
 * hit_stun_s, knockback_cm_s, launch_cm_s, hit_stop_s), all required, JSON
 * kinds enforced, unknown fields refused, then ValidateDamageProfile must
 * pass. On failure OutError names every problem and OutProfile is reset.
 */
bool ParseDamageProfile(const FCombatJsonValue& Row, FDamageProfile& OutProfile, FString& OutError);

/**
 * Parses one attack_reactions row into FAttackReaction (reaction_id,
 * control_penetration). Same strict-schema rules as ParseDamageProfile;
 * the penetration token goes through ParseControlPenetration.
 */
bool ParseAttackReaction(const FCombatJsonValue& Row, FAttackReaction& OutReaction, FString& OutError);

/**
 * Parses one target_reactions row into FTargetReaction (policy_id plus the
 * thirteen policy fields). Same strict-schema rules; integral counts are
 * refused when fractional.
 */
bool ParseTargetReaction(const FCombatJsonValue& Row, FTargetReaction& OutPolicy, FString& OutError);

/**
 * Parses one presentations row into FCombatPresentation (presentation_id,
 * reaction_id, montage_path, sound_path, effect_path, placeholder). A
 * non-placeholder row must declare at least one non-empty resource path.
 */
bool ParsePresentation(const FCombatJsonValue& Row, FCombatPresentation& OutPresentation, FString& OutError);

/**
 * Parses one weapons row into FWeaponDefinition (the twelve documented
 * fields, mode through ParseWeaponFireMode). Same strict-schema rules;
 * ValidateWeaponDefinition must pass afterwards.
 */
bool ParseWeaponDefinition(const FCombatJsonValue& Row, FWeaponDefinition& OutWeapon, FString& OutError);

/**
 * Parses one ammo_types row into FAmmoType (ammo_id, max_reserve,
 * magazine_size). Same strict-schema rules; ValidateAmmoType must pass.
 */
bool ParseAmmoType(const FCombatJsonValue& Row, FAmmoType& OutAmmo, FString& OutError);

/**
 * Parses one projectiles row into FProjectileDefinition (the nine documented
 * fields, motion through ParseProjectileMotion). Same strict-schema rules;
 * ValidateProjectileDefinition must pass.
 */
bool ParseProjectileDefinition(const FCombatJsonValue& Row, FProjectileDefinition& OutProjectile, FString& OutError);

// Convenience overloads: parse the row from raw JSON text first. They share
// the value-based implementations above and exist so tools and tests can
// throw a single row at the parser without building a document.
bool ParseDamageProfile(const FString& RowJsonText, FDamageProfile& OutProfile, FString& OutError);
bool ParseAttackReaction(const FString& RowJsonText, FAttackReaction& OutReaction, FString& OutError);
bool ParseTargetReaction(const FString& RowJsonText, FTargetReaction& OutPolicy, FString& OutError);
bool ParsePresentation(const FString& RowJsonText, FCombatPresentation& OutPresentation, FString& OutError);
bool ParseWeaponDefinition(const FString& RowJsonText, FWeaponDefinition& OutWeapon, FString& OutError);
bool ParseAmmoType(const FString& RowJsonText, FAmmoType& OutAmmo, FString& OutError);
bool ParseProjectileDefinition(const FString& RowJsonText, FProjectileDefinition& OutProjectile, FString& OutError);

// ===========================================================================
// Directory loading
// ===========================================================================

/**
 * The parsed content of one combat data directory. Every table is keyed by
 * its business id, so a duplicate id can never enter the set silently.
 * SkippedKnownTableFiles lists the known tables this parser does not parse
 * yet (target_filters/tags/vehicles/vehicle_seats/vehicle_mounts) that were
 * present in the directory - skipped explicitly, never silently.
 */
struct FCombatDataTableSet
{
	TMap<FName, FDamageProfile> DamageProfiles;
	TMap<FName, FAttackReaction> AttackReactions;
	TMap<FName, FTargetReaction> TargetReactions;
	TMap<FName, FCombatPresentation> Presentations;
	TMap<FName, FWeaponDefinition> Weapons;
	TMap<FName, FAmmoType> AmmoTypes;
	TMap<FName, FProjectileDefinition> Projectiles;

	TArray<FString> SkippedKnownTableFiles;

	void Reset();
};

/**
 * The required-table manifest of the A segment. The source manifest stays
 * code-defined until a data manifest file lands; vehicles and the other
 * C-segment tables are deliberately absent so a legal A-segment source set
 * parses without them (task acceptance), while an unknown table FILE name is
 * still refused.
 */
const TArray<FName>& GetRequiredCombatTableNames();

/**
 * Known combat tables that a later card owns parsing for. A file named after
 * one of these is skipped (and recorded) instead of being refused as unknown.
 */
const TArray<FName>& GetKnownUnparsedCombatTableNames();

/**
 * Loads every combat source table of one directory (Data/CombatSystem).
 * - Each required table file is parsed strictly (document schema_version=1,
 *   matching inner table name, rows array; per-row strict schema and value
 *   validation; in-table duplicate ids refused).
 * - Unknown .json table file names are refused (no silent ignoring of
 *   typos); known-unparsed tables and a manifest.json are skipped and
 *   recorded. Non-JSON files are ignored.
 * - One presentation per attack reaction is enforced inside the set.
 * On success OutTables holds the parsed candidate data and true is returned.
 * On failure OutProblems carries one message per problem (each names the
 * file/table, the row id when known and the field) and the already parsed
 * tables stay in OutTables for diagnostics; the caller must treat the set as
 * unpublished (a failed parse never produces a half-released catalog).
 */
bool LoadCombatDataDirectory(const FString& DataDir, FCombatDataTableSet& OutTables, TArray<FString>& OutProblems);

/**
 * Cross-table reference check over a loaded set: weapons.ammo_id resolves
 * into ammo_types, weapons.projectile_id into projectiles, the weapon,
 * projectile and explosion damage profile ids into damage_profiles and the
 * presentation reaction ids into attack_reactions. Returns true exactly when
 * no dangling reference remains; every problem names the source table, the
 * row id and the field. This is the mechanism for M5-006; wiring it into a
 * release gate (and reconciling the shipped sample damage profiles) belongs
 * to that card, so LoadCombatDataDirectory does not call it.
 */
bool ValidateCombatDataReferences(const FCombatDataTableSet& Tables, TArray<FString>& OutProblems);
