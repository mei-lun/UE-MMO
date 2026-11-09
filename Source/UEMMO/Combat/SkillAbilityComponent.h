// M8-031 part2: the skill ability component (the quality plan 08 problem 2).
// ONE component instance per skill, self-contained: the cast conditions (the
// state machine gate + the economy via the skill loadout), the delivery
// (the melee attack start through the shared combat chain, or a straight-
// flight skill projectile with the unified Health chain), and the hit
// feedback sound. The input layer routes slot presses to these components
// generically - no per-skill special-casing anywhere else.
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "SkillAbilityComponent.generated.h"

class UCombatComponent;
class USkillLoadoutComponent;
class USoundBase;
class AThrowStoneProjectile;

/** The projectile delivery spec for the projectile-shaped skills. */
USTRUCT(BlueprintType)
struct FSkillProjectileSpec
{
	GENERATED_BODY()

	/** True when the skill delivers through a projectile (false = the melee attack start). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skill")
	bool bProjectile = false;

	/** Straight-flight speed (cm/s). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skill", meta = (ClampMin = "1.0"))
	float ProjectileSpeedCmPerSecond = 400.0f;

	/** The projectile's damage through the unified Health chain. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skill", meta = (ClampMin = "0.0"))
	float ProjectileDamage = 6.0f;

	/** The flight hit box half extent (cm; the plan 08 keeps the 20cm cube
	 * convention the M8-007 source evidence pinned). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skill", meta = (ClampMin = "1.0"))
	float ProjectileBoxHalfCm = 10.0f;

	/** The flight range before landing (cm). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skill", meta = (ClampMin = "1.0"))
	float ProjectileRangeCm = 900.0f;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSkillAbilityCast, FName, SkillId, bool, bAccepted);

UCLASS(ClassGroup = (Combat), meta = (BlueprintSpawnableComponent))
class UEMMO_API USkillAbilityComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USkillAbilityComponent();

	/**
	 * The cast entry the input layer routes to (generic - no per-skill
	 * special-casing outside this component): the state machine gate + the
	 * economy (the loadout) + the delivery. Returns true when accepted.
	 */
	bool TryCast(UCombatComponent* Combat, USkillLoadoutComponent* Loadout, int32 Facing);

	/** The wiring: one-time configuration from the profession skill row. */
	void Configure(FName InSkillId, FName InAttackId, int32 InMPCost, float InCooldownSeconds,
		const FSkillProjectileSpec& InProjectileSpec, USkillLoadoutComponent* InLoadout);

	FName GetSkillId() const { return SkillId; }
	FName GetAttackId() const { return AttackId; }

	/** The cast result broadcast (the tests/observability). */
	FOnSkillAbilityCast OnCast;

	/** The test seam: removes the projectile spec (the isolation tests). */
	void DisableProjectileDelivery() { ProjectileSpec.bProjectile = false; }

protected:
	virtual void BeginPlay() override;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skill")
	FName SkillId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skill")
	FName AttackId;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skill")
	int32 MPCost = 0;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skill", meta = (ClampMin = "0.0"))
	float CooldownSeconds = 0.0f;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skill")
	FSkillProjectileSpec ProjectileSpec;

	/** The hit feedback sound through the unified chain (M8-021's voices). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Skill")
	TSoftObjectPtr<USoundBase> CastSound;

	TWeakObjectPtr<USkillLoadoutComponent> LoadoutWeak;
};
