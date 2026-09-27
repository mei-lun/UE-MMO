// M3-006: implementation of the pure level/XP growth curve (interface
// contract section 8), extracted from UProfileSubsystem (M3-003). Every
// function is static and pure over plain integers: no World, no UObject, no
// clock. The initial design values live in ExperienceCurve.h and are a
// design choice - later tuning must be recorded in the task report.

#include "ExperienceCurve.h"

int32 FExperienceCurve::GetNextLevelXP(int32 Level)
{
	// Card-local clamp: the design level range is closed [MinLevel, MaxLevel];
	// every formula treats out-of-range input as the nearest legal level, so
	// a bad caller can never synthesize a value outside the design table.
	const int32 ClampedLevel = FMath::Clamp(Level, MinLevel, MaxLevel);
	// The max level has no next level to buy: its requirement reads 0.
	return ClampedLevel >= MaxLevel ? 0 : XPPerLevelFactor * ClampedLevel;
}

int32 FExperienceCurve::GetMaxHPForLevel(int32 Level)
{
	return BaseMaxHP + MaxHPPerLevel * (FMath::Clamp(Level, MinLevel, MaxLevel) - 1);
}

int32 FExperienceCurve::GetAttackForLevel(int32 Level)
{
	return BaseAttack + AttackPerLevel * (FMath::Clamp(Level, MinLevel, MaxLevel) - 1);
}

int32 FExperienceCurve::GetDefenseForLevel(int32 Level)
{
	return BaseDefense + DefensePerLevel * (FMath::Clamp(Level, MinLevel, MaxLevel) - 1);
}

FLevelBaseStats FExperienceCurve::GetBaseStatsForLevel(int32 Level)
{
	FLevelBaseStats Stats;
	Stats.MaxHP = GetMaxHPForLevel(Level);
	Stats.Attack = GetAttackForLevel(Level);
	Stats.Defense = GetDefenseForLevel(Level);
	return Stats;
}

bool FExperienceCurve::AddExperience(int32 Level, int32 XP, int32 Amount,
	int32& OutNewLevel, int32& OutRemainingXP)
{
	// Normalize the starting state into the closed design range first: an
	// out-of-range level is treated as the nearest legal level and a negative
	// XP pool reads as 0 (the curve never reports negative XP either).
	const int32 StartLevel = FMath::Clamp(Level, MinLevel, MaxLevel);
	int64 Total = XP > 0 ? static_cast<int64>(XP) : 0;

	// Non-positive amounts can never grow XP (XP only ever increases):
	// reject with the unchanged normalized state and no level-up.
	if (Amount <= 0)
	{
		OutNewLevel = StartLevel;
		OutRemainingXP = static_cast<int32>(Total);
		return false;
	}

	// Saturate instead of wrapping: XP never overflows into a negative value
	// (int64 intermediate; XP and Amount are both non-negative here).
	Total += Amount;
	if (Total > MAX_int32)
	{
		Total = MAX_int32;
	}

	// Consume the curve requirement per crossed boundary: one call can pass
	// several level boundaries and always ends below the next requirement
	// (the remainder is retained, never discarded) or at the max level.
	bool bLeveledUp = false;
	int32 NewLevel = StartLevel;
	while (NewLevel < MaxLevel)
	{
		const int32 Requirement = GetNextLevelXP(NewLevel);
		if (Requirement <= 0 || Total < Requirement)
		{
			break;
		}
		Total -= Requirement;
		++NewLevel;
		bLeveledUp = true;
	}

	// At the max level XP has no meaning: clamp the pool to 0 - both for a
	// call that started there and for a cascade that just landed there.
	if (NewLevel >= MaxLevel)
	{
		Total = 0;
	}

	OutNewLevel = NewLevel;
	OutRemainingXP = static_cast<int32>(Total);
	return bLeveledUp;
}

FLevelBaseStats FStatCalculator::Recalculate(const FLevelBaseStats& LevelBase,
	const FLevelBaseStats& EquipmentBonus)
{
	// Complete from-scratch sum: level-curve base plus the (M3-005-reserved)
	// equipment bonus entry, component-wise. Never incremental - every call
	// recomputes the whole block, so repeated recalculation cannot accumulate
	// drift. No HealthComponent is read or written: leveling up never
	// auto-refills the HP pool.
	FLevelBaseStats Final;
	Final.MaxHP = LevelBase.MaxHP + EquipmentBonus.MaxHP;
	Final.Attack = LevelBase.Attack + EquipmentBonus.Attack;
	Final.Defense = LevelBase.Defense + EquipmentBonus.Defense;
	return Final;
}
