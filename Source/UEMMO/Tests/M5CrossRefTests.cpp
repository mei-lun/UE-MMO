// M5-006: cross-table id and reference validation for the combat source
// tables (M5 interface contract section 1, first owner 006 together with
// 005). Pins Combat/Data/CombatReferenceValidator.h:
//
// - ValidateAllReferences(const FParsedCombatConfig&): the six-table release
//   gate. weapons.damage_profile_id / ammo_id / projectile_id resolve, the
//   melee attack ids are exactly the complete legacy four-attack set and
//   every attack id resolves into an attack_reactions row (reaction_id) and
//   a damage_profiles row (the attack magnitudes), and
//   projectiles.damage_profile_id / explosion_damage_profile_id resolve.
// - ValidateAllReferences(const FParsedCombatConfig&, const
//   TArray<FCombatPresentation>&): the same gate plus the presentations
//   table (the presentations live outside FParsedCombatConfig - M5-007 froze
//   that struct with the six tables). Every presentation reaction_id
//   resolves into attack_reactions and every attack reaction keeps exactly
//   one presentation mapping.
// - duplicate row ids inside any input table are refused (defense in depth;
//   the parser already refuses them at the file level, but the candidate
//   struct can also be assembled by other code paths).
// - every problem names the source table, the row id and the field, so a
//   dangling reference can be fixed at its source.
//
// Positive runs use the shipped Data/CombatSystem tables. The shipped
// weapons/projectiles reference the damage profiles physical_10 and
// explosive_40, which the 003 sample damage_profiles.json does not define
// (M5-005 report: the alignment belongs to this card or to a coordinator
// data fix; Data/** is frozen for this card, so the two rows are supplied by
// the test). The integration test pins both truths: the raw shipped set
// reports only that known gap, and the shipped content passes the full gate
// once the two profiles exist.
//
// Pure logic only: no World, no UE assets, no wall clocks.

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

