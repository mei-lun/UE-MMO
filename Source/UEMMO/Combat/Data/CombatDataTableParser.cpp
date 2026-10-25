// M5-005: implementation of the combat source-table parser (see
// CombatDataTableParser.h for the contract). Pure C++ on Core types only:
// no UObject, no World, no engine Json module (its import library is not on
// the UEMMO link line, so the hand-rolled parser below is the shared
// definition site for the whole module).

#include "CombatDataTableParser.h"

#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

// ===========================================================================
// Hand-rolled JSON parser (RFC 8259 subset) with duplicate-key and
// non-finite-number rejection on top of the grammar.
// ===========================================================================

namespace
{
	class FCombatJsonScanner
	{
	public:
		explicit FCombatJsonScanner(const FString& InSource)
			: Source(InSource)
			, Position(0)
		{
		}

		bool ParseDocument(TSharedPtr<FCombatJsonValue>& OutRoot, FString& OutError)
		{
			SkipWhitespace();
			TSharedPtr<FCombatJsonValue> Root = ParseValue(OutError);
			if (!Root.IsValid())
			{
				return false;
			}
			SkipWhitespace();
			if (!AtEnd())
			{
				OutError = FString::Printf(TEXT("unexpected trailing characters after the JSON value at offset %d"), Position);
				return false;
			}
			OutRoot = Root;
			return true;
		}

	private:
		const FString& Source;
		int32 Position;

		/**
		 * The object member currently being parsed, for value-level error
		 * attribution; empty outside object members. A bad number is a JSON
		 * level failure, but naming the pending field keeps the error useful
		 * (M5-005: errors name the field).
		 */
		FString CurrentKey;

		FString FormatCurrentKeySuffix() const
		{
			return CurrentKey.IsEmpty() ? FString() : FString::Printf(TEXT(" for field '%s'"), *CurrentKey);
		}

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

		TSharedPtr<FCombatJsonValue> ParseValue(FString& OutError)
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

