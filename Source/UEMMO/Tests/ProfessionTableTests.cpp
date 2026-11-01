#include "Misc/AutomationTest.h"

#include "../Profession/ProfessionCatalog.h"
#include "../Profession/ProfessionTableParser.h"
#include "../Profile/ExperienceCurve.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M7_001
{
	// The shipped source tables, loaded from Data/Professions relative to the
	// project dir (the parser's directory entry is exercised exactly like the
	// editor wrapper calls it).
	static bool LoadShippedSource(FProfessionParsedSource& OutSource, FString& OutError)
	{
		const FString Dir = FPaths::ProjectDir() / TEXT("Data/Professions");
		if (!LoadProfessionSourceFromDirectory(Dir, OutSource))
		{
			OutError = OutSource.Problems.Num() > 0
				? FString::Join(OutSource.Problems, TEXT("\n"))
				: TEXT("unknown load failure");
			return false;
		}
		return true;
	}

	// The fixture's level rows, generated from the frozen curve module: the
	// prototype rows are value-identical to FExperienceCurve by construction
	// (the parser's equivalence gate re-derives the same numbers) and the
	// fighter rows add the MP/SP columns.
	static FString MakeCurveRowsText()
	{
		FString Rows;
		for (int32 Level = 1; Level <= 10; ++Level)
		{
			Rows += FString::Printf(TEXT(
				"{\"curve_id\": \"prototype_curve\", \"level\": %d, \"xp_to_next\": %d, \"base_max_hp\": %d, "
				"\"base_attack\": %d, \"base_defense\": %d, \"base_max_mp\": 0, \"base_mp_regen\": 0, \"skill_points\": 0},"),
				Level, FExperienceCurve::GetNextLevelXP(Level), FExperienceCurve::GetMaxHPForLevel(Level),
				FExperienceCurve::GetAttackForLevel(Level), FExperienceCurve::GetDefenseForLevel(Level));
		}
		for (int32 Level = 1; Level <= 10; ++Level)
		{
			const int32 MaxMP = 100 + 15 * (Level - 1);
			const int32 Regen = (Level >= 9) ? 4 : (Level >= 5 ? 3 : 2);
			const int32 SP = (Level == 10) ? 0 : 3;
			Rows += FString::Printf(TEXT(
				"{\"curve_id\": \"fighter_curve\", \"level\": %d, \"xp_to_next\": %d, \"base_max_hp\": %d, "
				"\"base_attack\": %d, \"base_defense\": %d, \"base_max_mp\": %d, \"base_mp_regen\": %d, \"skill_points\": %d}%s"),
				Level, FExperienceCurve::GetNextLevelXP(Level), FExperienceCurve::GetMaxHPForLevel(Level),
				FExperienceCurve::GetAttackForLevel(Level), FExperienceCurve::GetDefenseForLevel(Level),
				MaxMP, Regen, SP, Level == 10 ? TEXT("") : TEXT(","));
		}
		return Rows;
	}

	// A minimal VALID fixture: prototype (legacy equivalent) + fighter with one
	// wired skill and one pending skill, one default loadout row.
	static TMap<FString, FString> MakeValidFixture()
	{
		TMap<FString, FString> Tables;
		Tables.Add(TEXT("manifest"), TEXT(R"({
			"schema_version": 1,
			"required_tables": ["professions", "level_curves", "skills", "skill_levels", "loadouts"]
		})"));
		Tables.Add(TEXT("professions"), TEXT(R"({
			"schema_version": 1, "table": "professions", "rows": [
				{"profession_id": "prototype", "display_name": "proto", "curve_id": "prototype_curve",
				 "mesh_path": "", "skeleton_path": "", "locomotion_set_id": "proto_loc",
				 "starting_skill_ranks": {}, "allowed_weapon_modes": ["melee"],
				 "presentation_id": "", "asset_placeholder": true},
				{"profession_id": "fighter", "display_name": "fighter", "curve_id": "fighter_curve",
				 "mesh_path": "", "skeleton_path": "", "locomotion_set_id": "fighter_loc",
				 "starting_skill_ranks": {"launcher_z": 1}, "allowed_weapon_modes": ["melee"],
				 "presentation_id": "", "asset_placeholder": true}
			]
		})"));
		Tables.Add(TEXT("level_curves"), FString::Printf(TEXT(R"({
			"schema_version": 1, "table": "level_curves", "rows": [%s]
		})"), *MakeCurveRowsText()));
		Tables.Add(TEXT("skills"), TEXT(R"({
			"schema_version": 1, "table": "skills", "rows": [
				{"skill_id": "launcher_z", "profession_id": "fighter", "max_rank": 1,
				 "slot_eligibility": "launcher", "effect_policy": "SingleMeleeHit",
				 "attack_id": "launcher", "attack_pending": false,
				 "effect_parameters": {}, "icon_path": "", "montage_path": "", "vfx_path": "",
				 "sound_path": "", "placeholder": true},
				{"skill_id": "singlekick_w", "profession_id": "fighter", "max_rank": 1,
				 "slot_eligibility": "hotbar", "effect_policy": "SingleMeleeHit",
				 "attack_id": "singlekick", "attack_pending": true,
				 "effect_parameters": {}, "icon_path": "", "montage_path": "", "vfx_path": "",
				 "sound_path": "", "placeholder": true}
			]
		})"));
		Tables.Add(TEXT("skill_levels"), TEXT(R"({
			"schema_version": 1, "table": "skill_levels", "rows": [
				{"skill_id": "launcher_z", "rank": 1, "required_character_level": 1, "sp_cost": 0, "cooldown_s": 2.0, "mp_cost": 10, "damage_coefficient": 1.0, "effect_parameters": {}},
				{"skill_id": "singlekick_w", "rank": 1, "required_character_level": 1, "sp_cost": 2, "cooldown_s": 1.5, "mp_cost": 8, "damage_coefficient": 0.8, "effect_parameters": {}}
			]
		})"));
		Tables.Add(TEXT("loadouts"), TEXT(R"({
			"schema_version": 1, "table": "loadouts", "rows": [
				{"profession_id": "fighter", "slot_index": 2, "skill_id": "singlekick_w"}
			]
		})"));
		return Tables;
	}

	static const TSet<FName> KnownAttacks = { FName(TEXT("launcher")) };

	// Parses the fixture; returns false after reporting the problems.
	static bool ParseFixture(FAutomationTestBase& Test, FProfessionParsedSource& OutSource)
	{
		if (!ParseProfessionSourceFromText(MakeValidFixture(), KnownAttacks, OutSource))
		{
			Test.AddError(FString::Printf(TEXT("valid fixture refused: %s"),
				*FString::Join(OutSource.Problems, TEXT(" | "))));
			return false;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_001FixtureParses,
	"UEMMO.Tasks.M7_001.FixtureParses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_001FixtureParses::RunTest(const FString& Parameters)
{
	using namespace UE::UEMMO::Tasks::M7_001;
	FProfessionParsedSource Source;
	if (!ParseFixture(*this, Source))
	{
		return false;
	}
	TestTrue(TEXT("fixture has 2 professions"), Source.Tables.Professions.Num() == 2);
	TestTrue(TEXT("fixture revision is a non-empty digest"), Source.ConfigRevision.Len() >= 16);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_001ShippedTablesLoad,
	"UEMMO.Tasks.M7_001.ShippedTablesLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_001ShippedTablesLoad::RunTest(const FString& Parameters)
{
	using namespace UE::UEMMO::Tasks::M7_001;
	FProfessionParsedSource Source;
	FString Error;
	if (!LoadShippedSource(Source, Error))
	{
		AddError(FString::Printf(TEXT("shipped Data/Professions refused: %s"), *Error));
		return false;
	}
	TestTrue(TEXT("prototype profession present"), Source.Tables.Professions.Contains(TEXT("prototype")));
	TestTrue(TEXT("fighter profession present"), Source.Tables.Professions.Contains(TEXT("fighter")));
	const TArray<FProfessionLevelCurveRow>* PrototypeCurve =
		Source.Tables.LevelCurves.Find(TEXT("prototype_curve"));
	if (!PrototypeCurve)
	{
		AddError(TEXT("prototype_curve missing"));
		return false;
	}
	// Prototype equivalence, value by value, against the frozen curve module.
	for (const FProfessionLevelCurveRow& Row : *PrototypeCurve)
	{
		TestEqual(FString::Printf(TEXT("prototype xp@%d"), Row.Level),
			Row.XPToNext, FExperienceCurve::GetNextLevelXP(Row.Level));
		TestEqual(FString::Printf(TEXT("prototype hp@%d"), Row.Level),
			Row.BaseMaxHP, FExperienceCurve::GetMaxHPForLevel(Row.Level));
		TestEqual(FString::Printf(TEXT("prototype atk@%d"), Row.Level),
			Row.BaseAttack, FExperienceCurve::GetAttackForLevel(Row.Level));
		TestEqual(FString::Printf(TEXT("prototype def@%d"), Row.Level),
			Row.BaseDefense, FExperienceCurve::GetDefenseForLevel(Row.Level));
		TestEqual(FString::Printf(TEXT("prototype mp@%d"), Row.Level), Row.BaseMaxMP, 0);
		TestEqual(FString::Printf(TEXT("prototype sp@%d"), Row.Level), Row.SkillPoints, 0);
	}
	// fighter carries the new MP/SP columns and the wired launcher attack.
	const FSkillRow* Launcher = Source.Tables.Skills.Find(TEXT("launcher_z"));
	if (Launcher)
	{
		TestFalse(TEXT("launcher_z is wired (not pending)"), Launcher->AttackPending);
		TestEqual(TEXT("launcher_z drives the existing launcher attack"),
			Launcher->AttackId, FName(TEXT("launcher")));
		TestTrue(TEXT("launcher_z granted at creation"),
			Source.Tables.Professions.Contains(TEXT("fighter")) &&
			Source.Tables.Professions[TEXT("fighter")].StartingSkillRanks.Contains(TEXT("launcher_z")));
	}
	else
	{
		AddError(TEXT("launcher_z skill missing"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_001RevisionDeterministic,
	"UEMMO.Tasks.M7_001.RevisionDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_001RevisionDeterministic::RunTest(const FString& Parameters)
{
	using namespace UE::UEMMO::Tasks::M7_001;
	FProfessionParsedSource First;
	if (!ParseFixture(*this, First))
	{
		return false;
	}
	// Re-parse the identical fixture: the digest must be stable across runs.
	FProfessionParsedSource Second;
	if (!ParseFixture(*this, Second))
	{
		return false;
	}
	TestEqual(TEXT("revision is stable across runs"), Second.ConfigRevision, First.ConfigRevision);
	// A content change must move the digest.
	FProfessionTableSet Mutated = First.Tables;
	FProfessionRow* Fighter = Mutated.Professions.Find(TEXT("fighter"));
	Fighter->DisplayName = TEXT("renamed");
	TestNotEqual(TEXT("content change moves the digest"),
		ComputeProfessionConfigRevision(Mutated), First.ConfigRevision);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_001NegativeCases,
	"UEMMO.Tasks.M7_001.NegativeCases",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_001NegativeCases::RunTest(const FString& Parameters)
{
	using namespace UE::UEMMO::Tasks::M7_001;
	// Runs the fixture text through the parser and asserts that the problems
	// mention the expected fragment (a clean result is a test failure). All
	// mutations inject on SINGLE LINES so the source file's line endings can
	// never desync an anchor.
	auto ExpectRefused = [&](const TCHAR* CaseName, const TCHAR* Old, const TCHAR* New,
		const TCHAR* ExpectedFragment)
	{
		TMap<FString, FString> Tables = MakeValidFixture();
		bool bAnyChanged = false;
		for (TPair<FString, FString>& Pair : Tables)
		{
			const FString Before = Pair.Value;
			Pair.Value = Pair.Value.Replace(Old, New);
			bAnyChanged = bAnyChanged || Pair.Value != Before;
		}
		if (!bAnyChanged)
		{
			AddError(FString::Printf(TEXT("%s: mutation anchor matched nothing (test bug)"), CaseName));
			return;
		}
		FProfessionParsedSource Result;
		ParseProfessionSourceFromText(Tables, KnownAttacks, Result);
		if (Result.IsValid())
		{
			AddError(FString::Printf(TEXT("%s: mutation was accepted"), CaseName));
			return;
		}
		TestTrue(FString::Printf(TEXT("%s: error names the cause"), CaseName),
			Result.Problems.ContainsByPredicate([ExpectedFragment](const FString& P)
			{
				return P.Contains(ExpectedFragment);
			}));
	};

	ExpectRefused(TEXT("duplicate profession id"),
		TEXT("{\"profession_id\": \"fighter\", \"display_name\": \"fighter\", \"curve_id\": \"fighter_curve\","),
		TEXT("{\"profession_id\": \"fighter\", \"display_name\": \"fighter\", \"curve_id\": \"fighter_curve\", \"mesh_path\": \"\", \"skeleton_path\": \"\", \"locomotion_set_id\": \"l\", \"starting_skill_ranks\": {}, \"allowed_weapon_modes\": [\"melee\"], \"presentation_id\": \"\", \"asset_placeholder\": true}, {\"profession_id\": \"fighter\", \"display_name\": \"fighter\", \"curve_id\": \"fighter_curve\","),
		TEXT("duplicate profession_id"));

	ExpectRefused(TEXT("reserved effect policy"),
		TEXT("SingleMeleeHit"), TEXT("Grab"), TEXT("whitelist"));

	ExpectRefused(TEXT("non-pending unknown attack"),
		TEXT("\"attack_id\": \"singlekick\", \"attack_pending\": true"),
		TEXT("\"attack_id\": \"singlekick\", \"attack_pending\": false"),
		TEXT("combat-attacks.json"));

	ExpectRefused(TEXT("empty allowed_weapon_modes"),
		TEXT("\"allowed_weapon_modes\": [\"melee\"]"), TEXT("\"allowed_weapon_modes\": []"),
		TEXT("allowed_weapon_modes"));

	ExpectRefused(TEXT("missing prototype profession"),
		TEXT("\"profession_id\": \"prototype\""), TEXT("\"profession_id\": \"proto_legacy\""),
		TEXT("prototype"));

	ExpectRefused(TEXT("xp_to_next above max level"),
		TEXT("\"level\": 10, \"xp_to_next\": 0, \"base_max_hp\": 190"),
		TEXT("\"level\": 10, \"xp_to_next\": 1000, \"base_max_hp\": 190"),
		TEXT("max level"));

	ExpectRefused(TEXT("non-continuous curve"),
		TEXT("\"curve_id\": \"prototype_curve\", \"level\": 2, \"xp_to_next\": 200"),
		TEXT("\"curve_id\": \"prototype_curve\", \"level\": 3, \"xp_to_next\": 200"),
		TEXT("continuous"));

	ExpectRefused(TEXT("zero cooldown"),
		TEXT("\"cooldown_s\": 2.0"), TEXT("\"cooldown_s\": 0.0"), TEXT("cooldown_s"));

	ExpectRefused(TEXT("duplicate loadout slot"),
		TEXT("{\"profession_id\": \"fighter\", \"slot_index\": 2, \"skill_id\": \"singlekick_w\"}"),
		TEXT("{\"profession_id\": \"fighter\", \"slot_index\": 2, \"skill_id\": \"singlekick_w\"}, {\"profession_id\": \"fighter\", \"slot_index\": 2, \"skill_id\": \"singlekick_w\"}"),
		TEXT("duplicate slot"));

	ExpectRefused(TEXT("cross-profession starting skill"),
		TEXT("\"starting_skill_ranks\": {}"),
		TEXT("\"starting_skill_ranks\": {\"launcher_z\": 1}"),
		TEXT("another profession"));

	ExpectRefused(TEXT("pending launcher-eligible skill"),
		TEXT("\"attack_id\": \"launcher\", \"attack_pending\": false"),
		TEXT("\"attack_id\": \"launcher\", \"attack_pending\": true"),
		TEXT("launcher"));

	ExpectRefused(TEXT("rank rows not covering max_rank"),
		TEXT("\"skill_id\": \"launcher_z\", \"profession_id\": \"fighter\", \"max_rank\": 1"),
		TEXT("\"skill_id\": \"launcher_z\", \"profession_id\": \"fighter\", \"max_rank\": 2"),
		TEXT("1..max_rank"));

	// Stray unregistered table (the manifest/text registry refuses it): the
	// single-line mutation above cannot add a table, so this case appends one
	// at the set level.
	{
		TMap<FString, FString> Tables = MakeValidFixture();
		Tables.Add(TEXT("items"), TEXT(R"({"schema_version": 1, "table": "items", "rows": []})"));
		FProfessionParsedSource Result;
		ParseProfessionSourceFromText(Tables, KnownAttacks, Result);
		if (Result.IsValid())
		{
			AddError(TEXT("stray unregistered table was accepted"));
		}
		else
		{
			TestTrue(TEXT("stray table named in the error"),
				Result.Problems.ContainsByPredicate([](const FString& P) { return P.Contains(TEXT("manifest")); }));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_001CatalogPublishGate,
	"UEMMO.Tasks.M7_001.CatalogPublishGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_001CatalogPublishGate::RunTest(const FString& Parameters)
{
	using namespace UE::UEMMO::Tasks::M7_001;
	FProfessionParsedSource Source;
	if (!ParseFixture(*this, Source))
	{
		return false;
	}
	// A clean candidate publishes.
	FProfessionCatalog Catalog;
	TArray<FString> Problems;
	if (!Catalog.Build(Source.Tables, Problems))
	{
		AddError(FString::Printf(TEXT("clean candidate refused: %s"), *FString::Join(Problems, TEXT(" | "))));
		return false;
	}
	TestTrue(TEXT("catalog published"), Catalog.IsPublished());
	// Typed lookups on the published snapshot.
	const FProfessionRow* Fighter = Catalog.FindProfession(TEXT("fighter"));
	if (!Fighter)
	{
		AddError(TEXT("fighter lookup failed"));
		return false;
	}
	int32 HP = 0, Attack = 0, Defense = 0, MaxMP = 0;
	TestTrue(TEXT("base stats resolve"), Catalog.GetBaseStats(TEXT("fighter"), 2, HP, Attack, Defense, MaxMP));
	TestEqual(TEXT("fighter mp@2"), MaxMP, 115);
	// Out-of-range levels clamp into the curve.
	TestTrue(TEXT("level 99 clamps"), Catalog.GetBaseStats(TEXT("fighter"), 99, HP, Attack, Defense, MaxMP));
	TestEqual(TEXT("clamped to the curve max"), HP, 190);

	// A broken candidate must NOT replace the published snapshot.
	FProfessionTableSet Broken = Source.Tables;
	Broken.Professions.Remove(TEXT("prototype"));
	TArray<FString> BrokenProblems;
	TestFalse(TEXT("broken candidate refused"), Catalog.Build(Broken, BrokenProblems));
	// The refused build must leave the published snapshot UNCHANGED: the
	// prototype row the broken candidate removed is still published here.
	TestTrue(TEXT("snapshot survives a refused build"),
		Catalog.FindProfession(TEXT("fighter")) != nullptr &&
		Catalog.FindProfession(TEXT("prototype")) != nullptr);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
