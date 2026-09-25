#include "Misc/AutomationTest.h"

#include "../Character/PrototypeAnimInstance.h"

#include "Animation/BlendSpace.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    constexpr float TestTolerance = 0.01f;
    // Side/depth moves run at 280 cm/s (M1-029 Y speed); used as realistic magnitudes.
    constexpr float SideSpeed = 280.0f;
}

// Facing left/right maps the M1-029 axis rule to yaw 0/180, and the world
// forward axis becomes backward once the character faces the other way.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM1_031_FacingLeftRightLocalVelocity,
    "UEMMO.Tasks.M1_031.FacingLeftRightLocalVelocity",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_031_FacingLeftRightLocalVelocity::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("facing axis +1 maps to yaw 0"), FMath::IsNearlyEqual(UPrototypeAnimInstance::FacingYawFromHorizontalAxis(1.0f), 0.0f, TestTolerance));
    TestTrue(TEXT("facing axis -1 maps to yaw 180"), FMath::IsNearlyEqual(UPrototypeAnimInstance::FacingYawFromHorizontalAxis(-1.0f), 180.0f, TestTolerance));

    // Facing +X (yaw 0): world +X is forward.
    const FVector2D FacingRight = UPrototypeAnimInstance::ComputeLocalVelocity(FVector(100.0f, 0.0f, 0.0f), 0.0f);
    TestTrue(TEXT("facing +X turns world +X into positive forward"), FMath::IsNearlyEqual(FacingRight.X, 100.0f, TestTolerance));
    TestTrue(TEXT("facing +X gives world +X no side component"), FMath::IsNearlyEqual(FacingRight.Y, 0.0f, TestTolerance));

    // Facing -X (yaw 180): the same world +X is backward (negative forward).
    const FVector2D FacingLeft = UPrototypeAnimInstance::ComputeLocalVelocity(FVector(100.0f, 0.0f, 0.0f), 180.0f);
    TestTrue(TEXT("facing -X turns world +X into negative forward"), FMath::IsNearlyEqual(FacingLeft.X, -100.0f, TestTolerance));
    TestTrue(TEXT("facing -X gives world +X no side component"), FMath::IsNearlyEqual(FacingLeft.Y, 0.0f, TestTolerance));
    TestTrue(TEXT("facing -X turns world -X into positive forward"),
        FMath::IsNearlyEqual(UPrototypeAnimInstance::ComputeLocalVelocity(FVector(-100.0f, 0.0f, 0.0f), 180.0f).X, 100.0f, TestTolerance));

    // World depth axis (+Y) is lateral under both facings, never forward.
    const FVector2D DepthFacingRight = UPrototypeAnimInstance::ComputeLocalVelocity(FVector(0.0f, 100.0f, 0.0f), 0.0f);
    TestTrue(TEXT("facing +X gives world +Y zero forward"), FMath::IsNearlyEqual(DepthFacingRight.X, 0.0f, TestTolerance));
    TestTrue(TEXT("facing +X turns world +Y into positive right"), FMath::IsNearlyEqual(DepthFacingRight.Y, 100.0f, TestTolerance));
    const FVector2D DepthFacingLeft = UPrototypeAnimInstance::ComputeLocalVelocity(FVector(0.0f, 100.0f, 0.0f), 180.0f);
    TestTrue(TEXT("facing -X gives world +Y zero forward"), FMath::IsNearlyEqual(DepthFacingLeft.X, 0.0f, TestTolerance));
    TestTrue(TEXT("facing -X turns world +Y into negative right (left)"), FMath::IsNearlyEqual(DepthFacingLeft.Y, -100.0f, TestTolerance));
    return true;
}

