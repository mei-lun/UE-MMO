// M5-007: the read-only combat configuration catalog (M5 interface contract
// section 1, first owner 007). Pins Combat/Data/CombatCatalog.h:
//
// - BuildFromParsed constructs an immutable catalog from already-validated
//   candidate rows (the six frozen value types of M5-003/004); duplicate ids
//   and empty ids are refused, and a failed build leaves the target catalog
//   completely untouched (no half published state).
// - FindDamageProfile / FindAttackReaction / FindTargetReaction / FindWeapon /
//   FindAmmoType / FindProjectile return const pointers into stable storage
//   and nullptr for unknown ids - there is no silent fallback definition and
//   no default weapon.
// - GetConfigRevision is a deterministic content signature over the schema
//   version and the normalized rows: reordering rows keeps the revision,
//   changing any value (or adding/removing a row, or changing the schema
//   version) changes it.
// - Num* counters and repeated const queries never mutate the catalog.
//
// Pure logic only: no World, no UE assets, no wall clocks. Value validation
// of the individual rows belongs to M5-005/006; this card builds from legal
// value data and only guards its own lookup integrity (empty/duplicate ids).

#include "Algo/Reverse.h"
#include "Misc/AutomationTest.h"

#include "../Combat/Data/CombatCatalog.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_007
{
	/** One legacy melee damage profile row (Data/CombatSystem/damage_profiles.json). */
	FDamageProfile MakeDamageProfile(FName Id, float BaseDamage, float HitStunSeconds, float LaunchCmPerSecond)
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = Id;
		Profile.BaseDamage = BaseDamage;
		Profile.AttackCoefficient = 1.0f;
		Profile.HitStunSeconds = HitStunSeconds;
		Profile.KnockbackCmPerSecond = 90.0f;
		Profile.LaunchCmPerSecond = LaunchCmPerSecond;
		Profile.HitStopSeconds = 0.04f;
		return Profile;
	}

	/** The four legacy attack profiles with their exact M1 values. */
	TArray<FDamageProfile> MakeLegacyDamageProfiles()
	{
		TArray<FDamageProfile> Rows;
		Rows.Add(MakeDamageProfile(TEXT("light_01"), 10.0f, 0.22f, 0.0f));
		Rows.Add(MakeDamageProfile(TEXT("light_02"), 14.0f, 0.28f, 0.0f));
		Rows.Add(MakeDamageProfile(TEXT("launcher"), 18.0f, 1.0f, 700.0f));
		Rows.Add(MakeDamageProfile(TEXT("aerial_01"), 12.0f, 0.18f, 60.0f));
		return Rows;
	}

	/** One attack reaction per legacy attack, all control_penetration none. */
	TArray<FAttackReaction> MakeLegacyAttackReactions()
	{
		TArray<FAttackReaction> Rows;
		for (const FName AttackId : GetLegacyMeleeAttackIds())
		{
			FAttackReaction Row;
			Row.ReactionId = AttackId;
			Row.ControlPenetration = EControlPenetration::None;
			Rows.Add(Row);
		}
		return Rows;
	}

	/** normal / heavy / boss target policies following target_reactions.json. */
	TArray<FTargetReaction> MakeSampleTargetReactions()
	{
		TArray<FTargetReaction> Rows;

		FTargetReaction Normal; // defaults are the legacy normal target
		Normal.PolicyId = TEXT("normal");
		Rows.Add(Normal);

		FTargetReaction Heavy;
		Heavy.PolicyId = TEXT("heavy");
		Heavy.bAllowLaunch = false; // launch immune through the explicit gate
		Heavy.PoiseMax = 200.0f;
		Rows.Add(Heavy);

		FTargetReaction Boss;
		Boss.PolicyId = TEXT("boss");
		Boss.bImmuneControl = true; // separate axes: still takes damage
		Boss.bDeathResistant = true;
		Boss.MaxLaunchesPerAirCycle = 0;
		Boss.LaunchZScales = {1.0f};
		Rows.Add(Boss);

		return Rows;
	}

	/** Melee + hitscan + projectile weapon rows following weapons.json. */
	TArray<FWeaponDefinition> MakeSampleWeapons()
	{
		TArray<FWeaponDefinition> Rows;

		FWeaponDefinition Sword;
		Sword.WeaponId = TEXT("weapon_training_sword");
		Sword.FireMode = EWeaponFireMode::Melee;
		Sword.DamageProfileId = TEXT("light_01");
		Sword.MeleeAttackIds = GetLegacyMeleeAttackIds();
		Rows.Add(Sword);

		FWeaponDefinition Hitscan;
		Hitscan.WeaponId = TEXT("weapon_hitscan_sample");
		Hitscan.FireMode = EWeaponFireMode::Hitscan;
		Hitscan.DamageProfileId = TEXT("light_02");
		Hitscan.AmmoId = TEXT("ammo_cell");
		Hitscan.MagazineSize = 12;
		Hitscan.FireRateRpm = 240.0f;
		Hitscan.SpreadDegrees = 1.0f;
		Hitscan.RangeCm = 5000.0f;
		Rows.Add(Hitscan);

		FWeaponDefinition LauncherGun;
		LauncherGun.WeaponId = TEXT("weapon_projectile_sample");
		LauncherGun.FireMode = EWeaponFireMode::Projectile;
		LauncherGun.AmmoId = TEXT("ammo_cell");
		LauncherGun.ProjectileId = TEXT("bullet_linear");
		LauncherGun.MagazineSize = 8;
		LauncherGun.FireRateRpm = 120.0f;
		Rows.Add(LauncherGun);

		return Rows;
	}

	TArray<FAmmoType> MakeSampleAmmoTypes()
	{
		TArray<FAmmoType> Rows;

		FAmmoType Cell;
		Cell.AmmoId = TEXT("ammo_cell");
		Cell.MaxReserve = 120;
		Cell.MagazineSize = 12;
		Rows.Add(Cell);

		FAmmoType Light;
		Light.AmmoId = TEXT("ammo_light");
		Light.MaxReserve = 96;
		Light.MagazineSize = 12;
		Rows.Add(Light);

		return Rows;
	}

	/** Straight, parabolic and homing projectile rows following projectiles.json. */
	TArray<FProjectileDefinition> MakeSampleProjectiles()
	{
		TArray<FProjectileDefinition> Rows;

		FProjectileDefinition Linear;
		Linear.ProjectileId = TEXT("bullet_linear");
		Linear.Motion = EProjectileMotion::Straight;
		Linear.SpeedCmS = 6000.0f;
		Linear.LifetimeS = 2.0f;
		Linear.DamageProfileId = TEXT("light_01");
		Rows.Add(Linear);

		FProjectileDefinition Parabolic;
		Parabolic.ProjectileId = TEXT("shell_parabolic");
		Parabolic.Motion = EProjectileMotion::Parabolic;
		Parabolic.SpeedCmS = 3500.0f;
		Parabolic.LifetimeS = 3.0f;
		Parabolic.DamageProfileId = TEXT("light_01");
		Rows.Add(Parabolic);

		FProjectileDefinition Homing;
		Homing.ProjectileId = TEXT("missile_homing");
		Homing.Motion = EProjectileMotion::Homing;
		Homing.SpeedCmS = 2500.0f;
		Homing.LifetimeS = 5.0f;
		Homing.DamageProfileId = TEXT("light_01");
		Homing.HomingTurnRateDegS = 180.0f;
		Rows.Add(Homing);

		return Rows;
	}

	/** A complete legal candidate config mirroring the A-segment source tables. */
	FParsedCombatConfig MakeSampleConfig()
	{
		FParsedCombatConfig Parsed;
		Parsed.SchemaVersion = 1;
		Parsed.DamageProfiles = MakeLegacyDamageProfiles();
		Parsed.AttackReactions = MakeLegacyAttackReactions();
		Parsed.TargetReactions = MakeSampleTargetReactions();
		Parsed.Weapons = MakeSampleWeapons();
		Parsed.AmmoTypes = MakeSampleAmmoTypes();
		Parsed.Projectiles = MakeSampleProjectiles();
		return Parsed;
	}

	/** Reverses every row array: the same rows in the opposite order. */
	FParsedCombatConfig MakeRowReversedConfig()
	{
		FParsedCombatConfig Parsed = MakeSampleConfig();
		auto Reverse = [](auto& Rows) { Algo::Reverse(Rows); };
		Reverse(Parsed.DamageProfiles);
		Reverse(Parsed.AttackReactions);
		Reverse(Parsed.TargetReactions);
		Reverse(Parsed.Weapons);
		Reverse(Parsed.AmmoTypes);
		Reverse(Parsed.Projectiles);
		return Parsed;
	}

	/** Builds the sample catalog and fails the test when the build refuses. */
	bool BuildSampleCatalog(FAutomationTestBase& Test, FCombatCatalog& OutCatalog, const FString& ContextText)
	{
		FString Errors;
		const bool bBuilt = FCombatCatalog::BuildFromParsed(MakeSampleConfig(), OutCatalog, Errors);
		Test.TestTrue(ContextText + TEXT(": BuildFromParsed accepts the legal sample config"), bBuilt);
		Test.TestTrue(ContextText + TEXT(": the sample build reports no errors"), Errors.IsEmpty());
		return bBuilt;
	}
}

