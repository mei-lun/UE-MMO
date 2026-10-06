#pragma once

#include "CoreMinimal.h"

#include "CombatCatalog.h"
#include "CombatDataTableParser.h"

/**
 * M5-006: the cross-table reference release gate for the combat source
 * tables (M5 interface contract section 1, first owner 006 together with
 * 005). Where CombatDataTableParser.h enforces the per-row schema and the
 * local 003/004 value validators, this header enforces that every business
 * id a row references actually exists in the table that owns it, so a typo
 * in a source table never reaches the catalog or the runtime. It is the
 * explicit release-gate wiring M5-005 left out of LoadCombatDataDirectory
 * (the shipped 004 sample rows deliberately reference damage profiles that
 * the 003 sample table does not define yet, so the loader cannot hard-gate
 * references; see the M5-005 report).
 *
 * Checks (every problem is one entry, naming the source table, the row id,
 * the field and the missing id):
 * - weapons: a non-empty damage_profile_id resolves into damage_profiles, a
 *   non-empty ammo_id resolves into ammo_types and a non-empty projectile_id
 *   resolves into projectiles.
 * - weapons (melee): the melee_attack_ids are exactly the complete legacy
 *   four-attack set (light_01, light_02, launcher, aerial_01 - unknown ids,
 *   missing members and duplicates are each reported, contract section 7:
 *   unknown/incomplete attack sets are rejected at validation, never
 *   silently ignored), and every listed attack id resolves into an
 *   attack_reactions row (the reaction_id reference) and a damage_profiles
 *   row (the attack magnitudes).
 * - projectiles: damage_profile_id and, when the projectile explodes,
 *   explosion_damage_profile_id resolve into damage_profiles.
 * - presentations (two-argument entry): every reaction_id resolves into
 *   attack_reactions and every attack reaction keeps exactly one
 *   presentation mapping (missing coverage and duplicated mappings are both
 *   refused).
 * - duplicate row ids inside any input table are refused as defense in
 *   depth: the parser already refuses them at the file level, but the
 *   candidate struct can also be assembled by other code paths.
 *
 * Deliberately NOT in this gate (no frozen type carries the reference, so
 * there is nothing to validate without inventing data):
 * - target_reactions carry no reference fields and nothing links target
 *   reactions to presentations in the 003 value types, so a
 *   target_reactions -> presentations edge does not exist to check;
 * - resource soft-path existence and the required-resource gate belong to
 *   the cook/catalog side (contract section 6); optional presentation
 *   resources fall back to a text log (M5-017);
 * - item mapping (weapons carry no item id in the frozen 004 type), the
 *   target filter combinations (target_filters.json has no parsed value
 *   type yet) and save-alias cycles (persistence manifest card).
 *
 * Pure logic: no UObject, no World, no timers, no file access. The config is
 * only read (const references), a failed validation mutates nothing and
 * publishes nothing - the caller decides what to do with the problem list
 * (a failed gate must never reach FCombatCatalog::BuildFromParsed).
 */

/**
 * The six-table reference gate over the parsed candidate config. Returns one
 * message per problem (empty exactly when every cross-table reference of the
 * six tables resolves and no table carries a duplicate row id).
 */
TArray<FString> ValidateAllReferences(const FParsedCombatConfig& Config);

/**
 * The full release gate: the six-table checks above plus the presentations
 * table (FCombatPresentation rows live outside FParsedCombatConfig because
 * M5-007 froze that struct with the six tables). Use this entry whenever the
 * parsed presentations are available - it is the complete reference
 * validation of a combat data directory.
 */
TArray<FString> ValidateAllReferences(const FParsedCombatConfig& Config, const TArray<FCombatPresentation>& Presentations);
