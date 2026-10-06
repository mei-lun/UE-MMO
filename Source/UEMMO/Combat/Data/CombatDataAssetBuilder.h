#pragma once

#include "CoreMinimal.h"

#include "Engine/DataAsset.h"
#include "Kismet/BlueprintFunctionLibrary.h"

#include "CombatCatalog.h"

#include "CombatDataAssetBuilder.generated.h"

/**
 * M5-008: source-table to UE data asset generation (M5 interface contract
 * section 1, first owner 008 together with 009; work order places the card
 * under Combat/Data/ next to the parser/catalog/validator it builds on).
 * FCombatDataAssetBuilder::BuildFromConfig turns one legal FParsedCombatConfig
 * (the parsed output side of Data/CombatSystem, M5-005/006/007) into one UE
 * data asset per row of the six tables and saves every asset under
 * /Game/UEMMO/Combat/Data:
 *
 * - one UCombatDamageProfileAsset per damage_profiles row    (DA_Damage_<id>)
 * - one UCombatAttackReactionAsset per attack_reactions row  (DA_Reaction_<id>)
 * - one UCombatTargetReactionAsset per target_reactions row  (DA_Target_<id>)
 * - one UCombatWeaponAsset per weapons row                   (DA_Weapon_<id>)
 * - one UCombatAmmoTypeAsset per ammo_types row              (DA_Ammo_<id>)
 * - one UCombatProjectileAsset per projectiles row           (DA_Projectile_<id>)
 *
 * Contract:
 * - Validation-first. A config that fails the frozen catalog build (schema
 *   version, empty ids, duplicate ids), the local 003/004 row validators or
 *   the six-table reference gate (M5-006) produces NO asset at all - not even
 *   the legal rows of the same config - and existing assets are not touched
 *   (task acceptance: any illegal source table fails generation and never
 *   partially overwrites legal assets).
 * - Idempotent. A second call over the same config finds every asset
 *   (in memory or on disk) and updates it in place: created == 0, updated ==
 *   all rows, same object paths, no _1/_2 style duplicates.
 * - Field mirror. Every asset property mirrors its source row exactly (the
 *   numbers, the flags, the arrays in order); the frozen enum values are
 *   stored as their numeric value (uint8) because the 003/004 value types are
 *   deliberately non-reflected plain enums. Every asset carries
 *   ConfigRevision = FCombatCatalog::GetConfigRevision of its config, so the
 *   saved assets and the runtime catalog share one revision (contract
 *   section 6).
 * - Foreign names are refused, never overwritten: if the target object path
 *   of a row is occupied by an object of another class the whole build fails
 *   with an error naming the id.
 * - One asset per package (UE convention, M1-009/M2-016 precedent): the
 *   package /Game/UEMMO/Combat/Data/<AssetName> holds exactly one asset, so
 *   Content/UEMMO/Combat/Data/<AssetName>.uasset is the saved form.
 *
 * The Outer parameter selects the target folder: null selects the default
 * /Game/UEMMO/Combat/Data folder, a UPackage selects the folder named after
 * that package, any other UObject (in particular anything inside the
 * Transient package) is refused.
 *
 * Disk persistence is attempted for every touched package with
 * UPackage::SavePackage; because a headless process may not carry package
 * savers, a save problem is recorded on the summary (SaveProblems,
 * bSavedToDisk=false) instead of failing the build - the asset graph is
 * still correct and a re-run in an editor/commandlet process persists it.
 * The UCombatDataAssetBuilderLibrary entry (and the editor script / console
 * command on top of it) treats a save problem as a failure, because a full
 * editor process must be able to save.
 *
 * Not in this card's surface: the presentations table has no frozen asset
 * home yet (M5-017 owns hit presentation), so presentations are validated by
 * the source-directory entry (two-argument M5-006 gate) but generate no
 * asset; the new-process reload verification of the saved assets belongs to
 * M5-009; Data/items.json and Data/drops.json stay the single source of item
 * and drop definitions (normalizing import belongs to M5-007/020A).
 */

