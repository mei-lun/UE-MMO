// M5-004: weapon, ammo and projectile configuration structures (M5 interface
// contract section 1, first owner 004). Pins the three value-type headers
// Weapons/WeaponTypes.h, Weapons/AmmoTypes.h and Projectiles/ProjectileTypes.h:
// per-definition validation (mode vocabulary, ammo/projectile requirements per
// mode, bounded positive integers, non-negative physical quantities, the
// explosion/pierce mutual exclusion and the homing turn rate), the legacy
// four-attack melee set, and the four source tables under Data/CombatSystem
// (weapons.json, ammo_types.json, projectiles.json, target_filters.json with
// the first test_room manifest whose vehicle_spawns list starts empty).
//
// Pure checks only: this file reads JSON text and plain structs; it never
// touches UE assets, worlds or wall clocks. Cross-table validation with error
// codes and the production parser belong to M5-005/006; the small cross-file
// reference pass below only pins the coherence of this card's own sample
// data and is not a validator component.
//
// Parsing note (same lesson as M3-001): the engine's Json module headers
// compile through Engine's public dependency, but UnrealBuildTool does not put
// the Json import library on the UEMMO link line, so FJsonValue/FJsonObject
// symbols fail to link (LNK2019). UEMMO.Build.cs is outside this task's file
// range, so the tests carry the small hand-rolled parser below (objects,
// arrays, strings with escapes, numbers, booleans, null) built on Core types
// only; it is the same subset the M3-001 item tests use, duplicated per file
// because the M3 parser lives in that translation unit.

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"

#include "../Weapons/WeaponTypes.h"
#include "../Weapons/AmmoTypes.h"
#include "../Projectiles/ProjectileTypes.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_004
{
	/** Minimal JSON value: just enough shape for the combat config tables. */
	class FConfigJsonValue
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
		TArray<TSharedPtr<FConfigJsonValue>> Array;
		TMap<FString, TSharedPtr<FConfigJsonValue>> Object;
	};

	/** Recursive descent JSON parser (RFC 8259 subset; \u escapes decode to one TCHAR). */
	class FConfigJsonParser
	{
	public:
		static bool Parse(const FString& Text, TSharedPtr<FConfigJsonValue>& OutRoot, FString& OutError)
		{
			FConfigJsonParser Parser(Text);
			Parser.SkipWhitespace();
			TSharedPtr<FConfigJsonValue> Root = Parser.ParseValue(OutError);
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
		explicit FConfigJsonParser(const FString& InText)
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

		TSharedPtr<FConfigJsonValue> ParseValue(FString& OutError)
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

		TSharedPtr<FConfigJsonValue> ParseObject(FString& OutError)
		{
			if (!Expect(TEXT('{'), TEXT("to start an object"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FConfigJsonValue> Value = MakeShared<FConfigJsonValue>();
			Value->Kind = FConfigJsonValue::EKind::Object;
			SkipWhitespace();
			if (Consume(TEXT('}')))
			{
				return Value;
			}
			while (true)
			{
				SkipWhitespace();
				TSharedPtr<FConfigJsonValue> Key = ParseString(OutError);
				if (!Key.IsValid())
				{
					return nullptr;
				}
				SkipWhitespace();
				if (!Expect(TEXT(':'), TEXT("between object key and value"), OutError))
				{
					return nullptr;
				}
				TSharedPtr<FConfigJsonValue> Element = ParseValue(OutError);
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

		TSharedPtr<FConfigJsonValue> ParseArray(FString& OutError)
		{
			if (!Expect(TEXT('['), TEXT("to start an array"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FConfigJsonValue> Value = MakeShared<FConfigJsonValue>();
			Value->Kind = FConfigJsonValue::EKind::Array;
			SkipWhitespace();
			if (Consume(TEXT(']')))
			{
				return Value;
			}
			while (true)
			{
				TSharedPtr<FConfigJsonValue> Element = ParseValue(OutError);
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

		TSharedPtr<FConfigJsonValue> ParseString(FString& OutError)
		{
			if (!Expect(TEXT('"'), TEXT("to start a string"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FConfigJsonValue> Value = MakeShared<FConfigJsonValue>();
			Value->Kind = FConfigJsonValue::EKind::String;
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

		TSharedPtr<FConfigJsonValue> ParseBoolean(FString& OutError)
		{
			const TSharedPtr<FConfigJsonValue> Value = MakeShared<FConfigJsonValue>();
			if (MatchLiteral(TEXT("true"), 4))
			{
				Value->Kind = FConfigJsonValue::EKind::Boolean;
				Value->Boolean = true;
				Position += 4;
				return Value;
			}
			if (MatchLiteral(TEXT("false"), 5))
			{
				Value->Kind = FConfigJsonValue::EKind::Boolean;
				Value->Boolean = false;
				Position += 5;
				return Value;
			}
			OutError = FString::Printf(TEXT("invalid literal at offset %d"), Position);
			return nullptr;
		}

		TSharedPtr<FConfigJsonValue> ParseNull(FString& OutError)
		{
			if (MatchLiteral(TEXT("null"), 4))
			{
				const TSharedPtr<FConfigJsonValue> Value = MakeShared<FConfigJsonValue>();
				Value->Kind = FConfigJsonValue::EKind::Null;
				Position += 4;
				return Value;
			}
			OutError = FString::Printf(TEXT("invalid literal at offset %d"), Position);
			return nullptr;
		}

		TSharedPtr<FConfigJsonValue> ParseNumber(FString& OutError)
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
			const TSharedPtr<FConfigJsonValue> Value = MakeShared<FConfigJsonValue>();
			Value->Kind = FConfigJsonValue::EKind::Number;
			Value->Number = FCString::Atod(*Source.Mid(Start, Position - Start));
			return Value;
		}
	};

	// ------------------------------------------------------------------
	// JSON file and field helpers
	// ------------------------------------------------------------------

	static FString GetCombatSystemJsonPath(const TCHAR* FileName)
	{
		return FPaths::ProjectDir() / TEXT("Data") / TEXT("CombatSystem") / FileName;
	}

	static bool LoadJsonRoot(const FString& Path, FString& OutError, TSharedPtr<FConfigJsonValue>& OutRoot)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			OutError = FString::Printf(TEXT("could not read the JSON data file at %s"), *Path);
			return false;
		}
		TSharedPtr<FConfigJsonValue> Root;
		if (!FConfigJsonParser::Parse(Text, Root, OutError))
		{
			OutError = FString::Printf(TEXT("could not parse JSON in %s: %s"), *Path, *OutError);
			return false;
		}
		OutRoot = Root;
		return true;
	}

	/** Returns the field only when present with exactly the requested JSON kind. */
	static const TSharedPtr<FConfigJsonValue>* FindTyped(const TSharedPtr<FConfigJsonValue>& Object, const TCHAR* Name,
		FConfigJsonValue::EKind Kind)
	{
		const TSharedPtr<FConfigJsonValue>* Value = Object->Object.Find(Name);
		if (!Value || !Value->IsValid() || (*Value)->Kind != Kind)
		{
			return nullptr;
		}
		return Value;
	}

	static FString EntryName(const TSharedPtr<FConfigJsonValue>& Entry, const TCHAR* IdFieldName)
	{
		const TSharedPtr<FConfigJsonValue>* Id = FindTyped(Entry, IdFieldName, FConfigJsonValue::EKind::String);
		return (Id && !(*Id)->String.IsEmpty()) ? (*Id)->String : FString(TEXT("<unnamed entry>"));
	}

	/** Fails the test when the object carries a key outside the known set (strict schema). */
	static bool CheckOnlyKnownFields(FAutomationTestBase& Test, const TSharedPtr<FConfigJsonValue>& Object,
		const TCHAR* const* KnownNames, int32 KnownCount, const TCHAR* Context)
	{
		bool bClean = true;
		for (const TPair<FString, TSharedPtr<FConfigJsonValue>>& Field : Object->Object)
		{
			bool bKnown = false;
			for (int32 Index = 0; Index < KnownCount; ++Index)
			{
				if (Field.Key.Equals(KnownNames[Index]))
				{
					bKnown = true;
					break;
				}
			}
			if (!bKnown)
			{
				Test.AddError(FString::Printf(TEXT("%s: unknown field '%s'"), Context, *Field.Key));
				bClean = false;
			}
		}
		return bClean;
	}

	static bool GetStringOrReport(FAutomationTestBase& Test, const TSharedPtr<FConfigJsonValue>& Object,
		const TCHAR* Name, const TCHAR* Context, FString& OutValue)
	{
		const TSharedPtr<FConfigJsonValue>* Value = FindTyped(Object, Name, FConfigJsonValue::EKind::String);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("%s: missing or mistyped string field '%s'"), Context, Name));
			return false;
		}
		OutValue = (*Value)->String;
		return true;
	}

	static bool GetNumberOrReport(FAutomationTestBase& Test, const TSharedPtr<FConfigJsonValue>& Object,
		const TCHAR* Name, const TCHAR* Context, double& OutValue)
	{
		const TSharedPtr<FConfigJsonValue>* Value = FindTyped(Object, Name, FConfigJsonValue::EKind::Number);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("%s: missing or mistyped number field '%s'"), Context, Name));
			return false;
		}
		OutValue = (*Value)->Number;
		return true;
	}

	/** Reads an integral number field; fractional or out-of-range values are reported. */
	static bool GetIntOrReport(FAutomationTestBase& Test, const TSharedPtr<FConfigJsonValue>& Object,
		const TCHAR* Name, const TCHAR* Context, int32& OutValue)
	{
		double Number = 0.0;
		if (!GetNumberOrReport(Test, Object, Name, Context, Number))
		{
			return false;
		}
		const double Rounded = FMath::RoundToZero(Number);
		if (FMath::Abs(Number - Rounded) > 1e-9 || FMath::Abs(Rounded) > 2147483647.0)
		{
			Test.AddError(FString::Printf(TEXT("%s: field '%s' must be an integer (got %s)"),
				Context, Name, *FString::SanitizeFloat(Number)));
			return false;
		}
		OutValue = static_cast<int32>(Rounded);
		return true;
	}

	static bool GetBoolOrReport(FAutomationTestBase& Test, const TSharedPtr<FConfigJsonValue>& Object,
		const TCHAR* Name, const TCHAR* Context, bool& OutValue)
	{
		const TSharedPtr<FConfigJsonValue>* Value = FindTyped(Object, Name, FConfigJsonValue::EKind::Boolean);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("%s: missing or mistyped boolean field '%s'"), Context, Name));
			return false;
		}
		OutValue = (*Value)->Boolean;
		return true;
	}

	static bool GetStringArrayOrReport(FAutomationTestBase& Test, const TSharedPtr<FConfigJsonValue>& Object,
		const TCHAR* Name, const TCHAR* Context, TArray<FString>& OutValues)
	{
		OutValues.Reset();
		const TSharedPtr<FConfigJsonValue>* Value = FindTyped(Object, Name, FConfigJsonValue::EKind::Array);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("%s: missing or mistyped array field '%s'"), Context, Name));
			return false;
		}
		bool bClean = true;
		for (int32 Index = 0; Index < (*Value)->Array.Num(); ++Index)
		{
			const TSharedPtr<FConfigJsonValue>& Element = (*Value)->Array[Index];
			if (!Element.IsValid() || Element->Kind != FConfigJsonValue::EKind::String)
			{
				Test.AddError(FString::Printf(TEXT("%s: array field '%s' element %d is not a string"),
					Context, Name, Index));
				bClean = false;
				continue;
			}
			OutValues.Add(Element->String);
		}
		return bClean;
	}

	/** Comparison tolerance for doubles that both sides derive from decimal literals. */
	static bool NearlyEqual(double A, double B)
	{
		return FMath::Abs(A - B) <= 1e-9;
	}

	// ------------------------------------------------------------------
	// In-memory builders (independent of the JSON files)
	// ------------------------------------------------------------------

	/** Builds the valid melee sample: the legacy four-attack training sword. */
	static FWeaponDefinition MakeMeleeWeaponDefinition()
	{
		FWeaponDefinition Definition;
		Definition.WeaponId = TEXT("weapon_training_sword");
		Definition.FireMode = EWeaponFireMode::Melee;
		Definition.DamageProfileId = TEXT("physical_10");
		Definition.MeleeAttackIds = GetLegacyMeleeAttackIds();
		return Definition;
	}

	/** Builds the valid hitscan sample: a single-pellet ray weapon. */
	static FWeaponDefinition MakeHitscanWeaponDefinition()
	{
		FWeaponDefinition Definition;
		Definition.WeaponId = TEXT("weapon_hitscan_sample");
		Definition.FireMode = EWeaponFireMode::Hitscan;
		Definition.DamageProfileId = TEXT("physical_10");
		Definition.AmmoId = TEXT("ammo_cell");
		Definition.MagazineSize = 12;
		Definition.FireRateRpm = 240.0f;
		Definition.BurstCount = 1;
		Definition.PelletCount = 1;
		Definition.SpreadDegrees = 1.0f;
		Definition.RangeCm = 5000.0f;
		return Definition;
	}

	/** Builds the valid projectile sample: a single-bullet linear weapon. */
	static FWeaponDefinition MakeProjectileWeaponDefinition()
	{
		FWeaponDefinition Definition;
		Definition.WeaponId = TEXT("weapon_projectile_sample");
		Definition.FireMode = EWeaponFireMode::Projectile;
		Definition.AmmoId = TEXT("ammo_light");
		Definition.MagazineSize = 12;
		Definition.FireRateRpm = 300.0f;
		Definition.BurstCount = 1;
		Definition.PelletCount = 1;
		Definition.ProjectileId = TEXT("bullet_linear");
		return Definition;
	}

	/** Builds a valid ammo type with the sample cell values. */
	static FAmmoType MakeValidAmmoType()
	{
		FAmmoType Ammo;
		Ammo.AmmoId = TEXT("ammo_cell");
		Ammo.MaxReserve = 120;
		Ammo.MagazineSize = 12;
		return Ammo;
	}

	/** Builds the valid straight bullet. */
	static FProjectileDefinition MakeStraightProjectileDefinition()
	{
		FProjectileDefinition Definition;
		Definition.ProjectileId = TEXT("bullet_linear");
		Definition.Motion = EProjectileMotion::Straight;
		Definition.SpeedCmS = 6000.0f;
		Definition.LifetimeS = 2.0f;
		Definition.DamageProfileId = TEXT("physical_10");
		Definition.PierceCount = 0;
		Definition.ExplosionRadiusCm = 0.0f;
		Definition.HomingTurnRateDegS = 0.0f;
		return Definition;
	}

	/** Builds the valid homing missile (turn rate > 0). */
	static FProjectileDefinition MakeHomingProjectileDefinition()
	{
		FProjectileDefinition Definition = MakeStraightProjectileDefinition();
		Definition.ProjectileId = TEXT("missile_homing");
		Definition.Motion = EProjectileMotion::Homing;
		Definition.SpeedCmS = 2500.0f;
		Definition.LifetimeS = 5.0f;
		Definition.HomingTurnRateDegS = 180.0f;
		return Definition;
	}

	/** Builds the valid explosive rocket (radius > 0 with its own profile, no pierce). */
	static FProjectileDefinition MakeExplosiveProjectileDefinition()
	{
		FProjectileDefinition Definition = MakeStraightProjectileDefinition();
		Definition.ProjectileId = TEXT("rocket_explosive");
		Definition.SpeedCmS = 4000.0f;
		Definition.LifetimeS = 4.0f;
		Definition.ExplosionRadiusCm = 300.0f;
		Definition.ExplosionDamageProfileId = TEXT("explosive_40");
		return Definition;
	}
}

