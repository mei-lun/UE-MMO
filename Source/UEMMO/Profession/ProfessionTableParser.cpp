#include "ProfessionTableParser.h"

#include "../Combat/Data/CombatDataTableParser.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "../Profile/ExperienceCurve.h"

namespace UE::UEMMO::Profession
{
	static constexpr int32 MaxLevelCap = 99;
	static constexpr int32 MaxRankCap = 8;

	static bool IsIdSyntax(const FString& Text)
	{
		if (Text.IsEmpty() || Text.Len() > 64)
		{
			return false;
		}
		for (const TCHAR Char : Text)
		{
			const bool bOk = (Char >= TEXT('a') && Char <= TEXT('z')) ||
				(Char >= TEXT('0') && Char <= TEXT('9')) || Char == TEXT('_');
			if (!bOk)
			{
				return false;
			}
		}
		return true;
	}

	static bool IsIntegral(double Number)
	{
		return static_cast<double>(static_cast<int64>(Number)) == Number;
	}

	// -- small typed readers naming table/row/field on failure ------------------

	static bool ReadString(const TSharedPtr<FCombatJsonValue>& Row, const TCHAR* Key, FString& Out,
		const FString& Table, const FString& RowName, TArray<FString>& Problems)
	{
		const TSharedPtr<FCombatJsonValue>* Found = Row->Object.Find(Key);
		if (!Found || (*Found)->Kind != FCombatJsonValue::EKind::String)
		{
			Problems.Add(FString::Printf(TEXT("%s[%s]: missing or non-string field '%s'"), *Table, *RowName, Key));
			return false;
		}
		Out = (*Found)->String;
		return true;
	}

	static bool ReadName(const TSharedPtr<FCombatJsonValue>& Row, const TCHAR* Key, FName& Out,
		const FString& Table, const FString& RowName, TArray<FString>& Problems)
	{
		FString Text;
		if (!ReadString(Row, Key, Text, Table, RowName, Problems))
		{
			return false;
		}
		if (!IsIdSyntax(Text))
		{
			Problems.Add(FString::Printf(TEXT("%s[%s]: field '%s' is not a lowercase id: '%s'"), *Table, *RowName, Key, *Text));
			return false;
		}
		Out = FName(Text);
		return true;
	}

	static bool ReadInt(const TSharedPtr<FCombatJsonValue>& Row, const TCHAR* Key, int32& Out,
		const FString& Table, const FString& RowName, TArray<FString>& Problems)
	{
		const TSharedPtr<FCombatJsonValue>* Found = Row->Object.Find(Key);
		if (!Found || (*Found)->Kind != FCombatJsonValue::EKind::Number || !IsIntegral((*Found)->Number))
		{
			Problems.Add(FString::Printf(TEXT("%s[%s]: missing or non-integer field '%s'"), *Table, *RowName, Key));
			return false;
		}
		Out = static_cast<int32>((*Found)->Number);
		return true;
	}

	static bool ReadFloat(const TSharedPtr<FCombatJsonValue>& Row, const TCHAR* Key, float& Out,
		const FString& Table, const FString& RowName, TArray<FString>& Problems)
	{
		const TSharedPtr<FCombatJsonValue>* Found = Row->Object.Find(Key);
		if (!Found || (*Found)->Kind != FCombatJsonValue::EKind::Number)
		{
			Problems.Add(FString::Printf(TEXT("%s[%s]: missing or non-number field '%s'"), *Table, *RowName, Key));
			return false;
		}
		Out = static_cast<float>((*Found)->Number);
		return true;
	}

	static bool ReadBool(const TSharedPtr<FCombatJsonValue>& Row, const TCHAR* Key, bool& Out,
		const FString& Table, const FString& RowName, TArray<FString>& Problems)
	{
		const TSharedPtr<FCombatJsonValue>* Found = Row->Object.Find(Key);
		if (!Found || (*Found)->Kind != FCombatJsonValue::EKind::Boolean)
		{
			Problems.Add(FString::Printf(TEXT("%s[%s]: missing or non-boolean field '%s'"), *Table, *RowName, Key));
			return false;
		}
		Out = (*Found)->Boolean;
		return true;
	}

	static bool ReadNameMap(const TSharedPtr<FCombatJsonValue>& Row, const TCHAR* Key,
		TMap<FName, float>& Out, const FString& Table, const FString& RowName, TArray<FString>& Problems)
	{
		const TSharedPtr<FCombatJsonValue>* Found = Row->Object.Find(Key);
		if (!Found)
		{
			Problems.Add(FString::Printf(TEXT("%s[%s]: missing field '%s'"), *Table, *RowName, Key));
			return false;
		}
		if ((*Found)->Kind != FCombatJsonValue::EKind::Object)
		{
			Problems.Add(FString::Printf(TEXT("%s[%s]: field '%s' is not an object"), *Table, *RowName, Key));
			return false;
		}
		for (const TPair<FString, TSharedPtr<FCombatJsonValue>>& Pair : (*Found)->Object)
		{
			if (!IsIdSyntax(Pair.Key))
			{
				Problems.Add(FString::Printf(TEXT("%s[%s]: '%s' key '%s' is not a lowercase id"), *Table, *RowName, Key, *Pair.Key));
				return false;
			}
			if (!Pair.Value || Pair.Value->Kind != FCombatJsonValue::EKind::Number)
			{
				Problems.Add(FString::Printf(TEXT("%s[%s]: '%s[%s]' is not a number"), *Table, *RowName, Key, *Pair.Key));
				return false;
			}
			Out.Add(FName(Pair.Key), static_cast<float>(Pair.Value->Number));
		}
		return true;
	}

	static bool ReadStringArray(const TSharedPtr<FCombatJsonValue>& Row, const TCHAR* Key,
		TArray<FName>& Out, const FString& Table, const FString& RowName, TArray<FString>& Problems)
	{
		const TSharedPtr<FCombatJsonValue>* Found = Row->Object.Find(Key);
		if (!Found || (*Found)->Kind != FCombatJsonValue::EKind::Array)
		{
			Problems.Add(FString::Printf(TEXT("%s[%s]: missing or non-array field '%s'"), *Table, *RowName, Key));
			return false;
		}
		for (const TSharedPtr<FCombatJsonValue>& Item : (*Found)->Array)
		{
			if (!Item || Item->Kind != FCombatJsonValue::EKind::String || !IsIdSyntax(Item->String))
			{
				Problems.Add(FString::Printf(TEXT("%s[%s]: '%s' contains a non-id entry"), *Table, *RowName, Key));
				return false;
			}
			Out.Add(FName(Item->String));
		}
		return true;
	}

