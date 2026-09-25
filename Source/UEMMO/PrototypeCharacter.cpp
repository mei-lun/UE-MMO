#include "PrototypeCharacter.h"
#include "Animation/AnimInstance.h"
#include "Character/SideViewCameraComponent.h"
#include "Combat/AttackCatalog.h"
#include "Combat/CombatComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
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
    // M1-031: the generated AnimBP owns all animation state (locomotion
    // BlendSpace plus jump/fall/land poses). The M0 per-frame PlayAnimation
    // override is gone, so idle/move switching never restarts from time zero.
    static ConstructorHelpers::FClassFinder<UAnimInstance> AnimBPClass(TEXT("/Game/UEMMO/Animation/ABP_Prototype.ABP_Prototype_C"));
    if (AnimBPClass.Succeeded())
    {
        GetMesh()->SetAnimationMode(EAnimationMode::AnimationBlueprint);
        GetMesh()->SetAnimInstanceClass(AnimBPClass.Class);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO: /Game/UEMMO/Animation/ABP_Prototype missing; run Scripts/Editor/create_locomotion_assets.py."));
    }
    // M1-030: the fixed side-view framing (yaw -90, pitch -18, FOV 55, arm
    // 1700 cm) and the ground-anchor follow moved into SideViewCameraComponent;
    // those values are preserved as the component's defaults.
    CameraRig = CreateDefaultSubobject<USideViewCameraComponent>(TEXT("SideViewCameraRig"));
    CameraRig->SetupAttachment(RootComponent);
    // M1-012: combat lifecycle component (M1-011); J/K intents buffer here.
    Combat = CreateDefaultSubobject<UCombatComponent>(TEXT("Combat"));
}

void APrototypeCharacter::BeginPlay()
{
    Super::BeginPlay();
    SpawnLocation = GetActorLocation();
    // M1-012 catalog wiring point: build the read-only catalog once from the
    // DefaultGame.ini references (the same loading path M1-010/M1-011 use) and
    // inject it into the combat component. On failure combat intents still
    // buffer; TryStartAttack then rejects every id with its own diagnostic.
    UAttackCatalog* Catalog = NewObject<UAttackCatalog>(this);
    FText CatalogError;
    if (Catalog->InitializeFromConfig(CatalogError))
    {
        Combat->InitializeFromCatalog(Catalog);
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("UEMMO: attack catalog unavailable (%s); combat intents buffer without one."), *CatalogError.ToString());
    }
    UE_LOG(LogTemp, Display, TEXT("UEMMO: prototype character ready; X/Y movement enabled."));
}

void APrototypeCharacter::EnsureCombatInputActions()
{
    if (Mapping != nullptr)
    {
        // M1-012: actions and the mapping context are built exactly once per
        // pawn; a re-setup (re-possess) reuses them so nothing accumulates.
        return;
    }
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
    // M1-012: combat intents (J = Light, K = Launcher). Runtime-created actions
    // keep the M0 pattern: no uasset IMC/IA, keys mapped in code. Only the
    // Started event is bound, so holding a key never repeats and releasing
    // never enqueues.
    CombatLightAction = NewObject<UInputAction>(this, TEXT("CombatLight"));
    CombatLauncherAction = NewObject<UInputAction>(this, TEXT("CombatLauncher"));
    Mapping->MapKey(CombatLightAction, EKeys::J);
    Mapping->MapKey(CombatLauncherAction, EKeys::K);
}

void APrototypeCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);
    UEnhancedInputComponent* Input = CastChecked<UEnhancedInputComponent>(PlayerInputComponent);
    // M1-012 duplicate-binding guard: EnhancedInput stacks a delegate per
    // BindAction call, so re-running setup against the same component would
    // double-trigger. Clearing this pawn's bindings first makes the bind block
    // below idempotent.
    Input->ClearBindingsForObject(this);
    EnsureCombatInputActions();
    Input->BindAction(HorizontalAction, ETriggerEvent::Triggered, this, &APrototypeCharacter::MoveHorizontal);
    Input->BindAction(HorizontalAction, ETriggerEvent::Completed, this, &APrototypeCharacter::MoveHorizontal);
    Input->BindAction(DepthAction, ETriggerEvent::Triggered, this, &APrototypeCharacter::MoveDepth);
    Input->BindAction(DepthAction, ETriggerEvent::Completed, this, &APrototypeCharacter::MoveDepth);
    Input->BindAction(JumpAction, ETriggerEvent::Started, this, &APrototypeCharacter::StartJump);
    Input->BindAction(JumpAction, ETriggerEvent::Completed, this, &APrototypeCharacter::EndJump);
    Input->BindAction(ResetAction, ETriggerEvent::Started, this, &APrototypeCharacter::ResetPosition);
    Input->BindAction(CombatLightAction, ETriggerEvent::Started, this, &APrototypeCharacter::OnCombatLightPressed);
    Input->BindAction(CombatLauncherAction, ETriggerEvent::Started, this, &APrototypeCharacter::OnCombatLauncherPressed);
    if (APlayerController* PC = Cast<APlayerController>(Controller))
    {
        if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
        {
            // M1-012: the context object is a per-pawn singleton (see
            // EnsureCombatInputActions); registering it twice is pointless, so
            // a re-setup never re-adds it.
            if (!Subsystem->HasMappingContext(Mapping))
            {
                Subsystem->AddMappingContext(Mapping, 0);
            }
        }
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
void APrototypeCharacter::OnCombatLightPressed() { SubmitCombatInput(ECombatInput::Light); }
void APrototypeCharacter::OnCombatLauncherPressed() { SubmitCombatInput(ECombatInput::Launcher); }
void APrototypeCharacter::SubmitCombatInput(ECombatInput Action)
{
    const UWorld* World = GetWorld();
    // Input game clock (interface contract section 2): World GetTimeSeconds
    // advances with normal game time only, so pause and hit stop do not
    // advance it. World-less callers (early tests) read 0.0.
    SubmitCombatInput(Action, World ? World->GetTimeSeconds() : 0.0);
}
void APrototypeCharacter::SubmitCombatInput(ECombatInput Action, double PressedAt)
{
    if (Combat == nullptr)
    {
        return;
    }
    FBufferedCombatInput Intent;
    Intent.Sequence = NextCombatInputSequence++;
    Intent.Action = Action;
    Intent.PressedAt = PressedAt;
    // Rejections (duplicate/regressing sequence, non-finite time) are only
    // ignored: the character counter stays monotonic either way.
    Combat->QueueInput(Intent);
}
void APrototypeCharacter::ResetPosition()
{
    GetCharacterMovement()->StopMovementImmediately();
    SetActorLocation(SpawnLocation, false, nullptr, ETeleportType::TeleportPhysics);
}
void APrototypeCharacter::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    // M1-012: the combat component advances its own 60 Hz action clock from
    // here. Buffer consumption is NOT wired yet (M1-014 and later tasks own
    // that link; TryStartAttack still has no game-side caller).
    if (Combat != nullptr)
    {
        Combat->TickCombat(DeltaSeconds);
    }
    ApplyPlanarMovement(*this, PlanarAxes);
    if (GetActorLocation().Z < -1000.f) ResetPosition();
}