		TSharedPtr<FCombatJsonValue> ParseObject(FString& OutError)
		{
			if (!Expect(TEXT('{'), TEXT("to start an object"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FCombatJsonValue> Value = MakeShared<FCombatJsonValue>();
			Value->Kind = FCombatJsonValue::EKind::Object;
			SkipWhitespace();
			if (Consume(TEXT('}')))
			{
				return Value;
			}
			while (true)
			{
				SkipWhitespace();
				TSharedPtr<FCombatJsonValue> Key = ParseString(OutError);
				if (!Key.IsValid())
				{
					return nullptr;
				}
				SkipWhitespace();
				if (!Expect(TEXT(':'), TEXT("between object key and value"), OutError))
				{
					return nullptr;
				}
				TSharedPtr<FCombatJsonValue> Element;
				{
					const FString PreviousKey = CurrentKey;
					CurrentKey = Key->String;
					Element = ParseValue(OutError);
					CurrentKey = PreviousKey;
				}
				if (!Element.IsValid())
				{
					return nullptr;
				}
				// M5-005: duplicate keys inside one object are refused, never
				// silently overwritten (the last one would win invisibly).
				if (Value->Object.Contains(Key->String))
				{
					OutError = FString::Printf(TEXT("duplicate key '%s' at offset %d"), *Key->String, Position);
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

		TSharedPtr<FCombatJsonValue> ParseArray(FString& OutError)
		{
			if (!Expect(TEXT('['), TEXT("to start an array"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FCombatJsonValue> Value = MakeShared<FCombatJsonValue>();
			Value->Kind = FCombatJsonValue::EKind::Array;
			SkipWhitespace();
			if (Consume(TEXT(']')))
			{
				return Value;
			}
			while (true)
			{
				TSharedPtr<FCombatJsonValue> Element = ParseValue(OutError);
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

		TSharedPtr<FCombatJsonValue> ParseString(FString& OutError)
		{
			if (!Expect(TEXT('"'), TEXT("to start a string"), OutError))
			{
				return nullptr;
			}
			const TSharedPtr<FCombatJsonValue> Value = MakeShared<FCombatJsonValue>();
			Value->Kind = FCombatJsonValue::EKind::String;
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

		TSharedPtr<FCombatJsonValue> ParseBoolean(FString& OutError)
		{
			const TSharedPtr<FCombatJsonValue> Value = MakeShared<FCombatJsonValue>();
			if (MatchLiteral(TEXT("true"), 4))
			{
				Value->Kind = FCombatJsonValue::EKind::Boolean;
				Value->Boolean = true;
				Position += 4;
				return Value;
			}
			if (MatchLiteral(TEXT("false"), 5))
			{
				Value->Kind = FCombatJsonValue::EKind::Boolean;
				Value->Boolean = false;
				Position += 5;
				return Value;
			}
			OutError = FString::Printf(TEXT("invalid literal at offset %d"), Position);
			return nullptr;
		}

		TSharedPtr<FCombatJsonValue> ParseNull(FString& OutError)
		{
			if (MatchLiteral(TEXT("null"), 4))
			{
				const TSharedPtr<FCombatJsonValue> Value = MakeShared<FCombatJsonValue>();
				Value->Kind = FCombatJsonValue::EKind::Null;
				Position += 4;
				return Value;
			}
			OutError = FString::Printf(TEXT("invalid literal at offset %d"), Position);
			return nullptr;
		}

		TSharedPtr<FCombatJsonValue> ParseNumber(FString& OutError)
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
				OutError = FString::Printf(TEXT("invalid number at offset %d%s"), Start, *FormatCurrentKeySuffix());
				return nullptr;
			}
			// An overflowing literal decodes to a non-finite double; refusing it
			// here keeps every downstream value finite by construction.
			const double Decoded = FCString::Atod(*Source.Mid(Start, Position - Start));
			if (!FMath::IsFinite(Decoded))
			{
				OutError = FString::Printf(TEXT("the number at offset %d%s is not finite"), Start, *FormatCurrentKeySuffix());
				return nullptr;
			}
			const TSharedPtr<FCombatJsonValue> Value = MakeShared<FCombatJsonValue>();
			Value->Kind = FCombatJsonValue::EKind::Number;
			Value->Number = Decoded;
			return Value;
		}
	};
}

bool FCombatJsonParser::Parse(const FString& Text, TSharedPtr<FCombatJsonValue>& OutRoot, FString& OutError)
{
	FCombatJsonScanner Scanner(Text);
	return Scanner.ParseDocument(OutRoot, OutError);
}

// ===========================================================================
// Field extraction helpers
// ===========================================================================

namespace
{
	using FProblemList = TArray<FString>;

	FString GetJsonKindName(FCombatJsonValue::EKind Kind)
	{
		switch (Kind)
		{
		case FCombatJsonValue::EKind::Null: return TEXT("null");
		case FCombatJsonValue::EKind::Boolean: return TEXT("a boolean");
		case FCombatJsonValue::EKind::Number: return TEXT("a number");
		case FCombatJsonValue::EKind::String: return TEXT("a string");
		case FCombatJsonValue::EKind::Array: return TEXT("an array");
		case FCombatJsonValue::EKind::Object: return TEXT("an object");
		default: return TEXT("an unknown value");
		}
	}

	/** Returns the field only when present with exactly the requested JSON kind. */
	const TSharedPtr<FCombatJsonValue>* FindTyped(const FCombatJsonValue& Object, const TCHAR* Name, FCombatJsonValue::EKind Kind)
	{
		const TSharedPtr<FCombatJsonValue>* Value = Object.Object.Find(Name);
		if (!Value || !Value->IsValid() || (*Value)->Kind != Kind)
		{
			return nullptr;
		}
		return Value;
	}

	/** Fails the row when the object carries a key outside the known set (strict schema). */
	void RequireOnlyKnownFields(const FCombatJsonValue& Object, const TCHAR* const* KnownNames, int32 KnownCount, FProblemList& Problems)
	{
		for (const TPair<FString, TSharedPtr<FCombatJsonValue>>& Field : Object.Object)
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
				Problems.Add(FString::Printf(TEXT("unknown field '%s'"), *Field.Key));
			}
		}
	}

	void RequireString(const FCombatJsonValue& Object, const TCHAR* Name, FString& OutValue, FProblemList& Problems)
	{
		const TSharedPtr<FCombatJsonValue>* Value = Object.Object.Find(Name);
		if (!Value || !Value->IsValid())
		{
			Problems.Add(FString::Printf(TEXT("field '%s' must be a string (the field is missing)"), Name));
			return;
		}
		if ((*Value)->Kind != FCombatJsonValue::EKind::String)
		{
			Problems.Add(FString::Printf(TEXT("field '%s' must be a string (got %s)"), Name, *GetJsonKindName((*Value)->Kind)));
			return;
		}
		OutValue = (*Value)->String;
	}

	void RequireNumber(const FCombatJsonValue& Object, const TCHAR* Name, double& OutValue, FProblemList& Problems)
	{
		const TSharedPtr<FCombatJsonValue>* Value = Object.Object.Find(Name);
		if (!Value || !Value->IsValid())
		{
			Problems.Add(FString::Printf(TEXT("field '%s' must be a number (the field is missing)"), Name));
			return;
		}
		if ((*Value)->Kind != FCombatJsonValue::EKind::Number)
		{
			Problems.Add(FString::Printf(TEXT("field '%s' must be a number (got %s)"), Name, *GetJsonKindName((*Value)->Kind)));
			return;
		}
		OutValue = (*Value)->Number;
	}

	void RequireInteger(const FCombatJsonValue& Object, const TCHAR* Name, int32& OutValue, FProblemList& Problems)
	{
		const TSharedPtr<FCombatJsonValue>* Value = Object.Object.Find(Name);
		if (!Value || !Value->IsValid())
		{
			Problems.Add(FString::Printf(TEXT("field '%s' must be an integer (the field is missing)"), Name));
			return;
		}
		if ((*Value)->Kind != FCombatJsonValue::EKind::Number)
		{
			Problems.Add(FString::Printf(TEXT("field '%s' must be an integer (got %s)"), Name, *GetJsonKindName((*Value)->Kind)));
			return;
		}
		const double Rounded = FMath::RoundToZero((*Value)->Number);
		if (FMath::Abs((*Value)->Number - Rounded) > 1e-9 || FMath::Abs(Rounded) > 2147483647.0)
		{
			Problems.Add(FString::Printf(TEXT("field '%s' must be an integer (got %s)"), Name, *FString::SanitizeFloat((*Value)->Number)));
			return;
		}
		OutValue = static_cast<int32>(Rounded);
	}

	void RequireBool(const FCombatJsonValue& Object, const TCHAR* Name, bool& OutValue, FProblemList& Problems)
	{
		const TSharedPtr<FCombatJsonValue>* Value = Object.Object.Find(Name);
		if (!Value || !Value->IsValid())
		{
			Problems.Add(FString::Printf(TEXT("field '%s' must be a boolean (the field is missing)"), Name));
			return;
		}
		if ((*Value)->Kind != FCombatJsonValue::EKind::Boolean)
		{
			Problems.Add(FString::Printf(TEXT("field '%s' must be a boolean (got %s)"), Name, *GetJsonKindName((*Value)->Kind)));
			return;
		}
		OutValue = (*Value)->Boolean;
	}

	void RequireNumberArray(const FCombatJsonValue& Object, const TCHAR* Name, TArray<float>& OutValues, FProblemList& Problems)
	{
		OutValues.Reset();
		const TSharedPtr<FCombatJsonValue>* Value = Object.Object.Find(Name);
		if (!Value || !Value->IsValid())
		{
			Problems.Add(FString::Printf(TEXT("field '%s' must be an array of numbers (the field is missing)"), Name));
			return;
		}
		if ((*Value)->Kind != FCombatJsonValue::EKind::Array)
		{
			Problems.Add(FString::Printf(TEXT("field '%s' must be an array of numbers (got %s)"), Name, *GetJsonKindName((*Value)->Kind)));
			return;
		}
		for (int32 Index = 0; Index < (*Value)->Array.Num(); ++Index)
		{
			const TSharedPtr<FCombatJsonValue>& Element = (*Value)->Array[Index];
			if (!Element.IsValid() || Element->Kind != FCombatJsonValue::EKind::Number)
			{
				Problems.Add(FString::Printf(TEXT("field '%s[%d]' must be a number (got %s)"), Name, Index,
					Element.IsValid() ? *GetJsonKindName(Element->Kind) : TEXT("invalid")));
				continue;
			}
			OutValues.Add(static_cast<float>(Element->Number));
		}
	}

	void RequireStringArray(const FCombatJsonValue& Object, const TCHAR* Name, TArray<FString>& OutValues, FProblemList& Problems)
	{
		OutValues.Reset();
		const TSharedPtr<FCombatJsonValue>* Value = Object.Object.Find(Name);
		if (!Value || !Value->IsValid())
		{
			Problems.Add(FString::Printf(TEXT("field '%s' must be an array of strings (the field is missing)"), Name));
			return;
		}
		if ((*Value)->Kind != FCombatJsonValue::EKind::Array)
		{
			Problems.Add(FString::Printf(TEXT("field '%s' must be an array of strings (got %s)"), Name, *GetJsonKindName((*Value)->Kind)));
			return;
		}
		for (int32 Index = 0; Index < (*Value)->Array.Num(); ++Index)
		{
			const TSharedPtr<FCombatJsonValue>& Element = (*Value)->Array[Index];
			if (!Element.IsValid() || Element->Kind != FCombatJsonValue::EKind::String)
			{
				Problems.Add(FString::Printf(TEXT("field '%s[%d]' must be a string (got %s)"), Name, Index,
					Element.IsValid() ? *GetJsonKindName(Element->Kind) : TEXT("invalid")));
				continue;
			}
			OutValues.Add(Element->String);
		}
	}

	/**
	 * M5-037: requires one {x, y, z} object of numbers (centimeters; the
	 * room-local/vehicle-local offset shape the test room targets already
	 * use). Strict: the object may carry exactly x, y and z, and every value
	 * must be a finite number (the hand-rolled parser refuses non-finite
	 * literals already, so the value is finite by construction).
	 */
	void RequireVector3(const FCombatJsonValue& Object, const TCHAR* Name, FVector& OutValue, FProblemList& Problems)
	{
		OutValue = FVector::ZeroVector;
		const TSharedPtr<FCombatJsonValue>* Value = Object.Object.Find(Name);
		if (!Value || !Value->IsValid())
		{
			Problems.Add(FString::Printf(TEXT("field '%s' must be an object with numeric x/y/z (the field is missing)"), Name));
			return;
		}
		if ((*Value)->Kind != FCombatJsonValue::EKind::Object)
		{
			Problems.Add(FString::Printf(TEXT("field '%s' must be an object with numeric x/y/z (got %s)"), Name, *GetJsonKindName((*Value)->Kind)));
			return;
		}
		const FCombatJsonValue& VectorObject = **Value;
		static const TCHAR* const KnownFields[] = { TEXT("x"), TEXT("y"), TEXT("z") };
		for (const TPair<FString, TSharedPtr<FCombatJsonValue>>& Field : VectorObject.Object)
		{
			bool bKnown = false;
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(KnownFields); ++Index)
			{
				if (Field.Key.Equals(KnownFields[Index]))
				{
					bKnown = true;
					break;
				}
			}
			if (!bKnown)
			{
				Problems.Add(FString::Printf(TEXT("field '%s' carries the unknown member '%s' (only x/y/z are allowed)"), Name, *Field.Key));
			}
		}
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		RequireNumber(VectorObject, TEXT("x"), X, Problems);
		RequireNumber(VectorObject, TEXT("y"), Y, Problems);
		RequireNumber(VectorObject, TEXT("z"), Z, Problems);
		if (Problems.Num() == 0)
		{
			OutValue = FVector(X, Y, Z);
		}
	}

	FString JoinProblems(const FProblemList& Problems)
	{
		return FString::Join(Problems, TEXT("; "));
	}
}

// ===========================================================================
// Row parsers (value based)
// ===========================================================================

bool ParseDamageProfile(const FCombatJsonValue& Row, FDamageProfile& OutProfile, FString& OutError)
{
	OutProfile = FDamageProfile();
	OutError.Reset();
	if (Row.Kind != FCombatJsonValue::EKind::Object)
	{
		OutError = FString::Printf(TEXT("the row must be a JSON object (got %s)"), *GetJsonKindName(Row.Kind));
		return false;
	}
	static const TCHAR* const KnownFields[] = {
		TEXT("damage_profile_id"), TEXT("base_damage"), TEXT("attack_coefficient"),
		TEXT("hit_stun_s"), TEXT("knockback_cm_s"), TEXT("launch_cm_s"), TEXT("hit_stop_s")
	};
	FProblemList Problems;
	RequireOnlyKnownFields(Row, KnownFields, UE_ARRAY_COUNT(KnownFields), Problems);

	FString IdText;
	double BaseDamage = 0.0;
	double AttackCoefficient = 0.0;
	double HitStun = 0.0;
	double Knockback = 0.0;
	double Launch = 0.0;
	double HitStop = 0.0;
	RequireString(Row, TEXT("damage_profile_id"), IdText, Problems);
	RequireNumber(Row, TEXT("base_damage"), BaseDamage, Problems);
	RequireNumber(Row, TEXT("attack_coefficient"), AttackCoefficient, Problems);
	RequireNumber(Row, TEXT("hit_stun_s"), HitStun, Problems);
	RequireNumber(Row, TEXT("knockback_cm_s"), Knockback, Problems);
	RequireNumber(Row, TEXT("launch_cm_s"), Launch, Problems);
	RequireNumber(Row, TEXT("hit_stop_s"), HitStop, Problems);
	if (Problems.Num() > 0)
	{
		OutError = JoinProblems(Problems);
		return false;
	}

	// M5-036: the character-set contract is checked on the RAW id string
	// before the FName conversion. The packaged game build compiles without
	// case-preserving FNames, so FName(*IdText) can adopt the name pool's
	// existing display entry (the engine registers 'Normal' early) and a
	// legal lowercase 'normal' would fail any FName-based syntax check
	// in-package. The error text matches the validator's wording so parser
	// tests can pin the same field names.
	if (!IsValidDefinitionIdSyntax(IdText))
	{
		OutError = FString::Printf(TEXT("DamageProfileId '%s' must be 1..64 characters of a-z, 0-9 and _"), *IdText);
		return false;
	}

	OutProfile.DamageProfileId = FName(*IdText);
	OutProfile.BaseDamage = static_cast<float>(BaseDamage);
	OutProfile.AttackCoefficient = static_cast<float>(AttackCoefficient);
	OutProfile.HitStunSeconds = static_cast<float>(HitStun);
	OutProfile.KnockbackCmPerSecond = static_cast<float>(Knockback);
	OutProfile.LaunchCmPerSecond = static_cast<float>(Launch);
	OutProfile.HitStopSeconds = static_cast<float>(HitStop);

	FString ValidationErrors;
	if (!ValidateDamageProfile(OutProfile, ValidationErrors))
	{
		OutProfile = FDamageProfile();
		OutError = ValidationErrors;
		return false;
	}
	return true;
}

bool ParseAttackReaction(const FCombatJsonValue& Row, FAttackReaction& OutReaction, FString& OutError)
{
	OutReaction = FAttackReaction();
	OutError.Reset();
	if (Row.Kind != FCombatJsonValue::EKind::Object)
	{
		OutError = FString::Printf(TEXT("the row must be a JSON object (got %s)"), *GetJsonKindName(Row.Kind));
		return false;
	}
	static const TCHAR* const KnownFields[] = { TEXT("reaction_id"), TEXT("control_penetration") };
	FProblemList Problems;
	RequireOnlyKnownFields(Row, KnownFields, UE_ARRAY_COUNT(KnownFields), Problems);

	FString IdText;
	FString PenetrationText;
	RequireString(Row, TEXT("reaction_id"), IdText, Problems);
	RequireString(Row, TEXT("control_penetration"), PenetrationText, Problems);
	if (Problems.Num() > 0)
	{
		OutError = JoinProblems(Problems);
		return false;
	}

	// M5-036: raw-string syntax before the FName conversion (see
	// ParseDamageProfile for the in-package FName display caveat).
	if (!IsValidDefinitionIdSyntax(IdText))
	{
		OutError = FString::Printf(TEXT("ReactionId '%s' must be 1..64 characters of a-z, 0-9 and _"), *IdText);
		return false;
	}

	OutReaction.ReactionId = FName(*IdText);
	if (!ParseControlPenetration(PenetrationText, OutReaction.ControlPenetration))
	{
		OutError = FString::Printf(TEXT("field 'control_penetration' must be one of none/bypass_poise (got '%s')"), *PenetrationText);
		OutReaction = FAttackReaction();
		return false;
	}

	FString ValidationErrors;
	if (!ValidateAttackReaction(OutReaction, ValidationErrors))
	{
		OutReaction = FAttackReaction();
		OutError = ValidationErrors;
		return false;
	}
	return true;
}

bool ParseTargetReaction(const FCombatJsonValue& Row, FTargetReaction& OutPolicy, FString& OutError)
{
	OutPolicy = FTargetReaction();
	OutError.Reset();
	if (Row.Kind != FCombatJsonValue::EKind::Object)
	{
		OutError = FString::Printf(TEXT("the row must be a JSON object (got %s)"), *GetJsonKindName(Row.Kind));
		return false;
	}
	static const TCHAR* const KnownFields[] = {
		TEXT("policy_id"), TEXT("allow_stagger"), TEXT("allow_launch"), TEXT("allow_knockdown"),
		TEXT("max_launches_per_air_cycle"), TEXT("launch_z_scales"), TEXT("max_air_time_s"),
		TEXT("poise_max"), TEXT("poise_regen_s"), TEXT("knockdown_s"), TEXT("recovering_s"),
		TEXT("immune_damage"), TEXT("immune_control"), TEXT("death_resistant")
	};
	FProblemList Problems;
	RequireOnlyKnownFields(Row, KnownFields, UE_ARRAY_COUNT(KnownFields), Problems);

	FString IdText;
	bool bAllowStagger = false;
	bool bAllowLaunch = false;
	bool bAllowKnockdown = false;
	int32 MaxLaunches = 0;
	TArray<float> LaunchZScales;
	double MaxAirTime = 0.0;
	double PoiseMax = 0.0;
	double PoiseRegen = 0.0;
	double Knockdown = 0.0;
	double Recovering = 0.0;
	bool bImmuneDamage = false;
	bool bImmuneControl = false;
	bool bDeathResistant = false;
	RequireString(Row, TEXT("policy_id"), IdText, Problems);
	RequireBool(Row, TEXT("allow_stagger"), bAllowStagger, Problems);
	RequireBool(Row, TEXT("allow_launch"), bAllowLaunch, Problems);
	RequireBool(Row, TEXT("allow_knockdown"), bAllowKnockdown, Problems);
	RequireInteger(Row, TEXT("max_launches_per_air_cycle"), MaxLaunches, Problems);
	RequireNumberArray(Row, TEXT("launch_z_scales"), LaunchZScales, Problems);
	RequireNumber(Row, TEXT("max_air_time_s"), MaxAirTime, Problems);
	RequireNumber(Row, TEXT("poise_max"), PoiseMax, Problems);
	RequireNumber(Row, TEXT("poise_regen_s"), PoiseRegen, Problems);
	RequireNumber(Row, TEXT("knockdown_s"), Knockdown, Problems);
	RequireNumber(Row, TEXT("recovering_s"), Recovering, Problems);
	RequireBool(Row, TEXT("immune_damage"), bImmuneDamage, Problems);
	RequireBool(Row, TEXT("immune_control"), bImmuneControl, Problems);
	RequireBool(Row, TEXT("death_resistant"), bDeathResistant, Problems);
	if (Problems.Num() > 0)
	{
		OutError = JoinProblems(Problems);
		return false;
	}

	// M5-036: raw-string syntax before the FName conversion (see
	// ParseDamageProfile for the in-package FName display caveat; the shipped
	// target_reactions 'normal' row hits the pooled 'Normal' display in the
	// packaged game build).
	if (!IsValidDefinitionIdSyntax(IdText))
	{
		OutError = FString::Printf(TEXT("PolicyId '%s' must be 1..64 characters of a-z, 0-9 and _"), *IdText);
		return false;
	}

	OutPolicy.PolicyId = FName(*IdText);
	OutPolicy.bAllowStagger = bAllowStagger;
	OutPolicy.bAllowLaunch = bAllowLaunch;
	OutPolicy.bAllowKnockdown = bAllowKnockdown;
	OutPolicy.MaxLaunchesPerAirCycle = MaxLaunches;
	OutPolicy.LaunchZScales = LaunchZScales;
	OutPolicy.MaxAirTimeSeconds = static_cast<float>(MaxAirTime);
	OutPolicy.PoiseMax = static_cast<float>(PoiseMax);
	OutPolicy.PoiseRegenSeconds = static_cast<float>(PoiseRegen);
	OutPolicy.KnockdownSeconds = static_cast<float>(Knockdown);
	OutPolicy.RecoveringSeconds = static_cast<float>(Recovering);
	OutPolicy.bImmuneDamage = bImmuneDamage;
	OutPolicy.bImmuneControl = bImmuneControl;
	OutPolicy.bDeathResistant = bDeathResistant;

	FString ValidationErrors;
	if (!ValidateTargetReaction(OutPolicy, ValidationErrors))
	{
		OutPolicy = FTargetReaction();
		OutError = ValidationErrors;
		return false;
	}
	return true;
}

bool ParsePresentation(const FCombatJsonValue& Row, FCombatPresentation& OutPresentation, FString& OutError)
{
	OutPresentation = FCombatPresentation();
	OutError.Reset();
	if (Row.Kind != FCombatJsonValue::EKind::Object)
	{
		OutError = FString::Printf(TEXT("the row must be a JSON object (got %s)"), *GetJsonKindName(Row.Kind));
		return false;
	}
	static const TCHAR* const KnownFields[] = {
		TEXT("presentation_id"), TEXT("reaction_id"), TEXT("montage_path"),
		TEXT("sound_path"), TEXT("effect_path"), TEXT("placeholder")
	};
	FProblemList Problems;
	RequireOnlyKnownFields(Row, KnownFields, UE_ARRAY_COUNT(KnownFields), Problems);

	FString IdText;
	FString ReactionText;
	FString MontagePath;
	FString SoundPath;
	FString EffectPath;
	bool bPlaceholder = true;
	RequireString(Row, TEXT("presentation_id"), IdText, Problems);
	RequireString(Row, TEXT("reaction_id"), ReactionText, Problems);
	RequireString(Row, TEXT("montage_path"), MontagePath, Problems);
	RequireString(Row, TEXT("sound_path"), SoundPath, Problems);
	RequireString(Row, TEXT("effect_path"), EffectPath, Problems);
	RequireBool(Row, TEXT("placeholder"), bPlaceholder, Problems);
	if (Problems.Num() > 0)
	{
		OutError = JoinProblems(Problems);
		return false;
	}

	OutPresentation.PresentationId = FName(*IdText);
	OutPresentation.ReactionId = FName(*ReactionText);
	OutPresentation.MontagePath = MontagePath;
	OutPresentation.SoundPath = SoundPath;
	OutPresentation.EffectPath = EffectPath;
	OutPresentation.bPlaceholder = bPlaceholder;

	// Local value rules: business id syntax and the placeholder/path contract.
	if (!IsValidDefinitionIdSyntax(IdText))
	{
		Problems.Add(FString::Printf(TEXT("field 'presentation_id' must be 1..64 characters of a-z, 0-9 and _ (got '%s')"), *IdText));
	}
	if (!IsValidDefinitionIdSyntax(ReactionText))
	{
		Problems.Add(FString::Printf(TEXT("field 'reaction_id' must be 1..64 characters of a-z, 0-9 and _ (got '%s')"), *ReactionText));
	}
	if (!bPlaceholder && MontagePath.IsEmpty() && SoundPath.IsEmpty() && EffectPath.IsEmpty())
	{
		Problems.Add(TEXT("field 'placeholder': a non-placeholder presentation must declare at least one non-empty resource path (montage_path, sound_path or effect_path)"));
	}
	if (Problems.Num() > 0)
	{
		OutPresentation = FCombatPresentation();
		OutError = JoinProblems(Problems);
		return false;
	}
	return true;
}

bool ParseWeaponDefinition(const FCombatJsonValue& Row, FWeaponDefinition& OutWeapon, FString& OutError)
{
	OutWeapon = FWeaponDefinition();
	OutError.Reset();
	if (Row.Kind != FCombatJsonValue::EKind::Object)
	{
		OutError = FString::Printf(TEXT("the row must be a JSON object (got %s)"), *GetJsonKindName(Row.Kind));
		return false;
	}
	static const TCHAR* const KnownFields[] = {
		TEXT("weapon_id"), TEXT("mode"), TEXT("damage_profile_id"), TEXT("ammo_id"),
		TEXT("magazine_size"), TEXT("fire_rate_rpm"), TEXT("burst_count"), TEXT("pellet_count"),
		TEXT("spread_degrees_deg"), TEXT("range_cm"), TEXT("projectile_id"), TEXT("melee_attack_ids")
	};
	FProblemList Problems;
	RequireOnlyKnownFields(Row, KnownFields, UE_ARRAY_COUNT(KnownFields), Problems);

	FString IdText;
	FString ModeText;
	FString DamageProfileText;
	FString AmmoText;
	int32 MagazineSize = 0;
	double FireRateRpm = 0.0;
	int32 BurstCount = 1;
	int32 PelletCount = 1;
	double SpreadDegrees = 0.0;
	double RangeCm = 0.0;
	FString ProjectileText;
	TArray<FString> MeleeAttackIds;
	RequireString(Row, TEXT("weapon_id"), IdText, Problems);
	RequireString(Row, TEXT("mode"), ModeText, Problems);
	RequireString(Row, TEXT("damage_profile_id"), DamageProfileText, Problems);
	RequireString(Row, TEXT("ammo_id"), AmmoText, Problems);
	RequireInteger(Row, TEXT("magazine_size"), MagazineSize, Problems);
	RequireNumber(Row, TEXT("fire_rate_rpm"), FireRateRpm, Problems);
	RequireInteger(Row, TEXT("burst_count"), BurstCount, Problems);
	RequireInteger(Row, TEXT("pellet_count"), PelletCount, Problems);
	RequireNumber(Row, TEXT("spread_degrees_deg"), SpreadDegrees, Problems);
	RequireNumber(Row, TEXT("range_cm"), RangeCm, Problems);
	RequireString(Row, TEXT("projectile_id"), ProjectileText, Problems);
	RequireStringArray(Row, TEXT("melee_attack_ids"), MeleeAttackIds, Problems);
	if (Problems.Num() > 0)
	{
		OutError = JoinProblems(Problems);
		return false;
	}

	OutWeapon.WeaponId = FName(*IdText);
	if (!ParseWeaponFireMode(ModeText, OutWeapon.FireMode))
	{
		Problems.Add(FString::Printf(TEXT("field 'mode' must be one of melee/hitscan/projectile (got '%s')"), *ModeText));
		OutError = JoinProblems(Problems);
		return false;
	}
	OutWeapon.DamageProfileId = FName(*DamageProfileText);
	OutWeapon.AmmoId = FName(*AmmoText);
	OutWeapon.MagazineSize = MagazineSize;
	OutWeapon.FireRateRpm = static_cast<float>(FireRateRpm);
	OutWeapon.BurstCount = BurstCount;
	OutWeapon.PelletCount = PelletCount;
	OutWeapon.SpreadDegrees = static_cast<float>(SpreadDegrees);
	OutWeapon.RangeCm = static_cast<float>(RangeCm);
	OutWeapon.ProjectileId = FName(*ProjectileText);
	OutWeapon.MeleeAttackIds.Reset();
	for (const FString& AttackId : MeleeAttackIds)
	{
		OutWeapon.MeleeAttackIds.Add(FName(*AttackId));
	}

	FString ValidationErrors;
	if (!ValidateWeaponDefinition(OutWeapon, ValidationErrors))
	{
		OutWeapon = FWeaponDefinition();
		OutError = ValidationErrors;
		return false;
	}
	return true;
}

bool ParseAmmoType(const FCombatJsonValue& Row, FAmmoType& OutAmmo, FString& OutError)
{
	OutAmmo = FAmmoType();
	OutError.Reset();
	if (Row.Kind != FCombatJsonValue::EKind::Object)
	{
		OutError = FString::Printf(TEXT("the row must be a JSON object (got %s)"), *GetJsonKindName(Row.Kind));
		return false;
	}
	static const TCHAR* const KnownFields[] = { TEXT("ammo_id"), TEXT("max_reserve"), TEXT("magazine_size") };
	FProblemList Problems;
	RequireOnlyKnownFields(Row, KnownFields, UE_ARRAY_COUNT(KnownFields), Problems);

	FString IdText;
	int32 MaxReserve = 0;
	int32 MagazineSize = 1;
	RequireString(Row, TEXT("ammo_id"), IdText, Problems);
	RequireInteger(Row, TEXT("max_reserve"), MaxReserve, Problems);
	RequireInteger(Row, TEXT("magazine_size"), MagazineSize, Problems);
	if (Problems.Num() > 0)
	{
		OutError = JoinProblems(Problems);
		return false;
	}

	OutAmmo.AmmoId = FName(*IdText);
	OutAmmo.MaxReserve = MaxReserve;
	OutAmmo.MagazineSize = MagazineSize;

	FString ValidationErrors;
	if (!ValidateAmmoType(OutAmmo, ValidationErrors))
	{
		OutAmmo = FAmmoType();
		OutError = ValidationErrors;
		return false;
	}
	return true;
}

bool ParseProjectileDefinition(const FCombatJsonValue& Row, FProjectileDefinition& OutProjectile, FString& OutError)
{
	OutProjectile = FProjectileDefinition();
	OutError.Reset();
	if (Row.Kind != FCombatJsonValue::EKind::Object)
	{
		OutError = FString::Printf(TEXT("the row must be a JSON object (got %s)"), *GetJsonKindName(Row.Kind));
		return false;
	}
	static const TCHAR* const KnownFields[] = {
		TEXT("projectile_id"), TEXT("motion"), TEXT("speed_cm_s"), TEXT("lifetime_s"),
		TEXT("damage_profile_id"), TEXT("pierce_count"), TEXT("explosion_radius_cm"),
		TEXT("explosion_damage_profile_id"), TEXT("homing_turn_rate_deg_s")
	};
	FProblemList Problems;
	RequireOnlyKnownFields(Row, KnownFields, UE_ARRAY_COUNT(KnownFields), Problems);

	FString IdText;
	FString MotionText;
	double Speed = 0.0;
	double Lifetime = 0.0;
	FString DamageProfileText;
	int32 PierceCount = 0;
	double ExplosionRadius = 0.0;
	FString ExplosionDamageProfileText;
	double HomingTurnRate = 0.0;
	RequireString(Row, TEXT("projectile_id"), IdText, Problems);
	RequireString(Row, TEXT("motion"), MotionText, Problems);
	RequireNumber(Row, TEXT("speed_cm_s"), Speed, Problems);
	RequireNumber(Row, TEXT("lifetime_s"), Lifetime, Problems);
	RequireString(Row, TEXT("damage_profile_id"), DamageProfileText, Problems);
	RequireInteger(Row, TEXT("pierce_count"), PierceCount, Problems);
	RequireNumber(Row, TEXT("explosion_radius_cm"), ExplosionRadius, Problems);
	RequireString(Row, TEXT("explosion_damage_profile_id"), ExplosionDamageProfileText, Problems);
	RequireNumber(Row, TEXT("homing_turn_rate_deg_s"), HomingTurnRate, Problems);
	if (Problems.Num() > 0)
	{
		OutError = JoinProblems(Problems);
		return false;
	}

	OutProjectile.ProjectileId = FName(*IdText);
	if (!ParseProjectileMotion(MotionText, OutProjectile.Motion))
	{
		Problems.Add(FString::Printf(TEXT("field 'motion' must be one of straight/parabolic/homing (got '%s')"), *MotionText));
		OutError = JoinProblems(Problems);
		return false;
	}
	OutProjectile.SpeedCmS = static_cast<float>(Speed);
	OutProjectile.LifetimeS = static_cast<float>(Lifetime);
	OutProjectile.DamageProfileId = FName(*DamageProfileText);
	OutProjectile.PierceCount = PierceCount;
	OutProjectile.ExplosionRadiusCm = static_cast<float>(ExplosionRadius);
	OutProjectile.ExplosionDamageProfileId = FName(*ExplosionDamageProfileText);
	OutProjectile.HomingTurnRateDegS = static_cast<float>(HomingTurnRate);

	FString ValidationErrors;
	if (!ValidateProjectileDefinition(OutProjectile, ValidationErrors))
	{
		OutProjectile = FProjectileDefinition();
		OutError = ValidationErrors;
		return false;
	}
	return true;
}

bool ParseVehicleDefinition(const FCombatJsonValue& Row, FVehicleDefinition& OutVehicle, FString& OutError)
{
	OutVehicle = FVehicleDefinition();
	OutError.Reset();
	if (Row.Kind != FCombatJsonValue::EKind::Object)
	{
		OutError = FString::Printf(TEXT("the row must be a JSON object (got %s)"), *GetJsonKindName(Row.Kind));
		return false;
	}
	static const TCHAR* const KnownFields[] = {
		TEXT("vehicle_id"), TEXT("drive_type"), TEXT("max_health"), TEXT("defense"),
		TEXT("max_speed_cm_s"), TEXT("acceleration_cm_s2"), TEXT("mesh_path"),
		TEXT("camera_distance_cm"), TEXT("camera_height_cm"), TEXT("camera_pitch_deg")
	};
	FProblemList Problems;
	RequireOnlyKnownFields(Row, KnownFields, UE_ARRAY_COUNT(KnownFields), Problems);

	FString IdText;
	FString DriveTypeText;
	double MaxHealth = 0.0;
	double Defense = 0.0;
	double MaxSpeed = 0.0;
	double Acceleration = 0.0;
	FString MeshPath;
	double CameraDistance = 0.0;
	double CameraHeight = 0.0;
	double CameraPitch = 0.0;
	RequireString(Row, TEXT("vehicle_id"), IdText, Problems);
	RequireString(Row, TEXT("drive_type"), DriveTypeText, Problems);
	RequireNumber(Row, TEXT("max_health"), MaxHealth, Problems);
	RequireNumber(Row, TEXT("defense"), Defense, Problems);
	RequireNumber(Row, TEXT("max_speed_cm_s"), MaxSpeed, Problems);
	RequireNumber(Row, TEXT("acceleration_cm_s2"), Acceleration, Problems);
	RequireString(Row, TEXT("mesh_path"), MeshPath, Problems);
	RequireNumber(Row, TEXT("camera_distance_cm"), CameraDistance, Problems);
	RequireNumber(Row, TEXT("camera_height_cm"), CameraHeight, Problems);
	RequireNumber(Row, TEXT("camera_pitch_deg"), CameraPitch, Problems);
	if (Problems.Num() > 0)
	{
		OutError = JoinProblems(Problems);
		return false;
	}

	// M5-036: the character-set contract is checked on the RAW id string
	// before the FName conversion (see ParseDamageProfile for the in-package
	// FName display caveat).
	if (!IsValidDefinitionIdSyntax(IdText))
	{
		OutError = FString::Printf(TEXT("VehicleId '%s' must be 1..64 characters of a-z, 0-9 and _"), *IdText);
		return false;
	}

	OutVehicle.VehicleId = FName(*IdText);
	// Only "ground" exists: an aircraft/boat token must be reported as the
	// unsupported drive type it is, never mapped onto the ground value.
	if (!ParseVehicleDriveType(DriveTypeText, OutVehicle.DriveType))
	{
		OutError = FString::Printf(TEXT("field 'drive_type' must be 'ground' (the only implemented drive type; aircraft and boat drive types are not supported) (got '%s')"), *DriveTypeText);
		OutVehicle = FVehicleDefinition();
		return false;
	}
	OutVehicle.MaxHealth = static_cast<float>(MaxHealth);
	OutVehicle.Defense = static_cast<float>(Defense);
	OutVehicle.MaxSpeedCmS = static_cast<float>(MaxSpeed);
	OutVehicle.AccelerationCmS2 = static_cast<float>(Acceleration);
	OutVehicle.MeshPath = MeshPath;
	OutVehicle.CameraDistanceCm = static_cast<float>(CameraDistance);
	OutVehicle.CameraHeightCm = static_cast<float>(CameraHeight);
	OutVehicle.CameraPitchDeg = static_cast<float>(CameraPitch);

	// The mesh path is the hull resource reference: presence is the
	// source-table gate (the cook-list wiring belongs to the later cards).
	if (MeshPath.IsEmpty())
	{
		OutError = TEXT("field 'mesh_path' must be a non-empty soft object path (the hull mesh reference)");
		OutVehicle = FVehicleDefinition();
		return false;
	}

	FString ValidationErrors;
	if (!ValidateVehicleDefinition(OutVehicle, ValidationErrors))
	{
		OutVehicle = FVehicleDefinition();
		OutError = ValidationErrors;
		return false;
	}
	return true;
}

bool ParseVehicleSeat(const FCombatJsonValue& Row, FVehicleSeatDefinition& OutSeat, FString& OutError)
{
	OutSeat = FVehicleSeatDefinition();
	OutError.Reset();
	if (Row.Kind != FCombatJsonValue::EKind::Object)
	{
		OutError = FString::Printf(TEXT("the row must be a JSON object (got %s)"), *GetJsonKindName(Row.Kind));
		return false;
	}
	static const TCHAR* const KnownFields[] = {
		TEXT("seat_id"), TEXT("vehicle_id"), TEXT("seat_role"), TEXT("seat_location_cm"), TEXT("exit_offset_cm")
	};
	FProblemList Problems;
	RequireOnlyKnownFields(Row, KnownFields, UE_ARRAY_COUNT(KnownFields), Problems);

	FString IdText;
	FString VehicleIdText;
	FString SeatRoleText;
	FVector SeatLocation = FVector::ZeroVector;
	FVector ExitOffset = FVector::ZeroVector;
	RequireString(Row, TEXT("seat_id"), IdText, Problems);
	RequireString(Row, TEXT("vehicle_id"), VehicleIdText, Problems);
	RequireString(Row, TEXT("seat_role"), SeatRoleText, Problems);
	RequireVector3(Row, TEXT("seat_location_cm"), SeatLocation, Problems);
	RequireVector3(Row, TEXT("exit_offset_cm"), ExitOffset, Problems);
	if (Problems.Num() > 0)
	{
		OutError = JoinProblems(Problems);
		return false;
	}

	if (!IsValidDefinitionIdSyntax(IdText))
	{
		OutError = FString::Printf(TEXT("SeatId '%s' must be 1..64 characters of a-z, 0-9 and _"), *IdText);
		return false;
	}
	if (!IsValidDefinitionIdSyntax(VehicleIdText))
	{
		OutError = FString::Printf(TEXT("field 'vehicle_id' '%s' must be 1..64 characters of a-z, 0-9 and _"), *VehicleIdText);
		return false;
	}

	OutSeat.SeatId = FName(*IdText);
	OutSeat.VehicleId = FName(*VehicleIdText);
	// Only "driver" exists: a gunner/passenger token is the unsupported
	// multi-seat capability, reported as such instead of silently mapped.
	if (!ParseVehicleSeatRole(SeatRoleText, OutSeat.SeatRole))
	{
		OutError = FString::Printf(TEXT("field 'seat_role' must be 'driver' (the only implemented seat role; gunner and passenger roles of multi-seat layouts are not supported) (got '%s')"), *SeatRoleText);
		OutSeat = FVehicleSeatDefinition();
		return false;
	}
	OutSeat.SeatLocationCm = SeatLocation;
	OutSeat.ExitOffsetCm = ExitOffset;

	FString ValidationErrors;
	if (!ValidateVehicleSeatDefinition(OutSeat, ValidationErrors))
	{
		OutSeat = FVehicleSeatDefinition();
		OutError = ValidationErrors;
		return false;
	}
	return true;
}

bool ParseVehicleMount(const FCombatJsonValue& Row, FVehicleMountDefinition& OutMount, FString& OutError)
{
	OutMount = FVehicleMountDefinition();
	OutError.Reset();
	if (Row.Kind != FCombatJsonValue::EKind::Object)
	{
		OutError = FString::Printf(TEXT("the row must be a JSON object (got %s)"), *GetJsonKindName(Row.Kind));
		return false;
	}
	static const TCHAR* const KnownFields[] = {
		TEXT("mount_id"), TEXT("vehicle_id"), TEXT("turret_weapon_id"), TEXT("location_cm"),
		TEXT("pitch_min_deg"), TEXT("pitch_max_deg")
	};
	FProblemList Problems;
	RequireOnlyKnownFields(Row, KnownFields, UE_ARRAY_COUNT(KnownFields), Problems);

	FString IdText;
	FString VehicleIdText;
	FString WeaponIdText;
	FVector Location = FVector::ZeroVector;
	double PitchMin = 0.0;
	double PitchMax = 0.0;
	RequireString(Row, TEXT("mount_id"), IdText, Problems);
	RequireString(Row, TEXT("vehicle_id"), VehicleIdText, Problems);
	RequireString(Row, TEXT("turret_weapon_id"), WeaponIdText, Problems);
	RequireVector3(Row, TEXT("location_cm"), Location, Problems);
	RequireNumber(Row, TEXT("pitch_min_deg"), PitchMin, Problems);
	RequireNumber(Row, TEXT("pitch_max_deg"), PitchMax, Problems);
	if (Problems.Num() > 0)
	{
		OutError = JoinProblems(Problems);
		return false;
	}

	if (!IsValidDefinitionIdSyntax(IdText))
	{
		OutError = FString::Printf(TEXT("MountId '%s' must be 1..64 characters of a-z, 0-9 and _"), *IdText);
		return false;
	}
	if (!IsValidDefinitionIdSyntax(VehicleIdText))
	{
		OutError = FString::Printf(TEXT("field 'vehicle_id' '%s' must be 1..64 characters of a-z, 0-9 and _"), *VehicleIdText);
		return false;
	}
	if (!IsValidDefinitionIdSyntax(WeaponIdText))
	{
		OutError = FString::Printf(TEXT("field 'turret_weapon_id' '%s' must be 1..64 characters of a-z, 0-9 and _"), *WeaponIdText);
		return false;
	}

	OutMount.MountId = FName(*IdText);
	OutMount.VehicleId = FName(*VehicleIdText);
	OutMount.TurretWeaponId = FName(*WeaponIdText);
	OutMount.LocationCm = Location;
	OutMount.MinPitchDegrees = static_cast<float>(PitchMin);
	OutMount.MaxPitchDegrees = static_cast<float>(PitchMax);

	FString ValidationErrors;
	if (!ValidateVehicleMountDefinition(OutMount, ValidationErrors))
	{
		OutMount = FVehicleMountDefinition();
		OutError = ValidationErrors;
		return false;
	}
	return true;
}

bool ParseVehicleSpawnEntry(const FCombatJsonValue& Row, FVehicleSpawnEntry& OutSpawn, FString& OutError)
{
	OutSpawn = FVehicleSpawnEntry();
	OutError.Reset();
	if (Row.Kind != FCombatJsonValue::EKind::Object)
	{
		OutError = FString::Printf(TEXT("the row must be a JSON object (got %s)"), *GetJsonKindName(Row.Kind));
		return false;
	}
	static const TCHAR* const KnownFields[] = { TEXT("vehicle_id"), TEXT("location_cm") };
	FProblemList Problems;
	RequireOnlyKnownFields(Row, KnownFields, UE_ARRAY_COUNT(KnownFields), Problems);

	FString VehicleIdText;
	FVector Location = FVector::ZeroVector;
	RequireString(Row, TEXT("vehicle_id"), VehicleIdText, Problems);
	RequireVector3(Row, TEXT("location_cm"), Location, Problems);
	if (Problems.Num() > 0)
	{
		OutError = JoinProblems(Problems);
		return false;
	}

	if (!IsValidDefinitionIdSyntax(VehicleIdText))
	{
		OutError = FString::Printf(TEXT("VehicleId '%s' must be 1..64 characters of a-z, 0-9 and _"), *VehicleIdText);
		return false;
	}

	OutSpawn.VehicleId = FName(*VehicleIdText);
	OutSpawn.LocationCm = Location;

	FString ValidationErrors;
	if (!ValidateVehicleSpawnEntry(OutSpawn, ValidationErrors))
	{
		OutSpawn = FVehicleSpawnEntry();
		OutError = ValidationErrors;
		return false;
	}
	return true;
}

// ===========================================================================
// Row parsers (text convenience overloads)
// ===========================================================================

namespace
{
	/** Shared first step of the text overloads: parse the row text to an object. */
	bool ParseRowTextToObject(const FString& RowJsonText, TSharedPtr<FCombatJsonValue>& OutRow, FString& OutError)
	{
		if (!FCombatJsonParser::Parse(RowJsonText, OutRow, OutError))
		{
			OutError = FString::Printf(TEXT("invalid row JSON: %s"), *OutError);
			return false;
		}
		if (!OutRow.IsValid() || OutRow->Kind != FCombatJsonValue::EKind::Object)
		{
			OutError = TEXT("the row JSON must be an object");
			return false;
		}
		return true;
	}
}

bool ParseDamageProfile(const FString& RowJsonText, FDamageProfile& OutProfile, FString& OutError)
{
	OutProfile = FDamageProfile();
	TSharedPtr<FCombatJsonValue> Row;
	if (!ParseRowTextToObject(RowJsonText, Row, OutError))
	{
		return false;
	}
	return ParseDamageProfile(*Row, OutProfile, OutError);
}

bool ParseAttackReaction(const FString& RowJsonText, FAttackReaction& OutReaction, FString& OutError)
{
	OutReaction = FAttackReaction();
	TSharedPtr<FCombatJsonValue> Row;
	if (!ParseRowTextToObject(RowJsonText, Row, OutError))
	{
		return false;
	}
	return ParseAttackReaction(*Row, OutReaction, OutError);
}

bool ParseTargetReaction(const FString& RowJsonText, FTargetReaction& OutPolicy, FString& OutError)
{
	OutPolicy = FTargetReaction();
	TSharedPtr<FCombatJsonValue> Row;
	if (!ParseRowTextToObject(RowJsonText, Row, OutError))
	{
		return false;
	}
	return ParseTargetReaction(*Row, OutPolicy, OutError);
}

bool ParsePresentation(const FString& RowJsonText, FCombatPresentation& OutPresentation, FString& OutError)
{
	OutPresentation = FCombatPresentation();
	TSharedPtr<FCombatJsonValue> Row;
	if (!ParseRowTextToObject(RowJsonText, Row, OutError))
	{
		return false;
	}
	return ParsePresentation(*Row, OutPresentation, OutError);
}

bool ParseWeaponDefinition(const FString& RowJsonText, FWeaponDefinition& OutWeapon, FString& OutError)
{
	OutWeapon = FWeaponDefinition();
	TSharedPtr<FCombatJsonValue> Row;
	if (!ParseRowTextToObject(RowJsonText, Row, OutError))
	{
		return false;
	}
	return ParseWeaponDefinition(*Row, OutWeapon, OutError);
}

bool ParseAmmoType(const FString& RowJsonText, FAmmoType& OutAmmo, FString& OutError)
{
	OutAmmo = FAmmoType();
	TSharedPtr<FCombatJsonValue> Row;
	if (!ParseRowTextToObject(RowJsonText, Row, OutError))
	{
		return false;
	}
	return ParseAmmoType(*Row, OutAmmo, OutError);
}

bool ParseProjectileDefinition(const FString& RowJsonText, FProjectileDefinition& OutProjectile, FString& OutError)
{
	OutProjectile = FProjectileDefinition();
	TSharedPtr<FCombatJsonValue> Row;
	if (!ParseRowTextToObject(RowJsonText, Row, OutError))
	{
		return false;
	}
	return ParseProjectileDefinition(*Row, OutProjectile, OutError);
}

bool ParseVehicleDefinition(const FString& RowJsonText, FVehicleDefinition& OutVehicle, FString& OutError)
{
	OutVehicle = FVehicleDefinition();
	TSharedPtr<FCombatJsonValue> Row;
	if (!ParseRowTextToObject(RowJsonText, Row, OutError))
	{
		return false;
	}
	return ParseVehicleDefinition(*Row, OutVehicle, OutError);
}

bool ParseVehicleSeat(const FString& RowJsonText, FVehicleSeatDefinition& OutSeat, FString& OutError)
{
	OutSeat = FVehicleSeatDefinition();
	TSharedPtr<FCombatJsonValue> Row;
	if (!ParseRowTextToObject(RowJsonText, Row, OutError))
	{
		return false;
	}
	return ParseVehicleSeat(*Row, OutSeat, OutError);
}

bool ParseVehicleMount(const FString& RowJsonText, FVehicleMountDefinition& OutMount, FString& OutError)
{
	OutMount = FVehicleMountDefinition();
	TSharedPtr<FCombatJsonValue> Row;
	if (!ParseRowTextToObject(RowJsonText, Row, OutError))
	{
		return false;
	}
	return ParseVehicleMount(*Row, OutMount, OutError);
}

bool ParseVehicleSpawnEntry(const FString& RowJsonText, FVehicleSpawnEntry& OutSpawn, FString& OutError)
{
	OutSpawn = FVehicleSpawnEntry();
	TSharedPtr<FCombatJsonValue> Row;
	if (!ParseRowTextToObject(RowJsonText, Row, OutError))
	{
		return false;
	}
	return ParseVehicleSpawnEntry(*Row, OutSpawn, OutError);
}

// ===========================================================================
// Table document check and directory loading
// ===========================================================================

namespace
{
	/**
	 * Checks one table document: exact known top-level fields (schema_version,
	 * table, rows, optional comment), schema_version must be the integer 1,
	 * the inner table name must match the expected one and rows must be an
	 * array. Every problem is prefixed with the file name.
	 */
	bool ValidateCombatTableDocument(const TSharedPtr<FCombatJsonValue>& Root, const FString& FileName,
		const TCHAR* ExpectedTableName, TSharedPtr<FCombatJsonValue>& OutRows, FProblemList& OutProblems)
	{
		OutRows.Reset();
		if (!Root.IsValid() || Root->Kind != FCombatJsonValue::EKind::Object)
		{
			OutProblems.Add(FString::Printf(TEXT("%s: the table document must be a JSON object"), *FileName));
			return false;
		}
		bool bClean = true;

		static const TCHAR* const KnownFields[] = { TEXT("schema_version"), TEXT("table"), TEXT("rows"), TEXT("comment") };
		for (const TPair<FString, TSharedPtr<FCombatJsonValue>>& Field : Root->Object)
		{
			bool bKnown = false;
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(KnownFields); ++Index)
			{
				if (Field.Key.Equals(KnownFields[Index]))
				{
					bKnown = true;
					break;
				}
			}
			if (!bKnown)
			{
				OutProblems.Add(FString::Printf(TEXT("%s: unknown field '%s'"), *FileName, *Field.Key));
				bClean = false;
			}
		}

		const TSharedPtr<FCombatJsonValue>* SchemaVersion = FindTyped(*Root, TEXT("schema_version"), FCombatJsonValue::EKind::Number);
		if (!SchemaVersion)
		{
			OutProblems.Add(FString::Printf(TEXT("%s: field 'schema_version' must be the number 1"), *FileName));
			bClean = false;
		}
		else
		{
			const double Rounded = FMath::RoundToZero((*SchemaVersion)->Number);
			if (FMath::Abs((*SchemaVersion)->Number - Rounded) > 1e-9 || Rounded != 1.0)
			{
				OutProblems.Add(FString::Printf(TEXT("%s: field 'schema_version' must be the number 1 (got %s)"),
					*FileName, *FString::SanitizeFloat((*SchemaVersion)->Number)));
				bClean = false;
			}
		}

		const TSharedPtr<FCombatJsonValue>* Table = FindTyped(*Root, TEXT("table"), FCombatJsonValue::EKind::String);
		if (!Table)
		{
			OutProblems.Add(FString::Printf(TEXT("%s: field 'table' must be the string '%s'"), *FileName, ExpectedTableName));
			bClean = false;
		}
		else if (!(*Table)->String.Equals(ExpectedTableName))
		{
			OutProblems.Add(FString::Printf(TEXT("%s: field 'table' must be the string '%s' (got '%s')"),
				*FileName, ExpectedTableName, *(*Table)->String));
			bClean = false;
		}

		const TSharedPtr<FCombatJsonValue>* Rows = FindTyped(*Root, TEXT("rows"), FCombatJsonValue::EKind::Array);
		if (!Rows)
		{
			OutProblems.Add(FString::Printf(TEXT("%s: field 'rows' must be an array"), *FileName));
			bClean = false;
		}
		else
		{
			OutRows = *Rows;
		}

		if (const TSharedPtr<FCombatJsonValue>* Comment = Root->Object.Find(TEXT("comment")))
		{
			if (!Comment->IsValid() || (*Comment)->Kind != FCombatJsonValue::EKind::String)
			{
				OutProblems.Add(FString::Printf(TEXT("%s: field 'comment' must be a string when present"), *FileName));
				bClean = false;
			}
		}

		return bClean;
	}

	/**
	 * Loads one table file into OutRows: document check, strict per-row
	 * parsing through RowParser, duplicate-id refusal keyed on IdFieldName.
	 */
	template <typename RowType>
	void LoadCombatTableFile(const FString& FilePath, const TCHAR* ExpectedTableName, const TCHAR* IdFieldName,
		bool (*RowParser)(const FCombatJsonValue&, RowType&, FString&),
		TMap<FName, RowType>& OutRows, FProblemList& OutProblems)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FilePath))
		{
			OutProblems.Add(FString::Printf(TEXT("%s: could not read the source table file"), *FilePath));
			return;
		}
		const FString FileName = FPaths::GetCleanFilename(FilePath);

		TSharedPtr<FCombatJsonValue> Root;
		FString ParseError;
		if (!FCombatJsonParser::Parse(Text, Root, ParseError))
		{
			OutProblems.Add(FString::Printf(TEXT("%s: %s"), *FileName, *ParseError));
			return;
		}

		TSharedPtr<FCombatJsonValue> Rows;
		if (!ValidateCombatTableDocument(Root, FileName, ExpectedTableName, Rows, OutProblems))
		{
			return;
		}

		int32 RowIndex = 0;
		for (const TSharedPtr<FCombatJsonValue>& Row : Rows->Array)
		{
			++RowIndex;
			if (!Row.IsValid() || Row->Kind != FCombatJsonValue::EKind::Object)
			{
				OutProblems.Add(FString::Printf(TEXT("%s: row %d is not a JSON object"), *FileName, RowIndex));
				continue;
			}
			const TSharedPtr<FCombatJsonValue>* Id = FindTyped(*Row, IdFieldName, FCombatJsonValue::EKind::String);
			const FString RowIdText = (Id && !(*Id)->String.IsEmpty()) ? (*Id)->String : FString();

			RowType Parsed;
			FString RowError;
			if (!RowParser(*Row, Parsed, RowError))
			{
				if (!RowIdText.IsEmpty())
				{
					OutProblems.Add(FString::Printf(TEXT("%s: row %d ('%s'): %s"), *FileName, RowIndex, *RowIdText, *RowError));
				}
				else
				{
					OutProblems.Add(FString::Printf(TEXT("%s: row %d: %s"), *FileName, RowIndex, *RowError));
				}
				continue;
			}

			const FName RowId(*RowIdText);
			if (OutRows.Contains(RowId))
			{
				OutProblems.Add(FString::Printf(TEXT("%s: row %d ('%s'): duplicate id, it is already defined by an earlier row"),
					*FileName, RowIndex, *RowId.ToString()));
				continue;
			}
			OutRows.Add(RowId, MoveTemp(Parsed));
		}
	}
}

void FCombatDataTableSet::Reset()
{
	DamageProfiles.Reset();
	AttackReactions.Reset();
	TargetReactions.Reset();
	Presentations.Reset();
	Weapons.Reset();
	AmmoTypes.Reset();
	Projectiles.Reset();
	Vehicles.Reset();
	VehicleSeats.Reset();
	VehicleMounts.Reset();
	SkippedKnownTableFiles.Reset();
}

bool ParseCombatTableManifest(const FCombatJsonValue& Root, FCombatTableManifest& OutManifest, FString& OutError)
{
	OutManifest = FCombatTableManifest();
	OutError.Reset();
	if (Root.Kind != FCombatJsonValue::EKind::Object)
	{
		OutError = FString::Printf(TEXT("the manifest document must be a JSON object (got %s)"), *GetJsonKindName(Root.Kind));
		return false;
	}

	// Strict schema: the manifest is a registry document, not a row table -
	// exactly schema_version, required_tables and an optional comment.
	static const TCHAR* const KnownFields[] = { TEXT("schema_version"), TEXT("required_tables"), TEXT("comment") };
	for (const TPair<FString, TSharedPtr<FCombatJsonValue>>& Field : Root.Object)
	{
		bool bKnown = false;
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(KnownFields); ++Index)
		{
			if (Field.Key.Equals(KnownFields[Index]))
			{
				bKnown = true;
				break;
			}
		}
		if (!bKnown)
		{
			OutError = FString::Printf(TEXT("unknown field '%s'"), *Field.Key);
			return false;
		}
	}

	const TSharedPtr<FCombatJsonValue>* SchemaVersion = FindTyped(Root, TEXT("schema_version"), FCombatJsonValue::EKind::Number);
	if (!SchemaVersion)
	{
		OutError = TEXT("field 'schema_version' must be the number 1");
		return false;
	}
	const double Rounded = FMath::RoundToZero((*SchemaVersion)->Number);
	if (FMath::Abs((*SchemaVersion)->Number - Rounded) > 1e-9 || Rounded != 1.0)
	{
		OutError = FString::Printf(TEXT("field 'schema_version' must be the number 1 (got %s)"),
			*FString::SanitizeFloat((*SchemaVersion)->Number));
		return false;
	}

	const TSharedPtr<FCombatJsonValue>* Required = FindTyped(Root, TEXT("required_tables"), FCombatJsonValue::EKind::Array);
	if (!Required)
	{
		OutError = TEXT("field 'required_tables' must be an array of table names");
		return false;
	}
	if ((*Required)->Array.Num() == 0)
	{
		OutError = TEXT("field 'required_tables' must list at least one table (an empty registry would silently disable every table)");
		return false;
	}

	const TArray<FName>& SupportedTables = GetSupportedCombatTableNames();
	for (int32 Index = 0; Index < (*Required)->Array.Num(); ++Index)
	{
		const TSharedPtr<FCombatJsonValue>& Element = (*Required)->Array[Index];
		if (!Element.IsValid() || Element->Kind != FCombatJsonValue::EKind::String)
		{
			OutError = FString::Printf(TEXT("field 'required_tables[%d]' must be a string"), Index);
			return false;
		}
		const FString& NameText = Element->String;
		if (!IsValidDefinitionIdSyntax(NameText))
		{
			OutError = FString::Printf(TEXT("field 'required_tables[%d]' '%s' must be 1..64 characters of a-z, 0-9 and _"), Index, *NameText);
			return false;
		}
		bool bSupported = false;
		for (const FName Supported : SupportedTables)
		{
			if (Supported.ToString().Equals(NameText))
			{
				bSupported = true;
				break;
			}
		}
		if (!bSupported)
		{
			OutError = FString::Printf(TEXT("field 'required_tables[%d]' '%s' is not a supported combat table name; the manifest can only require tables this parser parses"), Index, *NameText);
			return false;
		}
		const FName Entry(*NameText);
		if (OutManifest.RequiredTables.Contains(Entry))
		{
			OutError = FString::Printf(TEXT("field 'required_tables' lists '%s' more than once"), *NameText);
			return false;
		}
		OutManifest.RequiredTables.Add(Entry);
	}

	if (const TSharedPtr<FCombatJsonValue>* Comment = Root.Object.Find(TEXT("comment")))
	{
		if (!Comment->IsValid() || (*Comment)->Kind != FCombatJsonValue::EKind::String)
		{
			OutError = TEXT("field 'comment' must be a string when present");
			return false;
		}
	}

	return true;
}

bool ParseCombatTableManifest(const FString& ManifestJsonText, FCombatTableManifest& OutManifest, FString& OutError)
{
	OutManifest = FCombatTableManifest();
	TSharedPtr<FCombatJsonValue> Root;
	if (!FCombatJsonParser::Parse(ManifestJsonText, Root, OutError))
	{
		OutError = FString::Printf(TEXT("invalid manifest JSON: %s"), *OutError);
		return false;
	}
	if (!Root.IsValid() || Root->Kind != FCombatJsonValue::EKind::Object)
	{
		OutError = TEXT("the manifest document must be a JSON object");
		return false;
	}
	return ParseCombatTableManifest(*Root, OutManifest, OutError);
}

const TArray<FName>& GetRequiredCombatTableNames()
{
	static const TArray<FName> RequiredTables = {
		TEXT("damage_profiles"), TEXT("attack_reactions"), TEXT("target_reactions"),
		TEXT("presentations"), TEXT("weapons"), TEXT("ammo_types"), TEXT("projectiles")
	};
	return RequiredTables;
}

const TArray<FName>& GetSupportedCombatTableNames()
{
	static const TArray<FName> SupportedTables = {
		TEXT("damage_profiles"), TEXT("attack_reactions"), TEXT("target_reactions"),
		TEXT("presentations"), TEXT("weapons"), TEXT("ammo_types"), TEXT("projectiles"),
		TEXT("vehicles"), TEXT("vehicle_seats"), TEXT("vehicle_mounts")
	};
	return SupportedTables;
}

const TArray<FName>& GetKnownUnparsedCombatTableNames()
{
	static const TArray<FName> KnownUnparsedTables = {
		TEXT("target_filters"), TEXT("tags")
	};
	return KnownUnparsedTables;
}

bool LoadCombatDataDirectory(const FString& DataDir, FCombatDataTableSet& OutTables, TArray<FString>& OutProblems)
{
	OutTables.Reset();
	OutProblems.Reset();

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*DataDir))
	{
		OutProblems.Add(FString::Printf(TEXT("the combat data directory '%s' does not exist"), *DataDir));
		return false;
	}