	// -- per-table row parsers ---------------------------------------------------

	static bool ParseProfessionRow(const TSharedPtr<FCombatJsonValue>& Row, FProfessionRow& Out, FString& RowName, TArray<FString>& Problems)
	{
		const FString Table = TEXT("professions");
		if (!ReadName(Row, TEXT("profession_id"), Out.ProfessionId, Table, TEXT("profession_id"), Problems))
		{
			return false;
		}
		RowName = Out.ProfessionId.ToString();
		bool bOk = ReadString(Row, TEXT("display_name"), Out.DisplayName, Table, RowName, Problems);
		bOk = ReadName(Row, TEXT("curve_id"), Out.CurveId, Table, RowName, Problems) && bOk;
		bOk = ReadString(Row, TEXT("mesh_path"), Out.MeshPath, Table, RowName, Problems) && bOk;
		bOk = ReadString(Row, TEXT("skeleton_path"), Out.SkeletonPath, Table, RowName, Problems) && bOk;
		bOk = ReadName(Row, TEXT("locomotion_set_id"), Out.LocomotionSetId, Table, RowName, Problems) && bOk;
		bOk = ReadString(Row, TEXT("presentation_id"), Out.PresentationId, Table, RowName, Problems) && bOk;
		bOk = ReadBool(Row, TEXT("asset_placeholder"), Out.AssetPlaceholder, Table, RowName, Problems) && bOk;

		// starting_skill_ranks: object skill_id -> integer rank (may be empty)
		const TSharedPtr<FCombatJsonValue>* Ranks = Row->Object.Find(TEXT("starting_skill_ranks"));
		if (!Ranks || (*Ranks)->Kind != FCombatJsonValue::EKind::Object)
		{
			Problems.Add(FString::Printf(TEXT("%s[%s]: missing or non-object field 'starting_skill_ranks'"), *Table, *RowName));
			bOk = false;
		}
		else
		{
			for (const TPair<FString, TSharedPtr<FCombatJsonValue>>& Pair : (*Ranks)->Object)
			{
				if (!IsIdSyntax(Pair.Key) || !Pair.Value || Pair.Value->Kind != FCombatJsonValue::EKind::Number ||
					!IsIntegral(Pair.Value->Number))
				{
					Problems.Add(FString::Printf(TEXT("%s[%s]: starting_skill_ranks entry '%s' is not id:integer"), *Table, *RowName, *Pair.Key));
					bOk = false;
					continue;
				}
				Out.StartingSkillRanks.Add(FName(Pair.Key), static_cast<int32>(Pair.Value->Number));
			}
		}
		bOk = ReadStringArray(Row, TEXT("allowed_weapon_modes"), Out.AllowedWeaponModes, Table, RowName, Problems) && bOk;
		return bOk;
	}

	static bool ParseLevelCurveRow(const TSharedPtr<FCombatJsonValue>& Row, FProfessionLevelCurveRow& Out, FString& RowName, TArray<FString>& Problems)
	{
		const FString Table = TEXT("level_curves");
		bool bOk = ReadName(Row, TEXT("curve_id"), Out.CurveId, Table, TEXT("curve_id"), Problems);
		RowName = Out.CurveId.ToString();
		bOk = ReadInt(Row, TEXT("level"), Out.Level, Table, RowName, Problems) && bOk;
		RowName = FString::Printf(TEXT("%s@%d"), *RowName, Out.Level);
		bOk = ReadInt(Row, TEXT("xp_to_next"), Out.XPToNext, Table, RowName, Problems) && bOk;
		bOk = ReadInt(Row, TEXT("base_max_hp"), Out.BaseMaxHP, Table, RowName, Problems) && bOk;
		bOk = ReadInt(Row, TEXT("base_attack"), Out.BaseAttack, Table, RowName, Problems) && bOk;
		bOk = ReadInt(Row, TEXT("base_defense"), Out.BaseDefense, Table, RowName, Problems) && bOk;
		bOk = ReadInt(Row, TEXT("base_max_mp"), Out.BaseMaxMP, Table, RowName, Problems) && bOk;
		bOk = ReadInt(Row, TEXT("base_mp_regen"), Out.BaseMPRegen, Table, RowName, Problems) && bOk;
		bOk = ReadInt(Row, TEXT("skill_points"), Out.SkillPoints, Table, RowName, Problems) && bOk;
		return bOk;
	}