/**
 * One damage profile row of Data/CombatSystem/damage_profiles.json as a UE
 * asset. Pure data mirror of the frozen FDamageProfile value type
 * (DamageTypes.h); the numeric enum-free shape keeps the asset editable in
 * the editor without inventing a second enum home.
 */
UCLASS(BlueprintType)
class UCombatDamageProfileAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Business id of the source row (lowercase a-z, 0-9 and _). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Identity")
	FName DamageProfileId;

	/** Base damage in health points before attack power and defense. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Damage")
	float BaseDamage = 0.0f;

	/** Multiplier applied to the attacker's attack power in the damage formula. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Damage")
	float AttackCoefficient = 1.0f;

	/** Hit stun duration the hit requests, in seconds. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Damage")
	float HitStunSeconds = 0.0f;

	/** Horizontal knockback speed the hit requests, in cm/s. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Damage")
	float KnockbackCmPerSecond = 0.0f;

	/** Vertical (Z) launch speed the hit requests, in cm/s. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Damage")
	float LaunchCmPerSecond = 0.0f;

	/** Hit stop duration the hit requests, in seconds. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Damage")
	float HitStopSeconds = 0.0f;

	/** Deterministic content revision of the config that generated this asset. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Provenance")
	FString ConfigRevision;
};

/**
 * One attack reaction row of attack_reactions.json as a UE asset. Mirror of
 * the frozen FAttackReaction (ReactionTypes.h); ControlPenetration carries
 * the numeric EControlPenetration value (0 = None, 1 = BypassPoise).
 */
UCLASS(BlueprintType)
class UCombatAttackReactionAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Business id of the source row. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Identity")
	FName ReactionId;

	/** Numeric EControlPenetration value: 0 = None, 1 = BypassPoise. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Control")
	uint8 ControlPenetration = 0;

	/** Deterministic content revision of the config that generated this asset. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Provenance")
	FString ConfigRevision;
};

/**
 * One target reaction policy row of target_reactions.json as a UE asset.
 * Mirror of the frozen FTargetReaction (ReactionTypes.h) with every field of
 * the target side of the control negotiation.
 */
UCLASS(BlueprintType)
class UCombatTargetReactionAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Business id of the source row. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Identity")
	FName PolicyId;

	/** Whether the target may be staggered into hit stun at all. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Control")
	bool bAllowStagger = true;

	/** Whether the target may be launched at all (false = launch immune). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Control")
	bool bAllowLaunch = true;

	/** Whether the target may be knocked down at all. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Control")
	bool bAllowKnockdown = true;

	/** Launches the target accepts per air cycle. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Control")
	int32 MaxLaunchesPerAirCycle = 2;

	/** Z speed scale per launch inside one air cycle. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Control")
	TArray<float> LaunchZScales;

	/** Upper bound on one continuous airborne period in seconds. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Control")
	float MaxAirTimeSeconds = 2.5f;

	/** Poise pool size (0 = no poise pool). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Control")
	float PoiseMax = 0.0f;

	/** Poise regeneration period in seconds (0 = no regeneration). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Control")
	float PoiseRegenSeconds = 0.0f;

	/** Knockdown duration in seconds once the target lands launched. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Control")
	float KnockdownSeconds = 0.45f;

	/** Recovering duration in seconds after the knockdown ends. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Control")
	float RecoveringSeconds = 0.25f;

	/** Whether the target takes no damage at all (separate from control immunity). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Immunity")
	bool bImmuneDamage = false;

	/** Whether the target accepts no control at all (separate from damage immunity). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Immunity")
	bool bImmuneControl = false;

	/** Whether the target refuses to die from damage (it still takes damage). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Immunity")
	bool bDeathResistant = false;

	/** Deterministic content revision of the config that generated this asset. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Provenance")
	FString ConfigRevision;
};

/**
 * One weapon definition row of weapons.json as a UE asset. Mirror of the
 * frozen FWeaponDefinition (WeaponTypes.h); FireMode carries the numeric
 * EWeaponFireMode value (0 = Melee, 1 = Hitscan, 2 = Projectile).
 */
