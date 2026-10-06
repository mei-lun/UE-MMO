#include "CombatDataAssetBuilder.h"

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"

#include "CombatDataTableParser.h"
#include "CombatReferenceValidator.h"

/**
 * M5-008 implementation notes:
 *
 * - BuildFromConfig runs in five phases. Phase 0 resolves the target folder
 *   from the Outer contract. Phase 1 validates the whole config (frozen
 *   catalog build for the revision + local 003/004 validators + the six-table
 *   reference gate of M5-006) before touching anything, so a refused config
 *   never creates or overwrites an asset. Phase 2 resolves every target
 *   object (in-memory lookup first, then a disk check, then a load) and
 *   refuses foreign name occupiers before creating anything. Phase 3 creates
 *   the missing assets and mirrors every row field. Phase 4 saves every
 *   touched package and records save problems on the summary (a headless
 *   process may not carry package savers; re-running in an editor process
 *   persists the graph - the UCombatDataAssetBuilderLibrary entry treats a
 *   save problem as a failure because a full editor must save).
 * - Names are type-prefixed (DA_Damage_, DA_Reaction_, DA_Target_,
 *   DA_Weapon_, DA_Ammo_, DA_Projectile_) because business ids repeat across
 *   tables by design (attack "light_01" and its damage profile "light_01");
 *   one prefix per table keeps the asset set collision-free under one folder.
 * - Every function here is game-thread static logic over CoreUObject; no
 *   World, no timers, no per-call state.
 */

namespace
{
	constexpr const TCHAR* DamagePrefix = TEXT("DA_Damage_");
	constexpr const TCHAR* ReactionPrefix = TEXT("DA_Reaction_");
	constexpr const TCHAR* TargetPrefix = TEXT("DA_Target_");
	constexpr const TCHAR* WeaponPrefix = TEXT("DA_Weapon_");
	constexpr const TCHAR* AmmoPrefix = TEXT("DA_Ammo_");
	constexpr const TCHAR* ProjectilePrefix = TEXT("DA_Projectile_");

	FString JoinProblems(const TArray<FString>& Problems)
	{
		FString Joined;
		for (int32 Index = 0; Index < Problems.Num(); ++Index)
		{
			if (Index > 0)
			{
				Joined += TEXT("; ");
			}
			Joined += Problems[Index];
		}
		return Joined;
	}

	/** Full object path of an asset in the LoadObject form (/Pkg/Name.Object). */
	FString MakeObjectPath(const FString& PackageName, const FString& AssetName)
	{
		return PackageName + TEXT(".") + AssetName;
	}

	// ------------------------------------------------------------------
	// Phase 1: local 003/004 row validation (defense in depth)
	// ------------------------------------------------------------------

	void CollectLocalRowProblems(const FParsedCombatConfig& Config, TArray<FString>& OutProblems)
	{
		FString RowErrors;
		for (const FDamageProfile& Row : Config.DamageProfiles)
		{
			if (!ValidateDamageProfile(Row, RowErrors))
			{
				OutProblems.Add(FString::Printf(TEXT("damage_profiles: row '%s': %s"),
					*Row.DamageProfileId.ToString(), *RowErrors));
			}
		}
		for (const FAttackReaction& Row : Config.AttackReactions)
		{
			if (!ValidateAttackReaction(Row, RowErrors))
			{
				OutProblems.Add(FString::Printf(TEXT("attack_reactions: row '%s': %s"),
					*Row.ReactionId.ToString(), *RowErrors));
			}
		}
		for (const FTargetReaction& Row : Config.TargetReactions)
		{
			if (!ValidateTargetReaction(Row, RowErrors))
			{
				OutProblems.Add(FString::Printf(TEXT("target_reactions: row '%s': %s"),
					*Row.PolicyId.ToString(), *RowErrors));
			}
		}
		for (const FWeaponDefinition& Row : Config.Weapons)
		{
			if (!ValidateWeaponDefinition(Row, RowErrors))
			{
				OutProblems.Add(FString::Printf(TEXT("weapons: row '%s': %s"),
					*Row.WeaponId.ToString(), *RowErrors));
			}
		}
		for (const FAmmoType& Row : Config.AmmoTypes)
		{
			if (!ValidateAmmoType(Row, RowErrors))
			{
				OutProblems.Add(FString::Printf(TEXT("ammo_types: row '%s': %s"),
					*Row.AmmoId.ToString(), *RowErrors));
			}
		}
		for (const FProjectileDefinition& Row : Config.Projectiles)
		{
			if (!ValidateProjectileDefinition(Row, RowErrors))
			{
				OutProblems.Add(FString::Printf(TEXT("projectiles: row '%s': %s"),
					*Row.ProjectileId.ToString(), *RowErrors));
			}
		}
	}

