#include "PrototypeCharacter.h"
#include "Animation/AnimSequence.h"
#include "Character/SideViewCameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/LocalPlayer.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "UObject/ConstructorHelpers.h"

using namespace UE::UEMMO::Tasks::M1_029;

namespace
{
    /**
     * Single choke point for all planar movement: every frame the accumulated
     * axis state is converted into movement input and facing here. This is the
     * spot where M1-013 attack gating (CanAcceptMovement) plugs in once
     * implemented; nothing else in this class feeds movement input.
     */
    void ApplyPlanarMovement(APrototypeCharacter& Character, const FPlanarAxisState& Axes)
    {
        const FVector PlanarVelocity = ComputePlanarVelocity(Axes);
        UCharacterMovementComponent* Movement = Character.GetCharacterMovement();
        const float Speed = PlanarVelocity.Size2D();
        if (Speed > UE_SMALL_NUMBER && Movement && Movement->MaxWalkSpeed > UE_SMALL_NUMBER)
        {
            // Normalized direction plus an input scale of speed / MaxWalkSpeed makes
            // CharacterMovement clamp its walk speed to the per-axis planar speed
            // (analog input modifier), keeping normal acceleration, braking and collision.
            Character.AddMovementInput(PlanarVelocity / Speed, Speed / Movement->MaxWalkSpeed);
        }
        // Only horizontal input changes facing; depth input (W/S) never flips it.
        if (Axes.AxisX > 0.0f)
        {
            Character.SetActorRotation(FRotator(0.f, 0.f, 0.f));
        }
        else if (Axes.AxisX < 0.0f)
        {
            Character.SetActorRotation(FRotator(0.f, 180.f, 0.f));
        }
    }
}

