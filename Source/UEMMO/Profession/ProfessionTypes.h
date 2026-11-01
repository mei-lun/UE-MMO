#pragma once

#include "CoreMinimal.h"

#include "ProfessionTypes.generated.h"

/**
 * M7-001: skill effect policy whitelist (02-多职业功能实施方案.md §6). Only the
 * four implemented policies are usable in source tables; the three reserved
 * values exist so later cards can extend the enum without renumbering, and a
 * table row that names a reserved (not yet implemented) policy is refused by
 * the parser - the whitelist is the implemented set, never the enum span.
 */
UENUM(BlueprintType)
enum class EProfessionSkillEffectPolicy : uint8
{
	SingleMeleeHit = 0,
	GroundArea = 1,
	Projectile = 2,
	SelfBuff = 3,
	// Reserved for later cards (M7-003+ follow-ups); refused by the M7-001 parser.
	MultiHit = 4,
	TargetDebuff = 5,
	Grab = 6
};

/**
 * One level_curves.json row. The prototype curve must reproduce
 * FExperienceCurve (Profile/ExperienceCurve.h) level 1..10 value by value and
 * carry zero MP/SP columns; the fighter curve is the first MP/SP consumer.
 * USTRUCT so the validated rows can live on the config data asset.
 */
USTRUCT(BlueprintType)
struct FProfessionLevelCurveRow
{
	GENERATED_BODY()

	/** The curve this row belongs to (lowercase id). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FName CurveId;

	/** Level this row describes (1-based; the set of rows of one curve is continuous). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	int32 Level = 0;

	/** XP needed to advance FROM this level; 0 exactly at the curve's max level. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	int32 XPToNext = 0;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	int32 BaseMaxHP = 0;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	int32 BaseAttack = 0;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	int32 BaseDefense = 0;

	/** New in M7: the MP pool. The prototype curve carries 0 (no MP usage). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	int32 BaseMaxMP = 0;

	/** New in M7: out-of-combat MP regeneration per second (prototype: 0). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	int32 BaseMPRegen = 0;

	/** Skill points granted when REACHING the next level (0 at the max level). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	int32 SkillPoints = 0;
};

/**
 * One professions.json row.
 */
USTRUCT(BlueprintType)
struct FProfessionRow
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FName ProfessionId;

	/** Display name as authored (localization is a later card's concern). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FString DisplayName;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FName CurveId;

	/** UE mesh object path; empty while asset_placeholder is true. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FString MeshPath;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FString SkeletonPath;

	/** Locomotion set id (reserved identifier in this card; consumed by M7-003). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FName LocomotionSetId;

	/** Skills granted at character creation (skill id -> rank). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	TMap<FName, int32> StartingSkillRanks;

	/** Weapon modes this profession may equip (subset of the weapon table modes). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	TArray<FName> AllowedWeaponModes;

	/** Hit presentation id; empty in this card (presentations arrive with M7-003). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FString PresentationId;

	/**
	 * True while the visual assets are placeholders (empty mesh/skeleton paths).
	 * The parser enforces: empty paths <=> placeholder true.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	bool AssetPlaceholder = true;
};

/**
 * One skills.json row. launcher_z references the EXISTING combat attack
 * 'launcher'; the five hotbar rows carry attack_pending=true with reserved
 * attack ids that M7-003 wires up (a pending skill is registered, never
 * executable).
 */
USTRUCT(BlueprintType)
struct FSkillRow
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FName SkillId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FName ProfessionId;

	/** 1..8; the skill_levels rows of this skill are continuous 1..MaxRank. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	int32 MaxRank = 1;

	/** "hotbar" (loadout slots 1..8) or "launcher" (the fixed Z input). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FName SlotEligibility;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	EProfessionSkillEffectPolicy EffectPolicy = EProfessionSkillEffectPolicy::SingleMeleeHit;

	/** The combat attack this skill drives (reserved id while attack_pending). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FName AttackId;

	/** True while the attack row does not exist in Data/combat-attacks.json yet. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	bool AttackPending = false;

	/** Design payloads (M7-001: poise_break only). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	TMap<FName, float> EffectParameters;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FString IconPath;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FString MontagePath;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FString VfxPath;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FString SoundPath;

	/** True when every visual path is empty on purpose. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	bool Placeholder = true;
};

/**
 * One skill_levels.json row: the per-rank numbers of one skill. Ranks of one
 * skill are continuous 1..MaxRank; each rank's numbers are looked up by the
 * character level check - never interpolated.
 */
USTRUCT(BlueprintType)
struct FSkillLevelRow
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FName SkillId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	int32 Rank = 0;

	/** Character level required to purchase/hold this rank (1-based). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	int32 RequiredCharacterLevel = 1;

	/** SP consumed when purchasing this rank (0 = free, e.g. the granted rank 1). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	int32 SPCost = 0;

	/** Cooldown in SECONDS (UE-side tuning value; PVF milliseconds are never used). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	float CooldownS = 0.f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	int32 MPCost = 0;

	/** Multiplier over the attack base at activation time. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	float DamageCoefficient = 1.f;

	/** Rank-local design payloads overriding/extending the skill row's. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	TMap<FName, float> EffectParameters;
};

/**
 * One loadouts.json row: the default hotbar assignment of one profession.
 * Slot index 1..8 maps to Q/W/E/R/A/S/D/F. launcher skills are fixed inputs
 * and must not appear as loadout rows.
 */
USTRUCT(BlueprintType)
struct FLoadoutRow
{
	GENERATED_BODY()

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FName ProfessionId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	int32 SlotIndex = 0;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Profession")
	FName SkillId;
};