	// ------------------------------------------------------------------
	// Phase 3: field mirrors
	// ------------------------------------------------------------------

	void ApplyDamageProfile(UCombatDamageProfileAsset& Asset, const FDamageProfile& Row, const FString& Revision)
	{
		Asset.DamageProfileId = Row.DamageProfileId;
		Asset.BaseDamage = Row.BaseDamage;
		Asset.AttackCoefficient = Row.AttackCoefficient;
		Asset.HitStunSeconds = Row.HitStunSeconds;
		Asset.KnockbackCmPerSecond = Row.KnockbackCmPerSecond;
		Asset.LaunchCmPerSecond = Row.LaunchCmPerSecond;
		Asset.HitStopSeconds = Row.HitStopSeconds;
		Asset.ConfigRevision = Revision;
	}

	void ApplyAttackReaction(UCombatAttackReactionAsset& Asset, const FAttackReaction& Row, const FString& Revision)
	{
		Asset.ReactionId = Row.ReactionId;
		Asset.ControlPenetration = static_cast<uint8>(Row.ControlPenetration);
		Asset.ConfigRevision = Revision;
	}

	void ApplyTargetReaction(UCombatTargetReactionAsset& Asset, const FTargetReaction& Row, const FString& Revision)
	{
		Asset.PolicyId = Row.PolicyId;
		Asset.bAllowStagger = Row.bAllowStagger;
		Asset.bAllowLaunch = Row.bAllowLaunch;
		Asset.bAllowKnockdown = Row.bAllowKnockdown;
		Asset.MaxLaunchesPerAirCycle = Row.MaxLaunchesPerAirCycle;
		Asset.LaunchZScales = Row.LaunchZScales;
		Asset.MaxAirTimeSeconds = Row.MaxAirTimeSeconds;
		Asset.PoiseMax = Row.PoiseMax;
		Asset.PoiseRegenSeconds = Row.PoiseRegenSeconds;
		Asset.KnockdownSeconds = Row.KnockdownSeconds;
		Asset.RecoveringSeconds = Row.RecoveringSeconds;
		Asset.bImmuneDamage = Row.bImmuneDamage;
		Asset.bImmuneControl = Row.bImmuneControl;
		Asset.bDeathResistant = Row.bDeathResistant;
		Asset.ConfigRevision = Revision;
	}

	void ApplyWeapon(UCombatWeaponAsset& Asset, const FWeaponDefinition& Row, const FString& Revision)
	{
		Asset.WeaponId = Row.WeaponId;
		Asset.FireMode = static_cast<uint8>(Row.FireMode);
		Asset.DamageProfileId = Row.DamageProfileId;
		Asset.AmmoId = Row.AmmoId;
		Asset.MagazineSize = Row.MagazineSize;
		Asset.FireRateRpm = Row.FireRateRpm;
		Asset.BurstCount = Row.BurstCount;
		Asset.PelletCount = Row.PelletCount;
		Asset.SpreadDegrees = Row.SpreadDegrees;
		Asset.RangeCm = Row.RangeCm;
		Asset.ProjectileId = Row.ProjectileId;
		Asset.MeleeAttackIds = Row.MeleeAttackIds;
		Asset.ConfigRevision = Revision;
	}