	static bool ParseSkillRow(const TSharedPtr<FCombatJsonValue>& Row, FSkillRow& Out, FString& RowName, TArray<FString>& Problems)
	{
		const FString Table = TEXT("skills");
		if (!ReadName(Row, TEXT("skill_id"), Out.SkillId, Table, TEXT("skill_id"), Problems))
		{
			return false;
		}
		RowName = Out.SkillId.ToString();
		bool bOk = ReadName(Row, TEXT("profession_id"), Out.ProfessionId, Table, RowName, Problems);
		bOk = ReadInt(Row, TEXT("max_rank"), Out.MaxRank, Table, RowName, Problems) && bOk;
		bOk = ReadName(Row, TEXT("slot_eligibility"), Out.SlotEligibility, Table, RowName, Problems) && bOk;
		bOk = ReadName(Row, TEXT("attack_id"), Out.AttackId, Table, RowName, Problems) && bOk;
		bOk = ReadBool(Row, TEXT("attack_pending"), Out.AttackPending, Table, RowName, Problems) && bOk;
		bOk = ReadNameMap(Row, TEXT("effect_parameters"), Out.EffectParameters, Table, RowName, Problems) && bOk;
		bOk = ReadString(Row, TEXT("icon_path"), Out.IconPath, Table, RowName, Problems) && bOk;
		bOk = ReadString(Row, TEXT("montage_path"), Out.MontagePath, Table, RowName, Problems) && bOk;
		bOk = ReadString(Row, TEXT("vfx_path"), Out.VfxPath, Table, RowName, Problems) && bOk;
		bOk = ReadString(Row, TEXT("sound_path"), Out.SoundPath, Table, RowName, Problems) && bOk;
		bOk = ReadBool(Row, TEXT("placeholder"), Out.Placeholder, Table, RowName, Problems) && bOk;

		// effect_policy: token -> enum, whitelist = implemented set only
		FString PolicyText;
		if (!ReadString(Row, TEXT("effect_policy"), PolicyText, Table, RowName, Problems))
		{
			return false;
		}
		if (PolicyText == TEXT("SingleMeleeHit"))
		{
			Out.EffectPolicy = EProfessionSkillEffectPolicy::SingleMeleeHit;
		}
		else if (PolicyText == TEXT("GroundArea"))
		{
			Out.EffectPolicy = EProfessionSkillEffectPolicy::GroundArea;
		}
		else if (PolicyText == TEXT("Projectile"))
		{
			Out.EffectPolicy = EProfessionSkillEffectPolicy::Projectile;
		}
		else if (PolicyText == TEXT("SelfBuff"))
		{
			Out.EffectPolicy = EProfessionSkillEffectPolicy::SelfBuff;
		}
		else
		{
			Problems.Add(FString::Printf(TEXT("skills[%s]: effect_policy '%s' is not in the implemented whitelist (SingleMeleeHit|GroundArea|Projectile|SelfBuff)"), *RowName, *PolicyText));
			bOk = false;
		}
		return bOk;
	}

	static bool ParseSkillLevelRow(const TSharedPtr<FCombatJsonValue>& Row, FSkillLevelRow& Out, FString& RowName, TArray<FString>& Problems)
	{
		const FString Table = TEXT("skill_levels");
		bool bOk = ReadName(Row, TEXT("skill_id"), Out.SkillId, Table, TEXT("skill_id"), Problems);
		RowName = Out.SkillId.ToString();
		bOk = ReadInt(Row, TEXT("rank"), Out.Rank, Table, RowName, Problems) && bOk;
		RowName = FString::Printf(TEXT("%s@r%d"), *RowName, Out.Rank);
		bOk = ReadInt(Row, TEXT("required_character_level"), Out.RequiredCharacterLevel, Table, RowName, Problems) && bOk;
		bOk = ReadInt(Row, TEXT("sp_cost"), Out.SPCost, Table, RowName, Problems) && bOk;
		bOk = ReadFloat(Row, TEXT("cooldown_s"), Out.CooldownS, Table, RowName, Problems) && bOk;
		bOk = ReadInt(Row, TEXT("mp_cost"), Out.MPCost, Table, RowName, Problems) && bOk;
		bOk = ReadFloat(Row, TEXT("damage_coefficient"), Out.DamageCoefficient, Table, RowName, Problems) && bOk;
		bOk = ReadNameMap(Row, TEXT("effect_parameters"), Out.EffectParameters, Table, RowName, Problems) && bOk;
		return bOk;
	}

	static bool ParseLoadoutRow(const TSharedPtr<FCombatJsonValue>& Row, FLoadoutRow& Out, FString& RowName, TArray<FString>& Problems)
	{
		const FString Table = TEXT("loadouts");
		bool bOk = ReadName(Row, TEXT("profession_id"), Out.ProfessionId, Table, TEXT("profession_id"), Problems);
		RowName = Out.ProfessionId.ToString();
		bOk = ReadInt(Row, TEXT("slot_index"), Out.SlotIndex, Table, RowName, Problems) && bOk;
		RowName = FString::Printf(TEXT("%s@%d"), *RowName, Out.SlotIndex);
		bOk = ReadName(Row, TEXT("skill_id"), Out.SkillId, Table, RowName, Problems) && bOk;
		return bOk;
	}

	// -- one table document: schema_version + table name + rows array ------------

	static bool ParseTableDocument(const FString& Table, const FString& Text, FString& OutSchemaError,
		TArray<TSharedPtr<FCombatJsonValue>>& OutRows)
	{
		TSharedPtr<FCombatJsonValue> Root;
		FString Error;
		if (!FCombatJsonParser::Parse(Text, Root, Error))
		{
			OutSchemaError = FString::Printf(TEXT("%s: JSON parse failed: %s"), *Table, *Error);
			return false;
		}
		const TSharedPtr<FCombatJsonValue>* Version = Root->Object.Find(TEXT("schema_version"));
		if (!Version || (*Version)->Kind != FCombatJsonValue::EKind::Number || !IsIntegral((*Version)->Number) ||
			static_cast<int32>((*Version)->Number) != 1)
		{
			OutSchemaError = FString::Printf(TEXT("%s: schema_version must be the integer 1"), *Table);
			return false;
		}
		const TSharedPtr<FCombatJsonValue>* TableName = Root->Object.Find(TEXT("table"));
		if (!TableName || (*TableName)->Kind != FCombatJsonValue::EKind::String || (*TableName)->String != Table)
		{
			OutSchemaError = FString::Printf(TEXT("%s: field 'table' is missing or does not match the file name"), *Table);
			return false;
		}
		const TSharedPtr<FCombatJsonValue>* Rows = Root->Object.Find(TEXT("rows"));
		if (!Rows || (*Rows)->Kind != FCombatJsonValue::EKind::Array)
		{
			OutSchemaError = FString::Printf(TEXT("%s: field 'rows' is missing or not an array"), *Table);
			return false;
		}
		OutRows = (*Rows)->Array;
		return true;
	}
}

// ===========================================================================
// Public entries
// ===========================================================================

