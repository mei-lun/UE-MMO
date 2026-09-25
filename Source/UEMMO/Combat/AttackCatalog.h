#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"
#include "UObject/NoExportTypes.h"
#include "UObject/SoftObjectPath.h"

#include "AttackCatalog.generated.h"

class UAttackDefinition;

/**
 * Runtime, read-only directory of attack definitions (interface contract
 * section 3). The catalog is built once at startup from explicit asset
 * references and afterwards only answers lookups; it never writes definitions
 * and never reads Data/combat-attacks.json (that developer JSON exists only on
 * authoring machines and in authoring-side tests).
 *
 * Loading model (M1-010): the four attack data assets are referenced
 * explicitly from Config/DefaultGame.ini in the [/Script/UEMMO.AttackCatalog]
 * section (AttackAssetPaths). The CDO picks that section up through the
 * UCLASS(config=Game) binding and InitializeFromConfig() loads exactly those
 * entries with LoadObject. The same ini pins /Game/UEMMO/Combat under
 * ProjectPackagingSettings.DirectoriesToAlwaysCook, which is the tracked cook
 * dependency for standalone packages (end-to-end package verification belongs
 * to M1-039). No Asset Manager scan is used, so the catalog can never silently
 * pick up unrelated assets from other directories.
 *
 * Failure policy (fail fast): an empty reference list, a reference outside
 * /Game/UEMMO/Combat, an unloadable asset, or a duplicate AttackId fails the
 * whole initialization and leaves the catalog empty; the catalog never comes
 * up half-initialized and Find() never silently falls back to another entry.
 */
UCLASS(BlueprintType, Config = Game)
class UAttackCatalog : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Reads AttackAssetPaths from this class's config defaults (the
	 * [/Script/UEMMO.AttackCatalog] section of the Game ini chain) and calls
	 * InitializeFromPaths. Returns false with OutError describing the first
	 * problem when the list is empty or any reference fails; on failure the
	 * catalog stays empty.
	 */
	bool InitializeFromConfig(FText& OutError);

	/**
	 * Path-injected variant used by tests and future callers that already hold
	 * object paths: loads every path as a UAttackDefinition and builds the
	 * catalog. Paths must be full object paths ("/Game/.../Asset.Asset") inside
	 * /Game/UEMMO/Combat. Fail fast on the first problem; on failure the
	 * catalog stays empty.
	 */
	bool InitializeFromPaths(const TArray<FString>& AssetObjectPaths, FText& OutError);

	/**
	 * Pure build step: (re)keys the catalog by AttackId from the given
	 * definitions in array order. Returns false and fills OutError when any
	 * entry is null, has no AttackId, or repeats an id already in the array;
	 * the error names the offending id. Definitions are built into locals and
	 * only committed on success, so a rejected build leaves the previous
	 * (possibly empty) catalog untouched.
	 */
	bool BuildFromDefinitions(const TArray<UAttackDefinition*>& Definitions, FText& OutError);

	/**
	 * Read-only lookup by stable id (interface contract section 3). Returns the
	 * matching definition or nullptr for a missing id; a missing id never falls
	 * back to another entry. Every unknown id is logged at most once per build
	 * so repeated lookups cannot spam the log.
	 */
	const UAttackDefinition* Find(FName AttackId) const;

	/** All definitions in configuration order; used by tests and tooling. */
	const TArray<TObjectPtr<UAttackDefinition>>& GetEntries() const;

private:
	/** Loaded definitions in configuration order; UPROPERTY keeps them alive for the GC. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAttackDefinition>> Entries;

	/** AttackId -> index into Entries; rebuilt only by BuildFromDefinitions. */
	TMap<FName, int32> IdToEntryIndex;

	/** Unknown ids already reported by Find; cleared on every successful rebuild (mutable: Find is const). */
	mutable TSet<FName> LoggedUnknownAttackIds;

	/** Explicit asset object paths loaded by InitializeFromConfig (Config/DefaultGame.ini). */
	UPROPERTY(config, EditDefaultsOnly, Category = "Combat")
	TArray<FSoftObjectPath> AttackAssetPaths;
};