	// M5-037: manifest.json, when the directory carries one, is the explicit
	// required-table registry and is parsed first - its required_tables list
	// replaces the code-defined A-segment fallback. A broken manifest refuses
	// the whole load: the registry is the switch that enables tables, so a
	// registry nobody can read must never silently degrade to the fallback.
	FCombatTableManifest Manifest;
	bool bHasManifest = false;
	const FString ManifestPath = FPaths::Combine(DataDir, TEXT("manifest.json"));
	if (PlatformFile.FileExists(*ManifestPath))
	{
		FString ManifestText;
		if (!FFileHelper::LoadFileToString(ManifestText, *ManifestPath))
		{
			OutProblems.Add(FString::Printf(TEXT("manifest.json: could not read the manifest file")));
			return false;
		}
		TSharedPtr<FCombatJsonValue> ManifestRoot;
		FString ManifestError;
		if (!FCombatJsonParser::Parse(ManifestText, ManifestRoot, ManifestError))
		{
			OutProblems.Add(FString::Printf(TEXT("manifest.json: %s"), *ManifestError));
			return false;
		}
		if (!ParseCombatTableManifest(*ManifestRoot, Manifest, ManifestError))
		{
			OutProblems.Add(FString::Printf(TEXT("manifest.json: %s"), *ManifestError));
			return false;
		}
		bHasManifest = true;
	}
	const TArray<FName>& RequiredTables = bHasManifest ? Manifest.RequiredTables : GetRequiredCombatTableNames();