#include "../Combat/Data/CombatDataTableParser.h"
#include "../Combat/Data/CombatReferenceValidator.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_006
{
	// ------------------------------------------------------------------
	// Row makers: one legal row per table, following the shipped samples.
	// ------------------------------------------------------------------

	static FDamageProfile MakeDamageProfile(const FName Id)
	{
		FDamageProfile Profile;
		Profile.DamageProfileId = Id;
		Profile.BaseDamage = 10.0f;
		Profile.AttackCoefficient = 1.0f;
		Profile.HitStunSeconds = 0.2f;
		Profile.KnockbackCmPerSecond = 90.0f;
		Profile.LaunchCmPerSecond = 0.0f;
		Profile.HitStopSeconds = 0.04f;
		return Profile;
	}

	static FAttackReaction MakeAttackReaction(const FName Id)
	{
		FAttackReaction Reaction;
		Reaction.ReactionId = Id;
		Reaction.ControlPenetration = EControlPenetration::None;
		return Reaction;
	}

	static FTargetReaction MakeTargetReaction(const FName Id)
	{
		FTargetReaction Policy; // defaults are the legacy normal target
		Policy.PolicyId = Id;
		return Policy;
	}

	static FAmmoType MakeAmmoType(const FName Id)
	{
		FAmmoType Ammo;
		Ammo.AmmoId = Id;
		Ammo.MaxReserve = 120;
		Ammo.MagazineSize = 12;
		return Ammo;
	}

	static FProjectileDefinition MakeProjectile(const FName Id, const FName DamageProfileId)
	{
		FProjectileDefinition Projectile;
		Projectile.ProjectileId = Id;
		Projectile.Motion = EProjectileMotion::Straight;
		Projectile.SpeedCmS = 6000.0f;
		Projectile.LifetimeS = 2.0f;
		Projectile.DamageProfileId = DamageProfileId;
		return Projectile;
	}

	static FWeaponDefinition MakeMeleeWeapon(const FName Id, const FName DamageProfileId)
	{
		FWeaponDefinition Weapon;
		Weapon.WeaponId = Id;
		Weapon.FireMode = EWeaponFireMode::Melee;
		Weapon.DamageProfileId = DamageProfileId;
		Weapon.MeleeAttackIds = GetLegacyMeleeAttackIds();
		return Weapon;
	}

	static FWeaponDefinition MakeProjectileWeapon(const FName Id, const FName ProjectileId, const FName AmmoId)
	{
		FWeaponDefinition Weapon;
		Weapon.WeaponId = Id;
		Weapon.FireMode = EWeaponFireMode::Projectile;
		Weapon.ProjectileId = ProjectileId;
		Weapon.AmmoId = AmmoId;
		Weapon.MagazineSize = 12;
		Weapon.FireRateRpm = 300.0f;
		return Weapon;
	}

	static FCombatPresentation MakePresentation(const FName Id, const FName ReactionId)
	{
		FCombatPresentation Presentation;
		Presentation.PresentationId = Id;
		Presentation.ReactionId = ReactionId;
		Presentation.bPlaceholder = true;
		return Presentation;
	}

	/**
	 * A fully resolved minimal config: the legacy melee chain (attack ids,
	 * one attack reaction and one damage profile per attack), a multi-pellet
	 * hitscan weapon, two projectile weapons (one plain, one exploding), the
	 * shared ammo kind and the three shipped target policies (normal, heavy,
	 * boss). Everything the six-table gate and the presentation gate need.
	 */
	static FParsedCombatConfig MakeValidConfig()
	{
		FParsedCombatConfig Config;
		Config.SchemaVersion = 1;

		for (const FName AttackId : GetLegacyMeleeAttackIds())
		{
			Config.DamageProfiles.Add(MakeDamageProfile(AttackId));
			Config.AttackReactions.Add(MakeAttackReaction(AttackId));
		}
		Config.DamageProfiles.Add(MakeDamageProfile(TEXT("profile_gun")));
		Config.DamageProfiles.Add(MakeDamageProfile(TEXT("profile_explosion")));

		Config.TargetReactions.Add(MakeTargetReaction(TEXT("normal")));

		FTargetReaction Heavy = MakeTargetReaction(TEXT("heavy"));
		Heavy.bAllowLaunch = false; // launch immune through the explicit gate
		Heavy.PoiseMax = 200.0f;
		Config.TargetReactions.Add(Heavy);

		FTargetReaction Boss = MakeTargetReaction(TEXT("boss"));
		Boss.bImmuneControl = true; // separate axes: still takes damage
		Boss.bDeathResistant = true;
		Boss.MaxLaunchesPerAirCycle = 0;
		Boss.LaunchZScales = {1.0f};
		Config.TargetReactions.Add(Boss);

		Config.AmmoTypes.Add(MakeAmmoType(TEXT("ammo_cell")));

		Config.Projectiles.Add(MakeProjectile(TEXT("bullet_test"), TEXT("profile_gun")));

		FProjectileDefinition Rocket = MakeProjectile(TEXT("rocket_test"), TEXT("profile_gun"));
		Rocket.ExplosionRadiusCm = 300.0f;
		Rocket.ExplosionDamageProfileId = TEXT("profile_explosion");
		Config.Projectiles.Add(Rocket);

		Config.Weapons.Add(MakeMeleeWeapon(TEXT("weapon_sword"), TEXT("light_01")));

		FWeaponDefinition PelletGun; // the legal multi-pellet strategy
		PelletGun.WeaponId = TEXT("weapon_pellet_gun");
		PelletGun.FireMode = EWeaponFireMode::Hitscan;
		PelletGun.DamageProfileId = TEXT("profile_gun");
		PelletGun.AmmoId = TEXT("ammo_cell");
		PelletGun.MagazineSize = 6;
		PelletGun.FireRateRpm = 90.0f;
		PelletGun.PelletCount = 5;
		PelletGun.SpreadDegrees = 8.0f;
		PelletGun.RangeCm = 2500.0f;
		Config.Weapons.Add(PelletGun);

		Config.Weapons.Add(MakeProjectileWeapon(TEXT("weapon_gun"), TEXT("bullet_test"), TEXT("ammo_cell")));
		Config.Weapons.Add(MakeProjectileWeapon(TEXT("weapon_rocket_gun"), TEXT("rocket_test"), TEXT("ammo_cell")));

		return Config;
	}

	/** The presentation rows of MakeValidConfig: one per legacy attack reaction. */
	static TArray<FCombatPresentation> MakeValidPresentations()
	{
		TArray<FCombatPresentation> Presentations;
		for (const FName AttackId : GetLegacyMeleeAttackIds())
		{
			Presentations.Add(MakePresentation(
				FName(*FString::Printf(TEXT("hit_%s"), *AttackId.ToString())), AttackId));
		}
		return Presentations;
	}

	/** Adapts the parser output set into the catalog candidate struct. */
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

	static TArray<FCombatPresentation> ToPresentationArray(const FCombatDataTableSet& Tables)
	{
		TArray<FCombatPresentation> Presentations;
		for (const TPair<FName, FCombatPresentation>& Row : Tables.Presentations)
		{
			Presentations.Add(Row.Value);
		}
		return Presentations;
	}

	/**
	 * Adds the damage profiles the shipped 004 sample rows reference but the
	 * shipped 003 sample table does not define yet (physical_10,
	 * explosive_40). Idempotent: a profile already present is kept, so the
	 * helper stays correct if the shipped table is aligned later.
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
			Config.DamageProfiles.Add(MakeDamageProfile(TEXT("physical_10")));
		}
		if (!bHasExplosive40)
		{
			FDamageProfile Explosion = MakeDamageProfile(TEXT("explosive_40"));
			Explosion.BaseDamage = 40.0f;
			Config.DamageProfiles.Add(Explosion);
		}
	}

	static FWeaponDefinition* FindWeapon(FParsedCombatConfig& Config, const FName Id)
	{
		for (FWeaponDefinition& Weapon : Config.Weapons)
		{
			if (Weapon.WeaponId == Id)
			{
				return &Weapon;
			}
		}
		return nullptr;
	}

	static FProjectileDefinition* FindProjectile(FParsedCombatConfig& Config, const FName Id)
	{
		for (FProjectileDefinition& Projectile : Config.Projectiles)
		{
			if (Projectile.ProjectileId == Id)
			{
				return &Projectile;
			}
		}
		return nullptr;
	}

	static FCombatPresentation* FindPresentation(TArray<FCombatPresentation>& Presentations, const FName Id)
	{
		for (FCombatPresentation& Presentation : Presentations)
		{
			if (Presentation.PresentationId == Id)
			{
				return &Presentation;
			}
		}
		return nullptr;
	}

	static void RemovePresentation(TArray<FCombatPresentation>& Presentations, const FName Id)
	{
		for (int32 Index = Presentations.Num() - 1; Index >= 0; --Index)
		{
			if (Presentations[Index].PresentationId == Id)
			{
				Presentations.RemoveAt(Index);
			}
		}
	}

	static void RemoveAttackReaction(FParsedCombatConfig& Config, const FName Id)
	{
		for (int32 Index = Config.AttackReactions.Num() - 1; Index >= 0; --Index)
		{
			if (Config.AttackReactions[Index].ReactionId == Id)
			{
				Config.AttackReactions.RemoveAt(Index);
			}
		}
	}

	static void RemoveDamageProfile(FParsedCombatConfig& Config, const FName Id)
	{
		for (int32 Index = Config.DamageProfiles.Num() - 1; Index >= 0; --Index)
		{
			if (Config.DamageProfiles[Index].DamageProfileId == Id)
			{
				Config.DamageProfiles.RemoveAt(Index);
			}
		}
	}

	/** True when at least one problem string contains every given token. */
	static bool AnyProblemContainsAll(const TArray<FString>& Problems, const TArray<FString>& Tokens)
	{
		for (const FString& Problem : Problems)
		{
			bool bAll = true;
			for (const FString& Token : Tokens)
			{
				if (!Problem.Contains(Token))
				{
					bAll = false;
					break;
				}
			}
			if (bAll)
			{
				return true;
			}
		}
		return false;
	}

	/** Joins problems for failure messages so the offending text is visible. */
	static FString JoinProblems(const TArray<FString>& Problems)
	{
		if (Problems.Num() == 0)
		{
			return TEXT("(no problems)");
		}
		return FString::Join(Problems, TEXT(" | "));
	}
}

