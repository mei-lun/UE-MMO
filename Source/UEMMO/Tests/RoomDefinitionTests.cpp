// M2-005: validates the textual room source data (Data/rooms.json) against the
// interface contract (section 7) and the M2-005 single-entry room validator,
// and pins the map soft reference. Pure checks: this file only reads JSON
// text, NewObject data assets and the map package named by the soft
// reference; it never spawns actors, executes waves or touches wall clocks.
//
// Parsing note (same lesson as M2-001): the engine's Json module headers
// compile through Engine's public dependency, but UnrealBuildTool does not put
// the Json import library on the UEMMO link line, so FJsonValue/FJsonObject
// symbols fail to link (LNK2019). UEMMO.Build.cs is outside this task's file
// range, so the tests use the small hand-rolled parser below (objects, arrays,
// strings with escapes, numbers, booleans, null) built on Core types only.
//
// Unit convention (acceptance item): every position and size travels through
// unit-bearing field names. JSON: *_cm vectors, *_degrees yaw. C++: FVector
// fields are UE world units (centimeters) and the yaw field is named
// PlayerSpawnYawDegrees; the tests below pin those JSON names literally.

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "UObject/UObjectGlobals.h"

#include <limits>

#include "../Room/RoomDefinition.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M2_005
{
	/** Minimal JSON value: just enough shape for the room/enemy data files. */
	class FRoomJsonValue
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
		TArray<TSharedPtr<FRoomJsonValue>> Array;
		TMap<FString, TSharedPtr<FRoomJsonValue>> Object;
	};

	/** Recursive descent JSON parser (RFC 8259 subset; \u escapes decode to one TCHAR). */
	class FRoomJsonParser
	{
	public:
		static bool Parse(const FString& Text, TSharedPtr<FRoomJsonValue>& OutRoot, FString& OutError)
		{
			FRoomJsonParser Parser(Text);
			Parser.SkipWhitespace();
			TSharedPtr<FRoomJsonValue> Root = Parser.ParseValue(OutError);
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
		explicit FRoomJsonParser(const FString& InText)
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

		TSharedPtr<FRoomJsonValue> ParseValue(FString& OutError)
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

		TSharedPtr<FRoomJsonValue> ParseObject(FString& OutError)
		{
			if (!Expect(TEXT('{'), TEXT("to start an object"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FRoomJsonValue> Value = MakeShared<FRoomJsonValue>();
			Value->Kind = FRoomJsonValue::EKind::Object;
			SkipWhitespace();
			if (Consume(TEXT('}')))
			{
				return Value;
			}
			while (true)
			{
				SkipWhitespace();
				TSharedPtr<FRoomJsonValue> Key = ParseString(OutError);
				if (!Key.IsValid())
				{
					return nullptr;
				}
				SkipWhitespace();
				if (!Expect(TEXT(':'), TEXT("between object key and value"), OutError))
				{
					return nullptr;
				}
				TSharedPtr<FRoomJsonValue> Element = ParseValue(OutError);
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

		TSharedPtr<FRoomJsonValue> ParseArray(FString& OutError)
		{
			if (!Expect(TEXT('['), TEXT("to start an array"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FRoomJsonValue> Value = MakeShared<FRoomJsonValue>();
			Value->Kind = FRoomJsonValue::EKind::Array;
			SkipWhitespace();
			if (Consume(TEXT(']')))
			{
				return Value;
			}
			while (true)
			{
				TSharedPtr<FRoomJsonValue> Element = ParseValue(OutError);
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

		TSharedPtr<FRoomJsonValue> ParseString(FString& OutError)
		{
			if (!Expect(TEXT('"'), TEXT("to start a string"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FRoomJsonValue> Value = MakeShared<FRoomJsonValue>();
			Value->Kind = FRoomJsonValue::EKind::String;
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

		TSharedPtr<FRoomJsonValue> ParseBoolean(FString& OutError)
		{
			const TSharedPtr<FRoomJsonValue> Value = MakeShared<FRoomJsonValue>();
			if (MatchLiteral(TEXT("true"), 4))
			{
				Value->Kind = FRoomJsonValue::EKind::Boolean;
				Value->Boolean = true;
				Position += 4;
				return Value;
			}
			if (MatchLiteral(TEXT("false"), 5))
			{
				Value->Kind = FRoomJsonValue::EKind::Boolean;
				Value->Boolean = false;
				Position += 5;
				return Value;
			}
			OutError = FString::Printf(TEXT("invalid literal at offset %d"), Position);
			return nullptr;
		}

		TSharedPtr<FRoomJsonValue> ParseNull(FString& OutError)
		{
			if (MatchLiteral(TEXT("null"), 4))
			{
				const TSharedPtr<FRoomJsonValue> Value = MakeShared<FRoomJsonValue>();
				Value->Kind = FRoomJsonValue::EKind::Null;
				Position += 4;
				return Value;
			}
			OutError = FString::Printf(TEXT("invalid literal at offset %d"), Position);
			return nullptr;
		}

		TSharedPtr<FRoomJsonValue> ParseNumber(FString& OutError)
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
			const TSharedPtr<FRoomJsonValue> Value = MakeShared<FRoomJsonValue>();
			Value->Kind = FRoomJsonValue::EKind::Number;
			Value->Number = FCString::Atod(*Source.Mid(Start, Position - Start));
			return Value;
		}
	};

	static const TCHAR* RoomRequiredStringFields[] = { TEXT("room_id"), TEXT("map_path"), TEXT("reward_table_id") };
	static const TCHAR* RoomRequiredVectorFieldsCm[] = { TEXT("player_spawn_location_cm"), TEXT("bounds_center_cm"), TEXT("bounds_size_cm") };

	static FString GetRoomsJsonPath()
	{
		return FPaths::ProjectDir() / TEXT("Data") / TEXT("rooms.json");
	}

	static FString GetEnemiesJsonPath()
	{
		return FPaths::ProjectDir() / TEXT("Data") / TEXT("enemies.json");
	}

	static bool LoadJsonRoot(const FString& Path, FString& OutError, TSharedPtr<FRoomJsonValue>& OutRoot)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			OutError = FString::Printf(TEXT("could not read the JSON data file at %s"), *Path);
			return false;
		}
		TSharedPtr<FRoomJsonValue> Root;
		if (!FRoomJsonParser::Parse(Text, Root, OutError))
		{
			OutError = FString::Printf(TEXT("could not parse JSON in %s: %s"), *Path, *OutError);
			return false;
		}
		OutRoot = Root;
		return true;
	}

	/** Returns the field only when present with exactly the requested JSON kind. */
	static const TSharedPtr<FRoomJsonValue>* FindTyped(const TSharedPtr<FRoomJsonValue>& Object, const TCHAR* Name,
		FRoomJsonValue::EKind Kind)
	{
		const TSharedPtr<FRoomJsonValue>* Value = Object->Object.Find(Name);
		if (!Value || !Value->IsValid() || (*Value)->Kind != Kind)
		{
			return nullptr;
		}
		return Value;
	}

	static FString RoomEntryName(const TSharedPtr<FRoomJsonValue>& Entry)
	{
		const TSharedPtr<FRoomJsonValue>* Id = FindTyped(Entry, TEXT("room_id"), FRoomJsonValue::EKind::String);
		return (Id && !(*Id)->String.IsEmpty()) ? (*Id)->String : FString(TEXT("<unnamed room>"));
	}

	static bool GetStringOrReport(FAutomationTestBase& Test, const TSharedPtr<FRoomJsonValue>& Entry,
		const TCHAR* Name, const FString& Context, FString& OutValue)
	{
		const TSharedPtr<FRoomJsonValue>* Value = FindTyped(Entry, Name, FRoomJsonValue::EKind::String);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("%s: missing or mistyped string field '%s'"), *Context, Name));
			return false;
		}
		OutValue = (*Value)->String;
		return true;
	}

	static bool GetNumberOrReport(FAutomationTestBase& Test, const TSharedPtr<FRoomJsonValue>& Entry,
		const TCHAR* Name, const FString& Context, double& OutValue)
	{
		const TSharedPtr<FRoomJsonValue>* Value = FindTyped(Entry, Name, FRoomJsonValue::EKind::Number);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("%s: missing or mistyped number field '%s'"), *Context, Name));
			return false;
		}
		OutValue = (*Value)->Number;
		return true;
	}

	/** Reads the {x, y, z} number members (centimeters) of the given object into an FVector. */
	static bool GetVectorMembersOrReport(FAutomationTestBase& Test, const TSharedPtr<FRoomJsonValue>& Object,
		const FString& Context, FVector& OutValue)
	{
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		const bool bComplete =
			GetNumberOrReport(Test, Object, TEXT("x"), Context, X) &&
			GetNumberOrReport(Test, Object, TEXT("y"), Context, Y) &&
			GetNumberOrReport(Test, Object, TEXT("z"), Context, Z);
		if (!bComplete)
		{
			return false;
		}
		OutValue = FVector(X, Y, Z);
		return true;
	}

	/** Reads an {x, y, z} number object (centimeters) named on the entry into an FVector. */
	static bool GetVectorOrReport(FAutomationTestBase& Test, const TSharedPtr<FRoomJsonValue>& Entry,
		const TCHAR* Name, const FString& Context, FVector& OutValue)
	{
		const TSharedPtr<FRoomJsonValue>* Value = FindTyped(Entry, Name, FRoomJsonValue::EKind::Object);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("%s: missing or mistyped cm vector object '%s'"), *Context, Name));
			return false;
		}
		return GetVectorMembersOrReport(Test, *Value, Context, OutValue);
	}

	/** Comparison tolerance for doubles that both sides derive from decimal literals. */
	static bool NearlyEqual(double A, double B)
	{
		return FMath::Abs(A - B) <= 1e-9;
	}

	/** One parsed wave entry of a room. */
	struct FRoomWaveJson
	{
		FString EnemyId;
		double Count = 0.0;
		TArray<FVector> SpawnLocationsCm;
	};

	static bool GetWaveOrReport(FAutomationTestBase& Test, const TSharedPtr<FRoomJsonValue>& WaveObject,
		const FString& Context, FRoomWaveJson& OutWave)
	{
		bool bOk = true;
		if (!GetStringOrReport(Test, WaveObject, TEXT("enemy_id"), Context, OutWave.EnemyId))
		{
			bOk = false;
		}
		if (!GetNumberOrReport(Test, WaveObject, TEXT("count"), Context, OutWave.Count))
		{
			bOk = false;
		}
		else if (!NearlyEqual(OutWave.Count, FMath::RoundToZero(OutWave.Count)))
		{
			Test.AddError(FString::Printf(TEXT("%s: 'count' must be a whole number (got %f)"), *Context, OutWave.Count));
			bOk = false;
		}

		const TSharedPtr<FRoomJsonValue>* Locations = FindTyped(WaveObject, TEXT("spawn_locations_cm"), FRoomJsonValue::EKind::Array);
		if (!Locations)
		{
			Test.AddError(FString::Printf(TEXT("%s: missing or mistyped cm location array 'spawn_locations_cm'"), *Context));
			return false;
		}
		int32 LocationIndex = 0;
		for (const TSharedPtr<FRoomJsonValue>& Location : (*Locations)->Array)
		{
			const FString LocationContext = FString::Printf(TEXT("%s spawn_locations_cm[%d]"), *Context, LocationIndex);
			++LocationIndex;
			if (!Location.IsValid() || Location->Kind != FRoomJsonValue::EKind::Object)
			{
				Test.AddError(FString::Printf(TEXT("%s is not a JSON object"), *LocationContext));
				bOk = false;
				continue;
			}
			FVector LocationCm = FVector::ZeroVector;
			if (!GetVectorMembersOrReport(Test, Location, LocationContext, LocationCm))
			{
				bOk = false;
				continue;
			}
			OutWave.SpawnLocationsCm.Add(LocationCm);
		}
		return bOk;
	}

	/** Fills a URoomDefinition from one JSON entry; returns null and reports when fields are missing. */
	static URoomDefinition* MakeRoomFromJson(FAutomationTestBase& Test, const TSharedPtr<FRoomJsonValue>& Entry)
	{
		const FString Context = FString::Printf(TEXT("room '%s'"), *RoomEntryName(Entry));

		FString RoomId;
		FString MapPath;
		FString RewardTableId;
		FVector PlayerSpawnLocationCm = FVector::ZeroVector;
		double PlayerSpawnYawDegrees = 0.0;
		FVector BoundsCenterCm = FVector::ZeroVector;
		FVector BoundsSizeCm = FVector::ZeroVector;

		bool bOk = GetStringOrReport(Test, Entry, TEXT("room_id"), Context, RoomId);
		bOk = GetStringOrReport(Test, Entry, TEXT("map_path"), Context, MapPath) && bOk;
		bOk = GetStringOrReport(Test, Entry, TEXT("reward_table_id"), Context, RewardTableId) && bOk;
		bOk = GetVectorOrReport(Test, Entry, TEXT("player_spawn_location_cm"), Context, PlayerSpawnLocationCm) && bOk;
		bOk = GetNumberOrReport(Test, Entry, TEXT("player_spawn_yaw_degrees"), Context, PlayerSpawnYawDegrees) && bOk;
		bOk = GetVectorOrReport(Test, Entry, TEXT("bounds_center_cm"), Context, BoundsCenterCm) && bOk;
		bOk = GetVectorOrReport(Test, Entry, TEXT("bounds_size_cm"), Context, BoundsSizeCm) && bOk;
		if (!bOk)
		{
			return nullptr;
		}

		const TSharedPtr<FRoomJsonValue>* Waves = FindTyped(Entry, TEXT("waves"), FRoomJsonValue::EKind::Array);
		if (!Waves)
		{
			Test.AddError(Context + TEXT(": missing or mistyped wave array 'waves'"));
			return nullptr;
		}

		URoomDefinition* Room = NewObject<URoomDefinition>();
		Room->RoomId = FName(*RoomId);
		Room->MapPath = TSoftObjectPtr<UWorld>(FSoftObjectPath(MapPath));
		Room->PlayerSpawnLocation = PlayerSpawnLocationCm;                 // cm
		Room->PlayerSpawnYawDegrees = static_cast<float>(PlayerSpawnYawDegrees);
		Room->BoundsCenter = BoundsCenterCm;                               // cm
		Room->BoundsSize = BoundsSizeCm;                                   // cm
		Room->RewardTableId = FName(*RewardTableId);

		int32 WaveIndex = 0;
		for (const TSharedPtr<FRoomJsonValue>& WaveObject : (*Waves)->Array)
		{
			const FString WaveContext = FString::Printf(TEXT("%s wave %d"), *Context, WaveIndex);
			++WaveIndex;
			if (!WaveObject.IsValid() || WaveObject->Kind != FRoomJsonValue::EKind::Object)
			{
				Test.AddError(WaveContext + TEXT(" is not a JSON object"));
				return nullptr;
			}
			FRoomWaveJson WaveJson;
			if (!GetWaveOrReport(Test, WaveObject, WaveContext, WaveJson))
			{
				return nullptr;
			}
			FRoomWaveDefinition Wave;
			Wave.EnemyId = FName(*WaveJson.EnemyId);
			Wave.Count = static_cast<int32>(WaveJson.Count);
			Wave.SpawnLocations = WaveJson.SpawnLocationsCm;               // cm
			Room->Waves.Add(Wave);
		}
		return Room;
	}

	/** Collects every enemy_id of Data/enemies.json into the known-id set. */
	static bool BuildKnownEnemyIds(FAutomationTestBase& Test, TSet<FName>& OutKnownEnemyIds)
	{
		FString LoadError;
		TSharedPtr<FRoomJsonValue> Root;
		if (!LoadJsonRoot(GetEnemiesJsonPath(), LoadError, Root))
		{
			Test.AddError(LoadError);
			return false;
		}
		const TSharedPtr<FRoomJsonValue>* Enemies = FindTyped(Root, TEXT("enemies"), FRoomJsonValue::EKind::Array);
		if (!Enemies)
		{
			Test.AddError(TEXT("enemies.json is missing the required top-level array 'enemies'"));
			return false;
		}
		for (const TSharedPtr<FRoomJsonValue>& Enemy : (*Enemies)->Array)
		{
			if (Enemy.IsValid() && Enemy->Kind == FRoomJsonValue::EKind::Object)
			{
				const TSharedPtr<FRoomJsonValue>* Id = FindTyped(Enemy, TEXT("enemy_id"), FRoomJsonValue::EKind::String);
				if (Id && !(*Id)->String.IsEmpty())
				{
					OutKnownEnemyIds.Add(FName(*(*Id)->String));
				}
			}
		}
		if (OutKnownEnemyIds.Num() == 0)
		{
			Test.AddError(TEXT("enemies.json provided no usable enemy_id entries"));
			return false;
		}
		return true;
	}

	/** Builds the room_training_01 definition in memory (independent of the JSON file). */
	static URoomDefinition* MakeValidTrainingRoom()
	{
		URoomDefinition* Room = NewObject<URoomDefinition>();
		Room->RoomId = TEXT("room_training_01");
		Room->MapPath = TSoftObjectPtr<UWorld>(FSoftObjectPath(TEXT("/Game/UEMMO/Maps/L_TrainingArena")));
		Room->PlayerSpawnLocation = FVector(-400.0, 0.0, 100.0);           // cm, the map's PlayerStart
		Room->PlayerSpawnYawDegrees = 0.0f;                                 // degrees, un-rotated PlayerStart
		Room->BoundsCenter = FVector(0.0, 0.0, 90.0);                       // cm
		Room->BoundsSize = FVector(2400.0, 1000.0, 180.0);                  // cm
		Room->RewardTableId = TEXT("starter");

		FRoomWaveDefinition Wave1;
		Wave1.EnemyId = TEXT("melee_grunt");
		Wave1.Count = 2;
		Wave1.SpawnLocations.Add(FVector(300.0, 0.0, 88.0));                // cm
		Wave1.SpawnLocations.Add(FVector(450.0, -150.0, 88.0));             // cm
		Room->Waves.Add(Wave1);

		FRoomWaveDefinition Wave2;
		Wave2.EnemyId = TEXT("melee_grunt");
		Wave2.Count = 3;
		Wave2.SpawnLocations.Add(FVector(600.0, 100.0, 88.0));              // cm
		Wave2.SpawnLocations.Add(FVector(750.0, -100.0, 88.0));             // cm
		Wave2.SpawnLocations.Add(FVector(0.0, 300.0, 88.0));                // cm
		Room->Waves.Add(Wave2);
		return Room;
	}

	/** Compares a cm vector against expected components with a tight literal tolerance. */
	static void TestVectorNear(FAutomationTestBase& Test, const TCHAR* What, const FVector& Actual,
		double ExpectedX, double ExpectedY, double ExpectedZ)
	{
		Test.TestTrue(FString::Printf(TEXT("%s.x is %.1f cm"), What, ExpectedX), NearlyEqual(Actual.X, ExpectedX));
		Test.TestTrue(FString::Printf(TEXT("%s.y is %.1f cm"), What, ExpectedY), NearlyEqual(Actual.Y, ExpectedY));
		Test.TestTrue(FString::Printf(TEXT("%s.z is %.1f cm"), What, ExpectedZ), NearlyEqual(Actual.Z, ExpectedZ));
	}
}

using namespace UE::UEMMO::Tasks::M2_005;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_005RoomTraining01LoadsAndPassesValidation,
	"UEMMO.Tasks.M2_005.RoomTraining01LoadsAndPassesValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_005RoomTraining01LoadsAndPassesValidation::RunTest(const FString& Parameters)
{
	FString LoadError;
	TSharedPtr<FRoomJsonValue> Root;
	if (!LoadJsonRoot(GetRoomsJsonPath(), LoadError, Root))
	{
		AddError(LoadError);
		return true;
	}

	const TSharedPtr<FRoomJsonValue>* SchemaVersion = FindTyped(Root, TEXT("schema_version"), FRoomJsonValue::EKind::Number);
	if (SchemaVersion)
	{
		TestTrue(TEXT("schema_version is 1"), NearlyEqual((*SchemaVersion)->Number, 1.0));
	}
	else
	{
		AddError(TEXT("missing or mistyped top-level number 'schema_version'"));
	}

	const TSharedPtr<FRoomJsonValue>* Rooms = FindTyped(Root, TEXT("rooms"), FRoomJsonValue::EKind::Array);
	if (!Rooms)
	{
		AddError(TEXT("missing required top-level array 'rooms'"));
		return true;
	}
	TestEqual(TEXT("rooms holds exactly 1 entry"), (*Rooms)->Array.Num(), 1);
	if ((*Rooms)->Array.Num() != 1)
	{
		return true;
	}

	const TSharedPtr<FRoomJsonValue> Entry = (*Rooms)->Array[0];
	if (!Entry.IsValid() || Entry->Kind != FRoomJsonValue::EKind::Object)
	{
		AddError(TEXT("rooms[0] is not a JSON object"));
		return true;
	}
	TestEqual(TEXT("room_id is room_training_01"), RoomEntryName(Entry), FString(TEXT("room_training_01")));

	// Unit clarity is pinned on the source field names (acceptance item).
	for (const TCHAR* Field : RoomRequiredStringFields)
	{
		TestTrue(FString::Printf(TEXT("room_training_01 has string field '%s'"), Field),
			FindTyped(Entry, Field, FRoomJsonValue::EKind::String) != nullptr);
	}
	for (const TCHAR* Field : RoomRequiredVectorFieldsCm)
	{
		TestTrue(FString::Printf(TEXT("room_training_01 has cm vector field '%s' (unit in the field name)"), Field),
			FindTyped(Entry, Field, FRoomJsonValue::EKind::Object) != nullptr);
	}
	TestTrue(TEXT("room_training_01 has degree field 'player_spawn_yaw_degrees' (unit in the field name)"),
		FindTyped(Entry, TEXT("player_spawn_yaw_degrees"), FRoomJsonValue::EKind::Number) != nullptr);

	// Contract values of the single room_training_01 entry: the player spawn
	// mirrors the map's PlayerStart at (-400, 0, 100) cm, yaw 0 faces +X, and
	// the bounds describe the 24m x 10m floor up to the wall top (z 0..180).
	FString MapPath;
	FString RewardTableId;
	FVector PlayerSpawnCm = FVector::ZeroVector;
	double YawDegrees = -1.0;
	FVector BoundsCenterCm = FVector::ZeroVector;
	FVector BoundsSizeCm = FVector::ZeroVector;
	const bool bValuesRead =
		GetStringOrReport(*this, Entry, TEXT("map_path"), TEXT("room_training_01"), MapPath) &&
		GetStringOrReport(*this, Entry, TEXT("reward_table_id"), TEXT("room_training_01"), RewardTableId) &&
		GetVectorOrReport(*this, Entry, TEXT("player_spawn_location_cm"), TEXT("room_training_01"), PlayerSpawnCm) &&
		GetNumberOrReport(*this, Entry, TEXT("player_spawn_yaw_degrees"), TEXT("room_training_01"), YawDegrees) &&
		GetVectorOrReport(*this, Entry, TEXT("bounds_center_cm"), TEXT("room_training_01"), BoundsCenterCm) &&
		GetVectorOrReport(*this, Entry, TEXT("bounds_size_cm"), TEXT("room_training_01"), BoundsSizeCm);
	if (bValuesRead)
	{
		TestEqual(TEXT("map_path is the L_TrainingArena package"), MapPath, FString(TEXT("/Game/UEMMO/Maps/L_TrainingArena")));
		TestEqual(TEXT("reward_table_id is starter"), RewardTableId, FString(TEXT("starter")));
		TestVectorNear(*this, TEXT("player_spawn_location_cm"), PlayerSpawnCm, -400.0, 0.0, 100.0);
		TestTrue(TEXT("player_spawn_yaw_degrees is 0"), NearlyEqual(YawDegrees, 0.0));
		TestVectorNear(*this, TEXT("bounds_center_cm"), BoundsCenterCm, 0.0, 0.0, 90.0);
		TestVectorNear(*this, TEXT("bounds_size_cm"), BoundsSizeCm, 2400.0, 1000.0, 180.0);
	}

	// The loaded entry maps into the data asset and passes single-entry
	// validation against the enemy catalog of Data/enemies.json.
	URoomDefinition* Room = MakeRoomFromJson(*this, Entry);
	if (!Room)
	{
		return true;
	}
	TestEqual(TEXT("loaded RoomId is room_training_01"), Room->RoomId, FName(TEXT("room_training_01")));
	TestEqual(TEXT("loaded RewardTableId is starter"), Room->RewardTableId, FName(TEXT("starter")));

	TSet<FName> KnownEnemyIds;
	if (!BuildKnownEnemyIds(*this, KnownEnemyIds))
	{
		return true;
	}
	FText Errors;
	TestTrue(TEXT("the loaded room_training_01 passes ValidateRoomDefinition"),
		ValidateRoomDefinition(*Room, KnownEnemyIds, Errors));
	TestTrue(TEXT("the loaded room_training_01 reports no validation errors"), Errors.IsEmpty());

	// Wave shape: exactly 2 waves of melee_grunt with counts [2, 3].
	const TSharedPtr<FRoomJsonValue>* Waves = FindTyped(Entry, TEXT("waves"), FRoomJsonValue::EKind::Array);
	if (!Waves)
	{
		AddError(TEXT("missing or mistyped wave array 'waves'"));
		return true;
	}
	TestEqual(TEXT("waves holds exactly 2 entries"), (*Waves)->Array.Num(), 2);
	TestEqual(TEXT("room_training_01 holds exactly 2 waves"), Room->Waves.Num(), 2);
	if (Room->Waves.Num() == 2)
	{
		TestEqual(TEXT("wave 1 count is 2"), Room->Waves[0].Count, 2);
		TestEqual(TEXT("wave 2 count is 3"), Room->Waves[1].Count, 3);
		TestEqual(TEXT("wave 1 enemy is melee_grunt"), Room->Waves[0].EnemyId, FName(TEXT("melee_grunt")));
		TestEqual(TEXT("wave 2 enemy is melee_grunt"), Room->Waves[1].EnemyId, FName(TEXT("melee_grunt")));
		TestEqual(TEXT("wave 1 lists 2 spawn locations"), Room->Waves[0].SpawnLocations.Num(), 2);
		TestEqual(TEXT("wave 2 lists 3 spawn locations"), Room->Waves[1].SpawnLocations.Num(), 3);
		TestTrue(TEXT("wave 1 carries the cm unit field 'spawn_locations_cm' in the source data"),
			FindTyped((*Waves)->Array[0], TEXT("spawn_locations_cm"), FRoomJsonValue::EKind::Array) != nullptr);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_005RoomTraining01WavePlanIsFixedAndReproducible,
	"UEMMO.Tasks.M2_005.RoomTraining01WavePlanIsFixedAndReproducible",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_005RoomTraining01WavePlanIsFixedAndReproducible::RunTest(const FString& Parameters)
{
	// Two independent parses of the same source file must agree exactly: the
	// wave plan [2, 3] is fixed source data, never generated at runtime.
	FString FirstError;
	FString SecondError;
	TSharedPtr<FRoomJsonValue> FirstRoot;
	TSharedPtr<FRoomJsonValue> SecondRoot;
	if (!LoadJsonRoot(GetRoomsJsonPath(), FirstError, FirstRoot))
	{
		AddError(FirstError);
		return true;
	}
	if (!LoadJsonRoot(GetRoomsJsonPath(), SecondError, SecondRoot))
	{
		AddError(SecondError);
		return true;
	}

	const TSharedPtr<FRoomJsonValue>* FirstRooms = FindTyped(FirstRoot, TEXT("rooms"), FRoomJsonValue::EKind::Array);
	const TSharedPtr<FRoomJsonValue>* SecondRooms = FindTyped(SecondRoot, TEXT("rooms"), FRoomJsonValue::EKind::Array);
	if (!FirstRooms || !SecondRooms || (*FirstRooms)->Array.Num() != 1 || (*SecondRooms)->Array.Num() != 1)
	{
		AddError(TEXT("both parses must expose exactly one room entry"));
		return true;
	}

	URoomDefinition* First = MakeRoomFromJson(*this, (*FirstRooms)->Array[0]);
	URoomDefinition* Second = MakeRoomFromJson(*this, (*SecondRooms)->Array[0]);
	if (!First || !Second)
	{
		return true;
	}

	TestEqual(TEXT("both parses see 2 waves"), First->Waves.Num(), 2);
	TestTrue(TEXT("the two parses produce identical wave plans"), First->Waves.Num() == Second->Waves.Num());
	if (First->Waves.Num() == 2 && Second->Waves.Num() == 2)
	{
		// The fixed [2, 3] schedule.
		TestEqual(TEXT("parse 1 wave 1 count is 2"), First->Waves[0].Count, 2);
		TestEqual(TEXT("parse 1 wave 2 count is 3"), First->Waves[1].Count, 3);
		TestEqual(TEXT("parse 2 wave 1 count is 2"), Second->Waves[0].Count, 2);
		TestEqual(TEXT("parse 2 wave 2 count is 3"), Second->Waves[1].Count, 3);

		for (int32 WaveIndex = 0; WaveIndex < 2; ++WaveIndex)
		{
			const FRoomWaveDefinition& FirstWave = First->Waves[WaveIndex];
			const FRoomWaveDefinition& SecondWave = Second->Waves[WaveIndex];
			TestEqual(FString::Printf(TEXT("parse 2 wave %d enemy matches parse 1"), WaveIndex + 1),
				SecondWave.EnemyId, FirstWave.EnemyId);
			TestTrue(FString::Printf(TEXT("parse 2 wave %d lists the same spawn locations as parse 1"), WaveIndex + 1),
				SecondWave.SpawnLocations.Num() == FirstWave.SpawnLocations.Num());
			for (int32 LocationIndex = 0; LocationIndex < FMath::Min(FirstWave.SpawnLocations.Num(), SecondWave.SpawnLocations.Num()); ++LocationIndex)
			{
				TestTrue(FString::Printf(TEXT("parse 2 wave %d location %d equals parse 1 (cm values identical)"),
					WaveIndex + 1, LocationIndex + 1),
					FirstWave.SpawnLocations[LocationIndex].Equals(SecondWave.SpawnLocations[LocationIndex]));
			}
		}
	}

	// Both parsed copies validate against the same enemy catalog.
	TSet<FName> KnownEnemyIds;
	if (!BuildKnownEnemyIds(*this, KnownEnemyIds))
	{
		return true;
	}
	FText FirstErrors;
	FText SecondErrors;
	TestTrue(TEXT("parse 1 passes ValidateRoomDefinition"), ValidateRoomDefinition(*First, KnownEnemyIds, FirstErrors));
	TestTrue(TEXT("parse 2 passes ValidateRoomDefinition"), ValidateRoomDefinition(*Second, KnownEnemyIds, SecondErrors));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_005MapPathSoftReferenceResolvesToExistingMap,
	"UEMMO.Tasks.M2_005.MapPathSoftReferenceResolvesToExistingMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_005MapPathSoftReferenceResolvesToExistingMap::RunTest(const FString& Parameters)
{
	FString LoadError;
	TSharedPtr<FRoomJsonValue> Root;
	if (!LoadJsonRoot(GetRoomsJsonPath(), LoadError, Root))
	{
		AddError(LoadError);
		return true;
	}
	const TSharedPtr<FRoomJsonValue>* Rooms = FindTyped(Root, TEXT("rooms"), FRoomJsonValue::EKind::Array);
	if (!Rooms || (*Rooms)->Array.Num() != 1)
	{
		AddError(TEXT("rooms must hold exactly 1 entry for this check"));
		return true;
	}
	URoomDefinition* Room = MakeRoomFromJson(*this, (*Rooms)->Array[0]);
	if (!Room)
	{
		return true;
	}

	// The soft reference carries the documented package path.
	const FString MapPathString = Room->MapPath.ToSoftObjectPath().ToString();
	TestFalse(TEXT("MapPath is not empty"), MapPathString.IsEmpty());
	TestEqual(TEXT("MapPath names the L_TrainingArena package"), MapPathString, FString(TEXT("/Game/UEMMO/Maps/L_TrainingArena")));

	// The package resolves to a real UWorld inside the -game process (same
	// load pattern as the M1 asset tests: LoadObject on a /Game/ path).
	FString ObjectPath = MapPathString;
	if (!ObjectPath.Contains(TEXT(".")))
	{
		ObjectPath += TEXT(".") + FPaths::GetBaseFilename(MapPathString);
	}
	UWorld* World = LoadObject<UWorld>(nullptr, *ObjectPath);
	TestTrue(TEXT("MapPath resolves to a loadable L_TrainingArena UWorld in the -game process"), World != nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_005RejectsInvalidDefinitionsWithFieldNames,
	"UEMMO.Tasks.M2_005.RejectsInvalidDefinitionsWithFieldNames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_005RejectsInvalidDefinitionsWithFieldNames::RunTest(const FString& Parameters)
{
	// Every rejected mutation must fail validation and name the offending field.
	TSet<FName> KnownEnemyIds;
	KnownEnemyIds.Add(FName(TEXT("melee_grunt")));

	URoomDefinition* Base = MakeValidTrainingRoom();
	FText BaseErrors;
	TestTrue(TEXT("the unmutated in-memory training room passes validation"),
		ValidateRoomDefinition(*Base, KnownEnemyIds, BaseErrors));
	TestTrue(TEXT("the unmutated in-memory training room reports no errors"), BaseErrors.IsEmpty());

	URoomDefinition* MissingId = MakeValidTrainingRoom();
	MissingId->RoomId = NAME_None;
	FText MissingIdErrors;
	TestFalse(TEXT("an empty RoomId is rejected"), ValidateRoomDefinition(*MissingId, KnownEnemyIds, MissingIdErrors));
	TestTrue(TEXT("empty RoomId error names RoomId"), MissingIdErrors.ToString().Contains(TEXT("RoomId")));

	URoomDefinition* MissingMap = MakeValidTrainingRoom();
	MissingMap->MapPath = TSoftObjectPtr<UWorld>();
	FText MissingMapErrors;
	TestFalse(TEXT("an empty MapPath is rejected"), ValidateRoomDefinition(*MissingMap, KnownEnemyIds, MissingMapErrors));
	TestTrue(TEXT("empty MapPath error names MapPath"), MissingMapErrors.ToString().Contains(TEXT("MapPath")));

	URoomDefinition* MissingReward = MakeValidTrainingRoom();
	MissingReward->RewardTableId = NAME_None;
	FText MissingRewardErrors;
	TestFalse(TEXT("an empty RewardTableId is rejected"), ValidateRoomDefinition(*MissingReward, KnownEnemyIds, MissingRewardErrors));
	TestTrue(TEXT("empty RewardTableId error names RewardTableId"), MissingRewardErrors.ToString().Contains(TEXT("RewardTableId")));

	URoomDefinition* NoWaves = MakeValidTrainingRoom();
	NoWaves->Waves.Reset();
	FText NoWavesErrors;
	TestFalse(TEXT("zero waves are rejected"), ValidateRoomDefinition(*NoWaves, KnownEnemyIds, NoWavesErrors));
	TestTrue(TEXT("zero waves error names Waves"), NoWavesErrors.ToString().Contains(TEXT("Waves")));

	URoomDefinition* ZeroCount = MakeValidTrainingRoom();
	ZeroCount->Waves[0].Count = 0;
	FText ZeroCountErrors;
	TestFalse(TEXT("Count=0 is rejected"), ValidateRoomDefinition(*ZeroCount, KnownEnemyIds, ZeroCountErrors));
	TestTrue(TEXT("Count=0 error names Count"), ZeroCountErrors.ToString().Contains(TEXT("Count")));

	URoomDefinition* NegativeCount = MakeValidTrainingRoom();
	NegativeCount->Waves[0].Count = -3;
	FText NegativeCountErrors;
	TestFalse(TEXT("Count=-3 is rejected"), ValidateRoomDefinition(*NegativeCount, KnownEnemyIds, NegativeCountErrors));
	TestTrue(TEXT("Count=-3 error names Count"), NegativeCountErrors.ToString().Contains(TEXT("Count")));

	URoomDefinition* MissingEnemy = MakeValidTrainingRoom();
	MissingEnemy->Waves[0].EnemyId = NAME_None;
	FText MissingEnemyErrors;
	TestFalse(TEXT("an empty wave EnemyId is rejected"), ValidateRoomDefinition(*MissingEnemy, KnownEnemyIds, MissingEnemyErrors));
	TestTrue(TEXT("empty EnemyId error names EnemyId"), MissingEnemyErrors.ToString().Contains(TEXT("EnemyId")));

	URoomDefinition* UnknownEnemy = MakeValidTrainingRoom();
	UnknownEnemy->Waves[0].EnemyId = TEXT("boss_missing");
	FText UnknownEnemyErrors;
	TestFalse(TEXT("an unknown wave EnemyId is rejected"), ValidateRoomDefinition(*UnknownEnemy, KnownEnemyIds, UnknownEnemyErrors));
	TestTrue(TEXT("unknown EnemyId error names EnemyId"), UnknownEnemyErrors.ToString().Contains(TEXT("EnemyId")));

	URoomDefinition* MismatchedLocations = MakeValidTrainingRoom();
	MismatchedLocations->Waves[0].SpawnLocations.RemoveAt(1);
	FText MismatchedLocationErrors;
	TestFalse(TEXT("SpawnLocations.Num() != Count is rejected"),
		ValidateRoomDefinition(*MismatchedLocations, KnownEnemyIds, MismatchedLocationErrors));
	TestTrue(TEXT("location/count mismatch error names SpawnLocations"),
		MismatchedLocationErrors.ToString().Contains(TEXT("SpawnLocations")));

	URoomDefinition* SpawnBeyondX = MakeValidTrainingRoom();
	SpawnBeyondX->Waves[0].SpawnLocations[0] = FVector(1300.0, 0.0, 88.0);   // cm, beyond the 1200 cm wall
	FText SpawnBeyondXErrors;
	TestFalse(TEXT("an enemy spawn beyond BoundsSize.X is rejected"),
		ValidateRoomDefinition(*SpawnBeyondX, KnownEnemyIds, SpawnBeyondXErrors));
	TestTrue(TEXT("spawn beyond X error names SpawnLocations"),
		SpawnBeyondXErrors.ToString().Contains(TEXT("SpawnLocations")));

	URoomDefinition* SpawnBeyondY = MakeValidTrainingRoom();
	SpawnBeyondY->Waves[0].SpawnLocations[0] = FVector(0.0, 550.0, 88.0);    // cm, beyond the 500 cm wall
	FText SpawnBeyondYErrors;
	TestFalse(TEXT("an enemy spawn beyond BoundsSize.Y is rejected"),
		ValidateRoomDefinition(*SpawnBeyondY, KnownEnemyIds, SpawnBeyondYErrors));
	TestTrue(TEXT("spawn beyond Y error names SpawnLocations"),
		SpawnBeyondYErrors.ToString().Contains(TEXT("SpawnLocations")));

	URoomDefinition* SpawnTooClose = MakeValidTrainingRoom();
	SpawnTooClose->Waves[0].SpawnLocations[0] = FVector(-350.0, 0.0, 88.0);  // cm, about 51 cm from the player spawn
	FText SpawnTooCloseErrors;
	TestFalse(TEXT("an enemy spawn closer than 300 cm to the player spawn is rejected"),
		ValidateRoomDefinition(*SpawnTooClose, KnownEnemyIds, SpawnTooCloseErrors));
	TestTrue(TEXT("too-close spawn error names SpawnLocations"),
		SpawnTooCloseErrors.ToString().Contains(TEXT("SpawnLocations")));

	URoomDefinition* SpawnNaN = MakeValidTrainingRoom();
	SpawnNaN->Waves[0].SpawnLocations[0] = FVector(std::numeric_limits<double>::quiet_NaN(), 0.0, 88.0);
	FText SpawnNaNErrors;
	TestFalse(TEXT("a non-finite enemy spawn is rejected"),
		ValidateRoomDefinition(*SpawnNaN, KnownEnemyIds, SpawnNaNErrors));
	TestTrue(TEXT("non-finite spawn error names SpawnLocations"),
		SpawnNaNErrors.ToString().Contains(TEXT("SpawnLocations")));

	URoomDefinition* PlayerBeyondY = MakeValidTrainingRoom();
	PlayerBeyondY->PlayerSpawnLocation = FVector(0.0, 600.0, 90.0);          // cm, beyond the 500 cm wall
	FText PlayerBeyondYErrors;
	TestFalse(TEXT("a player spawn beyond BoundsSize.Y is rejected"),
		ValidateRoomDefinition(*PlayerBeyondY, KnownEnemyIds, PlayerBeyondYErrors));
	TestTrue(TEXT("player spawn beyond Y error names PlayerSpawnLocation"),
		PlayerBeyondYErrors.ToString().Contains(TEXT("PlayerSpawnLocation")));

	URoomDefinition* PlayerBeyondX = MakeValidTrainingRoom();
	PlayerBeyondX->PlayerSpawnLocation = FVector(-1300.0, 0.0, 90.0);        // cm, beyond the 1200 cm wall
	FText PlayerBeyondXErrors;
	TestFalse(TEXT("a player spawn beyond BoundsSize.X is rejected"),
		ValidateRoomDefinition(*PlayerBeyondX, KnownEnemyIds, PlayerBeyondXErrors));
	TestTrue(TEXT("player spawn beyond X error names PlayerSpawnLocation"),
		PlayerBeyondXErrors.ToString().Contains(TEXT("PlayerSpawnLocation")));

	URoomDefinition* ZeroDepthBounds = MakeValidTrainingRoom();
	ZeroDepthBounds->BoundsSize = FVector(2400.0, 1000.0, 0.0);              // cm, zero Z extent
	FText ZeroDepthBoundsErrors;
	TestFalse(TEXT("a BoundsSize with a zero axis is rejected"),
		ValidateRoomDefinition(*ZeroDepthBounds, KnownEnemyIds, ZeroDepthBoundsErrors));
	TestTrue(TEXT("zero-axis BoundsSize error names BoundsSize"),
		ZeroDepthBoundsErrors.ToString().Contains(TEXT("BoundsSize")));

	URoomDefinition* NegativeWidthBounds = MakeValidTrainingRoom();
	NegativeWidthBounds->BoundsSize = FVector(2400.0, -5.0, 180.0);          // cm, negative Y extent
	FText NegativeWidthBoundsErrors;
	TestFalse(TEXT("a BoundsSize with a negative axis is rejected"),
		ValidateRoomDefinition(*NegativeWidthBounds, KnownEnemyIds, NegativeWidthBoundsErrors));
	TestTrue(TEXT("negative-axis BoundsSize error names BoundsSize"),
		NegativeWidthBoundsErrors.ToString().Contains(TEXT("BoundsSize")));

	// Several problems at once are collected and joined with "; ".
	URoomDefinition* Broken = MakeValidTrainingRoom();
	Broken->RoomId = NAME_None;
	Broken->Waves[0].Count = 0;
	Broken->Waves[0].EnemyId = NAME_None;
	FText BrokenErrors;
	TestFalse(TEXT("a room with multiple problems is rejected"),
		ValidateRoomDefinition(*Broken, KnownEnemyIds, BrokenErrors));
	TestTrue(TEXT("RoomId problem is included"), BrokenErrors.ToString().Contains(TEXT("RoomId")));
	TestTrue(TEXT("Count problem is included"), BrokenErrors.ToString().Contains(TEXT("Count")));
	TestTrue(TEXT("EnemyId problem is included"), BrokenErrors.ToString().Contains(TEXT("EnemyId")));
	TestTrue(TEXT("problems are joined with \"; \""), BrokenErrors.ToString().Contains(TEXT("; ")));
	return true;
}

#endif
