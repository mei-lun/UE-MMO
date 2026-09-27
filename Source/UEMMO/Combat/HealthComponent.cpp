#include "HealthComponent.h"

#include "../Items/StatCalculator.h"

UHealthComponent::UHealthComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

float UHealthComponent::ApplyDamage(float Damage)
{
	// Reject zero, negative and non-finite values: a rejected hit removes no
	// health and fires no events (negative values must never heal).
	if (!FMath::IsFinite(Damage) || Damage <= 0.0f)
	{
		return 0.0f;
	}

	// A dead component refuses further damage and fires no events.
	if (!IsAlive())
	{
		return 0.0f;
	}

	const float HealthBefore = Health;
	Health = FMath::Clamp(Health - Damage, 0.0f, MaxHealth);
	const float Applied = HealthBefore - Health;

	// Death fires exactly once per lifecycle, on the hit that reaches 0.
	if (Health <= 0.0f && !bDied)
	{
		bDied = true;
		OnDied.Broadcast();
	}

	return Applied;
}

void UHealthComponent::ResetHealth()
{
	Health = MaxHealth;
	bDied = false;
}

void UHealthComponent::SetMaxHealth(float NewMaxHealth)
{
	// Garbage bounds are refused (see the header contract): a non-finite or
	// non-positive max never rewrites the pool.
	if (!FMath::IsFinite(NewMaxHealth) || NewMaxHealth <= 0.0f)
	{
		return;
	}
	const float OldMax = MaxHealth;
	MaxHealth = NewMaxHealth;
	// The M3-005 pure clamp (min(CurrentHP, NewMax) with its hardening) is the
	// single source of the no-heal clamp semantics; MaxHealth was already
	// validated above, so the fallback branch of that function stays inert.
	Health = FStatCalculator::ClampHealthOnMaxChange(Health, OldMax, NewMaxHealth);
}

bool UHealthComponent::IsAlive() const
{
	return !bDied && Health > 0.0f;
}

float UHealthComponent::GetHealth() const
{
	return Health;
}

float UHealthComponent::GetMaxHealth() const
{
	return MaxHealth;
}