using namespace UE::UEMMO::Tasks::M5_006;

// ---------------------------------------------------------------------------
// ValidConfigPasses
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_006ValidConfigPasses,
	"UEMMO.Tasks.M5_006.ValidConfigPasses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_006ValidConfigPasses::RunTest(const FString& Parameters)
{
	const FParsedCombatConfig Config = MakeValidConfig();
	const TArray<FCombatPresentation> Presentations = MakeValidPresentations();

	// Sanity: the passing set really carries the legal multi-pellet strategy
	// and the heavy/boss target policies (task acceptance: legal multi-pellet
	// and heavy strategies pass without weapon_id special cases).
	bool bHasMultiPellet = false;
	for (const FWeaponDefinition& Weapon : Config.Weapons)
	{
		bHasMultiPellet = bHasMultiPellet
			|| (Weapon.WeaponId == FName(TEXT("weapon_pellet_gun")) && Weapon.PelletCount == 5);
	}
	TestTrue(TEXT("the valid set carries a multi-pellet weapon"), bHasMultiPellet);

	bool bHasHeavy = false;
	bool bHasBoss = false;
	for (const FTargetReaction& Policy : Config.TargetReactions)
	{
		bHasHeavy = bHasHeavy || Policy.PolicyId == FName(TEXT("heavy"));
		bHasBoss = bHasBoss || Policy.PolicyId == FName(TEXT("boss"));
	}
	TestTrue(TEXT("the valid set carries the heavy policy"), bHasHeavy);
	TestTrue(TEXT("the valid set carries the boss policy"), bHasBoss);

	// The six-table gate passes a fully resolved config.
	TArray<FString> Problems = ValidateAllReferences(Config);
	TestTrue(FString::Printf(TEXT("the six-table gate passes a fully resolved config (%s)"),
		*JoinProblems(Problems)), Problems.Num() == 0);

	// The full gate with the presentation rows passes the same config.
	TArray<FString> FullProblems = ValidateAllReferences(Config, Presentations);
	TestTrue(FString::Printf(TEXT("the full gate passes a fully resolved config (%s)"),
		*JoinProblems(FullProblems)), FullProblems.Num() == 0);

	return true;
}