	void ApplyAmmoType(UCombatAmmoTypeAsset& Asset, const FAmmoType& Row, const FString& Revision)
	{
		Asset.AmmoId = Row.AmmoId;
		Asset.MaxReserve = Row.MaxReserve;
		Asset.MagazineSize = Row.MagazineSize;
		Asset.ConfigRevision = Revision;
	}

	void ApplyProjectile(UCombatProjectileAsset& Asset, const FProjectileDefinition& Row, const FString& Revision)
	{
		Asset.ProjectileId = Row.ProjectileId;
		Asset.Motion = static_cast<uint8>(Row.Motion);
		Asset.SpeedCmS = Row.SpeedCmS;
		Asset.LifetimeS = Row.LifetimeS;
		Asset.DamageProfileId = Row.DamageProfileId;
		Asset.PierceCount = Row.PierceCount;
		Asset.ExplosionRadiusCm = Row.ExplosionRadiusCm;
		Asset.ExplosionDamageProfileId = Row.ExplosionDamageProfileId;
		Asset.HomingTurnRateDegS = Row.HomingTurnRateDegS;
		Asset.ConfigRevision = Revision;
	}

	/**
	 * One pending asset of the build: the object name, its package name, the
	 * expected class and the resolved object (null when it must be created).
	 */
	struct FAssetTarget
	{
		FString AssetName;
		FString PackageName;
		UClass* ExpectedClass = nullptr;
		UObject* Object = nullptr;
		bool bCreated = false;
	};

	/**
	 * Resolves one target object without log spam: in-memory lookup first,
	 * then a disk/package check, then (and only then) a load. Returns the
	 * occupant of the object path (of any class) or null when the path is
	 * free. A load failure of an existing file surfaces as the loaded
	 * foreign-class object, so the caller refuses instead of overwriting.
	 */
	UObject* ResolveTargetObject(const FString& PackageName, const FString& AssetName, const FString& ObjectPath)
	{
		if (UObject* InMemory = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return InMemory;
		}

		const FString Filename = FPackageName::LongPackageNameToFilename(
			PackageName, FPackageName::GetAssetPackageExtension());
		if (!IFileManager::Get().FileExists(*Filename) && FindPackage(nullptr, *PackageName) == nullptr)
		{
			return nullptr; // the path is genuinely free; no load warning needed
		}

		// Something exists on disk (or the package is in memory without the
		// object): load it and report the occupant of any class.
		LoadObject<UObject>(nullptr, *ObjectPath);
		return FindObject<UObject>(nullptr, *ObjectPath);
	}

	/**
	 * Finds or creates the asset object for one target. Returns false (with
	 * OutProblems filled) when the path is occupied by a foreign class or the
	 * creation failed; objects created earlier in this call are listed in
	 * CreatedThisCall so the caller can roll them back.
	 */
	bool ResolveOrCreateAsset(FAssetTarget& Target, TArray<FString>& OutProblems,
		TArray<UObject*>& CreatedThisCall)
	{
		if (Target.Object)
		{
			return true;
		}

	UObject* Occupant = ResolveTargetObject(Target.PackageName, Target.AssetName,
		MakeObjectPath(Target.PackageName, Target.AssetName));
		if (Occupant)
		{
			if (Occupant->GetClass() == Target.ExpectedClass)
			{
				Target.Object = Occupant;
				return true;
			}
			OutProblems.Add(FString::Printf(TEXT("CombatDataAssetBuilder: asset name '%s' is occupied by a foreign object of class '%s' (expected '%s'); refusing to overwrite"),
				*MakeObjectPath(Target.PackageName, Target.AssetName), *Occupant->GetClass()->GetName(),
				*Target.ExpectedClass->GetName()));
			return false;
		}

		UPackage* Package = CreatePackage(*Target.PackageName);
		if (Package == nullptr)
		{
			OutProblems.Add(FString::Printf(TEXT("CombatDataAssetBuilder: the asset package '%s' could not be created"),
				*Target.PackageName));
			return false;
		}

		UObject* Created = NewObject<UObject>(Package, Target.ExpectedClass, FName(*Target.AssetName),
			RF_Public | RF_Standalone);
		if (Created == nullptr || Created->GetName() != Target.AssetName)
		{
			OutProblems.Add(FString::Printf(TEXT("CombatDataAssetBuilder: the asset object '%s' could not be created under package '%s'"),
				*Target.AssetName, *Target.PackageName));
			if (Created)
			{
				Created->MarkAsGarbage();
			}
			return false;
		}

		Target.Object = Created;
		Target.bCreated = true;
		CreatedThisCall.Add(Created);
		return true;
	}
}

