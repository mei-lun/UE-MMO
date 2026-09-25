#include "Misc/AutomationTest.h"

#include "../Character/SideViewCameraComponent.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    constexpr float TestTolerance = 0.01f;
    constexpr float AcceptanceTolerance = 1.0f;
}

using namespace UE::UEMMO::Tasks::M1_030;

// Jump Z must never enter the anchor: with the same ground Z, a pawn 100cm in
// the air and the same pawn on the ground produce the identical anchor Z, so
// the in-place jump fluctuation is 0cm (< 5cm acceptance bound).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM1_030AnchorZIgnoresJump,
    "UEMMO.Tasks.M1_030.AnchorZIgnoresJump",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_030AnchorZIgnoresJump::RunTest(const FString& Parameters)
{
    const FSideViewCameraConfig Config;
    const float GroundZ = 0.0f;
    const FVector GroundPose(300.0f, 200.0f, 0.0f);
    const FVector AirPose(300.0f, 200.0f, 100.0f);
    const FVector GroundAnchor = ComputeAnchorLocation(GroundPose, GroundZ, 200.0f, Config);
    const FVector AirAnchor = ComputeAnchorLocation(AirPose, GroundZ, 200.0f, Config);
    const float Fluctuation = FMath::Abs(AirAnchor.Z - GroundAnchor.Z);
    TestTrue(TEXT("pawn Z 100 in air vs 0 on ground gives anchor Z fluctuation < 5cm"), Fluctuation < 5.0f);
    TestTrue(TEXT("anchor Z equals ground Z plus the fixed pivot height"),
        FMath::IsNearlyEqual(AirAnchor.Z, GroundZ + Config.PivotHeightAboveGround, TestTolerance));
    TestTrue(TEXT("anchor Z never equals the airborne pawn Z"), FMath::Abs(AirAnchor.Z - AirPose.Z) > 1.0f);
    TestTrue(TEXT("a raised traced ground (250) lifts the anchor accordingly"),
        FMath::IsNearlyEqual(ComputeAnchorLocation(GroundPose, 250.0f, 200.0f, Config).Z, 250.0f + Config.PivotHeightAboveGround, TestTolerance));
    return true;
}

// The anchor follows only 25% of the pawn's depth movement: moving 400cm in Y
// shifts the anchor Y by about 100cm, while X and Z keep their own semantics.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM1_030AnchorFollowsYDamping,
    "UEMMO.Tasks.M1_030.AnchorFollowsYDamping",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_030AnchorFollowsYDamping::RunTest(const FString& Parameters)
{
    const FSideViewCameraConfig Config; // YFollowRatio defaults to 0.25
    const float ReferenceY = 0.0f;
    const FVector StartPose(0.0f, 0.0f, 0.0f);
    const FVector MovedPose(0.0f, 400.0f, 0.0f);
    const float StartY = ComputeAnchorLocation(StartPose, 0.0f, ReferenceY, Config).Y;
    const float MovedY = ComputeAnchorLocation(MovedPose, 0.0f, ReferenceY, Config).Y;
    TestTrue(TEXT("pawn Y moved 400 moves the anchor Y by about 100 (25% damping)"),
        FMath::IsNearlyEqual(MovedY - StartY, 100.0f, AcceptanceTolerance));
    TestTrue(TEXT("negative depth movement damps the same way"),
        FMath::IsNearlyEqual(ApplyYDamping(ReferenceY, -400.0f, Config.YFollowRatio), -100.0f, AcceptanceTolerance));
    TestTrue(TEXT("X position passes through the anchor undamped"),
        FMath::IsNearlyEqual(ComputeAnchorLocation(FVector(300.0f, 400.0f, 0.0f), 0.0f, ReferenceY, Config).X, 300.0f, TestTolerance));
    TestTrue(TEXT("anchor Z is independent of the damped Y"),
        FMath::IsNearlyEqual(ComputeAnchorLocation(MovedPose, 0.0f, ReferenceY, Config).Z, Config.PivotHeightAboveGround, TestTolerance));
    return true;
}

// The anchor X clamps to the configurable room bounds (default +/- 1200cm).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM1_030RoomBoundsClamp,
    "UEMMO.Tasks.M1_030.RoomBoundsClamp",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_030RoomBoundsClamp::RunTest(const FString& Parameters)
{
    const FSideViewCameraConfig Config; // RoomMinX/RoomMaxX default to -1200/+1200
    TestTrue(TEXT("X beyond the max room bound clamps to +1200"),
        FMath::IsNearlyEqual(ClampToRoomBounds(2000.0f, Config.RoomMinX, Config.RoomMaxX), 1200.0f, TestTolerance));
    TestTrue(TEXT("X beyond the min room bound clamps to -1200"),
        FMath::IsNearlyEqual(ClampToRoomBounds(-2000.0f, Config.RoomMinX, Config.RoomMaxX), -1200.0f, TestTolerance));
    TestTrue(TEXT("X inside the room bounds passes through"),
        FMath::IsNearlyEqual(ClampToRoomBounds(800.0f, Config.RoomMinX, Config.RoomMaxX), 800.0f, TestTolerance));
    TestTrue(TEXT("X exactly on the bound stays on it"),
        FMath::IsNearlyEqual(ClampToRoomBounds(1200.0f, Config.RoomMinX, Config.RoomMaxX), 1200.0f, TestTolerance));
    TestTrue(TEXT("anchor X clamps when the pawn walks past the room edge"),
        FMath::IsNearlyEqual(ComputeAnchorLocation(FVector(5000.0f, 0.0f, 0.0f), 0.0f, 0.0f, Config).X, 1200.0f, TestTolerance));
    TestTrue(TEXT("the damped Y and ground Z survive the X clamp"),
        FMath::IsNearlyEqual(ComputeAnchorLocation(FVector(5000.0f, 400.0f, 0.0f), 0.0f, 0.0f, Config).Y, 100.0f, AcceptanceTolerance));
    return true;
}

