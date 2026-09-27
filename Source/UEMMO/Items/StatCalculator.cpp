// M3-005: implementation of FStatCalculator (migrated from
// Profile/ExperienceCurve.h, where M3-006 seeded it). The M3-005 entry point
// recomputes the whole final row from scratch on every call: sanitized base
// row plus every usable equipped entry, accumulated in double and saturated
// into float range, with the final MaxHP floored at 1. Non-finite equipment
// entries are rejected as a whole, finite negative fields clamp to 0 per
// field, and the CurrentHP clamp on a MaxHP change is min(CurrentHP, NewMax)
// with NaN/garbage hardening. No World, no UObject, no clock; nothing here
// reads or writes a HealthComponent - the clamp is a pure helper whose CALL
// timing belongs to the future wiring task (M3-010).

#include "StatCalculator.h"

namespace
{
	// File-local hardening helpers. One shared rule for every sanitized input:
	// a value that is not a usable non-negative float contributes 0.

	/** A non-finite or negative field sanitizes to 0 (never a stat drain). */
	float SanitizeStatField(float Value)
	{
		if (!FMath::IsFinite(Value) || Value < 0.0f)
		{
			return 0.0f;
		}
		return Value;
	}

	/**
	 * An equipped entry with ANY non-finite field is rejected as a whole: a
	 * poisoned entry has no trustworthy magnitude, so instead of guessing it
	 * contributes nothing (finite negatives are salvageable per field and are
	 * handled by SanitizeStatField instead).
	 */
	bool IsUsableEntry(const FItemStats& Entry)
	{
		return FMath::IsFinite(Entry.Attack)
			&& FMath::IsFinite(Entry.Defense)
			&& FMath::IsFinite(Entry.MaxHP);
	}

	/**
	 * Saturates an accumulated (non-negative, finite) double sum into float
	 * range: large inputs clamp at MAX_flt instead of overflowing to Inf.
	 */
	float SaturateToFloatRange(double Sum)
	{
		if (Sum >= static_cast<double>(MAX_flt))
		{
			return MAX_flt;
		}
		if (Sum <= 0.0)
		{
			return 0.0f;
		}
		return static_cast<float>(Sum);
	}
}

FItemStats FStatCalculator::Recalculate(const FItemStats& BaseStats,
	const TArray<FItemStats>& EquippedStats)
{
	// Complete from-scratch recalculation (interface contract section 8):
	// base row + the sum of every usable equipped entry, recomputed WHOLE on
	// every call - never incremental add/remove bookkeeping, so repeated
	// equip/unequip cycles can never accumulate drift. The accumulation runs
	// in double (float inputs cannot overflow a double sum) and saturates at
	// MAX_flt on the way out.
	double AttackSum = SanitizeStatField(BaseStats.Attack);
	double DefenseSum = SanitizeStatField(BaseStats.Defense);
	double MaxHPSum = SanitizeStatField(BaseStats.MaxHP);

	for (const FItemStats& Entry : EquippedStats)
	{
		if (!IsUsableEntry(Entry))
		{
			// The poisoned entry is rejected as a whole; the others still count.
			continue;
		}
		AttackSum += SanitizeStatField(Entry.Attack);
		DefenseSum += SanitizeStatField(Entry.Defense);
		MaxHPSum += SanitizeStatField(Entry.MaxHP);
	}

	FItemStats Final;
	Final.Attack = SaturateToFloatRange(AttackSum);
	Final.Defense = SaturateToFloatRange(DefenseSum);
	// The final MaxHP floor of 1: even an all-zero row yields a living
	// character's one-hit pool, never a zero or negative max HP.
	Final.MaxHP = FMath::Max(1.0f, SaturateToFloatRange(MaxHPSum));
	return Final;
}

FLevelBaseStats FStatCalculator::Recalculate(const FLevelBaseStats& LevelBase,
	const FLevelBaseStats& EquipmentBonus)
{
	// Migrated verbatim from ExperienceCurve.cpp (M3-006): plain component
	// sum, recomputed whole every call, no clamping and no floor - the level
	// curve rows are always valid design-table values.
	FLevelBaseStats Final;
	Final.MaxHP = LevelBase.MaxHP + EquipmentBonus.MaxHP;
	Final.Attack = LevelBase.Attack + EquipmentBonus.Attack;
	Final.Defense = LevelBase.Defense + EquipmentBonus.Defense;
	return Final;
}

float FStatCalculator::ClampHealthOnMaxChange(float CurrentHP, float OldMax, float NewMax)
{
	// Card semantics: CurrentHP = min(CurrentHP, NewMax). LOWERING the max
	// correctly clamps the pool; RAISING the max never restores already-lost
	// health (no free heal).
	//
	// Hardening: a non-finite or negative NewMax falls back to the old max
	// while that is itself a legal positive number, else to 0 - garbage max
	// data can never raise the pool above what the character had. A
	// non-finite current pool reads as 0 (NaN comparisons are always false,
	// so a raw min() would leak the raw NaN through).
	float EffectiveMax = NewMax;
	if (!FMath::IsFinite(EffectiveMax) || EffectiveMax < 0.0f)
	{
		EffectiveMax = (FMath::IsFinite(OldMax) && OldMax > 0.0f) ? OldMax : 0.0f;
	}
	const float SafeCurrent = FMath::IsFinite(CurrentHP) ? CurrentHP : 0.0f;
	return FMath::Min(SafeCurrent, EffectiveMax);
}

bool FStatCalculator::StatsChanged(const FItemStats& Previous, const FItemStats& Current)
{
	// Exact float comparison for finite fields: a complete recalculation from
	// identical inputs is deterministic, so bit-equal rows mean "nothing to
	// notify" (and a tolerance here would hide real small drift instead).
	// The explicit finite checks keep NaN meaningful under every floating
	// point model: NaN never compares equal, so a NaN side always reports as
	// changed - a poisoned value can never be silently swallowed.
	auto Differs = [](float A, float B)
	{
		if (!FMath::IsFinite(A) || !FMath::IsFinite(B))
		{
			// +Inf == +Inf stays "unchanged"; any NaN is always a change.
			return !(A == B);
		}
		return A != B;
	};
	return Differs(Previous.Attack, Current.Attack)
		|| Differs(Previous.Defense, Current.Defense)
		|| Differs(Previous.MaxHP, Current.MaxHP);
}