bool ParseProfessionSourceFromText(const TMap<FString, FString>& TableTexts,
	const TSet<FName>& KnownCombatAttackIds, FProfessionParsedSource& OutSource)
{
	using namespace UE::UEMMO::Profession;

	OutSource = FProfessionParsedSource();
	OutSource.KnownCombatAttackIds = KnownCombatAttackIds;
	TArray<FString>& Problems = OutSource.Problems;

	// Registry: a table key outside the known set is refused (never silently
	// ignored) - the text path mirrors the directory stray-file contract.
	{
		static const TCHAR* KnownKeys[] = {
			TEXT("manifest"), TEXT("professions"), TEXT("level_curves"), TEXT("skills"),
			TEXT("skill_levels"), TEXT("loadouts")
		};
		for (const TPair<FString, FString>& Pair : TableTexts)
		{
			bool bKnown = false;
			for (const TCHAR* Known : KnownKeys)
			{
				bKnown = bKnown || Pair.Key == Known;
			}
			if (!bKnown)
			{
				Problems.Add(FString::Printf(TEXT("%s: unknown table is not registered by manifest.json"), *Pair.Key));
			}
		}
	}

	// Manifest: required-table registry (same contract as Data/CombatSystem).
	static const TCHAR* KnownTables[] = {
		TEXT("professions"), TEXT("level_curves"), TEXT("skills"), TEXT("skill_levels"), TEXT("loadouts")
	};
	const FString* ManifestText = TableTexts.Find(TEXT("manifest"));
	if (!ManifestText)
	{
		Problems.Add(TEXT("manifest: manifest.json is required in a profession data directory"));
		return false;
	}
	TSharedPtr<FCombatJsonValue> ManifestRoot;
	FString Error;
	if (!FCombatJsonParser::Parse(*ManifestText, ManifestRoot, Error))
	{
		Problems.Add(FString::Printf(TEXT("manifest: JSON parse failed: %s"), *Error));
		return false;
	}
	const TSharedPtr<FCombatJsonValue>* Required = ManifestRoot->Object.Find(TEXT("required_tables"));
	if (!Required || (*Required)->Kind != FCombatJsonValue::EKind::Array)
	{
		Problems.Add(TEXT("manifest: field 'required_tables' is missing or not an array"));
		return false;
	}
	TSet<FString> RequiredSet;
	for (const TSharedPtr<FCombatJsonValue>& Item : (*Required)->Array)
	{
		if (!Item || Item->Kind != FCombatJsonValue::EKind::String)
		{
			Problems.Add(TEXT("manifest: required_tables contains a non-string entry"));
			return false;
		}
		RequiredSet.Add(Item->String);
	}
	for (const TCHAR* Known : KnownTables)
	{
		if (!RequiredSet.Contains(Known))
		{
			Problems.Add(FString::Printf(TEXT("manifest: required table '%s' is not registered"), Known));
		}
	}
	for (const FString& Listed : RequiredSet)
	{
		bool bKnown = false;
		for (const TCHAR* Known : KnownTables)
		{
			bKnown = bKnown || Listed == Known;
		}
		if (!bKnown)
		{
			Problems.Add(FString::Printf(TEXT("manifest: unknown required table '%s'"), *Listed));
		}
	}

	// professions
	{
		TArray<TSharedPtr<FCombatJsonValue>> Rows;
		if (!ParseTableDocument(TEXT("professions"), TableTexts.FindChecked(TEXT("professions")), Error, Rows))
		{
			Problems.Add(Error);
			return false;
		}
		for (const TSharedPtr<FCombatJsonValue>& Row : Rows)
		{
			FProfessionRow Parsed;
			FString RowName;
			if (!ParseProfessionRow(Row, Parsed, RowName, Problems))
			{
				continue;
			}
			if (OutSource.Tables.Professions.Contains(Parsed.ProfessionId))
			{
				Problems.Add(FString::Printf(TEXT("professions[%s]: duplicate profession_id"), *RowName));
				continue;
			}
			OutSource.Tables.Professions.Add(Parsed.ProfessionId, Parsed);
		}
	}

	// level_curves (rows appended, sorted by level below)
	{
		TArray<TSharedPtr<FCombatJsonValue>> Rows;
		if (!ParseTableDocument(TEXT("level_curves"), TableTexts.FindChecked(TEXT("level_curves")), Error, Rows))
		{
			Problems.Add(Error);
			return false;
		}
		for (const TSharedPtr<FCombatJsonValue>& Row : Rows)
		{
			FProfessionLevelCurveRow Parsed;
			FString RowName;
			if (!ParseLevelCurveRow(Row, Parsed, RowName, Problems))
			{
				continue;
			}
			OutSource.Tables.LevelCurves.FindOrAdd(Parsed.CurveId).Add(Parsed);
		}
		for (TPair<FName, TArray<FProfessionLevelCurveRow>>& Curve : OutSource.Tables.LevelCurves)
		{
			Curve.Value.Sort([](const FProfessionLevelCurveRow& A, const FProfessionLevelCurveRow& B)
			{
				return A.Level < B.Level;
			});
		}
	}

	// skills
	{
		TArray<TSharedPtr<FCombatJsonValue>> Rows;
		if (!ParseTableDocument(TEXT("skills"), TableTexts.FindChecked(TEXT("skills")), Error, Rows))
		{
			Problems.Add(Error);
			return false;
		}
		for (const TSharedPtr<FCombatJsonValue>& Row : Rows)
		{
			FSkillRow Parsed;
			FString RowName;
			if (!ParseSkillRow(Row, Parsed, RowName, Problems))
			{
				continue;
			}
			if (OutSource.Tables.Skills.Contains(Parsed.SkillId))
			{
				Problems.Add(FString::Printf(TEXT("skills[%s]: duplicate skill_id"), *RowName));
				continue;
			}
			OutSource.Tables.Skills.Add(Parsed.SkillId, Parsed);
		}
	}

	// skill_levels (rows appended, sorted by rank below)
	{
		TArray<TSharedPtr<FCombatJsonValue>> Rows;
		if (!ParseTableDocument(TEXT("skill_levels"), TableTexts.FindChecked(TEXT("skill_levels")), Error, Rows))
		{
			Problems.Add(Error);
			return false;
		}
		for (const TSharedPtr<FCombatJsonValue>& Row : Rows)
		{
			FSkillLevelRow Parsed;
			FString RowName;
			if (!ParseSkillLevelRow(Row, Parsed, RowName, Problems))
			{
				continue;
			}
			OutSource.Tables.SkillLevels.FindOrAdd(Parsed.SkillId).Add(Parsed);
		}
		for (TPair<FName, TArray<FSkillLevelRow>>& Levels : OutSource.Tables.SkillLevels)
		{
			Levels.Value.Sort([](const FSkillLevelRow& A, const FSkillLevelRow& B)
			{
				return A.Rank < B.Rank;
			});
		}
	}

	// loadouts
	{
		TArray<TSharedPtr<FCombatJsonValue>> Rows;
		if (!ParseTableDocument(TEXT("loadouts"), TableTexts.FindChecked(TEXT("loadouts")), Error, Rows))
		{
			Problems.Add(Error);
			return false;
		}
		for (const TSharedPtr<FCombatJsonValue>& Row : Rows)
		{
			FLoadoutRow Parsed;
			FString RowName;
			if (!ParseLoadoutRow(Row, Parsed, RowName, Problems))
			{
				continue;
			}
			OutSource.Tables.Loadouts.Add(Parsed);
		}
	}

	if (Problems.Num() > 0)
	{
		return false;
	}

	// -- cross-table gate --------------------------------------------------------
	{
		// professions: curve reference + placeholder path contract + prototype presence
		bool bHasPrototype = false;
		for (const TPair<FName, FProfessionRow>& Pair : OutSource.Tables.Professions)
		{
			const FProfessionRow& Row = Pair.Value;
			const FString RowName = Row.ProfessionId.ToString();
			if (!OutSource.Tables.LevelCurves.Contains(Row.CurveId))
			{
				Problems.Add(FString::Printf(TEXT("professions[%s]: curve_id '%s' has no level_curves rows"), *RowName, *Row.CurveId.ToString()));
			}
			const bool bHasPaths = !Row.MeshPath.IsEmpty() || !Row.SkeletonPath.IsEmpty();
			if (bHasPaths)
			{
				if (Row.AssetPlaceholder)
				{
					Problems.Add(FString::Printf(TEXT("professions[%s]: non-empty mesh/skeleton paths require asset_placeholder=false"), *RowName));
				}
				if ((!Row.MeshPath.IsEmpty() && !Row.MeshPath.StartsWith(TEXT("/Game/UEMMO/"))) ||
					(!Row.SkeletonPath.IsEmpty() && !Row.SkeletonPath.StartsWith(TEXT("/Game/UEMMO/"))))
				{
					Problems.Add(FString::Printf(TEXT("professions[%s]: mesh/skeleton paths must live under /Game/UEMMO/"), *RowName));
				}
			}
			else if (!Row.AssetPlaceholder)
			{
				Problems.Add(FString::Printf(TEXT("professions[%s]: empty mesh/skeleton paths require asset_placeholder=true"), *RowName));
			}
			if (Row.AllowedWeaponModes.Num() == 0)
			{
				Problems.Add(FString::Printf(TEXT("professions[%s]: allowed_weapon_modes is empty"), *RowName));
			}
			for (const TPair<FName, int32>& Starting : Row.StartingSkillRanks)
			{
				const FSkillRow* Skill = OutSource.Tables.Skills.Find(Starting.Key);
				if (!Skill)
				{
					Problems.Add(FString::Printf(TEXT("professions[%s]: starting skill '%s' does not exist"), *RowName, *Starting.Key.ToString()));
					continue;
				}
				if (Skill->ProfessionId != Row.ProfessionId)
				{
					Problems.Add(FString::Printf(TEXT("professions[%s]: starting skill '%s' belongs to another profession"), *RowName, *Starting.Key.ToString()));
				}
				if (Starting.Value < 1 || Starting.Value > Skill->MaxRank)
				{
					Problems.Add(FString::Printf(TEXT("professions[%s]: starting skill '%s' rank %d outside 1..%d"), *RowName, *Starting.Key.ToString(), Starting.Value, Skill->MaxRank));
				}
				else if (!OutSource.Tables.SkillLevels.Contains(Starting.Key) ||
					OutSource.Tables.SkillLevels[Starting.Key].Num() < Starting.Value)
				{
					Problems.Add(FString::Printf(TEXT("professions[%s]: starting skill '%s' has no rank %d row"), *RowName, *Starting.Key.ToString(), Starting.Value));
				}
			}
			bHasPrototype = bHasPrototype || Row.ProfessionId == TEXT("prototype");
		}
		if (!bHasPrototype)
		{
			Problems.Add(TEXT("professions: the legacy-compatibility profession 'prototype' is required"));
		}

		// level_curves: continuity, max-level contract, bounds, prototype equivalence
		for (const TPair<FName, TArray<FProfessionLevelCurveRow>>& Curve : OutSource.Tables.LevelCurves)
		{
			const FString CurveName = Curve.Key.ToString();
			const TArray<FProfessionLevelCurveRow>& Rows = Curve.Value;
			if (Rows.Num() == 0)
			{
				Problems.Add(FString::Printf(TEXT("level_curves[%s]: no rows"), *CurveName));
				continue;
			}
			const int32 MaxLevel = Rows.Last().Level;
			if (Rows[0].Level != 1 || MaxLevel > MaxLevelCap)
			{
				Problems.Add(FString::Printf(TEXT("level_curves[%s]: levels must start at 1 and stay <= %d"), *CurveName, MaxLevelCap));
				continue;
			}
			for (int32 Index = 0; Index < Rows.Num(); ++Index)
			{
				const FProfessionLevelCurveRow& Row = Rows[Index];
				const FString RowName = FString::Printf(TEXT("%s@%d"), *CurveName, Row.Level);
				if (Row.Level != Index + 1)
				{
					Problems.Add(FString::Printf(TEXT("level_curves[%s]: level sequence is not continuous at %d"), *CurveName, Row.Level));
				}
				if (Row.XPToNext < 0 || Row.BaseMaxHP < 0 || Row.BaseAttack < 0 || Row.BaseDefense < 0 ||
					Row.BaseMaxMP < 0 || Row.BaseMPRegen < 0 || Row.SkillPoints < 0)
				{
					Problems.Add(FString::Printf(TEXT("level_curves[%s]: negative value"), *RowName));
				}
				if (Row.Level == MaxLevel && Row.XPToNext != 0)
				{
					Problems.Add(FString::Printf(TEXT("level_curves[%s]: xp_to_next must be 0 at the max level"), *RowName));
				}
				if (Row.Level < MaxLevel && Row.XPToNext <= 0)
				{
					Problems.Add(FString::Printf(TEXT("level_curves[%s]: xp_to_next must be > 0 below the max level"), *RowName));
				}
			}
			if (Curve.Key == TEXT("prototype_curve"))
			{
				if (Rows.Num() != FExperienceCurve::MaxLevel)
				{
					Problems.Add(FString::Printf(TEXT("level_curves[prototype_curve]: must cover exactly levels 1..%d (found %d rows)"), FExperienceCurve::MaxLevel, Rows.Num()));
				}
				for (const FProfessionLevelCurveRow& Row : Rows)
				{
					const FString RowName = FString::Printf(TEXT("prototype_curve@%d"), Row.Level);
					if (Row.Level >= FExperienceCurve::MinLevel && Row.Level <= FExperienceCurve::MaxLevel)
					{
						if (Row.XPToNext != FExperienceCurve::GetNextLevelXP(Row.Level) ||
							Row.BaseMaxHP != FExperienceCurve::GetMaxHPForLevel(Row.Level) ||
							Row.BaseAttack != FExperienceCurve::GetAttackForLevel(Row.Level) ||
							Row.BaseDefense != FExperienceCurve::GetDefenseForLevel(Row.Level))
						{
							Problems.Add(FString::Printf(TEXT("level_curves[%s]: diverges from FExperienceCurve"), *RowName));
						}
					}
					if (Row.BaseMaxMP != 0 || Row.BaseMPRegen != 0 || Row.SkillPoints != 0)
					{
						Problems.Add(FString::Printf(TEXT("level_curves[%s]: the legacy prototype curve must carry zero MP/SP columns"), *RowName));
					}
				}
			}
		}

		// skills: profession reference, rank bounds, policy bounds, pending contract, placeholder paths
		for (const TPair<FName, FSkillRow>& Pair : OutSource.Tables.Skills)
		{
			const FSkillRow& Row = Pair.Value;
			const FString RowName = Row.SkillId.ToString();
			if (!OutSource.Tables.Professions.Contains(Row.ProfessionId))
			{
				Problems.Add(FString::Printf(TEXT("skills[%s]: profession_id '%s' does not exist"), *RowName, *Row.ProfessionId.ToString()));
			}
			if (Row.MaxRank < 1 || Row.MaxRank > MaxRankCap)
			{
				Problems.Add(FString::Printf(TEXT("skills[%s]: max_rank %d outside 1..%d"), *RowName, Row.MaxRank, MaxRankCap));
			}
			if (Row.SlotEligibility != TEXT("hotbar") && Row.SlotEligibility != TEXT("launcher"))
			{
				Problems.Add(FString::Printf(TEXT("skills[%s]: slot_eligibility '%s' is not hotbar|launcher"), *RowName, *Row.SlotEligibility.ToString()));
			}
			if (Row.AttackPending)
			{
				if (Row.SlotEligibility == TEXT("launcher"))
				{
					Problems.Add(FString::Printf(TEXT("skills[%s]: a launcher-eligible skill must not be attack_pending"), *RowName));
				}
			}
			else if (!OutSource.KnownCombatAttackIds.Contains(Row.AttackId))
			{
				Problems.Add(FString::Printf(TEXT("skills[%s]: attack_id '%s' does not exist in Data/combat-attacks.json"), *RowName, *Row.AttackId.ToString()));
			}
			const bool bAnyPath = !Row.IconPath.IsEmpty() || !Row.MontagePath.IsEmpty() ||
				!Row.VfxPath.IsEmpty() || !Row.SoundPath.IsEmpty();
			if (bAnyPath)
			{
				if (Row.Placeholder)
				{
					Problems.Add(FString::Printf(TEXT("skills[%s]: non-empty visual paths require placeholder=false"), *RowName));
				}
				const FString* Paths[] = { &Row.IconPath, &Row.MontagePath, &Row.VfxPath, &Row.SoundPath };
				for (const FString* Path : Paths)
				{
					if (!Path->IsEmpty() && !Path->StartsWith(TEXT("/Game/UEMMO/")))
					{
						Problems.Add(FString::Printf(TEXT("skills[%s]: visual paths must live under /Game/UEMMO/"), *RowName));
						break;
					}
				}
			}
			else if (!Row.Placeholder)
			{
				Problems.Add(FString::Printf(TEXT("skills[%s]: empty visual paths require placeholder=true"), *RowName));
			}
		}

		// skill_levels: rank continuity, bounds, profession curve gate
		for (const TPair<FName, TArray<FSkillLevelRow>>& Levels : OutSource.Tables.SkillLevels)
		{
			const FSkillRow* Skill = OutSource.Tables.Skills.Find(Levels.Key);
			const FString SkillName = Levels.Key.ToString();
			if (!Skill)
			{
				Problems.Add(FString::Printf(TEXT("skill_levels[%s]: skill_id does not exist"), *SkillName));
				continue;
			}
			const FProfessionRow* Owner = OutSource.Tables.Professions.Find(Skill->ProfessionId);
			int32 CurveMaxLevel = MaxLevelCap;
			if (Owner)
			{
				const TArray<FProfessionLevelCurveRow>* Curve = OutSource.Tables.LevelCurves.Find(Owner->CurveId);
				if (Curve && Curve->Num() > 0)
				{
					CurveMaxLevel = Curve->Last().Level;
				}
			}
			if (Skill->MaxRank < 1 || Levels.Value.Num() != Skill->MaxRank)
			{
				Problems.Add(FString::Printf(TEXT("skill_levels[%s]: rank rows must cover exactly 1..max_rank (%d rows for max_rank %d)"),
					*SkillName, Levels.Value.Num(), Skill->MaxRank));
			}
			for (int32 Index = 0; Index < Levels.Value.Num(); ++Index)
			{
				const FSkillLevelRow& Row = Levels.Value[Index];
				const FString RowName = FString::Printf(TEXT("%s@r%d"), *SkillName, Row.Rank);
				if (Row.Rank != Index + 1)
				{
					Problems.Add(FString::Printf(TEXT("skill_levels[%s]: rank sequence is not continuous at %d"), *SkillName, Row.Rank));
				}
				if (Row.RequiredCharacterLevel < 1 || Row.RequiredCharacterLevel > CurveMaxLevel)
				{
					Problems.Add(FString::Printf(TEXT("skill_levels[%s]: required_character_level %d outside 1..%d"), *RowName, Row.RequiredCharacterLevel, CurveMaxLevel));
				}
				if (Row.SPCost < 0 || Row.MPCost < 0)
				{
					Problems.Add(FString::Printf(TEXT("skill_levels[%s]: negative cost"), *RowName));
				}
				if (Row.CooldownS <= 0.f)
				{
					Problems.Add(FString::Printf(TEXT("skill_levels[%s]: cooldown_s must be > 0"), *RowName));
				}
				if (Row.DamageCoefficient <= 0.f)
				{
					Problems.Add(FString::Printf(TEXT("skill_levels[%s]: damage_coefficient must be > 0"), *RowName));
				}
			}
		}

		// loadouts: slot bounds, uniqueness, hotbar eligibility, profession match, rank-1 presence
		TSet<FString> SeenSlots;
		for (const FLoadoutRow& Row : OutSource.Tables.Loadouts)
		{
			const FString RowName = FString::Printf(TEXT("%s@%d"), *Row.ProfessionId.ToString(), Row.SlotIndex);
			if (Row.SlotIndex < 1 || Row.SlotIndex > 8)
			{
				Problems.Add(FString::Printf(TEXT("loadouts[%s]: slot_index outside 1..8"), *RowName));
				continue;
			}
			if (!SeenSlots.Contains(RowName))
			{
				SeenSlots.Add(RowName);
			}
			else
			{
				Problems.Add(FString::Printf(TEXT("loadouts[%s]: duplicate slot"), *RowName));
			}
			const FSkillRow* Skill = OutSource.Tables.Skills.Find(Row.SkillId);
			if (!Skill)
			{
				Problems.Add(FString::Printf(TEXT("loadouts[%s]: skill_id '%s' does not exist"), *RowName, *Row.SkillId.ToString()));
				continue;
			}
			if (Skill->ProfessionId != Row.ProfessionId)
			{
				Problems.Add(FString::Printf(TEXT("loadouts[%s]: skill '%s' belongs to another profession"), *RowName, *Row.SkillId.ToString()));
			}
			if (Skill->SlotEligibility != TEXT("hotbar"))
			{
				Problems.Add(FString::Printf(TEXT("loadouts[%s]: skill '%s' is not hotbar-eligible"), *RowName, *Row.SkillId.ToString()));
			}
			const TArray<FSkillLevelRow>* Levels = OutSource.Tables.SkillLevels.Find(Row.SkillId);
			if (!Levels || Levels->Num() == 0 || (*Levels)[0].Rank != 1)
			{
				Problems.Add(FString::Printf(TEXT("loadouts[%s]: skill '%s' has no rank-1 row"), *RowName, *Row.SkillId.ToString()));
			}
		}
	}

	OutSource.ConfigRevision = ComputeProfessionConfigRevision(OutSource.Tables);
	return Problems.Num() == 0;
}

