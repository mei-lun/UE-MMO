#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "ProfessionCatalog.h"

#include "ProfessionCatalogSubsystem.generated.h"

/**
 * M7-003: the runtime home of the published profession catalog (02 文档 §3:
 * 运行时建立只读的 FProfessionCatalog/FSkillCatalog/FProgressionCatalog，
 * 解析、局部与跨表校验全部成功才一次性发布快照). Loads the generated
 * /Game/UEMMO/Professions/DA_ProfessionConfig asset, rebuilds the plain
 * FProfessionCatalog value from its rows and exposes read-only lookups.
 * A failed build stays unpublished: every consumer reads IsPublished() and
 * refuses (skill activation reports InvalidContext), never half-loads.
 */
UCLASS()
class UEMMO_API UProfessionCatalogSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// USubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	/** True once the generated asset loaded and the catalog built+published. */
	bool IsPublished() const;

	/** The published catalog; only meaningful while IsPublished(). */
	const FProfessionCatalog& GetCatalog() const;

	/** The deterministic revision of the source tables this catalog came from. */
	const FString& GetConfigRevision() const;

	/**
	 * The rank row of one skill (null when the skill/rank does not exist).
	 * The convenience the loadout component and the purchase flow share.
	 */
	const FSkillLevelRow* FindSkillLevelRow(FName SkillId, int32 Rank) const;

	/** The skill row (null when unknown). */
	const FSkillRow* FindSkill(FName SkillId) const;

private:
	/** Reads Data/Professions (the authoritative text source) when the
	 * generated asset is missing - the dev/automation path; production
	 * always has the asset. Returns false when neither source validates. */
	bool BuildFromSourceDirectory();

	FProfessionCatalog Catalog;
	FString ConfigRevision;
	bool bPublished = false;
};
