#include "AttackCatalog.h"

#include "AttackDefinition.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogUEMMOAttackCatalog, Log, All);

namespace
{
	// Every configured asset must live inside this directory tree. The same
	// directory is pinned for cooking via ProjectPackagingSettings in
	// Config/DefaultGame.ini, keeping catalog content and the standalone
	// package cook rule in one scope.
	const TCHAR* const AllowedContentRoot = TEXT("/Game/UEMMO/Combat/");
}

bool UAttackCatalog::InitializeFromConfig(FText& OutError)
{
	const UAttackCatalog* Defaults = GetDefault<UAttackCatalog>();
	TArray<FString> AssetObjectPaths;
	if (Defaults != nullptr)
	{
		for (const FSoftObjectPath& AssetPath : Defaults->AttackAssetPaths)
		{
			AssetObjectPaths.Add(AssetPath.ToString());
		}
	}
	if (AssetObjectPaths.Num() == 0)
	{
		OutError = FText::FromString(TEXT("UAttackCatalog: [/Script/UEMMO.AttackCatalog] AttackAssetPaths is empty in the Game config (Config/DefaultGame.ini); the catalog refuses to start without explicit asset references."));
		return false;
	}
	return InitializeFromPaths(AssetObjectPaths, OutError);
}

bool UAttackCatalog::InitializeFromPaths(const TArray<FString>& AssetObjectPaths, FText& OutError)
{
	TArray<UAttackDefinition*> LoadedDefinitions;
	LoadedDefinitions.Reserve(AssetObjectPaths.Num());
	for (const FString& AssetObjectPath : AssetObjectPaths)
	{
		if (!AssetObjectPath.StartsWith(AllowedContentRoot))
		{
			OutError = FText::FromString(FString::Printf(TEXT("UAttackCatalog: configured attack asset '%s' is outside the allowed content root '%s'; refusing to initialize."), *AssetObjectPath, AllowedContentRoot));
			return false;
		}
		UAttackDefinition* Definition = LoadObject<UAttackDefinition>(nullptr, *AssetObjectPath);
		if (Definition == nullptr)
		{
			OutError = FText::FromString(FString::Printf(TEXT("UAttackCatalog: failed to load attack data asset '%s'; refusing to initialize with an incomplete catalog."), *AssetObjectPath));
			return false;
		}
		LoadedDefinitions.Add(Definition);
	}
	return BuildFromDefinitions(LoadedDefinitions, OutError);
}

bool UAttackCatalog::BuildFromDefinitions(const TArray<UAttackDefinition*>& Definitions, FText& OutError)
{
	// Build into locals and only commit on success so a rejected build never
	// leaves a half-initialized catalog behind.
	TArray<TObjectPtr<UAttackDefinition>> BuiltEntries;
	TMap<FName, int32> BuiltIndex;
	TSet<FName> SeenIds;
	SeenIds.Reserve(Definitions.Num());

	for (int32 Index = 0; Index < Definitions.Num(); ++Index)
	{
		UAttackDefinition* Definition = Definitions[Index];
		if (Definition == nullptr)
		{
			OutError = FText::FromString(FString::Printf(TEXT("UAttackCatalog: entry %d is a null definition; refusing to build the catalog."), Index));
			return false;
		}
		const FName AttackId = Definition->AttackId;
		if (AttackId.IsNone())
		{
			OutError = FText::FromString(FString::Printf(TEXT("UAttackCatalog: entry %d has no AttackId; refusing to build the catalog."), Index));
			return false;
		}
		bool bAlreadySeen = false;
		SeenIds.Add(AttackId, &bAlreadySeen);
		if (bAlreadySeen)
		{
			OutError = FText::FromString(FString::Printf(TEXT("UAttackCatalog: duplicate attack id '%s'; refusing to build the catalog."), *AttackId.ToString()));
			return false;
		}
		BuiltEntries.Add(Definition);
		BuiltIndex.Add(AttackId, BuiltEntries.Num() - 1);
	}

	Entries = MoveTemp(BuiltEntries);
	IdToEntryIndex = MoveTemp(BuiltIndex);
	LoggedUnknownAttackIds.Empty();
	OutError = FText::GetEmpty();
	return true;
}

const UAttackDefinition* UAttackCatalog::Find(FName AttackId) const
{
	const int32* EntryIndex = IdToEntryIndex.Find(AttackId);
	if (EntryIndex != nullptr)
	{
		return Entries[*EntryIndex].Get();
	}
	// Missing ids never fall back to another entry; each unknown id is logged
	// at most once per build so repeated lookups cannot spam the log.
	if (!LoggedUnknownAttackIds.Contains(AttackId))
	{
		UE_LOG(LogUEMMOAttackCatalog, Warning, TEXT("UAttackCatalog::Find: unknown AttackId '%s'; returning nullptr (no silent fallback)."), *AttackId.ToString());
		LoggedUnknownAttackIds.Add(AttackId);
	}
	return nullptr;
}

const TArray<TObjectPtr<UAttackDefinition>>& UAttackCatalog::GetEntries() const
{
	return Entries;
}