FString FCombatDataAssetBuilder::GetDefaultAssetRootPath()
{
	return TEXT("/Game/UEMMO/Combat/Data");
}

FString FCombatDataAssetBuilder::MakeDamageProfileAssetName(FName Id)
{
	return FString::Printf(TEXT("%s%s"), DamagePrefix, *Id.ToString());
}

FString FCombatDataAssetBuilder::MakeAttackReactionAssetName(FName Id)
{
	return FString::Printf(TEXT("%s%s"), ReactionPrefix, *Id.ToString());
}

FString FCombatDataAssetBuilder::MakeTargetReactionAssetName(FName Id)
{
	return FString::Printf(TEXT("%s%s"), TargetPrefix, *Id.ToString());
}

FString FCombatDataAssetBuilder::MakeWeaponAssetName(FName Id)
{
	return FString::Printf(TEXT("%s%s"), WeaponPrefix, *Id.ToString());
}

FString FCombatDataAssetBuilder::MakeAmmoTypeAssetName(FName Id)
{
	return FString::Printf(TEXT("%s%s"), AmmoPrefix, *Id.ToString());
}

FString FCombatDataAssetBuilder::MakeProjectileAssetName(FName Id)
{
	return FString::Printf(TEXT("%s%s"), ProjectilePrefix, *Id.ToString());
}

