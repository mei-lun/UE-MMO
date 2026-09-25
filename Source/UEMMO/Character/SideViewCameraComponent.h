#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "SideViewCameraComponent.generated.h"

class UCameraComponent;

// M1-030: componentized side-view camera. The rig keeps the M0 fixed framing
// (yaw -90, downward pitch -18, FOV 55, arm length 1700 cm) and never orbits
// the pawn: facing flips only mirror the pawn mesh. The spring arm pivot
// tracks a ground anchor: the anchor Z comes from a downward ground trace (an
// airborne pawn never lifts the camera), X follows smoothly and clamps to
// room bounds, Y follows only YFollowRatio (25% by default). The pure math
// lives in the namespace below and is unit tested without a World.

/** Fixed framing parameters and follow tuning for USideViewCameraComponent. */
USTRUCT(BlueprintType)
struct FSideViewCameraConfig
{
    GENERATED_BODY()

    /** Camera field of view (M0 value kept). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Side View Camera", meta = (ClampMin = "1.0", ClampMax = "179.0"))
    float FieldOfView = 55.0f;

    /** Spring arm length in cm (M0 value kept). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Side View Camera", meta = (ClampMin = "0.0"))
    float ArmLength = 1700.0f;

    /** Anchor pivot height above the traced ground in cm (M0: 96 capsule half height + 100 boom target offset). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Side View Camera")
    float PivotHeightAboveGround = 196.0f;

    /** Fraction of the pawn's depth (Y) displacement the anchor follows. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Side View Camera", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float YFollowRatio = 0.25f;

    /** Room bounds for the anchor X in world cm; the camera never leaves the room. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Side View Camera")
    float RoomMinX = -1200.0f;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Side View Camera")
    float RoomMaxX = 1200.0f;

    /** Exponential follow speed (1/s) for the anchor X axis; 0 snaps to the target. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Side View Camera", meta = (ClampMin = "0.0"))
    float FollowSpeed = 8.0f;

    /** Downward trace length used to find the ground (cm). */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Side View Camera", meta = (ClampMin = "0.0"))
    float GroundTraceDistance = 100000.0f;
};

/**
 * Componentizes the M0 fixed side-view camera. Derived from
 * USpringArmComponent so the component itself is the boom attached to the
 * pawn root, and it owns the UCameraComponent as a child subobject. The
 * rotation is set once (absolute world rotation, never per frame, facing
 * independent) and each tick only the pivot location moves to the ground
 * anchor computed from the pure functions below.
 */
UCLASS(ClassGroup = Camera, meta = (BlueprintSpawnableComponent))
class UEMMO_API USideViewCameraComponent : public USpringArmComponent
{
    GENERATED_BODY()

public:
    USideViewCameraComponent();

    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

    /** Last known ground Z feeding the anchor (trace hit, or the last fallback). */
    float GetAnchorGroundZ() const { return GroundZ; }

    /** Current anchor pivot location (smoothed X, damped Y, ground-locked Z). */
    const FVector& GetAnchorLocation() const { return CurrentAnchor; }

    /** Tunable framing and follow parameters; defaults keep the M0 values. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Side View Camera")
    FSideViewCameraConfig Config;

private:
    void ApplyConfig();
    bool TraceGroundZ(float& OutHitZ);
    void InitializeAnchor();

    UPROPERTY(VisibleAnywhere, Category = "Side View Camera")
    TObjectPtr<UCameraComponent> Camera;

    FVector CurrentAnchor = FVector::ZeroVector;
    float GroundZ = 0.0f;
    float ReferenceY = 0.0f;
};

namespace UE::UEMMO::Tasks::M1_030
{
    // Fixed world yaw/pitch of the side-view rig; facing never changes them.
    constexpr float SideViewCameraYaw = -90.0f;
    constexpr float SideViewCameraPitch = -18.0f;

    // The ground trace starts this far above the pawn so slight embedding or
    // standing on floor seams still resolves the ground below.
    constexpr float GroundTraceStartClearance = 50.0f;

    /**
     * Clamps the anchor X to the configurable room bounds. A non-finite X has
     * no side in the room and collapses to 0; a degenerate bound pair is
     * normalized so the clamp never inverts.
     */
    inline float ClampToRoomBounds(float X, float MinX, float MaxX)
    {
        if (!FMath::IsFinite(X))
        {
            return 0.0f;
        }
        const float Lo = FMath::IsFinite(MinX) ? MinX : X;
        const float Hi = FMath::IsFinite(MaxX) ? MaxX : X;
        return FMath::Clamp(X, FMath::Min(Lo, Hi), FMath::Max(Lo, Hi));
    }

