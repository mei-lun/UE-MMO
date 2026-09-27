#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Delegates/DelegateCombinations.h"
#include "HealthComponent.generated.h"

DECLARE_MULTICAST_DELEGATE(FOnDied);

/**
 * Health pool of a combatant.
 * ApplyDamage returns the amount of health actually removed; negative and
 * non-finite values are rejected, health is clamped to [0, MaxHealth], and
 * OnDied is broadcast at most once per death lifecycle. ResetHealth opens a
 * new lifecycle. Only accepted hits count as damage (interface contract 5).
 */
UCLASS(ClassGroup = (Combat), meta = (BlueprintSpawnableComponent))
class UHealthComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UHealthComponent();

	/**
	 * Applies damage and returns the health actually removed (never negative).
	 * Zero, negative and non-finite (NaN/Inf) values are rejected with 0 and
	 * no events. A dead component refuses further damage with 0 and no events.
	 */
	float ApplyDamage(float Damage);

	/** Restores full health and Alive; opens a new death lifecycle. */
	void ResetHealth();

	/**
	 * M3-010: applies a new maximum pool size with the M3-005
	 * FStatCalculator::ClampHealthOnMaxChange semantics: the current pool is
	 * min(CurrentHP, NewMax) - a LOWERED max clamps the pool down, a RAISED
	 * max never restores already-lost health (no implicit heal). The death
	 * lifecycle is untouched (revival stays ResetHealth's contract). A
	 * non-finite or non-positive max is refused (no change): the wired
	 * profile path can never produce one (FStatCalculator floors the final
	 * MaxHP at 1), so this guard only protects direct callers.
	 */
	void SetMaxHealth(float NewMaxHealth);

	bool IsAlive() const;

	float GetHealth() const;
	float GetMaxHealth() const;

	/** Broadcast at most once per death lifecycle, when health reaches 0. */
	FOnDied OnDied;

private:
	UPROPERTY(EditAnywhere, Category = "Combat")
	float MaxHealth = 100.0f;

	UPROPERTY(VisibleInstanceOnly, Category = "Combat")
	float Health = 100.0f;

	bool bDied = false;
};