UCLASS(BlueprintType)
class UCombatWeaponAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Business id of the source row. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Identity")
	FName WeaponId;

	/** Numeric EWeaponFireMode value: 0 = Melee, 1 = Hitscan, 2 = Projectile. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Identity")
	uint8 FireMode = 0;

	/** Damage profile applied by the weapon itself (empty for projectile weapons). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Damage")
	FName DamageProfileId;

	/** Shared reserve ammo type consumed per shot (empty for melee). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Weapon")
	FName AmmoId;

	/** Rounds of one magazine of this weapon instance (0 for melee). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Weapon")
	int32 MagazineSize = 0;

	/** Maximum sustained pacing in rounds per minute (0 for melee). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Weapon")
	float FireRateRpm = 0.0f;

	/** Rounds per triggered burst (1 = single shots). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Weapon")
	int32 BurstCount = 1;

	/** Pellets per shot (1 = single pellet). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Weapon")
	int32 PelletCount = 1;

	/** Maximum angular deviation of one pellet in degrees (0 = exact). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Weapon")
	float SpreadDegrees = 0.0f;

	/** Hitscan maximum hit distance in centimeters (0 otherwise). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Weapon")
	float RangeCm = 0.0f;

	/** Projectile definition used per shot (required for projectile mode only). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Weapon")
	FName ProjectileId;

	/** Melee attack ids of the legacy chain (melee weapons only). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Weapon")
	TArray<FName> MeleeAttackIds;

	/** Deterministic content revision of the config that generated this asset. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Provenance")
	FString ConfigRevision;
};

/**
 * One ammo type row of ammo_types.json as a UE asset. Mirror of the frozen
 * FAmmoType (AmmoTypes.h).
 */
UCLASS(BlueprintType)
class UCombatAmmoTypeAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Business id of the source row. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Identity")
	FName AmmoId;

	/** Maximum shared reserve stock of this ammo kind. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Ammo")
	int32 MaxReserve = 0;

	/** Standard capacity of one magazine of this ammo kind. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Ammo")
	int32 MagazineSize = 1;

	/** Deterministic content revision of the config that generated this asset. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Provenance")
	FString ConfigRevision;
};

/**
 * One projectile definition row of projectiles.json as a UE asset. Mirror of
 * the frozen FProjectileDefinition (ProjectileTypes.h); Motion carries the
 * numeric EProjectileMotion value (0 = Straight, 1 = Parabolic, 2 = Homing).
 */
UCLASS(BlueprintType)
class UCombatProjectileAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Business id of the source row. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Identity")
	FName ProjectileId;

	/** Numeric EProjectileMotion value: 0 = Straight, 1 = Parabolic, 2 = Homing. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Projectile")
	uint8 Motion = 0;

	/** Launch speed in centimeters per second. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Projectile")
	float SpeedCmS = 0.0f;

	/** Lifetime in seconds on the Pause-frozen World clock. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Projectile")
	float LifetimeS = 0.0f;

	/** Damage profile applied per hit. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Projectile")
	FName DamageProfileId;

	/** Extra targets the projectile may pass through after the first hit. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Projectile")
	int32 PierceCount = 0;

	/** Explosion radius in centimeters (0 = no explosion). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Projectile")
	float ExplosionRadiusCm = 0.0f;

	/** Damage profile of the explosion (required exactly when the radius is greater than 0). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Projectile")
	FName ExplosionDamageProfileId;

	/** Homing turn rate in degrees per second (positive for homing only). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Projectile")
	float HomingTurnRateDegS = 0.0f;

	/** Deterministic content revision of the config that generated this asset. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Provenance")
	FString ConfigRevision;
};

/**
 * What one BuildFromConfig call did. Created/updated entries are full object
 * paths in LoadObject form (/Game/UEMMO/Combat/Data/<AssetName>); on a
 * refused build both lists stay empty. SaveProblems carries one entry per
 * package that could not be saved to disk (a headless process may not carry
 * package savers); bSavedToDisk is true exactly when every touched package
 * was saved.
 */
