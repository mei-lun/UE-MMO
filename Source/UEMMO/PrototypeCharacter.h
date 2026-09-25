#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Character/PlanarMovement.h"
#include "PrototypeCharacter.generated.h"

class USideViewCameraComponent;
class UInputAction;
class UInputMappingContext;
class UAnimSequence;
struct FInputActionValue;

/** M0 movement scaffold. Combat belongs to a later milestone. */
UCLASS()
class UEMMO_API APrototypeCharacter : public ACharacter
{
    GENERATED_BODY()
public:
    APrototypeCharacter();
    virtual void Tick(float DeltaSeconds) override;
protected:
    virtual void BeginPlay() override;
    virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
private:
    void MoveHorizontal(const FInputActionValue& Value);
    void MoveDepth(const FInputActionValue& Value);
    void StartJump();
    void EndJump();
    void ResetPosition();
    void UpdateAnimation();
    // M1-030: single component owning the fixed side-view rig and the ground
    // anchor follow (replaces the M0 CameraBoom/Camera pair).
    UPROPERTY(VisibleAnywhere) TObjectPtr<USideViewCameraComponent> CameraRig;
    UPROPERTY() TObjectPtr<UInputMappingContext> Mapping;
    UPROPERTY() TObjectPtr<UInputAction> HorizontalAction;
    UPROPERTY() TObjectPtr<UInputAction> DepthAction;
    UPROPERTY() TObjectPtr<UInputAction> JumpAction;
    UPROPERTY() TObjectPtr<UInputAction> ResetAction;
    UPROPERTY() TObjectPtr<UAnimSequence> IdleAnimation;
    UPROPERTY() TObjectPtr<UAnimSequence> RunAnimation;
    UPROPERTY() TObjectPtr<UAnimSequence> FallAnimation;
    UPROPERTY() TObjectPtr<UAnimSequence> ActiveAnimation;
    FVector SpawnLocation;
    // M1-029: accumulated planar axis input; applied centrally in Tick.
    UE::UEMMO::Tasks::M1_029::FPlanarAxisState PlanarAxes;
};
