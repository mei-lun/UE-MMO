#include "ProfessionCatalog.h"

bool FProfessionCatalog::Build(const FProfessionTableSet& Candidate, TArray<FString>& OutProblems)
{
	// The full parse+validation runs in the parser (LoadProfessionSourceFrom...
	// / ParseProfessionSourceFromText own the attack-id gate). Build is the
	// publish gate: it re-checks the structural invariants directly so a
	// caller that skipped the parser cannot publish a broken snapshot.
	OutProblems.Reset();

	// Re-run the structural invariants that Build owns (the parse-step checks
	// live in the parser; both paths must refuse, so this gate is explicit).
	TArray<FString> Problems;
	if (Candidate.Professions.Num() == 0)
	{
		Problems.Add(TEXT("professions: no rows"));
	}
	bool bHasPrototype = false;
	for (const TPair<FName, FProfessionRow>& Pair : Candidate.Professions)
	{
		bHasPrototype = bHasPrototype || Pair.Key == TEXT("prototype");
		if (!Candidate.LevelCurves.Contains(Pair.Value.CurveId))
		{
			Problems.Add(FString::Printf(TEXT("professions[%s]: curve_id '%s' unresolved"),
				*Pair.Key.ToString(), *Pair.Value.CurveId.ToString()));
		}
		for (const TPair<FName, int32>& Starting : Pair.Value.StartingSkillRanks)
		{
			if (!Candidate.Skills.Contains(Starting.Key))
			{
				Problems.Add(FString::Printf(TEXT("professions[%s]: starting skill '%s' unresolved"),
					*Pair.Key.ToString(), *Starting.Key.ToString()));
			}
		}
	}
	if (!bHasPrototype)
	{
		Problems.Add(TEXT("professions: the legacy-compatibility profession 'prototype' is required"));
	}
	for (const TPair<FName, TArray<FProfessionLevelCurveRow>>& Curve : Candidate.LevelCurves)
	{
		if (Curve.Value.Num() == 0 || Curve.Value[0].Level != 1)
		{
			Problems.Add(FString::Printf(TEXT("level_curves[%s]: must start at level 1"), *Curve.Key.ToString()));
			continue;
		}
		for (int32 Index = 0; Index < Curve.Value.Num(); ++Index)
		{
			if (Curve.Value[Index].Level != Index + 1)
			{
				Problems.Add(FString::Printf(TEXT("level_curves[%s]: level sequence broken at %d"),
					*Curve.Key.ToString(), Curve.Value[Index].Level));
			}
		}
		const int32 MaxLevel = Curve.Value.Last().Level;
		if (Curve.Value.Last().XPToNext != 0)
		{
			Problems.Add(FString::Printf(TEXT("level_curves[%s]: xp_to_next must be 0 at the max level"),
				*Curve.Key.ToString()));
		}
		(void)MaxLevel;
	}
	for (const TPair<FName, FSkillRow>& Pair : Candidate.Skills)
	{
		if (!Candidate.Professions.Contains(Pair.Value.ProfessionId))
		{
			Problems.Add(FString::Printf(TEXT("skills[%s]: profession_id unresolved"), *Pair.Key.ToString()));
		}
		// attack_id 解析属于加载步（KnownCombatAttackIds 在解析器持有）；这里只保证
		// 非挂起技能一定携带非空 attack_id。
		if (!Pair.Value.AttackPending && Pair.Value.AttackId.IsNone())
		{
			Problems.Add(FString::Printf(TEXT("skills[%s]: non-pending skill needs an attack_id"),
				*Pair.Key.ToString()));
		}
		const TArray<FSkillLevelRow>* Levels = Candidate.SkillLevels.Find(Pair.Key);
		if (!Levels || Levels->Num() != Pair.Value.MaxRank)
		{
			Problems.Add(FString::Printf(TEXT("skill_levels[%s]: rank rows must cover exactly 1..max_rank"),
				*Pair.Key.ToString()));
		}
	}

	if (Problems.Num() > 0)
	{
		OutProblems = Problems;
		return false;
	}

	// Commit: publish-once, never half-initialized.
	Tables = Candidate;
	ConfigRevision = ComputeProfessionConfigRevision(Candidate);
	bPublished = true;
	return true;
}

const FProfessionRow* FProfessionCatalog::FindProfession(FName ProfessionId) const
{
	return bPublished ? Tables.Professions.Find(ProfessionId) : nullptr;
}

const TArray<FProfessionLevelCurveRow>* FProfessionCatalog::FindLevelCurve(FName CurveId) const
{
	return bPublished ? Tables.LevelCurves.Find(CurveId) : nullptr;
}

const FSkillRow* FProfessionCatalog::FindSkill(FName SkillId) const
{
	return bPublished ? Tables.Skills.Find(SkillId) : nullptr;
}

const TArray<FSkillLevelRow>* FProfessionCatalog::FindSkillLevels(FName SkillId) const
{
	return bPublished ? Tables.SkillLevels.Find(SkillId) : nullptr;
}

bool FProfessionCatalog::GetBaseStats(FName ProfessionId, int32 Level, int32& OutMaxHP,
	int32& OutAttack, int32& OutDefense, int32& OutMaxMP) const
{
	const FProfessionRow* Profession = FindProfession(ProfessionId);
	if (!Profession)
	{
		return false;
	}
	const TArray<FProfessionLevelCurveRow>* Curve = FindLevelCurve(Profession->CurveId);
	if (!Curve || Curve->Num() == 0)
	{
		return false;
	}
	const int32 Clamped = FMath::Clamp(Level, 1, Curve->Last().Level);
	for (const FProfessionLevelCurveRow& Row : *Curve)
	{
		if (Row.Level == Clamped)
		{
			OutMaxHP = Row.BaseMaxHP;
			OutAttack = Row.BaseAttack;
			OutDefense = Row.BaseDefense;
			OutMaxMP = Row.BaseMaxMP;
			return true;
		}
	}
	return false;
}