// ---------------------------------------------------------------------------
// ShippedTablesIntegration
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_006ShippedTablesIntegration,
	"UEMMO.Tasks.M5_006.ShippedTablesIntegration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_006ShippedTablesIntegration::RunTest(const FString& Parameters)
{
	FCombatDataTableSet Tables;
	TArray<FString> LoadProblems;
	if (!TestTrue(TEXT("the shipped Data/CombatSystem tables load"),
		LoadCombatDataDirectory(FPaths::ProjectDir() / TEXT("Data") / TEXT("CombatSystem"), Tables, LoadProblems)))
	{
		for (const FString& Problem : LoadProblems)
		{
			AddError(FString::Printf(TEXT("unexpected load problem: %s"), *Problem));
		}
		return true;
	}

	const FParsedCombatConfig Config = ToParsedConfig(Tables);
	const TArray<FCombatPresentation> Presentations = ToPresentationArray(Tables);

	// The shipped 004 sample rows reference physical_10/explosive_40, which
	// the shipped 003 sample table does not define (M5-005 report; Data/** is
	// frozen for this card). The gate must report that known gap and nothing
	// else - every problem names 'damage_profile_id' and one of the two ids.
	{
		TArray<FString> Problems = ValidateAllReferences(Config, Presentations);
		bool bAllKnownGap = true;
		for (const FString& Problem : Problems)
		{
			const bool bKnownGap = Problem.Contains(TEXT("damage_profile_id"))
				&& (Problem.Contains(TEXT("physical_10")) || Problem.Contains(TEXT("explosive_40")));
			bAllKnownGap = bAllKnownGap && bKnownGap;
		}
		TestTrue(FString::Printf(TEXT("shipped tables report only the known damage-profile alignment gap (%s)"),
			*JoinProblems(Problems)), bAllKnownGap);

		TestFalse(TEXT("no shipped ammo axis is flagged"),
			AnyProblemContainsAll(Problems, {TEXT("ammo_id")}));
		TestFalse(TEXT("no shipped projectile axis is flagged"),
			AnyProblemContainsAll(Problems, {TEXT("'projectile_id'")}));
		TestFalse(TEXT("no shipped reaction axis is flagged"),
			AnyProblemContainsAll(Problems, {TEXT("'reaction_id'")}));
		TestFalse(TEXT("no shipped melee set is flagged"),
			AnyProblemContainsAll(Problems, {TEXT("melee_attack_ids")}));
		TestFalse(TEXT("no shipped presentation axis is flagged"),
			AnyProblemContainsAll(Problems, {TEXT("presentations: ")}));
	}

	// Once the two sample profiles exist, the whole shipped content passes
	// both gate entries.
	{
		FParsedCombatConfig Aligned = ToParsedConfig(Tables);
		TArray<FCombatPresentation> AlignedPresentations = ToPresentationArray(Tables);
		AlignShippedSampleDamageProfiles(Aligned);

		TArray<FString> AlignedProblems = ValidateAllReferences(Aligned, AlignedPresentations);
		TestTrue(FString::Printf(TEXT("the aligned shipped tables pass the full gate (%s)"),
			*JoinProblems(AlignedProblems)), AlignedProblems.Num() == 0);

		TArray<FString> AlignedSixTableProblems = ValidateAllReferences(Aligned);
		TestTrue(FString::Printf(TEXT("the aligned shipped tables pass the six-table gate (%s)"),
			*JoinProblems(AlignedSixTableProblems)), AlignedSixTableProblems.Num() == 0);
	}

	// The passing set really carries the legal multi-pellet sample weapon and
	// the heavy/boss policies.
	bool bHasMultiPellet = false;
	for (const TPair<FName, FWeaponDefinition>& Row : Tables.Weapons)
	{
		bHasMultiPellet = bHasMultiPellet
			|| (Row.Value.WeaponId == FName(TEXT("weapon_pellet_sample")) && Row.Value.PelletCount == 5);
	}
	TestTrue(TEXT("the shipped set carries the multi-pellet sample weapon"), bHasMultiPellet);

	bool bHasHeavy = false;
	bool bHasBoss = false;
	for (const TPair<FName, FTargetReaction>& Row : Tables.TargetReactions)
	{
		bHasHeavy = bHasHeavy || Row.Value.PolicyId == FName(TEXT("heavy"));
		bHasBoss = bHasBoss || Row.Value.PolicyId == FName(TEXT("boss"));
	}
	TestTrue(TEXT("the shipped set carries the heavy policy"), bHasHeavy);
	TestTrue(TEXT("the shipped set carries the boss policy"), bHasBoss);

	return true;
}

