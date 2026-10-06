// M5-009: restart reload and cook references of the persisted combat data
// assets (M5 interface contract section 1, second owner 009 next to the
// M5-008 builder). M5-008 generated and saved the 31 assets under
// /Game/UEMMO/Combat/Data and explicitly left "the new-process reload
// verification of the saved assets" to this card; this file pins that:
//
// - The 31 saved .uasset files exist on disk under Content/UEMMO/Combat/Data
//   and load through LoadObject in a process that never built them: every
//   reloaded object carries RF_WasLoaded (set only when an object was
//   deserialized from an on-disk package), so the verification cannot be
//   satisfied by an in-memory transient of the generating process.
// - Every reloaded asset mirrors its source row exactly (the numbers, the
//   flags, the arrays in order, the enum values as their frozen numeric
//   value) - the saved form is the source form, not a defaulted copy.
// - ConfigRevision consistency: all reloaded assets carry one identical
//   ConfigRevision and it equals the deterministic catalog revision
//   recomputed in this process from the source tables, so the digest stamped
//   by the generating process and the digest recomputed after the restart
//   agree (contract section 6: stable across processes).
// - Reference closure of the persisted set: every cross-table id an asset
//   carries (weapon -> damage profile / ammo / projectile, projectile ->
//   damage profile / explosion profile) resolves to a reloaded asset of the
//   right class that itself declares that id. No dangling id survives the
//   save/reload round trip; a missing required asset is a hard failure,
//   never a fallback to a test double or a default row.
//
// Process model honesty: the automation harness (Scripts/TestAutomation.ps1)
// starts one fresh UnrealEditor-Cmd -game process per run, so a targeted
// -Filter UEMMO.Tasks.M5_009 run cold-loads every package from disk. In the
// full -Filter UEMMO.Tasks regression the M5-008 builder tests may run first
// in the same process; the builder resolves existing targets by loading them
// from disk before updating (CombatDataAssetBuilder.cpp ResolveTargetObject),
// so RF_WasLoaded stays set there too. If the persisted files ever went
// missing and something recreated them in memory only, RF_WasLoaded would be
// unset and these tests would correctly refuse - that is the transient
// illusion this card exists to catch.
//
// Not in this card's surface (recorded, not tested here): a literal cooked
// packaged-build (pak) evidence belongs to the segment-end card (M5-018 per
// the task card); the presentations table generates no asset (M5-017 owns
// hit presentation); Data/** stays frozen, so the two damage profile rows the
// shipped weapons/projectiles reference (physical_10, explosive_40) are
// supplied here with the exact M5-006/M5-008 alignment values.

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/Data/CombatCatalog.h"
#include "../Combat/Data/CombatDataTableParser.h"
#include "../Combat/Data/CombatDataAssetBuilder.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_009
{
	// ------------------------------------------------------------------
	// Small assertion helpers
	// ------------------------------------------------------------------

	static FString JoinProblems(const TArray<FString>& Problems)
	{
		FString Joined;
		for (int32 Index = 0; Index < Problems.Num(); ++Index)
		{
			if (Index > 0)
			{
				Joined += TEXT(" | ");
			}
			Joined += Problems[Index];
		}
		return Joined;
	}

	// ------------------------------------------------------------------
	// Config construction: the shipped tables plus the known sample gap rows
	// ------------------------------------------------------------------

	/**
	 * The shipped Data/CombatSystem tables, parsed by the M5-005 loader.
	 * Returns false (after reporting) when the shipped source is unparsable -
	 * that would be a source regression this test cannot neutralize.
	 */
	static bool LoadShippedTables(FAutomationTestBase& Test, FCombatDataTableSet& OutTables)
	{
		TArray<FString> LoadProblems;
		if (!LoadCombatDataDirectory(FPaths::ProjectDir() / TEXT("Data") / TEXT("CombatSystem"), OutTables, LoadProblems))
		{
			Test.AddError(FString::Printf(TEXT("the shipped Data/CombatSystem tables must load (%s)"),
				*JoinProblems(LoadProblems)));
			return false;
		}
		return true;
	}

	/** Adapts the parser output set into the catalog candidate struct (M5-006 pattern). */
	static FParsedCombatConfig ToParsedConfig(const FCombatDataTableSet& Tables)
	{
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
		return Config;
	}

	/**
	 * Adds the damage profiles the shipped 004 sample rows reference but the
	 * shipped 003 sample table does not define yet (physical_10,
	 * explosive_40). Exactly the values M5-006 and M5-008 aligned with, which
	 * are the values the committed assets were generated from. Idempotent: a
	 * profile already present is kept, so this stays correct once the shipped
	 * table is aligned.
	 */
	static void AlignShippedSampleDamageProfiles(FParsedCombatConfig& Config)
	{
		bool bHasPhysical10 = false;
		bool bHasExplosive40 = false;
		for (const FDamageProfile& Row : Config.DamageProfiles)
		{
			bHasPhysical10 = bHasPhysical10 || Row.DamageProfileId == FName(TEXT("physical_10"));
			bHasExplosive40 = bHasExplosive40 || Row.DamageProfileId == FName(TEXT("explosive_40"));
		}
		if (!bHasPhysical10)
		{
			FDamageProfile Bullet;
			Bullet.DamageProfileId = TEXT("physical_10");
			Bullet.BaseDamage = 10.0f;
			Bullet.AttackCoefficient = 1.0f;
			Bullet.HitStunSeconds = 0.2f;
			Bullet.KnockbackCmPerSecond = 90.0f;
			Bullet.LaunchCmPerSecond = 0.0f;
			Bullet.HitStopSeconds = 0.04f;
			Config.DamageProfiles.Add(Bullet);
		}
		if (!bHasExplosive40)
		{
			FDamageProfile Explosion;
			Explosion.DamageProfileId = TEXT("explosive_40");
			Explosion.BaseDamage = 40.0f;
			Explosion.AttackCoefficient = 1.0f;
			Explosion.HitStunSeconds = 0.2f;
			Explosion.KnockbackCmPerSecond = 90.0f;
			Explosion.LaunchCmPerSecond = 0.0f;
			Explosion.HitStopSeconds = 0.04f;
			Config.DamageProfiles.Add(Explosion);
		}
	}

	/**
	 * The shipped combat config with the known sample gap aligned: the legal
	 * A-segment candidate set whose 31 rows the committed assets mirror.
	 * Returns false (after reporting) when the shipped source does not parse.
	 */
	static bool MakeAlignedShippedConfig(FAutomationTestBase& Test, FParsedCombatConfig& OutConfig)
	{
		FCombatDataTableSet Tables;
		if (!LoadShippedTables(Test, Tables))
		{
			return false;
		}
		OutConfig = ToParsedConfig(Tables);
		AlignShippedSampleDamageProfiles(OutConfig);
		return true;
	}

	/** The deterministic revision of one config via the frozen catalog builder. */
	static FString ComputeExpectedRevision(FAutomationTestBase& Test, const FParsedCombatConfig& Config)
	{
		FCombatCatalog Catalog;
		FString BuildErrors;
		if (!FCombatCatalog::BuildFromParsed(Config, Catalog, BuildErrors))
		{
			Test.AddError(FString::Printf(TEXT("the aligned config must pass the catalog build (%s)"), *BuildErrors));
			return FString();
		}
		return Catalog.GetConfigRevision();
	}

	// ------------------------------------------------------------------
	// Expected asset naming, disk locations and reload helpers
	// ------------------------------------------------------------------

	static FString ExpectedAssetNameFor(const TCHAR* Prefix, const FName Id)
	{
		return FString::Printf(TEXT("%s%s"), Prefix, *Id.ToString());
	}

	/** Full object path of one saved asset (LoadObject form: /Pkg/Name.Object). */
	static FString ExpectedObjectPath(const FString& AssetName)
	{
		return FCombatDataAssetBuilder::GetDefaultAssetRootPath() / AssetName + TEXT(".") + AssetName;
	}

	/** The one-asset package of a saved asset (/Game/UEMMO/Combat/Data/<Name>). */
	static FString ExpectedPackagePath(const FString& AssetName)
	{
		return FCombatDataAssetBuilder::GetDefaultAssetRootPath() / AssetName;
	}

	/** The saved .uasset file of one asset on disk (Content/UEMMO/Combat/Data/<Name>.uasset). */
	static FString DiskFilePathFor(const FString& AssetName)
	{
		return (FPaths::ProjectContentDir() / TEXT("UEMMO/Combat/Data")) / (AssetName + TEXT(".uasset"));
	}

	/** Legal root path rule: the asset must live under /Game/UEMMO/Combat/Data. */
	static bool IsUnderLegalAssetRoot(const FString& PackagePath)
	{
		return PackagePath.StartsWith(FCombatDataAssetBuilder::GetDefaultAssetRootPath() + TEXT("/"));
	}

	/**
	 * Reloads one saved asset from its package. In a fresh process this is a
	 * cold deserialization of the committed .uasset; in a warm process it
	 * returns the object the builder itself loaded from disk.
	 */
	template <typename AssetClass>
	static AssetClass* ReloadAsset(const FString& AssetName)
	{
		return LoadObject<AssetClass>(nullptr, *ExpectedObjectPath(AssetName));
	}

	/**
	 * Provenance of one reloaded asset: it was deserialized from the saved
	 * package (RF_WasLoaded is set only on objects loaded from an on-disk
	 * package - an in-memory NewObject of the running process never carries
	 * it), it lives in its own one-asset package under the legal root, and no
	 * numeric-suffix duplicate package materialized next to it on disk.
	 */
	static void CheckReloadProvenance(FAutomationTestBase& Test, const UObject* Asset, const FString& AssetName)
	{
		const FString What = FString::Printf(TEXT("reloaded asset '%s'"), *AssetName);
		if (!Test.TestNotNull(*What, Asset))
		{
			return;
		}
		Test.TestTrue(*(What + TEXT(" was deserialized from the saved package (RF_WasLoaded)")),
			Asset->HasAnyFlags(RF_WasLoaded));

		const UPackage* Outermost = Asset->GetOutermost();
		if (Test.TestNotNull(*(What + TEXT(" has an outermost package")), Outermost))
		{
			const FString PackagePath = Outermost->GetName();
			Test.TestEqual(*(What + TEXT(" lives in its own one-asset package")), PackagePath, ExpectedPackagePath(AssetName));
			Test.TestTrue(*(What + TEXT(" package is under the legal asset root")), IsUnderLegalAssetRoot(PackagePath));
		}

		Test.TestFalse(*(What + TEXT(" must have no _1 duplicate package file on disk")),
			IFileManager::Get().FileExists(*DiskFilePathFor(AssetName + TEXT("_1"))));
		Test.TestFalse(*(What + TEXT(" must have no _2 duplicate package file on disk")),
			IFileManager::Get().FileExists(*DiskFilePathFor(AssetName + TEXT("_2"))));
	}

	/** The saved .uasset file of one asset exists on disk before any load. */
	static void CheckSavedFileOnDisk(FAutomationTestBase& Test, const FString& AssetName)
	{
		const FString FilePath = DiskFilePathFor(AssetName);
		Test.TestTrue(FString::Printf(TEXT("the saved package file '%s' exists on disk"), *FilePath),
			IFileManager::Get().FileExists(*FilePath));
	}

	// ------------------------------------------------------------------
	// Per-type field mirrors: every reloaded asset field must equal its row
	// ------------------------------------------------------------------

	static void CheckDamageProfileMirror(FAutomationTestBase& Test, const FDamageProfile& Row,
		const UCombatDamageProfileAsset* Asset)
	{
		const FString What = FString::Printf(TEXT("reloaded damage profile asset 'DA_Damage_%s'"), *Row.DamageProfileId.ToString());
		if (!Test.TestNotNull(*What, Asset))
		{
			return;
		}
		Test.TestEqual(*(What + " class"), Asset->GetClass(), UCombatDamageProfileAsset::StaticClass());
		Test.TestEqual(*(What + " DamageProfileId"), Asset->DamageProfileId, Row.DamageProfileId);
		Test.TestEqual(*(What + " BaseDamage"), Asset->BaseDamage, Row.BaseDamage);
		Test.TestEqual(*(What + " AttackCoefficient"), Asset->AttackCoefficient, Row.AttackCoefficient);
		Test.TestEqual(*(What + " HitStunSeconds"), Asset->HitStunSeconds, Row.HitStunSeconds);
		Test.TestEqual(*(What + " KnockbackCmPerSecond"), Asset->KnockbackCmPerSecond, Row.KnockbackCmPerSecond);
		Test.TestEqual(*(What + " LaunchCmPerSecond"), Asset->LaunchCmPerSecond, Row.LaunchCmPerSecond);
		Test.TestEqual(*(What + " HitStopSeconds"), Asset->HitStopSeconds, Row.HitStopSeconds);
	}

	static void CheckAttackReactionMirror(FAutomationTestBase& Test, const FAttackReaction& Row,
		const UCombatAttackReactionAsset* Asset)
	{
		const FString What = FString::Printf(TEXT("reloaded attack reaction asset 'DA_Reaction_%s'"), *Row.ReactionId.ToString());
		if (!Test.TestNotNull(*What, Asset))
		{
			return;
		}
		Test.TestEqual(*(What + " class"), Asset->GetClass(), UCombatAttackReactionAsset::StaticClass());
		Test.TestEqual(*(What + " ReactionId"), Asset->ReactionId, Row.ReactionId);
		Test.TestEqual(*(What + " ControlPenetration"), Asset->ControlPenetration,
			static_cast<uint8>(Row.ControlPenetration));
	}

	static void CheckTargetReactionMirror(FAutomationTestBase& Test, const FTargetReaction& Row,
		const UCombatTargetReactionAsset* Asset)
	{
		const FString What = FString::Printf(TEXT("reloaded target reaction asset 'DA_Target_%s'"), *Row.PolicyId.ToString());
		if (!Test.TestNotNull(*What, Asset))
		{
			return;
		}
		Test.TestEqual(*(What + " class"), Asset->GetClass(), UCombatTargetReactionAsset::StaticClass());
		Test.TestEqual(*(What + " PolicyId"), Asset->PolicyId, Row.PolicyId);
		Test.TestEqual(*(What + " bAllowStagger"), Asset->bAllowStagger, Row.bAllowStagger);
		Test.TestEqual(*(What + " bAllowLaunch"), Asset->bAllowLaunch, Row.bAllowLaunch);
		Test.TestEqual(*(What + " bAllowKnockdown"), Asset->bAllowKnockdown, Row.bAllowKnockdown);
		Test.TestEqual(*(What + " MaxLaunchesPerAirCycle"), Asset->MaxLaunchesPerAirCycle, Row.MaxLaunchesPerAirCycle);
		Test.TestEqual(*(What + " LaunchZScales.Num()"), Asset->LaunchZScales.Num(), Row.LaunchZScales.Num());
		for (int32 Index = 0; Index < Row.LaunchZScales.Num() && Index < Asset->LaunchZScales.Num(); ++Index)
		{
			Test.TestEqual(*FString::Printf(TEXT("%s LaunchZScales[%d]"), *What, Index),
				Asset->LaunchZScales[Index], Row.LaunchZScales[Index]);
		}
		Test.TestEqual(*(What + " MaxAirTimeSeconds"), Asset->MaxAirTimeSeconds, Row.MaxAirTimeSeconds);
		Test.TestEqual(*(What + " PoiseMax"), Asset->PoiseMax, Row.PoiseMax);
		Test.TestEqual(*(What + " PoiseRegenSeconds"), Asset->PoiseRegenSeconds, Row.PoiseRegenSeconds);
		Test.TestEqual(*(What + " KnockdownSeconds"), Asset->KnockdownSeconds, Row.KnockdownSeconds);
		Test.TestEqual(*(What + " RecoveringSeconds"), Asset->RecoveringSeconds, Row.RecoveringSeconds);
		Test.TestEqual(*(What + " bImmuneDamage"), Asset->bImmuneDamage, Row.bImmuneDamage);
		Test.TestEqual(*(What + " bImmuneControl"), Asset->bImmuneControl, Row.bImmuneControl);
		Test.TestEqual(*(What + " bDeathResistant"), Asset->bDeathResistant, Row.bDeathResistant);
	}

	static void CheckWeaponMirror(FAutomationTestBase& Test, const FWeaponDefinition& Row,
		const UCombatWeaponAsset* Asset)
	{
		const FString What = FString::Printf(TEXT("reloaded weapon asset 'DA_Weapon_%s'"), *Row.WeaponId.ToString());
		if (!Test.TestNotNull(*What, Asset))
		{
			return;
		}
		Test.TestEqual(*(What + " class"), Asset->GetClass(), UCombatWeaponAsset::StaticClass());
		Test.TestEqual(*(What + " WeaponId"), Asset->WeaponId, Row.WeaponId);
		Test.TestEqual(*(What + " FireMode"), Asset->FireMode, static_cast<uint8>(Row.FireMode));
		Test.TestEqual(*(What + " DamageProfileId"), Asset->DamageProfileId, Row.DamageProfileId);
		Test.TestEqual(*(What + " AmmoId"), Asset->AmmoId, Row.AmmoId);
		Test.TestEqual(*(What + " MagazineSize"), Asset->MagazineSize, Row.MagazineSize);
		Test.TestEqual(*(What + " FireRateRpm"), Asset->FireRateRpm, Row.FireRateRpm);
		Test.TestEqual(*(What + " BurstCount"), Asset->BurstCount, Row.BurstCount);
		Test.TestEqual(*(What + " PelletCount"), Asset->PelletCount, Row.PelletCount);
		Test.TestEqual(*(What + " SpreadDegrees"), Asset->SpreadDegrees, Row.SpreadDegrees);
		Test.TestEqual(*(What + " RangeCm"), Asset->RangeCm, Row.RangeCm);
		Test.TestEqual(*(What + " ProjectileId"), Asset->ProjectileId, Row.ProjectileId);
		Test.TestEqual(*(What + " MeleeAttackIds.Num()"), Asset->MeleeAttackIds.Num(), Row.MeleeAttackIds.Num());
		for (int32 Index = 0; Index < Row.MeleeAttackIds.Num() && Index < Asset->MeleeAttackIds.Num(); ++Index)
		{
			Test.TestEqual(*FString::Printf(TEXT("%s MeleeAttackIds[%d]"), *What, Index),
				Asset->MeleeAttackIds[Index], Row.MeleeAttackIds[Index]);
		}
	}

	static void CheckAmmoMirror(FAutomationTestBase& Test, const FAmmoType& Row,
		const UCombatAmmoTypeAsset* Asset)
	{
		const FString What = FString::Printf(TEXT("reloaded ammo asset 'DA_Ammo_%s'"), *Row.AmmoId.ToString());
		if (!Test.TestNotNull(*What, Asset))
		{
			return;
		}
		Test.TestEqual(*(What + " class"), Asset->GetClass(), UCombatAmmoTypeAsset::StaticClass());
		Test.TestEqual(*(What + " AmmoId"), Asset->AmmoId, Row.AmmoId);
		Test.TestEqual(*(What + " MaxReserve"), Asset->MaxReserve, Row.MaxReserve);
		Test.TestEqual(*(What + " MagazineSize"), Asset->MagazineSize, Row.MagazineSize);
	}

	static void CheckProjectileMirror(FAutomationTestBase& Test, const FProjectileDefinition& Row,
		const UCombatProjectileAsset* Asset)
	{
		const FString What = FString::Printf(TEXT("reloaded projectile asset 'DA_Projectile_%s'"), *Row.ProjectileId.ToString());
		if (!Test.TestNotNull(*What, Asset))
		{
			return;
		}
		Test.TestEqual(*(What + " class"), Asset->GetClass(), UCombatProjectileAsset::StaticClass());
		Test.TestEqual(*(What + " ProjectileId"), Asset->ProjectileId, Row.ProjectileId);
		Test.TestEqual(*(What + " Motion"), Asset->Motion, static_cast<uint8>(Row.Motion));
		Test.TestEqual(*(What + " SpeedCmS"), Asset->SpeedCmS, Row.SpeedCmS);
		Test.TestEqual(*(What + " LifetimeS"), Asset->LifetimeS, Row.LifetimeS);
		Test.TestEqual(*(What + " DamageProfileId"), Asset->DamageProfileId, Row.DamageProfileId);
		Test.TestEqual(*(What + " PierceCount"), Asset->PierceCount, Row.PierceCount);
		Test.TestEqual(*(What + " ExplosionRadiusCm"), Asset->ExplosionRadiusCm, Row.ExplosionRadiusCm);
		Test.TestEqual(*(What + " ExplosionDamageProfileId"), Asset->ExplosionDamageProfileId, Row.ExplosionDamageProfileId);
		Test.TestEqual(*(What + " HomingTurnRateDegS"), Asset->HomingTurnRateDegS, Row.HomingTurnRateDegS);
	}

	// ------------------------------------------------------------------
	// The tests
	// ------------------------------------------------------------------

	/**
	 * The 31 saved assets load in a fresh process: each .uasset exists on
	 * disk under Content/UEMMO/Combat/Data, LoadObject returns an object of
	 * the exact asset class, that object was deserialized from the saved
	 * package (RF_WasLoaded - no transient of the generating process), it
	 * lives in its own one-asset package under the legal root and no
	 * numeric-suffix duplicate package exists. A missing persisted asset is a
	 * hard failure naming the file - no test double and no default row is
	 * substituted.
	 */
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_009SavedAssetsLoadFromDiskInFreshProcess,
		"UEMMO.Tasks.M5_009.SavedAssetsLoadFromDiskInFreshProcess",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_009SavedAssetsLoadFromDiskInFreshProcess::RunTest(const FString& Parameters)
	{
		FParsedCombatConfig Config;
		if (!MakeAlignedShippedConfig(*this, Config))
		{
			return true;
		}
		const int32 ExpectedAssets = Config.DamageProfiles.Num() + Config.AttackReactions.Num()
			+ Config.TargetReactions.Num() + Config.Weapons.Num() + Config.AmmoTypes.Num()
			+ Config.Projectiles.Num();

		int32 ReloadedAssets = 0;
		for (const FDamageProfile& Row : Config.DamageProfiles)
		{
			const FString Name = ExpectedAssetNameFor(TEXT("DA_Damage_"), Row.DamageProfileId);
			CheckSavedFileOnDisk(*this, Name);
			const UCombatDamageProfileAsset* Asset = ReloadAsset<UCombatDamageProfileAsset>(Name);
			if (TestNotNull(*FString::Printf(TEXT("the saved damage profile asset '%s' loads"), *Name), Asset))
			{
				++ReloadedAssets;
				TestEqual(*FString::Printf(TEXT("asset '%s' class"), *Name),
					Asset->GetClass(), UCombatDamageProfileAsset::StaticClass());
				CheckReloadProvenance(*this, Asset, Name);
			}
		}
		for (const FAttackReaction& Row : Config.AttackReactions)
		{
			const FString Name = ExpectedAssetNameFor(TEXT("DA_Reaction_"), Row.ReactionId);
			CheckSavedFileOnDisk(*this, Name);
			const UCombatAttackReactionAsset* Asset = ReloadAsset<UCombatAttackReactionAsset>(Name);
			if (TestNotNull(*FString::Printf(TEXT("the saved attack reaction asset '%s' loads"), *Name), Asset))
			{
				++ReloadedAssets;
				TestEqual(*FString::Printf(TEXT("asset '%s' class"), *Name),
					Asset->GetClass(), UCombatAttackReactionAsset::StaticClass());
				CheckReloadProvenance(*this, Asset, Name);
			}
		}
		for (const FTargetReaction& Row : Config.TargetReactions)
		{
			const FString Name = ExpectedAssetNameFor(TEXT("DA_Target_"), Row.PolicyId);
			CheckSavedFileOnDisk(*this, Name);
			const UCombatTargetReactionAsset* Asset = ReloadAsset<UCombatTargetReactionAsset>(Name);
			if (TestNotNull(*FString::Printf(TEXT("the saved target reaction asset '%s' loads"), *Name), Asset))
			{
				++ReloadedAssets;
				TestEqual(*FString::Printf(TEXT("asset '%s' class"), *Name),
					Asset->GetClass(), UCombatTargetReactionAsset::StaticClass());
				CheckReloadProvenance(*this, Asset, Name);
			}
		}
		for (const FWeaponDefinition& Row : Config.Weapons)
		{
			const FString Name = ExpectedAssetNameFor(TEXT("DA_Weapon_"), Row.WeaponId);
			CheckSavedFileOnDisk(*this, Name);
			const UCombatWeaponAsset* Asset = ReloadAsset<UCombatWeaponAsset>(Name);
			if (TestNotNull(*FString::Printf(TEXT("the saved weapon asset '%s' loads"), *Name), Asset))
			{
				++ReloadedAssets;
				TestEqual(*FString::Printf(TEXT("asset '%s' class"), *Name),
					Asset->GetClass(), UCombatWeaponAsset::StaticClass());
				CheckReloadProvenance(*this, Asset, Name);
			}
		}
		for (const FAmmoType& Row : Config.AmmoTypes)
		{
			const FString Name = ExpectedAssetNameFor(TEXT("DA_Ammo_"), Row.AmmoId);
			CheckSavedFileOnDisk(*this, Name);
			const UCombatAmmoTypeAsset* Asset = ReloadAsset<UCombatAmmoTypeAsset>(Name);
			if (TestNotNull(*FString::Printf(TEXT("the saved ammo asset '%s' loads"), *Name), Asset))
			{
				++ReloadedAssets;
				TestEqual(*FString::Printf(TEXT("asset '%s' class"), *Name),
					Asset->GetClass(), UCombatAmmoTypeAsset::StaticClass());
				CheckReloadProvenance(*this, Asset, Name);
			}
		}
		for (const FProjectileDefinition& Row : Config.Projectiles)
		{
			const FString Name = ExpectedAssetNameFor(TEXT("DA_Projectile_"), Row.ProjectileId);
			CheckSavedFileOnDisk(*this, Name);
			const UCombatProjectileAsset* Asset = ReloadAsset<UCombatProjectileAsset>(Name);
			if (TestNotNull(*FString::Printf(TEXT("the saved projectile asset '%s' loads"), *Name), Asset))
			{
				++ReloadedAssets;
				TestEqual(*FString::Printf(TEXT("asset '%s' class"), *Name),
					Asset->GetClass(), UCombatProjectileAsset::StaticClass());
				CheckReloadProvenance(*this, Asset, Name);
			}
		}

		TestEqual(TEXT("every source row has its reloaded asset"), ReloadedAssets, ExpectedAssets);
		AddInfo(FString::Printf(TEXT("reloaded %d saved combat data assets from %s (31 assets on the shipped+aligned sample config)"),
			ReloadedAssets, *FCombatDataAssetBuilder::GetDefaultAssetRootPath()));
		return true;
	}

	/**
	 * Every reloaded asset mirrors its source row exactly - the numbers, the
	 * flags, the arrays in order and the frozen enum values as their numeric
	 * value. The saved form is the source form: a restart reload must not
	 * surface defaulted or drifted copies.
	 */
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_009ReloadedFieldsMirrorSourceTables,
		"UEMMO.Tasks.M5_009.ReloadedFieldsMirrorSourceTables",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_009ReloadedFieldsMirrorSourceTables::RunTest(const FString& Parameters)
	{
		FParsedCombatConfig Config;
		if (!MakeAlignedShippedConfig(*this, Config))
		{
			return true;
		}

		for (const FDamageProfile& Row : Config.DamageProfiles)
		{
			CheckDamageProfileMirror(*this, Row,
				ReloadAsset<UCombatDamageProfileAsset>(ExpectedAssetNameFor(TEXT("DA_Damage_"), Row.DamageProfileId)));
		}
		for (const FAttackReaction& Row : Config.AttackReactions)
		{
			CheckAttackReactionMirror(*this, Row,
				ReloadAsset<UCombatAttackReactionAsset>(ExpectedAssetNameFor(TEXT("DA_Reaction_"), Row.ReactionId)));
		}
		for (const FTargetReaction& Row : Config.TargetReactions)
		{
			CheckTargetReactionMirror(*this, Row,
				ReloadAsset<UCombatTargetReactionAsset>(ExpectedAssetNameFor(TEXT("DA_Target_"), Row.PolicyId)));
		}
		for (const FWeaponDefinition& Row : Config.Weapons)
		{
			CheckWeaponMirror(*this, Row,
				ReloadAsset<UCombatWeaponAsset>(ExpectedAssetNameFor(TEXT("DA_Weapon_"), Row.WeaponId)));
		}
		for (const FAmmoType& Row : Config.AmmoTypes)
		{
			CheckAmmoMirror(*this, Row,
				ReloadAsset<UCombatAmmoTypeAsset>(ExpectedAssetNameFor(TEXT("DA_Ammo_"), Row.AmmoId)));
		}
		for (const FProjectileDefinition& Row : Config.Projectiles)
		{
			CheckProjectileMirror(*this, Row,
				ReloadAsset<UCombatProjectileAsset>(ExpectedAssetNameFor(TEXT("DA_Projectile_"), Row.ProjectileId)));
		}

		// Spot pins against double faults: the reload keeps the legacy M1
		// numbers, the separate boss immunity axes, the five-pellet sample,
		// the homing turn rate and the explosive rocket profile.
		const UCombatDamageProfileAsset* Light01 = ReloadAsset<UCombatDamageProfileAsset>(TEXT("DA_Damage_light_01"));
		if (Light01)
		{
			TestEqual(TEXT("light_01 keeps the legacy base damage after reload"), Light01->BaseDamage, 10.0f);
			TestEqual(TEXT("light_01 keeps the legacy hit stun after reload"), Light01->HitStunSeconds, 0.22f);
			TestEqual(TEXT("light_01 keeps the legacy launch after reload"), Light01->LaunchCmPerSecond, 0.0f);
		}
		const UCombatTargetReactionAsset* Boss = ReloadAsset<UCombatTargetReactionAsset>(TEXT("DA_Target_boss"));
		if (Boss)
		{
			TestEqual(TEXT("boss keeps control immunity after reload"), Boss->bImmuneControl, true);
			TestEqual(TEXT("boss is death resistant but not damage immune after reload"),
				Boss->bDeathResistant && !Boss->bImmuneDamage, true);
		}
		const UCombatWeaponAsset* Pellet = ReloadAsset<UCombatWeaponAsset>(TEXT("DA_Weapon_weapon_pellet_sample"));
		if (Pellet)
		{
			TestEqual(TEXT("the pellet sample keeps five pellets after reload"), Pellet->PelletCount, 5);
		}
		const UCombatProjectileAsset* Missile = ReloadAsset<UCombatProjectileAsset>(TEXT("DA_Projectile_missile_homing"));
		if (Missile)
		{
			TestEqual(TEXT("the homing missile keeps its turn rate after reload"), Missile->HomingTurnRateDegS, 180.0f);
		}
		const UCombatProjectileAsset* Rocket = ReloadAsset<UCombatProjectileAsset>(TEXT("DA_Projectile_rocket_explosive"));
		if (Rocket)
		{
			TestEqual(TEXT("the explosive rocket keeps its explosion radius after reload"), Rocket->ExplosionRadiusCm, 300.0f);
			TestEqual(TEXT("the explosive rocket keeps its explosion profile after reload"),
				Rocket->ExplosionDamageProfileId, FName(TEXT("explosive_40")));
		}
		return true;
	}

	/**
	 * ConfigRevision consistency of the reloaded set: every asset carries the
	 * same non-empty revision digest (16 lowercase hex characters) and it
	 * equals the catalog revision recomputed in this process from the source
	 * tables - the digest stamped by the generating process and the digest of
	 * the restarted process agree, which is the cross-process stability the
	 * contract requires (section 6).
	 */
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_009ConfigRevisionConsistentAcrossReloadedAssets,
		"UEMMO.Tasks.M5_009.ConfigRevisionConsistentAcrossReloadedAssets",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_009ConfigRevisionConsistentAcrossReloadedAssets::RunTest(const FString& Parameters)
	{
		FParsedCombatConfig Config;
		if (!MakeAlignedShippedConfig(*this, Config))
		{
			return true;
		}
		const FString ExpectedRevision = ComputeExpectedRevision(*this, Config);
		if (ExpectedRevision.IsEmpty())
		{
			return true;
		}

		// Digest shape: 16 lowercase hex characters (catalog contract).
		TestEqual(TEXT("the recomputed revision is 16 characters long"), ExpectedRevision.Len(), 16);
		bool bAllHexLower = true;
		for (TCHAR Character : ExpectedRevision)
		{
			const bool bDigit = Character >= TEXT('0') && Character <= TEXT('9');
			const bool bLowerHex = Character >= TEXT('a') && Character <= TEXT('f');
			bAllHexLower = bAllHexLower && (bDigit || bLowerHex);
		}
		TestTrue(TEXT("the recomputed revision is lowercase hex"), bAllHexLower);

		TSet<FString> DistinctRevisions;
		int32 CheckedAssets = 0;
		for (const FDamageProfile& Row : Config.DamageProfiles)
		{
			const UCombatDamageProfileAsset* Asset = ReloadAsset<UCombatDamageProfileAsset>(
				ExpectedAssetNameFor(TEXT("DA_Damage_"), Row.DamageProfileId));
			if (Asset)
			{
				++CheckedAssets;
				DistinctRevisions.Add(Asset->ConfigRevision);
				TestEqual(*FString::Printf(TEXT("asset 'DA_Damage_%s' ConfigRevision"), *Row.DamageProfileId.ToString()),
					Asset->ConfigRevision, ExpectedRevision);
			}
		}
		for (const FAttackReaction& Row : Config.AttackReactions)
		{
			const UCombatAttackReactionAsset* Asset = ReloadAsset<UCombatAttackReactionAsset>(
				ExpectedAssetNameFor(TEXT("DA_Reaction_"), Row.ReactionId));
			if (Asset)
			{
				++CheckedAssets;
				DistinctRevisions.Add(Asset->ConfigRevision);
				TestEqual(*FString::Printf(TEXT("asset 'DA_Reaction_%s' ConfigRevision"), *Row.ReactionId.ToString()),
					Asset->ConfigRevision, ExpectedRevision);
			}
		}
		for (const FTargetReaction& Row : Config.TargetReactions)
		{
			const UCombatTargetReactionAsset* Asset = ReloadAsset<UCombatTargetReactionAsset>(
				ExpectedAssetNameFor(TEXT("DA_Target_"), Row.PolicyId));
			if (Asset)
			{
				++CheckedAssets;
				DistinctRevisions.Add(Asset->ConfigRevision);
				TestEqual(*FString::Printf(TEXT("asset 'DA_Target_%s' ConfigRevision"), *Row.PolicyId.ToString()),
					Asset->ConfigRevision, ExpectedRevision);
			}
		}
		for (const FWeaponDefinition& Row : Config.Weapons)
		{
			const UCombatWeaponAsset* Asset = ReloadAsset<UCombatWeaponAsset>(
				ExpectedAssetNameFor(TEXT("DA_Weapon_"), Row.WeaponId));
			if (Asset)
			{
				++CheckedAssets;
				DistinctRevisions.Add(Asset->ConfigRevision);
				TestEqual(*FString::Printf(TEXT("asset 'DA_Weapon_%s' ConfigRevision"), *Row.WeaponId.ToString()),
					Asset->ConfigRevision, ExpectedRevision);
			}
		}
		for (const FAmmoType& Row : Config.AmmoTypes)
		{
			const UCombatAmmoTypeAsset* Asset = ReloadAsset<UCombatAmmoTypeAsset>(
				ExpectedAssetNameFor(TEXT("DA_Ammo_"), Row.AmmoId));
			if (Asset)
			{
				++CheckedAssets;
				DistinctRevisions.Add(Asset->ConfigRevision);
				TestEqual(*FString::Printf(TEXT("asset 'DA_Ammo_%s' ConfigRevision"), *Row.AmmoId.ToString()),
					Asset->ConfigRevision, ExpectedRevision);
			}
		}
		for (const FProjectileDefinition& Row : Config.Projectiles)
		{
			const UCombatProjectileAsset* Asset = ReloadAsset<UCombatProjectileAsset>(
				ExpectedAssetNameFor(TEXT("DA_Projectile_"), Row.ProjectileId));
			if (Asset)
			{
				++CheckedAssets;
				DistinctRevisions.Add(Asset->ConfigRevision);
				TestEqual(*FString::Printf(TEXT("asset 'DA_Projectile_%s' ConfigRevision"), *Row.ProjectileId.ToString()),
					Asset->ConfigRevision, ExpectedRevision);
			}
		}

		TestEqual(TEXT("every reloaded asset was revision-checked"), CheckedAssets,
			Config.DamageProfiles.Num() + Config.AttackReactions.Num()
				+ Config.TargetReactions.Num() + Config.Weapons.Num() + Config.AmmoTypes.Num()
				+ Config.Projectiles.Num());
		TestEqual(TEXT("all reloaded assets share exactly one ConfigRevision"), DistinctRevisions.Num(), 1);
		AddInfo(FString::Printf(TEXT("reloaded assets carry ConfigRevision '%s' recomputed identically from the source tables in this process"),
			*ExpectedRevision));
		return true;
	}

	/**
	 * Reference closure of the persisted set: every cross-table id a
	 * reloaded asset carries resolves to a reloaded asset of the right class
	 * that itself declares that id - weapons into damage profiles / ammo /
	 * projectiles, projectiles into damage profiles / explosion profiles. No
	 * dangling id survives the save/reload round trip, so the asset set the
	 * cooker sees is closed under its references (a literal packaged-build
	 * cook evidence stays with the segment-end card M5-018). Every reference
	 * category must be exercised at least once so the closure cannot pass
	 * vacuously.
	 */
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_009ReloadedReferenceClosureResolves,
		"UEMMO.Tasks.M5_009.ReloadedReferenceClosureResolves",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_009ReloadedReferenceClosureResolves::RunTest(const FString& Parameters)
	{
		FParsedCombatConfig Config;
		if (!MakeAlignedShippedConfig(*this, Config))
		{
			return true;
		}

		int32 WeaponDamageRefs = 0;
		int32 WeaponAmmoRefs = 0;
		int32 WeaponProjectileRefs = 0;
		int32 ProjectileDamageRefs = 0;
		int32 ProjectileExplosionRefs = 0;

		for (const FWeaponDefinition& Row : Config.Weapons)
		{
			const UCombatWeaponAsset* Weapon = ReloadAsset<UCombatWeaponAsset>(
				ExpectedAssetNameFor(TEXT("DA_Weapon_"), Row.WeaponId));
			if (!Weapon)
			{
				continue;
			}
			if (!Weapon->DamageProfileId.IsNone())
			{
				const UCombatDamageProfileAsset* Target = ReloadAsset<UCombatDamageProfileAsset>(
					ExpectedAssetNameFor(TEXT("DA_Damage_"), Weapon->DamageProfileId));
				if (TestNotNull(*FString::Printf(TEXT("weapon '%s' damage profile '%s' resolves to a saved asset"),
					*Row.WeaponId.ToString(), *Weapon->DamageProfileId.ToString()), Target))
				{
					++WeaponDamageRefs;
					TestEqual(*FString::Printf(TEXT("weapon '%s' damage profile target declares the same id"),
						*Row.WeaponId.ToString()), Target->DamageProfileId, Weapon->DamageProfileId);
				}
			}
			if (!Weapon->AmmoId.IsNone())
			{
				const UCombatAmmoTypeAsset* Target = ReloadAsset<UCombatAmmoTypeAsset>(
					ExpectedAssetNameFor(TEXT("DA_Ammo_"), Weapon->AmmoId));
				if (TestNotNull(*FString::Printf(TEXT("weapon '%s' ammo '%s' resolves to a saved asset"),
					*Row.WeaponId.ToString(), *Weapon->AmmoId.ToString()), Target))
				{
					++WeaponAmmoRefs;
					TestEqual(*FString::Printf(TEXT("weapon '%s' ammo target declares the same id"),
						*Row.WeaponId.ToString()), Target->AmmoId, Weapon->AmmoId);
				}
			}
			if (!Weapon->ProjectileId.IsNone())
			{
				const UCombatProjectileAsset* Target = ReloadAsset<UCombatProjectileAsset>(
					ExpectedAssetNameFor(TEXT("DA_Projectile_"), Weapon->ProjectileId));
				if (TestNotNull(*FString::Printf(TEXT("weapon '%s' projectile '%s' resolves to a saved asset"),
					*Row.WeaponId.ToString(), *Weapon->ProjectileId.ToString()), Target))
				{
					++WeaponProjectileRefs;
					TestEqual(*FString::Printf(TEXT("weapon '%s' projectile target declares the same id"),
						*Row.WeaponId.ToString()), Target->ProjectileId, Weapon->ProjectileId);
				}
			}
		}

		for (const FProjectileDefinition& Row : Config.Projectiles)
		{
			const UCombatProjectileAsset* Projectile = ReloadAsset<UCombatProjectileAsset>(
				ExpectedAssetNameFor(TEXT("DA_Projectile_"), Row.ProjectileId));
			if (!Projectile)
			{
				continue;
			}
			if (!Projectile->DamageProfileId.IsNone())
			{
				const UCombatDamageProfileAsset* Target = ReloadAsset<UCombatDamageProfileAsset>(
					ExpectedAssetNameFor(TEXT("DA_Damage_"), Projectile->DamageProfileId));
				if (TestNotNull(*FString::Printf(TEXT("projectile '%s' damage profile '%s' resolves to a saved asset"),
					*Row.ProjectileId.ToString(), *Projectile->DamageProfileId.ToString()), Target))
				{
					++ProjectileDamageRefs;
					TestEqual(*FString::Printf(TEXT("projectile '%s' damage profile target declares the same id"),
						*Row.ProjectileId.ToString()), Target->DamageProfileId, Projectile->DamageProfileId);
				}
			}
			if (!Projectile->ExplosionDamageProfileId.IsNone())
			{
				const UCombatDamageProfileAsset* Target = ReloadAsset<UCombatDamageProfileAsset>(
					ExpectedAssetNameFor(TEXT("DA_Damage_"), Projectile->ExplosionDamageProfileId));
				if (TestNotNull(*FString::Printf(TEXT("projectile '%s' explosion profile '%s' resolves to a saved asset"),
					*Row.ProjectileId.ToString(), *Projectile->ExplosionDamageProfileId.ToString()), Target))
				{
					++ProjectileExplosionRefs;
					TestEqual(*FString::Printf(TEXT("projectile '%s' explosion profile target declares the same id"),
						*Row.ProjectileId.ToString()), Target->DamageProfileId, Projectile->ExplosionDamageProfileId);
				}
			}
		}

		TestTrue(TEXT("at least one weapon->damage profile reference was closure-checked"), WeaponDamageRefs > 0);
		TestTrue(TEXT("at least one weapon->ammo reference was closure-checked"), WeaponAmmoRefs > 0);
		TestTrue(TEXT("at least one weapon->projectile reference was closure-checked"), WeaponProjectileRefs > 0);
		TestTrue(TEXT("at least one projectile->damage profile reference was closure-checked"), ProjectileDamageRefs > 0);
		TestTrue(TEXT("at least one projectile->explosion profile reference was closure-checked"), ProjectileExplosionRefs > 0);
		AddInfo(FString::Printf(TEXT("closure-checked %d cross-table references over the reloaded asset set (weapons: %d damage, %d ammo, %d projectile; projectiles: %d damage, %d explosion)"),
			WeaponDamageRefs + WeaponAmmoRefs + WeaponProjectileRefs + ProjectileDamageRefs + ProjectileExplosionRefs,
			WeaponDamageRefs, WeaponAmmoRefs, WeaponProjectileRefs, ProjectileDamageRefs, ProjectileExplosionRefs));
		return true;
	}
}

#endif
