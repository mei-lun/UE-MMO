#include "SideViewCameraComponent.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"

using namespace UE::UEMMO::Tasks::M1_030;

USideViewCameraComponent::USideViewCameraComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    bDoCollisionTest = false;
    bUsePawnControlRotation = false;
    // M0 framing: absolute world rotation, so pawn rotation (facing flips)
    // never orbits the camera. The pivot height above the ground lives in
    // Config.PivotHeightAboveGround, so the boom target offset stays zero.
    SetUsingAbsoluteRotation(true);
    TargetOffset = FVector::ZeroVector;
    TargetArmLength = Config.ArmLength;
    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("SideViewCamera"));
    Camera->SetupAttachment(this);
    Camera->bUsePawnControlRotation = false;
    Camera->FieldOfView = Config.FieldOfView;
}

void USideViewCameraComponent::BeginPlay()
{
    Super::BeginPlay();
    ApplyConfig();
    InitializeAnchor();
}

void USideViewCameraComponent::ApplyConfig()
{
    TargetArmLength = Config.ArmLength;
    // ComputeCameraRotation ignores facing by contract: yaw and pitch are the
    // fixed side-view framing and are applied once here, never per frame.
    SetRelativeRotation(ComputeCameraRotation(0.0f));
    if (Camera)
    {
        Camera->FieldOfView = Config.FieldOfView;
    }
}

bool USideViewCameraComponent::TraceGroundZ(float& OutHitZ)
{
    OutHitZ = 0.0f;
    UWorld* World = GetWorld();
    const AActor* Owner = GetOwner();
    if (!World || !Owner || Config.GroundTraceDistance <= 0.0f)
    {
        return false;
    }
    FVector Start = Owner->GetActorLocation();
    Start.Z += GroundTraceStartClearance;
    const FVector End(Start.X, Start.Y, Start.Z - Config.GroundTraceDistance);
    FHitResult Hit;
    FCollisionQueryParams Params(TEXT("SideViewCameraGroundTrace"), /*bTraceComplex*/ false, Owner);
    if (!World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params))
    {
        return false;
    }
    OutHitZ = Hit.ImpactPoint.Z;
    return FMath::IsFinite(OutHitZ);
}

void USideViewCameraComponent::InitializeAnchor()
{
    const AActor* Owner = GetOwner();
    const FVector PawnLocation = Owner ? Owner->GetActorLocation() : FVector::ZeroVector;
    // Reference for the damped Y follow: the pawn depth at camera startup.
    ReferenceY = PawnLocation.Y;
    // Fallback ground before any trace: assume the pawn stands on the floor
    // (capsule center minus half height); a successful trace overwrites it.
    float InitialGroundZ = PawnLocation.Z;
    if (const ACharacter* Character = Cast<ACharacter>(Owner))
    {
        if (const UCapsuleComponent* Capsule = Character->GetCapsuleComponent())
        {
            InitialGroundZ -= Capsule->GetScaledCapsuleHalfHeight();
        }
    }
    float HitZ = 0.0f;
    GroundZ = ResolveGroundZ(TraceGroundZ(HitZ), HitZ, InitialGroundZ);
    CurrentAnchor = ComputeAnchorLocation(PawnLocation, GroundZ, ReferenceY, Config);
    SetWorldLocation(CurrentAnchor);
}

void USideViewCameraComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    const AActor* Owner = GetOwner();
    if (!Owner)
    {
        return;
    }
    const FVector PawnLocation = Owner->GetActorLocation();
    // Ground Z only: an airborne pawn keeps reporting the floor below, so the
    // anchor Z does not follow jumps. A missed trace keeps the last known Z.
    float HitZ = 0.0f;
    GroundZ = ResolveGroundZ(TraceGroundZ(HitZ), HitZ, GroundZ);
    const FVector TargetAnchor = ComputeAnchorLocation(PawnLocation, GroundZ, ReferenceY, Config);
    FVector Anchor = CurrentAnchor;
    // Only X is smoothed; Y is already the damped ratio value and Z must stay
    // locked to the ground so jumps do not wobble the camera.
    Anchor.X = SmoothFollowAxis(Anchor.X, TargetAnchor.X, DeltaTime, Config.FollowSpeed);
    Anchor.Y = TargetAnchor.Y;
    Anchor.Z = TargetAnchor.Z;
    CurrentAnchor = Anchor;
    SetWorldLocation(CurrentAnchor);
}
