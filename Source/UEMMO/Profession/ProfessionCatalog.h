#pragma once

#include "CoreMinimal.h"

#include "ProfessionTableParser.h"

/**
 * M7-001: the read-only profession config snapshot (02-多职业功能实施方案.md §3:
 * "运行时建立只读的 FProfessionCatalog/FSkillCatalog/FProgressionCatalog，
 * 解析、局部与跨表校验全部成功才一次性发布快照"). One struct owns the whole
 * validated table set for this card; the per-consumer views (skill catalog
 * for M7-003, progression catalog for M7-002) are thin lookups over the same
 * rows and are added with their consumers.
 *
 * Publish-once semantics: Build validates first and only commits on a fully
 * clean result - a rejected build leaves the previous snapshot untouched and
 * the caller receives false with the named problems. After a successful
 * Build the snapshot is treated as immutable by every consumer.
 */
struct FProfessionCatalog
{
	/** Validated table set (empty until a successful Build). */
	FProfessionTableSet Tables;

	/** Deterministic content digest of the published tables. */
	FString ConfigRevision;

	/** True after a successful Build. */
	bool IsPublished() const { return bPublished; }

	/**
	 * Validates and publishes. On failure OutProblems names the problems and
	 * this snapshot is left unchanged.
	 */
	bool Build(const FProfessionTableSet& Candidate, TArray<FString>& OutProblems);

	// -- typed lookups (nullptr/empty for unknown ids; never a fallback) --------

	const FProfessionRow* FindProfession(FName ProfessionId) const;
	const TArray<FProfessionLevelCurveRow>* FindLevelCurve(FName CurveId) const;
	const FSkillRow* FindSkill(FName SkillId) const;
	const TArray<FSkillLevelRow>* FindSkillLevels(FName SkillId) const;

	/** Base stats of one profession at one level; false when out of range. */
	bool GetBaseStats(FName ProfessionId, int32 Level, int32& OutMaxHP, int32& OutAttack,
		int32& OutDefense, int32& OutMaxMP) const;

private:
	bool bPublished = false;
};