	// Required tables: each has exactly one file named after the table. A
	// missing file is a missing table (never silently accepted as empty).
	for (const FName TableName : RequiredTables)
	{
		const FString FileName = TableName.ToString() + TEXT(".json");
		const FString FilePath = FPaths::Combine(DataDir, FileName);
		if (!PlatformFile.FileExists(*FilePath))
		{
			OutProblems.Add(FString::Printf(TEXT("the required combat table '%s' is missing (expected %s)"),
				*TableName.ToString(), *FilePath));
			continue;
		}
		if (TableName == FName(TEXT("damage_profiles")))
		{
			LoadCombatTableFile(FilePath, TEXT("damage_profiles"), TEXT("damage_profile_id"),
				&ParseDamageProfile, OutTables.DamageProfiles, OutProblems);
		}
		else if (TableName == FName(TEXT("attack_reactions")))
		{
			LoadCombatTableFile(FilePath, TEXT("attack_reactions"), TEXT("reaction_id"),
				&ParseAttackReaction, OutTables.AttackReactions, OutProblems);
		}
		else if (TableName == FName(TEXT("target_reactions")))
		{
			LoadCombatTableFile(FilePath, TEXT("target_reactions"), TEXT("policy_id"),
				&ParseTargetReaction, OutTables.TargetReactions, OutProblems);
		}
		else if (TableName == FName(TEXT("presentations")))
		{
			LoadCombatTableFile(FilePath, TEXT("presentations"), TEXT("presentation_id"),
				&ParsePresentation, OutTables.Presentations, OutProblems);
		}
		else if (TableName == FName(TEXT("weapons")))
		{
			LoadCombatTableFile(FilePath, TEXT("weapons"), TEXT("weapon_id"),
				&ParseWeaponDefinition, OutTables.Weapons, OutProblems);
		}
		else if (TableName == FName(TEXT("ammo_types")))
		{
			LoadCombatTableFile(FilePath, TEXT("ammo_types"), TEXT("ammo_id"),
				&ParseAmmoType, OutTables.AmmoTypes, OutProblems);
		}
		else if (TableName == FName(TEXT("projectiles")))
		{
			LoadCombatTableFile(FilePath, TEXT("projectiles"), TEXT("projectile_id"),
				&ParseProjectileDefinition, OutTables.Projectiles, OutProblems);
		}
		else if (TableName == FName(TEXT("vehicles")))
		{
			LoadCombatTableFile(FilePath, TEXT("vehicles"), TEXT("vehicle_id"),
				&ParseVehicleDefinition, OutTables.Vehicles, OutProblems);
		}
		else if (TableName == FName(TEXT("vehicle_seats")))
		{
			LoadCombatTableFile(FilePath, TEXT("vehicle_seats"), TEXT("seat_id"),
				&ParseVehicleSeat, OutTables.VehicleSeats, OutProblems);
		}
		else if (TableName == FName(TEXT("vehicle_mounts")))
		{
			LoadCombatTableFile(FilePath, TEXT("vehicle_mounts"), TEXT("mount_id"),
				&ParseVehicleMount, OutTables.VehicleMounts, OutProblems);
		}
	}