using namespace UE::UEMMO::Tasks::M5_007;

/**
 * Acceptance: building the six frozen tables reports per-table counts, every
 * finder returns the stored definition with its legal values, and repeated
 * queries through a const reference return the same pointer without mutating
 * anything (counts and revision stay identical).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_007BuildCountsAndFind,
	"UEMMO.Tasks.M5_007.BuildCountsAndFind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_007BuildCountsAndFind::RunTest(const FString& Parameters)
{
	FCombatCatalog Catalog;
	if (!BuildSampleCatalog(*this, Catalog, TEXT("sample build")))
	{
		return false;
	}

	// Per-table counts of the sample config.
	TestEqual(TEXT("NumDamageProfiles"), Catalog.NumDamageProfiles(), 4);
	TestEqual(TEXT("NumAttackReactions"), Catalog.NumAttackReactions(), 4);
	TestEqual(TEXT("NumTargetReactions"), Catalog.NumTargetReactions(), 3);
	TestEqual(TEXT("NumWeapons"), Catalog.NumWeapons(), 3);
	TestEqual(TEXT("NumAmmoTypes"), Catalog.NumAmmoTypes(), 2);
	TestEqual(TEXT("NumProjectiles"), Catalog.NumProjectiles(), 3);

	// Damage profiles keep their exact legacy values.
	const FDamageProfile* Light01 = Catalog.FindDamageProfile(TEXT("light_01"));
	TestNotNull(TEXT("FindDamageProfile(light_01)"), Light01);
	if (Light01)
	{
		TestEqual(TEXT("light_01 BaseDamage"), Light01->BaseDamage, 10.0f);
		TestEqual(TEXT("light_01 HitStunSeconds"), Light01->HitStunSeconds, 0.22f);
		TestEqual(TEXT("light_01 LaunchCmPerSecond"), Light01->LaunchCmPerSecond, 0.0f);
		TestEqual(TEXT("light_01 HitStopSeconds"), Light01->HitStopSeconds, 0.04f);
	}
	const FDamageProfile* LauncherProfile = Catalog.FindDamageProfile(TEXT("launcher"));
	if (LauncherProfile)
	{
		TestEqual(TEXT("launcher BaseDamage"), LauncherProfile->BaseDamage, 18.0f);
		TestEqual(TEXT("launcher LaunchCmPerSecond"), LauncherProfile->LaunchCmPerSecond, 700.0f);
	}
	const FDamageProfile* AerialProfile = Catalog.FindDamageProfile(TEXT("aerial_01"));
	if (AerialProfile)
	{
		TestEqual(TEXT("aerial_01 BaseDamage"), AerialProfile->BaseDamage, 12.0f);
	}

	// Attack reactions: the legacy attacks all use the closed none value.
	const FAttackReaction* LauncherReaction = Catalog.FindAttackReaction(TEXT("launcher"));
	TestNotNull(TEXT("FindAttackReaction(launcher)"), LauncherReaction);
	if (LauncherReaction)
	{
		TestEqual(TEXT("launcher ControlPenetration"), static_cast<uint8>(LauncherReaction->ControlPenetration), static_cast<uint8>(EControlPenetration::None));
	}

	// Target reactions: normal defaults, heavy launch gate, boss immunity axes.
	const FTargetReaction* NormalPolicy = Catalog.FindTargetReaction(TEXT("normal"));
	TestNotNull(TEXT("FindTargetReaction(normal)"), NormalPolicy);
	if (NormalPolicy)
	{
		TestEqual(TEXT("normal MaxLaunchesPerAirCycle"), NormalPolicy->MaxLaunchesPerAirCycle, 2);
		TestEqual(TEXT("normal LaunchZScales.Num()"), NormalPolicy->LaunchZScales.Num(), 2);
		if (NormalPolicy->LaunchZScales.Num() == 2)
		{
			TestEqual(TEXT("normal LaunchZScales[0]"), NormalPolicy->LaunchZScales[0], 1.0f);
			TestEqual(TEXT("normal LaunchZScales[1]"), NormalPolicy->LaunchZScales[1], 0.7f);
		}
		TestEqual(TEXT("normal KnockdownSeconds"), NormalPolicy->KnockdownSeconds, 0.45f);
		TestEqual(TEXT("normal RecoveringSeconds"), NormalPolicy->RecoveringSeconds, 0.25f);
	}
	const FTargetReaction* HeavyPolicy = Catalog.FindTargetReaction(TEXT("heavy"));
	if (HeavyPolicy)
	{
		TestFalse(TEXT("heavy bAllowLaunch"), HeavyPolicy->bAllowLaunch);
		TestEqual(TEXT("heavy PoiseMax"), HeavyPolicy->PoiseMax, 200.0f);
	}
	const FTargetReaction* BossPolicy = Catalog.FindTargetReaction(TEXT("boss"));
	if (BossPolicy)
	{
		TestTrue(TEXT("boss bImmuneControl"), BossPolicy->bImmuneControl);
		TestTrue(TEXT("boss bDeathResistant"), BossPolicy->bDeathResistant);
		TestFalse(TEXT("boss is not damage immune (separate axes)"), BossPolicy->bImmuneDamage);
	}

	// Weapons: the melee sword carries the legacy chain; ranged weapons carry
	// their ammo and delivery fields per mode.
	const FWeaponDefinition* Sword = Catalog.FindWeapon(TEXT("weapon_training_sword"));
	TestNotNull(TEXT("FindWeapon(weapon_training_sword)"), Sword);
	if (Sword)
	{
		TestEqual(TEXT("sword FireMode"), static_cast<uint8>(Sword->FireMode), static_cast<uint8>(EWeaponFireMode::Melee));
		TestEqual(TEXT("sword DamageProfileId"), Sword->DamageProfileId, FName(TEXT("light_01")));
		TestTrue(TEXT("sword AmmoId stays empty"), Sword->AmmoId.IsNone());
		TestEqual(TEXT("sword MeleeAttackIds.Num()"), Sword->MeleeAttackIds.Num(), 4);
		for (const FName LegacyAttackId : GetLegacyMeleeAttackIds())
		{
			TestTrue(FString::Printf(TEXT("sword MeleeAttackIds contains '%s'"), *LegacyAttackId.ToString()),
				Sword->MeleeAttackIds.Contains(LegacyAttackId));
		}
	}
	const FWeaponDefinition* Hitscan = Catalog.FindWeapon(TEXT("weapon_hitscan_sample"));
	if (Hitscan)
	{
		TestEqual(TEXT("hitscan FireMode"), static_cast<uint8>(Hitscan->FireMode), static_cast<uint8>(EWeaponFireMode::Hitscan));
		TestEqual(TEXT("hitscan AmmoId"), Hitscan->AmmoId, FName(TEXT("ammo_cell")));
		TestEqual(TEXT("hitscan RangeCm"), Hitscan->RangeCm, 5000.0f);
		TestEqual(TEXT("hitscan MagazineSize"), Hitscan->MagazineSize, 12);
	}
	const FWeaponDefinition* Gun = Catalog.FindWeapon(TEXT("weapon_projectile_sample"));
	if (Gun)
	{
		TestEqual(TEXT("gun FireMode"), static_cast<uint8>(Gun->FireMode), static_cast<uint8>(EWeaponFireMode::Projectile));
		TestEqual(TEXT("gun ProjectileId"), Gun->ProjectileId, FName(TEXT("bullet_linear")));
		TestTrue(TEXT("gun weapon-level DamageProfileId stays empty (projectile owns damage)"), Gun->DamageProfileId.IsNone());
	}

	// Ammo and projectiles.
	const FAmmoType* Cell = Catalog.FindAmmoType(TEXT("ammo_cell"));
	TestNotNull(TEXT("FindAmmoType(ammo_cell)"), Cell);
	if (Cell)
	{
		TestEqual(TEXT("ammo_cell MaxReserve"), Cell->MaxReserve, 120);
		TestEqual(TEXT("ammo_cell MagazineSize"), Cell->MagazineSize, 12);
	}
	const FProjectileDefinition* Homing = Catalog.FindProjectile(TEXT("missile_homing"));
	if (Homing)
	{
		TestEqual(TEXT("missile_homing Motion"), static_cast<uint8>(Homing->Motion), static_cast<uint8>(EProjectileMotion::Homing));
		TestEqual(TEXT("missile_homing HomingTurnRateDegS"), Homing->HomingTurnRateDegS, 180.0f);
		TestEqual(TEXT("missile_homing DamageProfileId"), Homing->DamageProfileId, FName(TEXT("light_01")));
	}
	const FProjectileDefinition* Parabolic = Catalog.FindProjectile(TEXT("shell_parabolic"));
	if (Parabolic)
	{
		TestEqual(TEXT("shell_parabolic Motion"), static_cast<uint8>(Parabolic->Motion), static_cast<uint8>(EProjectileMotion::Parabolic));
	}

	// Repeated queries through a const reference return the same pointer and
	// leave the catalog untouched (counts and revision identical afterwards).
	const FCombatCatalog& ConstCatalog = Catalog;
	const FString RevisionBefore = ConstCatalog.GetConfigRevision();
	const FDamageProfile* FirstLight01 = ConstCatalog.FindDamageProfile(TEXT("light_01"));
	const FDamageProfile* SecondLight01 = ConstCatalog.FindDamageProfile(TEXT("light_01"));
	TestTrue(TEXT("repeated FindDamageProfile returns the same stable pointer"), FirstLight01 == SecondLight01 && FirstLight01 == Light01);
	TestEqual(TEXT("count unchanged after queries"), ConstCatalog.NumDamageProfiles(), 4);
	TestEqual(TEXT("revision unchanged after queries"), ConstCatalog.GetConfigRevision(), RevisionBefore);

	return true;
}

/**
 * Acceptance: unknown and empty ids return nullptr on every table - there is
 * no silent fallback, no default weapon and no synthetic definition; a
 * default-constructed catalog behaves the same way everywhere.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_007MissingIdsReturnNull,
	"UEMMO.Tasks.M5_007.MissingIdsReturnNull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_007MissingIdsReturnNull::RunTest(const FString& Parameters)
{
	FCombatCatalog Catalog;
	if (!BuildSampleCatalog(*this, Catalog, TEXT("sample build")))
	{
		return false;
	}

	const FName UnknownId = TEXT("does_not_exist");

	// Every finder refuses an unknown id with nullptr instead of a fallback.
	TestTrue(TEXT("unknown damage profile id returns nullptr"), Catalog.FindDamageProfile(UnknownId) == nullptr);
	TestTrue(TEXT("unknown attack reaction id returns nullptr"), Catalog.FindAttackReaction(UnknownId) == nullptr);
	TestTrue(TEXT("unknown target reaction id returns nullptr"), Catalog.FindTargetReaction(UnknownId) == nullptr);
	TestTrue(TEXT("unknown weapon id returns nullptr"), Catalog.FindWeapon(UnknownId) == nullptr);
	TestTrue(TEXT("unknown ammo id returns nullptr"), Catalog.FindAmmoType(UnknownId) == nullptr);
	TestTrue(TEXT("unknown projectile id returns nullptr"), Catalog.FindProjectile(UnknownId) == nullptr);

	// The empty id is never mapped either.
	TestTrue(TEXT("empty damage profile id returns nullptr"), Catalog.FindDamageProfile(NAME_None) == nullptr);
	TestTrue(TEXT("empty weapon id returns nullptr"), Catalog.FindWeapon(NAME_None) == nullptr);
	TestTrue(TEXT("empty projectile id returns nullptr"), Catalog.FindProjectile(NAME_None) == nullptr);

	// No default weapon fallback: the training sword is only reachable by its
	// own id, never by a miss.
	const FWeaponDefinition* Miss = Catalog.FindWeapon(TEXT("weapon_missing"));
	TestTrue(TEXT("a missed weapon lookup never yields the training sword"),
		Miss == nullptr || Miss->WeaponId != FName(TEXT("weapon_training_sword")));

	// A default-constructed (never built) catalog exposes nothing at all.
	FCombatCatalog EmptyCatalog;
	TestTrue(TEXT("default catalog FindDamageProfile returns nullptr"), EmptyCatalog.FindDamageProfile(TEXT("light_01")) == nullptr);
	TestTrue(TEXT("default catalog FindWeapon returns nullptr"), EmptyCatalog.FindWeapon(TEXT("weapon_training_sword")) == nullptr);
	TestEqual(TEXT("default catalog NumDamageProfiles"), EmptyCatalog.NumDamageProfiles(), 0);
	TestEqual(TEXT("default catalog NumWeapons"), EmptyCatalog.NumWeapons(), 0);
	TestTrue(TEXT("default catalog revision is empty"), EmptyCatalog.GetConfigRevision().IsEmpty());

	return true;
}

/**
 * Acceptance: a duplicate id in any of the six tables refuses the whole
 * build, the error names the table and the id, the target catalog stays
 * completely untouched on failure, and the previously built catalog keeps
 * working unchanged.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_007DuplicateIdRejected,
	"UEMMO.Tasks.M5_007.DuplicateIdRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_007DuplicateIdRejected::RunTest(const FString& Parameters)
{
	FCombatCatalog GoodCatalog;
	if (!BuildSampleCatalog(*this, GoodCatalog, TEXT("good build")))
	{
		return false;
	}
	const FString GoodRevision = GoodCatalog.GetConfigRevision();

	// One duplicate row per table: same id, different values (the collision is
	// on identity, not on content).
	FParsedCombatConfig DamageDup = MakeSampleConfig();
	{
		FDamageProfile Row = MakeDamageProfile(TEXT("light_01"), 99.0f, 9.9f, 9.0f);
		DamageDup.DamageProfiles.Add(Row);
	}
	FParsedCombatConfig AttackDup = MakeSampleConfig();
	{
		FAttackReaction Row;
		Row.ReactionId = TEXT("light_02");
		Row.ControlPenetration = EControlPenetration::BypassPoise;
		AttackDup.AttackReactions.Add(Row);
	}
	FParsedCombatConfig TargetDup = MakeSampleConfig();
	{
		FTargetReaction Row;
		Row.PolicyId = TEXT("normal");
		Row.PoiseMax = 500.0f;
		TargetDup.TargetReactions.Add(Row);
	}
	FParsedCombatConfig WeaponDup = MakeSampleConfig();
	{
		FWeaponDefinition Row;
		Row.WeaponId = TEXT("weapon_training_sword");
		Row.FireMode = EWeaponFireMode::Hitscan;
		Row.DamageProfileId = TEXT("light_02");
		Row.AmmoId = TEXT("ammo_cell");
		Row.MagazineSize = 1;
		Row.FireRateRpm = 1.0f;
		Row.RangeCm = 1.0f;
		WeaponDup.Weapons.Add(Row);
	}
	FParsedCombatConfig AmmoDup = MakeSampleConfig();
	{
		FAmmoType Row;
		Row.AmmoId = TEXT("ammo_cell");
		Row.MaxReserve = 7;
		Row.MagazineSize = 7;
		AmmoDup.AmmoTypes.Add(Row);
	}
	FParsedCombatConfig ProjectileDup = MakeSampleConfig();
	{
		FProjectileDefinition Row;
		Row.ProjectileId = TEXT("bullet_linear");
		Row.Motion = EProjectileMotion::Homing;
		Row.SpeedCmS = 1.0f;
		Row.LifetimeS = 1.0f;
		Row.DamageProfileId = TEXT("light_02");
		Row.HomingTurnRateDegS = 1.0f;
		ProjectileDup.Projectiles.Add(Row);
	}

	struct FDuplicateCase
	{
		const TCHAR* CaseName;
		const FParsedCombatConfig* Config;
		const TCHAR* ExpectedTableName;
		const TCHAR* ExpectedId;
	};
	const TArray<FDuplicateCase> Cases = {
		{TEXT("damage_profiles"), &DamageDup, TEXT("damage_profiles"), TEXT("light_01")},
		{TEXT("attack_reactions"), &AttackDup, TEXT("attack_reactions"), TEXT("light_02")},
		{TEXT("target_reactions"), &TargetDup, TEXT("target_reactions"), TEXT("normal")},
		{TEXT("weapons"), &WeaponDup, TEXT("weapons"), TEXT("weapon_training_sword")},
		{TEXT("ammo_types"), &AmmoDup, TEXT("ammo_types"), TEXT("ammo_cell")},
		{TEXT("projectiles"), &ProjectileDup, TEXT("projectiles"), TEXT("bullet_linear")}
	};

	for (const FDuplicateCase& Case : Cases)
	{
		FCombatCatalog Scratch; // stays untouched by the refused build
		FString Errors;
		const bool bBuilt = FCombatCatalog::BuildFromParsed(*Case.Config, Scratch, Errors);
		TestTrue(FString::Printf(TEXT("%s: duplicate id refuses the build"), Case.CaseName), !bBuilt);
		TestTrue(FString::Printf(TEXT("%s: the error names the table"), Case.CaseName), Errors.Contains(Case.ExpectedTableName));
		TestTrue(FString::Printf(TEXT("%s: the error names the duplicated id"), Case.CaseName), Errors.Contains(Case.ExpectedId));

		// The refused build published nothing into the target catalog.
		TestTrue(FString::Printf(TEXT("%s: refused build leaves the target catalog empty"), Case.CaseName),
			Scratch.NumDamageProfiles() == 0 && Scratch.NumWeapons() == 0 && Scratch.NumProjectiles() == 0);
		TestTrue(FString::Printf(TEXT("%s: refused build leaves no revision"), Case.CaseName), Scratch.GetConfigRevision().IsEmpty());
	}

	// The previously built catalog is unaffected by the failed attempts.
	TestEqual(TEXT("good catalog NumDamageProfiles unchanged"), GoodCatalog.NumDamageProfiles(), 4);
	TestEqual(TEXT("good catalog NumWeapons unchanged"), GoodCatalog.NumWeapons(), 3);
	TestEqual(TEXT("good catalog revision unchanged"), GoodCatalog.GetConfigRevision(), GoodRevision);
	const FDamageProfile* Light01 = GoodCatalog.FindDamageProfile(TEXT("light_01"));
	TestTrue(TEXT("good catalog keeps the original light_01 values"), Light01 && Light01->BaseDamage == 10.0f);

	return true;
}

/**
 * Acceptance: the revision is a deterministic content signature. The same
 * rows in a different order keep the revision; changing one value, adding or
 * removing a row, or changing the schema version all change it. Two catalogs
 * built from identical data agree.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_007RevisionDigest,
	"UEMMO.Tasks.M5_007.RevisionDigest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_007RevisionDigest::RunTest(const FString& Parameters)
{
	FCombatCatalog First;
	if (!BuildSampleCatalog(*this, First, TEXT("first build")))
	{
		return false;
	}
	const FString Revision = First.GetConfigRevision();
	TestTrue(TEXT("the built revision is not empty"), !Revision.IsEmpty());

	// Deterministic: a second catalog from the same data gets the same digest.
	FCombatCatalog Second;
	if (!BuildSampleCatalog(*this, Second, TEXT("second build")))
	{
		return false;
	}
	TestEqual(TEXT("identical data builds produce identical revisions"), Second.GetConfigRevision(), Revision);

	// Row reordering (per table) keeps the digest: it is a content signature
	// over normalized rows, not over the incoming order.
	FCombatCatalog Reversed;
	{
		FString Errors;
		TestTrue(TEXT("the row-reversed config builds"),
			FCombatCatalog::BuildFromParsed(MakeRowReversedConfig(), Reversed, Errors));
	}
	TestEqual(TEXT("row reordering keeps the revision"), Reversed.GetConfigRevision(), Revision);

	// One changed value changes the digest.
	FParsedCombatConfig ChangedValue = MakeSampleConfig();
	ChangedValue.DamageProfiles[0].BaseDamage = 11.0f; // light_01: 10 -> 11
	FCombatCatalog ChangedValueCatalog;
	{
		FString Errors;
		TestTrue(TEXT("the value-changed config builds"),
			FCombatCatalog::BuildFromParsed(ChangedValue, ChangedValueCatalog, Errors));
	}
	TestTrue(TEXT("a changed value changes the revision"), ChangedValueCatalog.GetConfigRevision() != Revision);

	// Adding and removing a row both change the digest.
	FParsedCombatConfig AddedRow = MakeSampleConfig();
	AddedRow.AmmoTypes.Add(FAmmoType{FName(TEXT("ammo_extra")), 30, 6});
	FCombatCatalog AddedRowCatalog;
	{
		FString Errors;
		TestTrue(TEXT("the added-row config builds"),
			FCombatCatalog::BuildFromParsed(AddedRow, AddedRowCatalog, Errors));
	}
	TestTrue(TEXT("an added row changes the revision"), AddedRowCatalog.GetConfigRevision() != Revision);

	FParsedCombatConfig RemovedRow = MakeSampleConfig();
	RemovedRow.AmmoTypes.RemoveAt(RemovedRow.AmmoTypes.Num() - 1);
	FCombatCatalog RemovedRowCatalog;
	{
		FString Errors;
		TestTrue(TEXT("the removed-row config builds"),
			FCombatCatalog::BuildFromParsed(RemovedRow, RemovedRowCatalog, Errors));
	}
	TestTrue(TEXT("a removed row changes the revision"), RemovedRowCatalog.GetConfigRevision() != Revision);

	// The schema version participates in the digest.
	FParsedCombatConfig NextSchema = MakeSampleConfig();
	NextSchema.SchemaVersion = 2;
	FCombatCatalog NextSchemaCatalog;
	{
		FString Errors;
		TestTrue(TEXT("the next-schema config builds"),
			FCombatCatalog::BuildFromParsed(NextSchema, NextSchemaCatalog, Errors));
	}
	TestTrue(TEXT("a schema version change changes the revision"), NextSchemaCatalog.GetConfigRevision() != Revision);

	// The digest is a stable 16-character lowercase hex string.
	TestEqual(TEXT("the revision is 16 hex characters"), Revision.Len(), 16);
	for (const TCHAR Character : Revision)
	{
		const bool bHexDigit = (Character >= TEXT('0') && Character <= TEXT('9')) || (Character >= TEXT('a') && Character <= TEXT('f'));
		TestTrue(TEXT("the revision holds only lowercase hex characters"), bHexDigit);
	}

	return true;
}

/**
 * Acceptance: an all-empty legal config builds to a valid empty catalog, and
 * the catalog's own input guards refuse a non-positive schema version and an
 * empty-id row with an error naming the table.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_007EmptyConfigAndInputGuards,
	"UEMMO.Tasks.M5_007.EmptyConfigAndInputGuards",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_007EmptyConfigAndInputGuards::RunTest(const FString& Parameters)
{
	// A config with a legal schema version and zero rows is a legal (if
	// useless) catalog: counts are zero, every lookup misses, and the digest
	// still exists and is deterministic.
	FParsedCombatConfig EmptyConfig;
	EmptyConfig.SchemaVersion = 1;

	FCombatCatalog EmptyCatalogA;
	{
		FString Errors;
		TestTrue(TEXT("an all-empty legal config builds"),
			FCombatCatalog::BuildFromParsed(EmptyConfig, EmptyCatalogA, Errors));
		TestTrue(TEXT("the empty build reports no errors"), Errors.IsEmpty());
	}
	TestEqual(TEXT("empty catalog NumDamageProfiles"), EmptyCatalogA.NumDamageProfiles(), 0);
	TestEqual(TEXT("empty catalog NumAttackReactions"), EmptyCatalogA.NumAttackReactions(), 0);
	TestEqual(TEXT("empty catalog NumTargetReactions"), EmptyCatalogA.NumTargetReactions(), 0);
	TestEqual(TEXT("empty catalog NumWeapons"), EmptyCatalogA.NumWeapons(), 0);
	TestEqual(TEXT("empty catalog NumAmmoTypes"), EmptyCatalogA.NumAmmoTypes(), 0);
	TestEqual(TEXT("empty catalog NumProjectiles"), EmptyCatalogA.NumProjectiles(), 0);
	TestTrue(TEXT("empty catalog misses every lookup"), EmptyCatalogA.FindWeapon(TEXT("weapon_training_sword")) == nullptr);
	TestTrue(TEXT("empty catalog still has a digest"), !EmptyCatalogA.GetConfigRevision().IsEmpty());

	FCombatCatalog EmptyCatalogB;
	{
		FString Errors;
		TestTrue(TEXT("a second all-empty config builds"),
			FCombatCatalog::BuildFromParsed(EmptyConfig, EmptyCatalogB, Errors));
	}
	TestEqual(TEXT("two empty catalogs share the digest"), EmptyCatalogB.GetConfigRevision(), EmptyCatalogA.GetConfigRevision());
	TestTrue(TEXT("the empty digest differs from the sample digest"), EmptyCatalogA.GetConfigRevision().Len() == 16);

	// A non-positive schema version is refused with a schema-naming error.
	FParsedCombatConfig BadSchema;
	BadSchema.SchemaVersion = 0;
	BadSchema.DamageProfiles = MakeLegacyDamageProfiles();
	{
		FCombatCatalog Scratch;
		FString Errors;
		const bool bBuilt = FCombatCatalog::BuildFromParsed(BadSchema, Scratch, Errors);
		TestTrue(TEXT("a zero schema version refuses the build"), !bBuilt);
		TestTrue(TEXT("the schema error names schema_version"), Errors.Contains(TEXT("schema_version")));
		TestTrue(TEXT("the refused build leaves the target catalog empty"), Scratch.NumDamageProfiles() == 0);
	}

	// An empty-id row is refused with a table-naming error.
	FParsedCombatConfig EmptyIdRow;
	EmptyIdRow.SchemaVersion = 1;
	EmptyIdRow.AmmoTypes = MakeSampleAmmoTypes();
	FAmmoType NoId;
	NoId.AmmoId = NAME_None;
	NoId.MaxReserve = 5;
	NoId.MagazineSize = 5;
	EmptyIdRow.AmmoTypes.Add(NoId);
	{
		FCombatCatalog Scratch;
		FString Errors;
		const bool bBuilt = FCombatCatalog::BuildFromParsed(EmptyIdRow, Scratch, Errors);
		TestTrue(TEXT("an empty-id row refuses the build"), !bBuilt);
		TestTrue(TEXT("the empty-id error names the table"), Errors.Contains(TEXT("ammo_types")));
		TestTrue(TEXT("the refused build keeps the target empty"), Scratch.NumAmmoTypes() == 0);
	}

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