bool FCombatDataAssetBuilder::BuildFromConfig(const FParsedCombatConfig& Config, UObject* Outer,
	FString& OutErrors, FCombatAssetBuildSummary* OutSummary)
{
	OutErrors.Reset();
	if (OutSummary)
	{
		*OutSummary = FCombatAssetBuildSummary();
	}

	// ------------------------------------------------------------------
	// Phase 0: resolve the target folder from the Outer contract.
	// ------------------------------------------------------------------
	FString RootPath;
	if (Outer == nullptr)
	{
		RootPath = GetDefaultAssetRootPath();
	}
	else
	{
		if (!Outer->IsA<UPackage>())
		{
			OutErrors = TEXT("CombatDataAssetBuilder: the outer must be null or a UPackage; a non-package object (and never anything inside the Transient package) is refused");
			return false;
		}
		UPackage* OuterPackage = static_cast<UPackage*>(Outer);
		if (OuterPackage == GetTransientPackage())
		{
			OutErrors = TEXT("CombatDataAssetBuilder: the outer must not be the Transient package; pass null for the default /Game/UEMMO/Combat/Data folder");
			return false;
		}
		RootPath = OuterPackage->GetName();
	}

	// ------------------------------------------------------------------
	// Phase 1: validate everything before touching anything.
	// ------------------------------------------------------------------
	FCombatCatalog Catalog;
	FString CatalogErrors;
	if (!FCombatCatalog::BuildFromParsed(Config, Catalog, CatalogErrors))
	{
		OutErrors = FString::Printf(TEXT("CombatDataAssetBuilder: the catalog build refused the config: %s"), *CatalogErrors);
		return false;
	}
	const FString Revision = Catalog.GetConfigRevision();

	TArray<FString> Problems;
	CollectLocalRowProblems(Config, Problems);
	Problems.Append(ValidateAllReferences(Config));
	if (Problems.Num() > 0)
	{
		OutErrors = FString::Printf(TEXT("CombatDataAssetBuilder: the config failed validation and no asset was touched: %s"),
			*JoinProblems(Problems));
		return false;
	}

	// ------------------------------------------------------------------
	// Phase 2: plan every target object (name, package, class) and refuse
	// foreign name occupiers before creating anything.
	// ------------------------------------------------------------------
	TArray<FAssetTarget> Targets;

	auto PlanTargets = [&Targets, &RootPath](FName Id, const TCHAR* Prefix, UClass* ExpectedClass)
	{
		FAssetTarget Target;
		Target.AssetName = FString::Printf(TEXT("%s%s"), Prefix, *Id.ToString());
		Target.PackageName = RootPath / Target.AssetName;
		Target.ExpectedClass = ExpectedClass;
		Targets.Add(MoveTemp(Target));
	};

	for (const FDamageProfile& Row : Config.DamageProfiles)
	{
		PlanTargets(Row.DamageProfileId, DamagePrefix, UCombatDamageProfileAsset::StaticClass());
	}
	for (const FAttackReaction& Row : Config.AttackReactions)
	{
		PlanTargets(Row.ReactionId, ReactionPrefix, UCombatAttackReactionAsset::StaticClass());
	}
	for (const FTargetReaction& Row : Config.TargetReactions)
	{
		PlanTargets(Row.PolicyId, TargetPrefix, UCombatTargetReactionAsset::StaticClass());
	}
	for (const FWeaponDefinition& Row : Config.Weapons)
	{
		PlanTargets(Row.WeaponId, WeaponPrefix, UCombatWeaponAsset::StaticClass());
	}
	for (const FAmmoType& Row : Config.AmmoTypes)
	{
		PlanTargets(Row.AmmoId, AmmoPrefix, UCombatAmmoTypeAsset::StaticClass());
	}
	for (const FProjectileDefinition& Row : Config.Projectiles)
	{
		PlanTargets(Row.ProjectileId, ProjectilePrefix, UCombatProjectileAsset::StaticClass());
	}

	TArray<UObject*> CreatedThisCall;
	for (FAssetTarget& Target : Targets)
	{
		if (!ResolveOrCreateAsset(Target, Problems, CreatedThisCall))
		{
			// Roll back the objects created in this call; nothing was saved,
			// nothing else was touched.
			for (UObject* Created : CreatedThisCall)
			{
				Created->MarkAsGarbage();
			}
			OutErrors = JoinProblems(Problems);
			return false;
		}
	}

	// ------------------------------------------------------------------
	// Phase 3: mirror every row onto its asset (created or existing).
	// ------------------------------------------------------------------
	int32 TargetIndex = 0;
	FCombatAssetBuildSummary LocalSummary;
	FCombatAssetBuildSummary* const Summary = OutSummary ? OutSummary : &LocalSummary;

	auto ApplyAndRecord = [Summary, &Revision](UObject* Object, bool bCreated, const FAssetTarget& Target, auto&& ApplyFields)
	{
		UPackage* Package = Object->GetOutermost();
		ApplyFields(Object, Revision);
		Package->MarkPackageDirty();

		if (bCreated)
		{
			Summary->CreatedObjectPaths.Add(MakeObjectPath(Target.PackageName, Target.AssetName));
		}
		else
		{
			Summary->UpdatedObjectPaths.Add(MakeObjectPath(Target.PackageName, Target.AssetName));
		}
	};

	for (const FDamageProfile& Row : Config.DamageProfiles)
	{
		const FAssetTarget& Target = Targets[TargetIndex++];
		ApplyAndRecord(Target.Object, Target.bCreated, Target,
			[&Row](UObject* Object, const FString& RevisionValue)
			{
				ApplyDamageProfile(*CastChecked<UCombatDamageProfileAsset>(Object), Row, RevisionValue);
			});
	}
	for (const FAttackReaction& Row : Config.AttackReactions)
	{
		const FAssetTarget& Target = Targets[TargetIndex++];
		ApplyAndRecord(Target.Object, Target.bCreated, Target,
			[&Row](UObject* Object, const FString& RevisionValue)
			{
				ApplyAttackReaction(*CastChecked<UCombatAttackReactionAsset>(Object), Row, RevisionValue);
			});
	}
	for (const FTargetReaction& Row : Config.TargetReactions)
	{
		const FAssetTarget& Target = Targets[TargetIndex++];
		ApplyAndRecord(Target.Object, Target.bCreated, Target,
			[&Row](UObject* Object, const FString& RevisionValue)
			{
				ApplyTargetReaction(*CastChecked<UCombatTargetReactionAsset>(Object), Row, RevisionValue);
			});
	}
	for (const FWeaponDefinition& Row : Config.Weapons)
	{
		const FAssetTarget& Target = Targets[TargetIndex++];
		ApplyAndRecord(Target.Object, Target.bCreated, Target,
			[&Row](UObject* Object, const FString& RevisionValue)
			{
				ApplyWeapon(*CastChecked<UCombatWeaponAsset>(Object), Row, RevisionValue);
			});
	}
	for (const FAmmoType& Row : Config.AmmoTypes)
	{
		const FAssetTarget& Target = Targets[TargetIndex++];
		ApplyAndRecord(Target.Object, Target.bCreated, Target,
			[&Row](UObject* Object, const FString& RevisionValue)
			{
				ApplyAmmoType(*CastChecked<UCombatAmmoTypeAsset>(Object), Row, RevisionValue);
			});
	}
	for (const FProjectileDefinition& Row : Config.Projectiles)
	{
		const FAssetTarget& Target = Targets[TargetIndex++];
		ApplyAndRecord(Target.Object, Target.bCreated, Target,
			[&Row](UObject* Object, const FString& RevisionValue)
			{
				ApplyProjectile(*CastChecked<UCombatProjectileAsset>(Object), Row, RevisionValue);
			});
	}

	// ------------------------------------------------------------------
	// Phase 4: save every touched package; record save problems.
	// ------------------------------------------------------------------
	TSet<FName> SavedPackages;
	for (const FAssetTarget& Target : Targets)
	{
		UPackage* Package = Target.Object->GetOutermost();
		if (Package == nullptr || SavedPackages.Contains(Package->GetFName()))
		{
			continue;
		}
		SavedPackages.Add(Package->GetFName());

		const FString Filename = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.bSlowTask = false;
		const bool bSaved = UPackage::SavePackage(Package, Target.Object, *Filename, SaveArgs);
		if (!bSaved)
		{
			Summary->SaveProblems.Add(FString::Printf(TEXT("the package '%s' could not be saved to '%s'"),
				*Package->GetName(), *Filename));
		}
	}

	Summary->TargetRootPath = RootPath;
	Summary->ConfigRevision = Revision;
	Summary->bSavedToDisk = Summary->SaveProblems.Num() == 0 && SavedPackages.Num() > 0;
	return true;
}

