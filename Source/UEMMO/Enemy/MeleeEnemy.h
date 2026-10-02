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
	 * M3-025: launcher launches accepted since the last ground contact (the
	 * M1-022 recording pattern copied from ATrainingEnemy; minimal form, see
	 * the LaunchCharacter override below).
	 */
	int32 GetAirComboCount() const { return AirComboCount; }

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

	/**
	 * M3-025 production wiring arming entry. UWaveSpawner calls this right
	 * after it attached the AMeleeEnemyController and the chase target to a
	 * born enemy: from then on the first death of this enemy runs the corpse
	 * cleanup - the AI is stopped, the movement is frozen, every collision is
	 * dropped and the actor (plus its controller) is removed after the 2 s
	 * death-presentation prototype delay.
	 *
	 * Deliberately NOT armed by BeginPlay: the M2-014-style room bookkeeping
	 * scenarios spawn enemies into playerless temp worlds whose corpse and
	 * actor counts are pinned by their suites, so only a production-wired
	 * enemy (controller + target) gets the new death presentation.
	 */
	void ArmDeathCleanup();

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

	/**
	 * M3-025: launches are recorded for the launcher combo count before the
	 * base implementation defers the velocity application - the M1-022
	 * ATrainingEnemy override pattern, so the combat launch path
	 * (UCombatComponent::ApplyHitImpulse) lifts a wave enemy exactly like it
	 * lifts the training dummy.
	 */
	virtual void LaunchCharacter(FVector LaunchVelocity, bool bXYOverride, bool bZOverride) override;

	/**
	 * M3-025: minimal ground-contact recording - a landing re-opens the
	 * ground phase so the next vertical launch counts 1 again. The training
	 * enemy records the same flag (plus its audio/knockdown machinery) in
	 * RecordGroundContact; this enemy deliberately carries none of that.
	 */
	virtual void Landed(const FHitResult& Hit) override;

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

	/** M3-025: launcher launches accepted since the last ground contact. */
	int32 AirComboCount = 0;

	/** M3-025: true while no launcher launch happened since the last ground contact. */
	bool bGroundedSinceLastLaunch = true;

	/**
	 * M3-025: true once the spawner wiring armed the death cleanup (one-shot
	 * arming guard; the death handler is then bound exactly once).
	 */
	bool bDeathCleanupArmed = false;
};
