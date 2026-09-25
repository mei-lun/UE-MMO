#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/CombatGeometry.h"
#include "../Combat/AttackDefinition.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	// File-local tolerance; the M1_017 prefix keeps it unique even if unity
	// builds are ever re-enabled and anonymous namespaces merge into one TU.
	constexpr float M1_017_Tolerance = 0.01f;

	// Builds a definition with the given hit geometry; everything else keeps
	// the UAttackDefinition defaults, which ComputeHitBox never inspects.
	UAttackDefinition* M1_017_MakeAttack(const FVector& OffsetFromFeet, const FVector& HalfExtent)
	{
		UAttackDefinition* Attack = NewObject<UAttackDefinition>();
		Attack->HitOffsetFromFeet = OffsetFromFeet;
		Attack->HitHalfExtent = HalfExtent;
		return Attack;
	}
}

// Acceptance example: default offset (95,0,90) and half extent (85,50,70),
// right facing. The center is feet + offset; the extent passes through.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_017RightFacingCenterAndExtent,
	"UEMMO.Tasks.M1_017.RightFacingCenterAndExtent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_017RightFacingCenterAndExtent::RunTest(const FString& Parameters)
{
	UAttackDefinition* Attack = M1_017_MakeAttack(FVector(95.0f, 0.0f, 90.0f), FVector(85.0f, 50.0f, 70.0f));
	const FCombatHitBox HitBox = ComputeHitBox(FVector(100.0f, 20.0f, 0.0f), 1, *Attack);

	TestTrue(TEXT("facing +1 center X is feet X plus offset X 95"),
		FMath::IsNearlyEqual(HitBox.Center.X, 195.0, M1_017_Tolerance));
	TestTrue(TEXT("facing +1 center Y is feet Y plus offset Y 0"),
		FMath::IsNearlyEqual(HitBox.Center.Y, 20.0, M1_017_Tolerance));
	TestTrue(TEXT("facing +1 center Z is feet Z plus offset Z 90"),
		FMath::IsNearlyEqual(HitBox.Center.Z, 90.0, M1_017_Tolerance));

	TestTrue(TEXT("extent X comes from the definition unchanged"),
		FMath::IsNearlyEqual(HitBox.Extent.X, 85.0, M1_017_Tolerance));
	TestTrue(TEXT("extent Y comes from the definition unchanged"),
		FMath::IsNearlyEqual(HitBox.Extent.Y, 50.0, M1_017_Tolerance));
	TestTrue(TEXT("extent Z comes from the definition unchanged"),
		FMath::IsNearlyEqual(HitBox.Extent.Z, 70.0, M1_017_Tolerance));
	return true;
}

// Left facing mirrors only the offset's X component; center Y and Z stay
// identical to the right-facing box and the extent is not mirrored either.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_017LeftFacingMirrorsXOnly,
	"UEMMO.Tasks.M1_017.LeftFacingMirrorsXOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_017LeftFacingMirrorsXOnly::RunTest(const FString& Parameters)
{
	UAttackDefinition* Attack = M1_017_MakeAttack(FVector(95.0f, 0.0f, 90.0f), FVector(85.0f, 50.0f, 70.0f));
	const FCombatHitBox Right = ComputeHitBox(FVector(100.0f, 20.0f, 0.0f), 1, *Attack);
	const FCombatHitBox Left = ComputeHitBox(FVector(100.0f, 20.0f, 0.0f), -1, *Attack);

	TestTrue(TEXT("facing -1 center X is feet X minus offset X"),
		FMath::IsNearlyEqual(Left.Center.X, 5.0, M1_017_Tolerance));
	TestTrue(TEXT("facing -1 keeps center Y identical to facing +1"),
		FMath::IsNearlyEqual(Left.Center.Y, Right.Center.Y, M1_017_Tolerance));
	TestTrue(TEXT("facing -1 keeps center Z identical to facing +1"),
		FMath::IsNearlyEqual(Left.Center.Z, Right.Center.Z, M1_017_Tolerance));

	// The extent is direction independent, so both directions report the same box size.
	TestTrue(TEXT("facing -1 keeps the extent identical to facing +1"),
		Left.Extent.Equals(Right.Extent, M1_017_Tolerance));
	return true;
}

