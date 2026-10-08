#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"

#include "MeleeEnemy.generated.h"

class UAnimationAsset;
class UCombatComponent;
class UCombatPresentationComponent;
class UEnemyDefinition;
class UHealthComponent;
class UMaterialInstanceDynamic;
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

	/**
	 * M3-027 test seam: true while the hit flash tints the mesh materials
	 * (the victim-side visible reaction; active exactly while the shared
	 * snapshot holds this enemy in HitStun).
	 */
	bool IsHitFlashActive() const { return bHitFlashActive; }

	/**
	 * M3-027 test seam: the single-node animation asset currently presented
	 * on the mesh (the idle or the jog clip; null before the first switch).
	 */
	UAnimationAsset* PeekLocomotionAsset() const { return CurrentLocomotionAsset.Get(); }

	/**
	 * M3-027 test seam: the flash tint the overlay material carries while the
	 * flash is active (read back from the first inherited vector parameter
	 * the flash wrote; white when no flash overlay is applied).
	 */
	FLinearColor PeekHitFlashTint() const;

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
	/**
	 * M3-027: applies/clears the hit flash on the mesh (presentation only:
	 * the mesh's own slot-0 material as the overlay, with every inherited
	 * vector parameter carrying the flash red while the hit stun holds).
	 */
	void ApplyHitFlash(bool bActive);

	/**
	 * M3-027: switches the single-node animation between the idle and the jog
	 * clip from the horizontal speed (a moving enemy that glides in the idle
	 * loop reads as "not moving" - the user's fourth-round feedback).
	 */
	void RefreshLocomotionAnimation();

	/**
	 * M5-018C: swaps the single-node mesh to the hit-reaction clip while the
	 * accepted hit holds this enemy in HitStun and back to the locomotion
	 * assets on the stun-end edge. Montage playback (the M5-017 presenter path
	 * this enemy also carries) is inert on a single-node mesh - the engine's
	 * PlayAnimMontage only works through an Animation Graph AnimInstance - so
	 * this direct asset swap is the enemy's visible reaction, mirroring the
	 * M5-017 mapping's source clip.
	 */
	void ApplyHitReactAnimation(bool bActive);

	/**
	 * M5-018C: plays the death clip once inside the existing corpse window
	 * (called from the health death lambda; presentation only).
	 */
	void ApplyDeathReactAnimation();

	/**
	 * M5-018C: the launched landing shows the M1-026 downed state - the mesh
	 * eases to a lying rotation while the combat state holds Knockdown, eases
	 * back upright during Recovering, and is force-restored to the template
	 * baseline once Free (no knockdown animation asset exists, so the mesh
	 * pose is the honest placeholder; presentation only - the mesh has no
	 * collision and the combat pipeline is unaffected).
	 */
	void ApplyKnockdownPose(bool bKnockedDown, bool bRecovering, float DeltaSeconds);

	UPROPERTY(VisibleAnywhere, Category = "Combat")
	TObjectPtr<UHealthComponent> Health;

	/** M1-020: hit stun / death priority state for this enemy. */
	UPROPERTY(VisibleAnywhere, Category = "Combat")
	TObjectPtr<UCombatComponent> Combat;

	/**
	 * M5-018C: this enemy's own montage presenter (the same UCLASS the player
	 * pawn attaches, M1-032/M5-017). Sources are injected in BeginPlay: the
	 * component follows this enemy's combat component, so an accepted hit
	 * staggers through the mapped RCT_Hit montage and a death through
	 * RCT_Dead - the visible victim feedback beyond the M3-027 flash, which
	 * the M5-H01 playtest read as missing.
	 */
	UPROPERTY(VisibleAnywhere, Category = "Combat")
	TObjectPtr<UCombatPresentationComponent> Presentation;

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

	// ---------------- M3-027 hit feedback / locomotion presentation ----------------

	/**
	 * M3-027: the flash overlay material (a dynamic instance of the mesh's
	 * own slot-0 material with every inherited vector parameter carrying the
	 * flash red, written once at BeginPlay; rendered through the mesh's
	 * SetOverlayMaterial while the hit stun holds). The engine
	 * BasicShapeMaterial proved unusable here: as a skeletal-mesh overlay it
	 * silently renders nothing (static-mesh usage only - the render probe
	 * screenshot is pixel-identical to the unmodified mesh), while the
	 * slot-0-based overlay renders the whole body red.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> FlashOverlayMaterial;

	/**
	 * M3-027: the first inherited vector parameter of the overlay material
	 * (the test-seam read-back anchor; none when the material exposes no
	 * vector parameters).
	 */
	FName FlashTintParameterName;

	/** M3-027: flash state (follows the snapshot's HitStun transitions). */
	bool bHitFlashActive = false;

	/**
	 * M3-027: set by the vertical launch path (LaunchCharacter Z > 0, the
	 * M1-022 ATrainingEnemy marker pattern); a launched landing runs the
	 * M1-026 knockdown/recovery process so the launcher visibly ends its
	 * arc on a downed enemy. Consumed only by an accepted process.
	 */
	bool bLaunchedAirborne = false;

	/** M3-027: single-node locomotion assets (class-referenced, cook-covered). */
	UPROPERTY(Transient)
	TObjectPtr<UAnimationAsset> IdleLocomotionAsset;

	UPROPERTY(Transient)
	TObjectPtr<UAnimationAsset> RunLocomotionAsset;

	/** M3-027: the asset currently presented on the mesh. */
	UPROPERTY(Transient)
	TObjectPtr<UAnimationAsset> CurrentLocomotionAsset;

	// ---------------- M5-018C visible victim reactions (single-node) ----------------

	/**
	 * M5-018C: the hit-reaction clip the mesh swaps to while the accepted hit
	 * holds this enemy in HitStun (the MM_HitReact_Front_Hvy_01 template
	 * sequence the M5-017 RCT_Hit montage wraps). Class-referenced and
	 * cook-covered like the locomotion assets above; null keeps the flash as
	 * the only victim feedback.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UAnimationAsset> HitReactAnimationAsset;

	/**
	 * M5-018C: the death clip played once when the health pool dies (the
	 * MM_Death_Back_01 template sequence the M5-017 RCT_Dead montage wraps),
	 * inside the existing corpse window.
	 */
	UPROPERTY(Transient)
	TObjectPtr<UAnimationAsset> DeathReactAnimationAsset;

	/**
	 * M5-018C: true while the single-node mesh plays the hit-reaction clip.
	 * RefreshLocomotionAnimation stays suspended so it cannot override the
	 * reaction, and the stun-end edge re-issues the idle/jog swap.
	 */
	bool bHitReactAnimationActive = false;

	/**
	 * M5-018C: true while the mesh is rotated into the launched-landing downed
	 * pose; the Free edge restores the template baseline exactly once.
	 */
	bool bKnockdownPoseActive = false;
};
