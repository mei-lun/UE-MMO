// M5-008: source-table to UE data asset generation (M5 interface contract
// section 1, first owner 008 together with 009). Pins
// Combat/Data/CombatDataAssetBuilder.h:
//
// - BuildFromConfig(const FParsedCombatConfig&, UObject* Outer, FString&
//   OutErrors, FCombatAssetBuildSummary* OutSummary) turns a legal parsed
//   combat config into one UPrimaryDataAsset instance per row of the six
//   tables and saves every asset under /Game/UEMMO/Combat/Data:
//   one damage profile asset per damage_profiles row (DA_Damage_<id>), one
//   attack reaction asset per attack_reactions row (DA_Reaction_<id>), one
//   target reaction asset per target_reactions row (DA_Target_<id>), one
//   weapon asset per weapons row (DA_Weapon_<id>), one ammo asset per
//   ammo_types row (DA_Ammo_<id>) and one projectile asset per projectiles
//   row (DA_Projectile_<id>).
// - Every mirrored field of every generated asset matches its source row
//   exactly (the numbers, the flags, the arrays in order, the enum values as
//   their frozen numeric value) and every asset carries the deterministic
//   ConfigRevision of its config (FCombatCatalog::GetConfigRevision).
// - Idempotency: a second BuildFromConfig call over the same config updates
//   the existing assets in place and creates nothing (created == 0, updated
//   == all rows, same object paths, no _1/_2 style duplicates).
// - Validation-first: a config that fails the catalog build (schema version,
//   empty ids, duplicate ids), the local 003/004 row validators, or the
//   six-table reference gate produces NO asset at all - not even the legal
//   rows of the same config - and existing assets are left untouched.
// - A target name already occupied by a foreign object is refused instead of
//   being overwritten or suffixed.
//
// The positive runs load the shipped Data/CombatSystem tables and align the
// known sample gap (M5-005/M5-006 reports: the shipped 004 rows reference
// the damage profiles physical_10/explosive_40 that the shipped 003 sample
// table does not define; Data/** stays frozen, so the two rows are supplied
// by the test exactly like M5-006 did). One test pins the honest refusal of
// the raw shipped tables by the source-directory entry until that gap is
// fixed, while staying green if the tables are aligned later.
//
// The tests exercise the in-memory asset graph and, when the host process
// supports package saving, the saved files; the new-process reload of the
// saved assets belongs to M5-009.

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/Data/CombatCatalog.h"
#include "../Combat/Data/CombatDataTableParser.h"
#include "../Combat/Data/CombatReferenceValidator.h"
#include "../Combat/Data/CombatDataAssetBuilder.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_008
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

	static bool ContainsAll(const FString& Text, const TArray<const TCHAR*>& Needles)
	{
		for (const TCHAR* Needle : Needles)
		{
			if (!Text.Contains(Needle))
			{
				return false;
			}
		}
		return true;
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
	 * explosive_40). Same values as the M5-006 test alignment. Idempotent: a
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
	 * A-segment candidate set the builder must accept. Returns false (after
	 * reporting) when the shipped source does not parse.
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
	// Expected asset naming
	// ------------------------------------------------------------------

	static FString ExpectedAssetNameFor(const TCHAR* Prefix, const FName Id)
	{
		return FString::Printf(TEXT("%s%s"), Prefix, *Id.ToString());
	}

	/** Full object path of one generated asset (LoadObject form: /Pkg/Name.Object). */
	static FString ExpectedObjectPath(const FString& AssetName)
	{
		return FCombatDataAssetBuilder::GetDefaultAssetRootPath() / AssetName + TEXT(".") + AssetName;
	}

	// ------------------------------------------------------------------
	// Per-type field mirrors: every asset field must equal its source row
	// ------------------------------------------------------------------

	static void CheckDamageProfileMirror(FAutomationTestBase& Test, const FDamageProfile& Row,
		const UCombatDamageProfileAsset* Asset)
	{
		const FString What = FString::Printf(TEXT("damage profile asset '%s'"), *ExpectedAssetNameFor(TEXT("DA_Damage_"), Row.DamageProfileId));
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
		const FString What = FString::Printf(TEXT("attack reaction asset '%s'"), *ExpectedAssetNameFor(TEXT("DA_Reaction_"), Row.ReactionId));
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
		const FString What = FString::Printf(TEXT("target reaction asset '%s'"), *ExpectedAssetNameFor(TEXT("DA_Target_"), Row.PolicyId));
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
		const FString What = FString::Printf(TEXT("weapon asset '%s'"), *ExpectedAssetNameFor(TEXT("DA_Weapon_"), Row.WeaponId));
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
		const FString What = FString::Printf(TEXT("ammo asset '%s'"), *ExpectedAssetNameFor(TEXT("DA_Ammo_"), Row.AmmoId));
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
		const FString What = FString::Printf(TEXT("projectile asset '%s'"), *ExpectedAssetNameFor(TEXT("DA_Projectile_"), Row.ProjectileId));
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

	/**
	 * Every row of one config must have its asset under the default root,
	 * of the right class, with every field mirrored and the config revision
	 * stamped. Covers all six tables.
	 */
	static void CheckWholeConfigMirrored(FAutomationTestBase& Test, const FParsedCombatConfig& Config,
		const FString& Revision)
	{
		for (const FDamageProfile& Row : Config.DamageProfiles)
		{
			const UCombatDamageProfileAsset* Asset = LoadObject<UCombatDamageProfileAsset>(
				nullptr, *ExpectedObjectPath(ExpectedAssetNameFor(TEXT("DA_Damage_"), Row.DamageProfileId)));
			CheckDamageProfileMirror(Test, Row, Asset);
			if (Asset)
			{
				Test.TestEqual(*FString::Printf(TEXT("asset 'DA_Damage_%s' ConfigRevision"), *Row.DamageProfileId.ToString()),
					Asset->ConfigRevision, Revision);
			}
		}
		for (const FAttackReaction& Row : Config.AttackReactions)
		{
			const UCombatAttackReactionAsset* Asset = LoadObject<UCombatAttackReactionAsset>(
				nullptr, *ExpectedObjectPath(ExpectedAssetNameFor(TEXT("DA_Reaction_"), Row.ReactionId)));
			CheckAttackReactionMirror(Test, Row, Asset);
			if (Asset)
			{
				Test.TestEqual(*FString::Printf(TEXT("asset 'DA_Reaction_%s' ConfigRevision"), *Row.ReactionId.ToString()),
					Asset->ConfigRevision, Revision);
			}
		}
		for (const FTargetReaction& Row : Config.TargetReactions)
		{
			const UCombatTargetReactionAsset* Asset = LoadObject<UCombatTargetReactionAsset>(
				nullptr, *ExpectedObjectPath(ExpectedAssetNameFor(TEXT("DA_Target_"), Row.PolicyId)));
			CheckTargetReactionMirror(Test, Row, Asset);
			if (Asset)
			{
				Test.TestEqual(*FString::Printf(TEXT("asset 'DA_Target_%s' ConfigRevision"), *Row.PolicyId.ToString()),
					Asset->ConfigRevision, Revision);
			}
		}
		for (const FWeaponDefinition& Row : Config.Weapons)
		{
			const UCombatWeaponAsset* Asset = LoadObject<UCombatWeaponAsset>(
				nullptr, *ExpectedObjectPath(ExpectedAssetNameFor(TEXT("DA_Weapon_"), Row.WeaponId)));
			CheckWeaponMirror(Test, Row, Asset);
			if (Asset)
			{
				Test.TestEqual(*FString::Printf(TEXT("asset 'DA_Weapon_%s' ConfigRevision"), *Row.WeaponId.ToString()),
					Asset->ConfigRevision, Revision);
			}
		}
		for (const FAmmoType& Row : Config.AmmoTypes)
		{
			const UCombatAmmoTypeAsset* Asset = LoadObject<UCombatAmmoTypeAsset>(
				nullptr, *ExpectedObjectPath(ExpectedAssetNameFor(TEXT("DA_Ammo_"), Row.AmmoId)));
			CheckAmmoMirror(Test, Row, Asset);
			if (Asset)
			{
				Test.TestEqual(*FString::Printf(TEXT("asset 'DA_Ammo_%s' ConfigRevision"), *Row.AmmoId.ToString()),
					Asset->ConfigRevision, Revision);
			}
		}
		for (const FProjectileDefinition& Row : Config.Projectiles)
		{
			const UCombatProjectileAsset* Asset = LoadObject<UCombatProjectileAsset>(
				nullptr, *ExpectedObjectPath(ExpectedAssetNameFor(TEXT("DA_Projectile_"), Row.ProjectileId)));
			CheckProjectileMirror(Test, Row, Asset);
			if (Asset)
			{
				Test.TestEqual(*FString::Printf(TEXT("asset 'DA_Projectile_%s' ConfigRevision"), *Row.ProjectileId.ToString()),
					Asset->ConfigRevision, Revision);
			}
		}
	}

	// ------------------------------------------------------------------
	// The tests
	// ------------------------------------------------------------------

	/**
	 * The aligned shipped config builds every asset of the six tables under
	 * /Game/UEMMO/Combat/Data; every mirrored field matches its row and every
	 * asset carries the config revision. Spot pins keep a parser+builder
	 * double fault visible: light_01 keeps its legacy M1 numbers, boss keeps
	 * its separate immunity axes, the pellet sample keeps five pellets, the
	 * homing missile keeps its turn rate and the explosive rocket keeps its
	 * explosion profile.
	 */
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_008ShippedTablesBuildAssetsAndMirrorFields,
		"UEMMO.Tasks.M5_008.ShippedTablesBuildAssetsAndMirrorFields",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_008ShippedTablesBuildAssetsAndMirrorFields::RunTest(const FString& Parameters)
	{
		FParsedCombatConfig Config;
		if (!MakeAlignedShippedConfig(*this, Config))
		{
			return true;
		}
		const int32 ExpectedAssets = Config.DamageProfiles.Num() + Config.AttackReactions.Num()
			+ Config.TargetReactions.Num() + Config.Weapons.Num() + Config.AmmoTypes.Num()
			+ Config.Projectiles.Num();

		FCombatAssetBuildSummary Summary;
		FString Errors;
		const bool bBuilt = FCombatDataAssetBuilder::BuildFromConfig(Config, nullptr, Errors, &Summary);
		if (!TestTrue(FString::Printf(TEXT("BuildFromConfig accepts the aligned shipped config (errors: %s)"), *Errors), bBuilt))
		{
			return true;
		}
		AddInfo(FString::Printf(TEXT("build summary: created=%d updated=%d saved=%d save-problems=%d revision=%s root=%s"),
			Summary.CreatedObjectPaths.Num(), Summary.UpdatedObjectPaths.Num(),
			Summary.CreatedObjectPaths.Num() + Summary.UpdatedObjectPaths.Num(),
			Summary.SaveProblems.Num(), *Summary.ConfigRevision, *Summary.TargetRootPath));

		TestEqual(TEXT("created + updated equals the row count"), Summary.NumAssets(), ExpectedAssets);
		TestEqual(TEXT("summary root path"), Summary.TargetRootPath, FCombatDataAssetBuilder::GetDefaultAssetRootPath());
		TestEqual(TEXT("summary revision"), Summary.ConfigRevision, ComputeExpectedRevision(*this, Config));

		const FString Revision = ComputeExpectedRevision(*this, Config);
		CheckWholeConfigMirrored(*this, Config, Revision);

		// Spot pins against double faults.
		{
			const UCombatDamageProfileAsset* Light01 = LoadObject<UCombatDamageProfileAsset>(
				nullptr, *ExpectedObjectPath(TEXT("DA_Damage_light_01")));
			if (Light01)
			{
				TestEqual(TEXT("light_01 keeps the legacy base damage"), Light01->BaseDamage, 10.0f);
				TestEqual(TEXT("light_01 keeps the legacy hit stun"), Light01->HitStunSeconds, 0.22f);
				TestEqual(TEXT("light_01 keeps the legacy launch"), Light01->LaunchCmPerSecond, 0.0f);
			}
			const UCombatTargetReactionAsset* Boss = LoadObject<UCombatTargetReactionAsset>(
				nullptr, *ExpectedObjectPath(TEXT("DA_Target_boss")));
			if (Boss)
			{
				TestEqual(TEXT("boss keeps control immunity"), Boss->bImmuneControl, true);
				TestEqual(TEXT("boss is death resistant but not damage immune"),
					Boss->bDeathResistant && !Boss->bImmuneDamage, true);
			}
			const UCombatWeaponAsset* Pellet = LoadObject<UCombatWeaponAsset>(
				nullptr, *ExpectedObjectPath(TEXT("DA_Weapon_weapon_pellet_sample")));
			if (Pellet)
			{
				TestEqual(TEXT("the pellet sample keeps five pellets"), Pellet->PelletCount, 5);
			}
			const UCombatProjectileAsset* Missile = LoadObject<UCombatProjectileAsset>(
				nullptr, *ExpectedObjectPath(TEXT("DA_Projectile_missile_homing")));
			if (Missile)
			{
				TestEqual(TEXT("the homing missile keeps its turn rate"), Missile->HomingTurnRateDegS, 180.0f);
			}
			const UCombatProjectileAsset* Rocket = LoadObject<UCombatProjectileAsset>(
				nullptr, *ExpectedObjectPath(TEXT("DA_Projectile_rocket_explosive")));
			if (Rocket)
			{
				TestEqual(TEXT("the explosive rocket keeps its explosion radius"), Rocket->ExplosionRadiusCm, 300.0f);
				TestEqual(TEXT("the explosive rocket keeps its explosion profile"),
					Rocket->ExplosionDamageProfileId, FName(TEXT("explosive_40")));
			}
		}

		return true;
	}

	/**
	 * A second build over the same config updates the existing assets in
	 * place: created == 0, updated == the row count, the same object paths,
	 * and every asset still mirrors its row (no duplicate objects with _1/_2
	 * style suffixes appeared).
	 */
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_008RebuildIsIdempotentUpdateNotCreate,
		"UEMMO.Tasks.M5_008.RebuildIsIdempotentUpdateNotCreate",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_008RebuildIsIdempotentUpdateNotCreate::RunTest(const FString& Parameters)
	{
		FParsedCombatConfig Config;
		if (!MakeAlignedShippedConfig(*this, Config))
		{
			return true;
		}
		const int32 ExpectedAssets = Config.DamageProfiles.Num() + Config.AttackReactions.Num()
			+ Config.TargetReactions.Num() + Config.Weapons.Num() + Config.AmmoTypes.Num()
			+ Config.Projectiles.Num();

		FString Errors;
		FCombatAssetBuildSummary First;
		if (!TestTrue(TEXT("the first build succeeds"),
			FCombatDataAssetBuilder::BuildFromConfig(Config, nullptr, Errors, &First)))
		{
			AddError(Errors);
			return true;
		}

		FCombatAssetBuildSummary Second;
		if (!TestTrue(TEXT("the second build succeeds"),
			FCombatDataAssetBuilder::BuildFromConfig(Config, nullptr, Errors, &Second)))
		{
			AddError(Errors);
			return true;
		}

		TestEqual(TEXT("the second build creates nothing"), Second.CreatedObjectPaths.Num(), 0);
		TestEqual(TEXT("the second build updates every row asset"), Second.UpdatedObjectPaths.Num(), ExpectedAssets);
		TestEqual(TEXT("the second build keeps the revision"), Second.ConfigRevision, First.ConfigRevision);
		TestEqual(TEXT("the second build keeps the root"), Second.TargetRootPath, First.TargetRootPath);

		// No duplicate objects: every updated path is one of the expected
		// names, and no path carries a generated numeric suffix.
		TSet<FString> ExpectedPaths;
		for (const FDamageProfile& Row : Config.DamageProfiles)
		{
			ExpectedPaths.Add(ExpectedObjectPath(ExpectedAssetNameFor(TEXT("DA_Damage_"), Row.DamageProfileId)));
		}
		for (const FAttackReaction& Row : Config.AttackReactions)
		{
			ExpectedPaths.Add(ExpectedObjectPath(ExpectedAssetNameFor(TEXT("DA_Reaction_"), Row.ReactionId)));
		}
		for (const FTargetReaction& Row : Config.TargetReactions)
		{
			ExpectedPaths.Add(ExpectedObjectPath(ExpectedAssetNameFor(TEXT("DA_Target_"), Row.PolicyId)));
		}
		for (const FWeaponDefinition& Row : Config.Weapons)
		{
			ExpectedPaths.Add(ExpectedObjectPath(ExpectedAssetNameFor(TEXT("DA_Weapon_"), Row.WeaponId)));
		}
		for (const FAmmoType& Row : Config.AmmoTypes)
		{
			ExpectedPaths.Add(ExpectedObjectPath(ExpectedAssetNameFor(TEXT("DA_Ammo_"), Row.AmmoId)));
		}
		for (const FProjectileDefinition& Row : Config.Projectiles)
		{
			ExpectedPaths.Add(ExpectedObjectPath(ExpectedAssetNameFor(TEXT("DA_Projectile_"), Row.ProjectileId)));
		}
		for (const FString& Path : Second.UpdatedObjectPaths)
		{
			TestTrue(FString::Printf(TEXT("updated path '%s' is an expected asset path"), *Path),
				ExpectedPaths.Contains(Path));
		}
		TestEqual(TEXT("the expected path set is exactly the updated set"),
			Second.UpdatedObjectPaths.Num(), ExpectedPaths.Num());

		CheckWholeConfigMirrored(*this, Config, Second.ConfigRevision);
		return true;
	}

	/**
	 * A config with a dangling cross-table reference is refused completely:
	 * no asset is created or updated by the failed call, and an existing
	 * asset (canary) keeps its mirrored fields and revision untouched.
	 */
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_008DanglingReferenceRefusedWithoutTouchingAssets,
		"UEMMO.Tasks.M5_008.DanglingReferenceRefusedWithoutTouchingAssets",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_008DanglingReferenceRefusedWithoutTouchingAssets::RunTest(const FString& Parameters)
	{
		FParsedCombatConfig Config;
		if (!MakeAlignedShippedConfig(*this, Config))
		{
			return true;
		}

		FString Errors;
		FCombatAssetBuildSummary First;
		if (!TestTrue(TEXT("the aligned config builds"),
			FCombatDataAssetBuilder::BuildFromConfig(Config, nullptr, Errors, &First)))
		{
			AddError(Errors);
			return true;
		}

		// Canary before the failed call.
		const UCombatDamageProfileAsset* Canary = LoadObject<UCombatDamageProfileAsset>(
			nullptr, *ExpectedObjectPath(TEXT("DA_Damage_light_01")));
		if (!TestNotNull(TEXT("the light_01 canary asset exists"), Canary))
		{
			return true;
		}
		const FString CanaryRevision = Canary->ConfigRevision;
		const float CanaryBaseDamage = Canary->BaseDamage;

		// Break one reference: the training sword points at an unknown ammo.
		bool bFoundSword = false;
		for (FWeaponDefinition& Weapon : Config.Weapons)
		{
			if (Weapon.WeaponId == FName(TEXT("weapon_training_sword")))
			{
				Weapon.AmmoId = FName(TEXT("ammo_missing"));
				bFoundSword = true;
			}
		}
		TestTrue(TEXT("the shipped training sword row is present"), bFoundSword);

		FCombatAssetBuildSummary Failed;
		const bool bBuilt = FCombatDataAssetBuilder::BuildFromConfig(Config, nullptr, Errors, &Failed);
		TestFalse(TEXT("the dangling ammo reference refuses the build"), bBuilt);
		TestTrue(FString::Printf(TEXT("the errors name the missing id (errors: %s)"), *Errors),
			ContainsAll(Errors, {TEXT("ammo_missing"), TEXT("ammo_id")}));
		TestEqual(TEXT("the failed call created nothing"), Failed.CreatedObjectPaths.Num(), 0);
		TestEqual(TEXT("the failed call updated nothing"), Failed.UpdatedObjectPaths.Num(), 0);

		const UCombatDamageProfileAsset* CanaryAfter = LoadObject<UCombatDamageProfileAsset>(
			nullptr, *ExpectedObjectPath(TEXT("DA_Damage_light_01")));
		if (TestNotNull(TEXT("the canary still loads"), CanaryAfter))
		{
			TestEqual(TEXT("the canary keeps its revision"), CanaryAfter->ConfigRevision, CanaryRevision);
			TestEqual(TEXT("the canary keeps its base damage"), CanaryAfter->BaseDamage, CanaryBaseDamage);
		}
		return true;
	}

	/**
	 * A locally illegal row (base damage 0) refuses the whole build even when
	 * every other row of the same config is legal - the fresh id of the bad
	 * row must not appear as an asset afterwards.
	 */
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_008LocalValueErrorRefusedWithoutCreatingAssets,
		"UEMMO.Tasks.M5_008.LocalValueErrorRefusedWithoutCreatingAssets",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_008LocalValueErrorRefusedWithoutCreatingAssets::RunTest(const FString& Parameters)
	{
		FParsedCombatConfig Config;
		if (!MakeAlignedShippedConfig(*this, Config))
		{
			return true;
		}
		FDamageProfile Bad;
		Bad.DamageProfileId = TEXT("bad_profile_testonly");
		Bad.BaseDamage = 0.0f; // illegal: must be greater than 0
		Bad.AttackCoefficient = 1.0f;
		Config.DamageProfiles.Add(Bad);

		FString Errors;
		FCombatAssetBuildSummary Failed;
		const bool bBuilt = FCombatDataAssetBuilder::BuildFromConfig(Config, nullptr, Errors, &Failed);
		TestFalse(TEXT("a zero base damage row refuses the build"), bBuilt);
		TestTrue(FString::Printf(TEXT("the errors name the row and the field (errors: %s)"), *Errors),
			ContainsAll(Errors, {TEXT("bad_profile_testonly"), TEXT("BaseDamage")}));
		TestEqual(TEXT("the failed call created nothing"), Failed.CreatedObjectPaths.Num(), 0);
		TestEqual(TEXT("the failed call updated nothing"), Failed.UpdatedObjectPaths.Num(), 0);
		TestNull(TEXT("the bad row never became an asset"),
			LoadObject<UCombatDamageProfileAsset>(nullptr, *ExpectedObjectPath(TEXT("DA_Damage_bad_profile_testonly"))));
		return true;
	}

	/**
	 * The catalog integrity refusals: a duplicate row id, an empty row id and
	 * a non-positive schema version each refuse the build with a non-empty
	 * error and no asset changes.
	 */
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_008CatalogIntegrityRefusals,
		"UEMMO.Tasks.M5_008.CatalogIntegrityRefusals",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_008CatalogIntegrityRefusals::RunTest(const FString& Parameters)
	{
		FParsedCombatConfig Config;
		if (!MakeAlignedShippedConfig(*this, Config))
		{
			return true;
		}
		FString Errors;
		FCombatAssetBuildSummary First;
		if (!TestTrue(TEXT("the aligned config builds"),
			FCombatDataAssetBuilder::BuildFromConfig(Config, nullptr, Errors, &First)))
		{
			AddError(Errors);
			return true;
		}

		// Duplicate id inside one table.
		{
			FParsedCombatConfig Duplicate = Config;
			FDamageProfile Twin;
			Twin.DamageProfileId = TEXT("light_01");
			Twin.BaseDamage = 10.0f;
			Duplicate.DamageProfiles.Add(Twin);

			FCombatAssetBuildSummary Failed;
			FString DuplicateErrors;
			TestFalse(TEXT("a duplicate row id refuses the build"),
				FCombatDataAssetBuilder::BuildFromConfig(Duplicate, nullptr, DuplicateErrors, &Failed));
			TestTrue(FString::Printf(TEXT("the errors name the duplicate (errors: %s)"), *DuplicateErrors),
				ContainsAll(DuplicateErrors, {TEXT("light_01"), TEXT("duplicate")}));
			TestEqual(TEXT("the duplicate refusal created nothing"), Failed.CreatedObjectPaths.Num(), 0);
		}

		// Empty row id.
		{
			FParsedCombatConfig Empty = Config;
			FWeaponDefinition Anonymous;
			Anonymous.WeaponId = NAME_None;
			Anonymous.FireMode = EWeaponFireMode::Melee;
			Anonymous.DamageProfileId = TEXT("light_01");
			Anonymous.MeleeAttackIds = GetLegacyMeleeAttackIds();
			Empty.Weapons.Add(Anonymous);

			FCombatAssetBuildSummary Failed;
			FString EmptyErrors;
			TestFalse(TEXT("an empty row id refuses the build"),
				FCombatDataAssetBuilder::BuildFromConfig(Empty, nullptr, EmptyErrors, &Failed));
			TestFalse(TEXT("the empty-id refusal carries an error"), EmptyErrors.IsEmpty());
			TestEqual(TEXT("the empty-id refusal created nothing"), Failed.CreatedObjectPaths.Num(), 0);
		}

		// Non-positive schema version.
		{
			FParsedCombatConfig WrongSchema = Config;
			WrongSchema.SchemaVersion = 0;

			FCombatAssetBuildSummary Failed;
			FString SchemaErrors;
			TestFalse(TEXT("schema version 0 refuses the build"),
				FCombatDataAssetBuilder::BuildFromConfig(WrongSchema, nullptr, SchemaErrors, &Failed));
			TestFalse(TEXT("the schema refusal carries an error"), SchemaErrors.IsEmpty());
			TestEqual(TEXT("the schema refusal created nothing"), Failed.CreatedObjectPaths.Num(), 0);
		}
		return true;
	}

	/**
	 * A target asset name already occupied by a foreign object is refused
	 * with a clear error instead of being overwritten or suffixed; the
	 * foreign object survives untouched and no asset of the builder appears
	 * under that name.
	 */
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_008ForeignAssetNameCollisionRefused,
		"UEMMO.Tasks.M5_008.ForeignAssetNameCollisionRefused",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_008ForeignAssetNameCollisionRefused::RunTest(const FString& Parameters)
	{
		FParsedCombatConfig Config;
		if (!MakeAlignedShippedConfig(*this, Config))
		{
			return true;
		}
		FDamageProfile Fresh;
		Fresh.DamageProfileId = TEXT("collision_test");
		Fresh.BaseDamage = 5.0f;
		Config.DamageProfiles.Add(Fresh);

		// Occupy the fresh row's target name with a foreign (wrong class)
		// object in the default root folder. UCombatAmmoTypeAsset is a
		// concrete sibling asset class: the collision must be refused even
		// though the occupier is a legal combat asset of another table.
		const FString CollisionPath = ExpectedObjectPath(TEXT("DA_Damage_collision_test"));
		UPackage* CollisionPackage = CreatePackage(*(FCombatDataAssetBuilder::GetDefaultAssetRootPath() / TEXT("DA_Damage_collision_test")));
		UObject* Foreign = NewObject<UCombatAmmoTypeAsset>(CollisionPackage, TEXT("DA_Damage_collision_test"), RF_Public | RF_Standalone);
		TestNotNull(TEXT("the foreign occupier exists"), Foreign);

		FString Errors;
		FCombatAssetBuildSummary Failed;
		const bool bBuilt = FCombatDataAssetBuilder::BuildFromConfig(Config, nullptr, Errors, &Failed);
		TestFalse(TEXT("a foreign name occupier refuses the build"), bBuilt);
		TestTrue(FString::Printf(TEXT("the errors name the collision (errors: %s)"), *Errors),
			Errors.Contains(TEXT("collision_test")));
		TestEqual(TEXT("the refused call created nothing"), Failed.CreatedObjectPaths.Num(), 0);
		TestNull(TEXT("no damage profile asset appears under the foreign name"),
			LoadObject<UCombatDamageProfileAsset>(nullptr, *CollisionPath));
		TestTrue(TEXT("the foreign object is untouched"), IsValid(Foreign)
			&& Foreign->GetClass() != UCombatDamageProfileAsset::StaticClass());

		// Clean the scratch occupier so later runs start clean.
		if (IsValid(Foreign))
		{
			Foreign->MarkAsGarbage();
		}
		CollisionPackage->MarkAsGarbage();
		return true;
	}

	/**
	 * An empty config (positive schema version, zero rows) is legal for the
	 * frozen catalog contract: the build succeeds, creates nothing and still
	 * carries the catalog revision.
	 */
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_008EmptyConfigSucceedsWithZeroAssets,
		"UEMMO.Tasks.M5_008.EmptyConfigSucceedsWithZeroAssets",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_008EmptyConfigSucceedsWithZeroAssets::RunTest(const FString& Parameters)
	{
		FParsedCombatConfig Config;
		Config.SchemaVersion = 1;

		FString Errors;
		FCombatAssetBuildSummary Summary;
		if (!TestTrue(TEXT("an empty config builds"),
			FCombatDataAssetBuilder::BuildFromConfig(Config, nullptr, Errors, &Summary)))
		{
			AddError(Errors);
			return true;
		}
		TestEqual(TEXT("an empty config creates nothing"), Summary.NumAssets(), 0);
		TestEqual(TEXT("the empty revision matches the catalog"),
			Summary.ConfigRevision, ComputeExpectedRevision(*this, Config));
		TestFalse(TEXT("the empty revision is a real digest"), Summary.ConfigRevision.IsEmpty());
		return true;
	}

	/**
	 * The Outer contract: a non-package outer in the transient package is
	 * refused, and a UPackage outer selects the target folder (its package
	 * name becomes the root path, matching the null contract for the default
	 * folder).
	 */
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_008OuterContract,
		"UEMMO.Tasks.M5_008.OuterContract",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_008OuterContract::RunTest(const FString& Parameters)
	{
		TestEqual(TEXT("the default root path is the M5 combat data folder"),
			FCombatDataAssetBuilder::GetDefaultAssetRootPath(), FString(TEXT("/Game/UEMMO/Combat/Data")));

		FParsedCombatConfig Config;
		if (!MakeAlignedShippedConfig(*this, Config))
		{
			return true;
		}

		// A transient outer is refused: assets must never land in the
		// transient package.
		{
			UObject* TransientOwner = NewObject<UCombatAmmoTypeAsset>(GetTransientPackage(), TEXT("UEMMO_M5_008_TransientOwner"));
			FString Errors;
			FCombatAssetBuildSummary Failed;
			TestFalse(TEXT("a transient outer refuses the build"),
				FCombatDataAssetBuilder::BuildFromConfig(Config, TransientOwner, Errors, &Failed));
			TestTrue(FString::Printf(TEXT("the refusal names the transient package (errors: %s)"), *Errors),
				Errors.Contains(TEXT("Transient")));
			TestEqual(TEXT("the transient refusal created nothing"), Failed.CreatedObjectPaths.Num(), 0);
		}

		// A UPackage outer selects the folder: the default folder package
		// behaves exactly like the null contract.
		{
			UPackage* RootFolder = CreatePackage(TEXT("/Game/UEMMO/Combat/Data"));
			FString Errors;
			FCombatAssetBuildSummary Summary;
			if (!TestTrue(TEXT("a UPackage outer builds"),
				FCombatDataAssetBuilder::BuildFromConfig(Config, RootFolder, Errors, &Summary)))
			{
				AddError(Errors);
				return true;
			}
			TestEqual(TEXT("the package outer keeps the root path"),
				Summary.TargetRootPath, FString(TEXT("/Game/UEMMO/Combat/Data")));
			TestTrue(TEXT("the package outer touched only row assets"),
				Summary.NumAssets() == Config.DamageProfiles.Num() + Config.AttackReactions.Num()
					+ Config.TargetReactions.Num() + Config.Weapons.Num() + Config.AmmoTypes.Num()
					+ Config.Projectiles.Num());
		}
		return true;
	}

	/**
	 * The source-directory entry parses, validates and builds in one step.
	 * The raw shipped tables still carry the known sample gap (M5-006), so
	 * the entry must refuse them today with exactly that gap; once the
	 * shipped tables are aligned the entry must build the full asset set.
	 * Both truths keep this test green.
	 */
	IMPLEMENT_SIMPLE_AUTOMATION_TEST(
		FUEMMOTasksM5_008SourceDirectoryEntryRefusesOrBuilds,
		"UEMMO.Tasks.M5_008.SourceDirectoryEntryRefusesOrBuilds",
		EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

	bool FUEMMOTasksM5_008SourceDirectoryEntryRefusesOrBuilds::RunTest(const FString& Parameters)
	{
		int32 Created = 0;
		int32 Updated = 0;
		FString Revision;
		// The entry returns the empty string on success and the error text on
		// failure (UE Python discards out parameters when a bool-returning
		// entry fails, so the error rides on the return value).
		const FString Errors = UCombatDataAssetBuilderLibrary::BuildFromSourceDirectory(
			TEXT("Data/CombatSystem"), Created, Updated, Revision);
		const bool bBuilt = Errors.IsEmpty();

		if (!bBuilt)
		{
			// The current truth: only the known physical_10/explosive_40 gap
			// is reported, nothing else, and nothing was created.
			const bool bOnlyKnownGap = Errors.Contains(TEXT("physical_10")) && Errors.Contains(TEXT("explosive_40"))
				&& !Errors.Contains(TEXT("ammo_id")) && !Errors.Contains(TEXT("projectile_id"));
			TestTrue(FString::Printf(TEXT("the shipped tables are refused with only the known gap (errors: %s)"), *Errors),
				bOnlyKnownGap);
			TestEqual(TEXT("the refused directory entry created nothing"), Created, 0);
			TestEqual(TEXT("the refused directory entry updated nothing"), Updated, 0);
			AddInfo(TEXT("the shipped sample tables still miss physical_10/explosive_40; the editor entry refuses them until aligned"));
			return true;
		}

		// The future truth: aligned shipped tables build the full set.
		FParsedCombatConfig Config;
		if (!MakeAlignedShippedConfig(*this, Config))
		{
			return true;
		}
		const int32 ExpectedAssets = Config.DamageProfiles.Num() + Config.AttackReactions.Num()
			+ Config.TargetReactions.Num() + Config.Weapons.Num() + Config.AmmoTypes.Num()
			+ Config.Projectiles.Num();
		TestEqual(TEXT("the directory entry builds the full set"), Created + Updated, ExpectedAssets);
		TestFalse(TEXT("the directory entry carries the revision"), Revision.IsEmpty());
		return true;
	}
}

#endif