bool LoadProfessionSourceFromDirectory(const FString& DataDir, FProfessionParsedSource& OutSource)
{
	TMap<FString, FString> TableTexts;
	TArray<FString> Problems;

	// manifest.json first (it gates which tables the directory enables)
	const FString ManifestPath = DataDir / TEXT("manifest.json");
	FString ManifestText;
	if (!FFileHelper::LoadFileToString(ManifestText, *ManifestPath))
	{
		OutSource.Problems.Add(FString::Printf(TEXT("manifest: cannot read %s"), *ManifestPath));
		return false;
	}
	TableTexts.Add(TEXT("manifest"), ManifestText);

	static const TCHAR* KnownTables[] = {
		TEXT("professions"), TEXT("level_curves"), TEXT("skills"), TEXT("skill_levels"), TEXT("loadouts")
	};
	for (const TCHAR* Known : KnownTables)
	{
		const FString Path = DataDir / (FString(Known) + TEXT(".json"));
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			Problems.Add(FString::Printf(TEXT("%s: cannot read table file"), *Path));
			continue;
		}
		TableTexts.Add(Known, Text);
	}

	// refuse unlisted stray json files (registry contract, never silently ignore)
	TArray<FString> FoundFiles;
	IFileManager::Get().FindFiles(FoundFiles, *(DataDir / TEXT("*.json")), true, false);
	for (const FString& File : FoundFiles)
	{
		const FString Base = FPaths::GetBaseFilename(File);
		const bool bKnown = Base == TEXT("manifest") || TableTexts.Contains(Base);
		if (!bKnown)
		{
			Problems.Add(FString::Printf(TEXT("%s: unknown json file is not registered by manifest.json"), *(DataDir / File)));
		}
	}

	// Data/combat-attacks.json: the pending-attack gate needs its attack ids
	TSet<FName> KnownAttacks;
	FString AttacksText;
	if (FFileHelper::LoadFileToString(AttacksText, *(FPaths::ProjectDir() / TEXT("Data/combat-attacks.json"))))
	{
		TSharedPtr<FCombatJsonValue> Root;
		FString Error;
		if (FCombatJsonParser::Parse(AttacksText, Root, Error))
		{
			// "attacks" is an ARRAY of attack objects, each carrying attack_id.
			if (const TSharedPtr<FCombatJsonValue>* Attacks = Root->Object.Find(TEXT("attacks")))
			{
				if ((*Attacks)->Kind == FCombatJsonValue::EKind::Array)
				{
					for (const TSharedPtr<FCombatJsonValue>& Attack : (*Attacks)->Array)
					{
						if (Attack && Attack->Kind == FCombatJsonValue::EKind::Object)
						{
							if (const TSharedPtr<FCombatJsonValue>* Id = Attack->Object.Find(TEXT("attack_id")))
							{
								if (*Id && (*Id)->Kind == FCombatJsonValue::EKind::String)
								{
									KnownAttacks.Add(FName((*Id)->String));
								}
							}
						}
					}
				}
			}
		}
		else
		{
			Problems.Add(FString::Printf(TEXT("Data/combat-attacks.json: parse failed: %s"), *Error));
		}
	}
	else
	{
		Problems.Add(TEXT("Data/combat-attacks.json: cannot read (the pending-attack gate needs it)"));
	}

	if (Problems.Num() > 0)
	{
		OutSource.Problems = Problems;
		return false;
	}

	if (!ParseProfessionSourceFromText(TableTexts, KnownAttacks, OutSource))
	{
		return false;
	}
	return OutSource.IsValid();
}

