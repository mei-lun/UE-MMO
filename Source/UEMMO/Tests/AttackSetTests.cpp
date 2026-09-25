// M1-008: validates the textual four-attack source data (Data/combat-attacks.json)
// against the interface contract (section 3) and the M1-007 single-entry validator.
// Pure text checks: this file only reads the JSON, it never creates or mutates
// UE assets, and a failing validation must not touch the file on disk.
//
// Parsing note: the engine's Json module headers compile through Engine's public
// dependency, but UnrealBuildTool does not put the Json import library on the
// UEMMO link line, so FJsonValue/FJsonObject symbols fail to link (LNK2019).
// Since UEMMO.Build.cs is outside this task's file range, the tests use the
// small hand-rolled parser below (objects, arrays, strings with escapes,
// numbers, booleans, null) built on Core types only.

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"

#include "../Combat/AttackDefinition.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_008
{
	/** Minimal JSON value: just enough shape for the combat data file. */
	class FSimpleJson
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
		TArray<TSharedPtr<FSimpleJson>> Array;
		TMap<FString, TSharedPtr<FSimpleJson>> Object;
	};

	/** Recursive descent JSON parser (RFC 8259 subset; \\u escapes decode to a single TCHAR). */
	class FSimpleJsonParser
	{
	public:
		static bool Parse(const FString& Text, TSharedPtr<FSimpleJson>& OutRoot, FString& OutError)
		{
			FSimpleJsonParser Parser(Text);
			Parser.SkipWhitespace();
			TSharedPtr<FSimpleJson> Root = Parser.ParseValue(OutError);
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
		explicit FSimpleJsonParser(const FString& InText)
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

		TSharedPtr<FSimpleJson> ParseValue(FString& OutError)
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

		TSharedPtr<FSimpleJson> ParseObject(FString& OutError)
		{
			if (!Expect(TEXT('{'), TEXT("to start an object"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FSimpleJson> Value = MakeShared<FSimpleJson>();
			Value->Kind = FSimpleJson::EKind::Object;
			SkipWhitespace();
			if (Consume(TEXT('}')))
			{
				return Value;
			}
			while (true)
			{
				SkipWhitespace();
				TSharedPtr<FSimpleJson> Key = ParseString(OutError);
				if (!Key.IsValid())
				{
					return nullptr;
				}
				SkipWhitespace();
				if (!Expect(TEXT(':'), TEXT("between object key and value"), OutError))
				{
					return nullptr;
				}
				TSharedPtr<FSimpleJson> Element = ParseValue(OutError);
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

		TSharedPtr<FSimpleJson> ParseArray(FString& OutError)
		{
			if (!Expect(TEXT('['), TEXT("to start an array"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FSimpleJson> Value = MakeShared<FSimpleJson>();
			Value->Kind = FSimpleJson::EKind::Array;
			SkipWhitespace();
			if (Consume(TEXT(']')))
			{
				return Value;
			}
			while (true)
			{
				TSharedPtr<FSimpleJson> Element = ParseValue(OutError);
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

		TSharedPtr<FSimpleJson> ParseString(FString& OutError)
		{
			if (!Expect(TEXT('"'), TEXT("to start a string"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FSimpleJson> Value = MakeShared<FSimpleJson>();
			Value->Kind = FSimpleJson::EKind::String;
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

		TSharedPtr<FSimpleJson> ParseBoolean(FString& OutError)
		{
			const TSharedPtr<FSimpleJson> Value = MakeShared<FSimpleJson>();
			if (MatchLiteral(TEXT("true"), 4))
			{
				Value->Kind = FSimpleJson::EKind::Boolean;
				Value->Boolean = true;
				Position += 4;
				return Value;
			}
			if (MatchLiteral(TEXT("false"), 5))
			{
				Value->Kind = FSimpleJson::EKind::Boolean;
				Value->Boolean = false;
				Position += 5;
				return Value;
			}
			OutError = FString::Printf(TEXT("invalid literal at offset %d"), Position);
			return nullptr;
		}

		TSharedPtr<FSimpleJson> ParseNull(FString& OutError)
		{
			if (MatchLiteral(TEXT("null"), 4))
			{
				const TSharedPtr<FSimpleJson> Value = MakeShared<FSimpleJson>();
				Value->Kind = FSimpleJson::EKind::Null;
				Position += 4;
				return Value;
			}
			OutError = FString::Printf(TEXT("invalid literal at offset %d"), Position);
			return nullptr;
		}

		TSharedPtr<FSimpleJson> ParseNumber(FString& OutError)
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
			const TSharedPtr<FSimpleJson> Value = MakeShared<FSimpleJson>();
			Value->Kind = FSimpleJson::EKind::Number;
			Value->Number = FCString::Atod(*Source.Mid(Start, Position - Start));
			return Value;
		}
	};

	// The exact stable ID set required by the task card / design section 7.
	static const TCHAR* ExpectedAttackIds[4] =
	{
		TEXT("light_01"),
		TEXT("light_02"),
		TEXT("launcher"),
		TEXT("aerial_01")
	};

	static const TCHAR* RequiredStringFields[] = { TEXT("attack_id"), TEXT("animation_path") };
	static const TCHAR* RequiredNumberFields[] =
	{
		TEXT("duration_frames"),
		TEXT("active_start_frame"),
		TEXT("active_end_frame"),
		TEXT("cancel_start_frame"),
		TEXT("cancel_end_frame"),
		TEXT("base_damage"),
		TEXT("attack_coefficient"),
		TEXT("knockback_cm_per_s"),
		TEXT("launch_cm_per_s"),
		TEXT("hit_stun_seconds"),
		TEXT("hit_stop_seconds"),
		TEXT("clip_start_seconds"),
		TEXT("clip_end_seconds")
	};
	static const TCHAR* RequiredVectorFields[] = { TEXT("hit_offset_cm"), TEXT("hit_half_extent_cm") };
	static const TCHAR* RequiredIdArrayFields[] = { TEXT("next_attack_ids"), TEXT("cancel_window_allows") };
	static const TCHAR* RequiredBoolFields[] = { TEXT("placeholder_animation") };

	static FString GetCatalogPath()
	{
		return FPaths::ProjectDir() / TEXT("Data") / TEXT("combat-attacks.json");
	}

	static bool LoadCatalogRoot(FString& OutError, TSharedPtr<FSimpleJson>& OutRoot)
	{
		const FString Path = GetCatalogPath();
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			OutError = FString::Printf(TEXT("could not read the combat data file at %s"), *Path);
			return false;
		}
		TSharedPtr<FSimpleJson> Root;
		if (!FSimpleJsonParser::Parse(Text, Root, OutError))
		{
			OutError = FString::Printf(TEXT("could not parse JSON in %s: %s"), *Path, *OutError);
			return false;
		}
		OutRoot = Root;
		return true;
	}

	/** Returns the field only when present with exactly the requested JSON kind. */
	static const TSharedPtr<FSimpleJson>* FindTyped(const TSharedPtr<FSimpleJson>& Object, const TCHAR* Name,
		FSimpleJson::EKind Kind)
	{
		const TSharedPtr<FSimpleJson>* Value = Object->Object.Find(Name);
		if (!Value || !Value->IsValid() || (*Value)->Kind != Kind)
		{
			return nullptr;
		}
		return Value;
	}

	static bool GetAttacksOrReport(FAutomationTestBase& Test, const TSharedPtr<FSimpleJson>& Root,
		TArray<TSharedPtr<FSimpleJson>>& OutAttacks)
	{
		const TSharedPtr<FSimpleJson>* Attacks = FindTyped(Root, TEXT("attacks"), FSimpleJson::EKind::Array);
		if (!Attacks)
		{
			Test.AddError(TEXT("missing required top-level array 'attacks'"));
			return false;
		}
		OutAttacks = (*Attacks)->Array;
		return true;
	}

	/** Entry object accessor that reports unparseable array elements instead of crashing. */
	static TSharedPtr<FSimpleJson> AsEntryObject(FAutomationTestBase& Test, const TSharedPtr<FSimpleJson>& Value,
		int32 Index)
	{
		if (!Value.IsValid() || Value->Kind != FSimpleJson::EKind::Object)
		{
			Test.AddError(FString::Printf(TEXT("attacks[%d] is not a JSON object"), Index));
			return nullptr;
		}
		return Value;
	}

	static FString EntryName(const TSharedPtr<FSimpleJson>& Entry)
	{
		const TSharedPtr<FSimpleJson>* Id = FindTyped(Entry, TEXT("attack_id"), FSimpleJson::EKind::String);
		return (Id && !(*Id)->String.IsEmpty()) ? (*Id)->String : FString(TEXT("<unnamed entry>"));
	}

	static bool EntryHasFieldOfType(const TSharedPtr<FSimpleJson>& Entry, const TCHAR* Name, FSimpleJson::EKind Kind)
	{
		return FindTyped(Entry, Name, Kind) != nullptr;
	}

	static bool GetNumberOrReport(FAutomationTestBase& Test, const TSharedPtr<FSimpleJson>& Entry,
		const TCHAR* Name, double& OutValue)
	{
		const TSharedPtr<FSimpleJson>* Value = FindTyped(Entry, Name, FSimpleJson::EKind::Number);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("attack '%s': missing or mistyped number field '%s'"),
				*EntryName(Entry), Name));
			return false;
		}
		OutValue = (*Value)->Number;
		return true;
	}

	static bool GetStringOrReport(FAutomationTestBase& Test, const TSharedPtr<FSimpleJson>& Entry,
		const TCHAR* Name, FString& OutValue)
	{
		const TSharedPtr<FSimpleJson>* Value = FindTyped(Entry, Name, FSimpleJson::EKind::String);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("attack '%s': missing or mistyped string field '%s'"),
				*EntryName(Entry), Name));
			return false;
		}
		OutValue = (*Value)->String;
		return true;
	}

	static bool GetBoolOrReport(FAutomationTestBase& Test, const TSharedPtr<FSimpleJson>& Entry,
		const TCHAR* Name, bool& OutValue)
	{
		const TSharedPtr<FSimpleJson>* Value = FindTyped(Entry, Name, FSimpleJson::EKind::Boolean);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("attack '%s': missing or mistyped boolean field '%s'"),
				*EntryName(Entry), Name));
			return false;
		}
		OutValue = (*Value)->Boolean;
		return true;
	}

	/** Reads [x, y, z]; every element must be a number. */
	static bool GetVectorOrReport(FAutomationTestBase& Test, const TSharedPtr<FSimpleJson>& Entry,
		const TCHAR* Name, FVector& OutVector)
	{
		const TSharedPtr<FSimpleJson>* Value = FindTyped(Entry, Name, FSimpleJson::EKind::Array);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("attack '%s': missing or mistyped array field '%s'"),
				*EntryName(Entry), Name));
			return false;
		}
		const TArray<TSharedPtr<FSimpleJson>>& Components = (*Value)->Array;
		if (Components.Num() != 3)
		{
			Test.AddError(FString::Printf(TEXT("attack '%s': field '%s' must hold exactly 3 components (got %d)"),
				*EntryName(Entry), Name, Components.Num()));
			return false;
		}
		double XYZ[3] = { 0.0, 0.0, 0.0 };
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			if (!Components[Axis].IsValid() || Components[Axis]->Kind != FSimpleJson::EKind::Number)
			{
				Test.AddError(FString::Printf(TEXT("attack '%s': field '%s' component %d is not a number"),
					*EntryName(Entry), Name, Axis));
				return false;
			}
			XYZ[Axis] = Components[Axis]->Number;
		}
		OutVector = FVector(XYZ[0], XYZ[1], XYZ[2]);
		return true;
	}

	/** Fills a UAttackDefinition from one JSON entry; returns null and reports when fields are missing. */
	static UAttackDefinition* MakeDefinitionFromJson(FAutomationTestBase& Test, const TSharedPtr<FSimpleJson>& Entry)
	{
		UAttackDefinition* Attack = NewObject<UAttackDefinition>();

		FString AttackId;
		FString AnimationPath;
		double Duration = 0.0;
		double ActiveStart = 0.0;
		double ActiveEnd = 0.0;
		double CancelStart = 0.0;
		double CancelEnd = 0.0;
		double BaseDamage = 0.0;
		double Coefficient = 1.0;
		double Knockback = 0.0;
		double Launch = 0.0;
		double HitStun = 0.0;
		double HitStop = 0.04;
		double ClipStart = 0.0;
		double ClipEnd = 0.0;
		FVector HitOffset = FVector::ZeroVector;
		FVector HalfExtent = FVector::ZeroVector;
		bool bPlaceholder = true;

		const bool bComplete =
			GetStringOrReport(Test, Entry, TEXT("attack_id"), AttackId) &&
			GetStringOrReport(Test, Entry, TEXT("animation_path"), AnimationPath) &&
			GetNumberOrReport(Test, Entry, TEXT("duration_frames"), Duration) &&
			GetNumberOrReport(Test, Entry, TEXT("active_start_frame"), ActiveStart) &&
			GetNumberOrReport(Test, Entry, TEXT("active_end_frame"), ActiveEnd) &&
			GetNumberOrReport(Test, Entry, TEXT("cancel_start_frame"), CancelStart) &&
			GetNumberOrReport(Test, Entry, TEXT("cancel_end_frame"), CancelEnd) &&
			GetNumberOrReport(Test, Entry, TEXT("base_damage"), BaseDamage) &&
			GetNumberOrReport(Test, Entry, TEXT("attack_coefficient"), Coefficient) &&
			GetNumberOrReport(Test, Entry, TEXT("knockback_cm_per_s"), Knockback) &&
			GetNumberOrReport(Test, Entry, TEXT("launch_cm_per_s"), Launch) &&
			GetNumberOrReport(Test, Entry, TEXT("hit_stun_seconds"), HitStun) &&
			GetNumberOrReport(Test, Entry, TEXT("hit_stop_seconds"), HitStop) &&
			GetNumberOrReport(Test, Entry, TEXT("clip_start_seconds"), ClipStart) &&
			GetNumberOrReport(Test, Entry, TEXT("clip_end_seconds"), ClipEnd) &&
			GetVectorOrReport(Test, Entry, TEXT("hit_offset_cm"), HitOffset) &&
			GetVectorOrReport(Test, Entry, TEXT("hit_half_extent_cm"), HalfExtent) &&
			GetBoolOrReport(Test, Entry, TEXT("placeholder_animation"), bPlaceholder);
		if (!bComplete)
		{
			return nullptr;
		}

		Attack->AttackId = FName(*AttackId);
		Attack->DurationFrames = static_cast<int32>(Duration);
		Attack->ActiveWindow.StartFrame = static_cast<int32>(ActiveStart);
		Attack->ActiveWindow.EndFrame = static_cast<int32>(ActiveEnd);
		Attack->CancelWindow.StartFrame = static_cast<int32>(CancelStart);
		Attack->CancelWindow.EndFrame = static_cast<int32>(CancelEnd);
		Attack->BaseDamage = static_cast<float>(BaseDamage);
		Attack->AttackCoefficient = static_cast<float>(Coefficient);
		Attack->HitOffsetFromFeet = HitOffset;
		Attack->HitHalfExtent = HalfExtent;
		Attack->KnockbackSpeed = static_cast<float>(Knockback);
		Attack->LaunchSpeed = static_cast<float>(Launch);
		Attack->HitStunSeconds = static_cast<float>(HitStun);
		Attack->HitStopSeconds = static_cast<float>(HitStop);
		Attack->ClipStartSeconds = static_cast<float>(ClipStart);
		Attack->ClipEndSeconds = static_cast<float>(ClipEnd);
		Attack->Animation = TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(AnimationPath));
		Attack->bPlaceholderAnimation = bPlaceholder;

		const TSharedPtr<FSimpleJson>* NextIds = FindTyped(Entry, TEXT("next_attack_ids"), FSimpleJson::EKind::Array);
		if (NextIds)
		{
			for (const TSharedPtr<FSimpleJson>& Next : (*NextIds)->Array)
			{
				if (Next.IsValid() && Next->Kind == FSimpleJson::EKind::String)
				{
					Attack->AllowedNextAttacks.Add(FName(*Next->String));
				}
			}
		}
		return Attack;
	}
}