struct FCombatAssetBuildSummary
{
	/** The target folder the assets were generated under. */
	FString TargetRootPath;

	/** The deterministic content revision stamped on every asset. */
	FString ConfigRevision;

	/** Object paths created by this call. */
	TArray<FString> CreatedObjectPaths;

	/** Object paths found (in memory or on disk) and updated by this call. */
	TArray<FString> UpdatedObjectPaths;

	/** One entry per package that could not be saved to disk. */
	TArray<FString> SaveProblems;

	/** True exactly when every touched package was saved to disk. */
	bool bSavedToDisk = false;

	/** Number of assets this call created or updated. */
	int32 NumAssets() const { return CreatedObjectPaths.Num() + UpdatedObjectPaths.Num(); }
};

/**
 * The stateless build surface. Every function is a pure static; the builder
 * owns no state, no World and no timers.
 */
class FCombatDataAssetBuilder
{
public:
	/** The default target folder: /Game/UEMMO/Combat/Data. */
	static FString GetDefaultAssetRootPath();

	/** Asset object name of one damage profile row: DA_Damage_<id>. */
	static FString MakeDamageProfileAssetName(FName Id);

	/** Asset object name of one attack reaction row: DA_Reaction_<id>. */
	static FString MakeAttackReactionAssetName(FName Id);

	/** Asset object name of one target reaction row: DA_Target_<id>. */
	static FString MakeTargetReactionAssetName(FName Id);

	/** Asset object name of one weapon row: DA_Weapon_<id>. */
	static FString MakeWeaponAssetName(FName Id);

	/** Asset object name of one ammo row: DA_Ammo_<id>. */
	static FString MakeAmmoTypeAssetName(FName Id);

	/** Asset object name of one projectile row: DA_Projectile_<id>. */
	static FString MakeProjectileAssetName(FName Id);

	/**
	 * Builds (creates or updates) one data asset per row of the six tables
	 * and saves every touched package under the target folder. Returns true
	 * when the config was accepted and every asset exists with mirrored
	 * fields; a refused config returns false with OutErrors naming the
	 * offending table, row and field, and creates/updates nothing.
	 *
	 * Outer: null selects the default folder (/Game/UEMMO/Combat/Data); a
	 * UPackage selects the folder named after that package; any other UObject
	 * is refused (assets must never land in the Transient package).
	 *
	 * OutSummary (optional) receives the created/updated object paths, the
	 * stamped revision, the target root and the save problems; on a refused
	 * build it is reset empty.
	 */
	static bool BuildFromConfig(const FParsedCombatConfig& Config, UObject* Outer,
		FString& OutErrors, FCombatAssetBuildSummary* OutSummary = nullptr);
};

/**
 * Editor-facing entry (called by Scripts/Editor/create_combat_system_assets.py
 * and by the UEMMO.Combat.BuildDataAssets console command): parses one source
 * directory (Data/CombatSystem by project-relative path), validates it with
 * the full two-argument M5-006 gate (including the presentations table) and
 * then builds the assets. A validation problem or a save problem fails the
 * entry - a full editor process must produce saved assets or fail loudly.
 * Returns the created/updated counts and the stamped revision for reports.
 */
UCLASS()
class UCombatDataAssetBuilderLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Parses SourceDataDir (project-relative when relative), validates every
	 * required table plus cross-table references and presentations, and
	 * builds the assets under /Game/UEMMO/Combat/Data. Returns the empty
	 * string on success and the error text (naming the offending table, row
	 * and field) on failure; the existing assets are never partially
	 * overwritten. The error text is the return value on purpose: UE Python
	 * discards out parameters when a bool-returning UFunction fails, so a
	 * bool entry would hide the reason from the editor script.
	 */
	UFUNCTION(BlueprintCallable, Category = "Combat|Data")
	static FString BuildFromSourceDirectory(const FString& SourceDataDir,
		int32& OutCreatedCount, int32& OutUpdatedCount, FString& OutConfigRevision);
};