	// Every other .json file must be a known table name. A supported table
	// file that the required set does not name is REFUSED (the manifest
	// decides which tables a directory enables - a stray table file is never
	// silently loaded or ignored); known-unparsed tables (owned by later
	// cards) are skipped and recorded; anything else is refused instead of
	// silently ignored. The manifest file itself is consumed above.
	TArray<FString> FoundFiles;
	PlatformFile.FindFiles(FoundFiles, *DataDir, TEXT(".json"));
	FoundFiles.Sort();
	for (const FString& FoundPath : FoundFiles)
	{
		const FString FileName = FPaths::GetCleanFilename(FoundPath);
		const FString Stem = FPaths::GetBaseFilename(FileName);

		bool bKnown = false;
		for (const FName Required : RequiredTables)
		{
			if (Stem.Equals(Required.ToString(), ESearchCase::IgnoreCase))
			{
				bKnown = true;
				break;
			}
		}
		if (!bKnown)
		{
			for (const FName Supported : GetSupportedCombatTableNames())
			{
				if (Stem.Equals(Supported.ToString(), ESearchCase::IgnoreCase))
				{
					OutProblems.Add(FString::Printf(TEXT("%s: table '%s' is present but not registered in the required table set; add it to manifest.json 'required_tables' or remove the file"),
						*FileName, *Stem));
					bKnown = true;
					break;
				}
			}
		}
		if (!bKnown)
		{
			for (const FName KnownUnparsed : GetKnownUnparsedCombatTableNames())
			{
				if (Stem.Equals(KnownUnparsed.ToString(), ESearchCase::IgnoreCase))
				{
					OutTables.SkippedKnownTableFiles.AddUnique(FileName);
					bKnown = true;
					break;
				}
			}
		}
		if (!bKnown && FileName.Equals(TEXT("manifest.json"), ESearchCase::IgnoreCase))
		{
			bKnown = true;
		}
		if (!bKnown)
		{
			OutProblems.Add(FString::Printf(TEXT("%s: unknown combat table file '%s'; the combat data directory accepts only the known table names"),
				*FileName, *Stem));
		}
	}

