#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"

#include "ItemDefinition.h"

/**
 * One concrete, owned item occurrence (interface contract section 8): the
 * persistent business identity is InstanceId (FGuid), never a UE asset path;
 * the same definition can produce many instances. RollSeed records the seed
 * for future stat rolls (the roll itself belongs to M3-007); for M3-001 the
 * rolled stats start as an exact copy of the definition's BaseStats.
 */
struct FItemInstance
{
	/** Persistent business identity; stable across save/load (M3-013). */
	FGuid InstanceId;

	/** Which kind this occurrence came from; shared by all its instances. */
	FName DefinitionId;

	/** Seed recorded for the (future) stat roll; persisted unchanged. */
	int64 RollSeed = 0;

	/** Instance stats; M3-001 copies BaseStats verbatim. */
	FItemStats RolledStats;

	/** Item level; the profile contract starts items at level 1. */
	int32 Level = 1;

	/**
	 * Serializes the instance to a plain, engine-free key-value snapshot for
	 * the minimal round trip this card demands (the full save system belongs
	 * to M3-013). The first segment is a version tag, the rest are
	 * "key=value" pairs joined with '|'. Display text is not included: the
	 * instance references its kind by DefinitionId only.
	 */
	FString ToStringSnapshot() const
	{
		return FString::Printf(TEXT("%s|instance_id=%s|definition_id=%s|roll_seed=%lld|level=%d|attack=%s|defense=%s|max_hp=%s"),
			SnapshotVersionTag(),
			*InstanceId.ToString(),
			*DefinitionId.ToString(),
			RollSeed,
			Level,
			*FString::SanitizeFloat(RolledStats.Attack),
			*FString::SanitizeFloat(RolledStats.Defense),
			*FString::SanitizeFloat(RolledStats.MaxHP));
	}

	/**
	 * Restores an instance from ToStringSnapshot output. Strict on purpose:
	 * the version tag must match, every field must be present exactly once,
	 * unknown keys are rejected, and a restored InstanceId must be a valid
	 * FGuid (the all-zero guid is never a legal identity). Returns false and
	 * fills OutError (when provided, naming the offending field) without
	 * touching this instance on any failure.
	 */
	bool FromStringSnapshot(const FString& Snapshot, FString* OutError = nullptr)
	{
		FItemInstance Parsed;
		TArray<FString> Parts;
		Snapshot.ParseIntoArray(Parts, TEXT("|"), false);

		if (Parts.Num() < 1 || !Parts[0].Equals(SnapshotVersionTag(), ESearchCase::CaseSensitive))
		{
			SetError(OutError, FString::Printf(TEXT("snapshot must start with the version tag '%s'"),
				SnapshotVersionTag()));
			return false;
		}

		bool bHasInstanceId = false;
		bool bHasDefinitionId = false;
		bool bHasRollSeed = false;
		bool bHasLevel = false;
		bool bHasAttack = false;
		bool bHasDefense = false;
		bool bHasMaxHP = false;
		for (int32 Index = 1; Index < Parts.Num(); ++Index)
		{
			const FString& Part = Parts[Index];
			const int32 EqualsIndex = Part.Find(TEXT("="), ESearchCase::CaseSensitive, ESearchDir::FromStart);
			if (EqualsIndex == INDEX_NONE)
			{
				SetError(OutError, FString::Printf(TEXT("snapshot segment '%s' is not a key=value field"), *Part));
				return false;
			}
			const FString Key = Part.Left(EqualsIndex);
			const FString Value = Part.Mid(EqualsIndex + 1);
			if (Key == TEXT("instance_id"))
			{
				if (bHasInstanceId)
				{
					SetError(OutError, TEXT("snapshot field 'instance_id' appears more than once"));
					return false;
				}
				FGuid ParsedId;
				if (!FGuid::Parse(Value, ParsedId) || !ParsedId.IsValid())
				{
					SetError(OutError, FString::Printf(TEXT("instance_id '%s' is not a valid FGuid"), *Value));
					return false;
				}
				Parsed.InstanceId = ParsedId;
				bHasInstanceId = true;
			}
			else if (Key == TEXT("definition_id"))
			{
				if (bHasDefinitionId)
				{
					SetError(OutError, TEXT("snapshot field 'definition_id' appears more than once"));
					return false;
				}
				if (FName(*Value).IsNone())
				{
					SetError(OutError, TEXT("definition_id must be a non-empty identifier"));
					return false;
				}
				Parsed.DefinitionId = FName(*Value);
				bHasDefinitionId = true;
			}
			else if (Key == TEXT("roll_seed"))
			{
				if (bHasRollSeed)
				{
					SetError(OutError, TEXT("snapshot field 'roll_seed' appears more than once"));
					return false;
				}
				if (!ParseInt64Strict(Value, Parsed.RollSeed))
				{
					SetError(OutError, FString::Printf(TEXT("roll_seed '%s' is not an integer"), *Value));
					return false;
				}
				bHasRollSeed = true;
			}
			else if (Key == TEXT("level"))
			{
				if (bHasLevel)
				{
					SetError(OutError, TEXT("snapshot field 'level' appears more than once"));
					return false;
				}
				if (!ParseIntStrict(Value, Parsed.Level) || Parsed.Level < 1)
				{
					SetError(OutError, FString::Printf(TEXT("level '%s' is not a positive integer"), *Value));
					return false;
				}
				bHasLevel = true;
			}
			else if (Key == TEXT("attack") || Key == TEXT("defense") || Key == TEXT("max_hp"))
			{
				const TCHAR* SnapshotKey = Key == TEXT("attack") ? TEXT("attack") : (Key == TEXT("defense") ? TEXT("defense") : TEXT("max_hp"));
				bool& bHasField = Key == TEXT("attack") ? bHasAttack : (Key == TEXT("defense") ? bHasDefense : bHasMaxHP);
				float& StatField = Key == TEXT("attack") ? Parsed.RolledStats.Attack : (Key == TEXT("defense") ? Parsed.RolledStats.Defense : Parsed.RolledStats.MaxHP);
				if (bHasField)
				{
					SetError(OutError, FString::Printf(TEXT("snapshot field '%s' appears more than once"), SnapshotKey));
					return false;
				}
				if (!ParseFloatStrict(Value, StatField))
				{
					SetError(OutError, FString::Printf(TEXT("%s '%s' is not a finite number"), SnapshotKey, *Value));
					return false;
				}
				bHasField = true;
			}
			else
			{
				SetError(OutError, FString::Printf(TEXT("unknown snapshot field '%s'"), *Key));
				return false;
			}
		}

		if (!bHasInstanceId)
		{
			SetError(OutError, TEXT("snapshot is missing the field 'instance_id'"));
			return false;
		}
		if (!bHasDefinitionId)
		{
			SetError(OutError, TEXT("snapshot is missing the field 'definition_id'"));
			return false;
		}
		if (!bHasRollSeed)
		{
			SetError(OutError, TEXT("snapshot is missing the field 'roll_seed'"));
			return false;
		}
		if (!bHasLevel)
		{
			SetError(OutError, TEXT("snapshot is missing the field 'level'"));
			return false;
		}
		if (!bHasAttack || !bHasDefense || !bHasMaxHP)
		{
			SetError(OutError, TEXT("snapshot is missing one or more of the fields 'attack', 'defense', 'max_hp'"));
			return false;
		}

		*this = Parsed;
		return true;
	}

private:
	/** Version tag of the snapshot format; bump on any breaking change. */
	static const TCHAR* SnapshotVersionTag()
	{
		return TEXT("UEMMO_ITEM_INSTANCE_V1");
	}