// Facing flips (+1 / -1) never change the rig yaw: the fixed side-view yaw
// stays -90 and the pitch stays at its downward -18.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM1_030FacingDoesNotChangeYaw,
    "UEMMO.Tasks.M1_030.FacingDoesNotChangeYaw",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_030FacingDoesNotChangeYaw::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("facing +1 keeps the rig yaw at -90"),
        FMath::IsNearlyEqual(ComputeCameraYaw(1.0f), SideViewCameraYaw, TestTolerance));
    TestTrue(TEXT("facing -1 keeps the rig yaw at -90"),
        FMath::IsNearlyEqual(ComputeCameraYaw(-1.0f), SideViewCameraYaw, TestTolerance));
    TestTrue(TEXT("no facing input keeps the rig yaw at -90"),
        FMath::IsNearlyEqual(ComputeCameraYaw(0.0f), SideViewCameraYaw, TestTolerance));
    TestTrue(TEXT("the fixed side-view yaw constant is -90"),
        FMath::IsNearlyEqual(SideViewCameraYaw, -90.0f, TestTolerance));
    const FRotator FacingLeft = ComputeCameraRotation(-1.0f);
    const FRotator FacingRight = ComputeCameraRotation(1.0f);
    TestTrue(TEXT("rotation yaw for facing -1 is -90"), FMath::IsNearlyEqual(FacingLeft.Yaw, -90.0f, TestTolerance));
    TestTrue(TEXT("rotation yaw for facing +1 is -90"), FMath::IsNearlyEqual(FacingRight.Yaw, -90.0f, TestTolerance));
    TestTrue(TEXT("both facings produce the identical rotation"),
        FMath::IsNearlyEqual(FacingLeft.Yaw, FacingRight.Yaw, TestTolerance) && FMath::IsNearlyEqual(FacingLeft.Pitch, FacingRight.Pitch, TestTolerance));
    TestTrue(TEXT("rotation pitch stays at the downward -18"), FMath::IsNearlyEqual(FacingLeft.Pitch, SideViewCameraPitch, TestTolerance));
    return true;
}

// The component-side ground source: a successful trace hit feeds the anchor,
// while a missed or non-finite trace falls back to the last known ground Z.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM1_030GroundZFallback,
    "UEMMO.Tasks.M1_030.GroundZFallback",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_030GroundZFallback::RunTest(const FString& Parameters)
{
    const float NaN = std::numeric_limits<float>::quiet_NaN();
    TestTrue(TEXT("a successful trace hit becomes the ground Z"),
        FMath::IsNearlyEqual(ResolveGroundZ(true, 250.0f, 7.0f), 250.0f, TestTolerance));
    TestTrue(TEXT("a missed trace falls back to the last known ground Z"),
        FMath::IsNearlyEqual(ResolveGroundZ(false, 250.0f, 7.0f), 7.0f, TestTolerance));
    TestTrue(TEXT("a non-finite trace hit falls back too"),
        FMath::IsNearlyEqual(ResolveGroundZ(true, NaN, 7.0f), 7.0f, TestTolerance));
    TestTrue(TEXT("a non-finite fallback collapses to 0"),
        FMath::IsNearlyEqual(ResolveGroundZ(false, NaN, NaN), 0.0f, TestTolerance));
    return true;
}

// Only the anchor X is smoothed: an exponential step moves partway toward the
// target each tick, converges for long steps, and speed 0 snaps to the target.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM1_030SmoothFollowAxis,
    "UEMMO.Tasks.M1_030.SmoothFollowAxis",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_030SmoothFollowAxis::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("zero follow speed snaps to the target"),
        FMath::IsNearlyEqual(SmoothFollowAxis(0.0f, 100.0f, 1.0f / 30.0f, 0.0f), 100.0f, TestTolerance));
    const float First = SmoothFollowAxis(0.0f, 100.0f, 1.0f / 30.0f, 8.0f);
    TestTrue(TEXT("one small step moves partway toward the target"), First > 0.0f && First < 100.0f);
    const float Second = SmoothFollowAxis(First, 100.0f, 1.0f / 30.0f, 8.0f);
    TestTrue(TEXT("successive steps keep approaching the target without overshooting"),
        Second > First && Second < 100.0f);
    TestTrue(TEXT("a long step converges on the target"),
        FMath::IsNearlyEqual(SmoothFollowAxis(0.0f, 100.0f, 100.0f, 8.0f), 100.0f, TestTolerance));
    TestTrue(TEXT("non-finite targets leave the current value untouched"),
        FMath::IsNearlyEqual(SmoothFollowAxis(42.0f, std::numeric_limits<float>::quiet_NaN(), 1.0f / 30.0f, 8.0f), 42.0f, TestTolerance));
    return true;
}

#endif