using namespace UE::UEMMO::Tasks::M5_004;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_004WeaponDefinitionValidation,
	"UEMMO.Tasks.M5_004.WeaponDefinitionValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_004WeaponDefinitionValidation::RunTest(const FString& Parameters)
{
	// Every rejected mutation must fail validation and name the offending field.
	FString Errors;

	// The three mode samples validate in isolation.
	TestTrue(TEXT("the melee sample passes ValidateWeaponDefinition"),
		ValidateWeaponDefinition(MakeMeleeWeaponDefinition(), Errors));
	TestTrue(TEXT("the melee sample reports no validation errors"), Errors.IsEmpty());
	TestTrue(TEXT("the hitscan sample passes ValidateWeaponDefinition"),
		ValidateWeaponDefinition(MakeHitscanWeaponDefinition(), Errors));
	TestTrue(TEXT("the hitscan sample reports no validation errors"), Errors.IsEmpty());
	TestTrue(TEXT("the projectile sample passes ValidateWeaponDefinition"),
		ValidateWeaponDefinition(MakeProjectileWeaponDefinition(), Errors));
	TestTrue(TEXT("the projectile sample reports no validation errors"), Errors.IsEmpty());

	// Mode vocabulary: exactly melee/hitscan/projectile, case-insensitive text,
	// unknown text and out-of-enum values rejected.
	EWeaponFireMode Parsed = EWeaponFireMode::Melee;
	TestTrue(TEXT("ParseWeaponFireMode accepts 'melee'"), ParseWeaponFireMode(TEXT("melee"), Parsed) && Parsed == EWeaponFireMode::Melee);
	TestTrue(TEXT("ParseWeaponFireMode accepts 'HITSCAN' case-insensitively"),
		ParseWeaponFireMode(TEXT("HITSCAN"), Parsed) && Parsed == EWeaponFireMode::Hitscan);
	TestTrue(TEXT("ParseWeaponFireMode accepts 'Projectile'"), ParseWeaponFireMode(TEXT("Projectile"), Parsed) && Parsed == EWeaponFireMode::Projectile);
	TestFalse(TEXT("ParseWeaponFireMode rejects the empty text"), ParseWeaponFireMode(TEXT(""), Parsed));
	TestFalse(TEXT("ParseWeaponFireMode rejects 'automatic' (a pacing word, not a delivery mode)"),
		ParseWeaponFireMode(TEXT("automatic"), Parsed));
	TestFalse(TEXT("ParseWeaponFireMode rejects 'laser'"), ParseWeaponFireMode(TEXT("laser"), Parsed));
	{
		FWeaponDefinition UnknownMode = MakeHitscanWeaponDefinition();
		UnknownMode.FireMode = static_cast<EWeaponFireMode>(200);
		TestFalse(TEXT("an out-of-enum FireMode is rejected"), ValidateWeaponDefinition(UnknownMode, Errors));
		TestTrue(TEXT("the out-of-enum FireMode error names FireMode"), Errors.Contains(TEXT("FireMode")));
	}

	// Identity: lowercase a-z0-9_, 1-64 characters.
	{
		FWeaponDefinition NoId = MakeHitscanWeaponDefinition();
		NoId.WeaponId = NAME_None;
		TestFalse(TEXT("a weapon without WeaponId is rejected"), ValidateWeaponDefinition(NoId, Errors));
		TestTrue(TEXT("the missing WeaponId error names WeaponId"), Errors.Contains(TEXT("WeaponId")));

		FWeaponDefinition UppercaseId = MakeHitscanWeaponDefinition();
		UppercaseId.WeaponId = TEXT("Weapon_Hitscan");
		TestFalse(TEXT("an uppercase WeaponId is rejected"), ValidateWeaponDefinition(UppercaseId, Errors));
		TestTrue(TEXT("the uppercase WeaponId error names WeaponId"), Errors.Contains(TEXT("WeaponId")));

		FWeaponDefinition HyphenId = MakeHitscanWeaponDefinition();
		HyphenId.WeaponId = TEXT("weapon-hitscan");
		TestFalse(TEXT("a hyphenated WeaponId is rejected"), ValidateWeaponDefinition(HyphenId, Errors));

		FWeaponDefinition LongId = MakeHitscanWeaponDefinition();
		LongId.WeaponId = FName(*FString::ChrN(65, TEXT('a')));
		TestFalse(TEXT("a 65-character WeaponId is rejected"), ValidateWeaponDefinition(LongId, Errors));
	}

	// Melee: no ammo, no projectile, no firearm pacing or geometry fields; the
	// attack pacing and hit geometry live in the legacy attack definitions.
	{
		FWeaponDefinition WithAmmo = MakeMeleeWeaponDefinition();
		WithAmmo.AmmoId = TEXT("ammo_cell");
		TestFalse(TEXT("a melee weapon with an AmmoId is rejected"), ValidateWeaponDefinition(WithAmmo, Errors));
		TestTrue(TEXT("the melee-ammo error names AmmoId"), Errors.Contains(TEXT("AmmoId")));

		FWeaponDefinition WithProjectile = MakeMeleeWeaponDefinition();
		WithProjectile.ProjectileId = TEXT("bullet_linear");
		TestFalse(TEXT("a melee weapon with a ProjectileId is rejected"), ValidateWeaponDefinition(WithProjectile, Errors));
		TestTrue(TEXT("the melee-projectile error names ProjectileId"), Errors.Contains(TEXT("ProjectileId")));

		FWeaponDefinition WithMagazine = MakeMeleeWeaponDefinition();
		WithMagazine.MagazineSize = 8;
		TestFalse(TEXT("a melee weapon with a magazine is rejected"), ValidateWeaponDefinition(WithMagazine, Errors));
		TestTrue(TEXT("the melee-magazine error names MagazineSize"), Errors.Contains(TEXT("MagazineSize")));

		FWeaponDefinition WithFireRate = MakeMeleeWeaponDefinition();
		WithFireRate.FireRateRpm = 120.0f;
		TestFalse(TEXT("a melee weapon with a fire rate is rejected"), ValidateWeaponDefinition(WithFireRate, Errors));
		TestTrue(TEXT("the melee-fire-rate error names FireRateRpm"), Errors.Contains(TEXT("FireRateRpm")));

		FWeaponDefinition WithSpread = MakeMeleeWeaponDefinition();
		WithSpread.SpreadDegrees = 2.0f;
		TestFalse(TEXT("a melee weapon with spread is rejected"), ValidateWeaponDefinition(WithSpread, Errors));
		TestTrue(TEXT("the melee-spread error names SpreadDegrees"), Errors.Contains(TEXT("SpreadDegrees")));

		FWeaponDefinition WithRange = MakeMeleeWeaponDefinition();
		WithRange.RangeCm = 3000.0f;
		TestFalse(TEXT("a melee weapon with a weapon range is rejected"), ValidateWeaponDefinition(WithRange, Errors));
		TestTrue(TEXT("the melee-range error names RangeCm"), Errors.Contains(TEXT("RangeCm")));

		FWeaponDefinition WithBurst = MakeMeleeWeaponDefinition();
		WithBurst.BurstCount = 3;
		TestFalse(TEXT("a melee weapon with a burst count is rejected"), ValidateWeaponDefinition(WithBurst, Errors));
		TestTrue(TEXT("the melee-burst error names BurstCount"), Errors.Contains(TEXT("BurstCount")));

		FWeaponDefinition WithPellets = MakeMeleeWeaponDefinition();
		WithPellets.PelletCount = 2;
		TestFalse(TEXT("a melee weapon with pellet count is rejected"), ValidateWeaponDefinition(WithPellets, Errors));
		TestTrue(TEXT("the melee-pellet error names PelletCount"), Errors.Contains(TEXT("PelletCount")));
	}

	// Melee attack ids: only the complete legacy four-attack set is legal;
	// unknown, incomplete or duplicated sets are rejected, never ignored.
	{
		FWeaponDefinition Shortened = MakeMeleeWeaponDefinition();
		Shortened.MeleeAttackIds.RemoveAt(0);
		TestFalse(TEXT("an incomplete melee attack set (3 of 4) is rejected"),
			ValidateWeaponDefinition(Shortened, Errors));
		TestTrue(TEXT("the incomplete-set error names MeleeAttackIds"), Errors.Contains(TEXT("MeleeAttackIds")));

		FWeaponDefinition UnknownEntry = MakeMeleeWeaponDefinition();
		UnknownEntry.MeleeAttackIds[0] = TEXT("light_03");
		TestFalse(TEXT("an unknown melee attack id is rejected"), ValidateWeaponDefinition(UnknownEntry, Errors));
		TestTrue(TEXT("the unknown-attack error names MeleeAttackIds"), Errors.Contains(TEXT("MeleeAttackIds")));

		FWeaponDefinition ExtraEntry = MakeMeleeWeaponDefinition();
		ExtraEntry.MeleeAttackIds.Add(TEXT("light_01"));
		TestFalse(TEXT("a five-entry melee attack set is rejected"), ValidateWeaponDefinition(ExtraEntry, Errors));

		FWeaponDefinition Duplicated = MakeMeleeWeaponDefinition();
		Duplicated.MeleeAttackIds[1] = TEXT("light_01");
		TestFalse(TEXT("a duplicated melee attack set is rejected"), ValidateWeaponDefinition(Duplicated, Errors));

		FWeaponDefinition EmptySet = MakeMeleeWeaponDefinition();
		EmptySet.MeleeAttackIds.Reset();
		TestFalse(TEXT("an empty melee attack set is rejected"), ValidateWeaponDefinition(EmptySet, Errors));
	}

	// Ranged requires ammunition; the projectile delivery also requires a
	// projectile definition; hitscan never spawns an actor so it must not
	// reference one.
	{
		FWeaponDefinition NoAmmoHitscan = MakeHitscanWeaponDefinition();
		NoAmmoHitscan.AmmoId = NAME_None;
		TestFalse(TEXT("a hitscan weapon without ammo is rejected"), ValidateWeaponDefinition(NoAmmoHitscan, Errors));
		TestTrue(TEXT("the hitscan-ammo error names AmmoId"), Errors.Contains(TEXT("AmmoId")));

		FWeaponDefinition NoAmmoProjectile = MakeProjectileWeaponDefinition();
		NoAmmoProjectile.AmmoId = NAME_None;
		TestFalse(TEXT("a projectile weapon without ammo is rejected"), ValidateWeaponDefinition(NoAmmoProjectile, Errors));
		TestTrue(TEXT("the projectile-ammo error names AmmoId"), Errors.Contains(TEXT("AmmoId")));

		FWeaponDefinition NoProjectile = MakeProjectileWeaponDefinition();
		NoProjectile.ProjectileId = NAME_None;
		TestFalse(TEXT("a projectile weapon without a ProjectileId is rejected"),
			ValidateWeaponDefinition(NoProjectile, Errors));
		TestTrue(TEXT("the missing-projectile error names ProjectileId"), Errors.Contains(TEXT("ProjectileId")));

		FWeaponDefinition HitscanWithProjectile = MakeHitscanWeaponDefinition();
		HitscanWithProjectile.ProjectileId = TEXT("bullet_linear");
		TestFalse(TEXT("a hitscan weapon with a ProjectileId is rejected (no actor is spawned)"),
			ValidateWeaponDefinition(HitscanWithProjectile, Errors));
		TestTrue(TEXT("the hitscan-projectile error names ProjectileId"), Errors.Contains(TEXT("ProjectileId")));

		FWeaponDefinition ProjectileWithProfile = MakeProjectileWeaponDefinition();
		ProjectileWithProfile.DamageProfileId = TEXT("physical_10");
		TestFalse(TEXT("a projectile weapon carrying a weapon damage profile is rejected (the projectile owns damage)"),
			ValidateWeaponDefinition(ProjectileWithProfile, Errors));
		TestTrue(TEXT("the projectile-profile error names DamageProfileId"), Errors.Contains(TEXT("DamageProfileId")));

		FWeaponDefinition NoProfileHitscan = MakeHitscanWeaponDefinition();
		NoProfileHitscan.DamageProfileId = NAME_None;
		TestFalse(TEXT("a hitscan weapon without a damage profile is rejected"),
			ValidateWeaponDefinition(NoProfileHitscan, Errors));
		TestTrue(TEXT("the hitscan-profile error names DamageProfileId"), Errors.Contains(TEXT("DamageProfileId")));

		FWeaponDefinition NoProfileMelee = MakeMeleeWeaponDefinition();
		NoProfileMelee.DamageProfileId = NAME_None;
		TestFalse(TEXT("a melee weapon without a damage profile is rejected"),
			ValidateWeaponDefinition(NoProfileMelee, Errors));

		FWeaponDefinition RangedWithAttacks = MakeHitscanWeaponDefinition();
		RangedWithAttacks.MeleeAttackIds = GetLegacyMeleeAttackIds();
		TestFalse(TEXT("a ranged weapon listing melee attack ids is rejected"),
			ValidateWeaponDefinition(RangedWithAttacks, Errors));
		TestTrue(TEXT("the ranged-attacks error names MeleeAttackIds"), Errors.Contains(TEXT("MeleeAttackIds")));
	}

	// Negative physical quantities are rejected in every mode.
	{
		FWeaponDefinition NegativeMagazine = MakeHitscanWeaponDefinition();
		NegativeMagazine.MagazineSize = -1;
		TestFalse(TEXT("a negative magazine size is rejected"), ValidateWeaponDefinition(NegativeMagazine, Errors));
		TestTrue(TEXT("the negative-magazine error names MagazineSize"), Errors.Contains(TEXT("MagazineSize")));

		FWeaponDefinition NegativeFireRate = MakeHitscanWeaponDefinition();
		NegativeFireRate.FireRateRpm = -1.0f;
		TestFalse(TEXT("a negative fire rate is rejected"), ValidateWeaponDefinition(NegativeFireRate, Errors));
		TestTrue(TEXT("the negative-fire-rate error names FireRateRpm"), Errors.Contains(TEXT("FireRateRpm")));

		FWeaponDefinition NegativeSpread = MakeHitscanWeaponDefinition();
		NegativeSpread.SpreadDegrees = -0.5f;
		TestFalse(TEXT("a negative spread is rejected"), ValidateWeaponDefinition(NegativeSpread, Errors));
		TestTrue(TEXT("the negative-spread error names SpreadDegrees"), Errors.Contains(TEXT("SpreadDegrees")));

		FWeaponDefinition NegativeRange = MakeHitscanWeaponDefinition();
		NegativeRange.RangeCm = -1.0f;
		TestFalse(TEXT("a negative range is rejected"), ValidateWeaponDefinition(NegativeRange, Errors));
		TestTrue(TEXT("the negative-range error names RangeCm"), Errors.Contains(TEXT("RangeCm")));
	}

	// A ranged weapon must actually fire: a positive fire rate and a positive
	// magazine are required.
	{
		FWeaponDefinition ZeroFireRate = MakeHitscanWeaponDefinition();
		ZeroFireRate.FireRateRpm = 0.0f;
		TestFalse(TEXT("a ranged weapon with a zero fire rate is rejected"), ValidateWeaponDefinition(ZeroFireRate, Errors));

		FWeaponDefinition ZeroMagazine = MakeHitscanWeaponDefinition();
		ZeroMagazine.MagazineSize = 0;
		TestFalse(TEXT("a ranged weapon with a zero magazine is rejected"), ValidateWeaponDefinition(ZeroMagazine, Errors));

		FWeaponDefinition ZeroRange = MakeHitscanWeaponDefinition();
		ZeroRange.RangeCm = 0.0f;
		TestFalse(TEXT("a hitscan weapon with a zero range is rejected"), ValidateWeaponDefinition(ZeroRange, Errors));

		FWeaponDefinition ProjectileWithRange = MakeProjectileWeaponDefinition();
		ProjectileWithRange.RangeCm = 3000.0f;
		TestFalse(TEXT("a projectile weapon carrying a weapon range is rejected (the projectile owns range)"),
			ValidateWeaponDefinition(ProjectileWithRange, Errors));
		TestTrue(TEXT("the projectile-range error names RangeCm"), Errors.Contains(TEXT("RangeCm")));
	}

	// pellet_count and burst_count are bounded positive integers.
	{
		FWeaponDefinition ZeroPellets = MakeHitscanWeaponDefinition();
		ZeroPellets.PelletCount = 0;
		TestFalse(TEXT("a zero pellet count is rejected"), ValidateWeaponDefinition(ZeroPellets, Errors));
		TestTrue(TEXT("the zero-pellet error names PelletCount"), Errors.Contains(TEXT("PelletCount")));

		FWeaponDefinition TooManyPellets = MakeHitscanWeaponDefinition();
		TooManyPellets.PelletCount = MaxWeaponPelletCount + 1;
		TestFalse(TEXT("a pellet count above the bound is rejected"), ValidateWeaponDefinition(TooManyPellets, Errors));
		TestTrue(TEXT("the bound-pellet error names PelletCount"), Errors.Contains(TEXT("PelletCount")));

		FWeaponDefinition BoundaryPellets = MakeHitscanWeaponDefinition();
		BoundaryPellets.PelletCount = MaxWeaponPelletCount;
		TestTrue(TEXT("the pellet bound itself is accepted"), ValidateWeaponDefinition(BoundaryPellets, Errors));

		FWeaponDefinition ZeroBurst = MakeHitscanWeaponDefinition();
		ZeroBurst.BurstCount = 0;
		TestFalse(TEXT("a zero burst count is rejected"), ValidateWeaponDefinition(ZeroBurst, Errors));
		TestTrue(TEXT("the zero-burst error names BurstCount"), Errors.Contains(TEXT("BurstCount")));

		FWeaponDefinition TooManyBurst = MakeHitscanWeaponDefinition();
		TooManyBurst.BurstCount = MaxWeaponBurstCount + 1;
		TestFalse(TEXT("a burst count above the bound is rejected"), ValidateWeaponDefinition(TooManyBurst, Errors));

		FWeaponDefinition BoundaryBurst = MakeHitscanWeaponDefinition();
		BoundaryBurst.BurstCount = MaxWeaponBurstCount;
		TestTrue(TEXT("the burst bound itself is accepted"), ValidateWeaponDefinition(BoundaryBurst, Errors));

		FWeaponDefinition TooManyMagazine = MakeHitscanWeaponDefinition();
		TooManyMagazine.MagazineSize = MaxWeaponMagazineSize + 1;
		TestFalse(TEXT("a magazine size above the bound is rejected"), ValidateWeaponDefinition(TooManyMagazine, Errors));

		FWeaponDefinition TooFast = MakeHitscanWeaponDefinition();
		TooFast.FireRateRpm = MaxWeaponFireRateRpm + 1.0f;
		TestFalse(TEXT("a fire rate above the bound is rejected"), ValidateWeaponDefinition(TooFast, Errors));
		TestTrue(TEXT("the bound-fire-rate error names FireRateRpm"), Errors.Contains(TEXT("FireRateRpm")));

		FWeaponDefinition BoundaryFireRate = MakeHitscanWeaponDefinition();
		BoundaryFireRate.FireRateRpm = MaxWeaponFireRateRpm;
		TestTrue(TEXT("the fire-rate bound itself is accepted"), ValidateWeaponDefinition(BoundaryFireRate, Errors));

		FWeaponDefinition TooWideSpread = MakeHitscanWeaponDefinition();
		TooWideSpread.SpreadDegrees = MaxWeaponSpreadDegrees + 0.1f;
		TestFalse(TEXT("a spread above the bound is rejected"), ValidateWeaponDefinition(TooWideSpread, Errors));
		TestTrue(TEXT("the bound-spread error names SpreadDegrees"), Errors.Contains(TEXT("SpreadDegrees")));

		FWeaponDefinition BoundarySpread = MakeHitscanWeaponDefinition();
		BoundarySpread.SpreadDegrees = MaxWeaponSpreadDegrees;
		TestTrue(TEXT("the spread bound itself is accepted"), ValidateWeaponDefinition(BoundarySpread, Errors));

		FWeaponDefinition TooLongRange = MakeHitscanWeaponDefinition();
		TooLongRange.RangeCm = MaxWeaponRangeCm + 1.0f;
		TestFalse(TEXT("a range above the bound is rejected"), ValidateWeaponDefinition(TooLongRange, Errors));

		FWeaponDefinition NonFiniteSpread = MakeHitscanWeaponDefinition();
		NonFiniteSpread.SpreadDegrees = std::numeric_limits<float>::quiet_NaN();
		TestFalse(TEXT("a non-finite spread is rejected"), ValidateWeaponDefinition(NonFiniteSpread, Errors));
		TestTrue(TEXT("the non-finite-spread error names SpreadDegrees"), Errors.Contains(TEXT("SpreadDegrees")));
	}

	// Several problems at once are collected and joined with "; ".
	{
		FWeaponDefinition Broken = MakeHitscanWeaponDefinition();
		Broken.WeaponId = NAME_None;
		Broken.AmmoId = NAME_None;
		Broken.MagazineSize = -1;
		Broken.BurstCount = 0;
		TestFalse(TEXT("a weapon with multiple problems is rejected"), ValidateWeaponDefinition(Broken, Errors));
		TestTrue(TEXT("WeaponId problem is included"), Errors.Contains(TEXT("WeaponId")));
		TestTrue(TEXT("AmmoId problem is included"), Errors.Contains(TEXT("AmmoId")));
		TestTrue(TEXT("MagazineSize problem is included"), Errors.Contains(TEXT("MagazineSize")));
		TestTrue(TEXT("BurstCount problem is included"), Errors.Contains(TEXT("BurstCount")));
		TestTrue(TEXT("problems are joined with \"; \""), Errors.Contains(TEXT("; ")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_004AmmoTypeValidation,
	"UEMMO.Tasks.M5_004.AmmoTypeValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_004AmmoTypeValidation::RunTest(const FString& Parameters)
{
	// The sample ammo type validates; every rejected mutation names its field.
	FString Errors;
	TestTrue(TEXT("the sample ammo type passes ValidateAmmoType"), ValidateAmmoType(MakeValidAmmoType(), Errors));
	TestTrue(TEXT("the sample ammo type reports no validation errors"), Errors.IsEmpty());

	{
		FAmmoType NoId = MakeValidAmmoType();
		NoId.AmmoId = NAME_None;
		TestFalse(TEXT("an ammo type without AmmoId is rejected"), ValidateAmmoType(NoId, Errors));
		TestTrue(TEXT("the missing AmmoId error names AmmoId"), Errors.Contains(TEXT("AmmoId")));
	}
	{
		FAmmoType UppercaseId = MakeValidAmmoType();
		UppercaseId.AmmoId = TEXT("AMMO_CELL");
		TestFalse(TEXT("an uppercase AmmoId is rejected"), ValidateAmmoType(UppercaseId, Errors));
		TestTrue(TEXT("the uppercase AmmoId error names AmmoId"), Errors.Contains(TEXT("AmmoId")));
	}
	{
		FAmmoType HyphenId = MakeValidAmmoType();
		HyphenId.AmmoId = TEXT("ammo-cell");
		TestFalse(TEXT("a hyphenated AmmoId is rejected"), ValidateAmmoType(HyphenId, Errors));
	}
	{
		FAmmoType LongId = MakeValidAmmoType();
		LongId.AmmoId = FName(*FString::ChrN(65, TEXT('a')));
		TestFalse(TEXT("a 65-character AmmoId is rejected"), ValidateAmmoType(LongId, Errors));
	}
	{
		FAmmoType ZeroMagazine = MakeValidAmmoType();
		ZeroMagazine.MagazineSize = 0;
		TestFalse(TEXT("a zero ammo magazine size is rejected"), ValidateAmmoType(ZeroMagazine, Errors));
		TestTrue(TEXT("the zero-magazine error names MagazineSize"), Errors.Contains(TEXT("MagazineSize")));

		FAmmoType NegativeMagazine = MakeValidAmmoType();
		NegativeMagazine.MagazineSize = -4;
		TestFalse(TEXT("a negative ammo magazine size is rejected"), ValidateAmmoType(NegativeMagazine, Errors));

		FAmmoType TooLargeMagazine = MakeValidAmmoType();
		TooLargeMagazine.MagazineSize = MaxAmmoMagazineSize + 1;
		TestFalse(TEXT("an ammo magazine size above the bound is rejected"), ValidateAmmoType(TooLargeMagazine, Errors));

		FAmmoType BoundaryMagazine = MakeValidAmmoType();
		BoundaryMagazine.MagazineSize = MaxAmmoMagazineSize;
		TestTrue(TEXT("the ammo magazine bound itself is accepted"), ValidateAmmoType(BoundaryMagazine, Errors));
	}
	{
		FAmmoType NegativeReserve = MakeValidAmmoType();
		NegativeReserve.MaxReserve = -1;
		TestFalse(TEXT("a negative max reserve is rejected"), ValidateAmmoType(NegativeReserve, Errors));
		TestTrue(TEXT("the negative-reserve error names MaxReserve"), Errors.Contains(TEXT("MaxReserve")));

		FAmmoType TooLargeReserve = MakeValidAmmoType();
		TooLargeReserve.MaxReserve = MaxAmmoReserve + 1;
		TestFalse(TEXT("a max reserve above the bound is rejected"), ValidateAmmoType(TooLargeReserve, Errors));

		FAmmoType BoundaryReserve = MakeValidAmmoType();
		BoundaryReserve.MaxReserve = MaxAmmoReserve;
		TestTrue(TEXT("the max-reserve bound itself is accepted"), ValidateAmmoType(BoundaryReserve, Errors));
	}
	{
		// Multiple problems are collected and joined with "; ".
		FAmmoType Broken;
		Broken.AmmoId = TEXT("Ammo.Broken");
		Broken.MaxReserve = -1;
		Broken.MagazineSize = 0;
		TestFalse(TEXT("an ammo type with multiple problems is rejected"), ValidateAmmoType(Broken, Errors));
		TestTrue(TEXT("AmmoId problem is included"), Errors.Contains(TEXT("AmmoId")));
		TestTrue(TEXT("MaxReserve problem is included"), Errors.Contains(TEXT("MaxReserve")));
		TestTrue(TEXT("MagazineSize problem is included"), Errors.Contains(TEXT("MagazineSize")));
		TestTrue(TEXT("problems are joined with \"; \""), Errors.Contains(TEXT("; ")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_004ProjectileDefinitionValidation,
	"UEMMO.Tasks.M5_004.ProjectileDefinitionValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_004ProjectileDefinitionValidation::RunTest(const FString& Parameters)
{
	// The three motion samples validate; every rejected mutation names its field.
	FString Errors;
	TestTrue(TEXT("the straight bullet passes ValidateProjectileDefinition"),
		ValidateProjectileDefinition(MakeStraightProjectileDefinition(), Errors));
	TestTrue(TEXT("the straight bullet reports no validation errors"), Errors.IsEmpty());
	TestTrue(TEXT("the homing missile passes ValidateProjectileDefinition"),
		ValidateProjectileDefinition(MakeHomingProjectileDefinition(), Errors));
	TestTrue(TEXT("the explosive rocket passes ValidateProjectileDefinition"),
		ValidateProjectileDefinition(MakeExplosiveProjectileDefinition(), Errors));
	TestTrue(TEXT("the explosive rocket reports no validation errors"), Errors.IsEmpty());

	// Motion vocabulary: exactly straight/parabolic/homing, case-insensitive
	// text; unknown text and out-of-enum values rejected.
	{
		EProjectileMotion Parsed = EProjectileMotion::Straight;
		TestTrue(TEXT("ParseProjectileMotion accepts 'straight'"),
			ParseProjectileMotion(TEXT("straight"), Parsed) && Parsed == EProjectileMotion::Straight);
		TestTrue(TEXT("ParseProjectileMotion accepts 'PARABOLIC' case-insensitively"),
			ParseProjectileMotion(TEXT("PARABOLIC"), Parsed) && Parsed == EProjectileMotion::Parabolic);
		TestTrue(TEXT("ParseProjectileMotion accepts 'Homing'"),
			ParseProjectileMotion(TEXT("Homing"), Parsed) && Parsed == EProjectileMotion::Homing);
		TestFalse(TEXT("ParseProjectileMotion rejects the empty text"), ParseProjectileMotion(TEXT(""), Parsed));
		TestFalse(TEXT("ParseProjectileMotion rejects 'bounce' (no bouncing trajectories in the first version)"),
			ParseProjectileMotion(TEXT("bounce"), Parsed));

		FProjectileDefinition UnknownMotion = MakeStraightProjectileDefinition();
		UnknownMotion.Motion = static_cast<EProjectileMotion>(99);
		TestFalse(TEXT("an out-of-enum Motion is rejected"), ValidateProjectileDefinition(UnknownMotion, Errors));
		TestTrue(TEXT("the out-of-enum Motion error names Motion"), Errors.Contains(TEXT("Motion")));
	}

	// Identity.
	{
		FProjectileDefinition NoId = MakeStraightProjectileDefinition();
		NoId.ProjectileId = NAME_None;
		TestFalse(TEXT("a projectile without ProjectileId is rejected"), ValidateProjectileDefinition(NoId, Errors));
		TestTrue(TEXT("the missing ProjectileId error names ProjectileId"), Errors.Contains(TEXT("ProjectileId")));

		FProjectileDefinition UppercaseId = MakeStraightProjectileDefinition();
		UppercaseId.ProjectileId = TEXT("Bullet_Linear");
		TestFalse(TEXT("an uppercase ProjectileId is rejected"), ValidateProjectileDefinition(UppercaseId, Errors));
	}

	// Speed and lifetime: finite, strictly positive, bounded.
	{
		FProjectileDefinition NegativeSpeed = MakeStraightProjectileDefinition();
		NegativeSpeed.SpeedCmS = -1.0f;
		TestFalse(TEXT("a negative speed is rejected"), ValidateProjectileDefinition(NegativeSpeed, Errors));
		TestTrue(TEXT("the negative-speed error names SpeedCmS"), Errors.Contains(TEXT("SpeedCmS")));

		FProjectileDefinition ZeroSpeed = MakeStraightProjectileDefinition();
		ZeroSpeed.SpeedCmS = 0.0f;
		TestFalse(TEXT("a zero speed is rejected"), ValidateProjectileDefinition(ZeroSpeed, Errors));

		FProjectileDefinition TooFast = MakeStraightProjectileDefinition();
		TooFast.SpeedCmS = MaxProjectileSpeedCmS + 1.0f;
		TestFalse(TEXT("a speed above the bound is rejected"), ValidateProjectileDefinition(TooFast, Errors));

		FProjectileDefinition BoundarySpeed = MakeStraightProjectileDefinition();
		BoundarySpeed.SpeedCmS = MaxProjectileSpeedCmS;
		TestTrue(TEXT("the speed bound itself is accepted"), ValidateProjectileDefinition(BoundarySpeed, Errors));

		FProjectileDefinition NonFiniteSpeed = MakeStraightProjectileDefinition();
		NonFiniteSpeed.SpeedCmS = std::numeric_limits<float>::infinity();
		TestFalse(TEXT("a non-finite speed is rejected"), ValidateProjectileDefinition(NonFiniteSpeed, Errors));
		TestTrue(TEXT("the non-finite-speed error names SpeedCmS"), Errors.Contains(TEXT("SpeedCmS")));

		FProjectileDefinition NegativeLifetime = MakeStraightProjectileDefinition();
		NegativeLifetime.LifetimeS = -1.0f;
		TestFalse(TEXT("a negative lifetime is rejected"), ValidateProjectileDefinition(NegativeLifetime, Errors));
		TestTrue(TEXT("the negative-lifetime error names LifetimeS"), Errors.Contains(TEXT("LifetimeS")));

		FProjectileDefinition ZeroLifetime = MakeStraightProjectileDefinition();
		ZeroLifetime.LifetimeS = 0.0f;
		TestFalse(TEXT("a zero lifetime is rejected"), ValidateProjectileDefinition(ZeroLifetime, Errors));

		FProjectileDefinition TooLongLifetime = MakeStraightProjectileDefinition();
		TooLongLifetime.LifetimeS = MaxProjectileLifetimeS + 1.0f;
		TestFalse(TEXT("a lifetime above the bound is rejected"), ValidateProjectileDefinition(TooLongLifetime, Errors));

		FProjectileDefinition BoundaryLifetime = MakeStraightProjectileDefinition();
		BoundaryLifetime.LifetimeS = MaxProjectileLifetimeS;
		TestTrue(TEXT("the lifetime bound itself is accepted"), ValidateProjectileDefinition(BoundaryLifetime, Errors));
	}

	// Damage profile and pierce count.
	{
		FProjectileDefinition NoProfile = MakeStraightProjectileDefinition();
		NoProfile.DamageProfileId = NAME_None;
		TestFalse(TEXT("a projectile without a damage profile is rejected"), ValidateProjectileDefinition(NoProfile, Errors));
		TestTrue(TEXT("the missing-profile error names DamageProfileId"), Errors.Contains(TEXT("DamageProfileId")));

		FProjectileDefinition NegativePierce = MakeStraightProjectileDefinition();
		NegativePierce.PierceCount = -1;
		TestFalse(TEXT("a negative pierce count is rejected"), ValidateProjectileDefinition(NegativePierce, Errors));
		TestTrue(TEXT("the negative-pierce error names PierceCount"), Errors.Contains(TEXT("PierceCount")));

		FProjectileDefinition TooManyPierce = MakeStraightProjectileDefinition();
		TooManyPierce.PierceCount = MaxProjectilePierceCount + 1;
		TestFalse(TEXT("a pierce count above the bound is rejected"), ValidateProjectileDefinition(TooManyPierce, Errors));

		FProjectileDefinition BoundaryPierce = MakeStraightProjectileDefinition();
		BoundaryPierce.PierceCount = MaxProjectilePierceCount;
		TestTrue(TEXT("the pierce bound itself is accepted"), ValidateProjectileDefinition(BoundaryPierce, Errors));
	}

	// Explosion semantics: radius 0 means no explosion; an explosion needs its
	// own damage profile; an explosion must never combine with piercing (one
	// projectile must not detonate several times).
	{
		FProjectileDefinition RadiusWithoutProfile = MakeExplosiveProjectileDefinition();
		RadiusWithoutProfile.ExplosionDamageProfileId = NAME_None;
		TestFalse(TEXT("an explosion radius without an explosion damage profile is rejected"),
			ValidateProjectileDefinition(RadiusWithoutProfile, Errors));
		TestTrue(TEXT("the radius-without-profile error names ExplosionDamageProfileId"),
			Errors.Contains(TEXT("ExplosionDamageProfileId")));

		FProjectileDefinition ProfileWithoutRadius = MakeStraightProjectileDefinition();
		ProfileWithoutRadius.ExplosionDamageProfileId = TEXT("explosive_40");
		TestFalse(TEXT("an explosion damage profile without an explosion radius is rejected"),
			ValidateProjectileDefinition(ProfileWithoutRadius, Errors));
		TestTrue(TEXT("the profile-without-radius error names ExplosionDamageProfileId"),
			Errors.Contains(TEXT("ExplosionDamageProfileId")));

		FProjectileDefinition ExplosiveAndPiercing = MakeExplosiveProjectileDefinition();
		ExplosiveAndPiercing.PierceCount = 1;
		TestFalse(TEXT("an explosion combined with piercing is rejected"),
			ValidateProjectileDefinition(ExplosiveAndPiercing, Errors));
		TestTrue(TEXT("the explosion-plus-pierce error names PierceCount"), Errors.Contains(TEXT("PierceCount")));

		FProjectileDefinition NegativeRadius = MakeStraightProjectileDefinition();
		NegativeRadius.ExplosionRadiusCm = -1.0f;
		TestFalse(TEXT("a negative explosion radius is rejected"), ValidateProjectileDefinition(NegativeRadius, Errors));
		TestTrue(TEXT("the negative-radius error names ExplosionRadiusCm"), Errors.Contains(TEXT("ExplosionRadiusCm")));

		FProjectileDefinition TooLargeRadius = MakeExplosiveProjectileDefinition();
		TooLargeRadius.ExplosionRadiusCm = MaxProjectileExplosionRadiusCm + 1.0f;
		TestFalse(TEXT("an explosion radius above the bound is rejected"), ValidateProjectileDefinition(TooLargeRadius, Errors));

		FProjectileDefinition BoundaryRadius = MakeExplosiveProjectileDefinition();
		BoundaryRadius.ExplosionRadiusCm = MaxProjectileExplosionRadiusCm;
		TestTrue(TEXT("the explosion-radius bound itself is accepted"), ValidateProjectileDefinition(BoundaryRadius, Errors));
	}

	// Homing semantics: homing needs a positive turn rate; the other motions
	// must not carry one.
	{
		FProjectileDefinition ZeroTurnRate = MakeHomingProjectileDefinition();
		ZeroTurnRate.HomingTurnRateDegS = 0.0f;
		TestFalse(TEXT("a homing projectile with a zero turn rate is rejected"),
			ValidateProjectileDefinition(ZeroTurnRate, Errors));
		TestTrue(TEXT("the zero-turn-rate error names HomingTurnRateDegS"), Errors.Contains(TEXT("HomingTurnRateDegS")));

		FProjectileDefinition NegativeTurnRate = MakeHomingProjectileDefinition();
		NegativeTurnRate.HomingTurnRateDegS = -30.0f;
		TestFalse(TEXT("a homing projectile with a negative turn rate is rejected"),
			ValidateProjectileDefinition(NegativeTurnRate, Errors));

		FProjectileDefinition TooFastTurnRate = MakeHomingProjectileDefinition();
		TooFastTurnRate.HomingTurnRateDegS = MaxHomingTurnRateDegS + 1.0f;
		TestFalse(TEXT("a homing turn rate above the bound is rejected"), ValidateProjectileDefinition(TooFastTurnRate, Errors));

		FProjectileDefinition BoundaryTurnRate = MakeHomingProjectileDefinition();
		BoundaryTurnRate.HomingTurnRateDegS = MaxHomingTurnRateDegS;
		TestTrue(TEXT("the homing turn-rate bound itself is accepted"), ValidateProjectileDefinition(BoundaryTurnRate, Errors));

		FProjectileDefinition StraightWithTurnRate = MakeStraightProjectileDefinition();
		StraightWithTurnRate.HomingTurnRateDegS = 90.0f;
		TestFalse(TEXT("a straight projectile with a homing turn rate is rejected"),
			ValidateProjectileDefinition(StraightWithTurnRate, Errors));
		TestTrue(TEXT("the straight-turn-rate error names HomingTurnRateDegS"), Errors.Contains(TEXT("HomingTurnRateDegS")));
	}

	// Several problems at once are collected and joined with "; ".
	{
		FProjectileDefinition Broken;
		Broken.ProjectileId = NAME_None;
		Broken.Motion = static_cast<EProjectileMotion>(50);
		Broken.SpeedCmS = -5.0f;
		Broken.LifetimeS = 0.0f;
		TestFalse(TEXT("a projectile with multiple problems is rejected"), ValidateProjectileDefinition(Broken, Errors));
		TestTrue(TEXT("ProjectileId problem is included"), Errors.Contains(TEXT("ProjectileId")));
		TestTrue(TEXT("Motion problem is included"), Errors.Contains(TEXT("Motion")));
		TestTrue(TEXT("SpeedCmS problem is included"), Errors.Contains(TEXT("SpeedCmS")));
		TestTrue(TEXT("LifetimeS problem is included"), Errors.Contains(TEXT("LifetimeS")));
		TestTrue(TEXT("problems are joined with \"; \""), Errors.Contains(TEXT("; ")));
	}
	return true;
}

namespace UE::UEMMO::Tasks::M5_004
{
	// ------------------------------------------------------------------
	// JSON row -> struct converters (strict: unknown fields are rejected)
	// ------------------------------------------------------------------

	static const TCHAR* WeaponRowFields[] =
	{
		TEXT("weapon_id"), TEXT("mode"), TEXT("damage_profile_id"), TEXT("ammo_id"),
		TEXT("magazine_size"), TEXT("fire_rate_rpm"), TEXT("burst_count"), TEXT("pellet_count"),
		TEXT("spread_degrees_deg"), TEXT("range_cm"), TEXT("projectile_id"), TEXT("melee_attack_ids")
	};

	/** Reads one weapons.json row into FWeaponDefinition; reports every missing/mistyped field. */
	static bool MakeWeaponFromJson(FAutomationTestBase& Test, const TSharedPtr<FConfigJsonValue>& Entry,
		FWeaponDefinition& OutDefinition)
	{
		const FString Context = FString::Printf(TEXT("weapon '%s'"), *EntryName(Entry, TEXT("weapon_id")));
		if (!CheckOnlyKnownFields(Test, Entry, WeaponRowFields, 12, *Context))
		{
			return false;
		}

		FString WeaponId;
		FString ModeText;
		FString DamageProfileId;
		FString AmmoId;
		int32 MagazineSize = 0;
		double FireRateRpm = 0.0;
		int32 BurstCount = 0;
		int32 PelletCount = 0;
		double SpreadDegrees = 0.0;
		double RangeCm = 0.0;
		FString ProjectileId;
		TArray<FString> MeleeAttackIds;

		const bool bComplete =
			GetStringOrReport(Test, Entry, TEXT("weapon_id"), *Context, WeaponId) &&
			GetStringOrReport(Test, Entry, TEXT("mode"), *Context, ModeText) &&
			GetStringOrReport(Test, Entry, TEXT("damage_profile_id"), *Context, DamageProfileId) &&
			GetStringOrReport(Test, Entry, TEXT("ammo_id"), *Context, AmmoId) &&
			GetIntOrReport(Test, Entry, TEXT("magazine_size"), *Context, MagazineSize) &&
			GetNumberOrReport(Test, Entry, TEXT("fire_rate_rpm"), *Context, FireRateRpm) &&
			GetIntOrReport(Test, Entry, TEXT("burst_count"), *Context, BurstCount) &&
			GetIntOrReport(Test, Entry, TEXT("pellet_count"), *Context, PelletCount) &&
			GetNumberOrReport(Test, Entry, TEXT("spread_degrees_deg"), *Context, SpreadDegrees) &&
			GetNumberOrReport(Test, Entry, TEXT("range_cm"), *Context, RangeCm) &&
			GetStringOrReport(Test, Entry, TEXT("projectile_id"), *Context, ProjectileId) &&
			GetStringArrayOrReport(Test, Entry, TEXT("melee_attack_ids"), *Context, MeleeAttackIds);
		if (!bComplete)
		{
			return false;
		}

		EWeaponFireMode Mode;
		if (!ParseWeaponFireMode(ModeText, Mode))
		{
			Test.AddError(FString::Printf(TEXT("%s: mode '%s' is not one of melee/hitscan/projectile"),
				*Context, *ModeText));
			return false;
		}

		OutDefinition = FWeaponDefinition();
		OutDefinition.WeaponId = FName(*WeaponId);
		OutDefinition.FireMode = Mode;
		OutDefinition.DamageProfileId = FName(*DamageProfileId);
		OutDefinition.AmmoId = FName(*AmmoId);
		OutDefinition.MagazineSize = MagazineSize;
		OutDefinition.FireRateRpm = static_cast<float>(FireRateRpm);
		OutDefinition.BurstCount = BurstCount;
		OutDefinition.PelletCount = PelletCount;
		OutDefinition.SpreadDegrees = static_cast<float>(SpreadDegrees);
		OutDefinition.RangeCm = static_cast<float>(RangeCm);
		OutDefinition.ProjectileId = FName(*ProjectileId);
		for (const FString& AttackId : MeleeAttackIds)
		{
			OutDefinition.MeleeAttackIds.Add(FName(*AttackId));
		}
		return true;
	}

	static const TCHAR* AmmoRowFields[] =
	{
		TEXT("ammo_id"), TEXT("max_reserve"), TEXT("magazine_size")
	};

	/** Reads one ammo_types.json row into FAmmoType; reports every missing/mistyped field. */
	static bool MakeAmmoFromJson(FAutomationTestBase& Test, const TSharedPtr<FConfigJsonValue>& Entry,
		FAmmoType& OutAmmo)
	{
		const FString Context = FString::Printf(TEXT("ammo '%s'"), *EntryName(Entry, TEXT("ammo_id")));
		if (!CheckOnlyKnownFields(Test, Entry, AmmoRowFields, 3, *Context))
		{
			return false;
		}

		FString AmmoId;
		int32 MaxReserve = 0;
		int32 MagazineSize = 0;
		const bool bComplete =
			GetStringOrReport(Test, Entry, TEXT("ammo_id"), *Context, AmmoId) &&
			GetIntOrReport(Test, Entry, TEXT("max_reserve"), *Context, MaxReserve) &&
			GetIntOrReport(Test, Entry, TEXT("magazine_size"), *Context, MagazineSize);
		if (!bComplete)
		{
			return false;
		}

		OutAmmo = FAmmoType();
		OutAmmo.AmmoId = FName(*AmmoId);
		OutAmmo.MaxReserve = MaxReserve;
		OutAmmo.MagazineSize = MagazineSize;
		return true;
	}

	static const TCHAR* ProjectileRowFields[] =
	{
		TEXT("projectile_id"), TEXT("motion"), TEXT("speed_cm_s"), TEXT("lifetime_s"),
		TEXT("damage_profile_id"), TEXT("pierce_count"), TEXT("explosion_radius_cm"),
		TEXT("explosion_damage_profile_id"), TEXT("homing_turn_rate_deg_s")
	};

	/** Reads one projectiles.json row into FProjectileDefinition; reports every missing/mistyped field. */
	static bool MakeProjectileFromJson(FAutomationTestBase& Test, const TSharedPtr<FConfigJsonValue>& Entry,
		FProjectileDefinition& OutDefinition)
	{
		const FString Context = FString::Printf(TEXT("projectile '%s'"), *EntryName(Entry, TEXT("projectile_id")));
		if (!CheckOnlyKnownFields(Test, Entry, ProjectileRowFields, 9, *Context))
		{
			return false;
		}

		FString ProjectileId;
		FString MotionText;
		double SpeedCmS = 0.0;
		double LifetimeS = 0.0;
		FString DamageProfileId;
		int32 PierceCount = 0;
		double ExplosionRadiusCm = 0.0;
		FString ExplosionDamageProfileId;
		double HomingTurnRateDegS = 0.0;

		const bool bComplete =
			GetStringOrReport(Test, Entry, TEXT("projectile_id"), *Context, ProjectileId) &&
			GetStringOrReport(Test, Entry, TEXT("motion"), *Context, MotionText) &&
			GetNumberOrReport(Test, Entry, TEXT("speed_cm_s"), *Context, SpeedCmS) &&
			GetNumberOrReport(Test, Entry, TEXT("lifetime_s"), *Context, LifetimeS) &&
			GetStringOrReport(Test, Entry, TEXT("damage_profile_id"), *Context, DamageProfileId) &&
			GetIntOrReport(Test, Entry, TEXT("pierce_count"), *Context, PierceCount) &&
			GetNumberOrReport(Test, Entry, TEXT("explosion_radius_cm"), *Context, ExplosionRadiusCm) &&
			GetStringOrReport(Test, Entry, TEXT("explosion_damage_profile_id"), *Context, ExplosionDamageProfileId) &&
			GetNumberOrReport(Test, Entry, TEXT("homing_turn_rate_deg_s"), *Context, HomingTurnRateDegS);
		if (!bComplete)
		{
			return false;
		}

		EProjectileMotion Motion;
		if (!ParseProjectileMotion(MotionText, Motion))
		{
			Test.AddError(FString::Printf(TEXT("%s: motion '%s' is not one of straight/parabolic/homing"),
				*Context, *MotionText));
			return false;
		}

		OutDefinition = FProjectileDefinition();
		OutDefinition.ProjectileId = FName(*ProjectileId);
		OutDefinition.Motion = Motion;
		OutDefinition.SpeedCmS = static_cast<float>(SpeedCmS);
		OutDefinition.LifetimeS = static_cast<float>(LifetimeS);
		OutDefinition.DamageProfileId = FName(*DamageProfileId);
		OutDefinition.PierceCount = PierceCount;
		OutDefinition.ExplosionRadiusCm = static_cast<float>(ExplosionRadiusCm);
		OutDefinition.ExplosionDamageProfileId = FName(*ExplosionDamageProfileId);
		OutDefinition.HomingTurnRateDegS = static_cast<float>(HomingTurnRateDegS);
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_004WeaponsJsonLoadsAndValidates,
	"UEMMO.Tasks.M5_004.WeaponsJsonLoadsAndValidates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_004WeaponsJsonLoadsAndValidates::RunTest(const FString& Parameters)
{
	// The weapons source table loads, every row parses strictly, validates
	// individually and registers under a unique id; the pinned sample values
	// cover melee, hitscan (single/burst/pellet) and the four visible
	// projectile samples.
	FString LoadError;
	TSharedPtr<FConfigJsonValue> Root;
	if (!LoadJsonRoot(GetCombatSystemJsonPath(TEXT("weapons.json")), LoadError, Root))
	{
		AddError(LoadError);
		return true;
	}

	const TSharedPtr<FConfigJsonValue>* SchemaVersion = FindTyped(Root, TEXT("schema_version"), FConfigJsonValue::EKind::Number);
	if (SchemaVersion)
	{
		TestEqual(TEXT("weapons schema_version is 1"), (*SchemaVersion)->Number, 1.0);
	}
	else
	{
		AddError(TEXT("weapons.json: missing or mistyped top-level number 'schema_version'"));
	}

	const TSharedPtr<FConfigJsonValue>* Table = FindTyped(Root, TEXT("table"), FConfigJsonValue::EKind::String);
	if (Table)
	{
		TestEqual(TEXT("weapons table name is 'weapons'"), (*Table)->String, FString(TEXT("weapons")));
	}
	else
	{
		AddError(TEXT("weapons.json: missing or mistyped top-level string 'table'"));
	}

	static const TCHAR* TopLevelFields[] = { TEXT("schema_version"), TEXT("table"), TEXT("rows") };
	CheckOnlyKnownFields(*this, Root, TopLevelFields, 3, TEXT("weapons.json top level"));

	const TSharedPtr<FConfigJsonValue>* Rows = FindTyped(Root, TEXT("rows"), FConfigJsonValue::EKind::Array);
	if (!Rows)
	{
		AddError(TEXT("weapons.json: missing required top-level array 'rows'"));
		return true;
	}
	TestEqual(TEXT("weapons.json holds exactly 8 rows"), (*Rows)->Array.Num(), 8);

	TMap<FName, FWeaponDefinition> Loaded;
	for (const TSharedPtr<FConfigJsonValue>& Entry : (*Rows)->Array)
	{
		if (!Entry.IsValid() || Entry->Kind != FConfigJsonValue::EKind::Object)
		{
			AddError(TEXT("a weapons.json row is not a JSON object"));
			continue;
		}
		FWeaponDefinition Definition;
		if (!MakeWeaponFromJson(*this, Entry, Definition))
		{
			continue;
		}
		FString Errors;
		TestTrue(FString::Printf(TEXT("weapon '%s' passes ValidateWeaponDefinition"), *Definition.WeaponId.ToString()),
			ValidateWeaponDefinition(Definition, Errors));
		TestTrue(FString::Printf(TEXT("weapon '%s' reports no validation errors"), *Definition.WeaponId.ToString()),
			Errors.IsEmpty());
		if (Loaded.Contains(Definition.WeaponId))
		{
			AddError(FString::Printf(TEXT("weapons.json: duplicate weapon_id '%s'"), *Definition.WeaponId.ToString()));
		}
		else
		{
			Loaded.Add(Definition.WeaponId, Definition);
		}
	}
	TestEqual(TEXT("all 8 weapon rows loaded with unique ids"), Loaded.Num(), 8);

	// Pinned sample: the melee training sword carries exactly the legacy four
	// attacks and no ranged field.
	const FWeaponDefinition* Sword = Loaded.Find(FName(TEXT("weapon_training_sword")));
	TestTrue(TEXT("weapon_training_sword is registered"), Sword != nullptr);
	if (Sword)
	{
		TestTrue(TEXT("weapon_training_sword is melee"), Sword->FireMode == EWeaponFireMode::Melee);
		TestTrue(TEXT("weapon_training_sword has no ammo"), Sword->AmmoId.IsNone());
		TestTrue(TEXT("weapon_training_sword has no projectile"), Sword->ProjectileId.IsNone());
		TestEqual(TEXT("weapon_training_sword damage profile is physical_10"),
			Sword->DamageProfileId, FName(TEXT("physical_10")));
		TestTrue(TEXT("weapon_training_sword lists exactly the legacy four attacks"),
			Sword->MeleeAttackIds == GetLegacyMeleeAttackIds());
	}

	// Pinned sample: the hitscan ray gun (single pellet).
	const FWeaponDefinition* RayGun = Loaded.Find(FName(TEXT("weapon_hitscan_sample")));
	TestTrue(TEXT("weapon_hitscan_sample is registered"), RayGun != nullptr);
	if (RayGun)
	{
		TestTrue(TEXT("weapon_hitscan_sample is hitscan"), RayGun->FireMode == EWeaponFireMode::Hitscan);
		TestEqual(TEXT("weapon_hitscan_sample uses ammo_cell"), RayGun->AmmoId, FName(TEXT("ammo_cell")));
		TestTrue(TEXT("weapon_hitscan_sample has no projectile"), RayGun->ProjectileId.IsNone());
		TestEqual(TEXT("weapon_hitscan_sample magazine is 12"), RayGun->MagazineSize, 12);
		TestEqual(TEXT("weapon_hitscan_sample fire rate is 240 rpm"), RayGun->FireRateRpm, 240.0f);
		TestEqual(TEXT("weapon_hitscan_sample range is 5000 cm"), RayGun->RangeCm, 5000.0f);
	}

	// Pinned samples: burst and pellet variants cover the remaining firearm
	// pacing/geometry dimensions.
	const FWeaponDefinition* BurstGun = Loaded.Find(FName(TEXT("weapon_burst_sample")));
	TestTrue(TEXT("weapon_burst_sample is registered"), BurstGun != nullptr);
	if (BurstGun)
	{
		TestEqual(TEXT("weapon_burst_sample burst count is 3"), BurstGun->BurstCount, 3);
	}
	const FWeaponDefinition* PelletGun = Loaded.Find(FName(TEXT("weapon_pellet_sample")));
	TestTrue(TEXT("weapon_pellet_sample is registered"), PelletGun != nullptr);
	if (PelletGun)
	{
		TestEqual(TEXT("weapon_pellet_sample pellet count is 5"), PelletGun->PelletCount, 5);
		TestEqual(TEXT("weapon_pellet_sample spread is 8 degrees"), PelletGun->SpreadDegrees, 8.0f);
	}

	// Pinned samples: the four visible projectile weapons reference the
	// linear/parabolic/homing/explosive projectile rows.
	struct FProjectilePin
	{
		const TCHAR* WeaponId;
		const TCHAR* ProjectileId;
	};
	const FProjectilePin ProjectilePins[] =
	{
		{ TEXT("weapon_projectile_sample"), TEXT("bullet_linear") },
		{ TEXT("weapon_parabolic_sample"), TEXT("shell_parabolic") },
		{ TEXT("weapon_homing_sample"), TEXT("missile_homing") },
		{ TEXT("weapon_explosive_sample"), TEXT("rocket_explosive") }
	};
	for (const FProjectilePin& Pin : ProjectilePins)
	{
		const FWeaponDefinition* Weapon = Loaded.Find(FName(Pin.WeaponId));
		if (!TestNotNull(FString::Printf(TEXT("%s is registered"), Pin.WeaponId), Weapon))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s is in projectile mode"), Pin.WeaponId),
			Weapon->FireMode == EWeaponFireMode::Projectile);
		TestEqual(FString::Printf(TEXT("%s references %s"), Pin.WeaponId, Pin.ProjectileId),
			Weapon->ProjectileId, FName(Pin.ProjectileId));
		TestTrue(FString::Printf(TEXT("%s carries no weapon damage profile"), Pin.WeaponId),
			Weapon->DamageProfileId.IsNone());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_004AmmoAndProjectileJsonLoadAndValidate,
	"UEMMO.Tasks.M5_004.AmmoAndProjectileJsonLoadAndValidate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_004AmmoAndProjectileJsonLoadAndValidate::RunTest(const FString& Parameters)
{
	// The ammo and projectile source tables load, validate and cross-reference
	// the weapon rows coherently. The pass below only pins this card's own
	// sample data; the production parser/validator with error codes belongs to
	// M5-005/006.
	FString LoadError;
	TSharedPtr<FConfigJsonValue> AmmoRoot;
	if (!LoadJsonRoot(GetCombatSystemJsonPath(TEXT("ammo_types.json")), LoadError, AmmoRoot))
	{
		AddError(LoadError);
		return true;
	}

	const TSharedPtr<FConfigJsonValue>* AmmoSchema = FindTyped(AmmoRoot, TEXT("schema_version"), FConfigJsonValue::EKind::Number);
	if (AmmoSchema)
	{
		TestEqual(TEXT("ammo_types schema_version is 1"), (*AmmoSchema)->Number, 1.0);
	}
	else
	{
		AddError(TEXT("ammo_types.json: missing or mistyped top-level number 'schema_version'"));
	}
	const TSharedPtr<FConfigJsonValue>* AmmoTable = FindTyped(AmmoRoot, TEXT("table"), FConfigJsonValue::EKind::String);
	if (AmmoTable)
	{
		TestEqual(TEXT("ammo_types table name is 'ammo_types'"), (*AmmoTable)->String, FString(TEXT("ammo_types")));
	}
	else
	{
		AddError(TEXT("ammo_types.json: missing or mistyped top-level string 'table'"));
	}

	static const TCHAR* AmmoTopLevelFields[] = { TEXT("schema_version"), TEXT("table"), TEXT("rows") };
	CheckOnlyKnownFields(*this, AmmoRoot, AmmoTopLevelFields, 3, TEXT("ammo_types.json top level"));

	const TSharedPtr<FConfigJsonValue>* AmmoRows = FindTyped(AmmoRoot, TEXT("rows"), FConfigJsonValue::EKind::Array);
	if (!AmmoRows)
	{
		AddError(TEXT("ammo_types.json: missing required top-level array 'rows'"));
		return true;
	}
	TestEqual(TEXT("ammo_types.json holds exactly 5 rows"), (*AmmoRows)->Array.Num(), 5);

	TMap<FName, FAmmoType> AmmoTypes;
	for (const TSharedPtr<FConfigJsonValue>& Entry : (*AmmoRows)->Array)
	{
		if (!Entry.IsValid() || Entry->Kind != FConfigJsonValue::EKind::Object)
		{
			AddError(TEXT("an ammo_types.json row is not a JSON object"));
			continue;
		}
		FAmmoType Ammo;
		if (!MakeAmmoFromJson(*this, Entry, Ammo))
		{
			continue;
		}
		FString Errors;
		TestTrue(FString::Printf(TEXT("ammo '%s' passes ValidateAmmoType"), *Ammo.AmmoId.ToString()),
			ValidateAmmoType(Ammo, Errors));
		if (AmmoTypes.Contains(Ammo.AmmoId))
		{
			AddError(FString::Printf(TEXT("ammo_types.json: duplicate ammo_id '%s'"), *Ammo.AmmoId.ToString()));
		}
		else
		{
			AmmoTypes.Add(Ammo.AmmoId, Ammo);
		}
	}
	TestEqual(TEXT("all 5 ammo rows loaded with unique ids"), AmmoTypes.Num(), 5);

	const FAmmoType* CellAmmo = AmmoTypes.Find(FName(TEXT("ammo_cell")));
	TestTrue(TEXT("ammo_cell is registered"), CellAmmo != nullptr);
	if (CellAmmo)
	{
		TestEqual(TEXT("ammo_cell max reserve is 120"), CellAmmo->MaxReserve, 120);
		TestEqual(TEXT("ammo_cell magazine size is 12"), CellAmmo->MagazineSize, 12);
	}

	// Projectiles.
	TSharedPtr<FConfigJsonValue> ProjectileRoot;
	if (!LoadJsonRoot(GetCombatSystemJsonPath(TEXT("projectiles.json")), LoadError, ProjectileRoot))
	{
		AddError(LoadError);
		return true;
	}

	const TSharedPtr<FConfigJsonValue>* ProjectileSchema = FindTyped(ProjectileRoot, TEXT("schema_version"), FConfigJsonValue::EKind::Number);
	if (ProjectileSchema)
	{
		TestEqual(TEXT("projectiles schema_version is 1"), (*ProjectileSchema)->Number, 1.0);
	}
	else
	{
		AddError(TEXT("projectiles.json: missing or mistyped top-level number 'schema_version'"));
	}
	const TSharedPtr<FConfigJsonValue>* ProjectileTable = FindTyped(ProjectileRoot, TEXT("table"), FConfigJsonValue::EKind::String);
	if (ProjectileTable)
	{
		TestEqual(TEXT("projectiles table name is 'projectiles'"), (*ProjectileTable)->String, FString(TEXT("projectiles")));
	}
	else
	{
		AddError(TEXT("projectiles.json: missing or mistyped top-level string 'table'"));
	}

	static const TCHAR* ProjectileTopLevelFields[] = { TEXT("schema_version"), TEXT("table"), TEXT("rows") };
	CheckOnlyKnownFields(*this, ProjectileRoot, ProjectileTopLevelFields, 3, TEXT("projectiles.json top level"));

	const TSharedPtr<FConfigJsonValue>* ProjectileRows = FindTyped(ProjectileRoot, TEXT("rows"), FConfigJsonValue::EKind::Array);
	if (!ProjectileRows)
	{
		AddError(TEXT("projectiles.json: missing required top-level array 'rows'"));
		return true;
	}
	TestEqual(TEXT("projectiles.json holds exactly 5 rows"), (*ProjectileRows)->Array.Num(), 5);

	TMap<FName, FProjectileDefinition> Projectiles;
	for (const TSharedPtr<FConfigJsonValue>& Entry : (*ProjectileRows)->Array)
	{
		if (!Entry.IsValid() || Entry->Kind != FConfigJsonValue::EKind::Object)
		{
			AddError(TEXT("a projectiles.json row is not a JSON object"));
			continue;
		}
		FProjectileDefinition Definition;
		if (!MakeProjectileFromJson(*this, Entry, Definition))
		{
			continue;
		}
		FString Errors;
		TestTrue(FString::Printf(TEXT("projectile '%s' passes ValidateProjectileDefinition"), *Definition.ProjectileId.ToString()),
			ValidateProjectileDefinition(Definition, Errors));
		if (Projectiles.Contains(Definition.ProjectileId))
		{
			AddError(FString::Printf(TEXT("projectiles.json: duplicate projectile_id '%s'"), *Definition.ProjectileId.ToString()));
		}
		else
		{
			Projectiles.Add(Definition.ProjectileId, Definition);
		}
	}
	TestEqual(TEXT("all 5 projectile rows loaded with unique ids"), Projectiles.Num(), 5);

	// Pinned projectile samples.
	const FProjectileDefinition* Bullet = Projectiles.Find(FName(TEXT("bullet_linear")));
	TestTrue(TEXT("bullet_linear is registered"), Bullet != nullptr);
	if (Bullet)
	{
		TestTrue(TEXT("bullet_linear moves straight"), Bullet->Motion == EProjectileMotion::Straight);
		TestEqual(TEXT("bullet_linear speed is 6000 cm/s"), Bullet->SpeedCmS, 6000.0f);
		TestEqual(TEXT("bullet_linear lifetime is 2 s"), Bullet->LifetimeS, 2.0f);
		TestEqual(TEXT("bullet_linear pierces nothing"), Bullet->PierceCount, 0);
		TestEqual(TEXT("bullet_linear has no explosion"), Bullet->ExplosionRadiusCm, 0.0f);
	}
	const FProjectileDefinition* PiercingBullet = Projectiles.Find(FName(TEXT("bullet_piercing")));
	TestTrue(TEXT("bullet_piercing is registered"), PiercingBullet != nullptr);
	if (PiercingBullet)
	{
		TestEqual(TEXT("bullet_piercing pierces 2 extra targets"), PiercingBullet->PierceCount, 2);
	}
	const FProjectileDefinition* Shell = Projectiles.Find(FName(TEXT("shell_parabolic")));
	TestTrue(TEXT("shell_parabolic is registered"), Shell != nullptr);
	if (Shell)
	{
		TestTrue(TEXT("shell_parabolic moves parabolic"), Shell->Motion == EProjectileMotion::Parabolic);
	}
	const FProjectileDefinition* Missile = Projectiles.Find(FName(TEXT("missile_homing")));
	TestTrue(TEXT("missile_homing is registered"), Missile != nullptr);
	if (Missile)
	{
		TestTrue(TEXT("missile_homing homes"), Missile->Motion == EProjectileMotion::Homing);
		TestEqual(TEXT("missile_homing turn rate is 180 deg/s"), Missile->HomingTurnRateDegS, 180.0f);
	}
	const FProjectileDefinition* Rocket = Projectiles.Find(FName(TEXT("rocket_explosive")));
	TestTrue(TEXT("rocket_explosive is registered"), Rocket != nullptr);
	if (Rocket)
	{
		TestEqual(TEXT("rocket_explosive explosion radius is 300 cm"), Rocket->ExplosionRadiusCm, 300.0f);
		TestEqual(TEXT("rocket_explosive explosion profile is explosive_40"),
			Rocket->ExplosionDamageProfileId, FName(TEXT("explosive_40")));
		TestEqual(TEXT("rocket_explosive pierces nothing (explosion excludes piercing)"), Rocket->PierceCount, 0);
	}

	// Fixture coherence: reload weapons.json and check that every referenced
	// ammo/projectile id resolves inside the sibling tables of this card.
	TSharedPtr<FConfigJsonValue> WeaponRoot;
	if (!LoadJsonRoot(GetCombatSystemJsonPath(TEXT("weapons.json")), LoadError, WeaponRoot))
	{
		AddError(LoadError);
		return true;
	}
	const TSharedPtr<FConfigJsonValue>* WeaponRows = FindTyped(WeaponRoot, TEXT("rows"), FConfigJsonValue::EKind::Array);
	if (!WeaponRows)
	{
		AddError(TEXT("weapons.json: missing required top-level array 'rows'"));
		return true;
	}
	for (const TSharedPtr<FConfigJsonValue>& Entry : (*WeaponRows)->Array)
	{
		FWeaponDefinition Definition;
		if (!MakeWeaponFromJson(*this, Entry, Definition))
		{
			continue;
		}
		if (!Definition.AmmoId.IsNone() && !AmmoTypes.Contains(Definition.AmmoId))
		{
			AddError(FString::Printf(TEXT("weapon '%s' references unknown ammo '%s'"),
				*Definition.WeaponId.ToString(), *Definition.AmmoId.ToString()));
		}
		if (!Definition.ProjectileId.IsNone() && !Projectiles.Contains(Definition.ProjectileId))
		{
			AddError(FString::Printf(TEXT("weapon '%s' references unknown projectile '%s'"),
				*Definition.WeaponId.ToString(), *Definition.ProjectileId.ToString()));
		}
	}
	for (const TPair<FName, FProjectileDefinition>& Pair : Projectiles)
	{
		if (Pair.Value.DamageProfileId.IsNone())
		{
			AddError(FString::Printf(TEXT("projectile '%s' has no damage profile"), *Pair.Key.ToString()));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_004TargetFiltersJsonTestRoomManifest,
	"UEMMO.Tasks.M5_004.TargetFiltersJsonTestRoomManifest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_004TargetFiltersJsonTestRoomManifest::RunTest(const FString& Parameters)
{
	// target_filters.json carries the target filter rows plus the first
	// test_room manifest structure (menu entry, map, waves with layout/filter/
	// reward ids and the vehicle_spawns list, which starts empty; 037 owns the
	// vehicle entry schema and extends the list without new script hardcodes).
	FString LoadError;
	TSharedPtr<FConfigJsonValue> Root;
	if (!LoadJsonRoot(GetCombatSystemJsonPath(TEXT("target_filters.json")), LoadError, Root))
	{
		AddError(LoadError);
		return true;
	}

	const TSharedPtr<FConfigJsonValue>* SchemaVersion = FindTyped(Root, TEXT("schema_version"), FConfigJsonValue::EKind::Number);
	if (SchemaVersion)
	{
		TestEqual(TEXT("target_filters schema_version is 1"), (*SchemaVersion)->Number, 1.0);
	}
	else
	{
		AddError(TEXT("target_filters.json: missing or mistyped top-level number 'schema_version'"));
	}
	const TSharedPtr<FConfigJsonValue>* Table = FindTyped(Root, TEXT("table"), FConfigJsonValue::EKind::String);
	if (Table)
	{
		TestEqual(TEXT("target_filters table name is 'target_filters'"), (*Table)->String, FString(TEXT("target_filters")));
	}
	else
	{
		AddError(TEXT("target_filters.json: missing or mistyped top-level string 'table'"));
	}

	static const TCHAR* TopLevelFields[] = { TEXT("schema_version"), TEXT("table"), TEXT("rows"), TEXT("test_room") };
	CheckOnlyKnownFields(*this, Root, TopLevelFields, 4, TEXT("target_filters.json top level"));

	// Filter rows.
	const TSharedPtr<FConfigJsonValue>* Rows = FindTyped(Root, TEXT("rows"), FConfigJsonValue::EKind::Array);
	if (!Rows)
	{
		AddError(TEXT("target_filters.json: missing required top-level array 'rows'"));
		return true;
	}
	TestEqual(TEXT("target_filters.json holds exactly 2 filter rows"), (*Rows)->Array.Num(), 2);

	static const TCHAR* FilterRowFields[] =
	{
		TEXT("filter_id"), TEXT("allowed_team_relations"), TEXT("required_tags"),
		TEXT("excluded_tags"), TEXT("block_world")
	};

	TSet<FName> FilterIds;
	for (const TSharedPtr<FConfigJsonValue>& Entry : (*Rows)->Array)
	{
		if (!Entry.IsValid() || Entry->Kind != FConfigJsonValue::EKind::Object)
		{
			AddError(TEXT("a target_filters.json row is not a JSON object"));
			continue;
		}
		CheckOnlyKnownFields(*this, Entry, FilterRowFields, 5, TEXT("target_filters row"));

		FString FilterId;
		TArray<FString> AllowedRelations;
		TArray<FString> RequiredTags;
		TArray<FString> ExcludedTags;
		bool bBlockWorld = false;
		const bool bComplete =
			GetStringOrReport(*this, Entry, TEXT("filter_id"), TEXT("target_filters row"), FilterId) &&
			GetStringArrayOrReport(*this, Entry, TEXT("allowed_team_relations"), TEXT("target_filters row"), AllowedRelations) &&
			GetStringArrayOrReport(*this, Entry, TEXT("required_tags"), TEXT("target_filters row"), RequiredTags) &&
			GetStringArrayOrReport(*this, Entry, TEXT("excluded_tags"), TEXT("target_filters row"), ExcludedTags) &&
			GetBoolOrReport(*this, Entry, TEXT("block_world"), TEXT("target_filters row"), bBlockWorld);
		if (!bComplete)
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("filter_id '%s' uses the lowercase a-z0-9_ form"), *FilterId),
			IsValidDefinitionIdText(FilterId));
		TestTrue(FString::Printf(TEXT("filter '%s' allows at least one team relation"), *FilterId),
			AllowedRelations.Num() > 0);
		const FName FilterName = FName(*FilterId);
		TestFalse(FString::Printf(TEXT("filter_id '%s' is unique"), *FilterId), FilterIds.Contains(FilterName));
		FilterIds.Add(FilterName);
	}
	TestEqual(TEXT("both filter ids registered"), FilterIds.Num(), 2);

	// The test_room manifest.
	const TSharedPtr<FConfigJsonValue>* TestRoom = FindTyped(Root, TEXT("test_room"), FConfigJsonValue::EKind::Object);
	if (!TestRoom || !TestRoom->IsValid())
	{
		AddError(TEXT("target_filters.json: missing or mistyped top-level object 'test_room'"));
		return true;
	}
	static const TCHAR* TestRoomFields[] = { TEXT("menu_entry"), TEXT("map_path"), TEXT("waves"), TEXT("vehicle_spawns") };
	CheckOnlyKnownFields(*this, *TestRoom, TestRoomFields, 4, TEXT("test_room"));

	FString MenuEntry;
	FString MapPath;
	if (GetStringOrReport(*this, *TestRoom, TEXT("menu_entry"), TEXT("test_room"), MenuEntry))
	{
		TestTrue(TEXT("test_room menu_entry is not empty"), !MenuEntry.IsEmpty());
	}
	if (GetStringOrReport(*this, *TestRoom, TEXT("map_path"), TEXT("test_room"), MapPath))
	{
		TestTrue(TEXT("test_room map_path is a /Game/ content path"), MapPath.StartsWith(TEXT("/Game/")));
	}

	const TSharedPtr<FConfigJsonValue>* Waves = FindTyped(*TestRoom, TEXT("waves"), FConfigJsonValue::EKind::Array);
	if (!Waves)
	{
		AddError(TEXT("test_room: missing or mistyped array 'waves'"));
		return true;
	}
	TestTrue(TEXT("test_room declares at least one wave"), (*Waves)->Array.Num() > 0);

	static const TCHAR* WaveFields[] = { TEXT("wave_index"), TEXT("layout_id"), TEXT("target_filter_id"), TEXT("reward_pool_id") };
	int32 ExpectedWaveIndex = 1;
	for (const TSharedPtr<FConfigJsonValue>& Entry : (*Waves)->Array)
	{
		if (!Entry.IsValid() || Entry->Kind != FConfigJsonValue::EKind::Object)
		{
			AddError(TEXT("a test_room wave is not a JSON object"));
			continue;
		}
		CheckOnlyKnownFields(*this, Entry, WaveFields, 4, TEXT("test_room wave"));

		int32 WaveIndex = 0;
		FString LayoutId;
		FString TargetFilterId;
		FString RewardPoolId;
		const bool bComplete =
			GetIntOrReport(*this, Entry, TEXT("wave_index"), TEXT("test_room wave"), WaveIndex) &&
			GetStringOrReport(*this, Entry, TEXT("layout_id"), TEXT("test_room wave"), LayoutId) &&
			GetStringOrReport(*this, Entry, TEXT("target_filter_id"), TEXT("test_room wave"), TargetFilterId) &&
			GetStringOrReport(*this, Entry, TEXT("reward_pool_id"), TEXT("test_room wave"), RewardPoolId);
		if (!bComplete)
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("wave %d index is sequential from 1"), WaveIndex), WaveIndex, ExpectedWaveIndex);
		++ExpectedWaveIndex;
		TestTrue(TEXT("wave layout_id is not empty"), !LayoutId.IsEmpty());
		TestTrue(FString::Printf(TEXT("wave target_filter_id '%s' references a registered filter"), *TargetFilterId),
			FilterIds.Contains(FName(*TargetFilterId)));
		TestTrue(TEXT("wave reward_pool_id is not empty"), !RewardPoolId.IsEmpty());
	}

	// First version: the vehicle spawn list exists but stays empty; 037 adds
	// entries and validates their fields, nothing hardcodes content here.
	const TSharedPtr<FConfigJsonValue>* VehicleSpawns = FindTyped(*TestRoom, TEXT("vehicle_spawns"), FConfigJsonValue::EKind::Array);
	if (VehicleSpawns)
	{
		TestEqual(TEXT("test_room vehicle_spawns starts empty"), (*VehicleSpawns)->Array.Num(), 0);
	}
	else
	{
		AddError(TEXT("test_room: missing or mistyped array 'vehicle_spawns'"));
	}
	return true;
}

#endif
