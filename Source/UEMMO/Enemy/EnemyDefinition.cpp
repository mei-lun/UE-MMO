#include "EnemyDefinition.h"

namespace
{
	// Uniquely named (Enemy*) because other translation units in this module
	// keep same-purpose helpers in their own anonymous namespaces.
	void AddEnemyProblem(TArray<FString>& Problems, FString&& Message)
	{
		Problems.Add(MoveTemp(Message));
	}

	/** Appends one problem when a float is not finite, another when it is negative. */
	void CheckEnemyFiniteNonNegative(TArray<FString>& Problems, const TCHAR* FieldName, float Value)
	{
		if (!FMath::IsFinite(Value))
		{
			AddEnemyProblem(Problems, FString::Printf(TEXT("%s must be a finite number (got %s)"),
				FieldName, *FString::SanitizeFloat(Value)));
		}
		else if (Value < 0.0f)
		{
			AddEnemyProblem(Problems, FString::Printf(TEXT("%s must be >= 0 (got %s)"),
				FieldName, *FString::SanitizeFloat(Value)));
		}
	}

	/** Appends one problem when a float is not finite or not strictly positive. */
	void CheckEnemyFinitePositive(TArray<FString>& Problems, const TCHAR* FieldName, float Value)
	{
		if (!FMath::IsFinite(Value))
		{
			AddEnemyProblem(Problems, FString::Printf(TEXT("%s must be a finite number (got %s)"),
				FieldName, *FString::SanitizeFloat(Value)));
		}
		else if (Value <= 0.0f)
		{
			AddEnemyProblem(Problems, FString::Printf(TEXT("%s must be > 0 (got %s)"),
				FieldName, *FString::SanitizeFloat(Value)));
		}
	}
}

double UEnemyDefinition::GetSpawnGraceRemaining(double NowSeconds) const
{
	// SpawnGraceSeconds - elapsed, clamped at 0: the grace window is the
	// half-open [SpawnedAt, SpawnedAt + SpawnGraceSeconds) on the caller's
	// injected game clock. A NowSeconds before the spawn instant keeps the
	// full grace (the caller owns clock monotonicity).
	const double Remaining = static_cast<double>(SpawnGraceSeconds) - (NowSeconds - SpawnedAtGameSeconds);
	return Remaining > 0.0 ? Remaining : 0.0;
}

bool UEnemyDefinition::CanAttack(double NowSeconds) const
{
	return GetSpawnGraceRemaining(NowSeconds) <= 0.0;
}

bool ValidateEnemyDefinition(const UEnemyDefinition& Enemy, FText& OutErrors)
{
	TArray<FString> Problems;

	// Identity: a definition without a stable id cannot be referenced by rooms
	// or spawn tables.
	if (Enemy.EnemyId.IsNone())
	{
		AddEnemyProblem(Problems, TEXT("EnemyId must be a non-empty identifier (got None)"));
	}

	// Survival stats: the health pool must be a positive, finite value
	// (MaxHP <= 0 can never hold a living enemy); attack power may be 0.
	CheckEnemyFinitePositive(Problems, TEXT("MaxHP"), Enemy.MaxHP);
	CheckEnemyFiniteNonNegative(Problems, TEXT("AttackPower"), Enemy.AttackPower);

	// Movement and engagement distances: a melee enemy must be able to close
	// in (MoveSpeed) and needs a positive X attack distance (AttackRangeX).
	CheckEnemyFinitePositive(Problems, TEXT("MoveSpeed"), Enemy.MoveSpeed);
	CheckEnemyFinitePositive(Problems, TEXT("AttackRangeX"), Enemy.AttackRangeX);

	// Y depth alignment: zero or negative tolerance can never align, so the
	// melee enemy could never reach a valid attack position.
	CheckEnemyFinitePositive(Problems, TEXT("AlignYTolerance"), Enemy.AlignYTolerance);

	// Timing: a telegraph of 0 (no wind-up) and a grace of 0 (attackable
	// immediately) are legal designs; negative durations are not.
	CheckEnemyFiniteNonNegative(Problems, TEXT("TelegraphSeconds"), Enemy.TelegraphSeconds);
	CheckEnemyFiniteNonNegative(Problems, TEXT("SpawnGraceSeconds"), Enemy.SpawnGraceSeconds);

	// Melee attack reference: must name an attack of the M1 catalog. NOTE:
	// whether MeleeAttackId resolves to an existing attack_id inside
	// Data/combat-attacks.json is cross-entry validation, deliberately NOT
	// done here; the later M2 room/catalog tasks add the existence check on
	// top of this single-entry validation.
	if (Enemy.MeleeAttackId.IsNone())
	{
		AddEnemyProblem(Problems, TEXT("MeleeAttackId must be a non-empty attack id of the M1 catalog (got None)"));
	}

	if (Problems.Num() == 0)
	{
		OutErrors = FText::GetEmpty();
		return true;
	}
	OutErrors = FText::FromString(FString::Join(Problems, TEXT("; ")));
	return false;
}
