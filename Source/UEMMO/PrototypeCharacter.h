#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Character/PlanarMovement.h"
#include "Combat/CombatInputBuffer.h"
#include "PrototypeCharacter.generated.h"

class USideViewCameraComponent;
class UCombatComponent;
class UCombatPresentationComponent;
class UInputAction;
class UInputMappingContext;
struct FInputActionValue;

/** M0 movement scaffold. M1-012 routes J/K combat intents into the combat component. */
UCLASS()
class UEMMO_API APrototypeCharacter : public ACharacter
{
    GENERATED_BODY()
public:
    APrototypeCharacter();
    virtual void Tick(float DeltaSeconds) override;

    /** M1-012: the combat subobject every submitted intent is buffered into. */
    UCombatComponent* GetCombat() const { return Combat; }

    /**
     * M1-012: public combat intent entry (J = Light, K = Launcher). PressedAt
     * is read from the input game clock (World GetTimeSeconds: advances with
     * normal game time only, so pause and hit stop do not advance it) and the
     * sequence comes from a character-level counter starting at 1. Push
     * rejections (duplicate/regressing sequence) are silently ignored.
     */
    void SubmitCombatInput(ECombatInput Action);

    /** Explicit-clock variant for early tests (interface contract section 2). */
    void SubmitCombatInput(ECombatInput Action, double PressedAt);

    /**
     * Standard pawn input setup (binding point for movement, jump, reset and
     * the M1-012 combat actions). Public on this class so the automation test
     * can drive a second setup call and prove the idempotency guard.
     */
    virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

    /**
     * M1-023: the Space entry (Enhanced Input Started binding). Public so the
     * automation tests can drive the exact binding target: the press is
     * buffered into the combat component (never a direct state-bypassing
     * jump); the component's state decides Free jump vs launcher jump-cancel
     * vs keep-buffered.
     */
    void StartJump();

protected:
    virtual void BeginPlay() override;
private:
    void MoveHorizontal(const FInputActionValue& Value);
    void MoveDepth(const FInputActionValue& Value);
    void EndJump();
    void ResetPosition();
    // M1-012: builds the mapping context and actions exactly once (guarded by
    // Mapping != nullptr); re-setup (re-possess) reuses them.
    void EnsureCombatInputActions();
    void OnCombatLightPressed();
    void OnCombatLauncherPressed();
    // M1-030: single component owning the fixed side-view rig and the ground
    // anchor follow (replaces the M0 CameraBoom/Camera pair).
    UPROPERTY(VisibleAnywhere) TObjectPtr<USideViewCameraComponent> CameraRig;
    // M1-012: combat lifecycle component (M1-011); intents buffer here.
    UPROPERTY(VisibleAnywhere) TObjectPtr<UCombatComponent> Combat;
    // M1-032: owns attack montage playback; follows Combat's
    // Started/Finished events plus a per-tick snapshot fallback.
    UPROPERTY(VisibleAnywhere) TObjectPtr<UCombatPresentationComponent> CombatPresentation;
    UPROPERTY() TObjectPtr<UInputMappingContext> Mapping;
    UPROPERTY() TObjectPtr<UInputAction> HorizontalAction;
    UPROPERTY() TObjectPtr<UInputAction> DepthAction;
    UPROPERTY() TObjectPtr<UInputAction> JumpAction;
    UPROPERTY() TObjectPtr<UInputAction> ResetAction;
    // M1-012: combat actions (J = Light, K = Launcher); created with the M0
    // runtime-action pattern, only the Started event is bound.
    UPROPERTY(Transient) TObjectPtr<UInputAction> CombatLightAction;
    UPROPERTY(Transient) TObjectPtr<UInputAction> CombatLauncherAction;
    FVector SpawnLocation;
    // M1-029: accumulated planar axis input; applied centrally in Tick.
    UE::UEMMO::Tasks::M1_029::FPlanarAxisState PlanarAxes;
    // M1-012: next combat input sequence; strictly increases per submitted intent.
    uint64 NextCombatInputSequence = 1;
};
