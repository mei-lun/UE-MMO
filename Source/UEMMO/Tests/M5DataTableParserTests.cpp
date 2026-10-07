// M5-005: structured source-table parsing and field validation for the
// Data/CombatSystem JSON tables (M5 interface contract section 1, owner 005
// together with 006). Pins the unified JSON -> struct parser in
// Combat/Data/CombatDataTableParser.h:
// - strict per-row parsing of all seven A-segment tables (damage_profiles,
//   attack_reactions, target_reactions, presentations, weapons, ammo_types,
//   projectiles): unknown fields, missing fields, mistyped fields, duplicate
//   JSON keys, non-finite numbers and bad ids are refused with errors that
//   name the field;
// - the directory loader LoadCombatDataDirectory: required tables, rejection
//   of unknown table file names (no silent ignoring of typos), duplicate ids
//   inside one table and the one-presentation-per-reaction mapping rule;
// - the cross-table reference check ValidateCombatDataReferences (mechanism;
//   wiring it into a release gate is M5-006's contract).
//
// Positive loading runs against the shipped Data/CombatSystem tables; the
// negative cases build temporary directories with intentionally broken
// content. Pure checks only: this file reads JSON text and plain structs; it
// never touches UE assets, worlds or wall clocks.
//
// Parsing note (same lesson as M5-003/M5-004): the engine Json module import
// library is not on the UEMMO link line, so the hand-rolled parser lives in
// the parser translation unit itself and is reused from here through the
// header (third duplication avoided by keeping it in one place).

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"

