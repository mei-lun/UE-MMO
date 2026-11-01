#include "ProfessionDataAssetBuilder.h"

#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

#include "ProfessionDataAsset.h"
#include "ProfessionTableParser.h"

bool UProfessionDataAssetBuilderLibrary::BuildFromSourceDirectory(const FString& SourceDir,
	const FString& DestPackagePath, FString& OutError, FString& OutRevision, bool& OutCreated, bool& OutUpdated)
{
	OutCreated = false;
	OutUpdated = false;
	OutRevision.Reset();

	// 1) Parse + validate the source directory (validation-first: no asset is
	//    touched until the full gate passed).
	FProfessionParsedSource Source;
	if (!LoadProfessionSourceFromDirectory(SourceDir, Source))
	{
		OutError = Source.Problems.Num() > 0
			? FString::Join(Source.Problems, TEXT("\n"))
			: FString::Printf(TEXT("unknown profession source failure in %s"), *SourceDir);
		return false;
	}

	// 2) Build the in-memory asset (create or update in place, never duplicate).
	// One asset per package (UE convention): the package is
	// <DestPackagePath>/DA_ProfessionConfig and holds exactly one asset.
	const FName AssetName = TEXT("DA_ProfessionConfig");
	if (!FPackageName::IsValidLongPackageName(DestPackagePath))
	{
		OutError = FString::Printf(TEXT("invalid destination package name '%s'"), *DestPackagePath);
		return false;
	}
	const FString PackagePath = DestPackagePath / AssetName.ToString();
	UPackage* Package = FindPackage(nullptr, *PackagePath);
	if (!Package)
	{
		Package = CreatePackage(*PackagePath);
	}
	UProfessionConfigAsset* Asset = FindObject<UProfessionConfigAsset>(Package, *AssetName.ToString());
	if (Asset)
	{
		OutUpdated = true;
	}
	else
	{
		Asset = NewObject<UProfessionConfigAsset>(Package, AssetName, RF_Public | RF_Standalone);
		OutCreated = true;
	}

	// 3) Mirror the validated rows (field-for-field; the builder is the only
	//    writer of this asset).
	Asset->SchemaVersion = 1;
	Asset->ConfigRevision = Source.ConfigRevision;
	Asset->Professions.Reset();
	for (const TPair<FName, FProfessionRow>& Pair : Source.Tables.Professions)
	{
		Asset->Professions.Add(Pair.Value);
	}
	Asset->LevelCurves.Reset();
	for (const TPair<FName, TArray<FProfessionLevelCurveRow>>& Pair : Source.Tables.LevelCurves)
	{
		for (const FProfessionLevelCurveRow& Row : Pair.Value)
		{
			Asset->LevelCurves.Add(Row);
		}
	}
	Asset->Skills.Reset();
	for (const TPair<FName, FSkillRow>& Pair : Source.Tables.Skills)
	{
		Asset->Skills.Add(Pair.Value);
	}
	Asset->SkillLevels.Reset();
	for (const TPair<FName, TArray<FSkillLevelRow>>& Pair : Source.Tables.SkillLevels)
	{
		for (const FSkillLevelRow& Row : Pair.Value)
		{
			Asset->SkillLevels.Add(Row);
		}
	}
	Asset->Loadouts = Source.Tables.Loadouts;
	OutRevision = Source.ConfigRevision;

	// 4) Persist. A headless process must be able to save; a save problem is a
	//    failure (M5-008 precedent: the graph stays correct, but this entry
	//    treats the disk write as part of the contract).
	Package->FullyLoad();
	Asset->MarkPackageDirty();
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.bSlowTask = false;
	const FString FilePath = FPackageName::LongPackageNameToFilename(PackagePath, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, Asset, *FilePath, SaveArgs))
	{
		OutError = FString::Printf(TEXT("the package '%s' could not be saved to '%s'"), *DestPackagePath, *FilePath);
		return false;
	}
	return true;
}