// ---------------------------------------------------------------------------
// WeaponReferences
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_006WeaponReferences,
	"UEMMO.Tasks.M5_006.WeaponReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_006WeaponReferences::RunTest(const FString& Parameters)
{
	// A dangling ammo_id is refused and names the table, the row, the field
	// and the missing id.
	{
		FParsedCombatConfig Config = MakeValidConfig();
		FWeaponDefinition* PelletGun = FindWeapon(Config, FName(TEXT("weapon_pellet_gun")));
		if (TestNotNull(TEXT("the pellet gun exists"), PelletGun))
		{
			PelletGun->AmmoId = FName(TEXT("ammo_missing"));
			TArray<FString> Problems = ValidateAllReferences(Config);
			TestEqual(TEXT("one dangling ammo reference is one problem"), Problems.Num(), 1);
			TestTrue(FString::Printf(TEXT("the ammo error names the source (%s)"), *JoinProblems(Problems)),
				AnyProblemContainsAll(Problems, {TEXT("weapons"), TEXT("weapon_pellet_gun"), TEXT("'ammo_id'"), TEXT("ammo_missing")}));
		}
	}

	// A dangling projectile_id is refused.
	{
		FParsedCombatConfig Config = MakeValidConfig();
		FWeaponDefinition* Gun = FindWeapon(Config, FName(TEXT("weapon_gun")));
		if (TestNotNull(TEXT("the projectile weapon exists"), Gun))
		{
			Gun->ProjectileId = FName(TEXT("bullet_missing"));
			TArray<FString> Problems = ValidateAllReferences(Config);
			TestEqual(TEXT("one dangling projectile reference is one problem"), Problems.Num(), 1);
			TestTrue(FString::Printf(TEXT("the projectile error names the source (%s)"), *JoinProblems(Problems)),
				AnyProblemContainsAll(Problems, {TEXT("weapons"), TEXT("weapon_gun"), TEXT("'projectile_id'"), TEXT("bullet_missing")}));
		}
	}

	// A dangling weapon damage_profile_id is refused.
	{
		FParsedCombatConfig Config = MakeValidConfig();
		FWeaponDefinition* Sword = FindWeapon(Config, FName(TEXT("weapon_sword")));
		if (TestNotNull(TEXT("the sword exists"), Sword))
		{
			Sword->DamageProfileId = FName(TEXT("profile_missing"));
			TArray<FString> Problems = ValidateAllReferences(Config);
			TestEqual(TEXT("one dangling weapon profile reference is one problem"), Problems.Num(), 1);
			TestTrue(FString::Printf(TEXT("the weapon profile error names the source (%s)"), *JoinProblems(Problems)),
				AnyProblemContainsAll(Problems, {TEXT("weapons"), TEXT("weapon_sword"), TEXT("'damage_profile_id'"), TEXT("profile_missing")}));
		}
	}

	// The same dangling ammo reference is reported by the full gate too.
	{
		FParsedCombatConfig Config = MakeValidConfig();
		FWeaponDefinition* PelletGun = FindWeapon(Config, FName(TEXT("weapon_pellet_gun")));
		if (TestNotNull(TEXT("the pellet gun exists for the full gate"), PelletGun))
		{
			PelletGun->AmmoId = FName(TEXT("ammo_missing"));
			TArray<FString> Problems = ValidateAllReferences(Config, MakeValidPresentations());
			TestTrue(FString::Printf(TEXT("the full gate reports the dangling ammo reference (%s)"),
				*JoinProblems(Problems)),
				AnyProblemContainsAll(Problems, {TEXT("'ammo_id'"), TEXT("ammo_missing")}));
		}
	}

	// Empty reference fields on the mode-appropriate rows never error: the
	// projectile weapons carry no weapon damage profile and the melee weapon
	// carries no ammo/projectile (covered by ValidConfigPasses).

	return true;
}