// A side move must never register as forward motion: the forward component
// stays zero while the right/left component carries the whole speed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM1_031_SideMoveIsNotForward,
    "UEMMO.Tasks.M1_031.SideMoveIsNotForward",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_031_SideMoveIsNotForward::RunTest(const FString& Parameters)
{
    // Right move (world +Y) while facing +X: pure right, zero forward.
    const FVector2D RightMove = UPrototypeAnimInstance::ComputeLocalVelocity(FVector(0.0f, SideSpeed, 0.0f), 0.0f);
    TestTrue(TEXT("right move forward component is zero"), FMath::IsNearlyEqual(RightMove.X, 0.0f, TestTolerance));
    TestTrue(TEXT("right move runs at the full side speed to the right"), FMath::IsNearlyEqual(RightMove.Y, SideSpeed, TestTolerance));

    // Left move (world -Y) while facing +X: pure left, zero forward.
    const FVector2D LeftMove = UPrototypeAnimInstance::ComputeLocalVelocity(FVector(0.0f, -SideSpeed, 0.0f), 0.0f);
    TestTrue(TEXT("left move forward component is zero"), FMath::IsNearlyEqual(LeftMove.X, 0.0f, TestTolerance));
    TestTrue(TEXT("left move runs at the full side speed to the left"), FMath::IsNearlyEqual(LeftMove.Y, -SideSpeed, TestTolerance));

    // Side moves keep the same local direction once the facing flips: the
    // character must not start playing a forward jog after turning around.
    const FVector2D RightMoveFacingLeft = UPrototypeAnimInstance::ComputeLocalVelocity(FVector(0.0f, SideSpeed, 0.0f), 180.0f);
    TestTrue(TEXT("right move facing -X keeps zero forward"), FMath::IsNearlyEqual(RightMoveFacingLeft.X, 0.0f, TestTolerance));
    TestTrue(TEXT("right move facing -X is a mirrored left step"), FMath::IsNearlyEqual(RightMoveFacingLeft.Y, -SideSpeed, TestTolerance));
    return true;
}

// Speed is the planar magnitude (Z ignored) and the direction angle follows
// the standard 0=forward / 90=right / -90=left / 180=backward convention.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM1_031_SpeedAndDirection,
    "UEMMO.Tasks.M1_031.SpeedAndDirection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_031_SpeedAndDirection::RunTest(const FString& Parameters)
{
    const FVector Diagonal(300.0f, 400.0f, 500.0f);
    TestTrue(TEXT("planar speed ignores Z (300/400 -> 500)"),
        FMath::IsNearlyEqual(UPrototypeAnimInstance::ComputeSpeed(Diagonal), 500.0f, TestTolerance));
    TestTrue(TEXT("zero velocity gives zero speed"),
        FMath::IsNearlyEqual(UPrototypeAnimInstance::ComputeSpeed(FVector::ZeroVector), 0.0f, TestTolerance));

    TestTrue(TEXT("local (1,0) is 0 degrees"),
        FMath::IsNearlyEqual(UPrototypeAnimInstance::ComputeLocomotionDirection(FVector2D(1.0f, 0.0f)), 0.0f, TestTolerance));
    TestTrue(TEXT("local (0,1) is 90 degrees (right)"),
        FMath::IsNearlyEqual(UPrototypeAnimInstance::ComputeLocomotionDirection(FVector2D(0.0f, 1.0f)), 90.0f, TestTolerance));
    TestTrue(TEXT("local (0,-1) is -90 degrees (left)"),
        FMath::IsNearlyEqual(UPrototypeAnimInstance::ComputeLocomotionDirection(FVector2D(0.0f, -1.0f)), -90.0f, TestTolerance));
    TestTrue(TEXT("local (-1,0) is 180 degrees (backward)"),
        FMath::IsNearlyEqual(UPrototypeAnimInstance::ComputeLocomotionDirection(FVector2D(-1.0f, 0.0f)), 180.0f, TestTolerance));
    TestTrue(TEXT("zero local velocity defaults to 0 degrees"),
        FMath::IsNearlyEqual(UPrototypeAnimInstance::ComputeLocomotionDirection(FVector2D::ZeroVector), 0.0f, TestTolerance));
    return true;
}

