// M5-003: damage profiles, attack reactions and target reaction policies
// (interface contract section 1, first owner 003). Pins the three new value
// types, the four source tables under Data/CombatSystem and the legacy
// default values of the four melee attacks: damage 10/14/18/12, air combo
// cap 2 with 1.0/0.7 launch scales, knockdown 0.45 s plus recovering 0.25 s
// and hit stop 0.04 s. Pure checks: this file only reads JSON text and plain
// structs; it never touches UE assets, worlds or wall clocks.
//
// Parsing note (same lesson as M1-008/M3-001): UnrealBuildTool does not put
// the engine Json import library on the UEMMO link line, so the tests use
// the small hand-rolled parser below (objects, arrays, strings with escapes,
// numbers, booleans) built on Core types only.

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"

#include "../Combat/System/DamageTypes.h"
#include "../Combat/System/ReactionTypes.h"

#include <type_traits>
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

// ---------------------------------------------------------------------------
// Compile-time pins: definitions are immutable configuration values, never
// World/Actor pointer holders (interface contract section 0.5).
// ---------------------------------------------------------------------------

static_assert(!std::is_pointer_v<decltype(FDamageProfile::DamageProfileId)>, "FDamageProfile::DamageProfileId must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageProfile::BaseDamage)>, "FDamageProfile::BaseDamage must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageProfile::AttackCoefficient)>, "FDamageProfile::AttackCoefficient must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageProfile::HitStunSeconds)>, "FDamageProfile::HitStunSeconds must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageProfile::KnockbackCmPerSecond)>, "FDamageProfile::KnockbackCmPerSecond must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageProfile::LaunchCmPerSecond)>, "FDamageProfile::LaunchCmPerSecond must stay a value type");
static_assert(!std::is_pointer_v<decltype(FDamageProfile::HitStopSeconds)>, "FDamageProfile::HitStopSeconds must stay a value type");
static_assert(std::is_trivially_copyable_v<FDamageProfile>, "FDamageProfile must stay a trivially copyable value snapshot");

static_assert(!std::is_pointer_v<decltype(FAttackReaction::ReactionId)>, "FAttackReaction::ReactionId must stay a value type");
static_assert(!std::is_pointer_v<decltype(FAttackReaction::ControlPenetration)>, "FAttackReaction::ControlPenetration must stay a value type");
static_assert(std::is_trivially_copyable_v<FAttackReaction>, "FAttackReaction must stay a trivially copyable value snapshot");

static_assert(!std::is_pointer_v<decltype(FTargetReaction::PolicyId)>, "FTargetReaction::PolicyId must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::bAllowStagger)>, "FTargetReaction::bAllowStagger must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::bAllowLaunch)>, "FTargetReaction::bAllowLaunch must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::bAllowKnockdown)>, "FTargetReaction::bAllowKnockdown must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::MaxLaunchesPerAirCycle)>, "FTargetReaction::MaxLaunchesPerAirCycle must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::LaunchZScales)>, "FTargetReaction::LaunchZScales must stay a value container");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::MaxAirTimeSeconds)>, "FTargetReaction::MaxAirTimeSeconds must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::PoiseMax)>, "FTargetReaction::PoiseMax must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::PoiseRegenSeconds)>, "FTargetReaction::PoiseRegenSeconds must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::KnockdownSeconds)>, "FTargetReaction::KnockdownSeconds must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::RecoveringSeconds)>, "FTargetReaction::RecoveringSeconds must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::bImmuneDamage)>, "FTargetReaction::bImmuneDamage must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::bImmuneControl)>, "FTargetReaction::bImmuneControl must stay a value type");
static_assert(!std::is_pointer_v<decltype(FTargetReaction::bDeathResistant)>, "FTargetReaction::bDeathResistant must stay a value type");
static_assert(std::is_default_constructible_v<FTargetReaction>, "FTargetReaction must stay default constructible");

namespace UE::UEMMO::Tasks::M5_003
{
	/** Minimal JSON value: just enough shape for the combat system tables. */
	class FM5JsonValue
	{
	public:
		enum class EKind
		{
			Boolean,
			Number,
			String,
			Array,
			Object
		};

		EKind Kind = EKind::Number;
		bool Boolean = false;
		double Number = 0.0;
		FString String;
		TArray<TSharedPtr<FM5JsonValue>> Array;
		TMap<FString, TSharedPtr<FM5JsonValue>> Object;
	};

	/** Recursive descent JSON parser (RFC 8259 subset; \u escapes decode to one TCHAR). */
	class FM5JsonParser
	{
	public:
		static bool Parse(const FString& Text, TSharedPtr<FM5JsonValue>& OutRoot, FString& OutError)
		{
			FM5JsonParser Parser(Text);
			Parser.SkipWhitespace();
			TSharedPtr<FM5JsonValue> Root = Parser.ParseValue(OutError);
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
		explicit FM5JsonParser(const FString& InText)
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

		TSharedPtr<FM5JsonValue> ParseValue(FString& OutError)
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
			default: return ParseNumber(OutError);
			}
		}

