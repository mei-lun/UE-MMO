#include "DamageNumberModel.h"

// M1-035: real implementation of the pure display model behind the debug
// HUD's hit feedback. Everything runs on the explicitly injected clock; the
// pool owns plain data copies (no object references), so eviction and expiry
// can never dangle.

void FDamageNumberPool::SetNowSeconds(double InNowSeconds)
{
	NowSeconds = InNowSeconds;
}

void FDamageNumberPool::Add(float InDamage, const FVector& InLocation, FName InAttackId)
{
	// Expired entries go first so the cap compares against the live set only.
	PruneExpired();

	// Hard cap: evict the earliest entry (index 0, the oldest still stored)
	// until exactly one slot is free, then append at the back.
	while (Entries.Num() >= MaxEntries)
	{
		Entries.RemoveAt(0, 1, EAllowShrinking::No);
	}

	FDamageNumberEntry& Entry = Entries.AddDefaulted_GetRef();
	Entry.Damage = InDamage;
	Entry.SpawnTimeSeconds = NowSeconds;
	Entry.Location = InLocation;
	Entry.AttackId = InAttackId;
}

void FDamageNumberPool::PruneExpired()
{
	// An entry whose life reached the lifetime (boundary inclusive) is gone.
	for (int32 Index = Entries.Num() - 1; Index >= 0; --Index)
	{
		if (NowSeconds - Entries[Index].SpawnTimeSeconds >= LifeTimeSeconds)
		{
			Entries.RemoveAt(Index, 1, EAllowShrinking::No);
		}
	}
}

int32 FDamageNumberPool::Num()
{
	PruneExpired();
	return Entries.Num();
}

void FDamageNumberPool::Clear()
{
	Entries.Reset();
}

void FHealthBarData::Set(float InCurrentHP, float InMaxHP, bool bInAlive)
{
	CurrentHP = InCurrentHP;
	MaxHP = InMaxHP;
	bAlive = bInAlive;
	bHasData = true;

	// Clamp the current pool into the frame so a racing write can never push
	// the bar outside [0, MaxHP]; a non-positive max leaves the value as fed
	// (the ratio guard handles it).
	if (MaxHP > 0.0f)
	{
		CurrentHP = FMath::Clamp(CurrentHP, 0.0f, MaxHP);
	}
}

float FHealthBarData::GetRatio() const
{
	if (!bHasData || MaxHP <= 0.0f)
	{
		return 0.0f;
	}
	return FMath::Clamp(CurrentHP / MaxHP, 0.0f, 1.0f);
}

FComboCounter::FComboCounter(double InTimeoutSeconds)
	: TimeoutSeconds(InTimeoutSeconds)
{
}

int32 FComboCounter::NotifyHit(double NowSeconds)
{
	// Inside the window (strictly less than the timeout since the last hit)
	// the combo continues; otherwise it restarts at 1.
	if (bHasHit && (NowSeconds - LastHitTimeSeconds) < TimeoutSeconds)
	{
		++ComboCount;
	}
	else
	{
		ComboCount = 1;
	}
	LastHitTimeSeconds = NowSeconds;
	bHasHit = true;
	return ComboCount;
}

int32 FComboCounter::EvaluateCombo(double NowSeconds) const
{
	// Boundary inclusive: once the full window elapsed since the last hit the
	// combo reads 0 (the next hit restarts at 1).
	if (!bHasHit || (NowSeconds - LastHitTimeSeconds) >= TimeoutSeconds)
	{
		return 0;
	}
	return ComboCount;
}

void FComboCounter::Reset()
{
	ComboCount = 0;
	LastHitTimeSeconds = 0.0;
	bHasHit = false;
}