namespace
{
	// FNV-1a over UTF-8 text (the M5 combat revision precedent, CombatCatalog.cpp).
	constexpr uint64 RevisionHashOffsetBasis = 14695981039346656037ull;
	constexpr uint64 RevisionHashPrime = 1099511628211ull;

	uint64 RevisionHashBytes(uint64 Hash, const uint8* Data, int64 Size)
	{
		for (int64 Index = 0; Index < Size; ++Index)
		{
			Hash ^= static_cast<uint64>(Data[Index]);
			Hash *= RevisionHashPrime;
		}
		return Hash;
	}
}

FString ComputeProfessionConfigRevision(const FProfessionTableSet& Tables)
{
	// Deterministic canonical text: domain tag, then rows sorted by id in a
	// fixed field order. A 64-bit FNV-1a content digest (no security use),
	// rendered as 16 lowercase hex digits.
	uint64 Hash = RevisionHashOffsetBasis;
	auto HashText = [&Hash](const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		Hash = RevisionHashBytes(Hash, reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	};
	HashText(TEXT("UEMMO.M7.ProfessionCatalog.v1"));

	TArray<FName> Ids;
	Ids.Reset(Tables.Professions.Num());
	for (const TPair<FName, FProfessionRow>& Pair : Tables.Professions)
	{
		Ids.Add(Pair.Key);
	}
	Ids.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	for (const FName& Id : Ids)
	{
		const FProfessionRow& Row = Tables.Professions[Id];
		HashText(FString::Printf(TEXT("P|%s|%s|%s|%s|%s|%s|%d|%s"), *Id.ToString(), *Row.DisplayName,
			*Row.CurveId.ToString(), *Row.MeshPath, *Row.SkeletonPath, *Row.LocomotionSetId.ToString(),
			Row.AssetPlaceholder ? 1 : 0, *Row.PresentationId));
		TArray<FName> Starting;
		for (const TPair<FName, int32>& StartingPair : Row.StartingSkillRanks)
		{
			Starting.Add(StartingPair.Key);
		}
		Starting.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
		for (const FName& Skill : Starting)
		{
			HashText(FString::Printf(TEXT("P SR|%s|%d"), *Skill.ToString(), Row.StartingSkillRanks[Skill]));
		}
		HashText(FString::Printf(TEXT("P WM|%d"), Row.AllowedWeaponModes.Num()));
	}

	Ids.Reset(Tables.LevelCurves.Num());
	for (const TPair<FName, TArray<FProfessionLevelCurveRow>>& Pair : Tables.LevelCurves)
	{
		Ids.Add(Pair.Key);
	}
	Ids.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	for (const FName& Id : Ids)
	{
		for (const FProfessionLevelCurveRow& Row : Tables.LevelCurves[Id])
		{
			HashText(FString::Printf(TEXT("C|%s|%d|%d|%d|%d|%d|%d|%d|%d"), *Id.ToString(), Row.Level,
				Row.XPToNext, Row.BaseMaxHP, Row.BaseAttack, Row.BaseDefense, Row.BaseMaxMP,
				Row.BaseMPRegen, Row.SkillPoints));
		}
	}

	Ids.Reset(Tables.Skills.Num());
	for (const TPair<FName, FSkillRow>& Pair : Tables.Skills)
	{
		Ids.Add(Pair.Key);
	}
	Ids.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	for (const FName& Id : Ids)
	{
		const FSkillRow& Row = Tables.Skills[Id];
		HashText(FString::Printf(TEXT("S|%s|%s|%d|%s|%d|%s|%d|%d"), *Id.ToString(), *Row.ProfessionId.ToString(),
			Row.MaxRank, *Row.SlotEligibility.ToString(), static_cast<int32>(Row.EffectPolicy),
			*Row.AttackId.ToString(), Row.AttackPending ? 1 : 0, Row.Placeholder ? 1 : 0));
		TArray<FName> Params;
		for (const TPair<FName, float>& Param : Row.EffectParameters)
		{
			Params.Add(Param.Key);
		}
		Params.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
		for (const FName& Key : Params)
		{
			HashText(FString::Printf(TEXT("S EP|%s|%.6f"), *Key.ToString(), Row.EffectParameters[Key]));
		}
		HashText(FString::Printf(TEXT("S P|%s|%s|%s|%s"), *Row.IconPath, *Row.MontagePath, *Row.VfxPath, *Row.SoundPath));
	}

	Ids.Reset(Tables.SkillLevels.Num());
	for (const TPair<FName, TArray<FSkillLevelRow>>& Pair : Tables.SkillLevels)
	{
		Ids.Add(Pair.Key);
	}
	Ids.Sort([](const FName& A, const FName& B) { return A.LexicalLess(B); });
	for (const FName& Id : Ids)
	{
		for (const FSkillLevelRow& Row : Tables.SkillLevels[Id])
		{
			HashText(FString::Printf(TEXT("L|%s|%d|%d|%d|%.3f|%d|%.3f"), *Id.ToString(), Row.Rank,
				Row.RequiredCharacterLevel, Row.SPCost, Row.CooldownS, Row.MPCost, Row.DamageCoefficient));
		}
	}

	for (const FLoadoutRow& Row : Tables.Loadouts)
	{
		HashText(FString::Printf(TEXT("O|%s|%d|%s"), *Row.ProfessionId.ToString(), Row.SlotIndex, *Row.SkillId.ToString()));
	}

	return FString::Printf(TEXT("%016llx"), Hash);
}

void FProfessionTableSet::Reset()
{
	Professions.Reset();
	LevelCurves.Reset();
	Skills.Reset();
	SkillLevels.Reset();
	Loadouts.Reset();
}
