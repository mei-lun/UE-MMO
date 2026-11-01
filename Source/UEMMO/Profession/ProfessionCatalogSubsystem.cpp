#include "ProfessionCatalogSubsystem.h"

#include "Engine/DataAsset.h"
#include "../Profile/ProfileSubsystem.h"
#include "ProfessionDataAsset.h"
#include "ProfessionTableParser.h"

namespace
{
	// The SP granted when the character REACHED LevelReached: the curve row of
	// the level leveled up FROM carries it (row 10 - the max row - grants 0 by
	// data shape; the fighter rows 1..9 carry 3 each). 0 for unknown
	// professions/levels - the legacy prototype curve carries no SP either way.
	int32 M7_003_ResolveSkillPoints(const FProfessionCatalog& Catalog, FName ProfessionId, int32 LevelReached)
	{
		const FProfessionRow* Profession = Catalog.FindProfession(ProfessionId);
		if (Profession == nullptr || LevelReached <= 1)
		{
			return 0;
		}
		const TArray<FProfessionLevelCurveRow>* Curve = Catalog.FindLevelCurve(Profession->CurveId);
		if (Curve == nullptr)
		{
			return 0;
		}
		for (const FProfessionLevelCurveRow& Row : *Curve)
		{
			if (Row.Level == LevelReached - 1)
			{
				return Row.SkillPoints;
			}
		}
		return 0;
	}
}

void UProfessionCatalogSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The generated one-asset config is the runtime source (M7-001: 纯文本源
	// 表为权威，uasset 不手造 - the asset is a build of the same tables). The
	// source-directory fallback keeps automation environments without cooked
	// assets working and re-validates the text tables on the spot.
	UProfessionConfigAsset* Asset = LoadObject<UProfessionConfigAsset>(
		nullptr, TEXT("/Game/UEMMO/Professions/DA_ProfessionConfig.DA_ProfessionConfig"));
	if (Asset != nullptr)
	{
		FProfessionTableSet Tables;
		for (const FProfessionRow& Row : Asset->Professions)
		{
			Tables.Professions.Add(Row.ProfessionId, Row);
		}
		for (const FProfessionLevelCurveRow& Row : Asset->LevelCurves)
		{
			Tables.LevelCurves.FindOrAdd(Row.CurveId).Add(Row);
		}
		for (const FSkillRow& Row : Asset->Skills)
		{
			Tables.Skills.Add(Row.SkillId, Row);
		}
		for (const FSkillLevelRow& Row : Asset->SkillLevels)
		{
			Tables.SkillLevels.FindOrAdd(Row.SkillId).Add(Row);
		}
		Tables.Loadouts = Asset->Loadouts;

		TArray<FString> Problems;
		if (Catalog.Build(Tables, Problems))
		{
			// The asset carries the revision it was generated from; the
			// rebuild's own digest must agree or the asset is stale.
			ConfigRevision = Asset->ConfigRevision;
			bPublished = true;
			// M7-003: the one-time resolver wiring - the profile subsystem
			// stays catalog-free while the curve numbers (MP pool, SP gains)
			// flow through these read-only lambdas.
			UProfileSubsystem::SetProfessionProgressionResolvers(
				[this](FName ProfessionId, int32 Level)
				{
					int32 OutHP = 0, OutAttack = 0, OutDefense = 0, OutMP = 0;
					return Catalog.GetBaseStats(ProfessionId, Level, OutHP, OutAttack, OutDefense, OutMP)
						? OutMP : 0;
				},
				[this](FName ProfessionId, int32 LevelReached)
				{
					return M7_003_ResolveSkillPoints(Catalog, ProfessionId, LevelReached);
				});
			UE_LOG(LogTemp, Verbose,
				TEXT("UEMMO M7-003: profession catalog published from DA_ProfessionConfig (revision %s)"),
				*ConfigRevision);
			return;
		}
		UE_LOG(LogTemp, Error,
			TEXT("UEMMO M7-003: the generated profession config failed its rebuild validation (%d problems); trying the source tables"),
			Problems.Num());
	}

	if (BuildFromSourceDirectory())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO M7-003: profession catalog published from the Data/Professions source directory (revision %s)"),
			*ConfigRevision);
	}
	else
	{
		bPublished = false;
		UE_LOG(LogTemp, Error,
			TEXT("UEMMO M7-003: no profession catalog could be published; skill activation reports InvalidContext"));
	}
}

bool UProfessionCatalogSubsystem::BuildFromSourceDirectory()
{
	const FString DataDir = FPaths::Combine(FPaths::ProjectDir(), TEXT("Data/Professions"));
	FProfessionParsedSource Source;
	if (!LoadProfessionSourceFromDirectory(DataDir, Source) || !Source.IsValid())
	{
		return false;
	}
	TArray<FString> Problems;
	if (!Catalog.Build(Source.Tables, Problems))
	{
		return false;
	}
	ConfigRevision = Source.ConfigRevision;
	bPublished = true;
	UProfileSubsystem::SetProfessionProgressionResolvers(
		[this](FName ProfessionId, int32 Level)
		{
			int32 OutHP = 0, OutAttack = 0, OutDefense = 0, OutMP = 0;
			return Catalog.GetBaseStats(ProfessionId, Level, OutHP, OutAttack, OutDefense, OutMP)
				? OutMP : 0;
		},
		[this](FName ProfessionId, int32 LevelReached)
		{
			return M7_003_ResolveSkillPoints(Catalog, ProfessionId, LevelReached);
		});
	return true;
}

bool UProfessionCatalogSubsystem::IsPublished() const
{
	return bPublished && Catalog.IsPublished();
}

const FProfessionCatalog& UProfessionCatalogSubsystem::GetCatalog() const
{
	return Catalog;
}

const FString& UProfessionCatalogSubsystem::GetConfigRevision() const
{
	return ConfigRevision;
}

const FSkillLevelRow* UProfessionCatalogSubsystem::FindSkillLevelRow(FName SkillId, int32 Rank) const
{
	if (!IsPublished())
	{
		return nullptr;
	}
	const TArray<FSkillLevelRow>* Levels = Catalog.FindSkillLevels(SkillId);
	if (Levels == nullptr)
	{
		return nullptr;
	}
	for (const FSkillLevelRow& Row : *Levels)
	{
		if (Row.Rank == Rank)
		{
			return &Row;
		}
	}
	return nullptr;
}

const FSkillRow* UProfessionCatalogSubsystem::FindSkill(FName SkillId) const
{
	if (!IsPublished())
	{
		return nullptr;
	}
	return Catalog.FindSkill(SkillId);
}
