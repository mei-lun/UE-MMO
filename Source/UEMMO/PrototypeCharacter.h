#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Character/PlanarMovement.h"
#include "Combat/CombatInputBuffer.h"
#include "PrototypeCharacter.generated.h"

class USideViewCameraComponent;
class UCombatComponent;
class UCombatPresentationComponent;
class UTrainingResetService;
class UInputAction;
class UInputMappingContext;
struct FInputActionValue;

/** M0 movement scaffold. M1-012 routes combat intents into the combat component; M1-040 remaps the keys to the DNF layout. */
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
     * M1-012: public combat intent entry (M1-040 keys: X = Light, Z =
     * Launcher). PressedAt
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
     * M1-027: the reset entry is the public reset surface (M1-040: bound to
     * F2; the old R key moved to skill slot 4). With a registered
     * UTrainingResetService the press routes into exactly one unified session
     * reset (player + every registered enemy; the service owns the player
     * physics reset, so nothing runs twice); without a service the M0 local
     * reset stays (bare scaffolding, test worlds). Public because it is the
     * input binding target and the automation tests drive it directly.
     */
    void ResetPosition();

    /**
     * M1-027: the local physics half of the unified reset - zero velocity,
     * teleport back to the captured spawn location and spawn rotation, drop
     * pending launch/impulse forces and re-ground the movement mode. Called
     * by UTrainingResetService (phase 2) and as the service-less fallback of
     * ResetPosition; it never triggers a session reset itself, so the two
     * entry points cannot recurse into each other.
     */
    void ApplyTrainingRoomReset();

    /**
     * M1-027: injects the session reset service the F2-key path routes into
     * (the room owner / automation tests register the participants on the
     * service and hand it to its pawn here). Passing null restores the M0
     * fallback behavior.
     */
    void SetTrainingResetService(UTrainingResetService* InService);

    /**
     * M1-023: the Jump entry (Enhanced Input Started binding; M1-040 keys: C
     * primary with Space retained as the alias). Public so the
     * automation tests can drive the exact binding target: the press is
     * buffered into the combat component (never a direct state-bypassing
     * jump); the component's state decides Free jump vs launcher jump-cancel
     * vs keep-buffered.
     */
    void StartJump();

    /**
     * M1-040: DNF skill-slot entry (slot 1..8 = Q W E R A S D F in that
     * order). The press only lands in a per-slot counter plus a Verbose log -
     * no combat effect yet (M2+ owns skill execution). Out-of-range slots are
     * ignored. Public because it is the input binding target and the
     * automation tests drive it directly.
     */
    void SubmitSkillSlot(int32 SlotIndex);

    /** M1-040: how often skill slot 1..8 was pressed since spawn (0 out of range). */
    int32 GetSkillSlotPressCount(int32 SlotIndex) const;

    /**
     * M1-040: the eight runtime skill-slot actions in slot order (index 0 is
     * slot 1 = Q, index 7 is slot 8 = F), read-only for the automation tests'
     * mapping-table assertions.
     */
    const TArray<TObjectPtr<UInputAction>>& GetSkillSlotActions() const { return SkillSlotActions; }

    /**
     * M1-028: the runtime mapping context built by EnsureCombatInputActions,
     * read-only. The automation smoke asserts the F1 debug-toggle mapping
     * exists on it exactly once.
     */
    const UInputMappingContext* GetRuntimeInputMappingContext() const { return Mapping; }

    /**
     * M1-028: the F1 press entry (Enhanced Input Started binding). Toggles the
     * local HUD's combat debug overlay through the possessing player
     * controller; pure display, no combat state change. Public because it is
     * the input binding target.
     */
    void OnDebugTogglePressed();

protected:
    virtual void BeginPlay() override;
private:
    void MoveHorizontal(const FInputActionValue& Value);
    void MoveDepth(const FInputActionValue& Value);
    void EndJump();
    // M1-012: builds the mapping context and actions exactly once (guarded by
    // Mapping != nullptr); re-setup (re-possess) reuses them.
    void EnsureCombatInputActions();
    void OnCombatLightPressed();
    void OnCombatLauncherPressed();
    // M1-040: per-slot Started handlers (one member per action keeps the
    // member-pointer binding form, so ClearBindingsForObject keeps covering
    // every skill-slot binding on a re-setup).
    void OnSkillSlot1Pressed();
    void OnSkillSlot2Pressed();
    void OnSkillSlot3Pressed();
    void OnSkillSlot4Pressed();
    void OnSkillSlot5Pressed();
    void OnSkillSlot6Pressed();
    void OnSkillSlot7Pressed();
    void OnSkillSlot8Pressed();
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
    // M1-028: F1 debug-overlay toggle action (same runtime-action pattern).
    UPROPERTY(Transient) TObjectPtr<UInputAction> DebugToggleAction;
    // M1-040: the eight DNF skill-slot actions in slot order (Q W E R A S D F
    // = slot 1..8); created with the same runtime-action pattern.
    UPROPERTY(Transient) TArray<TObjectPtr<UInputAction>> SkillSlotActions;
    FVector SpawnLocation;
    // M1-027: the facing captured with the spawn point; the unified reset
    // restores it alongside the position (interface contract section 6).
    FRotator SpawnRotation = FRotator::ZeroRotator;
    // M1-027: the registered session reset service (weak: a destroyed service
    // falls the R-key path back to the M0 local reset); empty = fallback.
    TWeakObjectPtr<UTrainingResetService> TrainingResetService;
    // M1-029: accumulated planar axis input; applied centrally in Tick.
    UE::UEMMO::Tasks::M1_029::FPlanarAxisState PlanarAxes;
    // M1-012: next combat input sequence; strictly increases per submitted intent.
    uint64 NextCombatInputSequence = 1;
    // M1-040: per-slot press counters (slot 1..8 at index 0..7); the skill
    // slots have no combat effect yet, so this counter is their surface.
    int32 SkillSlotPressCounts[8] = {};
};