// A custom offset definition mirrors only its X component too: Y and Z of the
// offset are added unchanged for both facings.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_017CustomOffsetMirrorsXOnly,
	"UEMMO.Tasks.M1_017.CustomOffsetMirrorsXOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_017CustomOffsetMirrorsXOnly::RunTest(const FString& Parameters)
{
	UAttackDefinition* Attack = M1_017_MakeAttack(FVector(50.0f, 10.0f, 120.0f), FVector(30.0f, 40.0f, 60.0f));

	const FCombatHitBox Right = ComputeHitBox(FVector(100.0f, 20.0f, 0.0f), 1, *Attack);
	TestTrue(TEXT("custom offset facing +1 center is (150,30,120) X"),
		FMath::IsNearlyEqual(Right.Center.X, 150.0, M1_017_Tolerance));
	TestTrue(TEXT("custom offset facing +1 center is (150,30,120) Y"),
		FMath::IsNearlyEqual(Right.Center.Y, 30.0, M1_017_Tolerance));
	TestTrue(TEXT("custom offset facing +1 center is (150,30,120) Z"),
		FMath::IsNearlyEqual(Right.Center.Z, 120.0, M1_017_Tolerance));

	const FCombatHitBox Left = ComputeHitBox(FVector(100.0f, 20.0f, 0.0f), -1, *Attack);
	TestTrue(TEXT("custom offset facing -1 center is (50,30,120) X"),
		FMath::IsNearlyEqual(Left.Center.X, 50.0, M1_017_Tolerance));
	TestTrue(TEXT("custom offset facing -1 keeps offset Y unchanged"),
		FMath::IsNearlyEqual(Left.Center.Y, 30.0, M1_017_Tolerance));
	TestTrue(TEXT("custom offset facing -1 keeps offset Z unchanged"),
		FMath::IsNearlyEqual(Left.Center.Z, 120.0, M1_017_Tolerance));

	TestTrue(TEXT("custom extent passes through unchanged"),
		Right.Extent.Equals(FVector(30.0f, 40.0f, 60.0f), M1_017_Tolerance));
	return true;
}

// The feet origin is the only height anchor: with the feet at Z=131 the box
// center rises to 131 + 90, not to a capsule-center plus waist value.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_017FeetOriginFollowsJump,
	"UEMMO.Tasks.M1_017.FeetOriginFollowsJump",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_017FeetOriginFollowsJump::RunTest(const FString& Parameters)
{
	UAttackDefinition* Attack = M1_017_MakeAttack(FVector(95.0f, 0.0f, 90.0f), FVector(85.0f, 50.0f, 70.0f));
	const FCombatHitBox HitBox = ComputeHitBox(FVector(0.0f, 0.0f, 131.0f), 1, *Attack);

	TestTrue(TEXT("jumped feet Z 131 lifts the center Z to 221"),
		FMath::IsNearlyEqual(HitBox.Center.Z, 221.0, M1_017_Tolerance));
	TestTrue(TEXT("the center Z is not the bare feet Z"),
		!FMath::IsNearlyEqual(HitBox.Center.Z, 131.0, M1_017_Tolerance));
	TestTrue(TEXT("jumped feet keep center X at feet X plus offset X"),
		FMath::IsNearlyEqual(HitBox.Center.X, 95.0, M1_017_Tolerance));
	TestTrue(TEXT("jumped feet keep center Y at feet Y plus offset Y"),
		FMath::IsNearlyEqual(HitBox.Center.Y, 0.0, M1_017_Tolerance));
	return true;
}

// Facing values outside the contractual +-1 reduce to their sign: positive
// counts as +1, negative as -1, and 0 counts as +1.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_017FacingSignNormalization,
	"UEMMO.Tasks.M1_017.FacingSignNormalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_017FacingSignNormalization::RunTest(const FString& Parameters)
{
	UAttackDefinition* Attack = M1_017_MakeAttack(FVector(95.0f, 0.0f, 90.0f), FVector(85.0f, 50.0f, 70.0f));
	const FVector Feet(100.0f, 0.0f, 0.0f);

	const FCombatHitBox FacingZero = ComputeHitBox(Feet, 0, *Attack);
	TestTrue(TEXT("facing 0 counts as +1 (right-facing center X)"),
		FMath::IsNearlyEqual(FacingZero.Center.X, 195.0, M1_017_Tolerance));

	const FCombatHitBox FacingTwo = ComputeHitBox(Feet, 2, *Attack);
	TestTrue(TEXT("facing 2 counts as +1 (right-facing center X)"),
		FMath::IsNearlyEqual(FacingTwo.Center.X, 195.0, M1_017_Tolerance));

	const FCombatHitBox FacingMinusThree = ComputeHitBox(Feet, -3, *Attack);
	TestTrue(TEXT("facing -3 counts as -1 (left-facing center X)"),
		FMath::IsNearlyEqual(FacingMinusThree.Center.X, 5.0, M1_017_Tolerance));

	// Only the center X depends on the normalized sign; the extent never does.
	TestTrue(TEXT("normalized facings keep the same extent"),
		FacingZero.Extent.Equals(FacingMinusThree.Extent, M1_017_Tolerance));
	return true;
}

#endif