FString UCombatDataAssetBuilderLibrary::BuildFromSourceDirectory(const FString& SourceDataDir,
	int32& OutCreatedCount, int32& OutUpdatedCount, FString& OutConfigRevision)
{
	OutCreatedCount = 0;
	OutUpdatedCount = 0;
	OutConfigRevision.Reset();

	FString DataDir = SourceDataDir;
	if (FPaths::IsRelative(DataDir))
	{
		DataDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir(), DataDir);
	}

	// Parse every required table of the directory (M5-005 loader).
	FCombatDataTableSet Tables;
	TArray<FString> Problems;
	if (!LoadCombatDataDirectory(DataDir, Tables, Problems))
	{
		return FString::Printf(TEXT("CombatDataAssetBuilder: the source directory '%s' failed to parse: %s"),
			*DataDir, *JoinProblems(Problems));
	}

	// Assemble the candidate config (M5-006 pattern) and run the full
	// two-argument gate, which also covers the presentations table.
	FParsedCombatConfig Config;
	Config.SchemaVersion = 1;
	for (const TPair<FName, FDamageProfile>& Row : Tables.DamageProfiles)
	{
		Config.DamageProfiles.Add(Row.Value);
	}
	for (const TPair<FName, FAttackReaction>& Row : Tables.AttackReactions)
	{
		Config.AttackReactions.Add(Row.Value);
	}
	for (const TPair<FName, FTargetReaction>& Row : Tables.TargetReactions)
	{
		Config.TargetReactions.Add(Row.Value);
	}
	for (const TPair<FName, FWeaponDefinition>& Row : Tables.Weapons)
	{
		Config.Weapons.Add(Row.Value);
	}
	for (const TPair<FName, FAmmoType>& Row : Tables.AmmoTypes)
	{
		Config.AmmoTypes.Add(Row.Value);
	}
	for (const TPair<FName, FProjectileDefinition>& Row : Tables.Projectiles)
	{
		Config.Projectiles.Add(Row.Value);
	}

	TArray<FCombatPresentation> Presentations;
	for (const TPair<FName, FCombatPresentation>& Row : Tables.Presentations)
	{
		Presentations.Add(Row.Value);
	}

	Problems = ValidateAllReferences(Config, Presentations);
	if (Problems.Num() > 0)
	{
		return FString::Printf(TEXT("CombatDataAssetBuilder: the source directory '%s' failed the reference gate and no asset was touched: %s"),
			*DataDir, *JoinProblems(Problems));
	}

	// Build. The core builder records save problems on the summary; the
	// editor-facing entry treats them as a failure because a full editor
	// process must produce saved assets.
	FCombatAssetBuildSummary Summary;
	FString BuildErrors;
	if (!FCombatDataAssetBuilder::BuildFromConfig(Config, nullptr, BuildErrors, &Summary))
	{
		return BuildErrors;
	}
	if (Summary.SaveProblems.Num() > 0)
	{
		return FString::Printf(TEXT("CombatDataAssetBuilder: the assets were built but not all packages could be saved: %s"),
			*JoinProblems(Summary.SaveProblems));
	}

	OutCreatedCount = Summary.CreatedObjectPaths.Num();
	OutUpdatedCount = Summary.UpdatedObjectPaths.Num();
	OutConfigRevision = Summary.ConfigRevision;
	return FString();
}

// ---------------------------------------------------------------------------
// Console command: UEMMO.Combat.BuildDataAssets
// ---------------------------------------------------------------------------

namespace
{
	void RunBuildCombatDataAssetsCommand()
	{
		int32 Created = 0;
		int32 Updated = 0;
		FString Revision;
		const FString Errors = UCombatDataAssetBuilderLibrary::BuildFromSourceDirectory(
			TEXT("Data/CombatSystem"), Created, Updated, Revision);
		if (Errors.IsEmpty())
		{
			UE_LOG(LogTemp, Display, TEXT("UEMMO.Combat.BuildDataAssets: built the combat data assets (created=%d updated=%d revision=%s)"),
				Created, Updated, *Revision);
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("UEMMO.Combat.BuildDataAssets: refused, no asset was touched: %s"), *Errors);
		}
	}

	FAutoConsoleCommand GUEMMOBuildCombatDataAssets(
		TEXT("UEMMO.Combat.BuildDataAssets"),
		TEXT("Parses Data/CombatSystem, validates every required table and (re)builds the combat data assets under /Game/UEMMO/Combat/Data. Refuses without touching existing assets when any table is illegal."),
		FConsoleCommandDelegate::CreateStatic(&RunBuildCombatDataAssetsCommand));
}
