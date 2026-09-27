// M2-008: death-driven wave progression. URoomSessionSubsystem::BeginWaves
// orchestrates the waves of the running room: one fresh UWaveSpawner instance
// per wave, births driven by the injected session clock (every accepted
// SetSessionClockSeconds pumps the current wave's due births), the next wave
// starts exactly 1.0 s (injected clock) after the previous wave's last enemy
// died, and the run clears exactly once when the LAST wave is fully born and
// fully dead. Progression is driven exclusively by deaths of enemies the
// current run actually spawned: duplicate, foreign and old-run death events
// never advance or settle a wave, and an old run's leftover enemy dying during
// a new run is swallowed by the spawner's run-id filter before it can reach
// the new run's bookkeeping.
//
// The tests reuse the engine FTestWorldWrapper precedent from the M2-006 and
// M2-007 suites (a real, BeginPlay-initialized temp game world with the
// production URoomSessionSubsystem) and the M2-005 hand-rolled JSON reading
// mode (the engine Json import library is not on the UEMMO link line) to load
// room_training_01 straight out of Data/rooms.json (2+3 waves). The scene
// needs no floor and no world tick: the progression is pure bookkeeping on
// the injected clock, and deaths are applied directly through the health
// components.
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "UObject/UObjectGlobals.h"

#include <limits>

