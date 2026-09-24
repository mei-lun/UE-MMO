#include "PrototypeCharacter.h"
#include "Animation/AnimSequence.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/LocalPlayer.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "UObject/ConstructorHelpers.h"

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
    CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
    CameraBoom->SetupAttachment(RootComponent);
    CameraBoom->SetUsingAbsoluteRotation(true);
    CameraBoom->SetRelativeRotation(FRotator(-18.f, -90.f, 0.f));
    CameraBoom->TargetArmLength = 1700.f;
    CameraBoom->TargetOffset = FVector(0, 0, 100);
    CameraBoom->bDoCollisionTest = false;
    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("SideCamera"));
    Camera->SetupAttachment(CameraBoom);
    Camera->FieldOfView = 55.f;
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
    Input->BindAction(DepthAction, ETriggerEvent::Triggered, this, &APrototypeCharacter::MoveDepth);
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
    const float Axis = Value.Get<float>();
    AddMovementInput(FVector::ForwardVector, Axis);
    if (!FMath::IsNearlyZero(Axis)) SetActorRotation(FRotator(0, Axis > 0 ? 0 : 180, 0));
}
void APrototypeCharacter::MoveDepth(const FInputActionValue& Value) { AddMovementInput(FVector::RightVector, Value.Get<float>()); }
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