	static void SetError(FString* OutError, FString&& Message)
	{
		if (OutError)
		{
			*OutError = MoveTemp(Message);
		}
	}

	static bool IsAsciiDigit(TCHAR Character)
	{
		return Character >= TEXT('0') && Character <= TEXT('9');
	}

	/** Optional '-' then digits only; rejects empty text and trailing junk. */
	static bool ParseInt64Strict(const FString& Text, int64& OutValue)
	{
		int32 Index = 0;
		if (Index < Text.Len() && Text[Index] == TEXT('-'))
		{
			++Index;
		}
		if (Index >= Text.Len())
		{
			return false;
		}
		for (; Index < Text.Len(); ++Index)
		{
			if (!IsAsciiDigit(Text[Index]))
			{
				return false;
			}
		}
		OutValue = FCString::Atoi64(*Text);
		return true;
	}

	/** Digits only (no sign: levels are positive); rejects empty text. */
	static bool ParseIntStrict(const FString& Text, int32& OutValue)
	{
		if (Text.IsEmpty())
		{
			return false;
		}
		for (int32 Index = 0; Index < Text.Len(); ++Index)
		{
			if (!IsAsciiDigit(Text[Index]))
			{
				return false;
			}
		}
		OutValue = FCString::Atoi(*Text);
		return true;
	}

	/** [+-]?digits[.digits][(e|E)[+-]digits]; must be finite after parsing. */
	static bool ParseFloatStrict(const FString& Text, float& OutValue)
	{
		int32 Index = 0;
		if (Index < Text.Len() && (Text[Index] == TEXT('-') || Text[Index] == TEXT('+')))
		{
			++Index;
		}
		int32 DigitCount = 0;
		while (Index < Text.Len() && IsAsciiDigit(Text[Index]))
		{
			++DigitCount;
			++Index;
		}
		if (Index < Text.Len() && Text[Index] == TEXT('.'))
		{
			++Index;
			while (Index < Text.Len() && IsAsciiDigit(Text[Index]))
			{
				++DigitCount;
				++Index;
			}
		}
		if (DigitCount == 0)
		{
			return false;
		}
		if (Index < Text.Len() && (Text[Index] == TEXT('e') || Text[Index] == TEXT('E')))
		{
			++Index;
			if (Index < Text.Len() && (Text[Index] == TEXT('-') || Text[Index] == TEXT('+')))
			{
				++Index;
			}
			bool bExponentDigit = false;
			while (Index < Text.Len() && IsAsciiDigit(Text[Index]))
			{
				bExponentDigit = true;
				++Index;
			}
			if (!bExponentDigit)
			{
				return false;
			}
		}
		if (Index != Text.Len())
		{
			return false;
		}
		OutValue = static_cast<float>(FCString::Atod(*Text));
		return FMath::IsFinite(OutValue);
	}
};

/**
 * Factory: creates one new instance of Definition with a freshly stamped
 * FGuid identity, so two calls on the same definition yield two different
 * InstanceIds while sharing the DefinitionId. RollSeed is recorded unchanged;
 * RolledStats copies Definition.BaseStats verbatim for M3-001 (the seeded roll
 * belongs to M3-007) and Level is 1. Validating the definition stays with
 * ValidateItemDefinition (callers that need the guarantee run it first).
 */
inline FItemInstance MakeItemInstance(const FItemDefinition& Definition, int64 RollSeed)
{
	FItemInstance Instance;
	Instance.InstanceId = FGuid::NewGuid();
	Instance.DefinitionId = Definition.DefinitionId;
	Instance.RollSeed = RollSeed;
	Instance.RolledStats = Definition.BaseStats;
	Instance.Level = 1;
	return Instance;
}
