#include "HealthComponent.h"

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