// ---------------------------------------------------------------------------
// ProjectileReferences
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_006ProjectileReferences,
	"UEMMO.Tasks.M5_006.ProjectileReferences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_006ProjectileReferences::RunTest(const FString& Parameters)
{
	// A dangling per-hit damage profile is refused.
	{
		FParsedCombatConfig Config = MakeValidConfig();
		FProjectileDefinition* Bullet = FindProjectile(Config, FName(TEXT("bullet_test")));
		if (TestNotNull(TEXT("the bullet exists"), Bullet))
		{
			Bullet->DamageProfileId = FName(TEXT("profile_missing"));
			TArray<FString> Problems = ValidateAllReferences(Config);
			TestEqual(TEXT("one dangling projectile profile reference is one problem"), Problems.Num(), 1);
			TestTrue(FString::Printf(TEXT("the projectile profile error names the source (%s)"), *JoinProblems(Problems)),
				AnyProblemContainsAll(Problems, {TEXT("projectiles"), TEXT("bullet_test"), TEXT("'damage_profile_id'"), TEXT("profile_missing")}));
		}
	}

	// A dangling explosion damage profile is refused.
	{
		FParsedCombatConfig Config = MakeValidConfig();
		FProjectileDefinition* Rocket = FindProjectile(Config, FName(TEXT("rocket_test")));
		if (TestNotNull(TEXT("the rocket exists"), Rocket))
		{
			Rocket->ExplosionDamageProfileId = FName(TEXT("boom_missing"));
			TArray<FString> Problems = ValidateAllReferences(Config);
			TestEqual(TEXT("one dangling explosion profile reference is one problem"), Problems.Num(), 1);
			TestTrue(FString::Printf(TEXT("the explosion profile error names the source (%s)"), *JoinProblems(Problems)),
				AnyProblemContainsAll(Problems, {TEXT("projectiles"), TEXT("rocket_test"), TEXT("'explosion_damage_profile_id'"), TEXT("boom_missing")}));
		}
	}

	return true;
}

// ---------------------------------------------------------------------------
// MeleeAttackSet
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_006MeleeAttackSet,
	"UEMMO.Tasks.M5_006.MeleeAttackSet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_006MeleeAttackSet::RunTest(const FString& Parameters)
{
	// An unknown attack id replacing a legacy one is refused: the id is not
	// part of the legacy set, the replaced member is missing and the unknown
	// id resolves into neither attack_reactions nor damage_profiles.
	{
		FParsedCombatConfig Config = MakeValidConfig();
		FWeaponDefinition* Sword = FindWeapon(Config, FName(TEXT("weapon_sword")));
		if (TestNotNull(TEXT("the sword exists for the unknown case"), Sword))
		{
			Sword->MeleeAttackIds = {TEXT("light_01"), TEXT("light_99"), TEXT("launcher"), TEXT("aerial_01")};
			TArray<FString> Problems = ValidateAllReferences(Config);
			TestTrue(FString::Printf(TEXT("the unknown attack id is refused (%s)"), *JoinProblems(Problems)),
				Problems.Num() > 0);
			TestTrue(FString::Printf(TEXT("the unknown-id error names light_99 (%s)"), *JoinProblems(Problems)),
				AnyProblemContainsAll(Problems, {TEXT("light_99")}));
			TestTrue(FString::Printf(TEXT("the missing legacy member is named (%s)"), *JoinProblems(Problems)),
				AnyProblemContainsAll(Problems, {TEXT("missing 'light_02'")}));
			TestTrue(FString::Printf(TEXT("the unknown attack id reports its missing reaction row (%s)"), *JoinProblems(Problems)),
				AnyProblemContainsAll(Problems, {TEXT("light_99"), TEXT("attack_reactions")}));
			TestTrue(FString::Printf(TEXT("the unknown attack id reports its missing damage profile (%s)"), *JoinProblems(Problems)),
				AnyProblemContainsAll(Problems, {TEXT("light_99"), TEXT("damage_profiles")}));
		}
	}

	// An incomplete legacy set is refused and names the missing member.
	{
		FParsedCombatConfig Config = MakeValidConfig();
		FWeaponDefinition* Sword = FindWeapon(Config, FName(TEXT("weapon_sword")));
		if (TestNotNull(TEXT("the sword exists for the incomplete case"), Sword))
		{
			Sword->MeleeAttackIds = {TEXT("light_01"), TEXT("light_02"), TEXT("launcher")};
			TArray<FString> Problems = ValidateAllReferences(Config);
			TestEqual(TEXT("an incomplete legacy set is exactly one problem"), Problems.Num(), 1);
			TestTrue(FString::Printf(TEXT("the incomplete-set error names the missing member (%s)"), *JoinProblems(Problems)),
				AnyProblemContainsAll(Problems, {TEXT("melee_attack_ids"), TEXT("missing 'aerial_01'")}));
		}
	}

	// A duplicated attack id is refused and named.
	{
		FParsedCombatConfig Config = MakeValidConfig();
		FWeaponDefinition* Sword = FindWeapon(Config, FName(TEXT("weapon_sword")));
		if (TestNotNull(TEXT("the sword exists for the duplicate case"), Sword))
		{
			Sword->MeleeAttackIds = {TEXT("light_01"), TEXT("light_01"), TEXT("launcher"), TEXT("aerial_01")};
			TArray<FString> Problems = ValidateAllReferences(Config);
			TestTrue(FString::Printf(TEXT("the duplicated attack id is refused (%s)"), *JoinProblems(Problems)),
				Problems.Num() > 0);
			TestTrue(FString::Printf(TEXT("the duplicate error names light_01 (%s)"), *JoinProblems(Problems)),
				AnyProblemContainsAll(Problems, {TEXT("light_01"), TEXT("more than once")}));
			TestTrue(FString::Printf(TEXT("the dropped legacy member is named (%s)"), *JoinProblems(Problems)),
				AnyProblemContainsAll(Problems, {TEXT("missing 'light_02'")}));
		}
	}

	// A legacy attack id without an attack_reactions row is refused.
	{
		FParsedCombatConfig Config = MakeValidConfig();
		RemoveAttackReaction(Config, FName(TEXT("launcher")));
		TArray<FString> Problems = ValidateAllReferences(Config);
		TestEqual(TEXT("a missing attack reaction row is exactly one problem"), Problems.Num(), 1);
		TestTrue(FString::Printf(TEXT("the missing-reaction error names the source (%s)"), *JoinProblems(Problems)),
			AnyProblemContainsAll(Problems, {TEXT("weapon_sword"), TEXT("launcher"), TEXT("attack_reactions")}));
	}

	// A legacy attack id without its damage_profiles row is refused.
	{
		FParsedCombatConfig Config = MakeValidConfig();
		RemoveDamageProfile(Config, FName(TEXT("launcher")));
		TArray<FString> Problems = ValidateAllReferences(Config);
		TestEqual(TEXT("a missing attack damage profile is exactly one problem"), Problems.Num(), 1);
		TestTrue(FString::Printf(TEXT("the missing-profile error names the source (%s)"), *JoinProblems(Problems)),
			AnyProblemContainsAll(Problems, {TEXT("weapon_sword"), TEXT("launcher"), TEXT("damage_profiles")}));
	}

	return true;
}