#include "../Combat/HealthComponent.h"
#include "../Enemy/EnemyDefinition.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M2_008
{
	/** Minimal JSON value: just enough shape for the room/enemy data files. */
	class FM2_008JsonValue
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
		TArray<TSharedPtr<FM2_008JsonValue>> Array;
		TMap<FString, TSharedPtr<FM2_008JsonValue>> Object;
	};

	/** Recursive descent JSON parser (RFC 8259 subset; \u escapes decode to one TCHAR). */
	class FM2_008JsonParser
	{
	public:
		static bool Parse(const FString& Text, TSharedPtr<FM2_008JsonValue>& OutRoot, FString& OutError)
		{
			FM2_008JsonParser Parser(Text);
			Parser.SkipWhitespace();
			TSharedPtr<FM2_008JsonValue> Root = Parser.ParseValue(OutError);
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
		explicit FM2_008JsonParser(const FString& InText)
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

		TSharedPtr<FM2_008JsonValue> ParseValue(FString& OutError)
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

		TSharedPtr<FM2_008JsonValue> ParseObject(FString& OutError)
		{
			if (!Expect(TEXT('{'), TEXT("to start an object"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FM2_008JsonValue> Value = MakeShared<FM2_008JsonValue>();
			Value->Kind = FM2_008JsonValue::EKind::Object;
			SkipWhitespace();
			if (Consume(TEXT('}')))
			{
				return Value;
			}
			while (true)
			{
				SkipWhitespace();
				TSharedPtr<FM2_008JsonValue> Key = ParseString(OutError);
				if (!Key.IsValid())
				{
					return nullptr;
				}
				SkipWhitespace();
				if (!Expect(TEXT(':'), TEXT("between object key and value"), OutError))
				{
					return nullptr;
				}
				TSharedPtr<FM2_008JsonValue> Element = ParseValue(OutError);
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

		TSharedPtr<FM2_008JsonValue> ParseArray(FString& OutError)
		{
			if (!Expect(TEXT('['), TEXT("to start an array"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FM2_008JsonValue> Value = MakeShared<FM2_008JsonValue>();
			Value->Kind = FM2_008JsonValue::EKind::Array;
			SkipWhitespace();
			if (Consume(TEXT(']')))
			{
				return Value;
			}
			while (true)
			{
				TSharedPtr<FM2_008JsonValue> Element = ParseValue(OutError);
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

		TSharedPtr<FM2_008JsonValue> ParseString(FString& OutError)
		{
			if (!Expect(TEXT('"'), TEXT("to start a string"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FM2_008JsonValue> Value = MakeShared<FM2_008JsonValue>();
			Value->Kind = FM2_008JsonValue::EKind::String;
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

		TSharedPtr<FM2_008JsonValue> ParseBoolean(FString& OutError)
		{
			const TSharedPtr<FM2_008JsonValue> Value = MakeShared<FM2_008JsonValue>();
			if (MatchLiteral(TEXT("true"), 4))
			{
				Value->Kind = FM2_008JsonValue::EKind::Boolean;
				Value->Boolean = true;
				Position += 4;
				return Value;
			}
			if (MatchLiteral(TEXT("false"), 5))
			{
				Value->Kind = FM2_008JsonValue::EKind::Boolean;
				Value->Boolean = false;
				Position += 5;
				return Value;
			}
			OutError = FString::Printf(TEXT("invalid literal at offset %d"), Position);
			return nullptr;
		}

		TSharedPtr<FM2_008JsonValue> ParseNull(FString& OutError)
		{
			if (MatchLiteral(TEXT("null"), 4))
			{
				const TSharedPtr<FM2_008JsonValue> Value = MakeShared<FM2_008JsonValue>();
				Value->Kind = FM2_008JsonValue::EKind::Null;
				Position += 4;
				return Value;
			}
			OutError = FString::Printf(TEXT("invalid literal at offset %d"), Position);
			return nullptr;
		}

		TSharedPtr<FM2_008JsonValue> ParseNumber(FString& OutError)
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
			const TSharedPtr<FM2_008JsonValue> Value = MakeShared<FM2_008JsonValue>();
			Value->Kind = FM2_008JsonValue::EKind::Number;
			Value->Number = FCString::Atod(*Source.Mid(Start, Position - Start));
			return Value;
		}
	};

	static FString M2_008_GetRoomsJsonPath()
	{
		return FPaths::ProjectDir() / TEXT("Data") / TEXT("rooms.json");
	}

	static FString GetEnemiesJsonPath()
	{
		return FPaths::ProjectDir() / TEXT("Data") / TEXT("enemies.json");
	}

	static bool M2_008_LoadJsonRoot(const FString& Path, FString& OutError, TSharedPtr<FM2_008JsonValue>& OutRoot)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			OutError = FString::Printf(TEXT("could not read the JSON data file at %s"), *Path);
			return false;
		}
		TSharedPtr<FM2_008JsonValue> Root;
		if (!FM2_008JsonParser::Parse(Text, Root, OutError))
		{
			OutError = FString::Printf(TEXT("could not parse JSON in %s: %s"), *Path, *OutError);
			return false;
		}
		OutRoot = Root;
		return true;
	}

	/** Returns the field only when present with exactly the requested JSON kind. */
	static const TSharedPtr<FM2_008JsonValue>* M2_008_FindTyped(const TSharedPtr<FM2_008JsonValue>& Object, const TCHAR* Name,
		FM2_008JsonValue::EKind Kind)
	{
		const TSharedPtr<FM2_008JsonValue>* Value = Object->Object.Find(Name);
		if (!Value || !Value->IsValid() || (*Value)->Kind != Kind)
		{
			return nullptr;
		}
		return Value;
	}

	static FString M2_008_RoomEntryName(const TSharedPtr<FM2_008JsonValue>& Entry)
	{
		const TSharedPtr<FM2_008JsonValue>* Id = M2_008_FindTyped(Entry, TEXT("room_id"), FM2_008JsonValue::EKind::String);
		return (Id && !(*Id)->String.IsEmpty()) ? (*Id)->String : FString(TEXT("<unnamed room>"));
	}

	static bool M2_008_GetStringOrReport(FAutomationTestBase& Test, const TSharedPtr<FM2_008JsonValue>& Entry,
		const TCHAR* Name, const FString& Context, FString& OutValue)
	{
		const TSharedPtr<FM2_008JsonValue>* Value = M2_008_FindTyped(Entry, Name, FM2_008JsonValue::EKind::String);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("%s: missing or mistyped string field '%s'"), *Context, Name));
			return false;
		}
		OutValue = (*Value)->String;
		return true;
	}

	static bool M2_008_GetNumberOrReport(FAutomationTestBase& Test, const TSharedPtr<FM2_008JsonValue>& Entry,
		const TCHAR* Name, const FString& Context, double& OutValue)
	{
		const TSharedPtr<FM2_008JsonValue>* Value = M2_008_FindTyped(Entry, Name, FM2_008JsonValue::EKind::Number);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("%s: missing or mistyped number field '%s'"), *Context, Name));
			return false;
		}
		OutValue = (*Value)->Number;
		return true;
	}

	/** Reads the {x, y, z} number members (centimeters) of the given object into an FVector. */
	static bool M2_008_GetVectorMembersOrReport(FAutomationTestBase& Test, const TSharedPtr<FM2_008JsonValue>& Object,
		const FString& Context, FVector& OutValue)
	{
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		const bool bComplete =
			M2_008_GetNumberOrReport(Test, Object, TEXT("x"), Context, X) &&
			M2_008_GetNumberOrReport(Test, Object, TEXT("y"), Context, Y) &&
			M2_008_GetNumberOrReport(Test, Object, TEXT("z"), Context, Z);
		if (!bComplete)
		{
			return false;
		}
		OutValue = FVector(X, Y, Z);
		return true;
	}

	/** Reads an {x, y, z} number object (centimeters) named on the entry into an FVector. */
	static bool M2_008_GetVectorOrReport(FAutomationTestBase& Test, const TSharedPtr<FM2_008JsonValue>& Entry,
		const TCHAR* Name, const FString& Context, FVector& OutValue)
	{
		const TSharedPtr<FM2_008JsonValue>* Value = M2_008_FindTyped(Entry, Name, FM2_008JsonValue::EKind::Object);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("%s: missing or mistyped cm vector object '%s'"), *Context, Name));
			return false;
		}
		return M2_008_GetVectorMembersOrReport(Test, *Value, Context, OutValue);
	}

	/** Comparison tolerance for doubles that both sides derive from decimal literals. */
	static bool M2_008_NearlyEqual(double A, double B)
	{
		return FMath::Abs(A - B) <= 1e-9;
	}

	/** One parsed wave entry of a room. */
	struct FM2_008WaveJson
	{
		FString EnemyId;
		double Count = 0.0;
		TArray<FVector> SpawnLocationsCm;
	};

	static bool M2_008_GetWaveOrReport(FAutomationTestBase& Test, const TSharedPtr<FM2_008JsonValue>& WaveObject,
		const FString& Context, FM2_008WaveJson& OutWave)
	{
		bool bOk = true;
		if (!M2_008_GetStringOrReport(Test, WaveObject, TEXT("enemy_id"), Context, OutWave.EnemyId))
		{
			bOk = false;
		}
		if (!M2_008_GetNumberOrReport(Test, WaveObject, TEXT("count"), Context, OutWave.Count))
		{
			bOk = false;
		}
		else if (!M2_008_NearlyEqual(OutWave.Count, FMath::RoundToZero(OutWave.Count)))
		{
			Test.AddError(FString::Printf(TEXT("%s: 'count' must be a whole number (got %f)"), *Context, OutWave.Count));
			bOk = false;
		}

		const TSharedPtr<FM2_008JsonValue>* Locations = M2_008_FindTyped(WaveObject, TEXT("spawn_locations_cm"), FM2_008JsonValue::EKind::Array);
		if (!Locations)
		{
			Test.AddError(FString::Printf(TEXT("%s: missing or mistyped cm location array 'spawn_locations_cm'"), *Context));
			return false;
		}
		int32 LocationIndex = 0;
		for (const TSharedPtr<FM2_008JsonValue>& Location : (*Locations)->Array)
		{
			const FString LocationContext = FString::Printf(TEXT("%s spawn_locations_cm[%d]"), *Context, LocationIndex);
			++LocationIndex;
			if (!Location.IsValid() || Location->Kind != FM2_008JsonValue::EKind::Object)
			{
				Test.AddError(FString::Printf(TEXT("%s is not a JSON object"), *LocationContext));
				bOk = false;
				continue;
			}
			FVector LocationCm = FVector::ZeroVector;
			if (!M2_008_GetVectorMembersOrReport(Test, Location, LocationContext, LocationCm))
			{
				bOk = false;
				continue;
			}
			OutWave.SpawnLocationsCm.Add(LocationCm);
		}
		return bOk;
	}

	/** Fills a URoomDefinition from one JSON entry; returns null and reports when fields are missing. */
	static URoomDefinition* M2_008_MakeRoomFromJson(FAutomationTestBase& Test, const TSharedPtr<FM2_008JsonValue>& Entry)
	{
		const FString Context = FString::Printf(TEXT("room '%s'"), *M2_008_RoomEntryName(Entry));

		FString RoomId;
		FString MapPath;
		FString RewardTableId;
		FVector PlayerSpawnLocationCm = FVector::ZeroVector;
		double PlayerSpawnYawDegrees = 0.0;
		FVector BoundsCenterCm = FVector::ZeroVector;
		FVector BoundsSizeCm = FVector::ZeroVector;

		bool bOk = M2_008_GetStringOrReport(Test, Entry, TEXT("room_id"), Context, RoomId);
		bOk = M2_008_GetStringOrReport(Test, Entry, TEXT("map_path"), Context, MapPath) && bOk;
		bOk = M2_008_GetStringOrReport(Test, Entry, TEXT("reward_table_id"), Context, RewardTableId) && bOk;
		bOk = M2_008_GetVectorOrReport(Test, Entry, TEXT("player_spawn_location_cm"), Context, PlayerSpawnLocationCm) && bOk;
		bOk = M2_008_GetNumberOrReport(Test, Entry, TEXT("player_spawn_yaw_degrees"), Context, PlayerSpawnYawDegrees) && bOk;
		bOk = M2_008_GetVectorOrReport(Test, Entry, TEXT("bounds_center_cm"), Context, BoundsCenterCm) && bOk;
		bOk = M2_008_GetVectorOrReport(Test, Entry, TEXT("bounds_size_cm"), Context, BoundsSizeCm) && bOk;
		if (!bOk)
		{
			return nullptr;
		}

		const TSharedPtr<FM2_008JsonValue>* Waves = M2_008_FindTyped(Entry, TEXT("waves"), FM2_008JsonValue::EKind::Array);
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
		for (const TSharedPtr<FM2_008JsonValue>& WaveObject : (*Waves)->Array)
		{
			const FString WaveContext = FString::Printf(TEXT("%s wave %d"), *Context, WaveIndex);
			++WaveIndex;
			if (!WaveObject.IsValid() || WaveObject->Kind != FM2_008JsonValue::EKind::Object)
			{
				Test.AddError(WaveContext + TEXT(" is not a JSON object"));
				return nullptr;
			}
			FM2_008WaveJson WaveJson;
			if (!M2_008_GetWaveOrReport(Test, WaveObject, WaveContext, WaveJson))
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

	// Definition double of the Data/enemies.json "melee_grunt" row (source
	// JSON is never read at runtime; the progression takes the definition as a
	// parameter - the JSON to asset loading belongs to the later catalog task,
	// same precedent as the M2-007 suite).
	static UEnemyDefinition* M2_008_MakeEnemyDef(FName EnemyId)
	{
		UEnemyDefinition* Def = NewObject<UEnemyDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Def->EnemyId = EnemyId;
		Def->MaxHP = 60.0f;
		Def->AttackPower = 0.0f;
		Def->MoveSpeed = 220.0f;
		Def->AttackRangeX = 160.0f;
		Def->AlignYTolerance = 35.0f;
		Def->TelegraphSeconds = 0.35f;
		Def->SpawnGraceSeconds = 0.5f;
		Def->MeleeAttackId = FName(TEXT("light_01"));
		return Def;
	}

	/**
	 * Loads room_training_01 straight out of Data/rooms.json with the M2-005
	 * hand-rolled JSON reading mode and sanity-checks the 2+3 wave shape the
	 * card mandates before any progression runs against it.
	 */
	static URoomDefinition* M2_008_LoadTrainingRoom(FAutomationTestBase& Test)
	{
		FString LoadError;
		TSharedPtr<FM2_008JsonValue> Root;
		if (!M2_008_LoadJsonRoot(M2_008_GetRoomsJsonPath(), LoadError, Root))
		{
			Test.AddError(LoadError);
			return nullptr;
		}
		const TSharedPtr<FM2_008JsonValue>* Rooms = M2_008_FindTyped(Root, TEXT("rooms"), FM2_008JsonValue::EKind::Array);
		if (!Rooms)
		{
			Test.AddError(TEXT("rooms.json is missing the required top-level array 'rooms'"));
			return nullptr;
		}
		for (const TSharedPtr<FM2_008JsonValue>& Entry : (*Rooms)->Array)
		{
			if (!Entry.IsValid() || Entry->Kind != FM2_008JsonValue::EKind::Object)
			{
				continue;
			}
			const TSharedPtr<FM2_008JsonValue>* Id = M2_008_FindTyped(Entry, TEXT("room_id"), FM2_008JsonValue::EKind::String);
			if (Id && (*Id)->String == TEXT("room_training_01"))
			{
				URoomDefinition* Room = M2_008_MakeRoomFromJson(Test, Entry);
				if (Room == nullptr)
				{
					return nullptr;
				}
				if (!Test.TestEqual(TEXT("room_training_01 carries exactly two waves"), Room->Waves.Num(), 2))
				{
					return nullptr;
				}
				Test.TestEqual(TEXT("wave 0 of room_training_01 spawns 2 enemies"), Room->Waves[0].Count, 2);
				Test.TestEqual(TEXT("wave 1 of room_training_01 spawns 3 enemies"), Room->Waves[1].Count, 3);
				return Room;
			}
		}
		Test.AddError(TEXT("rooms.json carries no 'room_training_01' entry"));
		return nullptr;
	}

	// An illegal room for the failure-path suite: wave 0 requests 2 enemies
	// but carries only one spawn location, so the spawner refuses the wave
	// (hand-built data per the M2-007 precedent; the json room itself is legal).
	static URoomDefinition* M2_008_MakeIllegalRoom()
	{
		URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Room->RoomId = FName(TEXT("room_m2_008_illegal"));
		FRoomWaveDefinition Wave;
		Wave.EnemyId = FName(TEXT("melee_grunt"));
		Wave.Count = 2;
		Wave.SpawnLocations.Add(FVector(300.0, 0.0, 88.0));
		Room->Waves.Add(Wave);
		return Room;
	}

	// Recorded session events (the assertions read this struct only).
	struct FM2_008_RunEvents
	{
		int32 StartedCount = 0;
		int32 EndedCount = 0;
		TArray<FRoomResult> EndedResults;

		void Bind(URoomSessionSubsystem& Session)
		{
			Session.OnRunStarted().AddLambda([this]()
			{
				++StartedCount;
			});
			Session.OnRunEnded().AddLambda([this](const FRoomResult& Result)
			{
				++EndedCount;
				EndedResults.Add(Result);
			});
		}
	};

	// One temp game world with its real session subsystem per test (the
	// FTestWorldWrapper precedent of the M2-006/M2-007 suites). The session
	// owns the per-wave UWaveSpawner instances itself, so the scene needs no
	// spawner object and no floor geometry.
	struct FM2_008_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		URoomSessionSubsystem* Session = nullptr;

		bool Build(FAutomationTestBase& Test)
		{
			if (!Test.TestTrue(TEXT("the test world is created (engine FTestWorldWrapper precedent)"),
				Wrapper.CreateTestWorld(EWorldType::Game)))
			{
				return false;
			}
			World = Wrapper.GetTestWorld();
			if (!Test.TestNotNull(TEXT("the test world is available"), World))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("play begins in the test world (full actor initialization)"),
				Wrapper.BeginPlayInTestWorld()))
			{
				return false;
			}
			Session = World->GetSubsystem<URoomSessionSubsystem>();
			if (!Test.TestNotNull(TEXT("the room session subsystem exists in the test world"), Session))
			{
				return false;
			}
			return true;
		}
	};

	// Number of ALIVE melee enemies in the world (dead ones are corpses and
	// deliberately not counted).
	static int32 M2_008_CountAliveEnemies(UWorld& World)
	{
		int32 Count = 0;
		for (TActorIterator<AMeleeEnemy> It(&World); It; ++It)
		{
			UHealthComponent* Health = It->GetHealthComponent();
			if (Health != nullptr && Health->IsAlive())
			{
				++Count;
			}
		}
		return Count;
	}

	// First ALIVE melee enemy standing at the given world-space location; the
	// alive-only filter keeps the corpse of an earlier wave or run from
	// shadowing the fresh enemy born at the same configured location.
	static AMeleeEnemy* M2_008_FindAliveEnemyAt(UWorld& World, const FVector& Location)
	{
		for (TActorIterator<AMeleeEnemy> It(&World); It; ++It)
		{
			UHealthComponent* Health = It->GetHealthComponent();
			if (Health == nullptr || !Health->IsAlive())
			{
				continue;
			}
			if (FVector::DistSquared(It->GetActorLocation(), Location) < 1.0)
			{
				return *It;
			}
		}
		return nullptr;
	}

	// Kills one enemy through its health component and verifies the death took.
	static bool M2_008_KillEnemy(FAutomationTestBase& Test, AMeleeEnemy* Enemy, const TCHAR* What)
	{
		if (!Test.TestNotNull(What, Enemy))
		{
			return false;
		}
		Enemy->GetHealthComponent()->ApplyDamage(999.0f);
		return Test.TestTrue(TEXT("the enemy died from the applied damage"),
			Enemy->GetHealthComponent() != nullptr && !Enemy->GetHealthComponent()->IsAlive());
	}

}

using namespace UE::UEMMO::Tasks::M2_008;

// 1. Acceptance: the 2+3 waves of room_training_01 (loaded from Data/rooms.json)
//    progress strictly in order - wave 0's two enemies must all die before the
//    1.0 s wait, wave 1's three enemies start and die, and only the last death
//    clears the run, with OnRunEnded firing exactly once. At every intermediate
//    point the session is still Running and no end event fired (no early
//    settlement while an enemy is alive or unborn).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_008TwoWavesProgressInOrderAndClearExactlyOnce,
	"UEMMO.Tasks.M2_008.TwoWavesProgressInOrderAndClearExactlyOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_008TwoWavesProgressInOrderAndClearExactlyOnce::RunTest(const FString& Parameters)
{
	FM2_008_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_008_LoadTrainingRoom(*this);
	if (Room == nullptr)
	{
		return true;
	}
	UEnemyDefinition* Def = M2_008_MakeEnemyDef(FName(TEXT("melee_grunt")));
	const FVector& LocW0A = Room->Waves[0].SpawnLocations[0];
	const FVector& LocW0B = Room->Waves[0].SpawnLocations[1];
	const FVector& LocW1A = Room->Waves[1].SpawnLocations[0];
	const FVector& LocW1B = Room->Waves[1].SpawnLocations[1];
	const FVector& LocW1C = Room->Waves[1].SpawnLocations[2];

	FM2_008_RunEvents Events;
	Events.Bind(*Scene.Session);

	if (!TestTrue(TEXT("StartRoom accepts the run the progression belongs to"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 RunId = Scene.Session->GetRunId();
	TestTrue(TEXT("BeginWaves starts the progression from wave 0 while Running"),
		Scene.Session->BeginWaves(Room, Def));
	TestEqual(TEXT("wave 0 is the recorded wave index after BeginWaves"), Scene.Session->GetCurrentWaveIndex(), 0);
	TestEqual(TEXT("exactly one wave was started so far"), Scene.Session->GetStartedWaveCount(), 1);

	// The injected clock drives the births: anchor at 0, slot 1 at 0.3 s.
	Scene.Session->SetSessionClockSeconds(0.0);
	TestEqual(TEXT("the anchor injection birthed one wave-0 enemy"), Scene.Session->GetSpawnedEnemyCount(), 1);
	Scene.Session->SetSessionClockSeconds(0.3);
	TestEqual(TEXT("both wave-0 enemies were birthed by the 0.3 s mark"), Scene.Session->GetSpawnedEnemyCount(), 2);
	TestEqual(TEXT("both wave-0 enemies are alive in the world"), M2_008_CountAliveEnemies(*Scene.World), 2);

	// One alive enemy of two blocks the progression even far past the gap.
	AMeleeEnemy* EnemyA = M2_008_FindAliveEnemyAt(*Scene.World, LocW0A);
	if (!M2_008_KillEnemy(*this, EnemyA, TEXT("the first wave-0 enemy was found at its configured location")))
	{
		return true;
	}
	TestEqual(TEXT("the first wave-0 death was counted"), Scene.Session->GetKilledCount(), 1);
	Scene.Session->SetSessionClockSeconds(2.0);
	Scene.Session->SetSessionClockSeconds(5.0);
	TestTrue(TEXT("one alive wave-0 enemy keeps the run Running"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("one alive wave-0 enemy keeps the wave index at 0"), Scene.Session->GetCurrentWaveIndex(), 0);
	TestEqual(TEXT("no end event fired while a wave-0 enemy is alive"), Events.EndedCount, 0);
	TestEqual(TEXT("no wave-1 enemy was born while wave 0 is unfinished"), Scene.Session->GetSpawnedEnemyCount(), 2);

	// The last wave-0 death completes the wave: the 1.0 s wait is armed but
	// the run must not settle early at any point.
	AMeleeEnemy* EnemyB = M2_008_FindAliveEnemyAt(*Scene.World, LocW0B);
	if (!M2_008_KillEnemy(*this, EnemyB, TEXT("the second wave-0 enemy was found at its configured location")))
	{
		return true;
	}
	TestEqual(TEXT("both wave-0 deaths were counted"), Scene.Session->GetKilledCount(), 2);
	TestTrue(TEXT("the completed wave 0 alone does not clear the run (still Running)"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("the completed wave 0 fires no end event yet"), Events.EndedCount, 0);
	TestEqual(TEXT("the next wave has not started right after the last death"), Scene.Session->GetCurrentWaveIndex(), 0);

	Scene.Session->SetSessionClockSeconds(5.5);
	TestTrue(TEXT("0.5 s into the wait the run is still Running"), Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("0.5 s into the wait the wave index is still 0"), Scene.Session->GetCurrentWaveIndex(), 0);
	TestEqual(TEXT("0.5 s into the wait no wave-1 enemy was born"), Scene.Session->GetSpawnedEnemyCount(), 2);

	// Exactly at the 1.0 s mark wave 1 starts (its first slot births at once).
	Scene.Session->SetSessionClockSeconds(6.0);
	TestEqual(TEXT("after the full 1.0 s wait the wave index is 1"), Scene.Session->GetCurrentWaveIndex(), 1);
	TestEqual(TEXT("exactly two waves were started by the session"), Scene.Session->GetStartedWaveCount(), 2);
	TestEqual(TEXT("wave 1 birthed its first slot right after its start"), Scene.Session->GetSpawnedEnemyCount(), 3);
	Scene.Session->SetSessionClockSeconds(6.3);
	Scene.Session->SetSessionClockSeconds(6.6);
	TestEqual(TEXT("all three wave-1 enemies were birthed on their 0.3 s slots"), Scene.Session->GetSpawnedEnemyCount(), 5);
	TestEqual(TEXT("all three wave-1 enemies are alive in the world"), M2_008_CountAliveEnemies(*Scene.World), 3);

	// Every wave-1 death is required; only the LAST one clears the run.
	AMeleeEnemy* EnemyC = M2_008_FindAliveEnemyAt(*Scene.World, LocW1A);
	if (!M2_008_KillEnemy(*this, EnemyC, TEXT("the first wave-1 enemy was found at its configured location")))
	{
		return true;
	}
	TestTrue(TEXT("two alive wave-1 enemies keep the run Running"), Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("no end event fired with two wave-1 enemies alive"), Events.EndedCount, 0);
	AMeleeEnemy* EnemyD = M2_008_FindAliveEnemyAt(*Scene.World, LocW1B);
	if (!M2_008_KillEnemy(*this, EnemyD, TEXT("the second wave-1 enemy was found at its configured location")))
	{
		return true;
	}
	TestTrue(TEXT("one alive wave-1 enemy keeps the run Running"), Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("no end event fired with one wave-1 enemy alive"), Events.EndedCount, 0);

	AMeleeEnemy* EnemyE = M2_008_FindAliveEnemyAt(*Scene.World, LocW1C);
	if (!M2_008_KillEnemy(*this, EnemyE, TEXT("the third wave-1 enemy was found at its configured location")))
	{
		return true;
	}
	TestTrue(TEXT("the run is Cleared after the last wave-1 death"),
		Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("OnRunEnded fired exactly once for the whole progression"), Events.EndedCount, 1);
	if (Events.EndedResults.Num() == 1)
	{
		TestTrue(TEXT("the end result records Cleared"), Events.EndedResults[0].bCleared);
		TestEqual(TEXT("the end result counts exactly the five real deaths"), Events.EndedResults[0].KilledCount, 5);
		TestEqual(TEXT("the end result keeps the run id"), Events.EndedResults[0].RunId, RunId);
	}
	TestFalse(TEXT("a duplicate MarkCleared after the progression cleared is rejected"), Scene.Session->MarkCleared());
	TestEqual(TEXT("the duplicate clear fired no second end event"), Events.EndedCount, 1);
	return true;
}

// 2. Acceptance: neither one ALIVE enemy nor one UNBORN enemy lets the
//    progression advance - the session stays on the current wave through any
//    amount of injected time until the blocking enemy dies (alive case) or is
//    born and dies (unborn case).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_008AliveOrUnbornEnemyBlocksProgression,
	"UEMMO.Tasks.M2_008.AliveOrUnbornEnemyBlocksProgression",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_008AliveOrUnbornEnemyBlocksProgression::RunTest(const FString& Parameters)
{
	FM2_008_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_008_LoadTrainingRoom(*this);
	if (Room == nullptr)
	{
		return true;
	}
	UEnemyDefinition* Def = M2_008_MakeEnemyDef(FName(TEXT("melee_grunt")));
	const FVector& LocW0A = Room->Waves[0].SpawnLocations[0];
	const FVector& LocW0B = Room->Waves[0].SpawnLocations[1];

	FM2_008_RunEvents Events;
	Events.Bind(*Scene.Session);

	// Sub-case A: one ALIVE enemy blocks the progression.
	if (!TestTrue(TEXT("the first run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	TestTrue(TEXT("the first run begins its waves"), Scene.Session->BeginWaves(Room, Def));
	Scene.Session->SetSessionClockSeconds(0.0);
	Scene.Session->SetSessionClockSeconds(0.3);
	TestEqual(TEXT("both wave-0 enemies were birthed"), Scene.Session->GetSpawnedEnemyCount(), 2);

	AMeleeEnemy* EnemyA = M2_008_FindAliveEnemyAt(*Scene.World, LocW0A);
	if (!M2_008_KillEnemy(*this, EnemyA, TEXT("the first wave-0 enemy was found for the alive-block case")))
	{
		return true;
	}
	Scene.Session->SetSessionClockSeconds(2.0);
	Scene.Session->SetSessionClockSeconds(5.0);
	TestTrue(TEXT("the alive enemy kept the run Running far past the gap"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("the alive enemy kept the wave index at 0"), Scene.Session->GetCurrentWaveIndex(), 0);
	TestEqual(TEXT("no end event fired while the alive enemy blocked"), Events.EndedCount, 0);

	// Complete wave 0 for a clean run boundary, then end run 1 on purpose.
	AMeleeEnemy* EnemyB = M2_008_FindAliveEnemyAt(*Scene.World, LocW0B);
	if (!M2_008_KillEnemy(*this, EnemyB, TEXT("the second wave-0 enemy was found for the alive-block case")))
	{
		return true;
	}
	TestTrue(TEXT("FailRun ends run 1 from Running (setup for the fresh run)"), Scene.Session->FailRun());
	TestTrue(TEXT("run 1 is Failed"), Scene.Session->GetState() == ERoomSessionState::Failed);
	if (!TestTrue(TEXT("the session resets to Idle from Failed"), Scene.Session->ResetToIdle()))
	{
		return true;
	}

	// Sub-case B: one UNBORN enemy blocks the progression.
	if (!TestTrue(TEXT("the second run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	TestTrue(TEXT("the second run begins its waves"), Scene.Session->BeginWaves(Room, Def));
	Scene.Session->SetSessionClockSeconds(10.0);
	TestEqual(TEXT("only the first slot was born at the anchor"), Scene.Session->GetSpawnedEnemyCount(), 1);
	AMeleeEnemy* Slot0 = M2_008_FindAliveEnemyAt(*Scene.World, LocW0A);
	if (!M2_008_KillEnemy(*this, Slot0, TEXT("the unborn-block run's first slot was found")))
	{
		return true;
	}
	// Alive ids hit zero but the second enemy is still UNBORN (due at 10.3 s):
	// the wave is not settled and the wait is never armed.
	Scene.Session->SetSessionClockSeconds(10.2);
	TestTrue(TEXT("the unborn enemy keeps the run Running"), Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("the unborn enemy keeps the wave index at 0"), Scene.Session->GetCurrentWaveIndex(), 0);
	TestEqual(TEXT("only run 1's failure fired an end event so far"), Events.EndedCount, 1);
	// Driving past the armed-by-bug wait (had the pending enemy been ignored,
	// the wait would elapse at 11.0 s) must still not start wave 1.
	Scene.Session->SetSessionClockSeconds(11.0);
	TestEqual(TEXT("the overdue birth happened at the 11.0 s injection"), Scene.Session->GetSpawnedEnemyCount(), 2);
	TestEqual(TEXT("the unborn enemy blocked the progression through 11.0 s"), Scene.Session->GetCurrentWaveIndex(), 0);
	TestEqual(TEXT("no second end event fired"), Events.EndedCount, 1);

	// Only after the second enemy was born AND dies does the wait elapse.
	AMeleeEnemy* Slot1 = M2_008_FindAliveEnemyAt(*Scene.World, LocW0B);
	if (!M2_008_KillEnemy(*this, Slot1, TEXT("the second slot was found after its overdue birth")))
	{
		return true;
	}
	Scene.Session->SetSessionClockSeconds(11.5);
	TestEqual(TEXT("0.5 s into the wait the wave index is still 0"), Scene.Session->GetCurrentWaveIndex(), 0);
	Scene.Session->SetSessionClockSeconds(12.0);
	TestEqual(TEXT("after the full wait the second run reached wave 1"), Scene.Session->GetCurrentWaveIndex(), 1);
	return true;
}

// 3. Acceptance: a kill notification with an id the run never spawned (a
//    foreign/stale notification) never advances the waves - both real wave-0
//    deaths are still required before the wait and wave 1.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_008ForeignKillNotificationDoesNotAdvanceWaves,
	"UEMMO.Tasks.M2_008.ForeignKillNotificationDoesNotAdvanceWaves",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_008ForeignKillNotificationDoesNotAdvanceWaves::RunTest(const FString& Parameters)
{
	FM2_008_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_008_LoadTrainingRoom(*this);
	if (Room == nullptr)
	{
		return true;
	}
	UEnemyDefinition* Def = M2_008_MakeEnemyDef(FName(TEXT("melee_grunt")));
	const FVector& LocW0A = Room->Waves[0].SpawnLocations[0];
	const FVector& LocW0B = Room->Waves[0].SpawnLocations[1];

	FM2_008_RunEvents Events;
	Events.Bind(*Scene.Session);
	if (!TestTrue(TEXT("the run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	TestTrue(TEXT("the waves begin"), Scene.Session->BeginWaves(Room, Def));
	Scene.Session->SetSessionClockSeconds(0.0);
	Scene.Session->SetSessionClockSeconds(0.3);
	TestEqual(TEXT("both wave-0 enemies were birthed"), Scene.Session->GetSpawnedEnemyCount(), 2);

	// Direct session-level notification with a never-spawned id: the M2-006
	// counting semantics stay untouched (accepted while Running), but the
	// progression must not move.
	TestTrue(TEXT("a direct kill notification while Running is still accepted (M2-006 semantics)"),
		Scene.Session->NotifyEnemyKilled(FName(TEXT("stale_enemy_from_an_old_run"))));
	TestEqual(TEXT("the foreign notification was counted (M2-006 semantics)"), Scene.Session->GetKilledCount(), 1);
	TestEqual(TEXT("the foreign notification started no wave"), Scene.Session->GetCurrentWaveIndex(), 0);
	TestEqual(TEXT("the foreign notification fired no end event"), Events.EndedCount, 0);
	Scene.Session->SetSessionClockSeconds(1.0);
	Scene.Session->SetSessionClockSeconds(5.0);
	TestTrue(TEXT("the foreign notification left the run Running"), Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("the foreign notification kept the wave index at 0"), Scene.Session->GetCurrentWaveIndex(), 0);
	TestEqual(TEXT("no wave-1 enemy was born because of the foreign notification"), Scene.Session->GetSpawnedEnemyCount(), 2);

	// Both real wave-0 deaths are still required, in order.
	AMeleeEnemy* EnemyA = M2_008_FindAliveEnemyAt(*Scene.World, LocW0A);
	if (!M2_008_KillEnemy(*this, EnemyA, TEXT("the first wave-0 enemy was found after the foreign notification")))
	{
		return true;
	}
	TestEqual(TEXT("the first real death was counted on top of the foreign one"), Scene.Session->GetKilledCount(), 2);
	Scene.Session->SetSessionClockSeconds(5.5);
	TestEqual(TEXT("one alive enemy still blocks after the foreign notification"), Scene.Session->GetCurrentWaveIndex(), 0);
	AMeleeEnemy* EnemyB = M2_008_FindAliveEnemyAt(*Scene.World, LocW0B);
	if (!M2_008_KillEnemy(*this, EnemyB, TEXT("the second wave-0 enemy was found after the foreign notification")))
	{
		return true;
	}
	Scene.Session->SetSessionClockSeconds(6.5);
	TestEqual(TEXT("the wait after the real deaths elapses on schedule"), Scene.Session->GetCurrentWaveIndex(), 1);
	return true;
}

// 4. Acceptance: a duplicate death notification of the SAME enemy (a second
//    death lifecycle after a health reset) is idempotent - it neither counts
//    twice nor skips the wave; the second real death and the full 1.0 s wait
//    are still required before wave 1.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_008DuplicateEnemyDeathDoesNotSkipTheWave,
	"UEMMO.Tasks.M2_008.DuplicateEnemyDeathDoesNotSkipTheWave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_008DuplicateEnemyDeathDoesNotSkipTheWave::RunTest(const FString& Parameters)
{
	FM2_008_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_008_LoadTrainingRoom(*this);
	if (Room == nullptr)
	{
		return true;
	}
	UEnemyDefinition* Def = M2_008_MakeEnemyDef(FName(TEXT("melee_grunt")));
	const FVector& LocW0A = Room->Waves[0].SpawnLocations[0];
	const FVector& LocW0B = Room->Waves[0].SpawnLocations[1];
	const FVector& LocW1A = Room->Waves[1].SpawnLocations[0];
	const FVector& LocW1B = Room->Waves[1].SpawnLocations[1];
	const FVector& LocW1C = Room->Waves[1].SpawnLocations[2];

	FM2_008_RunEvents Events;
	Events.Bind(*Scene.Session);
	if (!TestTrue(TEXT("the run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	TestTrue(TEXT("the waves begin"), Scene.Session->BeginWaves(Room, Def));
	Scene.Session->SetSessionClockSeconds(0.0);
	Scene.Session->SetSessionClockSeconds(0.3);
	TestEqual(TEXT("both wave-0 enemies were birthed"), Scene.Session->GetSpawnedEnemyCount(), 2);

	AMeleeEnemy* EnemyA = M2_008_FindAliveEnemyAt(*Scene.World, LocW0A);
	if (!M2_008_KillEnemy(*this, EnemyA, TEXT("the first wave-0 enemy was found for the duplicate case")))
	{
		return true;
	}
	TestEqual(TEXT("the first real death was counted"), Scene.Session->GetKilledCount(), 1);

	// Duplicate death of the SAME enemy: ResetHealth opens a second death
	// lifecycle and OnDied fires again (the M2-007 duplicate precedent).
	EnemyA->GetHealthComponent()->ResetHealth();
	EnemyA->GetHealthComponent()->ApplyDamage(999.0f);
	TestEqual(TEXT("the duplicate death did not count again"), Scene.Session->GetKilledCount(), 1);
	TestEqual(TEXT("the duplicate death started no wave"), Scene.Session->GetCurrentWaveIndex(), 0);
	TestTrue(TEXT("the duplicate death left the run Running"), Scene.Session->GetState() == ERoomSessionState::Running);
	Scene.Session->SetSessionClockSeconds(1.0);
	TestEqual(TEXT("the duplicate death armed no early wave start"), Scene.Session->GetCurrentWaveIndex(), 0);
	TestEqual(TEXT("no wave-1 enemy was born because of the duplicate"), Scene.Session->GetSpawnedEnemyCount(), 2);

	// The second REAL death is still required; then the wait runs its course.
	AMeleeEnemy* EnemyB = M2_008_FindAliveEnemyAt(*Scene.World, LocW0B);
	if (!M2_008_KillEnemy(*this, EnemyB, TEXT("the second wave-0 enemy was found for the duplicate case")))
	{
		return true;
	}
	TestEqual(TEXT("the second real death was counted"), Scene.Session->GetKilledCount(), 2);
	Scene.Session->SetSessionClockSeconds(1.5);
	TestEqual(TEXT("0.5 s into the wait the wave index is still 0"), Scene.Session->GetCurrentWaveIndex(), 0);
	Scene.Session->SetSessionClockSeconds(2.0);
	TestEqual(TEXT("the wait elapsed on schedule despite the duplicate"), Scene.Session->GetCurrentWaveIndex(), 1);

	// Finish the progression for the exactly-once settlement proof.
	Scene.Session->SetSessionClockSeconds(2.3);
	Scene.Session->SetSessionClockSeconds(2.6);
	TestEqual(TEXT("all three wave-1 enemies were birthed"), Scene.Session->GetSpawnedEnemyCount(), 5);
	AMeleeEnemy* EnemyC = M2_008_FindAliveEnemyAt(*Scene.World, LocW1A);
	AMeleeEnemy* EnemyD = M2_008_FindAliveEnemyAt(*Scene.World, LocW1B);
	AMeleeEnemy* EnemyE = M2_008_FindAliveEnemyAt(*Scene.World, LocW1C);
	if (!M2_008_KillEnemy(*this, EnemyC, TEXT("the first wave-1 enemy was found for the duplicate case"))
		|| !M2_008_KillEnemy(*this, EnemyD, TEXT("the second wave-1 enemy was found for the duplicate case"))
		|| !M2_008_KillEnemy(*this, EnemyE, TEXT("the third wave-1 enemy was found for the duplicate case")))
	{
		return true;
	}
	TestTrue(TEXT("the run is Cleared after the real deaths"), Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("OnRunEnded fired exactly once"), Events.EndedCount, 1);
	if (Events.EndedResults.Num() == 1)
	{
		TestEqual(TEXT("the result counts exactly the five real deaths (no duplicate)"),
			Events.EndedResults[0].KilledCount, 5);
	}
	return true;
}

// 5. Acceptance: an enemy of an OLD run dying while the NEW run is already
//    progressing affects the new run in nothing - not the kill count, not the
//    wave state, not the settlement; the new run then completes its own 2+3
//    waves normally.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_008OldRunEnemyDeathDoesNotAffectNewRun,
	"UEMMO.Tasks.M2_008.OldRunEnemyDeathDoesNotAffectNewRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_008OldRunEnemyDeathDoesNotAffectNewRun::RunTest(const FString& Parameters)
{
	FM2_008_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_008_LoadTrainingRoom(*this);
	if (Room == nullptr)
	{
		return true;
	}
	UEnemyDefinition* Def = M2_008_MakeEnemyDef(FName(TEXT("melee_grunt")));
	const FVector& LocW0A = Room->Waves[0].SpawnLocations[0];
	const FVector& LocW0B = Room->Waves[0].SpawnLocations[1];
	const FVector& LocW1A = Room->Waves[1].SpawnLocations[0];
	const FVector& LocW1B = Room->Waves[1].SpawnLocations[1];
	const FVector& LocW1C = Room->Waves[1].SpawnLocations[2];

	FM2_008_RunEvents Events;
	Events.Bind(*Scene.Session);

	// Run 1: spawn both wave-0 enemies, kill one, fail the run. Its second
	// enemy is left behind ALIVE in the world (a failed run never destroys
	// actors) and its death binding stays wired to the old spawner.
	if (!TestTrue(TEXT("the old run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 OldRunId = Scene.Session->GetRunId();
	TestTrue(TEXT("the old run begins its waves"), Scene.Session->BeginWaves(Room, Def));
	Scene.Session->SetSessionClockSeconds(0.0);
	Scene.Session->SetSessionClockSeconds(0.3);
	AMeleeEnemy* OldA = M2_008_FindAliveEnemyAt(*Scene.World, LocW0A);
	AMeleeEnemy* OldB = M2_008_FindAliveEnemyAt(*Scene.World, LocW0B);
	if (!M2_008_KillEnemy(*this, OldA, TEXT("the old run's first enemy was found"))
		|| !TestNotNull(TEXT("the old run's second enemy was found"), OldB))
	{
		return true;
	}
	TestTrue(TEXT("the old run fails on purpose"), Scene.Session->FailRun());
	TestTrue(TEXT("the old run is Failed"), Scene.Session->GetState() == ERoomSessionState::Failed);
	TestTrue(TEXT("the old run's leftover enemy is still valid"), IsValid(OldB));
	if (OldB != nullptr)
	{
		TestTrue(TEXT("the old run's leftover enemy is still alive"),
			OldB->GetHealthComponent() != nullptr && OldB->GetHealthComponent()->IsAlive());
	}

	// New run on the same session: the RunId moves on, the waves restart.
	if (!TestTrue(TEXT("the session resets to Idle from the failed run"), Scene.Session->ResetToIdle())
		|| !TestTrue(TEXT("the new run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 NewRunId = Scene.Session->GetRunId();
	TestTrue(TEXT("the new run carries a fresh, larger run id"), NewRunId > OldRunId);
	TestTrue(TEXT("the new run begins its waves"), Scene.Session->BeginWaves(Room, Def));
	Scene.Session->SetSessionClockSeconds(10.0);
	Scene.Session->SetSessionClockSeconds(10.3);
	TestEqual(TEXT("the new run birthed both of its wave-0 enemies"), Scene.Session->GetSpawnedEnemyCount(), 2);

	// The OLD run's leftover enemy dies NOW, during the new run.
	OldB->GetHealthComponent()->ApplyDamage(999.0f);
	TestTrue(TEXT("the old run's leftover enemy died from the damage"),
		OldB->GetHealthComponent() != nullptr && !OldB->GetHealthComponent()->IsAlive());
	TestEqual(TEXT("the old-run death was NOT counted in the new run"), Scene.Session->GetKilledCount(), 0);
	TestEqual(TEXT("the old-run death started no wave in the new run"), Scene.Session->GetCurrentWaveIndex(), 0);
	TestTrue(TEXT("the old-run death left the new run Running"), Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("the old-run death fired no new end event"), Events.EndedCount, 1);
	Scene.Session->SetSessionClockSeconds(12.0);
	TestEqual(TEXT("the old-run death did not skip the new run's wait"), Scene.Session->GetCurrentWaveIndex(), 0);

	// The new run then completes its own 2+3 waves normally: both of its own
	// wave-0 deaths are still required.
	AMeleeEnemy* NewA = M2_008_FindAliveEnemyAt(*Scene.World, LocW0A);
	AMeleeEnemy* NewB = M2_008_FindAliveEnemyAt(*Scene.World, LocW0B);
	if (!M2_008_KillEnemy(*this, NewA, TEXT("the new run's first wave-0 enemy was found"))
		|| !M2_008_KillEnemy(*this, NewB, TEXT("the new run's second wave-0 enemy was found")))
	{
		return true;
	}
	TestEqual(TEXT("the new run counted exactly its two real deaths"), Scene.Session->GetKilledCount(), 2);
	Scene.Session->SetSessionClockSeconds(12.6);
	TestEqual(TEXT("0.5 s into the new run's wait the wave index is still 0"), Scene.Session->GetCurrentWaveIndex(), 0);
	Scene.Session->SetSessionClockSeconds(13.1);
	Scene.Session->SetSessionClockSeconds(13.4);
	Scene.Session->SetSessionClockSeconds(13.7);
	TestEqual(TEXT("the new run birthed all three wave-1 enemies"), Scene.Session->GetSpawnedEnemyCount(), 5);
	AMeleeEnemy* NewC = M2_008_FindAliveEnemyAt(*Scene.World, LocW1A);
	AMeleeEnemy* NewD = M2_008_FindAliveEnemyAt(*Scene.World, LocW1B);
	AMeleeEnemy* NewE = M2_008_FindAliveEnemyAt(*Scene.World, LocW1C);
	if (!M2_008_KillEnemy(*this, NewC, TEXT("the new run's first wave-1 enemy was found"))
		|| !M2_008_KillEnemy(*this, NewD, TEXT("the new run's second wave-1 enemy was found"))
		|| !M2_008_KillEnemy(*this, NewE, TEXT("the new run's third wave-1 enemy was found")))
	{
		return true;
	}
	TestTrue(TEXT("the new run is Cleared"), Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("the session fired exactly two end events (failed run 1, cleared run 2)"), Events.EndedCount, 2);
	if (Events.EndedResults.Num() == 2)
	{
		TestTrue(TEXT("run 1's end result stays Failed"), !Events.EndedResults[0].bCleared);
		TestTrue(TEXT("run 2's end result is Cleared"), Events.EndedResults[1].bCleared);
		TestEqual(TEXT("run 2's result carries the new run id"), Events.EndedResults[1].RunId, NewRunId);
		TestEqual(TEXT("run 2's result counts exactly its five real deaths (no old-run death)"),
			Events.EndedResults[1].KilledCount, 5);
	}
	return true;
}

// 6. Acceptance: the second wave starts with FRESH bookkeeping - the session
//    started a second spawner instance, no wave-0 counter leaks into it, wave
//    1 needs all three of ITS OWN deaths, and the totals stay exact (2+3).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_008SecondWaveStartsWithFreshCounters,
	"UEMMO.Tasks.M2_008.SecondWaveStartsWithFreshCounters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_008SecondWaveStartsWithFreshCounters::RunTest(const FString& Parameters)
{
	FM2_008_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_008_LoadTrainingRoom(*this);
	if (Room == nullptr)
	{
		return true;
	}
	UEnemyDefinition* Def = M2_008_MakeEnemyDef(FName(TEXT("melee_grunt")));
	const FVector& LocW0A = Room->Waves[0].SpawnLocations[0];
	const FVector& LocW0B = Room->Waves[0].SpawnLocations[1];
	const FVector& LocW1A = Room->Waves[1].SpawnLocations[0];
	const FVector& LocW1B = Room->Waves[1].SpawnLocations[1];
	const FVector& LocW1C = Room->Waves[1].SpawnLocations[2];

	FM2_008_RunEvents Events;
	Events.Bind(*Scene.Session);
	if (!TestTrue(TEXT("the run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	TestTrue(TEXT("the waves begin"), Scene.Session->BeginWaves(Room, Def));
	Scene.Session->SetSessionClockSeconds(0.0);
	Scene.Session->SetSessionClockSeconds(0.3);
	TestEqual(TEXT("wave 0 birthed exactly two enemies"), Scene.Session->GetSpawnedEnemyCount(), 2);
	AMeleeEnemy* EnemyA = M2_008_FindAliveEnemyAt(*Scene.World, LocW0A);
	AMeleeEnemy* EnemyB = M2_008_FindAliveEnemyAt(*Scene.World, LocW0B);
	if (!M2_008_KillEnemy(*this, EnemyA, TEXT("the first wave-0 enemy was found for the fresh-counter case"))
		|| !M2_008_KillEnemy(*this, EnemyB, TEXT("the second wave-0 enemy was found for the fresh-counter case")))
	{
		return true;
	}
	TestEqual(TEXT("wave 0's two kills are counted"), Scene.Session->GetKilledCount(), 2);

	// Wave 1 starts on a FRESH spawner instance: the wave-0 state (already all
	// dead) must not leak into it - the run is not settled by the wave start
	// and wave 1 births its own three enemies from zero. Both deaths happened
	// at clock 0.3, so the 1.0 s wait elapses exactly at 1.3.
	Scene.Session->SetSessionClockSeconds(0.8);
	TestEqual(TEXT("0.5 s into the wait the wave index is still 0"), Scene.Session->GetCurrentWaveIndex(), 0);
	TestEqual(TEXT("0.5 s into the wait only wave 0 was started"), Scene.Session->GetStartedWaveCount(), 1);
	Scene.Session->SetSessionClockSeconds(1.3);
	TestEqual(TEXT("wave 1 was started as the second wave"), Scene.Session->GetStartedWaveCount(), 2);
	TestEqual(TEXT("the wave index moved to 1"), Scene.Session->GetCurrentWaveIndex(), 1);
	TestTrue(TEXT("the wave-1 start did not settle the run (still Running)"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("the wave-1 start fired no end event"), Events.EndedCount, 0);
	TestEqual(TEXT("wave 1 birthed its first slot from zero (run total 2+1)"), Scene.Session->GetSpawnedEnemyCount(), 3);
	Scene.Session->SetSessionClockSeconds(1.6);
	Scene.Session->SetSessionClockSeconds(1.9);
	TestEqual(TEXT("wave 1 birthed all three of its enemies (run total 2+3)"), Scene.Session->GetSpawnedEnemyCount(), 5);
	TestEqual(TEXT("exactly three wave-1 enemies are alive now"), M2_008_CountAliveEnemies(*Scene.World), 3);

	// Wave 0's two kills carry no credit: all three wave-1 deaths are required.
	AMeleeEnemy* EnemyC = M2_008_FindAliveEnemyAt(*Scene.World, LocW1A);
	if (!M2_008_KillEnemy(*this, EnemyC, TEXT("the first wave-1 enemy was found for the fresh-counter case")))
	{
		return true;
	}
	TestTrue(TEXT("one wave-1 death alone does not clear the run"), Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("no end event after one wave-1 death"), Events.EndedCount, 0);
	AMeleeEnemy* EnemyD = M2_008_FindAliveEnemyAt(*Scene.World, LocW1B);
	if (!M2_008_KillEnemy(*this, EnemyD, TEXT("the second wave-1 enemy was found for the fresh-counter case")))
	{
		return true;
	}
	TestTrue(TEXT("two wave-1 deaths alone do not clear the run"), Scene.Session->GetState() == ERoomSessionState::Running);
	AMeleeEnemy* EnemyE = M2_008_FindAliveEnemyAt(*Scene.World, LocW1C);
	if (!M2_008_KillEnemy(*this, EnemyE, TEXT("the third wave-1 enemy was found for the fresh-counter case")))
	{
		return true;
	}
	TestTrue(TEXT("the third wave-1 death clears the run"), Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestEqual(TEXT("the total kill count stays exactly 2+3"), Scene.Session->GetKilledCount(), 5);
	TestEqual(TEXT("OnRunEnded fired exactly once"), Events.EndedCount, 1);
	return true;
}

// 7. Acceptance: the card's failure path - any refused wave start (a wave
//    whose Count does not match its spawn locations) ends the run through
//    FailRun (first terminal state wins), with nothing spawned and a single
//    failed end event.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_008WaveStartFailureFailsTheRun,
	"UEMMO.Tasks.M2_008.WaveStartFailureFailsTheRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_008WaveStartFailureFailsTheRun::RunTest(const FString& Parameters)
{
	FM2_008_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	URoomDefinition* Room = M2_008_LoadTrainingRoom(*this);
	if (Room == nullptr)
	{
		return true;
	}
	URoomDefinition* IllegalRoom = M2_008_MakeIllegalRoom();
	UEnemyDefinition* Def = M2_008_MakeEnemyDef(FName(TEXT("melee_grunt")));

	FM2_008_RunEvents Events;
	Events.Bind(*Scene.Session);
	if (!TestTrue(TEXT("the run starts with the legal room"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 RunId = Scene.Session->GetRunId();

	// A null definition is a request rejection BEFORE any wave exists: the
	// run stays Running and no failure is fabricated.
	TestFalse(TEXT("BeginWaves with a null room definition is rejected"), Scene.Session->BeginWaves(nullptr, Def));
	TestTrue(TEXT("the null-definition rejection left the run Running"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("the null-definition rejection started no wave"), Scene.Session->GetStartedWaveCount(), 0);

	// The illegal wave data is refused by the spawner: the card's failure path
	// fails the run (terminal, first-terminal-wins).
	TestFalse(TEXT("BeginWaves with an illegal wave is rejected"), Scene.Session->BeginWaves(IllegalRoom, Def));
	TestTrue(TEXT("the refused wave start failed the run"), Scene.Session->GetState() == ERoomSessionState::Failed);
	TestEqual(TEXT("the failure fired exactly one end event"), Events.EndedCount, 1);
	if (Events.EndedResults.Num() == 1)
	{
		TestTrue(TEXT("the end result records the failure"), !Events.EndedResults[0].bCleared);
		TestEqual(TEXT("the end result keeps the run id"), Events.EndedResults[0].RunId, RunId);
		TestEqual(TEXT("the failed run counted no kills"), Events.EndedResults[0].KilledCount, 0);
	}
	TestEqual(TEXT("the failed run started no wave"), Scene.Session->GetStartedWaveCount(), 0);
	TestEqual(TEXT("the failed run spawned no enemy"), Scene.Session->GetSpawnedEnemyCount(), 0);
	TestEqual(TEXT("the failed run left no enemy in the world"), M2_008_CountAliveEnemies(*Scene.World), 0);
	TestFalse(TEXT("BeginWaves after the failure is rejected (not Running)"), Scene.Session->BeginWaves(Room, Def));
	return true;
}

#endif
