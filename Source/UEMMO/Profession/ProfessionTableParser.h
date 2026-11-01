#pragma once

#include "CoreMinimal.h"

#include "ProfessionTypes.h"

/**
 * M7-001: strict parsing and validation for the Data/Professions tables
 * (02-多职业功能实施方案.md §3). One place turns the raw JSON text into the
 * validated value rows and refuses broken structure with errors that name the
 * table, the row and the field, so a typo in a source table can never become
 * a silently defaulted runtime value.
 *
 * Scope split:
 * - The parser enforces the manifest (required-table registry), the per-table
 *   schema (schema_version, exact field sets, JSON kinds, integral/finite
 *   numbers, id syntax), duplicate ids, and every local per-row rule.
 * - ValidateProfessionTables performs the cross-table gate: curve references,
 *   level continuity, max-level xp_to_next==0, prototype equivalence against
 *   FExperienceCurve, skill profession references, rank continuity, the
 *   attack_pending contract (pending rows skip combat-attack resolution,
 *   non-pending rows must resolve in Data/combat-attacks.json), loadout
 *   slot/skill/profession rules and the placeholder path contract.
 * - The DataAsset builder (ProfessionDataAssetBuilder) owns the UE asset
 *   generation on top of a validated set; this parser never touches UObject.
 *
 * Pure logic: no UObject, no World. JSON reuse: the hand-rolled RFC 8259
 * subset parser FCombatJsonParser from Combat/Data/CombatDataTableParser.h
 * (the engine Json module import library is not on this module's link line -
 * same lesson as M1-008/M5-005), including its duplicate-key and non-finite
 * number refusals.
 */

// ===========================================================================
// Parsed set
// ===========================================================================

/**
 * The parsed content of one profession data directory. Level curves are
 * keyed by curve id with the rows in ascending level order (the parser
 * sorts; source order never reaches the set). Skill levels are keyed by
 * skill id with the rows in ascending rank order.
 */
struct FProfessionTableSet
{
	TMap<FName, FProfessionRow> Professions;
	TMap<FName, TArray<FProfessionLevelCurveRow>> LevelCurves;
	TMap<FName, FSkillRow> Skills;
	TMap<FName, TArray<FSkillLevelRow>> SkillLevels;
	TArray<FLoadoutRow> Loadouts;

	void Reset();
};

/**
 * Result of a directory load: the parsed set plus the deterministic content
 * revision (M5-038 precedent). The revision is only valid when Problems is
 * empty.
 */
struct FProfessionParsedSource
{
	FProfessionTableSet Tables;

	/** Reserved attack ids listed in Data/combat-attacks.json (parsed for the pending gate). */
	TSet<FName> KnownCombatAttackIds;

	/** Deterministic lowercase hex digest of the normalized table content. */
	FString ConfigRevision;

	/** Empty when the load+validation succeeded; each entry names table/row/field. */
	TArray<FString> Problems;

	bool IsValid() const { return Problems.Num() == 0; }
};

/**
 * Loads Data/Professions from disk (manifest.json + the registered tables +
 * Data/combat-attacks.json for the pending-attack gate) and runs the full
 * local+cross validation. Never throws; on any problem IsValid() is false and
 * Problems names the first issues (bounded so a broken directory cannot spam).
 */
bool LoadProfessionSourceFromDirectory(const FString& DataDir, FProfessionParsedSource& OutSource);

/**
 * Text-injected variant used by tests and tools: table name -> raw JSON text.
 * The combat attack id set is injected by the caller (tests pass fixtures).
 * Runs the same strict parse+validate path as the directory loader.
 */
bool ParseProfessionSourceFromText(const TMap<FString, FString>& TableTexts,
	const TSet<FName>& KnownCombatAttackIds, FProfessionParsedSource& OutSource);

/**
 * Deterministic content digest of a validated set (domain-tagged FMD5 over
 * the normalized rows in sorted order; stable across runs and module reloads).
 * Also usable on unvalidated sets - the digest is content-only.
 */
FString ComputeProfessionConfigRevision(const FProfessionTableSet& Tables);