    /**
     * Damped depth follow: the anchor Y keeps a fixed reference and moves only
     * YFollowRatio (default 25%) of the pawn's Y displacement, so moving 400cm
     * in depth shifts the anchor by about 100cm.
     */
    inline float ApplyYDamping(float ReferenceY, float PawnY, float YFollowRatio)
    {
        const float Ratio = FMath::IsFinite(YFollowRatio) ? FMath::Clamp(YFollowRatio, 0.0f, 1.0f) : 0.0f;
        if (!FMath::IsFinite(ReferenceY))
        {
            return FMath::IsFinite(PawnY) ? PawnY * Ratio : 0.0f;
        }
        if (!FMath::IsFinite(PawnY))
        {
            return ReferenceY;
        }
        return ReferenceY + (PawnY - ReferenceY) * Ratio;
    }

    /**
     * Anchor Z from the ground only: the pawn's airborne Z is deliberately not
     * an input, so an in-place jump cannot lift the camera. The pivot height
     * above the ground (M0: 96 capsule half height + 100 boom target offset)
     * keeps the standing framing identical to M0.
     */
    inline float ComputeAnchorZ(float GroundZ, float HeightAboveGround)
    {
        return GroundZ + HeightAboveGround;
    }

    /**
     * Fixed rig yaw: any facing value (+1 left/right mirrors, 0, anything)
     * yields the same constant -90, so the camera never orbits the pawn.
     */
    inline float ComputeCameraYaw(float FacingSign)
    {
        static_cast<void>(FacingSign); // facing mirrors the pawn mesh only
        return SideViewCameraYaw;
    }

    /** Fixed side-view rotation: downward pitch -18, yaw -90, no roll. */
    inline FRotator ComputeCameraRotation(float FacingSign)
    {
        return FRotator(SideViewCameraPitch, ComputeCameraYaw(FacingSign), 0.0f);
    }

    /**
     * Ground source of the anchor: a successful, finite trace hit feeds the
     * anchor; a missed or non-finite trace keeps the passed fallback (the last
     * known ground Z), so the camera does not jump when the floor is unknown.
     */
    inline float ResolveGroundZ(bool bTraceHit, float TraceHitZ, float FallbackZ)
    {
        if (bTraceHit && FMath::IsFinite(TraceHitZ))
        {
            return TraceHitZ;
        }
        return FMath::IsFinite(FallbackZ) ? FallbackZ : 0.0f;
    }

    /**
     * Exponential smoothing for the anchor X axis (frame-rate independent):
     * each tick moves a 1 - exp(-Speed * DeltaSeconds) fraction of the way to
     * the target. Speed <= 0 or a non-positive DeltaSeconds snaps to the
     * target; non-finite values keep the sane side of the argument.
     */
    inline float SmoothFollowAxis(float Current, float Target, float DeltaSeconds, float FollowSpeed)
    {
        if (!FMath::IsFinite(Target))
        {
            return FMath::IsFinite(Current) ? Current : 0.0f;
        }
        if (!FMath::IsFinite(Current))
        {
            return Target;
        }
        const float SafeDelta = FMath::IsFinite(DeltaSeconds) ? FMath::Max(DeltaSeconds, 0.0f) : 0.0f;
        const float SafeSpeed = FMath::IsFinite(FollowSpeed) ? FMath::Max(FollowSpeed, 0.0f) : 0.0f;
        if (SafeSpeed <= 0.0f || SafeDelta <= 0.0f)
        {
            return Target;
        }
        const float Alpha = 1.0f - FMath::Exp(-SafeSpeed * SafeDelta);
        return Current + (Target - Current) * Alpha;
    }

    /**
     * Target anchor for the camera pivot: X follows the pawn and clamps to the
     * room bounds, Y is the damped 25% follow, Z is ground-locked (the pawn's
     * own Z never enters, so jumps do not lift the camera). The rig applies
     * its X smoothing to this target afterwards.
     */
    inline FVector ComputeAnchorLocation(const FVector& PawnLocation, float GroundZ, float ReferenceY, const FSideViewCameraConfig& Config)
    {
        const float X = ClampToRoomBounds(PawnLocation.X, Config.RoomMinX, Config.RoomMaxX);
        const float Y = ApplyYDamping(ReferenceY, PawnLocation.Y, Config.YFollowRatio);
        const float Z = ComputeAnchorZ(GroundZ, Config.PivotHeightAboveGround);
        return FVector(X, Y, Z);
    }
}