// Airborne pose selection: rising plays the jump start, spent rise plays the
// fall loop, grounded characters stay on the locomotion pose.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM1_031_AirPoseIndex,
    "UEMMO.Tasks.M1_031.AirPoseIndex",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_031_AirPoseIndex::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("rising at jump launch velocity selects the jump pose"),
        UPrototypeAnimInstance::ComputeAirPoseIndex(true, 620.0f) == UPrototypeAnimInstance::Pose_Jump);
    TestTrue(TEXT("slow upward drift selects the fall pose"),
        UPrototypeAnimInstance::ComputeAirPoseIndex(true, 10.0f) == UPrototypeAnimInstance::Pose_Fall);
    TestTrue(TEXT("descending selects the fall pose"),
        UPrototypeAnimInstance::ComputeAirPoseIndex(true, -800.0f) == UPrototypeAnimInstance::Pose_Fall);
    TestTrue(TEXT("grounded characters use the locomotion pose"),
        UPrototypeAnimInstance::ComputeAirPoseIndex(false, -800.0f) == UPrototypeAnimInstance::Pose_Locomotion);
    TestTrue(TEXT("pose indexes match the AnimBP blend list order"),
        UPrototypeAnimInstance::Pose_Locomotion == 0 && UPrototypeAnimInstance::Pose_Jump == 1 &&
        UPrototypeAnimInstance::Pose_Fall == 2 && UPrototypeAnimInstance::Pose_Land == 3);
    return true;
}

// Asset smoke check: the generated AnimBP and BlendSpace exist, the AnimBP
// derives from the native instance, and the BlendSpace carries the eight
// direction samples plus idle. Runs after the editor asset script created them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM1_031_LocomotionAssetSmoke,
    "UEMMO.Tasks.M1_031.LocomotionAssetSmoke",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_031_LocomotionAssetSmoke::RunTest(const FString& Parameters)
{
    UClass* AnimClass = LoadClass<UAnimInstance>(nullptr, TEXT("/Game/UEMMO/Animation/ABP_Prototype.ABP_Prototype_C"));
    TestNotNull(TEXT("generated AnimBP class loads from /Game/UEMMO/Animation"), AnimClass);
    if (AnimClass)
    {
        TestTrue(TEXT("AnimBP parent class is UPrototypeAnimInstance"), AnimClass->IsChildOf<UPrototypeAnimInstance>());
    }

    UBlendSpace* BlendSpace = LoadObject<UBlendSpace>(nullptr, TEXT("/Game/UEMMO/Animation/BS_Prototype_Locomotion.BS_Prototype_Locomotion"));
    TestNotNull(TEXT("locomotion BlendSpace loads from /Game/UEMMO/Animation"), BlendSpace);
    if (BlendSpace)
    {
        const TArray<FBlendSample>& Samples = BlendSpace->GetBlendSamples();
        TestTrue(TEXT("BlendSpace has idle plus the eight direction samples"), Samples.Num() >= 9);

        bool bHasIdle = false, bHasSide = false, bHasBackward = false, bAllLoad = Samples.Num() > 0;
        for (const FBlendSample& Sample : Samples)
        {
            if (Sample.Animation != nullptr)
            {
                if (FMath::IsNearlyZero(Sample.SampleValue.X, TestTolerance)) bHasIdle = true;
                if (FMath::Abs(Sample.SampleValue.Y) >= 90.0f - TestTolerance && Sample.SampleValue.X > 0.0f) bHasSide = true;
                if (FMath::Abs(Sample.SampleValue.Y) >= 135.0f - TestTolerance && Sample.SampleValue.X > 0.0f) bHasBackward = true;
            }
            else
            {
                bAllLoad = false;
            }
        }
        TestTrue(TEXT("BlendSpace contains an idle sample at speed 0"), bHasIdle);
        TestTrue(TEXT("BlendSpace contains left/right side samples"), bHasSide);
        TestTrue(TEXT("BlendSpace contains backward direction samples"), bHasBackward);
        TestTrue(TEXT("every BlendSpace sample references a loadable animation"), bAllLoad);
    }
    return true;
}

#endif