using namespace UE::UEMMO::Tasks::M1_008;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_008CatalogLoadsAsFourUniqueAttackIds,
	"UEMMO.Tasks.M1_008.CatalogLoadsAsFourUniqueAttackIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_008CatalogLoadsAsFourUniqueAttackIds::RunTest(const FString& Parameters)
{
	FString LoadError;
	TSharedPtr<FSimpleJson> Root;
	if (!LoadCatalogRoot(LoadError, Root))
	{
		AddError(LoadError);
		return true;
	}

	const TSharedPtr<FSimpleJson>* SchemaVersion = FindTyped(Root, TEXT("schema_version"), FSimpleJson::EKind::Number);
	if (SchemaVersion)
	{
		TestEqual(TEXT("schema_version is 1"), (*SchemaVersion)->Number, 1.0);
	}
	else
	{
		AddError(TEXT("missing or mistyped top-level number 'schema_version'"));
	}

	const TSharedPtr<FSimpleJson>* LogicFps = FindTyped(Root, TEXT("logic_fps"), FSimpleJson::EKind::Number);
	if (LogicFps)
	{
		TestEqual(TEXT("logic_fps is 60"), (*LogicFps)->Number, 60.0);
	}
	else
	{
		AddError(TEXT("missing or mistyped top-level number 'logic_fps'"));
	}

	TArray<TSharedPtr<FSimpleJson>> Attacks;
	if (!GetAttacksOrReport(*this, Root, Attacks))
	{
		return true;
	}
	TestEqual(TEXT("the catalog holds exactly 4 attacks"), Attacks.Num(), 4);

	TMap<FString, int32> IdCounts;
	for (int32 Index = 0; Index < Attacks.Num(); ++Index)
	{
		const TSharedPtr<FSimpleJson> Entry = AsEntryObject(*this, Attacks[Index], Index);
		if (!Entry.IsValid())
		{
			continue;
		}
		const TSharedPtr<FSimpleJson>* Id = FindTyped(Entry, TEXT("attack_id"), FSimpleJson::EKind::String);
		if (!Id || (*Id)->String.IsEmpty())
		{
			AddError(FString::Printf(TEXT("attacks[%d] is missing its string field 'attack_id'"), Index));
			continue;
		}
		++IdCounts.FindOrAdd((*Id)->String);
	}
	for (const TCHAR* ExpectedId : ExpectedAttackIds)
	{
		TestEqual(FString::Printf(TEXT("attack id '%s' appears exactly once"), ExpectedId),
			IdCounts.FindRef(ExpectedId), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_008RequiredFieldsPresentInEveryEntry,
	"UEMMO.Tasks.M1_008.RequiredFieldsPresentInEveryEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_008RequiredFieldsPresentInEveryEntry::RunTest(const FString& Parameters)
{
	FString LoadError;
	TSharedPtr<FSimpleJson> Root;
	if (!LoadCatalogRoot(LoadError, Root))
	{
		AddError(LoadError);
		return true;
	}
	TArray<TSharedPtr<FSimpleJson>> Attacks;
	if (!GetAttacksOrReport(*this, Root, Attacks))
	{
		return true;
	}

	for (int32 Index = 0; Index < Attacks.Num(); ++Index)
	{
		const TSharedPtr<FSimpleJson> Entry = AsEntryObject(*this, Attacks[Index], Index);
		if (!Entry.IsValid())
		{
			continue;
		}
		for (const TCHAR* Field : RequiredStringFields)
		{
			TestTrue(FString::Printf(TEXT("attack '%s' has string field '%s'"), *EntryName(Entry), Field),
				EntryHasFieldOfType(Entry, Field, FSimpleJson::EKind::String));
		}
		for (const TCHAR* Field : RequiredNumberFields)
		{
			TestTrue(FString::Printf(TEXT("attack '%s' has number field '%s'"), *EntryName(Entry), Field),
				EntryHasFieldOfType(Entry, Field, FSimpleJson::EKind::Number));
		}
		for (const TCHAR* Field : RequiredVectorFields)
		{
			TestTrue(FString::Printf(TEXT("attack '%s' has 3-component number array '%s'"), *EntryName(Entry), Field),
				EntryHasFieldOfType(Entry, Field, FSimpleJson::EKind::Array));
		}
		for (const TCHAR* Field : RequiredIdArrayFields)
		{
			TestTrue(FString::Printf(TEXT("attack '%s' has string array field '%s'"), *EntryName(Entry), Field),
				EntryHasFieldOfType(Entry, Field, FSimpleJson::EKind::Array));
		}
		for (const TCHAR* Field : RequiredBoolFields)
		{
			TestTrue(FString::Printf(TEXT("attack '%s' has boolean field '%s'"), *EntryName(Entry), Field),
				EntryHasFieldOfType(Entry, Field, FSimpleJson::EKind::Boolean));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_008NextAttackReferencesResolveWithinSet,
	"UEMMO.Tasks.M1_008.NextAttackReferencesResolveWithinSet",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_008NextAttackReferencesResolveWithinSet::RunTest(const FString& Parameters)
{
	FString LoadError;
	TSharedPtr<FSimpleJson> Root;
	if (!LoadCatalogRoot(LoadError, Root))
	{
		AddError(LoadError);
		return true;
	}
	TArray<TSharedPtr<FSimpleJson>> Attacks;
	if (!GetAttacksOrReport(*this, Root, Attacks))
	{
		return true;
	}

	TSet<FString> KnownIds;
	for (int32 Index = 0; Index < Attacks.Num(); ++Index)
	{
		const TSharedPtr<FSimpleJson> Entry = AsEntryObject(*this, Attacks[Index], Index);
		if (Entry.IsValid())
		{
			const TSharedPtr<FSimpleJson>* Id = FindTyped(Entry, TEXT("attack_id"), FSimpleJson::EKind::String);
			if (Id && !(*Id)->String.IsEmpty())
			{
				KnownIds.Add((*Id)->String);
			}
		}
	}

	for (int32 Index = 0; Index < Attacks.Num(); ++Index)
	{
		const TSharedPtr<FSimpleJson> Entry = AsEntryObject(*this, Attacks[Index], Index);
		if (!Entry.IsValid())
		{
			continue;
		}
		const TSharedPtr<FSimpleJson>* NextIds = FindTyped(Entry, TEXT("next_attack_ids"), FSimpleJson::EKind::Array);
		if (!NextIds)
		{
			AddError(FString::Printf(TEXT("attack '%s': missing or mistyped array field 'next_attack_ids'"),
				*EntryName(Entry)));
			continue;
		}
		for (const TSharedPtr<FSimpleJson>& Next : (*NextIds)->Array)
		{
			if (!Next.IsValid() || Next->Kind != FSimpleJson::EKind::String)
			{
				AddError(FString::Printf(TEXT("attack '%s': 'next_attack_ids' holds a non-string element"),
					*EntryName(Entry)));
				continue;
			}
			const FString NextId = Next->String;
			TestTrue(FString::Printf(TEXT("attack '%s' references known next id '%s'"), *EntryName(Entry), *NextId),
				KnownIds.Contains(NextId));
		}

		// cancel_window_allows entries must stay inside the known cancel kinds.
		const TSharedPtr<FSimpleJson>* Allows = FindTyped(Entry, TEXT("cancel_window_allows"), FSimpleJson::EKind::Array);
		if (!Allows)
		{
			AddError(FString::Printf(TEXT("attack '%s': missing or mistyped array field 'cancel_window_allows'"),
				*EntryName(Entry)));
			continue;
		}
		for (const TSharedPtr<FSimpleJson>& Kind : (*Allows)->Array)
		{
			if (!Kind.IsValid() || Kind->Kind != FSimpleJson::EKind::String ||
				!(Kind->String == TEXT("attack") || Kind->String == TEXT("jump")))
			{
				AddError(FString::Printf(TEXT("attack '%s': 'cancel_window_allows' holds an unknown kind (allowed: attack, jump)"),
					*EntryName(Entry)));
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_008EveryEntryPassesValidateAttackDefinition,
	"UEMMO.Tasks.M1_008.EveryEntryPassesValidateAttackDefinition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_008EveryEntryPassesValidateAttackDefinition::RunTest(const FString& Parameters)
{
	FString LoadError;
	TSharedPtr<FSimpleJson> Root;
	if (!LoadCatalogRoot(LoadError, Root))
	{
		AddError(LoadError);
		return true;
	}
	TArray<TSharedPtr<FSimpleJson>> Attacks;
	if (!GetAttacksOrReport(*this, Root, Attacks))
	{
		return true;
	}

	for (int32 Index = 0; Index < Attacks.Num(); ++Index)
	{
		const TSharedPtr<FSimpleJson> Entry = AsEntryObject(*this, Attacks[Index], Index);
		if (!Entry.IsValid())
		{
			continue;
		}
		UAttackDefinition* Attack = MakeDefinitionFromJson(*this, Entry);
		if (!Attack)
		{
			continue;
		}
		FText Errors;
		TestTrue(FString::Printf(TEXT("attack '%s' passes single-entry validation"), *EntryName(Entry)),
			ValidateAttackDefinition(*Attack, Errors));
		TestTrue(FString::Printf(TEXT("attack '%s' reports no validation errors"), *EntryName(Entry)),
			Errors.IsEmpty());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_008PlaceholderFlagsMatchDesignMapping,
	"UEMMO.Tasks.M1_008.PlaceholderFlagsMatchDesignMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_008PlaceholderFlagsMatchDesignMapping::RunTest(const FString& Parameters)
{
	FString LoadError;
	TSharedPtr<FSimpleJson> Root;
	if (!LoadCatalogRoot(LoadError, Root))
	{
		AddError(LoadError);
		return true;
	}
	TArray<TSharedPtr<FSimpleJson>> Attacks;
	if (!GetAttacksOrReport(*this, Root, Attacks))
	{
		return true;
	}

	// Design section 7: the two lights use the existing MM_Attack_01/02 (real),
	// launcher and aerial_01 are declared placeholders.
	TMap<FString, bool> ExpectedPlaceholder;
	ExpectedPlaceholder.Add(TEXT("light_01"), false);
	ExpectedPlaceholder.Add(TEXT("light_02"), false);
	ExpectedPlaceholder.Add(TEXT("launcher"), true);
	ExpectedPlaceholder.Add(TEXT("aerial_01"), true);

	for (int32 Index = 0; Index < Attacks.Num(); ++Index)
	{
		const TSharedPtr<FSimpleJson> Entry = AsEntryObject(*this, Attacks[Index], Index);
		if (!Entry.IsValid())
		{
			continue;
		}
		const TSharedPtr<FSimpleJson>* Id = FindTyped(Entry, TEXT("attack_id"), FSimpleJson::EKind::String);
		if (!Id || !ExpectedPlaceholder.Contains((*Id)->String))
		{
			continue; // ID-set problems are reported by the catalog test.
		}
		const FString IdString = (*Id)->String;
		bool bPlaceholder = true;
		if (!GetBoolOrReport(*this, Entry, TEXT("placeholder_animation"), bPlaceholder))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("attack '%s' placeholder_animation matches the design mapping"), *IdString),
			bPlaceholder, ExpectedPlaceholder.FindChecked(IdString));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_008EveryEntryHasAnimationExtentAndImpulses,
	"UEMMO.Tasks.M1_008.EveryEntryHasAnimationExtentAndImpulses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_008EveryEntryHasAnimationExtentAndImpulses::RunTest(const FString& Parameters)
{
	FString LoadError;
	TSharedPtr<FSimpleJson> Root;
	if (!LoadCatalogRoot(LoadError, Root))
	{
		AddError(LoadError);
		return true;
	}
	TArray<TSharedPtr<FSimpleJson>> Attacks;
	if (!GetAttacksOrReport(*this, Root, Attacks))
	{
		return true;
	}

	for (int32 Index = 0; Index < Attacks.Num(); ++Index)
	{
		const TSharedPtr<FSimpleJson> Entry = AsEntryObject(*this, Attacks[Index], Index);
		if (!Entry.IsValid())
		{
			continue;
		}
		const FString Name = EntryName(Entry);

		FString AnimationPath;
		if (GetStringOrReport(*this, Entry, TEXT("animation_path"), AnimationPath))
		{
			TestTrue(FString::Printf(TEXT("attack '%s' has a non-empty /Game/ animation_path"), *Name),
				!AnimationPath.IsEmpty() && AnimationPath.StartsWith(TEXT("/Game/")));
		}

		FVector HalfExtent = FVector::ZeroVector;
		if (GetVectorOrReport(*this, Entry, TEXT("hit_half_extent_cm"), HalfExtent))
		{
			TestTrue(FString::Printf(TEXT("attack '%s' hit_half_extent_cm components are all > 0"), *Name),
				HalfExtent.X > 0.0f && HalfExtent.Y > 0.0f && HalfExtent.Z > 0.0f);
			TestEqual<FVector>(FString::Printf(TEXT("attack '%s' hit_half_extent_cm matches the contract box"), *Name),
				HalfExtent, FVector(85.0f, 50.0f, 70.0f));
		}

		FVector HitOffset = FVector::ZeroVector;
		if (GetVectorOrReport(*this, Entry, TEXT("hit_offset_cm"), HitOffset))
		{
			TestEqual<FVector>(FString::Printf(TEXT("attack '%s' hit_offset_cm matches the contract offset"), *Name),
				HitOffset, FVector(95.0f, 0.0f, 90.0f));
		}

		double Coefficient = 0.0;
		if (GetNumberOrReport(*this, Entry, TEXT("attack_coefficient"), Coefficient))
		{
			TestEqual(FString::Printf(TEXT("attack '%s' attack_coefficient is 1.0"), *Name), Coefficient, 1.0);
		}

		double Knockback = -1.0;
		double Launch = -1.0;
		double HitStun = -1.0;
		double HitStop = -1.0;
		const bool bImpulsesRead =
			GetNumberOrReport(*this, Entry, TEXT("knockback_cm_per_s"), Knockback) &&
			GetNumberOrReport(*this, Entry, TEXT("launch_cm_per_s"), Launch) &&
			GetNumberOrReport(*this, Entry, TEXT("hit_stun_seconds"), HitStun) &&
			GetNumberOrReport(*this, Entry, TEXT("hit_stop_seconds"), HitStop);
		if (bImpulsesRead)
		{
			TestTrue(FString::Printf(TEXT("attack '%s' impulse and stun fields are all >= 0"), *Name),
				Knockback >= 0.0 && Launch >= 0.0 && HitStun >= 0.0 && HitStop >= 0.0);
			TestEqual(FString::Printf(TEXT("attack '%s' hit_stop_seconds is 0.04"), *Name), HitStop, 0.04);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_008ContractFrameAndDamageValuesMatch,
	"UEMMO.Tasks.M1_008.ContractFrameAndDamageValuesMatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_008ContractFrameAndDamageValuesMatch::RunTest(const FString& Parameters)
{
	FString LoadError;
	TSharedPtr<FSimpleJson> Root;
	if (!LoadCatalogRoot(LoadError, Root))
	{
		AddError(LoadError);
		return true;
	}
	TArray<TSharedPtr<FSimpleJson>> Attacks;
	if (!GetAttacksOrReport(*this, Root, Attacks))
	{
		return true;
	}

	// Interface contract section 3 initial values:
	// light_01=(26,[7,11),[12,24),10) light_02=(32,[9,14),[16,29),14)
	// launcher=(40,[12,17),[18,32),18) aerial_01=(28,[6,10),[12,23),12)
	struct FExpectedEntry
	{
		const TCHAR* Id;
		int32 Duration;
		int32 ActiveStart;
		int32 ActiveEnd;
		int32 CancelStart;
		int32 CancelEnd;
		double BaseDamage;
	};
	const FExpectedEntry Expected[4] =
	{
		{ TEXT("light_01"), 26, 7, 11, 12, 24, 10.0 },
		{ TEXT("light_02"), 32, 9, 14, 16, 29, 14.0 },
		{ TEXT("launcher"), 40, 12, 17, 18, 32, 18.0 },
		{ TEXT("aerial_01"), 28, 6, 10, 12, 23, 12.0 }
	};

	TMap<FString, TSharedPtr<FSimpleJson>> EntriesById;
	for (int32 Index = 0; Index < Attacks.Num(); ++Index)
	{
		const TSharedPtr<FSimpleJson> Entry = AsEntryObject(*this, Attacks[Index], Index);
		if (!Entry.IsValid())
		{
			continue;
		}
		const TSharedPtr<FSimpleJson>* Id = FindTyped(Entry, TEXT("attack_id"), FSimpleJson::EKind::String);
		if (Id && !(*Id)->String.IsEmpty())
		{
			EntriesById.Add((*Id)->String, Entry);
		}
	}

	for (const FExpectedEntry& Want : Expected)
	{
		const TSharedPtr<FSimpleJson>* Entry = EntriesById.Find(Want.Id);
		if (!Entry)
		{
			AddError(FString::Printf(TEXT("expected attack '%s' is missing from the catalog"), Want.Id));
			continue;
		}
		double Duration = 0.0;
		double ActiveStart = 0.0;
		double ActiveEnd = 0.0;
		double CancelStart = 0.0;
		double CancelEnd = 0.0;
		double BaseDamage = 0.0;
		if (!GetNumberOrReport(*this, *Entry, TEXT("duration_frames"), Duration) ||
			!GetNumberOrReport(*this, *Entry, TEXT("active_start_frame"), ActiveStart) ||
			!GetNumberOrReport(*this, *Entry, TEXT("active_end_frame"), ActiveEnd) ||
			!GetNumberOrReport(*this, *Entry, TEXT("cancel_start_frame"), CancelStart) ||
			!GetNumberOrReport(*this, *Entry, TEXT("cancel_end_frame"), CancelEnd) ||
			!GetNumberOrReport(*this, *Entry, TEXT("base_damage"), BaseDamage))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("attack '%s' duration_frames"), Want.Id),
			static_cast<int32>(Duration), Want.Duration);
		TestEqual(FString::Printf(TEXT("attack '%s' active_start_frame"), Want.Id),
			static_cast<int32>(ActiveStart), Want.ActiveStart);
		TestEqual(FString::Printf(TEXT("attack '%s' active_end_frame"), Want.Id),
			static_cast<int32>(ActiveEnd), Want.ActiveEnd);
		TestEqual(FString::Printf(TEXT("attack '%s' cancel_start_frame"), Want.Id),
			static_cast<int32>(CancelStart), Want.CancelStart);
		TestEqual(FString::Printf(TEXT("attack '%s' cancel_end_frame"), Want.Id),
			static_cast<int32>(CancelEnd), Want.CancelEnd);
		TestEqual(FString::Printf(TEXT("attack '%s' base_damage"), Want.Id), BaseDamage, Want.BaseDamage);
	}
	return true;
}

#endif
