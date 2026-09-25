#include "Misc/AutomationTest.h"

#include "../Character/PlanarMovement.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
    constexpr float TestTolerance = 0.01f;
}

using namespace UE::UEMMO::Tasks::M1_029;

// Pure X input: full horizontal speed, no depth component.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM1_029PureX,
    "UEMMO.Tasks.M1_029.PureX",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_029PureX::RunTest(const FString& Parameters)
{
    const FVector Velocity = ComputePlanarVelocity(1.0f, 0.0f);
    TestTrue(TEXT("input (1,0) runs at the X speed 420"), FMath::IsNearlyEqual(Velocity.X, PlanarSpeedX, TestTolerance));
    TestTrue(TEXT("input (1,0) has no Y speed"), FMath::IsNearlyEqual(Velocity.Y, 0.0f, TestTolerance));
    TestTrue(TEXT("planar velocity keeps Z at zero"), FMath::IsNearlyEqual(Velocity.Z, 0.0f, TestTolerance));
    return true;
}

// Pure Y input: full depth speed, no horizontal component.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM1_029PureY,
    "UEMMO.Tasks.M1_029.PureY",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_029PureY::RunTest(const FString& Parameters)
{
    const FVector Velocity = ComputePlanarVelocity(0.0f, 1.0f);
    TestTrue(TEXT("input (0,1) has no X speed"), FMath::IsNearlyEqual(Velocity.X, 0.0f, TestTolerance));
    TestTrue(TEXT("input (0,1) runs at the Y speed 280"), FMath::IsNearlyEqual(Velocity.Y, PlanarSpeedY, TestTolerance));
    TestTrue(TEXT("planar velocity keeps Z at zero"), FMath::IsNearlyEqual(Velocity.Z, 0.0f, TestTolerance));
    return true;
}

// Diagonal input is normalized first: each axis gets speed / sqrt(2), not both at full speed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM1_029DiagonalNormalized,
    "UEMMO.Tasks.M1_029.DiagonalNormalized",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_029DiagonalNormalized::RunTest(const FString& Parameters)
{
    const FVector Velocity = ComputePlanarVelocity(1.0f, 1.0f);
    const float ExpectedX = PlanarSpeedX / FMath::Sqrt(2.0f);
    const float ExpectedY = PlanarSpeedY / FMath::Sqrt(2.0f);
    TestTrue(TEXT("diagonal input gives X speed 420/sqrt(2)"), FMath::IsNearlyEqual(Velocity.X, ExpectedX, TestTolerance));
    TestTrue(TEXT("diagonal input gives Y speed 280/sqrt(2)"), FMath::IsNearlyEqual(Velocity.Y, ExpectedY, TestTolerance));
    TestTrue(TEXT("diagonal is not both axes at full speed"), Velocity.X < PlanarSpeedX - 1.0f && Velocity.Y < PlanarSpeedY - 1.0f);
    return true;
}

// Zero input yields zero velocity; opposite contributions on one axis cancel.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM1_029ZeroAndCancellation,
    "UEMMO.Tasks.M1_029.ZeroAndCancellation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_029ZeroAndCancellation::RunTest(const FString& Parameters)
{
    TestTrue(TEXT("input (0,0) yields zero velocity"), ComputePlanarVelocity(0.0f, 0.0f).IsNearlyZero());

    FPlanarAxisState Axes;
    Axes.AddAxisX(1.0f);
    Axes.AddAxisX(-1.0f); // +1 and -1 on the same axis cancel to 0
    Axes.AddAxisY(1.0f);
    const FVector Velocity = ComputePlanarVelocity(Axes);
    TestTrue(TEXT("canceled X axis yields no X velocity"), FMath::IsNearlyEqual(Velocity.X, 0.0f, TestTolerance));
    TestTrue(TEXT("uncanceled Y axis still runs at the Y speed"), FMath::IsNearlyEqual(Velocity.Y, PlanarSpeedY, TestTolerance));
    return true;
}

// Inputs outside [-1,1] are clamped; non-finite input counts as 0.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM1_029ClampAndNonFinite,
    "UEMMO.Tasks.M1_029.ClampAndNonFinite",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_029ClampAndNonFinite::RunTest(const FString& Parameters)
{
    // (2,-5) clamps to (1,-1) before normalization, so it behaves like a diagonal.
    const FVector Clamped = ComputePlanarVelocity(2.0f, -5.0f);
    const float ExpectedDiagonalX = PlanarSpeedX / FMath::Sqrt(2.0f);
    const float ExpectedDiagonalY = -PlanarSpeedY / FMath::Sqrt(2.0f);
    TestTrue(TEXT("out-of-range X clamps to the diagonal X speed"), FMath::IsNearlyEqual(Clamped.X, ExpectedDiagonalX, TestTolerance));
    TestTrue(TEXT("out-of-range Y clamps to the diagonal Y speed"), FMath::IsNearlyEqual(Clamped.Y, ExpectedDiagonalY, TestTolerance));

    const float NaN = std::numeric_limits<float>::quiet_NaN();
    const FVector WithNaN = ComputePlanarVelocity(NaN, 1.0f);
    TestTrue(TEXT("NaN X input counts as 0"), FMath::IsNearlyEqual(WithNaN.X, 0.0f, TestTolerance));
    TestTrue(TEXT("NaN X input leaves pure Y speed"), FMath::IsNearlyEqual(WithNaN.Y, PlanarSpeedY, TestTolerance));

    const float Infinity = std::numeric_limits<float>::infinity();
    const FVector WithInfinity = ComputePlanarVelocity(1.0f, Infinity);
    TestTrue(TEXT("infinite Y input counts as 0"), FMath::IsNearlyEqual(WithInfinity.Y, 0.0f, TestTolerance));
    TestTrue(TEXT("infinite Y input leaves pure X speed"), FMath::IsNearlyEqual(WithInfinity.X, PlanarSpeedX, TestTolerance));
    return true;
}

#endif