APrototypeCharacter::APrototypeCharacter()
{
    PrimaryActorTick.bCanEverTick = true;
    GetCapsuleComponent()->InitCapsuleSize(36.f, 96.f);
    bUseControllerRotationYaw = false;
    GetCharacterMovement()->bOrientRotationToMovement = false;
    GetCharacterMovement()->bConstrainToPlane = false;
    GetCharacterMovement()->MaxWalkSpeed = 420.f;
    GetCharacterMovement()->JumpZVelocity = 620.f;
    GetCharacterMovement()->GravityScale = 1.5f;
    GetCharacterMovement()->AirControl = 0.35f;
    GetCharacterMovement()->BrakingDecelerationWalking = 2400.f;
    GetMesh()->SetRelativeLocation(FVector(0, 0, -96));
    GetMesh()->SetRelativeRotation(FRotator(0, -90, 0));
    GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    static ConstructorHelpers::FObjectFinder<USkeletalMesh> MeshAsset(TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"));
    if (MeshAsset.Succeeded()) GetMesh()->SetSkeletalMesh(MeshAsset.Object);
    static ConstructorHelpers::FObjectFinder<UAnimSequence> Idle(TEXT("/Game/Characters/Mannequins/Anims/Unarmed/MM_Idle.MM_Idle"));
    static ConstructorHelpers::FObjectFinder<UAnimSequence> Run(TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Jog/MF_Unarmed_Jog_Fwd.MF_Unarmed_Jog_Fwd"));
    static ConstructorHelpers::FObjectFinder<UAnimSequence> Fall(TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Jump/MM_Fall_Loop.MM_Fall_Loop"));
    IdleAnimation = Idle.Object;
    RunAnimation = Run.Object;
    FallAnimation = Fall.Object;
    // M1-030: the fixed side-view framing (yaw -90, pitch -18, FOV 55, arm
    // 1700 cm) and the ground-anchor follow moved into SideViewCameraComponent;
    // those values are preserved as the component's defaults.
    CameraRig = CreateDefaultSubobject<USideViewCameraComponent>(TEXT("SideViewCameraRig"));
    CameraRig->SetupAttachment(RootComponent);
}

void APrototypeCharacter::BeginPlay()
{
    Super::BeginPlay();
    SpawnLocation = GetActorLocation();
    UpdateAnimation();
    UE_LOG(LogTemp, Display, TEXT("UEMMO: prototype character ready; X/Y movement enabled."));
}

void APrototypeCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);
    UEnhancedInputComponent* Input = CastChecked<UEnhancedInputComponent>(PlayerInputComponent);
    Mapping = NewObject<UInputMappingContext>(this);
    auto MakeAxis = [this](const TCHAR* Name, FKey Positive, FKey Negative)
    {
        UInputAction* Action = NewObject<UInputAction>(this, Name);
        Action->ValueType = EInputActionValueType::Axis1D;
        Action->AccumulationBehavior = EInputActionAccumulationBehavior::Cumulative;
        Mapping->MapKey(Action, Positive);
        FEnhancedActionKeyMapping& NegativeMapping = Mapping->MapKey(Action, Negative);
        NegativeMapping.Modifiers.Add(NewObject<UInputModifierNegate>(Mapping));
        return Action;
    };
    HorizontalAction = MakeAxis(TEXT("MoveHorizontal"), EKeys::D, EKeys::A);
    DepthAction = MakeAxis(TEXT("MoveDepth"), EKeys::S, EKeys::W);
    JumpAction = NewObject<UInputAction>(this, TEXT("Jump"));
    ResetAction = NewObject<UInputAction>(this, TEXT("Reset"));
    Mapping->MapKey(JumpAction, EKeys::SpaceBar);
    Mapping->MapKey(ResetAction, EKeys::R);
    Input->BindAction(HorizontalAction, ETriggerEvent::Triggered, this, &APrototypeCharacter::MoveHorizontal);
    Input->BindAction(HorizontalAction, ETriggerEvent::Completed, this, &APrototypeCharacter::MoveHorizontal);
    Input->BindAction(DepthAction, ETriggerEvent::Triggered, this, &APrototypeCharacter::MoveDepth);
    Input->BindAction(DepthAction, ETriggerEvent::Completed, this, &APrototypeCharacter::MoveDepth);
    Input->BindAction(JumpAction, ETriggerEvent::Started, this, &APrototypeCharacter::StartJump);
    Input->BindAction(JumpAction, ETriggerEvent::Completed, this, &APrototypeCharacter::EndJump);
    Input->BindAction(ResetAction, ETriggerEvent::Started, this, &APrototypeCharacter::ResetPosition);
    if (APlayerController* PC = Cast<APlayerController>(Controller))
    {
        if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
            Subsystem->AddMappingContext(Mapping, 0);
    }
}

void APrototypeCharacter::MoveHorizontal(const FInputActionValue& Value)
{
    // Enhanced Input already sums the mapped keys (D and negated A) into one net
    // axis; Triggered carries it while held and Completed carries 0 on release.
    // The value is only recorded here; movement is applied centrally in Tick.
    PlanarAxes.SetAxisX(Value.Get<float>());
}
void APrototypeCharacter::MoveDepth(const FInputActionValue& Value) { PlanarAxes.SetAxisY(Value.Get<float>()); }
void APrototypeCharacter::StartJump() { Jump(); }
void APrototypeCharacter::EndJump() { StopJumping(); }
void APrototypeCharacter::ResetPosition()
{
    GetCharacterMovement()->StopMovementImmediately();
    SetActorLocation(SpawnLocation, false, nullptr, ETeleportType::TeleportPhysics);
}
void APrototypeCharacter::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    ApplyPlanarMovement(*this, PlanarAxes);
    if (GetActorLocation().Z < -1000.f) ResetPosition();
    UpdateAnimation();
}
void APrototypeCharacter::UpdateAnimation()
{
    UAnimSequence* Desired = GetCharacterMovement()->IsFalling() ? FallAnimation.Get()
        : (GetVelocity().SizeSquared2D() > 100.f ? RunAnimation.Get() : IdleAnimation.Get());
    if (Desired && Desired != ActiveAnimation)
    {
        ActiveAnimation = Desired;
        GetMesh()->PlayAnimation(Desired, true);
    }
}
