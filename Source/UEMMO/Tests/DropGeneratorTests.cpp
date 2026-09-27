// M3-007: fixed-seed drop table and deterministic rewards (interface contract
// section 8). Pins the starter table values (weights 50/30/20), the
// deterministic reward mapping (same RewardSeed + SettlementId -> same
// DefinitionId, same InstanceId, same stats), the failure rules (zero total
// weight, unknown DefinitionId, negative weight), the independence of the
// reward stream from combat randomness, and the consistency of
// Data/drops.json with Data/items.json. Pure checks: this file only reads
// JSON text, plain structs and local random streams; it never touches UE
// assets, worlds or wall clocks.
//
// Parsing note (same lesson as M3-001): the engine's Json module headers
// compile through Engine's public dependency, but UnrealBuildTool does not
// put the Json import library on the UEMMO link line, so FJsonValue/
// FJsonObject symbols fail to link (LNK2019). UEMMO.Build.cs is outside this
// task's file range, so the tests use the small hand-rolled parser below
// (objects, arrays, strings with escapes, numbers, booleans, null) built on
// Core types only.

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Math/RandomStream.h"
#include "Math/UnrealMathUtility.h"

#include "../Items/DropGenerator.h"
#include "../Items/DropTable.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_007
{
	/** Minimal JSON value: just enough shape for the drop/item data files. */
	class FDropJsonValue
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
		TArray<TSharedPtr<FDropJsonValue>> Array;
		TMap<FString, TSharedPtr<FDropJsonValue>> Object;
	};

	/** Recursive descent JSON parser (RFC 8259 subset; \u escapes decode to one TCHAR). */
	class FDropJsonParser
	{
	public:
		static bool Parse(const FString& Text, TSharedPtr<FDropJsonValue>& OutRoot, FString& OutError)
		{
			FDropJsonParser Parser(Text);
			Parser.SkipWhitespace();
			TSharedPtr<FDropJsonValue> Root = Parser.ParseValue(OutError);
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
		explicit FDropJsonParser(const FString& InText)
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

		TSharedPtr<FDropJsonValue> ParseValue(FString& OutError)
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

		TSharedPtr<FDropJsonValue> ParseObject(FString& OutError)
		{
			if (!Expect(TEXT('{'), TEXT("to start an object"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FDropJsonValue> Value = MakeShared<FDropJsonValue>();
			Value->Kind = FDropJsonValue::EKind::Object;
			SkipWhitespace();
			if (Consume(TEXT('}')))
			{
				return Value;
			}
			while (true)
			{
				SkipWhitespace();
				TSharedPtr<FDropJsonValue> Key = ParseString(OutError);
				if (!Key.IsValid())
				{
					return nullptr;
				}
				SkipWhitespace();
				if (!Expect(TEXT(':'), TEXT("between object key and value"), OutError))
				{
					return nullptr;
				}
				TSharedPtr<FDropJsonValue> Element = ParseValue(OutError);
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

		TSharedPtr<FDropJsonValue> ParseArray(FString& OutError)
		{
			if (!Expect(TEXT('['), TEXT("to start an array"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FDropJsonValue> Value = MakeShared<FDropJsonValue>();
			Value->Kind = FDropJsonValue::EKind::Array;
			SkipWhitespace();
			if (Consume(TEXT(']')))
			{
				return Value;
			}
			while (true)
			{
				TSharedPtr<FDropJsonValue> Element = ParseValue(OutError);
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

		TSharedPtr<FDropJsonValue> ParseString(FString& OutError)
		{
			if (!Expect(TEXT('"'), TEXT("to start a string"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FDropJsonValue> Value = MakeShared<FDropJsonValue>();
			Value->Kind = FDropJsonValue::EKind::String;
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

		TSharedPtr<FDropJsonValue> ParseBoolean(FString& OutError)
		{
			const TSharedPtr<FDropJsonValue> Value = MakeShared<FDropJsonValue>();
			if (MatchLiteral(TEXT("true"), 4))
			{
				Value->Kind = FDropJsonValue::EKind::Boolean;
				Value->Boolean = true;
				Position += 4;
				return Value;
			}
			if (MatchLiteral(TEXT("false"), 5))
			{
				Value->Kind = FDropJsonValue::EKind::Boolean;
				Value->Boolean = false;
				Position += 5;
				return Value;
			}
			OutError = FString::Printf(TEXT("invalid literal at offset %d"), Position);
			return nullptr;
		}

		TSharedPtr<FDropJsonValue> ParseNull(FString& OutError)
		{
			if (MatchLiteral(TEXT("null"), 4))
			{
				const TSharedPtr<FDropJsonValue> Value = MakeShared<FDropJsonValue>();
				Value->Kind = FDropJsonValue::EKind::Null;
				Position += 4;
				return Value;
			}
			OutError = FString::Printf(TEXT("invalid literal at offset %d"), Position);
			return nullptr;
		}

		TSharedPtr<FDropJsonValue> ParseNumber(FString& OutError)
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
			const TSharedPtr<FDropJsonValue> Value = MakeShared<FDropJsonValue>();
			Value->Kind = FDropJsonValue::EKind::Number;
			Value->Number = FCString::Atod(*Source.Mid(Start, Position - Start));
			return Value;
		}
	};

	static FString GetDataJsonPath(const TCHAR* FileName)
	{
		return FPaths::ProjectDir() / TEXT("Data") / FileName;
	}

	static bool LoadJsonRoot(const FString& Path, FString& OutError, TSharedPtr<FDropJsonValue>& OutRoot)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			OutError = FString::Printf(TEXT("could not read the JSON data file at %s"), *Path);
			return false;
		}
		TSharedPtr<FDropJsonValue> Root;
		if (!FDropJsonParser::Parse(Text, Root, OutError))
		{
			OutError = FString::Printf(TEXT("could not parse JSON in %s: %s"), *Path, *OutError);
			return false;
		}
		OutRoot = Root;
		return true;
	}

	/** Returns the field only when present with exactly the requested JSON kind. */
	static const TSharedPtr<FDropJsonValue>* FindTyped(const TSharedPtr<FDropJsonValue>& Object, const TCHAR* Name,
		FDropJsonValue::EKind Kind)
	{
		const TSharedPtr<FDropJsonValue>* Value = Object->Object.Find(Name);
		if (!Value || !Value->IsValid() || (*Value)->Kind != Kind)
		{
			return nullptr;
		}
		return Value;
	}

	/** One training definition with the pinned Data/items.json design values. */
	static FItemDefinition MakeTrainingDefinition(const TCHAR* DefinitionId, EItemSlot Slot,
		float Attack, float Defense, float MaxHP)
	{
		FItemDefinition Definition;
		Definition.DefinitionId = FName(DefinitionId);
		Definition.DisplayName = FString(DefinitionId);
		Definition.Slot = Slot;
		Definition.BaseStats.Attack = Attack;
		Definition.BaseStats.Defense = Defense;
		Definition.BaseStats.MaxHP = MaxHP;
		Definition.IconPath = TEXT("");
		Definition.Rarity = EItemRarity::Normal;
		return Definition;
	}

	/** The three training definitions in a fresh catalog (mirrors Data/items.json). */
	static FItemDefinitionCatalog MakeTrainingCatalog()
	{
		FItemDefinitionCatalog Catalog;
		Catalog.AddDefinition(MakeTrainingDefinition(TEXT("weapon_training"), EItemSlot::Weapon, 5.0f, 0.0f, 0.0f), nullptr);
		Catalog.AddDefinition(MakeTrainingDefinition(TEXT("armor_training"), EItemSlot::Armor, 0.0f, 3.0f, 0.0f), nullptr);
		Catalog.AddDefinition(MakeTrainingDefinition(TEXT("charm_training"), EItemSlot::Accessory, 0.0f, 0.0f, 20.0f), nullptr);
		return Catalog;
	}

	/** True when both tables carry the same TableId and the same ordered entries. */
	static bool DropTablesEqual(const FDropTable& A, const FDropTable& B)
	{
		if (A.TableId != B.TableId || A.Entries.Num() != B.Entries.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Entries.Num(); ++Index)
		{
			if (A.Entries[Index].DefinitionId != B.Entries[Index].DefinitionId ||
				A.Entries[Index].Weight != B.Entries[Index].Weight)
			{
				return false;
			}
		}
		return true;
	}
}

using namespace UE::UEMMO::Tasks::M3_007;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_007SameRewardSeedAndSettlementAreDeterministic,
	"UEMMO.Tasks.M3_007.SameRewardSeedAndSettlementAreDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_007SameRewardSeedAndSettlementAreDeterministic::RunTest(const FString& Parameters)
{
	// The core acceptance rule: retrying with the same RewardSeed and
	// SettlementId reproduces the identical instance - same DefinitionId,
	// same deterministic InstanceId, same recorded RollSeed, same stats.
	const FItemDefinitionCatalog Catalog = MakeTrainingCatalog();
	const FDropTable Table = MakeStarterDropTable();
	FString Errors;
	TestTrue(TEXT("the starter drop table passes ValidateDropTable"), ValidateDropTable(Table, Errors));
	TestTrue(TEXT("the starter drop table reports no validation errors"), Errors.IsEmpty());

	const int64 RewardSeed = 0x5DEECE66DLL;
	const uint64 SettlementId = 42;

	FDropRewardResult First = FDropGenerator::GenerateReward(RewardSeed, SettlementId, Table, Catalog);
	TestTrue(TEXT("the first generation succeeds"), First.bSuccess);
	TestTrue(TEXT("the first generation reports no error"), First.Error.IsEmpty());
	TestTrue(TEXT("the generated instance has a valid InstanceId"), First.Instance.InstanceId.IsValid());
	TestEqual(TEXT("the instance starts at Level 1"), First.Instance.Level, 1);
	if (!First.bSuccess)
	{
		return true;
	}

	for (int32 Retry = 0; Retry < 3; ++Retry)
	{
		const FDropRewardResult Again = FDropGenerator::GenerateReward(RewardSeed, SettlementId, Table, Catalog);
		const FString Context = FString::Printf(TEXT("retry %d"), Retry);
		TestTrue(FString::Printf(TEXT("retry %d succeeds"), Retry), Again.bSuccess);
		if (!Again.bSuccess)
		{
			continue;
		}
		TestEqual(TEXT("retry picks the same DefinitionId"), Again.Instance.DefinitionId, First.Instance.DefinitionId);
		TestTrue(TEXT("retry yields the identical InstanceId"), Again.Instance.InstanceId == First.Instance.InstanceId);
		TestEqual(TEXT("retry records the same RollSeed"), Again.Instance.RollSeed, First.Instance.RollSeed);
		TestEqual(TEXT("retry keeps RolledStats.Attack"), Again.Instance.RolledStats.Attack, First.Instance.RolledStats.Attack);
		TestEqual(TEXT("retry keeps RolledStats.Defense"), Again.Instance.RolledStats.Defense, First.Instance.RolledStats.Defense);
		TestEqual(TEXT("retry keeps RolledStats.MaxHP"), Again.Instance.RolledStats.MaxHP, First.Instance.RolledStats.MaxHP);
		TestEqual(TEXT("retry keeps Level"), Again.Instance.Level, First.Instance.Level);
		TestTrue(TEXT("retry serializes to the identical snapshot"),
			Again.Instance.ToStringSnapshot() == First.Instance.ToStringSnapshot());
	}

	// The InstanceId is a function of (SettlementId, RewardIndex), so a
	// different settlement maps to a different stable identity while the same
	// RewardSeed still picks the same kind of item.
	const FDropRewardResult OtherSettlement = FDropGenerator::GenerateReward(RewardSeed, SettlementId + 1, Table, Catalog);
	TestTrue(TEXT("generating for another settlement succeeds"), OtherSettlement.bSuccess);
	if (OtherSettlement.bSuccess)
	{
		TestTrue(TEXT("another settlement gets a valid InstanceId"), OtherSettlement.Instance.InstanceId.IsValid());
		TestTrue(TEXT("another settlement gets a different InstanceId"),
			OtherSettlement.Instance.InstanceId != First.Instance.InstanceId);
		TestEqual(TEXT("the same RewardSeed still picks the same DefinitionId for another settlement"),
			OtherSettlement.Instance.DefinitionId, First.Instance.DefinitionId);
	}

	// The picked definition must be one of the three starter entries.
	const FName Picked = First.Instance.DefinitionId;
	TestTrue(TEXT("the picked DefinitionId is weapon_training, armor_training or charm_training"),
		Picked == FName(TEXT("weapon_training")) || Picked == FName(TEXT("armor_training")) ||
		Picked == FName(TEXT("charm_training")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_007DifferentRewardSeedsFollowIndependentSequences,
	"UEMMO.Tasks.M3_007.DifferentRewardSeedsFollowIndependentSequences",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_007DifferentRewardSeedsFollowIndependentSequences::RunTest(const FString& Parameters)
{
	// Different RewardSeeds follow independent deterministic sequences:
	// interleaved generation must not cross-contaminate, and every seed
	// reproduces its own result on retry.
	const FItemDefinitionCatalog Catalog = MakeTrainingCatalog();
	const FDropTable Table = MakeStarterDropTable();
	const uint64 SettlementId = 7;
	const int64 SeedA = 1001;
	const int64 SeedB = 2002;

	const FDropRewardResult A1 = FDropGenerator::GenerateReward(SeedA, SettlementId, Table, Catalog);
	const FDropRewardResult B1 = FDropGenerator::GenerateReward(SeedB, SettlementId, Table, Catalog);
	TestTrue(TEXT("generation with SeedA succeeds"), A1.bSuccess);
	TestTrue(TEXT("generation with SeedB succeeds"), B1.bSuccess);
	if (!A1.bSuccess || !B1.bSuccess)
	{
		return true;
	}

	// Interleave: A, B, A, B - each result must stay exactly as before.
	const FDropRewardResult A2 = FDropGenerator::GenerateReward(SeedA, SettlementId, Table, Catalog);
	const FDropRewardResult B2 = FDropGenerator::GenerateReward(SeedB, SettlementId, Table, Catalog);
	TestTrue(TEXT("SeedA keeps its result after SeedB was generated in between"),
		A2.Instance.ToStringSnapshot() == A1.Instance.ToStringSnapshot());
	TestTrue(TEXT("SeedB keeps its result after SeedA was generated in between"),
		B2.Instance.ToStringSnapshot() == B1.Instance.ToStringSnapshot());

	// Distinct seeds derive distinct recorded RollSeeds (different purpose
	// stream), so the two reward results cannot be identical objects.
	TestTrue(TEXT("SeedA and SeedB record different RollSeeds"),
		A1.Instance.RollSeed != B1.Instance.RollSeed);
	TestTrue(TEXT("SeedA and SeedB produce different full snapshots"),
		A1.Instance.ToStringSnapshot() != B1.Instance.ToStringSnapshot());

	// A sweep of consecutive seeds: every seed succeeds, reproduces itself and
	// derives its own RollSeed; across the sweep both outcomes of the 50/30/20
	// table actually occur (deterministic for these pinned seeds).
	TSet<int64> SeenRollSeeds;
	TSet<FName> SeenDefinitions;
	for (int64 Seed = 1; Seed <= 24; ++Seed)
	{
		const FDropRewardResult First = FDropGenerator::GenerateReward(Seed, SettlementId, Table, Catalog);
		TestTrue(FString::Printf(TEXT("sweep seed %lld succeeds"), Seed), First.bSuccess);
		if (!First.bSuccess)
		{
			continue;
		}
		const FDropRewardResult Again = FDropGenerator::GenerateReward(Seed, SettlementId, Table, Catalog);
		TestTrue(FString::Printf(TEXT("sweep seed %lld reproduces its result"), Seed),
			Again.Instance.ToStringSnapshot() == First.Instance.ToStringSnapshot());
		SeenRollSeeds.Add(First.Instance.RollSeed);
		SeenDefinitions.Add(First.Instance.DefinitionId);
	}
	TestEqual(TEXT("the 24 swept seeds all derive distinct RollSeeds"), SeenRollSeeds.Num(), 24);
	TestTrue(TEXT("the sweep covers more than one DefinitionId (weighted choice really varies)"),
		SeenDefinitions.Num() >= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_007RejectsZeroTotalUnknownIdAndNegativeWeight,
	"UEMMO.Tasks.M3_007.RejectsZeroTotalUnknownIdAndNegativeWeight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_007RejectsZeroTotalUnknownIdAndNegativeWeight::RunTest(const FString& Parameters)
{
	// Every rejected table must fail generation with an error naming the
	// offending field, and a failed call must not fabricate an instance.
	const FItemDefinitionCatalog Catalog = MakeTrainingCatalog();
	const uint64 SettlementId = 1;
	const int64 RewardSeed = 5;

	// Zero total weight: all-known ids, all weights 0.
	FDropTable ZeroTotal = MakeStarterDropTable();
	for (FDropEntry& Entry : ZeroTotal.Entries)
	{
		Entry.Weight = 0;
	}
	FDropRewardResult Result = FDropGenerator::GenerateReward(RewardSeed, SettlementId, ZeroTotal, Catalog);
	TestFalse(TEXT("a table with total weight 0 is rejected"), Result.bSuccess);
	TestTrue(TEXT("the zero-total error names Weight"), Result.Error.Contains(TEXT("Weight")));
	TestTrue(TEXT("a rejected call produces no valid instance"), !Result.Instance.InstanceId.IsValid());

	// Empty entries: structurally unrollable.
	FDropTable Empty;
	Empty.TableId = TEXT("empty");
	Result = FDropGenerator::GenerateReward(RewardSeed, SettlementId, Empty, Catalog);
	TestFalse(TEXT("a table without entries is rejected"), Result.bSuccess);
	TestTrue(TEXT("the empty-table error names Entries"), Result.Error.Contains(TEXT("Entries")));

	// Negative weight: fails even though the total would be positive.
	FDropTable Negative = MakeStarterDropTable();
	Negative.Entries[1].Weight = -5;
	Result = FDropGenerator::GenerateReward(RewardSeed, SettlementId, Negative, Catalog);
	TestFalse(TEXT("a table with a negative Weight is rejected"), Result.bSuccess);
	TestTrue(TEXT("the negative-weight error names Weight"), Result.Error.Contains(TEXT("Weight")));

	// Unknown DefinitionId: not registered in the catalog.
	FDropTable Unknown = MakeStarterDropTable();
	Unknown.Entries[1].DefinitionId = FName(TEXT("armor_missing"));
	Result = FDropGenerator::GenerateReward(RewardSeed, SettlementId, Unknown, Catalog);
	TestFalse(TEXT("a table with an unknown DefinitionId is rejected"), Result.bSuccess);
	TestTrue(TEXT("the unknown-id error names DefinitionId"), Result.Error.Contains(TEXT("DefinitionId")));
	TestTrue(TEXT("the unknown-id error names the missing id"), Result.Error.Contains(TEXT("armor_missing")));

	// Unknown-only table: a single ghost entry.
	FDropTable Ghost;
	Ghost.TableId = TEXT("ghost");
	Ghost.Entries.Add(FDropEntry{ FName(TEXT("ghost_item")), 10 });
	Result = FDropGenerator::GenerateReward(RewardSeed, SettlementId, Ghost, Catalog);
	TestFalse(TEXT("a table whose only entry is unknown is rejected"), Result.bSuccess);
	TestTrue(TEXT("the ghost-entry error names DefinitionId"), Result.Error.Contains(TEXT("DefinitionId")));

	// Duplicate DefinitionId inside one table: ambiguous weight split.
	FDropTable Duplicate;
	Duplicate.TableId = TEXT("dupe");
	Duplicate.Entries.Add(FDropEntry{ FName(TEXT("weapon_training")), 10 });
	Duplicate.Entries.Add(FDropEntry{ FName(TEXT("weapon_training")), 20 });
	Duplicate.Entries.Add(FDropEntry{ FName(TEXT("charm_training")), 20 });
	Result = FDropGenerator::GenerateReward(RewardSeed, SettlementId, Duplicate, Catalog);
	TestFalse(TEXT("a table with a duplicated DefinitionId is rejected"), Result.bSuccess);
	TestTrue(TEXT("the duplicate error names DefinitionId"), Result.Error.Contains(TEXT("DefinitionId")));

	// Empty DefinitionId in one entry.
	FDropTable Anonymous;
	Anonymous.TableId = TEXT("anon");
	Anonymous.Entries.Add(FDropEntry{ FName(), 10 });
	Result = FDropGenerator::GenerateReward(RewardSeed, SettlementId, Anonymous, Catalog);
	TestFalse(TEXT("a table with an empty DefinitionId entry is rejected"), Result.bSuccess);
	TestTrue(TEXT("the empty-id error names DefinitionId"), Result.Error.Contains(TEXT("DefinitionId")));

	// ValidateDropTable agrees with the generator on the structural cases.
	FString Errors;
	TestFalse(TEXT("ValidateDropTable rejects the negative-weight table"), ValidateDropTable(Negative, Errors));
	TestTrue(TEXT("the validator's negative-weight error names Weight"), Errors.Contains(TEXT("Weight")));
	TestFalse(TEXT("ValidateDropTable rejects the duplicate table"), ValidateDropTable(Duplicate, Errors));
	TestTrue(TEXT("the validator's duplicate error names DefinitionId"), Errors.Contains(TEXT("DefinitionId")));
	TestTrue(TEXT("ValidateDropTable accepts the starter table"), ValidateDropTable(MakeStarterDropTable(), Errors));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_007BattleRandomDoesNotDisturbRewardSequence,
	"UEMMO.Tasks.M3_007.BattleRandomDoesNotDisturbRewardSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_007BattleRandomDoesNotDisturbRewardSequence::RunTest(const FString& Parameters)
{
	// The reward stream is an FRandomStream seeded from RewardSeed only. Heavy
	// combat-side random consumption (instance streams and the engine global
	// stream that FMath::RandRange uses) must not shift the reward sequence.
	const FItemDefinitionCatalog Catalog = MakeTrainingCatalog();
	const FDropTable Table = MakeStarterDropTable();
	const uint64 SettlementId = 9;
	const int64 SeedA = 0x1234;
	const int64 SeedB = 0x5678;

	const FDropRewardResult BaselineA = FDropGenerator::GenerateReward(SeedA, SettlementId, Table, Catalog);
	const FDropRewardResult BaselineB = FDropGenerator::GenerateReward(SeedB, SettlementId, Table, Catalog);
	TestTrue(TEXT("baseline SeedA generation succeeds"), BaselineA.bSuccess);
	TestTrue(TEXT("baseline SeedB generation succeeds"), BaselineB.bSuccess);
	if (!BaselineA.bSuccess || !BaselineB.bSuccess)
	{
		return true;
	}

	// Simulate combat randomness: a dedicated battle stream draws a lot, and
	// the global engine stream (FMath::Rand) draws too - frame-rate dependent
	// call counts change both, and must not change the rewards.
	int32 CombatSink = 0;
	FRandomStream BattleStream(0xBEEF);
	for (int32 Draw = 0; Draw < 1000; ++Draw)
	{
		CombatSink += BattleStream.RandRange(0, 100);
	}
	for (int32 Draw = 0; Draw < 200; ++Draw)
	{
		CombatSink += FMath::Rand();
	}

	const FDropRewardResult AfterA = FDropGenerator::GenerateReward(SeedA, SettlementId, Table, Catalog);
	const FDropRewardResult AfterB = FDropGenerator::GenerateReward(SeedB, SettlementId, Table, Catalog);
	TestTrue(TEXT("post-combat SeedA generation succeeds"), AfterA.bSuccess);
	TestTrue(TEXT("post-combat SeedB generation succeeds"), AfterB.bSuccess);
	TestTrue(TEXT("SeedA reward is unchanged after combat random consumption"),
		AfterA.bSuccess && AfterA.Instance.ToStringSnapshot() == BaselineA.Instance.ToStringSnapshot());
	TestTrue(TEXT("SeedB reward is unchanged after combat random consumption"),
		AfterB.bSuccess && AfterB.Instance.ToStringSnapshot() == BaselineB.Instance.ToStringSnapshot());

	// Same in the other order: consume combat randomness first, then take both
	// baselines - they must equal the earlier ones taken before any draw.
	int32 CombatSink2 = 0;
	FRandomStream BattleStream2(0xF00D);
	for (int32 Draw = 0; Draw < 500; ++Draw)
	{
		CombatSink2 += BattleStream2.RandRange(0, 100);
	}
	CombatSink2 += FMath::Rand();
	CombatSink2 += FMath::Rand();
	const FDropRewardResult FreshA = FDropGenerator::GenerateReward(SeedA, SettlementId, Table, Catalog);
	TestTrue(TEXT("a fresh SeedA generation still matches the original baseline"),
		FreshA.bSuccess && FreshA.Instance.ToStringSnapshot() == BaselineA.Instance.ToStringSnapshot());

	// The sink only exists to consume the combat draws; its value is irrelevant.
	TestTrue(TEXT("the combat random draws happened and returned values"), CombatSink != 2147483647 || CombatSink2 != 2147483647);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_007DropsJsonMatchesItemsCatalogAndDrivesGeneration,
	"UEMMO.Tasks.M3_007.DropsJsonMatchesItemsCatalogAndDrivesGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_007DropsJsonMatchesItemsCatalogAndDrivesGeneration::RunTest(const FString& Parameters)
{
	// Data/drops.json pins the starter table (schema_version=1,
	// table_id="starter", weights 50/30/20); every DefinitionId it references
	// must exist in Data/items.json, and the loaded table must drive a real
	// generation against the in-memory training catalog.
	FString LoadError;
	TSharedPtr<FDropJsonValue> DropsRoot;
	if (!LoadJsonRoot(GetDataJsonPath(TEXT("drops.json")), LoadError, DropsRoot))
	{
		AddError(LoadError);
		return true;
	}

	const TSharedPtr<FDropJsonValue>* SchemaVersion = FindTyped(DropsRoot, TEXT("schema_version"), FDropJsonValue::EKind::Number);
	if (SchemaVersion)
	{
		TestEqual(TEXT("drops.json schema_version is 1"), (*SchemaVersion)->Number, 1.0);
	}
	else
	{
		AddError(TEXT("drops.json: missing or mistyped top-level number 'schema_version'"));
	}

	const TSharedPtr<FDropJsonValue>* TableId = FindTyped(DropsRoot, TEXT("table_id"), FDropJsonValue::EKind::String);
	if (TableId)
	{
		TestEqual(TEXT("drops.json table_id is 'starter'"), (*TableId)->String, FString(TEXT("starter")));
	}
	else
	{
		AddError(TEXT("drops.json: missing or mistyped top-level string 'table_id'"));
	}

	const TSharedPtr<FDropJsonValue>* Entries = FindTyped(DropsRoot, TEXT("entries"), FDropJsonValue::EKind::Array);
	if (!Entries)
	{
		AddError(TEXT("drops.json: missing required top-level array 'entries'"));
		return true;
	}
	TestEqual(TEXT("drops.json entries holds exactly 3 entries"), (*Entries)->Array.Num(), 3);
	if ((*Entries)->Array.Num() != 3)
	{
		return true;
	}

	// Pinned design values: order and weights are part of the contract.
	const TCHAR* ExpectedIds[3] = { TEXT("weapon_training"), TEXT("armor_training"), TEXT("charm_training") };
	const double ExpectedWeights[3] = { 50.0, 30.0, 20.0 };

	FDropTable Loaded;
	Loaded.TableId = TEXT("starter");
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const TSharedPtr<FDropJsonValue>& Entry = (*Entries)->Array[Index];
		if (!Entry.IsValid() || Entry->Kind != FDropJsonValue::EKind::Object)
		{
			AddError(FString::Printf(TEXT("drops.json entry %d is not a JSON object"), Index));
			continue;
		}
		const TSharedPtr<FDropJsonValue>* Id = FindTyped(Entry, TEXT("definition_id"), FDropJsonValue::EKind::String);
		const TSharedPtr<FDropJsonValue>* Weight = FindTyped(Entry, TEXT("weight"), FDropJsonValue::EKind::Number);
		if (!Id)
		{
			AddError(FString::Printf(TEXT("drops.json entry %d: missing or mistyped string field 'definition_id'"), Index));
			continue;
		}
		if (!Weight)
		{
			AddError(FString::Printf(TEXT("drops.json entry %d: missing or mistyped number field 'weight'"), Index));
			continue;
		}
		TestEqual(FString::Printf(TEXT("drops.json entry %d definition_id"), Index), (*Id)->String, FString(ExpectedIds[Index]));
		TestEqual(FString::Printf(TEXT("drops.json entry %d weight"), Index), (*Weight)->Number, ExpectedWeights[Index]);
		Loaded.Entries.Add(FDropEntry{ FName(*(*Id)->String), static_cast<int32>((*Weight)->Number) });
	}

	FString Errors;
	TestTrue(TEXT("the table loaded from drops.json passes ValidateDropTable"), ValidateDropTable(Loaded, Errors));
	TestTrue(TEXT("the loaded table reports no validation errors"), Errors.IsEmpty());
	TestTrue(TEXT("MakeStarterDropTable matches the loaded drops.json table (inline mirror stays in sync)"),
		DropTablesEqual(MakeStarterDropTable(), Loaded));

	// items.json consistency: the three definitions exist there by id.
	TSharedPtr<FDropJsonValue> ItemsRoot;
	if (!LoadJsonRoot(GetDataJsonPath(TEXT("items.json")), LoadError, ItemsRoot))
	{
		AddError(LoadError);
		return true;
	}
	const TSharedPtr<FDropJsonValue>* Items = FindTyped(ItemsRoot, TEXT("items"), FDropJsonValue::EKind::Array);
	if (!Items)
	{
		AddError(TEXT("items.json: missing required top-level array 'items'"));
		return true;
	}
	TSet<FString> ItemIds;
	for (const TSharedPtr<FDropJsonValue>& Entry : (*Items)->Array)
	{
		if (!Entry.IsValid() || Entry->Kind != FDropJsonValue::EKind::Object)
		{
			AddError(TEXT("items.json: an items entry is not a JSON object"));
			continue;
		}
		const TSharedPtr<FDropJsonValue>* Id = FindTyped(Entry, TEXT("definition_id"), FDropJsonValue::EKind::String);
		if (!Id)
		{
			AddError(TEXT("items.json: an items entry is missing the string field 'definition_id'"));
			continue;
		}
		ItemIds.Add((*Id)->String);
	}
	TestEqual(TEXT("items.json registers exactly 3 definition ids"), ItemIds.Num(), 3);
	for (int32 Index = 0; Index < 3; ++Index)
	{
		TestTrue(FString::Printf(TEXT("items.json contains the drops entry '%s'"), ExpectedIds[Index]),
			ItemIds.Contains(FString(ExpectedIds[Index])));
	}

	// End to end: the loaded table drives a real generation.
	const FItemDefinitionCatalog Catalog = MakeTrainingCatalog();
	const FDropRewardResult Reward = FDropGenerator::GenerateReward(1234, 77, Loaded, Catalog);
	TestTrue(TEXT("generation with the loaded drops.json table succeeds"), Reward.bSuccess);
	if (Reward.bSuccess)
	{
		TestTrue(TEXT("the loaded-table reward has a valid InstanceId"), Reward.Instance.InstanceId.IsValid());
		const FName Picked = Reward.Instance.DefinitionId;
		TestTrue(TEXT("the loaded-table reward picks one of the three starter definitions"),
			Picked == FName(TEXT("weapon_training")) || Picked == FName(TEXT("armor_training")) ||
			Picked == FName(TEXT("charm_training")));
	}
	return true;
}

#endif