	// In-table rule: every attack reaction maps to exactly one presentation.
	TSet<FName> MappedReactions;
	for (const TPair<FName, FCombatPresentation>& Pair : OutTables.Presentations)
	{
		if (MappedReactions.Contains(Pair.Value.ReactionId))
		{
			OutProblems.Add(FString::Printf(TEXT("presentations.json: row '%s': field 'reaction_id' '%s' is already mapped by another presentation; every attack reaction maps to exactly one presentation"),
				*Pair.Value.PresentationId.ToString(), *Pair.Value.ReactionId.ToString()));
			continue;
		}
		MappedReactions.Add(Pair.Value.ReactionId);
	}

	return OutProblems.Num() == 0;
}

bool ValidateCombatDataReferences(const FCombatDataTableSet& Tables, TArray<FString>& OutProblems)
{
	OutProblems.Reset();

	for (const TPair<FName, FWeaponDefinition>& Pair : Tables.Weapons)
	{
		const FWeaponDefinition& Weapon = Pair.Value;
		if (!Weapon.AmmoId.IsNone() && !Tables.AmmoTypes.Contains(Weapon.AmmoId))
		{
			OutProblems.Add(FString::Printf(TEXT("weapons: row '%s': field 'ammo_id' references unknown ammo_types id '%s'"),
				*Weapon.WeaponId.ToString(), *Weapon.AmmoId.ToString()));
		}
		if (!Weapon.ProjectileId.IsNone() && !Tables.Projectiles.Contains(Weapon.ProjectileId))
		{
			OutProblems.Add(FString::Printf(TEXT("weapons: row '%s': field 'projectile_id' references unknown projectiles id '%s'"),
				*Weapon.WeaponId.ToString(), *Weapon.ProjectileId.ToString()));
		}
		if (!Weapon.DamageProfileId.IsNone() && !Tables.DamageProfiles.Contains(Weapon.DamageProfileId))
		{
			OutProblems.Add(FString::Printf(TEXT("weapons: row '%s': field 'damage_profile_id' references unknown damage_profiles id '%s'"),
				*Weapon.WeaponId.ToString(), *Weapon.DamageProfileId.ToString()));
		}
	}

	for (const TPair<FName, FProjectileDefinition>& Pair : Tables.Projectiles)
	{
		const FProjectileDefinition& Projectile = Pair.Value;
		if (!Projectile.DamageProfileId.IsNone() && !Tables.DamageProfiles.Contains(Projectile.DamageProfileId))
		{
			OutProblems.Add(FString::Printf(TEXT("projectiles: row '%s': field 'damage_profile_id' references unknown damage_profiles id '%s'"),
				*Projectile.ProjectileId.ToString(), *Projectile.DamageProfileId.ToString()));
		}
		if (!Projectile.ExplosionDamageProfileId.IsNone() && !Tables.DamageProfiles.Contains(Projectile.ExplosionDamageProfileId))
		{
			OutProblems.Add(FString::Printf(TEXT("projectiles: row '%s': field 'explosion_damage_profile_id' references unknown damage_profiles id '%s'"),
				*Projectile.ProjectileId.ToString(), *Projectile.ExplosionDamageProfileId.ToString()));
		}
	}

	for (const TPair<FName, FCombatPresentation>& Pair : Tables.Presentations)
	{
		const FCombatPresentation& Presentation = Pair.Value;
		if (!Presentation.ReactionId.IsNone() && !Tables.AttackReactions.Contains(Presentation.ReactionId))
		{
			OutProblems.Add(FString::Printf(TEXT("presentations: row '%s': field 'reaction_id' references unknown attack_reactions id '%s'"),
				*Presentation.PresentationId.ToString(), *Presentation.ReactionId.ToString()));
		}
	}

	// M5-037: the vehicle edges. Seats and mounts resolve their vehicle,
	// mounts resolve their turret weapon (a mount without a weapon is
	// refused), and every vehicle keeps at most one seat - the single-seat
	// first version rejects multi-seat layouts explicitly.
	for (const TPair<FName, FVehicleSeatDefinition>& Pair : Tables.VehicleSeats)
	{
		const FVehicleSeatDefinition& Seat = Pair.Value;
		if (!Seat.VehicleId.IsNone() && !Tables.Vehicles.Contains(Seat.VehicleId))
		{
			OutProblems.Add(FString::Printf(TEXT("vehicle_seats: row '%s': field 'vehicle_id' references unknown vehicles id '%s'"),
				*Seat.SeatId.ToString(), *Seat.VehicleId.ToString()));
		}
	}
	TMap<FName, FName> FirstSeatByVehicle;
	for (const TPair<FName, FVehicleSeatDefinition>& Pair : Tables.VehicleSeats)
	{
		const FVehicleSeatDefinition& Seat = Pair.Value;
		if (Seat.VehicleId.IsNone())
		{
			continue;
		}
		if (const FName* ExistingSeatId = FirstSeatByVehicle.Find(Seat.VehicleId))
		{
			OutProblems.Add(FString::Printf(TEXT("vehicle_seats: row '%s': field 'vehicle_id' '%s' already has the seat '%s'; the first version supports exactly one seat per vehicle (multi-seat layouts are not supported)"),
				*Seat.SeatId.ToString(), *Seat.VehicleId.ToString(), *ExistingSeatId->ToString()));
			continue;
		}
		FirstSeatByVehicle.Add(Seat.VehicleId, Seat.SeatId);
	}
	for (const TPair<FName, FVehicleMountDefinition>& Pair : Tables.VehicleMounts)
	{
		const FVehicleMountDefinition& Mount = Pair.Value;
		if (!Mount.VehicleId.IsNone() && !Tables.Vehicles.Contains(Mount.VehicleId))
		{
			OutProblems.Add(FString::Printf(TEXT("vehicle_mounts: row '%s': field 'vehicle_id' references unknown vehicles id '%s'"),
				*Mount.MountId.ToString(), *Mount.VehicleId.ToString()));
		}
		if (!Mount.TurretWeaponId.IsNone() && !Tables.Weapons.Contains(Mount.TurretWeaponId))
		{
			OutProblems.Add(FString::Printf(TEXT("vehicle_mounts: row '%s': field 'turret_weapon_id' references unknown weapons id '%s'"),
				*Mount.MountId.ToString(), *Mount.TurretWeaponId.ToString()));
		}
	}

	return OutProblems.Num() == 0;
}

