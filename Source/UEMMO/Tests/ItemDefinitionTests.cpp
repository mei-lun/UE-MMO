// M3-001: item definitions and stable instance identities (interface contract
// section 8). Pins the definition/instance split, the FGuid instance identity,
// the snapshot round trip, single-definition validation and the duplicate
// rejection of the minimal catalog, plus the three source entries of
// Data/items.json. Pure checks: this file only reads JSON text and plain
// structs; it never touches UE assets, worlds or wall clocks.
//
// Parsing note (same lesson as M1-008/M2-001): the engine's Json module
// headers compile through Engine's public dependency, but UnrealBuildTool does
// not put the Json import library on the UEMMO link line, so
// FJsonValue/FJsonObject symbols fail to link (LNK2019). UEMMO.Build.cs is
// outside this task's file range, so the tests use the small hand-rolled
// parser below (objects, arrays, strings with escapes, numbers, booleans,
// null) built on Core types only.

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"

#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_001
{
	/** Minimal JSON value: just enough shape for the item data file. */
	class FItemJsonValue
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
		TArray<TSharedPtr<FItemJsonValue>> Array;
		TMap<FString, TSharedPtr<FItemJsonValue>> Object;
	};

	/** Recursive descent JSON parser (RFC 8259 subset; \u escapes decode to one TCHAR). */
	class FItemJsonParser
	{
	public:
		static bool Parse(const FString& Text, TSharedPtr<FItemJsonValue>& OutRoot, FString& OutError)
		{
			FItemJsonParser Parser(Text);
			Parser.SkipWhitespace();
			TSharedPtr<FItemJsonValue> Root = Parser.ParseValue(OutError);
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
		explicit FItemJsonParser(const FString& InText)
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
				return 10 + static_cast<int32>(Character - TEXT('0'));
			}
			if (Character >= TEXT('A') && Character <= TEXT('F'))
			{
				return 10 + static_cast<int32>(Character - TEXT('A'));
			}
			return -1;
		}

		TSharedPtr<FItemJsonValue> ParseValue(FString& OutError)
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

		TSharedPtr<FItemJsonValue> ParseObject(FString& OutError)
		{
			if (!Expect(TEXT('{'), TEXT("to start an object"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FItemJsonValue> Value = MakeShared<FItemJsonValue>();
			Value->Kind = FItemJsonValue::EKind::Object;
			SkipWhitespace();
			if (Consume(TEXT('}')))
			{
				return Value;
			}
			while (true)
			{
				SkipWhitespace();
				TSharedPtr<FItemJsonValue> Key = ParseString(OutError);
				if (!Key.IsValid())
				{
					return nullptr;
				}
				SkipWhitespace();
				if (!Expect(TEXT(':'), TEXT("between object key and value"), OutError))
				{
					return nullptr;
				}
				TSharedPtr<FItemJsonValue> Element = ParseValue(OutError);
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

		TSharedPtr<FItemJsonValue> ParseArray(FString& OutError)
		{
			if (!Expect(TEXT('['), TEXT("to start an array"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FItemJsonValue> Value = MakeShared<FItemJsonValue>();
			Value->Kind = FItemJsonValue::EKind::Array;
			SkipWhitespace();
			if (Consume(TEXT(']')))
			{
				return Value;
			}
			while (true)
			{
				TSharedPtr<FItemJsonValue> Element = ParseValue(OutError);
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

		TSharedPtr<FItemJsonValue> ParseString(FString& OutError)
		{
			if (!Expect(TEXT('"'), TEXT("to start a string"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FItemJsonValue> Value = MakeShared<FItemJsonValue>();
			Value->Kind = FItemJsonValue::EKind::String;
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

		TSharedPtr<FItemJsonValue> ParseBoolean(FString& OutError)
		{
			const TSharedPtr<FItemJsonValue> Value = MakeShared<FItemJsonValue>();
			if (MatchLiteral(TEXT("true"), 4))
			{
				Value->Kind = FItemJsonValue::EKind::Boolean;
				Value->Boolean = true;
				Position += 4;
				return Value;
			}
			if (MatchLiteral(TEXT("false"), 5))
			{
				Value->Kind = FItemJsonValue::EKind::Boolean;
				Value->Boolean = false;
				Position += 5;
				return Value;
			}
			OutError = FString::Printf(TEXT("invalid literal at offset %d"), Position);
			return nullptr;
		}

		TSharedPtr<FItemJsonValue> ParseNull(FString& OutError)
		{
			if (MatchLiteral(TEXT("null"), 4))
			{
				const TSharedPtr<FItemJsonValue> Value = MakeShared<FItemJsonValue>();
				Value->Kind = FItemJsonValue::EKind::Null;
				Position += 4;
				return Value;
			}
			OutError = FString::Printf(TEXT("invalid literal at offset %d"), Position);
			return nullptr;
		}

		TSharedPtr<FItemJsonValue> ParseNumber(FString& OutError)
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
			const TSharedPtr<FItemJsonValue> Value = MakeShared<FItemJsonValue>();
			Value->Kind = FItemJsonValue::EKind::Number;
			Value->Number = FCString::Atod(*Source.Mid(Start, Position - Start));
			return Value;
		}
	};

	static FString GetItemsJsonPath()
	{
		return FPaths::ProjectDir() / TEXT("Data") / TEXT("items.json");
	}

	static bool LoadJsonRoot(const FString& Path, FString& OutError, TSharedPtr<FItemJsonValue>& OutRoot)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			OutError = FString::Printf(TEXT("could not read the JSON data file at %s"), *Path);
			return false;
		}
		TSharedPtr<FItemJsonValue> Root;
		if (!FItemJsonParser::Parse(Text, Root, OutError))
		{
			OutError = FString::Printf(TEXT("could not parse JSON in %s: %s"), *Path, *OutError);
			return false;
		}
		OutRoot = Root;
		return true;
	}

	/** Returns the field only when present with exactly the requested JSON kind. */
	static const TSharedPtr<FItemJsonValue>* FindTyped(const TSharedPtr<FItemJsonValue>& Object, const TCHAR* Name,
		FItemJsonValue::EKind Kind)
	{
		const TSharedPtr<FItemJsonValue>* Value = Object->Object.Find(Name);
		if (!Value || !Value->IsValid() || (*Value)->Kind != Kind)
		{
			return nullptr;
		}
		return Value;
	}

	static FString ItemEntryName(const TSharedPtr<FItemJsonValue>& Entry)
	{
		const TSharedPtr<FItemJsonValue>* Id = FindTyped(Entry, TEXT("definition_id"), FItemJsonValue::EKind::String);
		return (Id && !(*Id)->String.IsEmpty()) ? (*Id)->String : FString(TEXT("<unnamed entry>"));
	}

	static bool GetStringOrReport(FAutomationTestBase& Test, const TSharedPtr<FItemJsonValue>& Entry,
		const TCHAR* Name, FString& OutValue)
	{
		const TSharedPtr<FItemJsonValue>* Value = FindTyped(Entry, Name, FItemJsonValue::EKind::String);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("item '%s': missing or mistyped string field '%s'"),
				*ItemEntryName(Entry), Name));
			return false;
		}
		OutValue = (*Value)->String;
		return true;
	}

	static bool GetNumberOrReport(FAutomationTestBase& Test, const TSharedPtr<FItemJsonValue>& Entry,
		const TCHAR* Name, double& OutValue)
	{
		const TSharedPtr<FItemJsonValue>* Value = FindTyped(Entry, Name, FItemJsonValue::EKind::Number);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("item '%s': missing or mistyped number field '%s'"),
				*ItemEntryName(Entry), Name));
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

	/** Builds a valid weapon_training definition in memory (independent of the JSON file). */
	static FItemDefinition MakeValidSwordDefinition()
	{
		FItemDefinition Definition;
		Definition.DefinitionId = TEXT("weapon_training");
		Definition.DisplayName = TEXT("Training Sword");
		Definition.Slot = EItemSlot::Weapon;
		Definition.BaseStats.Attack = 5.0f;
		Definition.BaseStats.Defense = 0.0f;
		Definition.BaseStats.MaxHP = 0.0f;
		Definition.IconPath = TEXT("");
		Definition.Rarity = EItemRarity::Normal;
		return Definition;
	}

	/** Reads one item entry into FItemDefinition; returns false and reports when fields are missing. */
	static bool MakeDefinitionFromJson(FAutomationTestBase& Test, const TSharedPtr<FItemJsonValue>& Entry,
		FItemDefinition& OutDefinition)
	{
		FString DefinitionId;
		FString DisplayName;
		FString SlotText;
		FString IconPath;
		double RarityNumber = 0.0;
		double Attack = 0.0;
		double Defense = 0.0;
		double MaxHP = 0.0;

		const TSharedPtr<FItemJsonValue>* Stats = FindTyped(Entry, TEXT("base_stats"), FItemJsonValue::EKind::Object);
		if (!Stats || !Stats->IsValid())
		{
			Test.AddError(FString::Printf(TEXT("item '%s': missing or mistyped object field 'base_stats'"),
				*ItemEntryName(Entry)));
			return false;
		}

		const bool bComplete =
			GetStringOrReport(Test, Entry, TEXT("definition_id"), DefinitionId) &&
			GetStringOrReport(Test, Entry, TEXT("display_name"), DisplayName) &&
			GetStringOrReport(Test, Entry, TEXT("slot"), SlotText) &&
			GetStringOrReport(Test, Entry, TEXT("icon_path"), IconPath) &&
			GetNumberOrReport(Test, Entry, TEXT("rarity"), RarityNumber) &&
			GetNumberOrReport(Test, *Stats, TEXT("attack"), Attack) &&
			GetNumberOrReport(Test, *Stats, TEXT("defense"), Defense) &&
			GetNumberOrReport(Test, *Stats, TEXT("max_hp"), MaxHP);
		if (!bComplete)
		{
			return false;
		}

		EItemSlot Slot;
		if (!ParseItemSlot(SlotText, Slot))
		{
			Test.AddError(FString::Printf(TEXT("item '%s': slot '%s' is not one of Weapon/Armor/Accessory"),
				*ItemEntryName(Entry), *SlotText));
			return false;
		}

		const int32 RarityWhole = static_cast<int32>(RarityNumber);
		if (RarityWhole < 1 || RarityWhole > 3 || !NearlyEqual(RarityNumber, static_cast<double>(RarityWhole)))
		{
			Test.AddError(FString::Printf(TEXT("item '%s': rarity %s is not one of 1/2/3"),
				*ItemEntryName(Entry), *FString::SanitizeFloat(RarityNumber)));
			return false;
		}

		OutDefinition = FItemDefinition();
		OutDefinition.DefinitionId = FName(*DefinitionId);
		OutDefinition.DisplayName = DisplayName;
		OutDefinition.Slot = Slot;
		OutDefinition.BaseStats.Attack = static_cast<float>(Attack);
		OutDefinition.BaseStats.Defense = static_cast<float>(Defense);
		OutDefinition.BaseStats.MaxHP = static_cast<float>(MaxHP);
		OutDefinition.IconPath = IconPath;
		OutDefinition.Rarity = static_cast<EItemRarity>(RarityWhole);
		return true;
	}
}

using namespace UE::UEMMO::Tasks::M3_001;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_001SameDefinitionYieldsDistinctInstanceIds,
	"UEMMO.Tasks.M3_001.SameDefinitionYieldsDistinctInstanceIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_001SameDefinitionYieldsDistinctInstanceIds::RunTest(const FString& Parameters)
{
	// Two MakeItemInstance calls on one definition must produce two owned
	// occurrences: different FGuid identities, shared DefinitionId.
	const FItemDefinition Sword = MakeValidSwordDefinition();
	FString Errors;
	TestTrue(TEXT("the in-memory weapon_training definition passes ValidateItemDefinition"),
		ValidateItemDefinition(Sword, Errors));
	TestTrue(TEXT("the in-memory weapon_training definition reports no validation errors"), Errors.IsEmpty());

	const FItemInstance First = MakeItemInstance(Sword, 11);
	const FItemInstance Second = MakeItemInstance(Sword, 11);

	TestTrue(TEXT("the first instance id is a valid FGuid"), First.InstanceId.IsValid());
	TestTrue(TEXT("the second instance id is a valid FGuid"), Second.InstanceId.IsValid());
	TestTrue(TEXT("two instances of the same definition carry different InstanceIds"),
		First.InstanceId != Second.InstanceId);
	TestEqual(TEXT("both instances share the same DefinitionId"), First.DefinitionId, Second.DefinitionId);
	TestEqual(TEXT("the shared DefinitionId is weapon_training"), First.DefinitionId, FName(TEXT("weapon_training")));

	// RollSeed passes through unchanged; M3-001 copies BaseStats verbatim
	// (the seeded roll belongs to M3-007) and items start at level 1.
	TestEqual(TEXT("RollSeed passes through to the first instance"), First.RollSeed, static_cast<int64>(11));
	TestEqual(TEXT("RollSeed passes through to the second instance"), Second.RollSeed, static_cast<int64>(11));
	TestEqual(TEXT("RolledStats.Attack equals BaseStats.Attack (5)"), First.RolledStats.Attack, 5.0f);
	TestEqual(TEXT("RolledStats.Defense equals BaseStats.Defense (0)"), First.RolledStats.Defense, 0.0f);
	TestEqual(TEXT("RolledStats.MaxHP equals BaseStats.MaxHP (0)"), First.RolledStats.MaxHP, 0.0f);
	TestEqual(TEXT("a fresh instance starts at Level 1"), First.Level, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_001SnapshotRoundTripPreservesInstanceId,
	"UEMMO.Tasks.M3_001.SnapshotRoundTripPreservesInstanceId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_001SnapshotRoundTripPreservesInstanceId::RunTest(const FString& Parameters)
{
	// Serialize -> restore must keep every business field, above all the
	// InstanceId: a saved item may never come back as a different identity.
	const FItemDefinition Sword = MakeValidSwordDefinition();
	FItemInstance Source = MakeItemInstance(Sword, 0x5DEECE66DLL);
	Source.Level = 3;
	Source.RolledStats.Attack = 7.5f;
	Source.RolledStats.MaxHP = 20.0f;

	const FString Snapshot = Source.ToStringSnapshot();
	FItemInstance Restored;
	FString Error;
	TestTrue(TEXT("FromStringSnapshot accepts the snapshot of a live instance"),
		Restored.FromStringSnapshot(Snapshot, &Error));
	TestTrue(TEXT("the round trip reports no error"), Error.IsEmpty());
	TestTrue(TEXT("InstanceId survives the round trip"), Restored.InstanceId == Source.InstanceId);
	TestEqual(TEXT("DefinitionId survives the round trip"), Restored.DefinitionId, Source.DefinitionId);
	TestEqual(TEXT("RollSeed survives the round trip"), Restored.RollSeed, Source.RollSeed);
	TestEqual(TEXT("Level survives the round trip"), Restored.Level, 3);
	TestEqual(TEXT("RolledStats.Attack survives the round trip"), Restored.RolledStats.Attack, 7.5f);
	TestEqual(TEXT("RolledStats.Defense survives the round trip"), Restored.RolledStats.Defense, Source.RolledStats.Defense);
	TestEqual(TEXT("RolledStats.MaxHP survives the round trip"), Restored.RolledStats.MaxHP, 20.0f);
	TestEqual(TEXT("re-serializing the restored instance is byte-identical (canonical form)"),
		Restored.ToStringSnapshot(), Snapshot);

	// A negative seed is a legal int64 and must survive unchanged as well.
	FItemInstance NegativeSeed = MakeItemInstance(Sword, -1234567890123LL);
	FItemInstance NegativeSeedRestored;
	TestTrue(TEXT("FromStringSnapshot accepts a snapshot with a negative RollSeed"),
		NegativeSeedRestored.FromStringSnapshot(NegativeSeed.ToStringSnapshot(), &Error));
	TestEqual(TEXT("a negative RollSeed survives the round trip"),
		NegativeSeedRestored.RollSeed, static_cast<int64>(-1234567890123LL));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_001SnapshotRejectsMalformedPayload,
	"UEMMO.Tasks.M3_001.SnapshotRejectsMalformedPayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_001SnapshotRejectsMalformedPayload::RunTest(const FString& Parameters)
{
	// Corrupt or hand-mangled snapshots must be rejected with an error naming
	// the offending field instead of silently fabricating an identity.
	FItemInstance Out;
	FString Error;
	TestFalse(TEXT("an empty snapshot is rejected"), Out.FromStringSnapshot(TEXT(""), &Error));
	TestFalse(TEXT("an empty snapshot reports an error"), Error.IsEmpty());
	TestTrue(TEXT("a failed restore leaves the target instance untouched"), !Out.InstanceId.IsValid());
	TestFalse(TEXT("a garbage snapshot is rejected"), Out.FromStringSnapshot(TEXT("garbage without fields"), &Error));

	// Missing instance_id: the whole field must be reported as absent.
	const FString WithoutInstanceId = TEXT("UEMMO_ITEM_INSTANCE_V1|definition_id=weapon_training|roll_seed=7|level=1|attack=5|defense=0|max_hp=0");
	TestFalse(TEXT("a snapshot without instance_id is rejected"), Out.FromStringSnapshot(WithoutInstanceId, &Error));
	TestTrue(TEXT("the missing-instance_id error names instance_id"), Error.Contains(TEXT("instance_id")));

	// Corrupted guid text.
	const FString CorruptedId = FString::Printf(TEXT("UEMMO_ITEM_INSTANCE_V1|instance_id=not-a-guid|definition_id=weapon_training|roll_seed=7|level=1|attack=5|defense=0|max_hp=0"));
	TestFalse(TEXT("a snapshot with a corrupted instance_id is rejected"), Out.FromStringSnapshot(CorruptedId, &Error));
	TestTrue(TEXT("the corrupted-instance_id error names instance_id"), Error.Contains(TEXT("instance_id")));

	// The all-zero guid is FGuid() and never a legal identity.
	const FString ZeroId = FString::Printf(TEXT("UEMMO_ITEM_INSTANCE_V1|instance_id=00000000-0000-0000-0000-000000000000|definition_id=weapon_training|roll_seed=7|level=1|attack=5|defense=0|max_hp=0"));
	TestFalse(TEXT("a snapshot with the all-zero instance_id is rejected"), Out.FromStringSnapshot(ZeroId, &Error));
	TestTrue(TEXT("the all-zero-instance_id error names instance_id"), Error.Contains(TEXT("instance_id")));

	// Unknown fields are rejected (strict schema), naming the key.
	const FString FreshId = FGuid::NewGuid().ToString();
	const FString UnknownField = FString::Printf(TEXT("UEMMO_ITEM_INSTANCE_V1|instance_id=%s|definition_id=weapon_training|roll_seed=7|level=1|attack=5|defense=0|max_hp=0|mystery=1"), *FreshId);
	TestFalse(TEXT("a snapshot with an unknown field is rejected"), Out.FromStringSnapshot(UnknownField, &Error));
	TestTrue(TEXT("the unknown-field error names the key"), Error.Contains(TEXT("mystery")));

	// Malformed numbers are rejected, naming the field.
	const FString BadSeed = FString::Printf(TEXT("UEMMO_ITEM_INSTANCE_V1|instance_id=%s|definition_id=weapon_training|roll_seed=abc|level=1|attack=5|defense=0|max_hp=0"), *FreshId);
	TestFalse(TEXT("a snapshot with a non-numeric roll_seed is rejected"), Out.FromStringSnapshot(BadSeed, &Error));
	TestTrue(TEXT("the roll_seed error names roll_seed"), Error.Contains(TEXT("roll_seed")));

	const FString BadLevel = FString::Printf(TEXT("UEMMO_ITEM_INSTANCE_V1|instance_id=%s|definition_id=weapon_training|roll_seed=7|level=x|attack=5|defense=0|max_hp=0"), *FreshId);
	TestFalse(TEXT("a snapshot with a non-numeric level is rejected"), Out.FromStringSnapshot(BadLevel, &Error));
	TestTrue(TEXT("the level error names level"), Error.Contains(TEXT("level")));

	const FString ZeroLevel = FString::Printf(TEXT("UEMMO_ITEM_INSTANCE_V1|instance_id=%s|definition_id=weapon_training|roll_seed=7|level=0|attack=5|defense=0|max_hp=0"), *FreshId);
	TestFalse(TEXT("a snapshot with level=0 is rejected"), Out.FromStringSnapshot(ZeroLevel, &Error));
	TestTrue(TEXT("the level=0 error names level"), Error.Contains(TEXT("level")));

	// A wrong version tag is rejected outright.
	const FString WrongTag = TEXT("SOME_OTHER_FORMAT|instance_id=0");
	TestFalse(TEXT("a snapshot with a foreign version tag is rejected"), Out.FromStringSnapshot(WrongTag, &Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_001RejectsInvalidSlotsAndNegativeStats,
	"UEMMO.Tasks.M3_001.RejectsInvalidSlotsAndNegativeStats",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_001RejectsInvalidSlotsAndNegativeStats::RunTest(const FString& Parameters)
{
	// Every rejected mutation must fail validation and name the offending field.
	FString Errors;

	FItemDefinition OutOfEnumSlot = MakeValidSwordDefinition();
	OutOfEnumSlot.Slot = static_cast<EItemSlot>(200);
	TestFalse(TEXT("an out-of-enum Slot value is rejected"), ValidateItemDefinition(OutOfEnumSlot, Errors));
	TestTrue(TEXT("the out-of-enum Slot error names Slot"), Errors.Contains(TEXT("Slot")));

	EItemSlot Parsed = EItemSlot::Weapon;
	TestFalse(TEXT("an empty slot string is rejected by ParseItemSlot"), ParseItemSlot(TEXT(""), Parsed));
	TestFalse(TEXT("an unknown slot string is rejected by ParseItemSlot"), ParseItemSlot(TEXT("Trinket"), Parsed));
	TestTrue(TEXT("ParseItemSlot accepts 'Weapon'"), ParseItemSlot(TEXT("Weapon"), Parsed) && Parsed == EItemSlot::Weapon);
	TestTrue(TEXT("ParseItemSlot is case-insensitive"),
		ParseItemSlot(TEXT("armor"), Parsed) && Parsed == EItemSlot::Armor);

	FItemDefinition NegativeAttack = MakeValidSwordDefinition();
	NegativeAttack.BaseStats.Attack = -1.0f;
	TestFalse(TEXT("Attack=-1 is rejected"), ValidateItemDefinition(NegativeAttack, Errors));
	TestTrue(TEXT("Attack=-1 error names Attack"), Errors.Contains(TEXT("Attack")));

	FItemDefinition NegativeDefense = MakeValidSwordDefinition();
	NegativeDefense.BaseStats.Defense = -0.5f;
	TestFalse(TEXT("Defense=-0.5 is rejected"), ValidateItemDefinition(NegativeDefense, Errors));
	TestTrue(TEXT("Defense=-0.5 error names Defense"), Errors.Contains(TEXT("Defense")));

	FItemDefinition NegativeMaxHP = MakeValidSwordDefinition();
	NegativeMaxHP.BaseStats.MaxHP = -20.0f;
	TestFalse(TEXT("MaxHP=-20 is rejected"), ValidateItemDefinition(NegativeMaxHP, Errors));
	TestTrue(TEXT("MaxHP=-20 error names MaxHP"), Errors.Contains(TEXT("MaxHP")));

	FItemDefinition NotFinite = MakeValidSwordDefinition();
	NotFinite.BaseStats.Attack = std::numeric_limits<float>::quiet_NaN();
	TestFalse(TEXT("a non-finite Attack is rejected"), ValidateItemDefinition(NotFinite, Errors));
	TestTrue(TEXT("the non-finite Attack error names Attack"), Errors.Contains(TEXT("Attack")));

	FItemDefinition OutOfEnumRarity = MakeValidSwordDefinition();
	OutOfEnumRarity.Rarity = static_cast<EItemRarity>(9);
	TestFalse(TEXT("an out-of-enum Rarity value is rejected"), ValidateItemDefinition(OutOfEnumRarity, Errors));
	TestTrue(TEXT("the out-of-enum Rarity error names Rarity"), Errors.Contains(TEXT("Rarity")));

	FItemDefinition Empty;
	TestFalse(TEXT("a default-constructed definition (empty DefinitionId) is rejected"),
		ValidateItemDefinition(Empty, Errors));
	TestTrue(TEXT("the default-constructed error names DefinitionId"), Errors.Contains(TEXT("DefinitionId")));

	// Several problems at once are collected and joined with "; ".
	FItemDefinition Broken = MakeValidSwordDefinition();
	Broken.DefinitionId = NAME_None;
	Broken.BaseStats.Attack = -1.0f;
	Broken.BaseStats.MaxHP = -2.0f;
	TestFalse(TEXT("a definition with multiple problems is rejected"), ValidateItemDefinition(Broken, Errors));
	TestTrue(TEXT("DefinitionId problem is included"), Errors.Contains(TEXT("DefinitionId")));
	TestTrue(TEXT("Attack problem is included"), Errors.Contains(TEXT("Attack")));
	TestTrue(TEXT("MaxHP problem is included"), Errors.Contains(TEXT("MaxHP")));
	TestTrue(TEXT("problems are joined with \"; \""), Errors.Contains(TEXT("; ")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_001CatalogRejectsDuplicateDefinitionIds,
	"UEMMO.Tasks.M3_001.CatalogRejectsDuplicateDefinitionIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_001CatalogRejectsDuplicateDefinitionIds::RunTest(const FString& Parameters)
{
	// The minimal catalog must keep the first registration and reject every
	// later definition that reuses the same DefinitionId.
	FItemDefinitionCatalog Catalog;
	FString Error;
	const FItemDefinition Sword = MakeValidSwordDefinition();

	TestTrue(TEXT("the first weapon_training is accepted"), Catalog.AddDefinition(Sword, &Error));
	TestEqual(TEXT("catalog holds 1 definition"), Catalog.Num(), 1);

	FItemDefinition Duplicate = MakeValidSwordDefinition();
	Duplicate.BaseStats.Attack = 99.0f;
	TestFalse(TEXT("a second definition with the same DefinitionId is rejected"),
		Catalog.AddDefinition(Duplicate, &Error));
	TestTrue(TEXT("the duplicate error names DefinitionId"), Error.Contains(TEXT("DefinitionId")));
	TestEqual(TEXT("the duplicate does not change the catalog size"), Catalog.Num(), 1);

	const FItemDefinition* Stored = Catalog.Find(FName(TEXT("weapon_training")));
	TestTrue(TEXT("Find returns the registered definition"), Stored != nullptr);
	if (Stored)
	{
		TestEqual(TEXT("the stored definition keeps its original stats"), Stored->BaseStats.Attack, 5.0f);
	}

	FItemDefinition Anonymous;
	TestFalse(TEXT("a definition without DefinitionId is rejected"), Catalog.AddDefinition(Anonymous, &Error));
	TestTrue(TEXT("the empty-id error names DefinitionId"), Error.Contains(TEXT("DefinitionId")));
	TestEqual(TEXT("the empty id did not change the catalog size"), Catalog.Num(), 1);

	// Distinct ids coexist; the catalog grows accordingly.
	FItemDefinition Armor = MakeValidSwordDefinition();
	Armor.DefinitionId = TEXT("armor_training");
	Armor.Slot = EItemSlot::Armor;
	Armor.BaseStats.Attack = 0.0f;
	Armor.BaseStats.Defense = 3.0f;
	TestTrue(TEXT("a definition with a fresh DefinitionId is accepted"), Catalog.AddDefinition(Armor, nullptr));
	TestEqual(TEXT("catalog holds 2 definitions"), Catalog.Num(), 2);
	TestTrue(TEXT("Find resolves armor_training after the second add"), Catalog.Find(FName(TEXT("armor_training"))) != nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_001ItemsJsonLoadsThreeDefinitions,
	"UEMMO.Tasks.M3_001.ItemsJsonLoadsThreeDefinitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_001ItemsJsonLoadsThreeDefinitions::RunTest(const FString& Parameters)
{
	// The three design entries load, validate individually, and add to the
	// catalog without duplicate rejections; the pinned design values hold.
	FString LoadError;
	TSharedPtr<FItemJsonValue> Root;
	if (!LoadJsonRoot(GetItemsJsonPath(), LoadError, Root))
	{
		AddError(LoadError);
		return true;
	}

	const TSharedPtr<FItemJsonValue>* SchemaVersion = FindTyped(Root, TEXT("schema_version"), FItemJsonValue::EKind::Number);
	if (SchemaVersion)
	{
		TestEqual(TEXT("schema_version is 1"), (*SchemaVersion)->Number, 1.0);
	}
	else
	{
		AddError(TEXT("missing or mistyped top-level number 'schema_version'"));
	}

	const TSharedPtr<FItemJsonValue>* Items = FindTyped(Root, TEXT("items"), FItemJsonValue::EKind::Array);
	if (!Items)
	{
		AddError(TEXT("missing required top-level array 'items'"));
		return true;
	}
	TestEqual(TEXT("items holds exactly 3 entries"), (*Items)->Array.Num(), 3);
	if ((*Items)->Array.Num() != 3)
	{
		return true;
	}

	FItemDefinitionCatalog Catalog;
	for (const TSharedPtr<FItemJsonValue>& Entry : (*Items)->Array)
	{
		if (!Entry.IsValid() || Entry->Kind != FItemJsonValue::EKind::Object)
		{
			AddError(TEXT("an items entry is not a JSON object"));
			continue;
		}
		FItemDefinition Definition;
		if (!MakeDefinitionFromJson(*this, Entry, Definition))
		{
			continue;
		}
		FString Errors;
		TestTrue(FString::Printf(TEXT("item '%s' passes ValidateItemDefinition"), *Definition.DefinitionId.ToString()),
			ValidateItemDefinition(Definition, Errors));
		TestTrue(FString::Printf(TEXT("item '%s' reports no validation errors"), *Definition.DefinitionId.ToString()),
			Errors.IsEmpty());
		FString CatalogError;
		TestTrue(FString::Printf(TEXT("item '%s' is accepted by the catalog"), *Definition.DefinitionId.ToString()),
			Catalog.AddDefinition(Definition, &CatalogError));
	}

	TestEqual(TEXT("the catalog holds all 3 items (no duplicate DefinitionId)"), Catalog.Num(), 3);

	// Pinned design values of the three training items.
	const FItemDefinition* Weapon = Catalog.Find(FName(TEXT("weapon_training")));
	TestTrue(TEXT("weapon_training is registered"), Weapon != nullptr);
	if (Weapon)
	{
		TestTrue(TEXT("weapon_training slot is Weapon"), Weapon->Slot == EItemSlot::Weapon);
		TestEqual(TEXT("weapon_training Attack is 5"), Weapon->BaseStats.Attack, 5.0f);
		TestEqual(TEXT("weapon_training Defense is 0"), Weapon->BaseStats.Defense, 0.0f);
		TestEqual(TEXT("weapon_training MaxHP is 0"), Weapon->BaseStats.MaxHP, 0.0f);
		TestTrue(TEXT("weapon_training rarity is 1 (Normal)"), Weapon->Rarity == EItemRarity::Normal);
	}

	const FItemDefinition* Armor = Catalog.Find(FName(TEXT("armor_training")));
	TestTrue(TEXT("armor_training is registered"), Armor != nullptr);
	if (Armor)
	{
		TestTrue(TEXT("armor_training slot is Armor"), Armor->Slot == EItemSlot::Armor);
		TestEqual(TEXT("armor_training Attack is 0"), Armor->BaseStats.Attack, 0.0f);
		TestEqual(TEXT("armor_training Defense is 3"), Armor->BaseStats.Defense, 3.0f);
		TestEqual(TEXT("armor_training MaxHP is 0"), Armor->BaseStats.MaxHP, 0.0f);
	}

	const FItemDefinition* Charm = Catalog.Find(FName(TEXT("charm_training")));
	TestTrue(TEXT("charm_training is registered"), Charm != nullptr);
	if (Charm)
	{
		TestTrue(TEXT("charm_training slot is Accessory"), Charm->Slot == EItemSlot::Accessory);
		TestEqual(TEXT("charm_training Attack is 0"), Charm->BaseStats.Attack, 0.0f);
		TestEqual(TEXT("charm_training Defense is 0"), Charm->BaseStats.Defense, 0.0f);
		TestEqual(TEXT("charm_training MaxHP is 20"), Charm->BaseStats.MaxHP, 20.0f);
	}

	// A loaded definition feeds the factory like any other definition.
	if (Weapon)
	{
		const FItemInstance Instance = MakeItemInstance(*Weapon, 1);
		TestEqual(TEXT("an instance of the loaded weapon_training carries its DefinitionId"),
			Instance.DefinitionId, FName(TEXT("weapon_training")));
		TestTrue(TEXT("the instance of the loaded definition has a valid InstanceId"), Instance.InstanceId.IsValid());
	}
	return true;
}

#endif