// ---------------------------------------------------------------------------
// PresentationMapping
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_006PresentationMapping,
	"UEMMO.Tasks.M5_006.PresentationMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_006PresentationMapping::RunTest(const FString& Parameters)
{
	// A presentation whose reaction_id resolves into nothing is refused. The
	// retarget also leaves the previously mapped reaction uncovered, so the
	// gate reports both sides of the broken mapping.
	{
		FParsedCombatConfig Config = MakeValidConfig();
		TArray<FCombatPresentation> Presentations = MakeValidPresentations();
		FCombatPresentation* Presentation = FindPresentation(Presentations, FName(TEXT("hit_light_01")));
		if (TestNotNull(TEXT("the hit_light_01 presentation exists"), Presentation))
		{
			Presentation->ReactionId = FName(TEXT("reaction_missing"));
			TArray<FString> Problems = ValidateAllReferences(Config, Presentations);
			TestEqual(TEXT("a dangling presentation reaction is two problems (dangling plus coverage)"), Problems.Num(), 2);
			TestTrue(FString::Printf(TEXT("the presentation error names the source (%s)"), *JoinProblems(Problems)),
				AnyProblemContainsAll(Problems, {TEXT("presentations"), TEXT("hit_light_01"), TEXT("'reaction_id'"), TEXT("reaction_missing")}));
			TestTrue(FString::Printf(TEXT("the uncovered reaction error names light_01 (%s)"), *JoinProblems(Problems)),
				AnyProblemContainsAll(Problems, {TEXT("attack_reactions"), TEXT("light_01"), TEXT("presentations")}));
		}
	}

	// An attack reaction without any presentation mapping is refused.
	{
		FParsedCombatConfig Config = MakeValidConfig();
		TArray<FCombatPresentation> Presentations = MakeValidPresentations();
		FCombatPresentation* Presentation = FindPresentation(Presentations, FName(TEXT("hit_launcher")));
		if (TestNotNull(TEXT("the hit_launcher presentation exists"), Presentation))
		{
			RemovePresentation(Presentations, FName(TEXT("hit_launcher")));
			TArray<FString> Problems = ValidateAllReferences(Config, Presentations);
			TestEqual(TEXT("a missing presentation mapping is exactly one problem"), Problems.Num(), 1);
			TestTrue(FString::Printf(TEXT("the coverage error names the reaction (%s)"), *JoinProblems(Problems)),
				AnyProblemContainsAll(Problems, {TEXT("attack_reactions"), TEXT("launcher"), TEXT("presentations")}));
		}
	}

	// Two presentations mapping one attack reaction are refused.
	{
		FParsedCombatConfig Config = MakeValidConfig();
		TArray<FCombatPresentation> Presentations = MakeValidPresentations();
		Presentations.Add(MakePresentation(TEXT("hit_light_01_alt"), TEXT("light_01")));
		TArray<FString> Problems = ValidateAllReferences(Config, Presentations);
		TestEqual(TEXT("a duplicated presentation mapping is exactly one problem"), Problems.Num(), 1);
		TestTrue(FString::Printf(TEXT("the duplicate-mapping error names both rows (%s)"), *JoinProblems(Problems)),
			AnyProblemContainsAll(Problems, {TEXT("presentations"), TEXT("hit_light_01_alt"), TEXT("light_01")}));
	}

	// A duplicated presentation_id is refused.
	{
		FParsedCombatConfig Config = MakeValidConfig();
		TArray<FCombatPresentation> Presentations = MakeValidPresentations();
		Presentations.Add(MakePresentation(TEXT("hit_light_02"), TEXT("light_01")));
		TArray<FString> Problems = ValidateAllReferences(Config, Presentations);
		TestTrue(FString::Printf(TEXT("the duplicated presentation id is refused (%s)"), *JoinProblems(Problems)),
			Problems.Num() > 0);
		TestTrue(FString::Printf(TEXT("the duplicate-id error names the field and the id (%s)"), *JoinProblems(Problems)),
			AnyProblemContainsAll(Problems, {TEXT("'presentation_id'"), TEXT("hit_light_02")}));
	}

	return true;
}