		TSharedPtr<FM5JsonValue> ParseObject(FString& OutError)
		{
			if (!Expect(TEXT('{'), TEXT("to start an object"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FM5JsonValue> Value = MakeShared<FM5JsonValue>();
			Value->Kind = FM5JsonValue::EKind::Object;
			SkipWhitespace();
			if (Consume(TEXT('}')))
			{
				return Value;
			}
			while (true)
			{
				SkipWhitespace();
				TSharedPtr<FM5JsonValue> Key = ParseString(OutError);
				if (!Key.IsValid())
				{
					return nullptr;
				}
				SkipWhitespace();
				if (!Expect(TEXT(':'), TEXT("between object key and value"), OutError))
				{
					return nullptr;
				}
				TSharedPtr<FM5JsonValue> Element = ParseValue(OutError);
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

		TSharedPtr<FM5JsonValue> ParseArray(FString& OutError)
		{
			if (!Expect(TEXT('['), TEXT("to start an array"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FM5JsonValue> Value = MakeShared<FM5JsonValue>();
			Value->Kind = FM5JsonValue::EKind::Array;
			SkipWhitespace();
			if (Consume(TEXT(']')))
			{
				return Value;
			}
			while (true)
			{
				TSharedPtr<FM5JsonValue> Element = ParseValue(OutError);
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

		TSharedPtr<FM5JsonValue> ParseString(FString& OutError)
		{
			if (!Expect(TEXT('"'), TEXT("to start a string"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FM5JsonValue> Value = MakeShared<FM5JsonValue>();
			Value->Kind = FM5JsonValue::EKind::String;
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

		TSharedPtr<FM5JsonValue> ParseBoolean(FString& OutError)
		{
			const TSharedPtr<FM5JsonValue> Value = MakeShared<FM5JsonValue>();
			if (MatchLiteral(TEXT("true"), 4))
			{
				Value->Kind = FM5JsonValue::EKind::Boolean;
				Value->Boolean = true;
				Position += 4;
				return Value;
			}
			if (MatchLiteral(TEXT("false"), 5))
			{
				Value->Kind = FM5JsonValue::EKind::Boolean;
				Value->Boolean = false;
				Position += 5;
				return Value;
			}
			OutError = FString::Printf(TEXT("invalid literal at offset %d"), Position);
			return nullptr;
		}

		TSharedPtr<FM5JsonValue> ParseNumber(FString& OutError)
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
			const TSharedPtr<FM5JsonValue> Value = MakeShared<FM5JsonValue>();
			Value->Kind = FM5JsonValue::EKind::Number;
			Value->Number = FCString::Atod(*Source.Mid(Start, Position - Start));
			return Value;
		}
	};

	static FString GetCombatSystemJsonPath(const TCHAR* FileName)
	{
		return FPaths::ProjectDir() / TEXT("Data") / TEXT("CombatSystem") / FileName;
	}

	static bool LoadJsonRoot(const FString& Path, FString& OutError, TSharedPtr<FM5JsonValue>& OutRoot)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			OutError = FString::Printf(TEXT("could not read the JSON data file at %s"), *Path);
			return false;
		}
		TSharedPtr<FM5JsonValue> Root;
		if (!FM5JsonParser::Parse(Text, Root, OutError))
		{
			OutError = FString::Printf(TEXT("could not parse JSON in %s: %s"), *Path, *OutError);
			return false;
		}
		OutRoot = Root;
		return true;
	}

	/** Returns the field only when present with exactly the requested JSON kind. */
	static const TSharedPtr<FM5JsonValue>* FindTyped(const TSharedPtr<FM5JsonValue>& Object, const TCHAR* Name,
		FM5JsonValue::EKind Kind)
	{
		const TSharedPtr<FM5JsonValue>* Value = Object->Object.Find(Name);
		if (!Value || !Value->IsValid() || (*Value)->Kind != Kind)
		{
			return nullptr;
		}
		return Value;
	}

	static FString RowEntryName(const TSharedPtr<FM5JsonValue>& Row, const TCHAR* IdFieldName)
	{
		const TSharedPtr<FM5JsonValue>* Id = FindTyped(Row, IdFieldName, FM5JsonValue::EKind::String);
		return (Id && !(*Id)->String.IsEmpty()) ? (*Id)->String : FString(TEXT("<unnamed row>"));
	}

	static bool GetStringOrReport(FAutomationTestBase& Test, const TSharedPtr<FM5JsonValue>& Row,
		const TCHAR* Name, FString& OutValue)
	{
		const TSharedPtr<FM5JsonValue>* Value = FindTyped(Row, Name, FM5JsonValue::EKind::String);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("row '%s': missing or mistyped string field '%s'"),
				*RowEntryName(Row, TEXT("row_id")), Name));
			return false;
		}
		OutValue = (*Value)->String;
		return true;
	}

	static bool GetNumberOrReport(FAutomationTestBase& Test, const TSharedPtr<FM5JsonValue>& Row,
		const TCHAR* Name, double& OutValue)
	{
		const TSharedPtr<FM5JsonValue>* Value = FindTyped(Row, Name, FM5JsonValue::EKind::Number);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("row '%s': missing or mistyped number field '%s'"),
				*RowEntryName(Row, TEXT("row_id")), Name));
			return false;
		}
		OutValue = (*Value)->Number;
		return true;
	}

	static bool GetBoolOrReport(FAutomationTestBase& Test, const TSharedPtr<FM5JsonValue>& Row,
		const TCHAR* Name, bool& OutValue)
	{
		const TSharedPtr<FM5JsonValue>* Value = FindTyped(Row, Name, FM5JsonValue::EKind::Boolean);
		if (!Value)
		{
			Test.AddError(FString::Printf(TEXT("row '%s': missing or mistyped boolean field '%s'"),
				*RowEntryName(Row, TEXT("row_id")), Name));
			return false;
		}
		OutValue = (*Value)->Boolean;
		return true;
	}

	/** Comparison tolerance for doubles that both sides derive from decimal literals. */
	static bool NearlyEqual(double A, double B)
	{
		return FMath::Abs(A - B) <= 1e-9;
	}

	/** Reads one damage_profiles row into FDamageProfile; reports missing fields. */
	static bool MakeDamageProfileFromJson(FAutomationTestBase& Test, const TSharedPtr<FM5JsonValue>& Row,
		FDamageProfile& OutProfile)
	{
		FString IdText;
		double BaseDamage = 0.0;
		double Coefficient = 0.0;
		double HitStun = 0.0;
		double Knockback = 0.0;
		double Launch = 0.0;
		double HitStop = 0.0;

		const bool bComplete =
			GetStringOrReport(Test, Row, TEXT("damage_profile_id"), IdText) &&
			GetNumberOrReport(Test, Row, TEXT("base_damage"), BaseDamage) &&
			GetNumberOrReport(Test, Row, TEXT("attack_coefficient"), Coefficient) &&
			GetNumberOrReport(Test, Row, TEXT("hit_stun_s"), HitStun) &&
			GetNumberOrReport(Test, Row, TEXT("knockback_cm_s"), Knockback) &&
			GetNumberOrReport(Test, Row, TEXT("launch_cm_s"), Launch) &&
			GetNumberOrReport(Test, Row, TEXT("hit_stop_s"), HitStop);
		if (!bComplete)
		{
			return false;
		}

		OutProfile = FDamageProfile();
		OutProfile.DamageProfileId = FName(*IdText);
		OutProfile.BaseDamage = static_cast<float>(BaseDamage);
		OutProfile.AttackCoefficient = static_cast<float>(Coefficient);
		OutProfile.HitStunSeconds = static_cast<float>(HitStun);
		OutProfile.KnockbackCmPerSecond = static_cast<float>(Knockback);
		OutProfile.LaunchCmPerSecond = static_cast<float>(Launch);
		OutProfile.HitStopSeconds = static_cast<float>(HitStop);
		return true;
	}

	/** Reads one attack_reactions row into FAttackReaction; reports bad fields. */
	static bool MakeAttackReactionFromJson(FAutomationTestBase& Test, const TSharedPtr<FM5JsonValue>& Row,
		FAttackReaction& OutReaction)
	{
		FString IdText;
		FString PenetrationText;
		if (!GetStringOrReport(Test, Row, TEXT("reaction_id"), IdText) ||
			!GetStringOrReport(Test, Row, TEXT("control_penetration"), PenetrationText))
		{
			return false;
		}
		EControlPenetration Penetration = EControlPenetration::None;
		if (!ParseControlPenetration(PenetrationText, Penetration))
		{
			Test.AddError(FString::Printf(TEXT("attack reaction '%s': control_penetration '%s' is not one of none/bypass_poise"),
				*IdText, *PenetrationText));
			return false;
		}
		OutReaction = FAttackReaction();
		OutReaction.ReactionId = FName(*IdText);
		OutReaction.ControlPenetration = Penetration;
		return true;
	}

	/** Reads one target_reactions row into FTargetReaction; reports bad fields. */
	static bool MakeTargetReactionFromJson(FAutomationTestBase& Test, const TSharedPtr<FM5JsonValue>& Row,
		FTargetReaction& OutPolicy)
	{
		FString IdText;
		double MaxLaunches = 0.0;
		double MaxAirTime = 0.0;
		double PoiseMax = 0.0;
		double PoiseRegen = 0.0;
		double Knockdown = 0.0;
		double Recovering = 0.0;

		const TSharedPtr<FM5JsonValue>* Scales = FindTyped(Row, TEXT("launch_z_scales"), FM5JsonValue::EKind::Array);
		if (!Scales || !Scales->IsValid())
		{
			Test.AddError(FString::Printf(TEXT("row '%s': missing or mistyped array field 'launch_z_scales'"),
				*RowEntryName(Row, TEXT("policy_id"))));
			return false;
		}
		TArray<float> ScalesParsed;
		for (const TSharedPtr<FM5JsonValue>& Scale : (*Scales)->Array)
		{
			if (!Scale.IsValid() || Scale->Kind != FM5JsonValue::EKind::Number)
			{
				Test.AddError(FString::Printf(TEXT("row '%s': launch_z_scales holds a non-number entry"),
					*RowEntryName(Row, TEXT("policy_id"))));
				return false;
			}
			ScalesParsed.Add(static_cast<float>(Scale->Number));
		}

		bool bAllowStagger = false;
		bool bAllowLaunch = false;
		bool bAllowKnockdown = false;
		bool bImmuneDamage = false;
		bool bImmuneControl = false;
		bool bDeathResistant = false;

		const bool bComplete =
			GetStringOrReport(Test, Row, TEXT("policy_id"), IdText) &&
			GetNumberOrReport(Test, Row, TEXT("max_launches_per_air_cycle"), MaxLaunches) &&
			GetNumberOrReport(Test, Row, TEXT("max_air_time_s"), MaxAirTime) &&
			GetNumberOrReport(Test, Row, TEXT("poise_max"), PoiseMax) &&
			GetNumberOrReport(Test, Row, TEXT("poise_regen_s"), PoiseRegen) &&
			GetNumberOrReport(Test, Row, TEXT("knockdown_s"), Knockdown) &&
			GetNumberOrReport(Test, Row, TEXT("recovering_s"), Recovering) &&
			GetBoolOrReport(Test, Row, TEXT("allow_stagger"), bAllowStagger) &&
			GetBoolOrReport(Test, Row, TEXT("allow_launch"), bAllowLaunch) &&
			GetBoolOrReport(Test, Row, TEXT("allow_knockdown"), bAllowKnockdown) &&
			GetBoolOrReport(Test, Row, TEXT("immune_damage"), bImmuneDamage) &&
			GetBoolOrReport(Test, Row, TEXT("immune_control"), bImmuneControl) &&
			GetBoolOrReport(Test, Row, TEXT("death_resistant"), bDeathResistant);
		if (!bComplete)
		{
			return false;
		}

		if (MaxLaunches < 0.0 || MaxLaunches > static_cast<double>(MAX_int32) || !NearlyEqual(MaxLaunches, static_cast<double>(static_cast<int32>(MaxLaunches))))
		{
			Test.AddError(FString::Printf(TEXT("row '%s': max_launches_per_air_cycle is not a non-negative whole number"),
				*IdText));
			return false;
		}

		OutPolicy = FTargetReaction();
		OutPolicy.PolicyId = FName(*IdText);
		OutPolicy.bAllowStagger = bAllowStagger;
		OutPolicy.bAllowLaunch = bAllowLaunch;
		OutPolicy.bAllowKnockdown = bAllowKnockdown;
		OutPolicy.MaxLaunchesPerAirCycle = static_cast<int32>(MaxLaunches);
		OutPolicy.LaunchZScales = ScalesParsed;
		OutPolicy.MaxAirTimeSeconds = static_cast<float>(MaxAirTime);
		OutPolicy.PoiseMax = static_cast<float>(PoiseMax);
		OutPolicy.PoiseRegenSeconds = static_cast<float>(PoiseRegen);
		OutPolicy.KnockdownSeconds = static_cast<float>(Knockdown);
		OutPolicy.RecoveringSeconds = static_cast<float>(Recovering);
		OutPolicy.bImmuneDamage = bImmuneDamage;
		OutPolicy.bImmuneControl = bImmuneControl;
		OutPolicy.bDeathResistant = bDeathResistant;
		return true;
	}
}

using namespace UE::UEMMO::Tasks::M5_003;

// ---------------------------------------------------------------------------
// DamageProfilesJsonMatchesLegacyDefaults
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_003DamageProfilesJsonMatchesLegacyDefaults,
	"UEMMO.Tasks.M5_003.DamageProfilesJsonMatchesLegacyDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_003DamageProfilesJsonMatchesLegacyDefaults::RunTest(const FString& Parameters)
{
	// The four legacy melee attacks keep their exact old values in the new
	// damage_profiles source table: damage 10/14/18/12, the old stun/knockback/
	// launch magnitudes and the shared 0.04 s hit stop.
	FString LoadError;
	TSharedPtr<FM5JsonValue> Root;
	if (!LoadJsonRoot(GetCombatSystemJsonPath(TEXT("damage_profiles.json")), LoadError, Root))
	{
		AddError(LoadError);
		return true;
	}

	const TSharedPtr<FM5JsonValue>* SchemaVersion = FindTyped(Root, TEXT("schema_version"), FM5JsonValue::EKind::Number);
	if (TestNotNull(TEXT("schema_version is present as a number"), SchemaVersion))
	{
		TestEqual(TEXT("schema_version is 1"), (*SchemaVersion)->Number, 1.0);
	}
	const TSharedPtr<FM5JsonValue>* Table = FindTyped(Root, TEXT("table"), FM5JsonValue::EKind::String);
	if (TestNotNull(TEXT("table is present as a string"), Table))
	{
		TestEqual(TEXT("table is damage_profiles"), (*Table)->String, FString(TEXT("damage_profiles")));
	}
	const TSharedPtr<FM5JsonValue>* Rows = FindTyped(Root, TEXT("rows"), FM5JsonValue::EKind::Array);
	if (!TestNotNull(TEXT("rows is present as an array"), Rows))
	{
		return true;
	}

	TMap<FName, FDamageProfile> Profiles;
	for (const TSharedPtr<FM5JsonValue>& Row : (*Rows)->Array)
	{
		if (!Row.IsValid() || Row->Kind != FM5JsonValue::EKind::Object)
		{
			AddError(TEXT("a damage_profiles row is not a JSON object"));
			continue;
		}
		FDamageProfile Profile;
		if (!MakeDamageProfileFromJson(*this, Row, Profile))
		{
			continue;
		}
		FString Errors;
		TestTrue(FString::Printf(TEXT("damage profile '%s' passes ValidateDamageProfile"), *Profile.DamageProfileId.ToString()),
			ValidateDamageProfile(Profile, Errors));
		TestTrue(FString::Printf(TEXT("damage profile '%s' reports no validation errors"), *Profile.DamageProfileId.ToString()),
			Errors.IsEmpty());
		TestFalse(FString::Printf(TEXT("damage profile '%s' is not a duplicate id"), *Profile.DamageProfileId.ToString()),
			Profiles.Contains(Profile.DamageProfileId));
		Profiles.Add(Profile.DamageProfileId, Profile);
	}
	TestEqual(TEXT("damage_profiles holds exactly the 4 legacy attack rows"), Profiles.Num(), 4);

	// Pinned legacy values, row by row.
	struct FLegacyDamageRow
	{
		const TCHAR* Id;
		float BaseDamage;
		float HitStun;
		float Knockback;
		float Launch;
	};
	const FLegacyDamageRow LegacyRows[] = {
		{TEXT("light_01"), 10.0f, 0.22f, 90.0f, 0.0f},
		{TEXT("light_02"), 14.0f, 0.28f, 120.0f, 0.0f},
		{TEXT("launcher"), 18.0f, 1.0f, 70.0f, 700.0f},
		{TEXT("aerial_01"), 12.0f, 0.18f, 80.0f, 60.0f}
	};
	for (const FLegacyDamageRow& Legacy : LegacyRows)
	{
		const FDamageProfile* Profile = Profiles.Find(FName(Legacy.Id));
		if (!TestNotNull(FString::Printf(TEXT("%s is registered"), Legacy.Id), Profile))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s BaseDamage keeps its legacy value"), Legacy.Id), Profile->BaseDamage, Legacy.BaseDamage);
		TestEqual(FString::Printf(TEXT("%s AttackCoefficient is 1.0"), Legacy.Id), Profile->AttackCoefficient, 1.0f);
		TestEqual(FString::Printf(TEXT("%s HitStunSeconds keeps its legacy value"), Legacy.Id), Profile->HitStunSeconds, Legacy.HitStun);
		TestEqual(FString::Printf(TEXT("%s KnockbackCmPerSecond keeps its legacy value"), Legacy.Id), Profile->KnockbackCmPerSecond, Legacy.Knockback);
		TestEqual(FString::Printf(TEXT("%s LaunchCmPerSecond keeps its legacy value"), Legacy.Id), Profile->LaunchCmPerSecond, Legacy.Launch);
		TestEqual(FString::Printf(TEXT("%s HitStopSeconds keeps the legacy 0.04 s"), Legacy.Id), Profile->HitStopSeconds, 0.04f);
	}
	return true;
}

// ---------------------------------------------------------------------------
// AttackReactionsJsonMatchesLegacyDefaults
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_003AttackReactionsJsonMatchesLegacyDefaults,
	"UEMMO.Tasks.M5_003.AttackReactionsJsonMatchesLegacyDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_003AttackReactionsJsonMatchesLegacyDefaults::RunTest(const FString& Parameters)
{
	// The four legacy attacks all carry control_penetration none: the legacy
	// game has no poise bypass, and the new table must express that exactly.
	FString LoadError;
	TSharedPtr<FM5JsonValue> Root;
	if (!LoadJsonRoot(GetCombatSystemJsonPath(TEXT("attack_reactions.json")), LoadError, Root))
	{
		AddError(LoadError);
		return true;
	}
	const TSharedPtr<FM5JsonValue>* SchemaVersion = FindTyped(Root, TEXT("schema_version"), FM5JsonValue::EKind::Number);
	if (TestNotNull(TEXT("schema_version is present as a number"), SchemaVersion))
	{
		TestEqual(TEXT("schema_version is 1"), (*SchemaVersion)->Number, 1.0);
	}
	const TSharedPtr<FM5JsonValue>* Table = FindTyped(Root, TEXT("table"), FM5JsonValue::EKind::String);
	if (TestNotNull(TEXT("table is present as a string"), Table))
	{
		TestEqual(TEXT("table is attack_reactions"), (*Table)->String, FString(TEXT("attack_reactions")));
	}
	const TSharedPtr<FM5JsonValue>* Rows = FindTyped(Root, TEXT("rows"), FM5JsonValue::EKind::Array);
	if (!TestNotNull(TEXT("rows is present as an array"), Rows))
	{
		return true;
	}

	TMap<FName, FAttackReaction> Reactions;
	for (const TSharedPtr<FM5JsonValue>& Row : (*Rows)->Array)
	{
		if (!Row.IsValid() || Row->Kind != FM5JsonValue::EKind::Object)
		{
			AddError(TEXT("an attack_reactions row is not a JSON object"));
			continue;
		}
		FAttackReaction Reaction;
		if (!MakeAttackReactionFromJson(*this, Row, Reaction))
		{
			continue;
		}
		FString Errors;
		TestTrue(FString::Printf(TEXT("attack reaction '%s' passes ValidateAttackReaction"), *Reaction.ReactionId.ToString()),
			ValidateAttackReaction(Reaction, Errors));
		TestTrue(FString::Printf(TEXT("attack reaction '%s' reports no validation errors"), *Reaction.ReactionId.ToString()),
			Errors.IsEmpty());
		TestFalse(FString::Printf(TEXT("attack reaction '%s' is not a duplicate id"), *Reaction.ReactionId.ToString()),
			Reactions.Contains(Reaction.ReactionId));
		Reactions.Add(Reaction.ReactionId, Reaction);
	}
	TestEqual(TEXT("attack_reactions holds exactly the 4 legacy attack rows"), Reactions.Num(), 4);

	const TCHAR* LegacyIds[] = {TEXT("light_01"), TEXT("light_02"), TEXT("launcher"), TEXT("aerial_01")};
	for (const TCHAR* LegacyId : LegacyIds)
	{
		const FAttackReaction* Reaction = Reactions.Find(FName(LegacyId));
		if (!TestNotNull(FString::Printf(TEXT("%s attack reaction is registered"), LegacyId), Reaction))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s control_penetration is none (legacy behavior)"), LegacyId),
			Reaction->ControlPenetration == EControlPenetration::None);
	}

	// The penetration token parser accepts exactly the two supported values,
	// case-insensitively, and refuses anything else.
	EControlPenetration Parsed = EControlPenetration::None;
	TestTrue(TEXT("ParseControlPenetration accepts 'none'"), ParseControlPenetration(TEXT("none"), Parsed) && Parsed == EControlPenetration::None);
	TestTrue(TEXT("ParseControlPenetration accepts 'NONE'"), ParseControlPenetration(TEXT("NONE"), Parsed) && Parsed == EControlPenetration::None);
	TestTrue(TEXT("ParseControlPenetration accepts 'bypass_poise'"), ParseControlPenetration(TEXT("bypass_poise"), Parsed) && Parsed == EControlPenetration::BypassPoise);
	TestTrue(TEXT("ParseControlPenetration accepts 'Bypass_Poise'"), ParseControlPenetration(TEXT("Bypass_Poise"), Parsed) && Parsed == EControlPenetration::BypassPoise);
	TestFalse(TEXT("ParseControlPenetration refuses the empty token"), ParseControlPenetration(TEXT(""), Parsed));
	TestFalse(TEXT("ParseControlPenetration refuses 'super_armor'"), ParseControlPenetration(TEXT("super_armor"), Parsed));
	TestFalse(TEXT("ParseControlPenetration refuses 'none ' with trailing space"), ParseControlPenetration(TEXT("none "), Parsed));
	TestFalse(TEXT("ParseControlPenetration refuses 'bypass'"), ParseControlPenetration(TEXT("bypass"), Parsed));

	// An in-memory bypass_poise reaction validates: the value exists in the
	// contract even though no legacy sample uses it.
	FAttackReaction Bypass;
	Bypass.ReactionId = TEXT("probe_bypass");
	Bypass.ControlPenetration = EControlPenetration::BypassPoise;
	FString Errors;
	TestTrue(TEXT("an in-memory bypass_poise reaction passes ValidateAttackReaction"), ValidateAttackReaction(Bypass, Errors));
	TestTrue(TEXT("the in-memory bypass_poise reaction reports no errors"), Errors.IsEmpty());
	return true;
}

// ---------------------------------------------------------------------------
// TargetReactionsJsonCoversLegacyDefaults
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_003TargetReactionsJsonCoversLegacyDefaults,
	"UEMMO.Tasks.M5_003.TargetReactionsJsonCoversLegacyDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_003TargetReactionsJsonCoversLegacyDefaults::RunTest(const FString& Parameters)
{
	// Three target policies: normal (the legacy default), heavy (launch
	// gated, high poise) and boss (all control immune, death resistant).
	FString LoadError;
	TSharedPtr<FM5JsonValue> Root;
	if (!LoadJsonRoot(GetCombatSystemJsonPath(TEXT("target_reactions.json")), LoadError, Root))
	{
		AddError(LoadError);
		return true;
	}
	const TSharedPtr<FM5JsonValue>* SchemaVersion = FindTyped(Root, TEXT("schema_version"), FM5JsonValue::EKind::Number);
	if (TestNotNull(TEXT("schema_version is present as a number"), SchemaVersion))
	{
		TestEqual(TEXT("schema_version is 1"), (*SchemaVersion)->Number, 1.0);
	}
	const TSharedPtr<FM5JsonValue>* Table = FindTyped(Root, TEXT("table"), FM5JsonValue::EKind::String);
	if (TestNotNull(TEXT("table is present as a string"), Table))
	{
		TestEqual(TEXT("table is target_reactions"), (*Table)->String, FString(TEXT("target_reactions")));
	}
	const TSharedPtr<FM5JsonValue>* Rows = FindTyped(Root, TEXT("rows"), FM5JsonValue::EKind::Array);
	if (!TestNotNull(TEXT("rows is present as an array"), Rows))
	{
		return true;
	}

	TMap<FName, FTargetReaction> Policies;
	for (const TSharedPtr<FM5JsonValue>& Row : (*Rows)->Array)
	{
		if (!Row.IsValid() || Row->Kind != FM5JsonValue::EKind::Object)
		{
			AddError(TEXT("a target_reactions row is not a JSON object"));
			continue;
		}
		FTargetReaction Policy;
		if (!MakeTargetReactionFromJson(*this, Row, Policy))
		{
			continue;
		}
		FString Errors;
		TestTrue(FString::Printf(TEXT("target policy '%s' passes ValidateTargetReaction"), *Policy.PolicyId.ToString()),
			ValidateTargetReaction(Policy, Errors));
		TestTrue(FString::Printf(TEXT("target policy '%s' reports no validation errors"), *Policy.PolicyId.ToString()),
			Errors.IsEmpty());
		TestFalse(FString::Printf(TEXT("target policy '%s' is not a duplicate id"), *Policy.PolicyId.ToString()),
			Policies.Contains(Policy.PolicyId));
		Policies.Add(Policy.PolicyId, Policy);
	}
	TestEqual(TEXT("target_reactions holds exactly 3 policies (normal/heavy/boss)"), Policies.Num(), 3);

	// normal: every legacy default, including the full air-combo policy.
	const FTargetReaction* Normal = Policies.Find(FName(TEXT("normal")));
	if (TestNotNull(TEXT("normal is registered"), Normal))
	{
		TestTrue(TEXT("normal allows stagger"), Normal->bAllowStagger);
		TestTrue(TEXT("normal allows launch"), Normal->bAllowLaunch);
		TestTrue(TEXT("normal allows knockdown"), Normal->bAllowKnockdown);
		TestEqual(TEXT("normal caps the air combo at the legacy 2 launches"), Normal->MaxLaunchesPerAirCycle, 2);
		TestEqual(TEXT("normal carries exactly 2 launch scales"), Normal->LaunchZScales.Num(), 2);
		if (Normal->LaunchZScales.Num() == 2)
		{
			TestEqual(TEXT("the first legacy launch scale is 1.0"), Normal->LaunchZScales[0], 1.0f);
			TestEqual(TEXT("the second legacy launch scale is 0.7"), Normal->LaunchZScales[1], 0.7f);
		}
		TestEqual(TEXT("normal knockdown keeps the legacy 0.45 s"), Normal->KnockdownSeconds, 0.45f);
		TestEqual(TEXT("normal recovering keeps the legacy 0.25 s"), Normal->RecoveringSeconds, 0.25f);
		TestTrue(TEXT("normal max air time is a positive bound"), Normal->MaxAirTimeSeconds > 0.0f);
		TestEqual(TEXT("normal has no poise pool"), Normal->PoiseMax, 0.0f);
		TestFalse(TEXT("normal is not damage immune"), Normal->bImmuneDamage);
		TestFalse(TEXT("normal is not control immune"), Normal->bImmuneControl);
		TestFalse(TEXT("normal is not death resistant"), Normal->bDeathResistant);
	}

	// heavy: launch gated (launch immune) with a high poise pool, still
	// staggerable and knockdown-able.
	const FTargetReaction* Heavy = Policies.Find(FName(TEXT("heavy")));
	if (TestNotNull(TEXT("heavy is registered"), Heavy))
	{
		TestFalse(TEXT("heavy refuses launch (launch immune)"), Heavy->bAllowLaunch);
		TestTrue(TEXT("heavy still allows stagger"), Heavy->bAllowStagger);
		TestTrue(TEXT("heavy still allows knockdown"), Heavy->bAllowKnockdown);
		TestTrue(TEXT("heavy carries a poise pool above zero"), Heavy->PoiseMax > 0.0f);
		TestFalse(TEXT("heavy is not blanket control immune (gate stays per-kind)"), Heavy->bImmuneControl);
		TestFalse(TEXT("heavy is not damage immune"), Heavy->bImmuneDamage);
		TestFalse(TEXT("heavy is not death resistant"), Heavy->bDeathResistant);
	}

	// boss: all control refused, death resistant, but NOT damage immune -
	// death resistance and damage immunity are separate axes.
	const FTargetReaction* Boss = Policies.Find(FName(TEXT("boss")));
	if (TestNotNull(TEXT("boss is registered"), Boss))
	{
		TestTrue(TEXT("boss is control immune (all control refused)"), Boss->bImmuneControl);
		TestFalse(TEXT("boss refuses stagger via its gates too"), Boss->bAllowStagger);
		TestFalse(TEXT("boss refuses launch via its gates too"), Boss->bAllowLaunch);
		TestFalse(TEXT("boss refuses knockdown via its gates too"), Boss->bAllowKnockdown);
		TestTrue(TEXT("boss is death resistant"), Boss->bDeathResistant);
		TestFalse(TEXT("boss is NOT damage immune (death resistance is not damage immunity)"), Boss->bImmuneDamage);
	}

	// The default-constructed policy must reproduce the legacy normal values
	// field by field, so old behavior needs no explicit row at all.
	FTargetReaction DefaultWithId = FTargetReaction();
	DefaultWithId.PolicyId = TEXT("normal");
	FString DefaultErrors;
	TestTrue(TEXT("the default target reaction (with an id) passes validation"), ValidateTargetReaction(DefaultWithId, DefaultErrors));
	TestTrue(TEXT("the default target reaction reports no errors"), DefaultErrors.IsEmpty());
	if (Normal)
	{
		TestTrue(TEXT("default bAllowStagger equals normal"), DefaultWithId.bAllowStagger == Normal->bAllowStagger);
		TestTrue(TEXT("default bAllowLaunch equals normal"), DefaultWithId.bAllowLaunch == Normal->bAllowLaunch);
		TestTrue(TEXT("default bAllowKnockdown equals normal"), DefaultWithId.bAllowKnockdown == Normal->bAllowKnockdown);
		TestEqual(TEXT("default MaxLaunchesPerAirCycle equals normal"), DefaultWithId.MaxLaunchesPerAirCycle, Normal->MaxLaunchesPerAirCycle);
		TestEqual(TEXT("default launch scale count equals normal"), DefaultWithId.LaunchZScales.Num(), Normal->LaunchZScales.Num());
		TestEqual(TEXT("default MaxAirTimeSeconds equals normal"), DefaultWithId.MaxAirTimeSeconds, Normal->MaxAirTimeSeconds);
		TestEqual(TEXT("default PoiseMax equals normal"), DefaultWithId.PoiseMax, Normal->PoiseMax);
		TestEqual(TEXT("default PoiseRegenSeconds equals normal"), DefaultWithId.PoiseRegenSeconds, Normal->PoiseRegenSeconds);
		TestEqual(TEXT("default KnockdownSeconds equals normal"), DefaultWithId.KnockdownSeconds, Normal->KnockdownSeconds);
		TestEqual(TEXT("default RecoveringSeconds equals normal"), DefaultWithId.RecoveringSeconds, Normal->RecoveringSeconds);
		TestTrue(TEXT("default bImmuneDamage equals normal"), DefaultWithId.bImmuneDamage == Normal->bImmuneDamage);
		TestTrue(TEXT("default bImmuneControl equals normal"), DefaultWithId.bImmuneControl == Normal->bImmuneControl);
		TestTrue(TEXT("default bDeathResistant equals normal"), DefaultWithId.bDeathResistant == Normal->bDeathResistant);
	}
	return true;
}

// ---------------------------------------------------------------------------
// PresentationsJsonMapsEveryReaction
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_003PresentationsJsonMapsEveryReaction,
	"UEMMO.Tasks.M5_003.PresentationsJsonMapsEveryReaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_003PresentationsJsonMapsEveryReaction::RunTest(const FString& Parameters)
{
	// Every attack reaction maps to exactly one placeholder hit presentation.
	FString LoadError;
	TSharedPtr<FM5JsonValue> PresentationsRoot;
	if (!LoadJsonRoot(GetCombatSystemJsonPath(TEXT("presentations.json")), LoadError, PresentationsRoot))
	{
		AddError(LoadError);
		return true;
	}
	TSharedPtr<FM5JsonValue> ReactionsRoot;
	if (!LoadJsonRoot(GetCombatSystemJsonPath(TEXT("attack_reactions.json")), LoadError, ReactionsRoot))
	{
		AddError(LoadError);
		return true;
	}

	const TSharedPtr<FM5JsonValue>* Table = FindTyped(PresentationsRoot, TEXT("table"), FM5JsonValue::EKind::String);
	if (TestNotNull(TEXT("presentations table is present as a string"), Table))
	{
		TestEqual(TEXT("table is presentations"), (*Table)->String, FString(TEXT("presentations")));
	}
	const TSharedPtr<FM5JsonValue>* Rows = FindTyped(PresentationsRoot, TEXT("rows"), FM5JsonValue::EKind::Array);
	if (!TestNotNull(TEXT("presentations rows is present as an array"), Rows))
	{
		return true;
	}

	// Collect the reaction ids of attack_reactions.json for the cross check.
	TSet<FName> ReactionIds;
	const TSharedPtr<FM5JsonValue>* ReactionRows = FindTyped(ReactionsRoot, TEXT("rows"), FM5JsonValue::EKind::Array);
	if (ReactionRows)
	{
		for (const TSharedPtr<FM5JsonValue>& Row : (*ReactionRows)->Array)
		{
			if (Row.IsValid() && Row->Kind == FM5JsonValue::EKind::Object)
			{
				const TSharedPtr<FM5JsonValue>* Id = FindTyped(Row, TEXT("reaction_id"), FM5JsonValue::EKind::String);
				if (Id)
				{
					ReactionIds.Add(FName(*(*Id)->String));
				}
			}
		}
	}
	TestEqual(TEXT("the reaction set used for the cross check holds the 4 legacy attacks"), ReactionIds.Num(), 4);

	TMap<FName, FName> PresentationByReaction;
	for (const TSharedPtr<FM5JsonValue>& Row : (*Rows)->Array)
	{
		if (!Row.IsValid() || Row->Kind != FM5JsonValue::EKind::Object)
		{
			AddError(TEXT("a presentations row is not a JSON object"));
			continue;
		}
		FString PresentationId;
		FString ReactionId;
		FString MontagePath;
		FString SoundPath;
		FString EffectPath;
		bool bPlaceholder = false;
		if (!GetStringOrReport(*this, Row, TEXT("presentation_id"), PresentationId) ||
			!GetStringOrReport(*this, Row, TEXT("reaction_id"), ReactionId) ||
			!GetStringOrReport(*this, Row, TEXT("montage_path"), MontagePath) ||
			!GetStringOrReport(*this, Row, TEXT("sound_path"), SoundPath) ||
			!GetStringOrReport(*this, Row, TEXT("effect_path"), EffectPath) ||
			!GetBoolOrReport(*this, Row, TEXT("placeholder"), bPlaceholder))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("presentation id '%s' uses the lowercase id syntax"), *PresentationId),
			IsValidDefinitionIdSyntax(PresentationId));
		TestTrue(FString::Printf(TEXT("presentation '%s' targets the valid reaction id '%s'"), *PresentationId, *ReactionId),
			IsValidDefinitionIdSyntax(ReactionId));
		const FName ReactionKey = FName(*ReactionId);
		TestTrue(FString::Printf(TEXT("presentation '%s' targets a reaction that exists in attack_reactions"), *PresentationId),
			ReactionIds.Contains(ReactionKey));
		TestFalse(FString::Printf(TEXT("presentation '%s' is not a duplicate reaction mapping"), *PresentationId),
			PresentationByReaction.Contains(ReactionKey));
		TestTrue(FString::Printf(TEXT("presentation '%s' is a placeholder row"), *PresentationId), bPlaceholder);
		PresentationByReaction.Add(ReactionKey, FName(*PresentationId));
	}

	// The mapping is total: one presentation per legacy attack reaction.
	TestEqual(TEXT("every attack reaction maps to one presentation"), PresentationByReaction.Num(), 4);
	for (const TCHAR* LegacyId : {TEXT("light_01"), TEXT("light_02"), TEXT("launcher"), TEXT("aerial_01")})
	{
		TestTrue(FString::Printf(TEXT("%s has a hit presentation"), LegacyId),
			PresentationByReaction.Contains(FName(LegacyId)));
	}
	return true;
}

// ---------------------------------------------------------------------------
// ValidatorsRejectIllegalValues
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_003ValidatorsRejectIllegalValues,
	"UEMMO.Tasks.M5_003.ValidatorsRejectIllegalValues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_003ValidatorsRejectIllegalValues::RunTest(const FString& Parameters)
{
	// Baseline valid structs first: every mutation below is the only change.
	FDamageProfile ValidProfile;
	ValidProfile.DamageProfileId = TEXT("light_01");
	ValidProfile.BaseDamage = 10.0f;
	ValidProfile.AttackCoefficient = 1.0f;
	ValidProfile.HitStunSeconds = 0.22f;
	ValidProfile.KnockbackCmPerSecond = 90.0f;
	ValidProfile.LaunchCmPerSecond = 0.0f;
	ValidProfile.HitStopSeconds = 0.04f;
	FString Errors;
	TestTrue(TEXT("the baseline damage profile passes validation"), ValidateDamageProfile(ValidProfile, Errors));
	TestTrue(TEXT("the baseline damage profile reports no errors"), Errors.IsEmpty());

	// Non-finite values are refused everywhere.
	FDamageProfile NanDamage = ValidProfile;
	NanDamage.BaseDamage = std::numeric_limits<float>::quiet_NaN();
	TestFalse(TEXT("a NaN BaseDamage is rejected"), ValidateDamageProfile(NanDamage, Errors));
	TestTrue(TEXT("the NaN BaseDamage error names BaseDamage"), Errors.Contains(TEXT("BaseDamage")));

	FDamageProfile InfiniteCoefficient = ValidProfile;
	InfiniteCoefficient.AttackCoefficient = std::numeric_limits<float>::infinity();
	TestFalse(TEXT("an infinite AttackCoefficient is rejected"), ValidateDamageProfile(InfiniteCoefficient, Errors));
	TestTrue(TEXT("the infinite AttackCoefficient error names AttackCoefficient"), Errors.Contains(TEXT("AttackCoefficient")));

	// Negative times and speeds are refused; zero time stays legal.
	FDamageProfile NegativeStun = ValidProfile;
	NegativeStun.HitStunSeconds = -0.1f;
	TestFalse(TEXT("a negative HitStunSeconds is rejected"), ValidateDamageProfile(NegativeStun, Errors));
	TestTrue(TEXT("the negative stun error names HitStunSeconds"), Errors.Contains(TEXT("HitStunSeconds")));

	FDamageProfile NegativeKnockback = ValidProfile;
	NegativeKnockback.KnockbackCmPerSecond = -1.0f;
	TestFalse(TEXT("a negative KnockbackCmPerSecond is rejected"), ValidateDamageProfile(NegativeKnockback, Errors));
	TestTrue(TEXT("the negative knockback error names KnockbackCmPerSecond"), Errors.Contains(TEXT("KnockbackCmPerSecond")));

	FDamageProfile NegativeLaunch = ValidProfile;
	NegativeLaunch.LaunchCmPerSecond = -700.0f;
	TestFalse(TEXT("a negative LaunchCmPerSecond is rejected"), ValidateDamageProfile(NegativeLaunch, Errors));
	TestTrue(TEXT("the negative launch error names LaunchCmPerSecond"), Errors.Contains(TEXT("LaunchCmPerSecond")));

	FDamageProfile NegativeHitStop = ValidProfile;
	NegativeHitStop.HitStopSeconds = -0.04f;
	TestFalse(TEXT("a negative HitStopSeconds is rejected"), ValidateDamageProfile(NegativeHitStop, Errors));
	TestTrue(TEXT("the negative hit stop error names HitStopSeconds"), Errors.Contains(TEXT("HitStopSeconds")));

	FDamageProfile ZeroDamage = ValidProfile;
	ZeroDamage.BaseDamage = 0.0f;
	TestFalse(TEXT("a zero BaseDamage is rejected"), ValidateDamageProfile(ZeroDamage, Errors));
	TestTrue(TEXT("the zero BaseDamage error names BaseDamage"), Errors.Contains(TEXT("BaseDamage")));

	// Ids must be 1..64 characters of a-z, 0-9 and _.
	FDamageProfile EmptyId = ValidProfile;
	EmptyId.DamageProfileId = NAME_None;
	TestFalse(TEXT("an empty DamageProfileId is rejected"), ValidateDamageProfile(EmptyId, Errors));
	TestTrue(TEXT("the empty DamageProfileId error names DamageProfileId"), Errors.Contains(TEXT("DamageProfileId")));

	FDamageProfile UppercaseId = ValidProfile;
	UppercaseId.DamageProfileId = TEXT("Light_01");
	TestFalse(TEXT("an uppercase DamageProfileId is rejected"), ValidateDamageProfile(UppercaseId, Errors));

	FDamageProfile HyphenId = ValidProfile;
	HyphenId.DamageProfileId = TEXT("light-01");
	TestFalse(TEXT("a DamageProfileId with a hyphen is rejected"), ValidateDamageProfile(HyphenId, Errors));

	FDamageProfile SpaceId = ValidProfile;
	SpaceId.DamageProfileId = TEXT("light 01");
	TestFalse(TEXT("a DamageProfileId with a space is rejected"), ValidateDamageProfile(SpaceId, Errors));

	FDamageProfile LongId = ValidProfile;
	LongId.DamageProfileId = TEXT("a1234567890123456789012345678901234567890123456789012345678901234");
	TestFalse(TEXT("a 65-character DamageProfileId is rejected"), ValidateDamageProfile(LongId, Errors));

	FDamageProfile Exactly64Id = ValidProfile;
	Exactly64Id.DamageProfileId = TEXT("a123456789012345678901234567890123456789012345678901234567890123");
	TestTrue(TEXT("a 64-character DamageProfileId is accepted"), ValidateDamageProfile(Exactly64Id, Errors));

	// Attack reactions: out-of-range penetration enums and ids are refused.
	FAttackReaction ValidReaction;
	ValidReaction.ReactionId = TEXT("light_01");
	ValidReaction.ControlPenetration = EControlPenetration::None;
	TestTrue(TEXT("the baseline attack reaction passes validation"), ValidateAttackReaction(ValidReaction, Errors));

	FAttackReaction BadPenetration = ValidReaction;
	BadPenetration.ControlPenetration = static_cast<EControlPenetration>(7);
	TestFalse(TEXT("an out-of-range ControlPenetration is rejected"), ValidateAttackReaction(BadPenetration, Errors));
	TestTrue(TEXT("the out-of-range penetration error names ControlPenetration"), Errors.Contains(TEXT("ControlPenetration")));

	FAttackReaction EmptyReactionId;
	EmptyReactionId.ControlPenetration = EControlPenetration::BypassPoise;
	TestFalse(TEXT("an attack reaction without a ReactionId is rejected"), ValidateAttackReaction(EmptyReactionId, Errors));
	TestTrue(TEXT("the empty reaction id error names ReactionId"), Errors.Contains(TEXT("ReactionId")));

	// Target reactions: every numeric field has its own guard.
	FTargetReaction ValidPolicy;
	ValidPolicy.PolicyId = TEXT("normal");
	TestTrue(TEXT("the baseline target policy passes validation"), ValidateTargetReaction(ValidPolicy, Errors));

	FTargetReaction NegativeLaunches = ValidPolicy;
	NegativeLaunches.MaxLaunchesPerAirCycle = -1;
	TestFalse(TEXT("a negative MaxLaunchesPerAirCycle is rejected"), ValidateTargetReaction(NegativeLaunches, Errors));
	TestTrue(TEXT("the negative launches error names MaxLaunchesPerAirCycle"), Errors.Contains(TEXT("MaxLaunchesPerAirCycle")));

	FTargetReaction NoScales = ValidPolicy;
	NoScales.LaunchZScales.Reset();
	TestFalse(TEXT("an empty LaunchZScales array is rejected"), ValidateTargetReaction(NoScales, Errors));
	TestTrue(TEXT("the empty scales error names LaunchZScales"), Errors.Contains(TEXT("LaunchZScales")));

	FTargetReaction ZeroScale = ValidPolicy;
	ZeroScale.LaunchZScales = {1.0f, 0.0f};
	TestFalse(TEXT("a zero launch scale is rejected"), ValidateTargetReaction(ZeroScale, Errors));

	FTargetReaction NanScale = ValidPolicy;
	NanScale.LaunchZScales = {std::numeric_limits<float>::quiet_NaN()};
	TestFalse(TEXT("a NaN launch scale is rejected"), ValidateTargetReaction(NanScale, Errors));

	FTargetReaction ZeroAirTime = ValidPolicy;
	ZeroAirTime.MaxAirTimeSeconds = 0.0f;
	TestFalse(TEXT("a zero MaxAirTimeSeconds is rejected (it must be a positive bound)"), ValidateTargetReaction(ZeroAirTime, Errors));
	TestTrue(TEXT("the zero air time error names MaxAirTimeSeconds"), Errors.Contains(TEXT("MaxAirTimeSeconds")));

	FTargetReaction NegativeAirTime = ValidPolicy;
	NegativeAirTime.MaxAirTimeSeconds = -2.5f;
	TestFalse(TEXT("a negative MaxAirTimeSeconds is rejected"), ValidateTargetReaction(NegativeAirTime, Errors));

	FTargetReaction InfiniteAirTime = ValidPolicy;
	InfiniteAirTime.MaxAirTimeSeconds = std::numeric_limits<float>::infinity();
	TestFalse(TEXT("an infinite MaxAirTimeSeconds is rejected"), ValidateTargetReaction(InfiniteAirTime, Errors));

	FTargetReaction NegativePoise = ValidPolicy;
	NegativePoise.PoiseMax = -1.0f;
	TestFalse(TEXT("a negative PoiseMax is rejected"), ValidateTargetReaction(NegativePoise, Errors));
	TestTrue(TEXT("the negative poise error names PoiseMax"), Errors.Contains(TEXT("PoiseMax")));

	FTargetReaction NegativeRegen = ValidPolicy;
	NegativeRegen.PoiseRegenSeconds = -1.0f;
	TestFalse(TEXT("a negative PoiseRegenSeconds is rejected"), ValidateTargetReaction(NegativeRegen, Errors));
	TestTrue(TEXT("the negative regen error names PoiseRegenSeconds"), Errors.Contains(TEXT("PoiseRegenSeconds")));

	FTargetReaction NegativeKnockdown = ValidPolicy;
	NegativeKnockdown.KnockdownSeconds = -0.45f;
	TestFalse(TEXT("a negative KnockdownSeconds is rejected"), ValidateTargetReaction(NegativeKnockdown, Errors));
	TestTrue(TEXT("the negative knockdown error names KnockdownSeconds"), Errors.Contains(TEXT("KnockdownSeconds")));

	FTargetReaction NegativeRecovering = ValidPolicy;
	NegativeRecovering.RecoveringSeconds = -0.25f;
	TestFalse(TEXT("a negative RecoveringSeconds is rejected"), ValidateTargetReaction(NegativeRecovering, Errors));
	TestTrue(TEXT("the negative recovering error names RecoveringSeconds"), Errors.Contains(TEXT("RecoveringSeconds")));

	FTargetReaction EmptyPolicyId = ValidPolicy;
	EmptyPolicyId.PolicyId = NAME_None;
	TestFalse(TEXT("an empty PolicyId is rejected"), ValidateTargetReaction(EmptyPolicyId, Errors));
	TestTrue(TEXT("the empty policy id error names PolicyId"), Errors.Contains(TEXT("PolicyId")));

	// Several problems at once are collected and joined with "; ".
	FTargetReaction Broken = ValidPolicy;
	Broken.KnockdownSeconds = -1.0f;
	Broken.RecoveringSeconds = -1.0f;
	TestFalse(TEXT("a policy with multiple problems is rejected"), ValidateTargetReaction(Broken, Errors));
	TestTrue(TEXT("KnockdownSeconds problem is included"), Errors.Contains(TEXT("KnockdownSeconds")));
	TestTrue(TEXT("RecoveringSeconds problem is included"), Errors.Contains(TEXT("RecoveringSeconds")));
	TestTrue(TEXT("problems are joined with \"; \""), Errors.Contains(TEXT("; ")));
	return true;
}

// ---------------------------------------------------------------------------
// ImmunityAndPenetrationAxesAreIndependent
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_003ImmunityAndPenetrationAxesAreIndependent,
	"UEMMO.Tasks.M5_003.ImmunityAndPenetrationAxesAreIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_003ImmunityAndPenetrationAxesAreIndependent::RunTest(const FString& Parameters)
{
	// The frozen penetration ordinals (append only, never renumber).
	TestEqual(TEXT("EControlPenetration::None is 0"), static_cast<uint8>(EControlPenetration::None), 0);
	TestEqual(TEXT("EControlPenetration::BypassPoise is 1"), static_cast<uint8>(EControlPenetration::BypassPoise), 1);

	// Damage immunity and control immunity are separate flags: each one can
	// be set alone, so no target can be forced into a blended "immune".
	FTargetReaction DamageImmuneOnly;
	DamageImmuneOnly.PolicyId = TEXT("probe_damage_immune");
	DamageImmuneOnly.bImmuneDamage = true;
	FString Errors;
	TestTrue(TEXT("damage immunity without control immunity passes validation"), ValidateTargetReaction(DamageImmuneOnly, Errors));
	TestTrue(TEXT("the damage-immune-only policy reports no errors"), Errors.IsEmpty());
	TestFalse(TEXT("damage immunity alone does not touch the control flag"), DamageImmuneOnly.bImmuneControl);

	FTargetReaction ControlImmuneOnly;
	ControlImmuneOnly.PolicyId = TEXT("probe_control_immune");
	ControlImmuneOnly.bImmuneControl = true;
	TestTrue(TEXT("control immunity without damage immunity passes validation"), ValidateTargetReaction(ControlImmuneOnly, Errors));
	TestFalse(TEXT("control immunity alone does not touch the damage flag"), ControlImmuneOnly.bImmuneDamage);

	// Poise and death resistance are separate axes as well.
	FTargetReaction DeathResistantNoPoise;
	DeathResistantNoPoise.PolicyId = TEXT("probe_death_resistant");
	DeathResistantNoPoise.bDeathResistant = true;
	TestTrue(TEXT("death resistance without a poise pool passes validation"), ValidateTargetReaction(DeathResistantNoPoise, Errors));
	TestEqual(TEXT("death resistance alone keeps PoiseMax at 0"), DeathResistantNoPoise.PoiseMax, 0.0f);
	TestFalse(TEXT("death resistance alone does not touch control immunity"), DeathResistantNoPoise.bImmuneControl);

	FTargetReaction PoiseOnly;
	PoiseOnly.PolicyId = TEXT("probe_poise_only");
	PoiseOnly.PoiseMax = 100.0f;
	PoiseOnly.PoiseRegenSeconds = 2.0f;
	TestTrue(TEXT("a poise pool without death resistance passes validation"), ValidateTargetReaction(PoiseOnly, Errors));
	TestFalse(TEXT("a poise pool alone does not touch death resistance"), PoiseOnly.bDeathResistant);

	// The explicit allow gates are independent of the poise pool: a gate can
	// exist without poise and poise can exist without gates. BypassPoise
	// skips the poise threshold only - it can never bypass these gates or a
	// get-up immunity window, so the gates must stay their own booleans.
	FTargetReaction GateWithoutPoise;
	GateWithoutPoise.PolicyId = TEXT("probe_gate_no_poise");
	GateWithoutPoise.bAllowLaunch = false;
	TestTrue(TEXT("a launch gate without a poise pool passes validation"), ValidateTargetReaction(GateWithoutPoise, Errors));
	TestEqual(TEXT("the gate-only policy keeps PoiseMax at 0"), GateWithoutPoise.PoiseMax, 0.0f);

	FTargetReaction PoiseWithoutGates;
	PoiseWithoutGates.PolicyId = TEXT("probe_poise_no_gates");
	PoiseWithoutGates.PoiseMax = 100.0f;
	TestTrue(TEXT("a poise pool without any gate passes validation"), ValidateTargetReaction(PoiseWithoutGates, Errors));
	TestTrue(TEXT("the poise-only policy keeps every allow gate open"), PoiseWithoutGates.bAllowLaunch && PoiseWithoutGates.bAllowKnockdown && PoiseWithoutGates.bAllowStagger);
	return true;
}

#endif
