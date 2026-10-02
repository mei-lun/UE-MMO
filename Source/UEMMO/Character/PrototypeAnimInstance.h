#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "PrototypeAnimInstance.generated.h"

class UAnimSequenceBase;
class UBlendSpace;
class UBlueprint;

/**
 * M1-031: native anim instance driving the eight-direction locomotion layer.
 * Every frame it exports planar speed, facing-local velocity and the jump /
 * fall / land pose state; the generated AnimBP blends a 2D locomotion
 * BlendSpace against jump, fall and land poses from these values. Replaces the
 * M0 per-frame PlayAnimation override (idle/move switching no longer restarts
 * from time zero). Pure helpers are static and world-less for automation tests.
 */
UCLASS()
class UEMMO_API UPrototypeAnimInstance : public UAnimInstance
{
    GENERATED_BODY()
public:
    /** Pose the AnimBP blend list selects; indexes match the constants below. */
    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    int32 PoseIndex = Pose_Locomotion;

    /** Planar speed magnitude in cm/s (Z ignored). */
    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    float Speed = 0.0f;

    /** World velocity in character-local space; X = forward, Y = right. */
    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    FVector2D LocalVelocity = FVector2D::ZeroVector;

    /** Locomotion blend direction in degrees; 0 forward, 90 right, -90 left. */
    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    float Direction = 0.0f;

    /** True while the character movement reports falling. */
    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    bool IsFalling = false;

    /** True above the moving speed threshold (drives idle vs move blending). */
    UPROPERTY(BlueprintReadOnly, Category = "Locomotion")
    bool IsMoving = false;

    virtual void NativeUpdateAnimation(float DeltaSeconds) override;

    /**
     * Editor-only gap filler: builds the locomotion AnimGraph inside the given
     * AnimBP (locomotion BlendSpace player plus jump/fall/land sequence
     * players feeding one pose blend list, routed through the DefaultSlot
     * montage slot before the root so M1-032 montages render - M3-024) and
     * configures the BlendSpace axis ranges. UE Python cannot author
     * animation graphs because graph pins are
     * not UObjects and pin allocation exposes no Python-callable UFunction, so
     * the asset script calls this editor helper, which uses the same engine
     * editor APIs (FGraphNodeCreator, UAnimGraphNode_*) the AnimGraph editor
     * itself uses. Idempotent: graphs built before the slot existed get the
     * slot inserted between the blend list and the root; graphs that already
     * route through a slot are left unchanged. Returns true when the graph is
     * present after the call.
     */
    UFUNCTION(BlueprintCallable, Category = "Editor Scripting")
    static bool EditorBuildLocomotionGraph(UBlueprint* Blueprint, UBlendSpace* LocomotionBlendSpace, UAnimSequenceBase* JumpSequence, UAnimSequenceBase* FallSequence, UAnimSequenceBase* LandSequence);

    // Pose indexes the AnimBP blend list consumes; the asset graph is authored
    // against this exact order.
    static constexpr int32 Pose_Locomotion = 0;
    static constexpr int32 Pose_Jump = 1;
    static constexpr int32 Pose_Fall = 2;
    static constexpr int32 Pose_Land = 3;

    // Below this planar speed (cm/s) the character counts as idle.
    static constexpr float MovingThreshold = 10.0f;
    // Above this vertical speed (cm/s) an airborne character counts as rising
    // (jump start); at or below it counts as falling.
    static constexpr float RiseFallSplitSpeed = 50.0f;

    // ---- pure helpers (no World), unit tested as UEMMO.Tasks.M1_031.* ----

    /** M1-029 facing rule: positive axis faces yaw 0, negative faces yaw 180. */
    static float FacingYawFromHorizontalAxis(float AxisX);

    /** Projects world velocity onto the yaw facing: X forward, Y right. */
    static FVector2D ComputeLocalVelocity(const FVector& WorldVelocity, float FacingYaw);

    /** Planar speed magnitude in cm/s; the vertical component is ignored. */
    static float ComputeSpeed(const FVector& WorldVelocity);

    /** atan2(right, forward) in degrees in [-180, 180]; zero input gives 0. */
    static float ComputeLocomotionDirection(const FVector2D& LocalVelocity);

    /** Air pose: jump start while rising, fall loop once the rise is spent. */
    static int32 ComputeAirPoseIndex(bool bIsFalling, float VerticalVelocity);

private:
    // Seconds the land pose holds after touchdown before returning to the
    // locomotion BlendSpace (roughly the land animation's distinct part).
    UPROPERTY(EditAnywhere, Category = "Locomotion")
    float LandPoseDuration = 0.6f;
    // Seconds left on the land pose after touching down.
    float LandTimer = 0.0f;
    // Falling state of the previous update; detects the landing transition.
    bool bWasFalling = false;
};