bool ValidateVehicleSpawnReferences(const FCombatDataTableSet& Tables, const TArray<FVehicleSpawnEntry>& Spawns, TArray<FString>& OutProblems)
{
	OutProblems.Reset();

	for (int32 Index = 0; Index < Spawns.Num(); ++Index)
	{
		const FVehicleSpawnEntry& Spawn = Spawns[Index];

		// The generic value rules (id present, finite in-bounds room-local
		// location) re-run here so a hand-assembled entry cannot bypass the
		// parser-side checks; every problem names its field.
		FString ValidationErrors;
		if (!ValidateVehicleSpawnEntry(Spawn, ValidationErrors))
		{
			OutProblems.Add(FString::Printf(TEXT("vehicle_spawns: entry %d: %s"), Index, *ValidationErrors));
		}

		// The identity edge: the vehicle id must resolve into the vehicles
		// table - a spawn list entry for an unregistered vehicle is refused,
		// never fabricated from a stand-in.
		if (!Spawn.VehicleId.IsNone() && !Tables.Vehicles.Contains(Spawn.VehicleId))
		{
			OutProblems.Add(FString::Printf(TEXT("vehicle_spawns: entry %d: field 'vehicle_id' references unknown vehicles id '%s'"),
				Index, *Spawn.VehicleId.ToString()));
		}
	}

	return OutProblems.Num() == 0;
}
