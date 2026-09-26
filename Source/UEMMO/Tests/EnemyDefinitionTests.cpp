// M2-001: validates the textual melee-enemy source data (Data/enemies.json)
// against the interface contract (section 7) and the M2-001 single-entry
// validator, and pins the spawn-grace semantics on an explicitly injected game
// clock. Pure checks: this file only reads JSON text and NewObject data
// assets; it never touches UE assets, worlds or wall clocks.
//
// Parsing note (same lesson as M1-008): the engine's Json module headers
// compile through Engine's public dependency, but UnrealBuildTool does not put
// the Json import library on the UEMMO link line, so FJsonValue/FJsonObject
// symbols fail to link (LNK2019). UEMMO.Build.cs is outside this task's file
// range, so the tests use the small hand-rolled parser below (objects, arrays,
// strings with escapes, numbers, booleans, null) built on Core types only.

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "UObject/UObjectGlobals.h"

#include "../Enemy/EnemyDefinition.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M2_001
{
	/** Minimal JSON value: just enough shape for the enemy data files. */
	class FEnemyJsonValue
	{
	public:
		enum class EKind
		{
			Null,
			Boolean,
			Number,
			String,
			Array,
			Object
		};

		EKind Kind = EKind::Null;
		bool Boolean = false;
		double Number = 0.0;
		FString String;
		TArray<TSharedPtr<FEnemyJsonValue>> Array;
		TMap<FString, TSharedPtr<FEnemyJsonValue>> Object;
	};

	/** Recursive descent JSON parser (RFC 8259 subset; \u escapes decode to one TCHAR). */
	class FEnemyJsonParser
	{
	public:
		static bool Parse(const FString& Text, TSharedPtr<FEnemyJsonValue>& OutRoot, FString& OutError)
		{
			FEnemyJsonParser Parser(Text);
			Parser.SkipWhitespace();
			TSharedPtr<FEnemyJsonValue> Root = Parser.ParseValue(OutError);
			if (!Root.IsValid())
			{
				return false;
			}
			Parser.SkipWhitespace();
			if (!Parser.AtEnd())
			{
				OutError = FString::Printf(TEXT("unexpected trailing characters after the JSON value at offset %d"),
					Parser.Position);
				return false;
			}
			OutRoot = Root;
			return true;
		}

	private:
		explicit FEnemyJsonParser(const FString& InText)
			: Source(InText)
			, Position(0)
		{
		}

		const FString& Source;
		int32 Position;

		bool AtEnd() const { return Position >= Source.Len(); }
		TCHAR Peek() const { return Source[Position]; }
		void Advance() { ++Position; }
		void SkipWhitespace()
		{
			while (!AtEnd())
			{
				const TCHAR Character = Peek();
				if (Character != TEXT(' ') && Character != TEXT('\t') && Character != TEXT('\r') && Character != TEXT('\n'))
				{
					break;
				}
				++Position;
			}
		}
		bool Consume(TCHAR Character)
		{
			if (!AtEnd() && Peek() == Character)
			{
				++Position;
				return true;
			}
			return false;
		}
		bool Expect(TCHAR Character, const TCHAR* What, FString& OutError)
		{
			if (Consume(Character))
			{
				return true;
			}
			OutError = FString::Printf(TEXT("expected '%c' %s at offset %d"), Character, What, Position);
			return false;
		}
		bool MatchLiteral(const TCHAR* Literal, int32 Length)
		{
			if (Position + Length > Source.Len())
			{
				return false;
			}
			return Source.Mid(Position, Length).Equals(Literal);
		}

		/** Returns the hex digit value 0..15, or -1 when the character is not a hex digit. */
		static int32 HexDigitValue(TCHAR Character)
		{
			if (Character >= TEXT('0') && Character <= TEXT('9'))
			{
				return static_cast<int32>(Character - TEXT('0'));
			}
			if (Character >= TEXT('a') && Character <= TEXT('f'))
			{
				return 10 + static_cast<int32>(Character - TEXT('a'));
			}
			if (Character >= TEXT('A') && Character <= TEXT('F'))
			{
				return 10 + static_cast<int32>(Character - TEXT('A'));
			}
			return -1;
		}

		TSharedPtr<FEnemyJsonValue> ParseValue(FString& OutError)
		{
			SkipWhitespace();
			if (AtEnd())
			{
				OutError = FString::Printf(TEXT("unexpected end of input at offset %d"), Position);
				return nullptr;
			}
			switch (Peek())
			{
			case TEXT('{'): return ParseObject(OutError);
			case TEXT('['): return ParseArray(OutError);
			case TEXT('"'): return ParseString(OutError);
			case TEXT('t'):
			case TEXT('f'): return ParseBoolean(OutError);
			case TEXT('n'): return ParseNull(OutError);
			default: return ParseNumber(OutError);
			}
		}

		TSharedPtr<FEnemyJsonValue> ParseObject(FString& OutError)
		{
			if (!Expect(TEXT('{'), TEXT("to start an object"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FEnemyJsonValue> Value = MakeShared<FEnemyJsonValue>();
			Value->Kind = FEnemyJsonValue::EKind::Object;
			SkipWhitespace();
			if (Consume(TEXT('}')))
			{
				return Value;
			}
			while (true)
			{
				SkipWhitespace();
				TSharedPtr<FEnemyJsonValue> Key = ParseString(OutError);
				if (!Key.IsValid())
				{
					return nullptr;
				}
				SkipWhitespace();
				if (!Expect(TEXT(':'), TEXT("between object key and value"), OutError))
				{
					return nullptr;
				}
				TSharedPtr<FEnemyJsonValue> Element = ParseValue(OutError);
				if (!Element.IsValid())
				{
					return nullptr;
				}
				Value->Object.Add(Key->String, Element);
				SkipWhitespace();
				if (Consume(TEXT(',')))
				{
					continue;
				}
				if (!Expect(TEXT('}'), TEXT("to end the object"), OutError))
				{
					return nullptr;
				}
				return Value;
			}
		}

		TSharedPtr<FEnemyJsonValue> ParseArray(FString& OutError)
		{
			if (!Expect(TEXT('['), TEXT("to start an array"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FEnemyJsonValue> Value = MakeShared<FEnemyJsonValue>();
			Value->Kind = FEnemyJsonValue::EKind::Array;
			SkipWhitespace();
			if (Consume(TEXT(']')))
			{
				return Value;
			}
			while (true)
			{
				TSharedPtr<FEnemyJsonValue> Element = ParseValue(OutError);
				if (!Element.IsValid())
				{
					return nullptr;
				}
				Value->Array.Add(Element);
				SkipWhitespace();
				if (Consume(TEXT(',')))
				{
					continue;
				}
				if (!Expect(TEXT(']'), TEXT("to end the array"), OutError))
				{
					return nullptr;
				}
				return Value;
			}
		}

		TSharedPtr<FEnemyJsonValue> ParseString(FString& OutError)
		{
			if (!Expect(TEXT('"'), TEXT("to start a string"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FEnemyJsonValue> Value = MakeShared<FEnemyJsonValue>();
			Value->Kind = FEnemyJsonValue::EKind::String;
			FString Decoded;
			while (true)
			{
				if (AtEnd())
				{
					OutError = FString::Printf(TEXT("unterminated string at offset %d"), Position);
					return nullptr;
				}
				const TCHAR Character = Peek();
				if (Character == TEXT('"'))
				{
					Advance();
					break;
				}
				if (Character != TEXT('\\'))
				{
					Decoded.AppendChar(Character);
					Advance();
					continue;
				}
				Advance(); // consume the backslash
				if (AtEnd())
				{
					OutError = FString::Printf(TEXT("unterminated escape at offset %d"), Position);
					return nullptr;
				}
				const TCHAR Escape = Peek();
				Advance();
				switch (Escape)
				{
				case TEXT('"'): Decoded.AppendChar(TEXT('"')); break;
				case TEXT('\\'): Decoded.AppendChar(TEXT('\\')); break;
				case TEXT('/'): Decoded.AppendChar(TEXT('/')); break;
				case TEXT('b'): Decoded.AppendChar(TEXT('\b')); break;
				case TEXT('f'): Decoded.AppendChar(TEXT('\f')); break;
				case TEXT('n'): Decoded.AppendChar(TEXT('\n')); break;
				case TEXT('r'): Decoded.AppendChar(TEXT('\r')); break;
				case TEXT('t'): Decoded.AppendChar(TEXT('\t')); break;
				case TEXT('u'):
				{
					uint32 CodePoint = 0;
					for (int32 Digit = 0; Digit < 4; ++Digit)
					{
						const int32 HexValue = AtEnd() ? -1 : HexDigitValue(Peek());
						if (HexValue < 0)
						{
							OutError = FString::Printf(TEXT("invalid \\u escape at offset %d"), Position);
							return nullptr;
						}
						CodePoint = CodePoint * 16 + static_cast<uint32>(HexValue);
						Advance();
					}
					Decoded.AppendChar(static_cast<TCHAR>(CodePoint));
					break;
				}
				default:
					OutError = FString::Printf(TEXT("unknown escape '\\%c' at offset %d"), Escape, Position);
					return nullptr;
				}
			}
			Value->String = Decoded;
			return Value;
		}

		TSharedPtr<FEnemyJsonValue> ParseBoolean(FString& OutError)
		{
			const TSharedPtr<FEnemyJsonValue> Value = MakeShared<FEnemyJsonValue>();
			if (MatchLiteral(TEXT("true"), 4))
			{
				Value->Kind = FEnemyJsonValue::EKind::Boolean;
				Value->Boolean = true;
				Position += 4;
				return Value;
			}
			if (MatchLiteral(TEXT("false"), 5))
			{
				Value->Kind = FEnemyJsonValue::EKind::Boolean;
				Value->Boolean = false;
				Position += 5;
				return Value;
			}
			OutError = FString::Printf(TEXT("invalid literal at offset %d"), Position);
			return nullptr;
		}

		TSharedPtr<FEnemyJsonValue> ParseNull(FString& OutError)
		{
			if (MatchLiteral(TEXT("null"), 4))
			{
				const TSharedPtr<FEnemyJsonValue> Value = MakeShared<FEnemyJsonValue>();
				Value->Kind = FEnemyJsonValue::EKind::Null;
				Position += 4;
				return Value;
			}
			OutError = FString::Printf(TEXT("invalid literal at offset %d"), Position);
			return nullptr;
		}

		TSharedPtr<FEnemyJsonValue> ParseNumber(FString& OutError)
		{
			const int32 Start = Position;
			if (!AtEnd() && Peek() == TEXT('-'))
			{
				Advance();
			}
			bool bAnyDigit = false;
			while (!AtEnd() && Peek() >= TEXT('0') && Peek() <= TEXT('9'))
			{
				bAnyDigit = true;
				Advance();
			}
			if (!AtEnd() && Peek() == TEXT('.'))
			{
				Advance();
				while (!AtEnd() && Peek() >= TEXT('0') && Peek() <= TEXT('9'))
				{
					bAnyDigit = true;
					Advance();
				}
			}
			if (!AtEnd() && (Peek() == TEXT('e') || Peek() == TEXT('E')))
			{
				Advance();
				if (!AtEnd() && (Peek() == TEXT('+') || Peek() == TEXT('-')))
				{
					Advance();
				}
				while (!AtEnd() && Peek() >= TEXT('0') && Peek() <= TEXT('9'))
				{
					Advance();
				}
			}
			if (!bAnyDigit)
			{
				OutError = FString::Printf(TEXT("invalid number at offset %d"), Start);
				return nullptr;
			}
			const TSharedPtr<FEnemyJsonValue> Value = MakeShared<FEnemyJsonValue>();
			Value->Kind = FEnemyJsonValue::EKind::Number;
			Value->Number = FCString::Atod(*Source.Mid(Start, Position - Start));
			return Value;
		}
	};

	static const TCHAR* RequiredStringFields[] = { TEXT("enemy_id"), TEXT("melee_attack_id") };
	static const TCHAR* RequiredNumberFields[] =
	{
		TEXT("max_hp"),
		TEXT("attack_power"),
		TEXT("move_speed"),
		TEXT("attack_range_x"),
		TEXT("align_y_tolerance"),
		TEXT("telegraph_seconds"),
		TEXT("spawn_grace_seconds")
	};

	static FString GetEnemiesJsonPath()
	{
		return FPaths::ProjectDir() / TEXT("Data") / TEXT("enemies.json");
	}

	static FString GetCatalogJsonPath()
	{
		return FPaths::ProjectDir() / TEXT("Data") / TEXT("combat-attacks.json");
	}

	static bool LoadJsonRoot(const FString& Path, FString& OutError, TSharedPtr<FEnemyJsonValue>& OutRoot)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			OutError = FString::Printf(TEXT("could not read the JSON data file at %s"), *Path);
			return false;
		}
		TSharedPtr<FEnemyJsonValue> Root;
		if (!FEnemyJsonParser::Parse(Text, Root, OutError))
		{
			OutError = FString::Printf(TEXT("could not parse JSON in %s: %s"), *Path, *OutError);
			return false;
		}
		OutRoot = Root;
		return true;
	}

	/** Returns the field only when present with exactly the requested JSON kind. */
	static const TSharedPtr<FEnemyJsonValue>* FindTyped(const TSharedPtr<FEnemyJsonValue>& Object, const TCHAR* Name,
		FEnemyJsonValue::EKind Kind)
	{
		const TSharedPtr<FEnemyJsonValue>* Value = Object->Object.Find(Name);
		if (!Value || !Value->IsValid() || (*Value)->Kind != Kind)
		{
			return nullptr;
		}
		return Value;
	}

	static FString EnemyEntryName(const TSharedPtr<FEnemyJsonValue>& Entry)
	{
		const TSharedPtr<FEnemyJsonValue>* Id = FindTyped(Entry, TEXT("enemy_id"), FEnemyJsonValue::EKind::String);
		return (Id && !(*Id)->String.IsEmpty()) ? (*Id)->String : FString(TEXT("<unnamed entry>"));
	}

	static bool GetStringOrReport(FAutomationTestBase& Test, const TSharedPtr<FEnemyJsonValue>& Entry,
		const TCHAR* Name, FString& OutValue)
	{
		const TSharedPtr<FEnemyJsonValue>* Value = FindTyped(Entry, Name, FEnemyJsonValue::EKind::String);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("enemy '%s': missing or mistyped string field '%s'"),
				*EnemyEntryName(Entry), Name));
			return false;
		}
		OutValue = (*Value)->String;
		return true;
	}

	static bool GetNumberOrReport(FAutomationTestBase& Test, const TSharedPtr<FEnemyJsonValue>& Entry,
		const TCHAR* Name, double& OutValue)
	{
		const TSharedPtr<FEnemyJsonValue>* Value = FindTyped(Entry, Name, FEnemyJsonValue::EKind::Number);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("enemy '%s': missing or mistyped number field '%s'"),
				*EnemyEntryName(Entry), Name));
			return false;
		}
		OutValue = (*Value)->Number;
		return true;
	}

	/** Comparison tolerance for doubles that both sides derive from decimal literals. */
	static bool NearlyEqual(double A, double B)
	{
		return FMath::Abs(A - B) <= 1e-9;
	}

	/** Builds a valid melee_grunt definition in memory (independent of the JSON file). */
	static UEnemyDefinition* MakeValidGrunt()
	{
		UEnemyDefinition* Enemy = NewObject<UEnemyDefinition>();
		Enemy->EnemyId = TEXT("melee_grunt");
		Enemy->MaxHP = 60.0f;
		Enemy->AttackPower = 0.0f;
		Enemy->MoveSpeed = 220.0f;
		Enemy->AttackRangeX = 160.0f;
		Enemy->AlignYTolerance = 35.0f;
		Enemy->TelegraphSeconds = 0.35f;
		Enemy->SpawnGraceSeconds = 0.5f;
		Enemy->MeleeAttackId = TEXT("light_01");
		return Enemy;
	}

	/** Fills a UEnemyDefinition from one JSON entry; returns null and reports when fields are missing. */
	static UEnemyDefinition* MakeEnemyFromJson(FAutomationTestBase& Test, const TSharedPtr<FEnemyJsonValue>& Entry)
	{
		FString EnemyId;
		FString MeleeAttackId;
		double MaxHP = 0.0;
		double AttackPower = 0.0;
		double MoveSpeed = 0.0;
		double AttackRangeX = 0.0;
		double AlignYTolerance = 0.0;
		double TelegraphSeconds = 0.0;
		double SpawnGraceSeconds = 0.0;

		const bool bComplete =
			GetStringOrReport(Test, Entry, TEXT("enemy_id"), EnemyId) &&
			GetStringOrReport(Test, Entry, TEXT("melee_attack_id"), MeleeAttackId) &&
			GetNumberOrReport(Test, Entry, TEXT("max_hp"), MaxHP) &&
			GetNumberOrReport(Test, Entry, TEXT("attack_power"), AttackPower) &&
			GetNumberOrReport(Test, Entry, TEXT("move_speed"), MoveSpeed) &&
			GetNumberOrReport(Test, Entry, TEXT("attack_range_x"), AttackRangeX) &&
			GetNumberOrReport(Test, Entry, TEXT("align_y_tolerance"), AlignYTolerance) &&
			GetNumberOrReport(Test, Entry, TEXT("telegraph_seconds"), TelegraphSeconds) &&
			GetNumberOrReport(Test, Entry, TEXT("spawn_grace_seconds"), SpawnGraceSeconds);
		if (!bComplete)
		{
			return nullptr;
		}

		UEnemyDefinition* Enemy = NewObject<UEnemyDefinition>();
		Enemy->EnemyId = FName(*EnemyId);
		Enemy->MaxHP = static_cast<float>(MaxHP);
		Enemy->AttackPower = static_cast<float>(AttackPower);
		Enemy->MoveSpeed = static_cast<float>(MoveSpeed);
		Enemy->AttackRangeX = static_cast<float>(AttackRangeX);
		Enemy->AlignYTolerance = static_cast<float>(AlignYTolerance);
		Enemy->TelegraphSeconds = static_cast<float>(TelegraphSeconds);
		Enemy->SpawnGraceSeconds = static_cast<float>(SpawnGraceSeconds);
		Enemy->MeleeAttackId = FName(*MeleeAttackId);
		return Enemy;
	}
}

using namespace UE::UEMMO::Tasks::M2_001;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_001MeleeGruntLoadsAsSingleValidEntry,
	"UEMMO.Tasks.M2_001.MeleeGruntLoadsAsSingleValidEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_001MeleeGruntLoadsAsSingleValidEntry::RunTest(const FString& Parameters)
{
	FString LoadError;
	TSharedPtr<FEnemyJsonValue> Root;
	if (!LoadJsonRoot(GetEnemiesJsonPath(), LoadError, Root))
	{
		AddError(LoadError);
		return true;
	}

	const TSharedPtr<FEnemyJsonValue>* SchemaVersion = FindTyped(Root, TEXT("schema_version"), FEnemyJsonValue::EKind::Number);
	if (SchemaVersion)
	{
		TestEqual(TEXT("schema_version is 1"), (*SchemaVersion)->Number, 1.0);
	}
	else
	{
		AddError(TEXT("missing or mistyped top-level number 'schema_version'"));
	}

	const TSharedPtr<FEnemyJsonValue>* Enemies = FindTyped(Root, TEXT("enemies"), FEnemyJsonValue::EKind::Array);
	if (!Enemies)
	{
		AddError(TEXT("missing required top-level array 'enemies'"));
		return true;
	}
	TestEqual(TEXT("enemies holds exactly 1 entry"), (*Enemies)->Array.Num(), 1);
	if ((*Enemies)->Array.Num() != 1)
	{
		return true;
	}

	const TSharedPtr<FEnemyJsonValue> Entry = (*Enemies)->Array[0];
	if (!Entry.IsValid() || Entry->Kind != FEnemyJsonValue::EKind::Object)
	{
		AddError(TEXT("enemies[0] is not a JSON object"));
		return true;
	}
	TestEqual(TEXT("enemy_id is melee_grunt"), EnemyEntryName(Entry), FString(TEXT("melee_grunt")));

	for (const TCHAR* Field : RequiredStringFields)
	{
		TestTrue(FString::Printf(TEXT("enemy 'melee_grunt' has string field '%s'"), Field),
			FindTyped(Entry, Field, FEnemyJsonValue::EKind::String) != nullptr);
	}
	for (const TCHAR* Field : RequiredNumberFields)
	{
		TestTrue(FString::Printf(TEXT("enemy 'melee_grunt' has number field '%s'"), Field),
			FindTyped(Entry, Field, FEnemyJsonValue::EKind::Number) != nullptr);
	}

	// Contract values of the single melee_grunt entry (interface contract section 7).
	double MaxHP = 0.0;
	double AttackPower = -1.0;
	double MoveSpeed = 0.0;
	double AttackRangeX = 0.0;
	double AlignYTolerance = 0.0;
	double TelegraphSeconds = -1.0;
	double SpawnGraceSeconds = -1.0;
	FString MeleeAttackId;
	const bool bValuesRead =
		GetNumberOrReport(*this, Entry, TEXT("max_hp"), MaxHP) &&
		GetNumberOrReport(*this, Entry, TEXT("attack_power"), AttackPower) &&
		GetNumberOrReport(*this, Entry, TEXT("move_speed"), MoveSpeed) &&
		GetNumberOrReport(*this, Entry, TEXT("attack_range_x"), AttackRangeX) &&
		GetNumberOrReport(*this, Entry, TEXT("align_y_tolerance"), AlignYTolerance) &&
		GetNumberOrReport(*this, Entry, TEXT("telegraph_seconds"), TelegraphSeconds) &&
		GetNumberOrReport(*this, Entry, TEXT("spawn_grace_seconds"), SpawnGraceSeconds) &&
		GetStringOrReport(*this, Entry, TEXT("melee_attack_id"), MeleeAttackId);
	if (bValuesRead)
	{
		TestTrue(TEXT("max_hp is 60"), NearlyEqual(MaxHP, 60.0));
		TestTrue(TEXT("attack_power is 0"), NearlyEqual(AttackPower, 0.0));
		TestTrue(TEXT("move_speed is 220"), NearlyEqual(MoveSpeed, 220.0));
		TestTrue(TEXT("attack_range_x is 160"), NearlyEqual(AttackRangeX, 160.0));
		TestTrue(TEXT("align_y_tolerance is 35"), NearlyEqual(AlignYTolerance, 35.0));
		TestTrue(TEXT("telegraph_seconds is 0.35"), NearlyEqual(TelegraphSeconds, 0.35));
		TestTrue(TEXT("spawn_grace_seconds is 0.5"), NearlyEqual(SpawnGraceSeconds, 0.5));
		TestEqual(TEXT("melee_attack_id is light_01"), MeleeAttackId, FString(TEXT("light_01")));
	}

	// The loaded entry maps into the data asset and passes single-entry validation.
	UEnemyDefinition* Enemy = MakeEnemyFromJson(*this, Entry);
	if (!Enemy)
	{
		return true;
	}
	TestEqual(TEXT("loaded EnemyId is melee_grunt"), Enemy->EnemyId, FName(TEXT("melee_grunt")));
	TestEqual(TEXT("loaded MaxHP is 60"), Enemy->MaxHP, 60.0f);
	TestEqual(TEXT("loaded AttackPower is 0"), Enemy->AttackPower, 0.0f);
	TestEqual(TEXT("loaded MoveSpeed is 220"), Enemy->MoveSpeed, 220.0f);
	TestEqual(TEXT("loaded AttackRangeX is 160"), Enemy->AttackRangeX, 160.0f);
	TestEqual(TEXT("loaded AlignYTolerance is 35"), Enemy->AlignYTolerance, 35.0f);
	TestEqual(TEXT("loaded TelegraphSeconds is 0.35"), Enemy->TelegraphSeconds, 0.35f);
	TestEqual(TEXT("loaded SpawnGraceSeconds is 0.5"), Enemy->SpawnGraceSeconds, 0.5f);
	TestEqual(TEXT("loaded MeleeAttackId is light_01"), Enemy->MeleeAttackId, FName(TEXT("light_01")));

	FText Errors;
	TestTrue(TEXT("the loaded melee_grunt passes ValidateEnemyDefinition"),
		ValidateEnemyDefinition(*Enemy, Errors));
	TestTrue(TEXT("the loaded melee_grunt reports no validation errors"), Errors.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_001MeleeGruntAttackIdExistsInM1Catalog,
	"UEMMO.Tasks.M2_001.MeleeGruntAttackIdExistsInM1Catalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_001MeleeGruntAttackIdExistsInM1Catalog::RunTest(const FString& Parameters)
{
	// Data-level existence check: the C++ single-entry validator only requires a
	// non-empty MeleeAttackId (cross-entry validation belongs to later M2 tasks),
	// so this test enforces the source-data rule at the JSON level instead.
	FString EnemiesError;
	TSharedPtr<FEnemyJsonValue> EnemiesRoot;
	if (!LoadJsonRoot(GetEnemiesJsonPath(), EnemiesError, EnemiesRoot))
	{
		AddError(EnemiesError);
		return true;
	}

	const TSharedPtr<FEnemyJsonValue>* Enemies = FindTyped(EnemiesRoot, TEXT("enemies"), FEnemyJsonValue::EKind::Array);
	if (!Enemies || (*Enemies)->Array.Num() != 1)
	{
		AddError(TEXT("enemies must hold exactly 1 entry for this check"));
		return true;
	}
	const TSharedPtr<FEnemyJsonValue>& Entry = (*Enemies)->Array[0];
	FString MeleeAttackId;
	if (!GetStringOrReport(*this, Entry, TEXT("melee_attack_id"), MeleeAttackId))
	{
		return true;
	}
	TestTrue(TEXT("melee_attack_id is non-empty"), !MeleeAttackId.IsEmpty());

	FString CatalogError;
	TSharedPtr<FEnemyJsonValue> CatalogRoot;
	if (!LoadJsonRoot(GetCatalogJsonPath(), CatalogError, CatalogRoot))
	{
		AddError(CatalogError);
		return true;
	}
	const TSharedPtr<FEnemyJsonValue>* Attacks = FindTyped(CatalogRoot, TEXT("attacks"), FEnemyJsonValue::EKind::Array);
	if (!Attacks)
	{
		AddError(TEXT("missing required top-level array 'attacks' in combat-attacks.json"));
		return true;
	}
	TSet<FString> KnownAttackIds;
	for (const TSharedPtr<FEnemyJsonValue>& Attack : (*Attacks)->Array)
	{
		if (Attack.IsValid() && Attack->Kind == FEnemyJsonValue::EKind::Object)
		{
			const TSharedPtr<FEnemyJsonValue>* Id = FindTyped(Attack, TEXT("attack_id"), FEnemyJsonValue::EKind::String);
			if (Id && !(*Id)->String.IsEmpty())
			{
				KnownAttackIds.Add((*Id)->String);
			}
		}
	}
	TestTrue(FString::Printf(TEXT("melee_attack_id '%s' resolves to an attack of the M1 catalog"), *MeleeAttackId),
		KnownAttackIds.Contains(MeleeAttackId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_001SpawnGraceDecrementsOnInjectedClock,
	"UEMMO.Tasks.M2_001.SpawnGraceDecrementsOnInjectedClock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_001SpawnGraceDecrementsOnInjectedClock::RunTest(const FString& Parameters)
{
	// Spawn marked at game time 100.0 with the contract grace of 0.5s. The
	// grace window is half-open [100.0, 100.5): attackable only from 0.5s
	// elapsed onwards; every time value below injects an explicit game clock.
	UEnemyDefinition* Enemy = MakeValidGrunt();
	Enemy->MarkSpawned(100.0);

	const double RemainingAtSpawn = Enemy->GetSpawnGraceRemaining(100.0);
	TestTrue(TEXT("remaining grace at the spawn instant is the full 0.5s"),
		NearlyEqual(RemainingAtSpawn, 0.5));
	TestFalse(TEXT("CanAttack is false at the spawn instant"), Enemy->CanAttack(100.0));

	const double RemainingAt049 = Enemy->GetSpawnGraceRemaining(100.49);
	TestTrue(TEXT("remaining grace after 0.49s is inside (0, 0.5]"),
		RemainingAt049 > 0.0 && RemainingAt049 <= 0.5);
	TestFalse(TEXT("CanAttack is false after 0.49s"), Enemy->CanAttack(100.49));

	const double RemainingAt050 = Enemy->GetSpawnGraceRemaining(100.5);
	TestTrue(TEXT("remaining grace after exactly 0.5s is 0"), NearlyEqual(RemainingAt050, 0.0));
	TestTrue(TEXT("CanAttack is true after exactly 0.5s (grace window is half-open)"),
		Enemy->CanAttack(100.5));

	const double RemainingAt099 = Enemy->GetSpawnGraceRemaining(100.99);
	TestTrue(TEXT("remaining grace after 0.99s is 0"), NearlyEqual(RemainingAt099, 0.0));
	TestTrue(TEXT("CanAttack is true after 0.99s"), Enemy->CanAttack(100.99));

	const double RemainingFarLater = Enemy->GetSpawnGraceRemaining(200.0);
	TestTrue(TEXT("remaining grace never goes negative (clamped at 0)"), RemainingFarLater == 0.0);
	TestTrue(TEXT("CanAttack is true long after the grace window"), Enemy->CanAttack(200.0));

	// A definition without an explicit MarkSpawned behaves like a spawn at
	// game time 0: the grace starts at the full SpawnGraceSeconds and counts
	// down on the injected clock only.
	UEnemyDefinition* Fresh = MakeValidGrunt();
	TestTrue(TEXT("fresh definition keeps the full grace at game time 0"),
		NearlyEqual(Fresh->GetSpawnGraceRemaining(0.0), 0.5));
	TestFalse(TEXT("fresh definition cannot attack at game time 0"), Fresh->CanAttack(0.0));
	TestFalse(TEXT("fresh definition cannot attack at game time 0.49"), Fresh->CanAttack(0.49));
	TestTrue(TEXT("fresh definition can attack at game time 0.5"), Fresh->CanAttack(0.5));

	// Re-spawning reopens the grace window from the new mark.
	Fresh->MarkSpawned(50.0);
	TestFalse(TEXT("a re-spawned definition cannot attack again right after the mark"),
		Fresh->CanAttack(50.1));
	TestTrue(TEXT("a re-spawned definition can attack after its new grace elapsed"),
		Fresh->CanAttack(50.5));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_001RejectsInvalidFieldsWithFieldNames,
	"UEMMO.Tasks.M2_001.RejectsInvalidFieldsWithFieldNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_001RejectsInvalidFieldsWithFieldNames::RunTest(const FString& Parameters)
{
	// Every rejected mutation must fail validation and name the offending field.
	UEnemyDefinition* NegativeMaxHP = MakeValidGrunt();
	NegativeMaxHP->MaxHP = -1.0f;
	FText NegativeMaxHPErrors;
	TestFalse(TEXT("MaxHP=-1 is rejected"), ValidateEnemyDefinition(*NegativeMaxHP, NegativeMaxHPErrors));
	TestTrue(TEXT("MaxHP=-1 error names MaxHP"), NegativeMaxHPErrors.ToString().Contains(TEXT("MaxHP")));

	UEnemyDefinition* ZeroMaxHP = MakeValidGrunt();
	ZeroMaxHP->MaxHP = 0.0f;
	FText ZeroMaxHPErrors;
	TestFalse(TEXT("MaxHP=0 is rejected"), ValidateEnemyDefinition(*ZeroMaxHP, ZeroMaxHPErrors));
	TestTrue(TEXT("MaxHP=0 error names MaxHP"), ZeroMaxHPErrors.ToString().Contains(TEXT("MaxHP")));

	UEnemyDefinition* NegativeAttackPower = MakeValidGrunt();
	NegativeAttackPower->AttackPower = -1.0f;
	FText NegativeAttackPowerErrors;
	TestFalse(TEXT("AttackPower=-1 is rejected"), ValidateEnemyDefinition(*NegativeAttackPower, NegativeAttackPowerErrors));
	TestTrue(TEXT("AttackPower=-1 error names AttackPower"),
		NegativeAttackPowerErrors.ToString().Contains(TEXT("AttackPower")));

	UEnemyDefinition* NegativeMoveSpeed = MakeValidGrunt();
	NegativeMoveSpeed->MoveSpeed = -10.0f;
	FText NegativeMoveSpeedErrors;
	TestFalse(TEXT("MoveSpeed=-10 is rejected"), ValidateEnemyDefinition(*NegativeMoveSpeed, NegativeMoveSpeedErrors));
	TestTrue(TEXT("MoveSpeed=-10 error names MoveSpeed"),
		NegativeMoveSpeedErrors.ToString().Contains(TEXT("MoveSpeed")));

	UEnemyDefinition* NegativeRange = MakeValidGrunt();
	NegativeRange->AttackRangeX = -1.0f;
	FText NegativeRangeErrors;
	TestFalse(TEXT("AttackRangeX=-1 is rejected"), ValidateEnemyDefinition(*NegativeRange, NegativeRangeErrors));
	TestTrue(TEXT("AttackRangeX=-1 error names AttackRangeX"),
		NegativeRangeErrors.ToString().Contains(TEXT("AttackRangeX")));

	UEnemyDefinition* ZeroTolerance = MakeValidGrunt();
	ZeroTolerance->AlignYTolerance = 0.0f;
	FText ZeroToleranceErrors;
	TestFalse(TEXT("AlignYTolerance=0 is rejected"), ValidateEnemyDefinition(*ZeroTolerance, ZeroToleranceErrors));
	TestTrue(TEXT("AlignYTolerance=0 error names AlignYTolerance"),
		ZeroToleranceErrors.ToString().Contains(TEXT("AlignYTolerance")));

	UEnemyDefinition* NegativeTolerance = MakeValidGrunt();
	NegativeTolerance->AlignYTolerance = -5.0f;
	FText NegativeToleranceErrors;
	TestFalse(TEXT("AlignYTolerance=-5 is rejected"), ValidateEnemyDefinition(*NegativeTolerance, NegativeToleranceErrors));
	TestTrue(TEXT("AlignYTolerance=-5 error names AlignYTolerance"),
		NegativeToleranceErrors.ToString().Contains(TEXT("AlignYTolerance")));

	UEnemyDefinition* NegativeTelegraph = MakeValidGrunt();
	NegativeTelegraph->TelegraphSeconds = -0.1f;
	FText NegativeTelegraphErrors;
	TestFalse(TEXT("TelegraphSeconds=-0.1 is rejected"), ValidateEnemyDefinition(*NegativeTelegraph, NegativeTelegraphErrors));
	TestTrue(TEXT("TelegraphSeconds=-0.1 error names TelegraphSeconds"),
		NegativeTelegraphErrors.ToString().Contains(TEXT("TelegraphSeconds")));

	UEnemyDefinition* NegativeGrace = MakeValidGrunt();
	NegativeGrace->SpawnGraceSeconds = -0.5f;
	FText NegativeGraceErrors;
	TestFalse(TEXT("SpawnGraceSeconds=-0.5 is rejected"), ValidateEnemyDefinition(*NegativeGrace, NegativeGraceErrors));
	TestTrue(TEXT("SpawnGraceSeconds=-0.5 error names SpawnGraceSeconds"),
		NegativeGraceErrors.ToString().Contains(TEXT("SpawnGraceSeconds")));

	UEnemyDefinition* MissingAttack = MakeValidGrunt();
	MissingAttack->MeleeAttackId = NAME_None;
	FText MissingAttackErrors;
	TestFalse(TEXT("an empty MeleeAttackId is rejected"), ValidateEnemyDefinition(*MissingAttack, MissingAttackErrors));
	TestTrue(TEXT("empty MeleeAttackId error names MeleeAttackId"),
		MissingAttackErrors.ToString().Contains(TEXT("MeleeAttackId")));

	UEnemyDefinition* MissingId = MakeValidGrunt();
	MissingId->EnemyId = NAME_None;
	FText MissingIdErrors;
	TestFalse(TEXT("an empty EnemyId is rejected"), ValidateEnemyDefinition(*MissingId, MissingIdErrors));
	TestTrue(TEXT("empty EnemyId error names EnemyId"),
		MissingIdErrors.ToString().Contains(TEXT("EnemyId")));

	// Several problems at once are collected and joined with "; ".
	UEnemyDefinition* Broken = MakeValidGrunt();
	Broken->MaxHP = -1.0f;
	Broken->AlignYTolerance = 0.0f;
	Broken->MeleeAttackId = NAME_None;
	FText BrokenErrors;
	TestFalse(TEXT("a definition with multiple problems is rejected"),
		ValidateEnemyDefinition(*Broken, BrokenErrors));
	TestTrue(TEXT("MaxHP problem is included"), BrokenErrors.ToString().Contains(TEXT("MaxHP")));
	TestTrue(TEXT("AlignYTolerance problem is included"), BrokenErrors.ToString().Contains(TEXT("AlignYTolerance")));
	TestTrue(TEXT("MeleeAttackId problem is included"), BrokenErrors.ToString().Contains(TEXT("MeleeAttackId")));
	TestTrue(TEXT("problems are joined with \"; \""), BrokenErrors.ToString().Contains(TEXT("; ")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_001DefaultConstructionFailsValidation,
	"UEMMO.Tasks.M2_001.DefaultConstructionFailsValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_001DefaultConstructionFailsValidation::RunTest(const FString& Parameters)
{
	// The numeric defaults come from the interface contract (section 7); the
	// identity fields stay empty, so a default-constructed entry is invalid.
	UEnemyDefinition* Enemy = NewObject<UEnemyDefinition>();
	TestEqual(TEXT("default EnemyId is None"), Enemy->EnemyId, FName(NAME_None));
	TestEqual(TEXT("default MaxHP is 60"), Enemy->MaxHP, 60.0f);
	TestEqual(TEXT("default AttackPower is 0"), Enemy->AttackPower, 0.0f);
	TestEqual(TEXT("default MoveSpeed is 220"), Enemy->MoveSpeed, 220.0f);
	TestEqual(TEXT("default AttackRangeX is 160"), Enemy->AttackRangeX, 160.0f);
	TestEqual(TEXT("default AlignYTolerance is 35"), Enemy->AlignYTolerance, 35.0f);
	TestEqual(TEXT("default TelegraphSeconds is 0.35"), Enemy->TelegraphSeconds, 0.35f);
	TestEqual(TEXT("default SpawnGraceSeconds is 0.5"), Enemy->SpawnGraceSeconds, 0.5f);
	TestEqual(TEXT("default MeleeAttackId is None"), Enemy->MeleeAttackId, FName(NAME_None));

	FText Errors;
	TestFalse(TEXT("default-constructed enemy (empty EnemyId/MeleeAttackId) is rejected"),
		ValidateEnemyDefinition(*Enemy, Errors));
	TestTrue(TEXT("default-constructed error names EnemyId"), Errors.ToString().Contains(TEXT("EnemyId")));
	TestTrue(TEXT("default-constructed error names MeleeAttackId"),
		Errors.ToString().Contains(TEXT("MeleeAttackId")));

	// With a valid identity but HP<=0 the rejection must point at MaxHP.
	UEnemyDefinition* ZeroHP = NewObject<UEnemyDefinition>();
	ZeroHP->EnemyId = TEXT("melee_grunt");
	ZeroHP->MaxHP = 0.0f;
	FText ZeroHPErrors;
	TestFalse(TEXT("an enemy with MaxHP=0 is rejected"), ValidateEnemyDefinition(*ZeroHP, ZeroHPErrors));
	TestTrue(TEXT("MaxHP=0 error names MaxHP"), ZeroHPErrors.ToString().Contains(TEXT("MaxHP")));
	return true;
}

#endif