#include "../Combat/Data/CombatDataTableParser.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_005
{
	// ------------------------------------------------------------------
	// Temporary directory helpers for the negative loader cases
	// ------------------------------------------------------------------

	static FString BuildTempDataDir(const TCHAR* CaseName)
	{
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("Temp") / TEXT("M5_005") / CaseName;
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		PlatformFile.DeleteDirectoryRecursively(*Dir);
		PlatformFile.CreateDirectoryTree(*Dir);
		return Dir;
	}

	static void RemoveTempDataDir(const FString& Dir)
	{
		FPlatformFileManager::Get().GetPlatformFile().DeleteDirectoryRecursively(*Dir);
	}

	static bool WriteTableFile(const FString& Dir, const TCHAR* FileName, const FString& Content)
	{
		return FFileHelper::SaveStringToFile(Content, *(Dir / FileName), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	/**
	 * Writes a minimal but fully reference-consistent A-segment table set:
	 * the weapon and the projectile both reference the light_01 damage
	 * profile, so ValidateCombatDataReferences must come back clean.
	 */
	static bool WriteMinimalValidSet(const FString& Dir)
	{
		bool bOk = WriteTableFile(Dir, TEXT("damage_profiles.json"), TEXT(
			"{\n"
			"  \"schema_version\": 1,\n"
			"  \"table\": \"damage_profiles\",\n"
			"  \"rows\": [\n"
			"    {\"damage_profile_id\": \"light_01\", \"base_damage\": 10, \"attack_coefficient\": 1.0, \"hit_stun_s\": 0.22, \"knockback_cm_s\": 90, \"launch_cm_s\": 0, \"hit_stop_s\": 0.04}\n"
			"  ]\n"
			"}\n"));
		bOk = bOk && WriteTableFile(Dir, TEXT("attack_reactions.json"), TEXT(
			"{\n"
			"  \"schema_version\": 1,\n"
			"  \"table\": \"attack_reactions\",\n"
			"  \"rows\": [\n"
			"    {\"reaction_id\": \"light_01\", \"control_penetration\": \"none\"}\n"
			"  ]\n"
			"}\n"));
		bOk = bOk && WriteTableFile(Dir, TEXT("target_reactions.json"), TEXT(
			"{\n"
			"  \"schema_version\": 1,\n"
			"  \"table\": \"target_reactions\",\n"
			"  \"rows\": [\n"
			"    {\"policy_id\": \"normal\", \"allow_stagger\": true, \"allow_launch\": true, \"allow_knockdown\": true, \"max_launches_per_air_cycle\": 2, \"launch_z_scales\": [1.0, 0.7], \"max_air_time_s\": 2.5, \"poise_max\": 0, \"poise_regen_s\": 0, \"knockdown_s\": 0.45, \"recovering_s\": 0.25, \"immune_damage\": false, \"immune_control\": false, \"death_resistant\": false}\n"
			"  ]\n"
			"}\n"));
		bOk = bOk && WriteTableFile(Dir, TEXT("presentations.json"), TEXT(
			"{\n"
			"  \"schema_version\": 1,\n"
			"  \"table\": \"presentations\",\n"
			"  \"rows\": [\n"
			"    {\"presentation_id\": \"hit_light_01\", \"reaction_id\": \"light_01\", \"montage_path\": \"\", \"sound_path\": \"\", \"effect_path\": \"\", \"placeholder\": true}\n"
			"  ]\n"
			"}\n"));
		bOk = bOk && WriteTableFile(Dir, TEXT("weapons.json"), TEXT(
			"{\n"
			"  \"schema_version\": 1,\n"
			"  \"table\": \"weapons\",\n"
			"  \"rows\": [\n"
			"    {\"weapon_id\": \"weapon_test_sword\", \"mode\": \"melee\", \"damage_profile_id\": \"light_01\", \"ammo_id\": \"\", \"magazine_size\": 0, \"fire_rate_rpm\": 0, \"burst_count\": 1, \"pellet_count\": 1, \"spread_degrees_deg\": 0, \"range_cm\": 0, \"projectile_id\": \"\", \"melee_attack_ids\": [\"light_01\", \"light_02\", \"launcher\", \"aerial_01\"]}\n"
			"  ]\n"
			"}\n"));
		bOk = bOk && WriteTableFile(Dir, TEXT("ammo_types.json"), TEXT(
			"{\n"
			"  \"schema_version\": 1,\n"
			"  \"table\": \"ammo_types\",\n"
			"  \"rows\": [\n"
			"    {\"ammo_id\": \"ammo_cell\", \"max_reserve\": 120, \"magazine_size\": 12}\n"
			"  ]\n"
			"}\n"));
		bOk = bOk && WriteTableFile(Dir, TEXT("projectiles.json"), TEXT(
			"{\n"
			"  \"schema_version\": 1,\n"
			"  \"table\": \"projectiles\",\n"
			"  \"rows\": [\n"
			"    {\"projectile_id\": \"bullet_test\", \"motion\": \"straight\", \"speed_cm_s\": 6000, \"lifetime_s\": 2.0, \"damage_profile_id\": \"light_01\", \"pierce_count\": 0, \"explosion_radius_cm\": 0, \"explosion_damage_profile_id\": \"\", \"homing_turn_rate_deg_s\": 0}\n"
			"  ]\n"
			"}\n"));
		return bOk;
	}

	// Pinned fixture row copied from Data/CombatSystem/weapons.json (the
	// legacy training sword) used as the canonical positive weapon row.
	static const TCHAR* const TrainingSwordRowJson = TEXT(
		"{\"weapon_id\": \"weapon_training_sword\", \"mode\": \"melee\", \"damage_profile_id\": \"physical_10\", \"ammo_id\": \"\", \"magazine_size\": 0, \"fire_rate_rpm\": 0, \"burst_count\": 1, \"pellet_count\": 1, \"spread_degrees_deg\": 0, \"range_cm\": 0, \"projectile_id\": \"\", \"melee_attack_ids\": [\"light_01\", \"light_02\", \"launcher\", \"aerial_01\"]}");

	static const TCHAR* const BulletLinearRowJson = TEXT(
		"{\"projectile_id\": \"bullet_linear\", \"motion\": \"straight\", \"speed_cm_s\": 6000, \"lifetime_s\": 2.0, \"damage_profile_id\": \"physical_10\", \"pierce_count\": 0, \"explosion_radius_cm\": 0, \"explosion_damage_profile_id\": \"\", \"homing_turn_rate_deg_s\": 0}");

	static const TCHAR* const NormalTargetRowJson = TEXT(
		"{\"policy_id\": \"normal\", \"allow_stagger\": true, \"allow_launch\": true, \"allow_knockdown\": true, \"max_launches_per_air_cycle\": 2, \"launch_z_scales\": [1.0, 0.7], \"max_air_time_s\": 2.5, \"poise_max\": 0, \"poise_regen_s\": 0, \"knockdown_s\": 0.45, \"recovering_s\": 0.25, \"immune_damage\": false, \"immune_control\": false, \"death_resistant\": false}");

	static const TCHAR* const PlaceholderPresentationRowJson = TEXT(
		"{\"presentation_id\": \"hit_light_01\", \"reaction_id\": \"light_01\", \"montage_path\": \"\", \"sound_path\": \"\", \"effect_path\": \"\", \"placeholder\": true}");
}

using namespace UE::UEMMO::Tasks::M5_005;

// ---------------------------------------------------------------------------
// JsonParserBasics
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_005JsonParserBasics,
	"UEMMO.Tasks.M5_005.JsonParserBasics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_005JsonParserBasics::RunTest(const FString& Parameters)
{
	// Bad JSON is refused with an offset-bearing error.
	TSharedPtr<FCombatJsonValue> Root;
	FString Error;
	TestFalse(TEXT("broken JSON '{' is refused"), FCombatJsonParser::Parse(TEXT("{"), Root, Error));
	TestFalse(TEXT("the broken JSON error is not empty"), Error.IsEmpty());

	TestFalse(TEXT("trailing garbage after the value is refused"),
		FCombatJsonParser::Parse(TEXT("{} trailing"), Root, Error));
	TestTrue(TEXT("the trailing-garbage error names the offset"), Error.Contains(TEXT("offset")));

	// The NaN literal is not a JSON number and must be refused outright.
	TestFalse(TEXT("a NaN literal is refused"), FCombatJsonParser::Parse(TEXT("{\"a\": NaN}"), Root, Error));
	TestTrue(TEXT("the NaN error reports an invalid number"), Error.Contains(TEXT("invalid number")));

	// An overflowing number decodes to a non-finite double and is refused.
	TestFalse(TEXT("an overflowing number (1e999) is refused"),
		FCombatJsonParser::Parse(TEXT("{\"a\": 1e999}"), Root, Error));
	TestTrue(TEXT("the overflow error reports a non-finite number"), Error.Contains(TEXT("finite")));

	// Duplicate keys inside one object are refused (M5-005 acceptance: duplicate keys).
	TestFalse(TEXT("a duplicate key inside a row object is refused"),
		FCombatJsonParser::Parse(TEXT("{\"a\": 1, \"a\": 2}"), Root, Error));
	TestTrue(TEXT("the duplicate-key error names the key"), Error.Contains(TEXT("duplicate key")) && Error.Contains(TEXT("a")));

	TestFalse(TEXT("a duplicate key at the document top level is refused"),
		FCombatJsonParser::Parse(TEXT("{\"rows\": [], \"rows\": []}"), Root, Error));
	TestTrue(TEXT("the top-level duplicate-key error names the key"), Error.Contains(TEXT("rows")));

	// Well-formed basics keep working: object, array, string escapes, numbers, booleans, null.
	TestTrue(TEXT("a well-formed document parses"),
		FCombatJsonParser::Parse(TEXT("{\"s\": \"a\\nb\", \"n\": -1.5e2, \"b\": true, \"z\": null, \"arr\": [1, 2, 3]}"), Root, Error));
	if (TestTrue(TEXT("the well-formed document produced a root"), Root.IsValid()))
	{
		const TSharedPtr<FCombatJsonValue>* Text = Root->Object.Find(TEXT("s"));
		if (TestNotNull(TEXT("field s exists"), Text) && (*Text)->Kind == FCombatJsonValue::EKind::String)
		{
			TestEqual(TEXT("the \\n escape decoded to one newline"), (*Text)->String, FString(TEXT("a\nb")));
		}
		const TSharedPtr<FCombatJsonValue>* Number = Root->Object.Find(TEXT("n"));
		if (TestNotNull(TEXT("field n exists"), Number) && (*Number)->Kind == FCombatJsonValue::EKind::Number)
		{
			TestEqual(TEXT("the exponent number decoded"), (*Number)->Number, -150.0);
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// DamageProfileRowParsing
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_005DamageProfileRowParsing,
	"UEMMO.Tasks.M5_005.DamageProfileRowParsing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_005DamageProfileRowParsing::RunTest(const FString& Parameters)
{
	const TCHAR* ValidRow = TEXT(
		"{\"damage_profile_id\": \"light_01\", \"base_damage\": 10, \"attack_coefficient\": 1.0, \"hit_stun_s\": 0.22, \"knockback_cm_s\": 90, \"launch_cm_s\": 0, \"hit_stop_s\": 0.04}");

	FDamageProfile Profile;
	FString Error;
	TestTrue(TEXT("a valid damage profile row parses"), ParseDamageProfile(ValidRow, Profile, Error));
	if (TestTrue(TEXT("the parsed profile holds values"), Error.IsEmpty()))
	{
		TestEqual(TEXT("the id is parsed"), Profile.DamageProfileId, FName(TEXT("light_01")));
		TestEqual(TEXT("BaseDamage is parsed"), Profile.BaseDamage, 10.0f);
		TestEqual(TEXT("AttackCoefficient is parsed"), Profile.AttackCoefficient, 1.0f);
		TestEqual(TEXT("HitStunSeconds is parsed"), Profile.HitStunSeconds, 0.22f);
		TestEqual(TEXT("KnockbackCmPerSecond is parsed"), Profile.KnockbackCmPerSecond, 90.0f);
		TestEqual(TEXT("LaunchCmPerSecond is parsed"), Profile.LaunchCmPerSecond, 0.0f);
		TestEqual(TEXT("HitStopSeconds is parsed"), Profile.HitStopSeconds, 0.04f);
	}

	// Missing fields are refused and name the field.
	struct FMissingFieldCase
	{
		const TCHAR* RowJson;
		const TCHAR* FieldName;
	};
	const FMissingFieldCase MissingCases[] = {
		{TEXT("{\"base_damage\": 10, \"attack_coefficient\": 1.0, \"hit_stun_s\": 0.22, \"knockback_cm_s\": 90, \"launch_cm_s\": 0, \"hit_stop_s\": 0.04}"), TEXT("damage_profile_id")},
		{TEXT("{\"damage_profile_id\": \"light_01\", \"attack_coefficient\": 1.0, \"hit_stun_s\": 0.22, \"knockback_cm_s\": 90, \"launch_cm_s\": 0, \"hit_stop_s\": 0.04}"), TEXT("base_damage")},
		{TEXT("{\"damage_profile_id\": \"light_01\", \"base_damage\": 10, \"hit_stun_s\": 0.22, \"knockback_cm_s\": 90, \"launch_cm_s\": 0, \"hit_stop_s\": 0.04}"), TEXT("attack_coefficient")},
		{TEXT("{\"damage_profile_id\": \"light_01\", \"base_damage\": 10, \"attack_coefficient\": 1.0, \"knockback_cm_s\": 90, \"launch_cm_s\": 0, \"hit_stop_s\": 0.04}"), TEXT("hit_stun_s")},
		{TEXT("{\"damage_profile_id\": \"light_01\", \"base_damage\": 10, \"attack_coefficient\": 1.0, \"hit_stun_s\": 0.22, \"launch_cm_s\": 0, \"hit_stop_s\": 0.04}"), TEXT("knockback_cm_s")},
		{TEXT("{\"damage_profile_id\": \"light_01\", \"base_damage\": 10, \"attack_coefficient\": 1.0, \"hit_stun_s\": 0.22, \"knockback_cm_s\": 90, \"hit_stop_s\": 0.04}"), TEXT("launch_cm_s")},
		{TEXT("{\"damage_profile_id\": \"light_01\", \"base_damage\": 10, \"attack_coefficient\": 1.0, \"hit_stun_s\": 0.22, \"knockback_cm_s\": 90, \"launch_cm_s\": 0}"), TEXT("hit_stop_s")}
	};
	for (const FMissingFieldCase& Case : MissingCases)
	{
		FDamageProfile Missing;
		TestFalse(FString::Printf(TEXT("a row without '%s' is refused"), Case.FieldName),
			ParseDamageProfile(Case.RowJson, Missing, Error));
		TestTrue(FString::Printf(TEXT("the missing '%s' error names the field"), Case.FieldName),
			Error.Contains(Case.FieldName));
	}

	// Mistyped fields are refused and name the field.
	FDamageProfile Mistyped;
	TestFalse(TEXT("a string base_damage is refused"),
		ParseDamageProfile(TEXT("{\"damage_profile_id\": \"light_01\", \"base_damage\": \"10\", \"attack_coefficient\": 1.0, \"hit_stun_s\": 0.22, \"knockback_cm_s\": 90, \"launch_cm_s\": 0, \"hit_stop_s\": 0.04}"), Mistyped, Error));
	TestTrue(TEXT("the mistyped base_damage error names the field"), Error.Contains(TEXT("base_damage")));

	TestFalse(TEXT("a boolean hit_stun_s is refused"),
		ParseDamageProfile(TEXT("{\"damage_profile_id\": \"light_01\", \"base_damage\": 10, \"attack_coefficient\": true, \"hit_stun_s\": false, \"knockback_cm_s\": 90, \"launch_cm_s\": 0, \"hit_stop_s\": 0.04}"), Mistyped, Error));

	// Unknown fields are strictly refused and name the field.
	FDamageProfile UnknownField;
	TestFalse(TEXT("an unknown field is refused"),
		ParseDamageProfile(TEXT("{\"damage_profile_id\": \"light_01\", \"base_damage\": 10, \"attack_coefficient\": 1.0, \"hit_stun_s\": 0.22, \"knockback_cm_s\": 90, \"launch_cm_s\": 0, \"hit_stop_s\": 0.04, \"crit_multiplier\": 2.0}"), UnknownField, Error));
	TestTrue(TEXT("the unknown-field error names 'crit_multiplier'"), Error.Contains(TEXT("crit_multiplier")));
	TestTrue(TEXT("the unknown-field error says unknown"), Error.Contains(TEXT("unknown field")));

	// A duplicate key inside the row is refused.
	FDamageProfile DuplicateKey;
	TestFalse(TEXT("a duplicate key inside the row is refused"),
		ParseDamageProfile(TEXT("{\"damage_profile_id\": \"light_01\", \"damage_profile_id\": \"light_02\", \"base_damage\": 10, \"attack_coefficient\": 1.0, \"hit_stun_s\": 0.22, \"knockback_cm_s\": 90, \"launch_cm_s\": 0, \"hit_stop_s\": 0.04}"), DuplicateKey, Error));
	TestTrue(TEXT("the duplicate-key error names 'damage_profile_id'"), Error.Contains(TEXT("duplicate key")));

	// Non-finite numbers never enter the struct.
	FDamageProfile Overflow;
	TestFalse(TEXT("an overflowing base_damage (1e999) is refused"),
		ParseDamageProfile(TEXT("{\"damage_profile_id\": \"light_01\", \"base_damage\": 1e999, \"attack_coefficient\": 1.0, \"hit_stun_s\": 0.22, \"knockback_cm_s\": 90, \"launch_cm_s\": 0, \"hit_stop_s\": 0.04}"), Overflow, Error));
	TestTrue(TEXT("the overflow error names base_damage"), Error.Contains(TEXT("base_damage")));

	// Illegal values are refused through the 003 validator.
	FDamageProfile ZeroDamage;
	TestFalse(TEXT("a zero base_damage is refused"),
		ParseDamageProfile(TEXT("{\"damage_profile_id\": \"light_01\", \"base_damage\": 0, \"attack_coefficient\": 1.0, \"hit_stun_s\": 0.22, \"knockback_cm_s\": 90, \"launch_cm_s\": 0, \"hit_stop_s\": 0.04}"), ZeroDamage, Error));
	TestTrue(TEXT("the zero-damage error names BaseDamage"), Error.Contains(TEXT("BaseDamage")));

	FDamageProfile BadId;
	TestFalse(TEXT("an uppercase id is refused"),
		ParseDamageProfile(TEXT("{\"damage_profile_id\": \"Light_01\", \"base_damage\": 10, \"attack_coefficient\": 1.0, \"hit_stun_s\": 0.22, \"knockback_cm_s\": 90, \"launch_cm_s\": 0, \"hit_stop_s\": 0.04}"), BadId, Error));
	TestTrue(TEXT("the bad-id error names DamageProfileId"), Error.Contains(TEXT("DamageProfileId")));

	// A failed parse leaves the out struct untouched (no half-filled row).
	TestTrue(TEXT("the failed parse reset the struct"), BadId.DamageProfileId.IsNone());
	return true;
}

// ---------------------------------------------------------------------------
// AttackReactionRowParsing
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_005AttackReactionRowParsing,
	"UEMMO.Tasks.M5_005.AttackReactionRowParsing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_005AttackReactionRowParsing::RunTest(const FString& Parameters)
{
	FAttackReaction Reaction;
	FString Error;
	TestTrue(TEXT("a none penetration row parses"),
		ParseAttackReaction(TEXT("{\"reaction_id\": \"light_01\", \"control_penetration\": \"none\"}"), Reaction, Error));
	if (TestTrue(TEXT("the none row holds values"), Error.IsEmpty()))
	{
		TestEqual(TEXT("the id is parsed"), Reaction.ReactionId, FName(TEXT("light_01")));
		TestTrue(TEXT("the penetration is None"), Reaction.ControlPenetration == EControlPenetration::None);
	}

	TestTrue(TEXT("a bypass_poise row parses"),
		ParseAttackReaction(TEXT("{\"reaction_id\": \"launcher\", \"control_penetration\": \"bypass_poise\"}"), Reaction, Error));
	TestTrue(TEXT("the penetration is BypassPoise"), Reaction.ControlPenetration == EControlPenetration::BypassPoise);

	TestTrue(TEXT("the penetration token is case-insensitive"),
		ParseAttackReaction(TEXT("{\"reaction_id\": \"launcher\", \"control_penetration\": \"BYPASS_POISE\"}"), Reaction, Error));

	Error.Reset();
	TestFalse(TEXT("an unknown penetration token is refused"),
		ParseAttackReaction(TEXT("{\"reaction_id\": \"launcher\", \"control_penetration\": \"poise_smash\"}"), Reaction, Error));
	TestTrue(TEXT("the unknown-token error names control_penetration"), Error.Contains(TEXT("control_penetration")));

	TestFalse(TEXT("a missing control_penetration is refused"),
		ParseAttackReaction(TEXT("{\"reaction_id\": \"launcher\"}"), Reaction, Error));
	TestTrue(TEXT("the missing-token error names control_penetration"), Error.Contains(TEXT("control_penetration")));

	TestFalse(TEXT("a number control_penetration is refused"),
		ParseAttackReaction(TEXT("{\"reaction_id\": \"launcher\", \"control_penetration\": 1}"), Reaction, Error));

	TestFalse(TEXT("an unknown field is refused"),
		ParseAttackReaction(TEXT("{\"reaction_id\": \"launcher\", \"control_penetration\": \"none\", \"poise_damage\": 10}"), Reaction, Error));
	TestTrue(TEXT("the unknown-field error names poise_damage"), Error.Contains(TEXT("poise_damage")));

	TestFalse(TEXT("a bad reaction id is refused"),
		ParseAttackReaction(TEXT("{\"reaction_id\": \"Light-01\", \"control_penetration\": \"none\"}"), Reaction, Error));
	TestTrue(TEXT("the bad-id error names ReactionId"), Error.Contains(TEXT("ReactionId")));
	return true;
}

// ---------------------------------------------------------------------------
// TargetReactionRowParsing
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_005TargetReactionRowParsing,
	"UEMMO.Tasks.M5_005.TargetReactionRowParsing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_005TargetReactionRowParsing::RunTest(const FString& Parameters)
{
	FTargetReaction Policy;
	FString Error;
	TestTrue(TEXT("the legacy normal target row parses"), ParseTargetReaction(NormalTargetRowJson, Policy, Error));
	if (TestTrue(TEXT("the normal row holds values"), Error.IsEmpty()))
	{
		TestEqual(TEXT("the id is parsed"), Policy.PolicyId, FName(TEXT("normal")));
		TestEqual(TEXT("MaxLaunchesPerAirCycle is parsed"), Policy.MaxLaunchesPerAirCycle, 2);
		TestEqual(TEXT("the first launch scale is 1.0"), Policy.LaunchZScales.Num() == 2 ? Policy.LaunchZScales[0] : -1.0f, 1.0f);
		TestEqual(TEXT("the second launch scale is 0.7"), Policy.LaunchZScales.Num() == 2 ? Policy.LaunchZScales[1] : -1.0f, 0.7f);
		TestEqual(TEXT("MaxAirTimeSeconds is parsed"), Policy.MaxAirTimeSeconds, 2.5f);
		TestEqual(TEXT("KnockdownSeconds is parsed"), Policy.KnockdownSeconds, 0.45f);
		TestEqual(TEXT("RecoveringSeconds is parsed"), Policy.RecoveringSeconds, 0.25f);
		TestEqual(TEXT("PoiseMax is parsed"), Policy.PoiseMax, 0.0f);
		TestEqual(TEXT("PoiseRegenSeconds is parsed"), Policy.PoiseRegenSeconds, 0.0f);
		TestFalse(TEXT("no immunity axes are set"), Policy.bImmuneDamage || Policy.bImmuneControl || Policy.bDeathResistant);
	}

	Error.Reset();
	TestFalse(TEXT("a fractional max_launches_per_air_cycle is refused"),
		ParseTargetReaction(TEXT("{\"policy_id\": \"normal\", \"allow_stagger\": true, \"allow_launch\": true, \"allow_knockdown\": true, \"max_launches_per_air_cycle\": 2.5, \"launch_z_scales\": [1.0], \"max_air_time_s\": 2.5, \"poise_max\": 0, \"poise_regen_s\": 0, \"knockdown_s\": 0.45, \"recovering_s\": 0.25, \"immune_damage\": false, \"immune_control\": false, \"death_resistant\": false}"), Policy, Error));
	TestTrue(TEXT("the fractional error names max_launches_per_air_cycle"), Error.Contains(TEXT("max_launches_per_air_cycle")));

	TestFalse(TEXT("a number where a boolean is required is refused"),
		ParseTargetReaction(TEXT("{\"policy_id\": \"normal\", \"allow_stagger\": 1, \"allow_launch\": true, \"allow_knockdown\": true, \"max_launches_per_air_cycle\": 2, \"launch_z_scales\": [1.0], \"max_air_time_s\": 2.5, \"poise_max\": 0, \"poise_regen_s\": 0, \"knockdown_s\": 0.45, \"recovering_s\": 0.25, \"immune_damage\": false, \"immune_control\": false, \"death_resistant\": false}"), Policy, Error));
	TestTrue(TEXT("the mistyped boolean error names allow_stagger"), Error.Contains(TEXT("allow_stagger")));

	TestFalse(TEXT("a missing launch_z_scales is refused"),
		ParseTargetReaction(TEXT("{\"policy_id\": \"normal\", \"allow_stagger\": true, \"allow_launch\": true, \"allow_knockdown\": true, \"max_launches_per_air_cycle\": 2, \"max_air_time_s\": 2.5, \"poise_max\": 0, \"poise_regen_s\": 0, \"knockdown_s\": 0.45, \"recovering_s\": 0.25, \"immune_damage\": false, \"immune_control\": false, \"death_resistant\": false}"), Policy, Error));
	TestTrue(TEXT("the missing-array error names launch_z_scales"), Error.Contains(TEXT("launch_z_scales")));

	TestFalse(TEXT("a non-number inside launch_z_scales is refused"),
		ParseTargetReaction(TEXT("{\"policy_id\": \"normal\", \"allow_stagger\": true, \"allow_launch\": true, \"allow_knockdown\": true, \"max_launches_per_air_cycle\": 2, \"launch_z_scales\": [1.0, \"0.7\"], \"max_air_time_s\": 2.5, \"poise_max\": 0, \"poise_regen_s\": 0, \"knockdown_s\": 0.45, \"recovering_s\": 0.25, \"immune_damage\": false, \"immune_control\": false, \"death_resistant\": false}"), Policy, Error));
	TestTrue(TEXT("the element error names launch_z_scales[1]"), Error.Contains(TEXT("launch_z_scales[1]")));

	// An empty scale array parses structurally but the 003 validator refuses it.
	FTargetReaction EmptyScales;
	TestFalse(TEXT("an empty launch_z_scales is refused by validation"),
		ParseTargetReaction(TEXT("{\"policy_id\": \"normal\", \"allow_stagger\": true, \"allow_launch\": true, \"allow_knockdown\": true, \"max_launches_per_air_cycle\": 2, \"launch_z_scales\": [], \"max_air_time_s\": 2.5, \"poise_max\": 0, \"poise_regen_s\": 0, \"knockdown_s\": 0.45, \"recovering_s\": 0.25, \"immune_damage\": false, \"immune_control\": false, \"death_resistant\": false}"), EmptyScales, Error));
	TestTrue(TEXT("the empty-array error names LaunchZScales"), Error.Contains(TEXT("LaunchZScales")));

	// Negative poise is refused by the 003 validator.
	FTargetReaction NegativePoise;
	TestFalse(TEXT("a negative poise_max is refused"),
		ParseTargetReaction(TEXT("{\"policy_id\": \"normal\", \"allow_stagger\": true, \"allow_launch\": true, \"allow_knockdown\": true, \"max_launches_per_air_cycle\": 2, \"launch_z_scales\": [1.0], \"max_air_time_s\": 2.5, \"poise_max\": -1, \"poise_regen_s\": 0, \"knockdown_s\": 0.45, \"recovering_s\": 0.25, \"immune_damage\": false, \"immune_control\": false, \"death_resistant\": false}"), NegativePoise, Error));
	TestTrue(TEXT("the negative-poise error names PoiseMax"), Error.Contains(TEXT("PoiseMax")));

	// Unknown fields are strictly refused.
	FTargetReaction UnknownField;
	TestFalse(TEXT("an unknown field is refused"),
		ParseTargetReaction(TEXT("{\"policy_id\": \"normal\", \"allow_stagger\": true, \"allow_launch\": true, \"allow_knockdown\": true, \"max_launches\": 2, \"launch_z_scales\": [1.0], \"max_air_time_s\": 2.5, \"poise_max\": 0, \"poise_regen_s\": 0, \"knockdown_s\": 0.45, \"recovering_s\": 0.25, \"immune_damage\": false, \"immune_control\": false, \"death_resistant\": false, \"air_damage_scale\": 1.0}"), UnknownField, Error));
	TestTrue(TEXT("the unknown-field error names air_damage_scale"), Error.Contains(TEXT("air_damage_scale")));
	return true;
}

// ---------------------------------------------------------------------------
// PresentationRowParsing
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_005PresentationRowParsing,
	"UEMMO.Tasks.M5_005.PresentationRowParsing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_005PresentationRowParsing::RunTest(const FString& Parameters)
{
	FCombatPresentation Presentation;
	FString Error;
	TestTrue(TEXT("the placeholder presentation row parses"), ParsePresentation(PlaceholderPresentationRowJson, Presentation, Error));
	if (TestTrue(TEXT("the placeholder row holds values"), Error.IsEmpty()))
	{
		TestEqual(TEXT("the id is parsed"), Presentation.PresentationId, FName(TEXT("hit_light_01")));
		TestEqual(TEXT("the reaction id is parsed"), Presentation.ReactionId, FName(TEXT("light_01")));
		TestTrue(TEXT("the paths stay empty"), Presentation.MontagePath.IsEmpty() && Presentation.SoundPath.IsEmpty() && Presentation.EffectPath.IsEmpty());
		TestTrue(TEXT("the placeholder flag is set"), Presentation.bPlaceholder);
	}

	// A non-placeholder row with no resource path at all is a configuration error.
	FCombatPresentation EmptyNonPlaceholder;
	TestFalse(TEXT("a non-placeholder row without any path is refused"),
		ParsePresentation(TEXT("{\"presentation_id\": \"hit_light_01\", \"reaction_id\": \"light_01\", \"montage_path\": \"\", \"sound_path\": \"\", \"effect_path\": \"\", \"placeholder\": false}"), EmptyNonPlaceholder, Error));
	TestTrue(TEXT("the empty non-placeholder error names placeholder"), Error.Contains(TEXT("placeholder")));

	// A non-placeholder row with one resource path is fine.
	FCombatPresentation RealRow;
	TestTrue(TEXT("a non-placeholder row with a montage path parses"),
		ParsePresentation(TEXT("{\"presentation_id\": \"hit_light_01\", \"reaction_id\": \"light_01\", \"montage_path\": \"/Game/UEMMO/Animation/Montages/MNT_light_01.MNT_light_01\", \"sound_path\": \"\", \"effect_path\": \"\", \"placeholder\": false}"), RealRow, Error));
	if (TestTrue(TEXT("the real row holds its path"), Error.IsEmpty()))
	{
		TestTrue(TEXT("the montage path is parsed"), RealRow.MontagePath.Contains(TEXT("MNT_light_01")));
	}

	Error.Reset();
	TestFalse(TEXT("a missing placeholder flag is refused"),
		ParsePresentation(TEXT("{\"presentation_id\": \"hit_light_01\", \"reaction_id\": \"light_01\", \"montage_path\": \"\", \"sound_path\": \"\", \"effect_path\": \"\"}"), RealRow, Error));
	TestTrue(TEXT("the missing-flag error names placeholder"), Error.Contains(TEXT("placeholder")));

	TestFalse(TEXT("a numeric placeholder flag is refused"),
		ParsePresentation(TEXT("{\"presentation_id\": \"hit_light_01\", \"reaction_id\": \"light_01\", \"montage_path\": \"\", \"sound_path\": \"\", \"effect_path\": \"\", \"placeholder\": 1}"), RealRow, Error));

	TestFalse(TEXT("a missing reaction_id is refused"),
		ParsePresentation(TEXT("{\"presentation_id\": \"hit_light_01\", \"montage_path\": \"\", \"sound_path\": \"\", \"effect_path\": \"\", \"placeholder\": true}"), RealRow, Error));
	TestTrue(TEXT("the missing-reaction error names reaction_id"), Error.Contains(TEXT("reaction_id")));

	TestFalse(TEXT("an unknown field is refused"),
		ParsePresentation(TEXT("{\"presentation_id\": \"hit_light_01\", \"reaction_id\": \"light_01\", \"montage_path\": \"\", \"sound_path\": \"\", \"effect_path\": \"\", \"placeholder\": true, \"particle\": \"x\"}"), RealRow, Error));
	TestTrue(TEXT("the unknown-field error names particle"), Error.Contains(TEXT("particle")));
	return true;
}

// ---------------------------------------------------------------------------
// WeaponRowParsing
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_005WeaponRowParsing,
	"UEMMO.Tasks.M5_005.WeaponRowParsing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_005WeaponRowParsing::RunTest(const FString& Parameters)
{
	FWeaponDefinition Weapon;
	FString Error;
	TestTrue(TEXT("the training sword row parses"), ParseWeaponDefinition(TrainingSwordRowJson, Weapon, Error));
	if (TestTrue(TEXT("the sword row holds values"), Error.IsEmpty()))
	{
		TestEqual(TEXT("the id is parsed"), Weapon.WeaponId, FName(TEXT("weapon_training_sword")));
		TestTrue(TEXT("the mode is Melee"), Weapon.FireMode == EWeaponFireMode::Melee);
		TestEqual(TEXT("the melee attack ids are the legacy four"), Weapon.MeleeAttackIds.Num(), 4);
		TestTrue(TEXT("no ammo and no projectile"), Weapon.AmmoId.IsNone() && Weapon.ProjectileId.IsNone());
	}

	// A hitscan row parses with its pacing and geometry.
	FWeaponDefinition Hitscan;
	TestTrue(TEXT("a hitscan row parses"),
		ParseWeaponDefinition(TEXT("{\"weapon_id\": \"weapon_hitscan_sample\", \"mode\": \"hitscan\", \"damage_profile_id\": \"physical_10\", \"ammo_id\": \"ammo_cell\", \"magazine_size\": 12, \"fire_rate_rpm\": 240, \"burst_count\": 1, \"pellet_count\": 1, \"spread_degrees_deg\": 1.0, \"range_cm\": 5000, \"projectile_id\": \"\", \"melee_attack_ids\": []}"), Hitscan, Error));
	if (TestTrue(TEXT("the hitscan row holds values"), Error.IsEmpty()))
	{
		TestTrue(TEXT("the mode is Hitscan"), Hitscan.FireMode == EWeaponFireMode::Hitscan);
		TestEqual(TEXT("MagazineSize is parsed"), Hitscan.MagazineSize, 12);
		TestEqual(TEXT("FireRateRpm is parsed"), Hitscan.FireRateRpm, 240.0f);
		TestEqual(TEXT("RangeCm is parsed"), Hitscan.RangeCm, 5000.0f);
	}

	Error.Reset();
	TestFalse(TEXT("an unknown mode is refused"),
		ParseWeaponDefinition(TEXT("{\"weapon_id\": \"weapon_laser\", \"mode\": \"laser\", \"damage_profile_id\": \"physical_10\", \"ammo_id\": \"ammo_cell\", \"magazine_size\": 12, \"fire_rate_rpm\": 240, \"burst_count\": 1, \"pellet_count\": 1, \"spread_degrees_deg\": 0, \"range_cm\": 5000, \"projectile_id\": \"\", \"melee_attack_ids\": []}"), Hitscan, Error));
	TestTrue(TEXT("the unknown-mode error names mode"), Error.Contains(TEXT("mode")));

	TestFalse(TEXT("a fractional burst_count is refused"),
		ParseWeaponDefinition(TEXT("{\"weapon_id\": \"weapon_burst\", \"mode\": \"hitscan\", \"damage_profile_id\": \"physical_10\", \"ammo_id\": \"ammo_cell\", \"magazine_size\": 12, \"fire_rate_rpm\": 240, \"burst_count\": 2.5, \"pellet_count\": 1, \"spread_degrees_deg\": 0, \"range_cm\": 5000, \"projectile_id\": \"\", \"melee_attack_ids\": []}"), Hitscan, Error));
	TestTrue(TEXT("the fractional error names burst_count"), Error.Contains(TEXT("burst_count")));

	TestFalse(TEXT("a missing melee_attack_ids field is refused"),
		ParseWeaponDefinition(TEXT("{\"weapon_id\": \"weapon_training_sword\", \"mode\": \"melee\", \"damage_profile_id\": \"physical_10\", \"ammo_id\": \"\", \"magazine_size\": 0, \"fire_rate_rpm\": 0, \"burst_count\": 1, \"pellet_count\": 1, \"spread_degrees_deg\": 0, \"range_cm\": 0, \"projectile_id\": \"\"}"), Hitscan, Error));
	TestTrue(TEXT("the missing-array error names melee_attack_ids"), Error.Contains(TEXT("melee_attack_ids")));

	TestFalse(TEXT("a non-string entry inside melee_attack_ids is refused"),
		ParseWeaponDefinition(TEXT("{\"weapon_id\": \"weapon_training_sword\", \"mode\": \"melee\", \"damage_profile_id\": \"physical_10\", \"ammo_id\": \"\", \"magazine_size\": 0, \"fire_rate_rpm\": 0, \"burst_count\": 1, \"pellet_count\": 1, \"spread_degrees_deg\": 0, \"range_cm\": 0, \"projectile_id\": \"\", \"melee_attack_ids\": [\"light_01\", 2, \"launcher\", \"aerial_01\"]}"), Hitscan, Error));
	TestTrue(TEXT("the element error names melee_attack_ids[1]"), Error.Contains(TEXT("melee_attack_ids[1]")));

	// Illegal combinations are refused through the 004 validator.
	FWeaponDefinition MeleeWithAmmo;
	TestFalse(TEXT("a melee weapon with an ammo id is refused"),
		ParseWeaponDefinition(TEXT("{\"weapon_id\": \"weapon_training_sword\", \"mode\": \"melee\", \"damage_profile_id\": \"physical_10\", \"ammo_id\": \"ammo_cell\", \"magazine_size\": 0, \"fire_rate_rpm\": 0, \"burst_count\": 1, \"pellet_count\": 1, \"spread_degrees_deg\": 0, \"range_cm\": 0, \"projectile_id\": \"\", \"melee_attack_ids\": [\"light_01\", \"light_02\", \"launcher\", \"aerial_01\"]}"), MeleeWithAmmo, Error));
	TestTrue(TEXT("the melee-ammo error names AmmoId"), Error.Contains(TEXT("AmmoId")));

	FWeaponDefinition IncompleteSet;
	TestFalse(TEXT("an incomplete melee attack set is refused"),
		ParseWeaponDefinition(TEXT("{\"weapon_id\": \"weapon_training_sword\", \"mode\": \"melee\", \"damage_profile_id\": \"physical_10\", \"ammo_id\": \"\", \"magazine_size\": 0, \"fire_rate_rpm\": 0, \"burst_count\": 1, \"pellet_count\": 1, \"spread_degrees_deg\": 0, \"range_cm\": 0, \"projectile_id\": \"\", \"melee_attack_ids\": [\"light_01\"]}"), IncompleteSet, Error));
	TestTrue(TEXT("the incomplete-set error names MeleeAttackIds"), Error.Contains(TEXT("MeleeAttackIds")));

	FWeaponDefinition UnknownField;
	TestFalse(TEXT("an unknown field is refused"),
		ParseWeaponDefinition(TEXT("{\"weapon_id\": \"weapon_training_sword\", \"mode\": \"melee\", \"damage_profile_id\": \"physical_10\", \"ammo_id\": \"\", \"magazine_size\": 0, \"fire_rate_rpm\": 0, \"burst_count\": 1, \"pellet_count\": 1, \"spread_degrees_deg\": 0, \"range_cm\": 0, \"projectile_id\": \"\", \"melee_attack_ids\": [\"light_01\", \"light_02\", \"launcher\", \"aerial_01\"], \"reload_s\": 1.2}"), UnknownField, Error));
	TestTrue(TEXT("the unknown-field error names reload_s"), Error.Contains(TEXT("reload_s")));
	return true;
}

// ---------------------------------------------------------------------------
// AmmoAndProjectileRowParsing
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_005AmmoAndProjectileRowParsing,
	"UEMMO.Tasks.M5_005.AmmoAndProjectileRowParsing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_005AmmoAndProjectileRowParsing::RunTest(const FString& Parameters)
{
	FAmmoType Ammo;
	FString Error;
	TestTrue(TEXT("the ammo_cell row parses"),
		ParseAmmoType(TEXT("{\"ammo_id\": \"ammo_cell\", \"max_reserve\": 120, \"magazine_size\": 12}"), Ammo, Error));
	if (TestTrue(TEXT("the ammo row holds values"), Error.IsEmpty()))
	{
		TestEqual(TEXT("the id is parsed"), Ammo.AmmoId, FName(TEXT("ammo_cell")));
		TestEqual(TEXT("MaxReserve is parsed"), Ammo.MaxReserve, 120);
		TestEqual(TEXT("MagazineSize is parsed"), Ammo.MagazineSize, 12);
	}

	Error.Reset();
	TestFalse(TEXT("a missing max_reserve is refused"),
		ParseAmmoType(TEXT("{\"ammo_id\": \"ammo_cell\", \"magazine_size\": 12}"), Ammo, Error));
	TestTrue(TEXT("the missing-reserve error names max_reserve"), Error.Contains(TEXT("max_reserve")));

	TestFalse(TEXT("a fractional magazine_size is refused"),
		ParseAmmoType(TEXT("{\"ammo_id\": \"ammo_cell\", \"max_reserve\": 120, \"magazine_size\": 12.5}"), Ammo, Error));
	TestTrue(TEXT("the fractional error names magazine_size"), Error.Contains(TEXT("magazine_size")));

	TestFalse(TEXT("a negative max_reserve is refused"),
		ParseAmmoType(TEXT("{\"ammo_id\": \"ammo_cell\", \"max_reserve\": -1, \"magazine_size\": 12}"), Ammo, Error));
	TestTrue(TEXT("the negative-reserve error names MaxReserve"), Error.Contains(TEXT("MaxReserve")));

	TestFalse(TEXT("a zero magazine_size is refused"),
		ParseAmmoType(TEXT("{\"ammo_id\": \"ammo_cell\", \"max_reserve\": 120, \"magazine_size\": 0}"), Ammo, Error));
	TestTrue(TEXT("the zero-magazine error names MagazineSize"), Error.Contains(TEXT("MagazineSize")));

	TestFalse(TEXT("an unknown ammo field is refused"),
		ParseAmmoType(TEXT("{\"ammo_id\": \"ammo_cell\", \"max_reserve\": 120, \"magazine_size\": 12, \"display_name\": \"Cell\"}"), Ammo, Error));
	TestTrue(TEXT("the unknown-field error names display_name"), Error.Contains(TEXT("display_name")));

	// Projectiles.
	FProjectileDefinition Projectile;
	TestTrue(TEXT("the bullet_linear row parses"), ParseProjectileDefinition(BulletLinearRowJson, Projectile, Error));
	if (TestTrue(TEXT("the projectile row holds values"), Error.IsEmpty()))
	{
		TestEqual(TEXT("the id is parsed"), Projectile.ProjectileId, FName(TEXT("bullet_linear")));
		TestTrue(TEXT("the motion is Straight"), Projectile.Motion == EProjectileMotion::Straight);
		TestEqual(TEXT("SpeedCmS is parsed"), Projectile.SpeedCmS, 6000.0f);
		TestEqual(TEXT("LifetimeS is parsed"), Projectile.LifetimeS, 2.0f);
		TestEqual(TEXT("the damage profile id is parsed"), Projectile.DamageProfileId, FName(TEXT("physical_10")));
	}

	Error.Reset();
	TestFalse(TEXT("an unknown motion is refused"),
		ParseProjectileDefinition(TEXT("{\"projectile_id\": \"bullet_bounce\", \"motion\": \"bounce\", \"speed_cm_s\": 6000, \"lifetime_s\": 2.0, \"damage_profile_id\": \"physical_10\", \"pierce_count\": 0, \"explosion_radius_cm\": 0, \"explosion_damage_profile_id\": \"\", \"homing_turn_rate_deg_s\": 0}"), Projectile, Error));
	TestTrue(TEXT("the unknown-motion error names motion"), Error.Contains(TEXT("motion")));

	TestFalse(TEXT("a fractional pierce_count is refused"),
		ParseProjectileDefinition(TEXT("{\"projectile_id\": \"bullet_piercing\", \"motion\": \"straight\", \"speed_cm_s\": 6000, \"lifetime_s\": 2.0, \"damage_profile_id\": \"physical_10\", \"pierce_count\": 1.5, \"explosion_radius_cm\": 0, \"explosion_damage_profile_id\": \"\", \"homing_turn_rate_deg_s\": 0}"), Projectile, Error));
	TestTrue(TEXT("the fractional error names pierce_count"), Error.Contains(TEXT("pierce_count")));

	FProjectileDefinition ZeroSpeed;
	TestFalse(TEXT("a zero speed is refused"),
		ParseProjectileDefinition(TEXT("{\"projectile_id\": \"bullet_stuck\", \"motion\": \"straight\", \"speed_cm_s\": 0, \"lifetime_s\": 2.0, \"damage_profile_id\": \"physical_10\", \"pierce_count\": 0, \"explosion_radius_cm\": 0, \"explosion_damage_profile_id\": \"\", \"homing_turn_rate_deg_s\": 0}"), ZeroSpeed, Error));
	TestTrue(TEXT("the zero-speed error names SpeedCmS"), Error.Contains(TEXT("SpeedCmS")));

	// Explosion and pierce do not combine (004 validator).
	FProjectileDefinition ExplodingPiercer;
	TestFalse(TEXT("an explosion combined with piercing is refused"),
		ParseProjectileDefinition(TEXT("{\"projectile_id\": \"rocket_boom\", \"motion\": \"straight\", \"speed_cm_s\": 4000, \"lifetime_s\": 4.0, \"damage_profile_id\": \"physical_10\", \"pierce_count\": 2, \"explosion_radius_cm\": 300, \"explosion_damage_profile_id\": \"explosive_40\", \"homing_turn_rate_deg_s\": 0}"), ExplodingPiercer, Error));
	TestTrue(TEXT("the explosion-pierce error names PierceCount"), Error.Contains(TEXT("PierceCount")));

	// A homing projectile needs a positive turn rate (004 validator).
	FProjectileDefinition HomingWithoutRate;
	TestFalse(TEXT("a homing projectile with a zero turn rate is refused"),
		ParseProjectileDefinition(TEXT("{\"projectile_id\": \"missile_lost\", \"motion\": \"homing\", \"speed_cm_s\": 2500, \"lifetime_s\": 5.0, \"damage_profile_id\": \"physical_10\", \"pierce_count\": 0, \"explosion_radius_cm\": 0, \"explosion_damage_profile_id\": \"\", \"homing_turn_rate_deg_s\": 0}"), HomingWithoutRate, Error));
	TestTrue(TEXT("the homing error names HomingTurnRateDegS"), Error.Contains(TEXT("HomingTurnRateDegS")));

	// An unknown projectile field is refused.
	FProjectileDefinition UnknownField;
	TestFalse(TEXT("an unknown field is refused"),
		ParseProjectileDefinition(TEXT("{\"projectile_id\": \"bullet_linear\", \"motion\": \"straight\", \"speed_cm_s\": 6000, \"lifetime_s\": 2.0, \"damage_profile_id\": \"physical_10\", \"pierce_count\": 0, \"explosion_radius_cm\": 0, \"explosion_damage_profile_id\": \"\", \"homing_turn_rate_deg_s\": 0, \"gravity_scale\": 1.0}"), UnknownField, Error));
	TestTrue(TEXT("the unknown-field error names gravity_scale"), Error.Contains(TEXT("gravity_scale")));
	return true;
}

// ---------------------------------------------------------------------------
// DirectoryLoadPositive (shipped Data/CombatSystem tables)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_005DirectoryLoadPositive,
	"UEMMO.Tasks.M5_005.DirectoryLoadPositive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_005DirectoryLoadPositive::RunTest(const FString& Parameters)
{
	const FString DataDir = FPaths::ProjectDir() / TEXT("Data") / TEXT("CombatSystem");
	FCombatDataTableSet Tables;
	TArray<FString> Problems;
	TestTrue(TEXT("the shipped Data/CombatSystem directory loads"),
		LoadCombatDataDirectory(DataDir, Tables, Problems));
	if (!TestTrue(TEXT("the shipped directory load reported no problems"), Problems.Num() == 0))
	{
		for (const FString& Problem : Problems)
		{
			AddError(FString::Printf(TEXT("load problem: %s"), *Problem));
		}
		return true;
	}

	// Required-table manifest: exactly the seven A-segment tables.
	const TArray<FName>& Required = GetRequiredCombatTableNames();
	TestEqual(TEXT("the required-table manifest lists seven tables"), Required.Num(), 7);
	TestTrue(TEXT("the manifest requires damage_profiles"), Required.Contains(FName(TEXT("damage_profiles"))));
	TestTrue(TEXT("the manifest requires weapons"), Required.Contains(FName(TEXT("weapons"))));
	TestTrue(TEXT("the manifest requires projectiles"), Required.Contains(FName(TEXT("projectiles"))));
	TestFalse(TEXT("the manifest does not require vehicles"), Required.Contains(FName(TEXT("vehicles"))));

	// Row counts of the shipped tables.
	TestEqual(TEXT("damage_profiles holds 4 rows"), Tables.DamageProfiles.Num(), 4);
	TestEqual(TEXT("attack_reactions holds 4 rows"), Tables.AttackReactions.Num(), 4);
	TestEqual(TEXT("target_reactions holds 3 rows"), Tables.TargetReactions.Num(), 3);
	TestEqual(TEXT("presentations holds 4 rows"), Tables.Presentations.Num(), 4);
	TestEqual(TEXT("weapons holds 8 rows"), Tables.Weapons.Num(), 8);
	TestEqual(TEXT("ammo_types holds 5 rows"), Tables.AmmoTypes.Num(), 5);
	TestEqual(TEXT("projectiles holds 5 rows"), Tables.Projectiles.Num(), 5);

	// The known-unparsed tables were skipped explicitly, not silently ignored.
	TestTrue(TEXT("target_filters.json is recorded as a skipped known table"),
		Tables.SkippedKnownTableFiles.Contains(FString(TEXT("target_filters.json"))));

	// Pinned legacy values survive the round trip.
	const FDamageProfile* Light01 = Tables.DamageProfiles.Find(FName(TEXT("light_01")));
	if (TestNotNull(TEXT("light_01 is loaded"), Light01))
	{
		TestEqual(TEXT("light_01 keeps base damage 10"), Light01->BaseDamage, 10.0f);
		TestEqual(TEXT("light_01 keeps hit stun 0.22"), Light01->HitStunSeconds, 0.22f);
	}
	const FDamageProfile* Launcher = Tables.DamageProfiles.Find(FName(TEXT("launcher")));
	if (TestNotNull(TEXT("launcher is loaded"), Launcher))
	{
		TestEqual(TEXT("launcher keeps launch speed 700"), Launcher->LaunchCmPerSecond, 700.0f);
	}

	const FTargetReaction* Boss = Tables.TargetReactions.Find(FName(TEXT("boss")));
	if (TestNotNull(TEXT("boss is loaded"), Boss))
	{
		TestTrue(TEXT("boss is control immune"), Boss->bImmuneControl);
		TestTrue(TEXT("boss is death resistant"), Boss->bDeathResistant);
		TestFalse(TEXT("boss is not damage immune"), Boss->bImmuneDamage);
		TestEqual(TEXT("boss accepts zero launches per air cycle"), Boss->MaxLaunchesPerAirCycle, 0);
	}
	const FTargetReaction* Heavy = Tables.TargetReactions.Find(FName(TEXT("heavy")));
	if (TestNotNull(TEXT("heavy is loaded"), Heavy))
	{
		TestFalse(TEXT("heavy refuses launch"), Heavy->bAllowLaunch);
		TestEqual(TEXT("heavy carries a poise pool"), Heavy->PoiseMax, 120.0f);
	}

	const FWeaponDefinition* Sword = Tables.Weapons.Find(FName(TEXT("weapon_training_sword")));
	if (TestNotNull(TEXT("weapon_training_sword is loaded"), Sword))
	{
		TestTrue(TEXT("the sword is melee"), Sword->FireMode == EWeaponFireMode::Melee);
		TestEqual(TEXT("the sword carries the legacy four attacks"), Sword->MeleeAttackIds.Num(), 4);
	}
	const FWeaponDefinition* Pellet = Tables.Weapons.Find(FName(TEXT("weapon_pellet_sample")));
	if (TestNotNull(TEXT("weapon_pellet_sample is loaded"), Pellet))
	{
		TestEqual(TEXT("the pellet weapon fires 5 pellets"), Pellet->PelletCount, 5);
		TestEqual(TEXT("the pellet weapon spreads 8 degrees"), Pellet->SpreadDegrees, 8.0f);
	}

	const FAmmoType* AmmoCell = Tables.AmmoTypes.Find(FName(TEXT("ammo_cell")));
	if (TestNotNull(TEXT("ammo_cell is loaded"), AmmoCell))
	{
		TestEqual(TEXT("ammo_cell keeps reserve 120"), AmmoCell->MaxReserve, 120);
		TestEqual(TEXT("ammo_cell keeps magazine 12"), AmmoCell->MagazineSize, 12);
	}

	const FProjectileDefinition* Bullet = Tables.Projectiles.Find(FName(TEXT("bullet_linear")));
	if (TestNotNull(TEXT("bullet_linear is loaded"), Bullet))
	{
		TestTrue(TEXT("bullet_linear flies straight"), Bullet->Motion == EProjectileMotion::Straight);
		TestEqual(TEXT("bullet_linear keeps speed 6000"), Bullet->SpeedCmS, 6000.0f);
	}
	const FProjectileDefinition* Rocket = Tables.Projectiles.Find(FName(TEXT("rocket_explosive")));
	if (TestNotNull(TEXT("rocket_explosive is loaded"), Rocket))
	{
		TestEqual(TEXT("rocket_explosive explodes with radius 300"), Rocket->ExplosionRadiusCm, 300.0f);
		TestEqual(TEXT("rocket_explosive uses the explosive_40 profile"), Rocket->ExplosionDamageProfileId, FName(TEXT("explosive_40")));
	}

	const FCombatPresentation* HitLight01 = Tables.Presentations.Find(FName(TEXT("hit_light_01")));
	if (TestNotNull(TEXT("hit_light_01 is loaded"), HitLight01))
	{
		TestEqual(TEXT("hit_light_01 targets light_01"), HitLight01->ReactionId, FName(TEXT("light_01")));
		// M5-017 owned the presentation resources the M5-003 sample deferred:
		// the row is no longer a placeholder and its montage reference is real.
		TestFalse(TEXT("hit_light_01 is no longer a placeholder (owned since M5-017)"), HitLight01->bPlaceholder);
		TestTrue(TEXT("hit_light_01 carries a real montage reference"), HitLight01->MontagePath.Contains(TEXT("RCT_Hit")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// DirectoryLoadRejectsBadSets (temporary directories)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_005DirectoryLoadRejectsBadSets,
	"UEMMO.Tasks.M5_005.DirectoryLoadRejectsBadSets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_005DirectoryLoadRejectsBadSets::RunTest(const FString& Parameters)
{
	// A minimal valid set loads from a fresh directory: the A segment needs no
	// vehicles, no tags and no manifest file to parse.
	{
		const FString Dir = BuildTempDataDir(TEXT("MinimalValid"));
		TestTrue(TEXT("the minimal valid set was written"), WriteMinimalValidSet(Dir));
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestTrue(TEXT("the minimal valid directory loads"), LoadCombatDataDirectory(Dir, Tables, Problems));
		if (!TestTrue(TEXT("the minimal load reported no problems"), Problems.Num() == 0))
		{
			for (const FString& Problem : Problems)
			{
				AddError(FString::Printf(TEXT("minimal load problem: %s"), *Problem));
			}
		}
		else
		{
			TestEqual(TEXT("the minimal set loaded one weapon"), Tables.Weapons.Num(), 1);
			TestEqual(TEXT("the minimal set skipped no known tables"), Tables.SkippedKnownTableFiles.Num(), 0);
			TArray<FString> ReferenceProblems;
			TestTrue(TEXT("the minimal set has clean cross-table references"),
				ValidateCombatDataReferences(Tables, ReferenceProblems));
			TestTrue(TEXT("the clean references reported no problems"), ReferenceProblems.Num() == 0);
		}
		RemoveTempDataDir(Dir);
	}

	// A missing required table is refused and names the table.
	{
		const FString Dir = BuildTempDataDir(TEXT("MissingWeapons"));
		TestTrue(TEXT("the set was written"), WriteMinimalValidSet(Dir));
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		TestTrue(TEXT("weapons.json was deleted"), PlatformFile.DeleteFile(*(Dir / TEXT("weapons.json"))));
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestFalse(TEXT("a directory without weapons.json is refused"), LoadCombatDataDirectory(Dir, Tables, Problems));
		bool bNamedWeapons = false;
		for (const FString& Problem : Problems)
		{
			bNamedWeapons = bNamedWeapons || Problem.Contains(TEXT("weapons"));
		}
		TestTrue(TEXT("the missing-table error names weapons"), bNamedWeapons);
		RemoveTempDataDir(Dir);
	}

	// A non-existent directory is refused.
	{
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestFalse(TEXT("a non-existent directory is refused"),
			LoadCombatDataDirectory(FPaths::ProjectSavedDir() / TEXT("Temp") / TEXT("M5_005") / TEXT("DoesNotExist"), Tables, Problems));
	}

	// An unknown table file name is refused (no silent ignoring of typos).
	{
		const FString Dir = BuildTempDataDir(TEXT("UnknownTable"));
		TestTrue(TEXT("the set was written"), WriteMinimalValidSet(Dir));
		TestTrue(TEXT("the typo table file was written"), WriteTableFile(Dir, TEXT("wepons.json"), TEXT(
			"{\"schema_version\": 1, \"table\": \"wepons\", \"rows\": []}\n")));
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestFalse(TEXT("an unknown table file is refused"), LoadCombatDataDirectory(Dir, Tables, Problems));
		bool bNamedUnknown = false;
		for (const FString& Problem : Problems)
		{
			bNamedUnknown = bNamedUnknown || Problem.Contains(TEXT("wepons.json"));
		}
		TestTrue(TEXT("the unknown-file error names wepons.json"), bNamedUnknown);
		RemoveTempDataDir(Dir);
	}

	// A known-unparsed table is accepted and recorded, not parsed.
	{
		const FString Dir = BuildTempDataDir(TEXT("KnownUnparsed"));
		TestTrue(TEXT("the set was written"), WriteMinimalValidSet(Dir));
		TestTrue(TEXT("the target_filters file was written"), WriteTableFile(Dir, TEXT("target_filters.json"), TEXT(
			"{\"schema_version\": 1, \"table\": \"target_filters\", \"rows\": []}\n")));
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestTrue(TEXT("a directory with target_filters.json loads"), LoadCombatDataDirectory(Dir, Tables, Problems));
		TestTrue(TEXT("target_filters.json was recorded as skipped"),
			Tables.SkippedKnownTableFiles.Contains(FString(TEXT("target_filters.json"))));
		RemoveTempDataDir(Dir);
	}

	// The inner table name must match the file.
	{
		const FString Dir = BuildTempDataDir(TEXT("TableNameMismatch"));
		TestTrue(TEXT("the set was written"), WriteMinimalValidSet(Dir));
		TestTrue(TEXT("the mismatched file was written"), WriteTableFile(Dir, TEXT("weapons.json"), TEXT(
			"{\"schema_version\": 1, \"table\": \"wepons\", \"rows\": []}\n")));
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestFalse(TEXT("a table-name mismatch is refused"), LoadCombatDataDirectory(Dir, Tables, Problems));
		bool bNamedMismatch = false;
		for (const FString& Problem : Problems)
		{
			bNamedMismatch = bNamedMismatch || Problem.Contains(TEXT("table"));
		}
		TestTrue(TEXT("the mismatch error mentions the table field"), bNamedMismatch);
		RemoveTempDataDir(Dir);
	}

	// A wrong schema version is refused.
	{
		const FString Dir = BuildTempDataDir(TEXT("WrongSchemaVersion"));
		TestTrue(TEXT("the set was written"), WriteMinimalValidSet(Dir));
		TestTrue(TEXT("the v2 file was written"), WriteTableFile(Dir, TEXT("ammo_types.json"), TEXT(
			"{\"schema_version\": 2, \"table\": \"ammo_types\", \"rows\": []}\n")));
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestFalse(TEXT("a future schema version is refused"), LoadCombatDataDirectory(Dir, Tables, Problems));
		bool bNamedVersion = false;
		for (const FString& Problem : Problems)
		{
			bNamedVersion = bNamedVersion || Problem.Contains(TEXT("schema_version"));
		}
		TestTrue(TEXT("the version error names schema_version"), bNamedVersion);
		RemoveTempDataDir(Dir);
	}

	// A duplicate id inside one table is refused.
	{
		const FString Dir = BuildTempDataDir(TEXT("DuplicateAmmoId"));
		TestTrue(TEXT("the set was written"), WriteMinimalValidSet(Dir));
		TestTrue(TEXT("the duplicate file was written"), WriteTableFile(Dir, TEXT("ammo_types.json"), TEXT(
			"{\"schema_version\": 1, \"table\": \"ammo_types\", \"rows\": ["
			"{\"ammo_id\": \"ammo_cell\", \"max_reserve\": 120, \"magazine_size\": 12}, "
			"{\"ammo_id\": \"ammo_cell\", \"max_reserve\": 60, \"magazine_size\": 12}"
			"]}\n")));
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestFalse(TEXT("a duplicate id is refused"), LoadCombatDataDirectory(Dir, Tables, Problems));
		bool bNamedDuplicate = false;
		for (const FString& Problem : Problems)
		{
			bNamedDuplicate = bNamedDuplicate || Problem.Contains(TEXT("duplicate"));
		}
		TestTrue(TEXT("the duplicate error says duplicate"), bNamedDuplicate);
		RemoveTempDataDir(Dir);
	}

	// Broken JSON is refused.
	{
		const FString Dir = BuildTempDataDir(TEXT("BrokenJson"));
		TestTrue(TEXT("the set was written"), WriteMinimalValidSet(Dir));
		TestTrue(TEXT("the broken file was written"), WriteTableFile(Dir, TEXT("projectiles.json"), TEXT("{")));
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestFalse(TEXT("broken JSON is refused"), LoadCombatDataDirectory(Dir, Tables, Problems));
		TestTrue(TEXT("the broken-JSON error names the file"), Problems.Num() > 0 && Problems[0].Contains(TEXT("projectiles.json")));
		RemoveTempDataDir(Dir);
	}

	// An unknown field inside a shipped-style row is refused through the same path.
	{
		const FString Dir = BuildTempDataDir(TEXT("UnknownRowField"));
		TestTrue(TEXT("the set was written"), WriteMinimalValidSet(Dir));
		TestTrue(TEXT("the unknown-field file was written"), WriteTableFile(Dir, TEXT("ammo_types.json"), TEXT(
			"{\"schema_version\": 1, \"table\": \"ammo_types\", \"rows\": ["
			"{\"ammo_id\": \"ammo_cell\", \"max_reserve\": 120, \"magazine_size\": 12, \"tier\": 2}"
			"]}\n")));
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestFalse(TEXT("an unknown row field is refused"), LoadCombatDataDirectory(Dir, Tables, Problems));
		bool bNamedField = false;
		for (const FString& Problem : Problems)
		{
			bNamedField = bNamedField || Problem.Contains(TEXT("tier"));
		}
		TestTrue(TEXT("the loader error names the unknown field"), bNamedField);
		RemoveTempDataDir(Dir);
	}

	// Two presentations mapping one reaction are refused.
	{
		const FString Dir = BuildTempDataDir(TEXT("DuplicateReactionMapping"));
		TestTrue(TEXT("the set was written"), WriteMinimalValidSet(Dir));
		TestTrue(TEXT("the double-mapping file was written"), WriteTableFile(Dir, TEXT("presentations.json"), TEXT(
			"{\"schema_version\": 1, \"table\": \"presentations\", \"rows\": ["
			"{\"presentation_id\": \"hit_light_01\", \"reaction_id\": \"light_01\", \"montage_path\": \"\", \"sound_path\": \"\", \"effect_path\": \"\", \"placeholder\": true}, "
			"{\"presentation_id\": \"hit_light_01_alt\", \"reaction_id\": \"light_01\", \"montage_path\": \"\", \"sound_path\": \"\", \"effect_path\": \"\", \"placeholder\": true}"
			"]}\n")));
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		TestFalse(TEXT("a duplicated reaction mapping is refused"), LoadCombatDataDirectory(Dir, Tables, Problems));
		bool bNamedReaction = false;
		for (const FString& Problem : Problems)
		{
			bNamedReaction = bNamedReaction || Problem.Contains(TEXT("light_01"));
		}
		TestTrue(TEXT("the double-mapping error names the reaction"), bNamedReaction);
		RemoveTempDataDir(Dir);
	}
	return true;
}

// ---------------------------------------------------------------------------
// ReferenceValidation (cross-table references; enforcement wiring is M5-006)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_005ReferenceValidation,
	"UEMMO.Tasks.M5_005.ReferenceValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_005ReferenceValidation::RunTest(const FString& Parameters)
{
	// Every unknown cross-table reference is refused and names the source
	// row, the field and the missing id.
	struct FReferenceCase
	{
		const TCHAR* TableFile;
		const TCHAR* RowJson;
		const TCHAR* ExpectedField;
		const TCHAR* ExpectedId;
	};
	const FReferenceCase Cases[] = {
		{TEXT("weapons.json"),
			TEXT("{\"weapon_id\": \"weapon_test_gun\", \"mode\": \"hitscan\", \"damage_profile_id\": \"light_01\", \"ammo_id\": \"ammo_missing\", \"magazine_size\": 12, \"fire_rate_rpm\": 240, \"burst_count\": 1, \"pellet_count\": 1, \"spread_degrees_deg\": 0, \"range_cm\": 5000, \"projectile_id\": \"\", \"melee_attack_ids\": []}"),
			TEXT("ammo_id"), TEXT("ammo_missing")},
		{TEXT("weapons.json"),
			TEXT("{\"weapon_id\": \"weapon_test_gun\", \"mode\": \"projectile\", \"damage_profile_id\": \"\", \"ammo_id\": \"ammo_cell\", \"magazine_size\": 12, \"fire_rate_rpm\": 240, \"burst_count\": 1, \"pellet_count\": 1, \"spread_degrees_deg\": 0, \"range_cm\": 0, \"projectile_id\": \"bullet_missing\", \"melee_attack_ids\": []}"),
			TEXT("projectile_id"), TEXT("bullet_missing")},
		{TEXT("weapons.json"),
			TEXT("{\"weapon_id\": \"weapon_test_gun\", \"mode\": \"hitscan\", \"damage_profile_id\": \"profile_missing\", \"ammo_id\": \"ammo_cell\", \"magazine_size\": 12, \"fire_rate_rpm\": 240, \"burst_count\": 1, \"pellet_count\": 1, \"spread_degrees_deg\": 0, \"range_cm\": 5000, \"projectile_id\": \"\", \"melee_attack_ids\": []}"),
			TEXT("damage_profile_id"), TEXT("profile_missing")},
		{TEXT("projectiles.json"),
			TEXT("{\"projectile_id\": \"bullet_test\", \"motion\": \"straight\", \"speed_cm_s\": 6000, \"lifetime_s\": 2.0, \"damage_profile_id\": \"profile_missing\", \"pierce_count\": 0, \"explosion_radius_cm\": 0, \"explosion_damage_profile_id\": \"\", \"homing_turn_rate_deg_s\": 0}"),
			TEXT("damage_profile_id"), TEXT("profile_missing")},
		{TEXT("presentations.json"),
			TEXT("{\"presentation_id\": \"hit_light_01\", \"reaction_id\": \"reaction_missing\", \"montage_path\": \"\", \"sound_path\": \"\", \"effect_path\": \"\", \"placeholder\": true}"),
			TEXT("reaction_id"), TEXT("reaction_missing")}
	};
	for (const FReferenceCase& Case : Cases)
	{
		const FString Dir = BuildTempDataDir(TEXT("ReferenceCase"));
		bool bWritten = WriteMinimalValidSet(Dir);
		bWritten = bWritten && WriteTableFile(Dir, Case.TableFile, FString::Printf(
			TEXT("{\"schema_version\": 1, \"table\": \"%s\", \"rows\": [%s]}"),
			*FPaths::GetBaseFilename(Case.TableFile), Case.RowJson));
		if (!TestTrue(FString::Printf(TEXT("the reference case files were written (%s)"), Case.ExpectedField), bWritten))
		{
			RemoveTempDataDir(Dir);
			continue;
		}
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		if (!TestTrue(FString::Printf(TEXT("the reference case directory loads (%s)"), Case.ExpectedField),
			LoadCombatDataDirectory(Dir, Tables, Problems)))
		{
			for (const FString& Problem : Problems)
			{
				AddError(FString::Printf(TEXT("unexpected load problem: %s"), *Problem));
			}
			RemoveTempDataDir(Dir);
			continue;
		}
		TArray<FString> ReferenceProblems;
		TestFalse(FString::Printf(TEXT("an unknown %s reference is refused"), Case.ExpectedField),
			ValidateCombatDataReferences(Tables, ReferenceProblems));
		bool bNamedField = false;
		bool bNamedId = false;
		for (const FString& Problem : ReferenceProblems)
		{
			bNamedField = bNamedField || Problem.Contains(Case.ExpectedField);
			bNamedId = bNamedId || Problem.Contains(Case.ExpectedId);
		}
		TestTrue(FString::Printf(TEXT("the reference error names the field %s"), Case.ExpectedField), bNamedField);
		TestTrue(FString::Printf(TEXT("the reference error names the missing id %s"), Case.ExpectedId), bNamedId);
		RemoveTempDataDir(Dir);
	}

	// The shipped tables resolve their ammo, projectile and reaction
	// references. The shipped weapons/projectiles reference the damage
	// profiles physical_10 and explosive_40, which the 003 sample
	// damage_profiles.json does not define; that gap is reported here (and
	// resolved by the later cross-table validation card), so only the
	// ammo/projectile/reaction axes are pinned clean.
	{
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		if (TestTrue(TEXT("the shipped directory loads"), LoadCombatDataDirectory(FPaths::ProjectDir() / TEXT("Data") / TEXT("CombatSystem"), Tables, Problems)))
		{
			TArray<FString> ReferenceProblems;
			ValidateCombatDataReferences(Tables, ReferenceProblems);
			for (const FString& Problem : ReferenceProblems)
			{
				TestFalse(FString::Printf(TEXT("shipped ammo/projectile/reaction references stay clean (%s)"), *Problem),
					Problem.Contains(TEXT("'ammo_id'")) || Problem.Contains(TEXT("'projectile_id'")) || Problem.Contains(TEXT("'reaction_id'")));
			}
		}
	}
	return true;
}

#endif
