#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"

#include "MeleeEnemy.generated.h"

class UCombatComponent;
class UEnemyDefinition;
class UHealthComponent;

/**
 * M2-002: ground melee enemy for the M2 rooms. It reuses the M1 Health/Combat
 * component pair as subobjects exactly like ATrainingEnemy (interface
 * contract section 7: never a second direct health-removal path) and drives
 * its combat component every game frame with the exact M1-043 pattern
 * (SetInputClockSeconds BEFORE TickCombat, one injection per frame).
 *
 * This card ships no attack (M2-003 adds the telegraph/recover pipeline):
 * the combat component is driven only so its victim-side timers advance with
 * identical semantics to the training dummy once attacks exist. Movement and
 * facing are commanded by AMeleeEnemyController through AddMovementInput on
 * the normal CharacterMovement (real walking physics, real collision); the
	 * facing rule is "only +/-X" (ApplyFacingIntent) so the enemy never turns to
 * face the camera direction or the raw diagonal to its target. The visual is
 * the same Quinn template asset the training dummy uses - no new art.
 */
UCLASS()
class UEMMO_API AMeleeEnemy : public ACharacter
{
	GENERATED_BODY()

public:
	AMeleeEnemy();

	/** Health pool of this enemy; damage must go through ApplyDamage. */
	UFUNCTION(BlueprintPure, Category = "Combat")
	UHealthComponent* GetHealthComponent() const { return Health; }

	/**
	 * M1-020 combat state of this enemy (hit stun and death priority), the
	 * same shared component the training dummy carries.
	 */
	UFUNCTION(BlueprintPure, Category = "Combat")
	UCombatComponent* GetCombatComponent() const { return Combat; }

	/**
	 * Applies the M2-001 data definition: MoveSpeed feeds the movement
	 * component's MaxWalkSpeed; the controller reads the combat fields
	 * (AttackRangeX / AlignYTolerance) from here every tick. Passing nullptr
	 * clears the applied definition (the constructor defaults stay).
	 */
	void SetEnemyDefinition(const UEnemyDefinition* InDefinition);

	/** Definition applied by SetEnemyDefinition (may be null). */
	const UEnemyDefinition* GetEnemyDefinition() const { return EnemyDefinition.Get(); }

	/**
	 * M2-002 facing rule: the approach intent is a world-space direction.
	 * Only an X component turns the enemy (yaw 0 for +X, yaw 180 for -X); a
	 * pure Y (depth) or zero intent keeps the current yaw. The pure rule
	 * lives in ComputeMeleeFacingYaw (MeleeEnemyController.h) so tests can
	 * exercise it without a world; this entry applies it to the actor.
	 */
	void ApplyFacingIntent(const FVector& MoveIntent);

	/**
	 * M2-003 minimal telegraph presentation: a uniform mesh swell while the
	 * attack wind-up runs, restored when it ends (color tint or scale was
	 * the card's allowed minimum; this is the scale variant). Pure
	 * presentation: the mesh carries no collision, so the real hit query of
	 * the combat component pipeline is unaffected either way.
	 */
	void ApplyTelegraphVisual(bool bActive);

protected:
	virtual void BeginPlay() override;

	/**
	 * M1-043 per-frame driver of this enemy's combat component, copied from
	 * the ATrainingEnemy precedent: inject the input game clock once per
	 * game frame BEFORE TickCombat from the World GetTimeSeconds clock. The
	 * component's victim-side timers (hit stop, hit stun, landing recovery)
	 * only advance on this injection. A world-less pawn (early tests) keeps
	 * the pre-M1-043 semantics: no injection, no clock-driven progress.
	 */
	virtual void Tick(float DeltaSeconds) override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Combat")
	TObjectPtr<UHealthComponent> Health;

	/** M1-020: hit stun / death priority state for this enemy. */
	UPROPERTY(VisibleAnywhere, Category = "Combat")
	TObjectPtr<UCombatComponent> Combat;

	/**
	 * Applied M2-001 definition (non-owning weak reference: definition data
	 * assets are owned by the content/spawner side, never by the enemy).
	 */
	TWeakObjectPtr<const UEnemyDefinition> EnemyDefinition;
};
