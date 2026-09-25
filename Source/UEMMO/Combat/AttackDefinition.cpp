#include "AttackDefinition.h"

namespace
{
	void AddProblem(TArray<FString>& Problems, FString&& Message)
	{
		Problems.Add(MoveTemp(Message));
	}

	/** Appends one problem when a float is not finite, another when it is negative. */
	void CheckFiniteNonNegative(TArray<FString>& Problems, const TCHAR* FieldName, float Value)
	{
		if (!FMath::IsFinite(Value))
		{
			AddProblem(Problems, FString::Printf(TEXT("%s must be a finite number (got %s)"),
				FieldName, *FString::SanitizeFloat(Value)));
		}
		else if (Value < 0.0f)
		{
			AddProblem(Problems, FString::Printf(TEXT("%s must be >= 0 (got %s)"),
				FieldName, *FString::SanitizeFloat(Value)));
		}
	}
}

bool ValidateAttackDefinition(const UAttackDefinition& Attack, FText& OutErrors)
{
	TArray<FString> Problems;

	// Total length: a positive integer frame count.
	if (Attack.DurationFrames <= 0)
	{
		AddProblem(Problems, FString::Printf(TEXT("DurationFrames must be a positive frame count (got %d)"),
			Attack.DurationFrames));
	}

	// Frame windows: half-open [Start, End) fully inside [0, DurationFrames].
	// NOTE: cross-entry validation of AllowedNextAttacks (IDs referring to other
	// attacks in the catalog) is deliberately NOT done here. This function checks
	// a single entry only; the catalog tasks (M1-009/M1-010) can loop over their
	// entries, call this per entry, and add the set-based existence checks on top.
	const FAttackFrameWindow* Windows[2] = { &Attack.ActiveWindow, &Attack.CancelWindow };
	const TCHAR* WindowNames[2] = { TEXT("ActiveWindow"), TEXT("CancelWindow") };
	for (int32 Index = 0; Index < 2; ++Index)
	{
		const FAttackFrameWindow& Window = *Windows[Index];
		const TCHAR* Name = WindowNames[Index];
		if (Window.StartFrame < 0)
		{
			AddProblem(Problems, FString::Printf(TEXT("%s StartFrame must be >= 0 (got %d)"),
				Name, Window.StartFrame));
		}
		if (Window.EndFrame <= Window.StartFrame)
		{
			AddProblem(Problems, FString::Printf(TEXT("%s EndFrame must be greater than StartFrame (got [%d, %d))"),
				Name, Window.StartFrame, Window.EndFrame));
		}
		if (Window.EndFrame > Attack.DurationFrames)
		{
			AddProblem(Problems, FString::Printf(TEXT("%s EndFrame must be <= DurationFrames (got end %d > duration %d)"),
				Name, Window.EndFrame, Attack.DurationFrames));
		}
	}

	// Damage: finite and non-negative.
	if (!FMath::IsFinite(Attack.BaseDamage) || Attack.BaseDamage < 0.0f)
	{
		AddProblem(Problems, FString::Printf(TEXT("BaseDamage must be a finite number >= 0 (got %s)"),
			*FString::SanitizeFloat(Attack.BaseDamage)));
	}

	// Attack coefficient: finite and strictly positive (multiplying by it must be meaningful).
	if (!FMath::IsFinite(Attack.AttackCoefficient) || Attack.AttackCoefficient <= 0.0f)
	{
		AddProblem(Problems, FString::Printf(TEXT("AttackCoefficient must be a finite number > 0 (got %s)"),
			*FString::SanitizeFloat(Attack.AttackCoefficient)));
	}

	// Hit box half extent: every component finite and > 0 (a non-positive extent is no box).
	{
		const FVector& Extent = Attack.HitHalfExtent;
		if (!FMath::IsFinite(Extent.X) || !FMath::IsFinite(Extent.Y) || !FMath::IsFinite(Extent.Z) ||
			Extent.X <= 0.0f || Extent.Y <= 0.0f || Extent.Z <= 0.0f)
		{
			AddProblem(Problems, FString::Printf(TEXT("HitHalfExtent components must be finite and > 0 (got %s)"),
				*Extent.ToString()));
		}
	}

	// Hit box offset: every component finite (sign is meaningful, negatives allowed).
	{
		const FVector& Offset = Attack.HitOffsetFromFeet;
		if (!FMath::IsFinite(Offset.X) || !FMath::IsFinite(Offset.Y) || !FMath::IsFinite(Offset.Z))
		{
			AddProblem(Problems, FString::Printf(TEXT("HitOffsetFromFeet components must be finite numbers (got %s)"),
				*Offset.ToString()));
		}
	}

	// Impulses in cm/s and recovery seconds: finite and non-negative.
	CheckFiniteNonNegative(Problems, TEXT("KnockbackSpeed"), Attack.KnockbackSpeed);
	CheckFiniteNonNegative(Problems, TEXT("LaunchSpeed"), Attack.LaunchSpeed);
	CheckFiniteNonNegative(Problems, TEXT("HitStunSeconds"), Attack.HitStunSeconds);
	CheckFiniteNonNegative(Problems, TEXT("HitStopSeconds"), Attack.HitStopSeconds);

	// Clip seconds: finite and non-negative always; the ordering (End >= Start) is
	// only checked for real animations (an animation is set and not a placeholder).
	// Placeholder animations never validate their clip range.
	CheckFiniteNonNegative(Problems, TEXT("ClipStartSeconds"), Attack.ClipStartSeconds);
	CheckFiniteNonNegative(Problems, TEXT("ClipEndSeconds"), Attack.ClipEndSeconds);
	const bool bRealAnimation = !Attack.Animation.IsNull() && !Attack.bPlaceholderAnimation;
	if (bRealAnimation && Attack.ClipEndSeconds < Attack.ClipStartSeconds)
	{
		AddProblem(Problems, FString::Printf(TEXT("ClipEndSeconds must be >= ClipStartSeconds for a real animation (got ClipStartSeconds %s > ClipEndSeconds %s)"),
			*FString::SanitizeFloat(Attack.ClipStartSeconds), *FString::SanitizeFloat(Attack.ClipEndSeconds)));
	}

	if (Problems.Num() == 0)
	{
		OutErrors = FText::GetEmpty();
		return true;
	}
	OutErrors = FText::FromString(FString::Join(Problems, TEXT("; ")));
	return false;
}
