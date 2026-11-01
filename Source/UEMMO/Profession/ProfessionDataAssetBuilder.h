#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "ProfessionDataAssetBuilder.generated.h"

/**
 * M7-001: editor-side generation of the profession config data asset from a
 * validated Data/Professions directory (same architecture as the combat
 * builder: validation-first, idempotent, revision-stamped).
 *
 * Contract:
 * - Validation-first. Any parse/validation problem produces NO asset at all
 *   and leaves existing assets untouched; OutError names the table/row/field.
 * - Idempotent. A second call over the same source updates
 *   /Game/UEMMO/Professions/DA_ProfessionConfig in place (no duplicates).
 * - Revision stamp. The asset carries ComputeProfessionConfigRevision of its
 *   source, so consumers can refuse a stale asset.
 * - Save. The touched package is saved to disk; a save problem is reported
 *   through OutError (a full editor/commandlet process must be able to save).
 */
UCLASS()
class UProfessionDataAssetBuilderLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Builds /Game/UEMMO/Professions/DA_ProfessionConfig from SourceDir
	 * (a directory holding manifest.json + the registered profession tables).
	 * Returns false with OutError naming the first problem on any failure.
	 * OutRevision receives the stamped revision; OutCreated/OutUpdated report
	 * whether the asset was created or updated in place.
	 */
	UFUNCTION(BlueprintCallable, Category = "Profession")
	static bool BuildFromSourceDirectory(const FString& SourceDir, const FString& DestPackagePath,
		FString& OutError, FString& OutRevision, bool& OutCreated, bool& OutUpdated);
};