// ---------------------------------------------------------------------------
// DuplicateRowIds
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_006DuplicateRowIds,
	"UEMMO.Tasks.M5_006.DuplicateRowIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_006DuplicateRowIds::RunTest(const FString& Parameters)
{
	// A duplicated id inside any of the six tables is refused with the table,
	// the id and the id field named (defense in depth; the parser refuses
	// duplicates at the file level, the candidate struct is guarded too).
	// Every case copies the row first so the array never grows from a
	// reference into itself.
	{
		FParsedCombatConfig Config = MakeValidConfig();
		const FWeaponDefinition Copy = Config.Weapons[0];
		Config.Weapons.Add(Copy);
		TArray<FString> Problems = ValidateAllReferences(Config);
		TestTrue(FString::Printf(TEXT("a duplicated weapon id is refused (%s)"), *JoinProblems(Problems)),
			AnyProblemContainsAll(Problems, {TEXT("weapons"), TEXT("weapon_sword"), TEXT("'weapon_id'"), TEXT("duplicates")}));
	}
	{
		FParsedCombatConfig Config = MakeValidConfig();
		const FDamageProfile Copy = Config.DamageProfiles[0];
		Config.DamageProfiles.Add(Copy);
		TArray<FString> Problems = ValidateAllReferences(Config);
		TestTrue(FString::Printf(TEXT("a duplicated damage profile id is refused (%s)"), *JoinProblems(Problems)),
			AnyProblemContainsAll(Problems, {TEXT("damage_profiles"), TEXT("light_01"), TEXT("'damage_profile_id'"), TEXT("duplicates")}));
	}
	{
		FParsedCombatConfig Config = MakeValidConfig();
		const FAttackReaction Copy = Config.AttackReactions[0];
		Config.AttackReactions.Add(Copy);
		TArray<FString> Problems = ValidateAllReferences(Config);
		TestTrue(FString::Printf(TEXT("a duplicated attack reaction id is refused (%s)"), *JoinProblems(Problems)),
			AnyProblemContainsAll(Problems, {TEXT("attack_reactions"), TEXT("light_01"), TEXT("'reaction_id'"), TEXT("duplicates")}));
	}
	{
		FParsedCombatConfig Config = MakeValidConfig();
		const FTargetReaction Copy = Config.TargetReactions[0];
		Config.TargetReactions.Add(Copy);
		TArray<FString> Problems = ValidateAllReferences(Config);
		TestTrue(FString::Printf(TEXT("a duplicated target reaction id is refused (%s)"), *JoinProblems(Problems)),
			AnyProblemContainsAll(Problems, {TEXT("target_reactions"), TEXT("normal"), TEXT("'policy_id'"), TEXT("duplicates")}));
	}
	{
		FParsedCombatConfig Config = MakeValidConfig();
		const FAmmoType Copy = Config.AmmoTypes[0];
		Config.AmmoTypes.Add(Copy);
		TArray<FString> Problems = ValidateAllReferences(Config);
		TestTrue(FString::Printf(TEXT("a duplicated ammo id is refused (%s)"), *JoinProblems(Problems)),
			AnyProblemContainsAll(Problems, {TEXT("ammo_types"), TEXT("ammo_cell"), TEXT("'ammo_id'"), TEXT("duplicates")}));
	}
	{
		FParsedCombatConfig Config = MakeValidConfig();
		const FProjectileDefinition Copy = Config.Projectiles[0];
		Config.Projectiles.Add(Copy);
		TArray<FString> Problems = ValidateAllReferences(Config);
		TestTrue(FString::Printf(TEXT("a duplicated projectile id is refused (%s)"), *JoinProblems(Problems)),
			AnyProblemContainsAll(Problems, {TEXT("projectiles"), TEXT("bullet_test"), TEXT("'projectile_id'"), TEXT("duplicates")}));
	}

	return true;
}

#endif
