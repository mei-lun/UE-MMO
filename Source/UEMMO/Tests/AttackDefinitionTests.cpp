#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/SoftObjectPath.h"

#include <limits>

#include "../Combat/AttackDefinition.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_007
{
	// Acceptance light_01 values from the interface contract section 3:
	// duration 26, active window [7,11), cancel window [12,24), base damage 10.
	// Animation stays unset and bPlaceholderAnimation keeps its true default,
	// so no clip seconds are ordered-checked for this entry.
	static UAttackDefinition* MakeLight01()
	{
		UAttackDefinition* Attack = NewObject<UAttackDefinition>();
		Attack->AttackId = TEXT("light_01");
		Attack->DurationFrames = 26;
		Attack->ActiveWindow.StartFrame = 7;
		Attack->ActiveWindow.EndFrame = 11;
		Attack->CancelWindow.StartFrame = 12;
		Attack->CancelWindow.EndFrame = 24;
		Attack->BaseDamage = 10.0f;
		Attack->AttackCoefficient = 1.0f;
		Attack->KnockbackSpeed = 420.0f;
		Attack->LaunchSpeed = 0.0f;
		Attack->HitStunSeconds = 0.25f;
		Attack->HitStopSeconds = 0.04f;
		return Attack;
	}

	static const float NanValue = std::numeric_limits<float>::quiet_NaN();
	static const float InfValue = std::numeric_limits<float>::infinity();

	static TSoftObjectPtr<UAnimSequence> MakeTestAnimation()
	{
		return TSoftObjectPtr<UAnimSequence>(FSoftObjectPath(TEXT("/Game/UEMMO/Test")));
	}
}

using namespace UE::UEMMO::Tasks::M1_007;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_007Light01Validates,
	"UEMMO.Tasks.M1_007.Light01Validates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_007Light01Validates::RunTest(const FString& Parameters)
{
	UAttackDefinition* Attack = MakeLight01();
	FText Errors;
	TestTrue(TEXT("valid light_01 passes single-entry validation"), ValidateAttackDefinition(*Attack, Errors));
	TestTrue(TEXT("no errors are reported for valid light_01"), Errors.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_007DefaultsMatchContractAndFailValidation,
	"UEMMO.Tasks.M1_007.DefaultsMatchContractAndFailValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_007DefaultsMatchContractAndFailValidation::RunTest(const FString& Parameters)
{
	// Default values come from the interface contract; none of them require a World.
	UAttackDefinition* Attack = NewObject<UAttackDefinition>();
	TestEqual(TEXT("default AttackId is None"), Attack->AttackId, FName(NAME_None));
	TestEqual(TEXT("default DurationFrames is 0"), Attack->DurationFrames, 0);
	TestEqual(TEXT("default AttackCoefficient is 1.0"), Attack->AttackCoefficient, 1.0f);
	TestEqual(TEXT("default HitOffsetFromFeet is (95,0,90)"), Attack->HitOffsetFromFeet, FVector(95.0f, 0.0f, 90.0f));
	TestEqual(TEXT("default HitHalfExtent is (85,50,70)"), Attack->HitHalfExtent, FVector(85.0f, 50.0f, 70.0f));
	TestEqual(TEXT("default HitStopSeconds is 0.04"), Attack->HitStopSeconds, 0.04f);
	TestEqual(TEXT("default HitStunSeconds is 0"), Attack->HitStunSeconds, 0.0f);
	TestTrue(TEXT("default bPlaceholderAnimation is true"), Attack->bPlaceholderAnimation);

	// A default-constructed entry is invalid because DurationFrames must be positive.
	FText Errors;
	TestFalse(TEXT("default-constructed attack (DurationFrames=0) is rejected"), ValidateAttackDefinition(*Attack, Errors));
	TestTrue(TEXT("default-constructed error names DurationFrames"),
		Errors.ToString().Contains(TEXT("DurationFrames")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_007RejectsInvalidDurationFrames,
	"UEMMO.Tasks.M1_007.RejectsInvalidDurationFrames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_007RejectsInvalidDurationFrames::RunTest(const FString& Parameters)
{
	UAttackDefinition* Zero = MakeLight01();
	Zero->DurationFrames = 0;
	FText ZeroErrors;
	TestFalse(TEXT("DurationFrames=0 is rejected"), ValidateAttackDefinition(*Zero, ZeroErrors));
	TestTrue(TEXT("DurationFrames=0 error names DurationFrames"),
		ZeroErrors.ToString().Contains(TEXT("DurationFrames")));

	UAttackDefinition* Negative = MakeLight01();
	Negative->DurationFrames = -5;
	FText NegativeErrors;
	TestFalse(TEXT("DurationFrames=-5 is rejected"), ValidateAttackDefinition(*Negative, NegativeErrors));
	TestTrue(TEXT("DurationFrames=-5 error names DurationFrames"),
		NegativeErrors.ToString().Contains(TEXT("DurationFrames")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_007RejectsInvalidFrameWindows,
	"UEMMO.Tasks.M1_007.RejectsInvalidFrameWindows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_007RejectsInvalidFrameWindows::RunTest(const FString& Parameters)
{
	UAttackDefinition* OutOfBounds = MakeLight01();
	OutOfBounds->ActiveWindow.EndFrame = 27; // duration is 26, so end 27 leaves the total range
	FText OutOfBoundsErrors;
	TestFalse(TEXT("ActiveWindow end beyond DurationFrames is rejected"),
		ValidateAttackDefinition(*OutOfBounds, OutOfBoundsErrors));
	TestTrue(TEXT("out-of-bounds error names ActiveWindow"),
		OutOfBoundsErrors.ToString().Contains(TEXT("ActiveWindow")));

	UAttackDefinition* Reversed = MakeLight01();
	Reversed->ActiveWindow.StartFrame = 11;
	Reversed->ActiveWindow.EndFrame = 11; // end <= start makes the half-open window empty
	FText ReversedErrors;
	TestFalse(TEXT("ActiveWindow end<=start is rejected"), ValidateAttackDefinition(*Reversed, ReversedErrors));
	TestTrue(TEXT("end<=start error names ActiveWindow"),
		ReversedErrors.ToString().Contains(TEXT("ActiveWindow")));

	UAttackDefinition* NegativeStart = MakeLight01();
	NegativeStart->ActiveWindow.StartFrame = -1;
	NegativeStart->ActiveWindow.EndFrame = 5;
	FText NegativeStartErrors;
	TestFalse(TEXT("ActiveWindow negative start frame is rejected"),
		ValidateAttackDefinition(*NegativeStart, NegativeStartErrors));
	TestTrue(TEXT("negative start error names ActiveWindow"),
		NegativeStartErrors.ToString().Contains(TEXT("ActiveWindow")));

	UAttackDefinition* CancelOutOfBounds = MakeLight01();
	CancelOutOfBounds->CancelWindow.EndFrame = 30; // duration is 26
	FText CancelOutOfBoundsErrors;
	TestFalse(TEXT("CancelWindow beyond DurationFrames is rejected"),
		ValidateAttackDefinition(*CancelOutOfBounds, CancelOutOfBoundsErrors));
	TestTrue(TEXT("CancelWindow out-of-bounds error names CancelWindow"),
		CancelOutOfBoundsErrors.ToString().Contains(TEXT("CancelWindow")));

	UAttackDefinition* CancelReversed = MakeLight01();
	CancelReversed->CancelWindow.StartFrame = 20;
	CancelReversed->CancelWindow.EndFrame = 18;
	FText CancelReversedErrors;
	TestFalse(TEXT("reversed CancelWindow is rejected"), ValidateAttackDefinition(*CancelReversed, CancelReversedErrors));
	TestTrue(TEXT("reversed CancelWindow error names CancelWindow"),
		CancelReversedErrors.ToString().Contains(TEXT("CancelWindow")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_007RejectsInvalidDamageAndCoefficient,
	"UEMMO.Tasks.M1_007.RejectsInvalidDamageAndCoefficient",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_007RejectsInvalidDamageAndCoefficient::RunTest(const FString& Parameters)
{
	UAttackDefinition* NanDamage = MakeLight01();
	NanDamage->BaseDamage = NanValue;
	FText NanDamageErrors;
	TestFalse(TEXT("BaseDamage=NaN is rejected"), ValidateAttackDefinition(*NanDamage, NanDamageErrors));
	TestTrue(TEXT("BaseDamage=NaN error names BaseDamage"),
		NanDamageErrors.ToString().Contains(TEXT("BaseDamage")));

	UAttackDefinition* NegativeDamage = MakeLight01();
	NegativeDamage->BaseDamage = -1.0f;
	FText NegativeDamageErrors;
	TestFalse(TEXT("BaseDamage=-1 is rejected"), ValidateAttackDefinition(*NegativeDamage, NegativeDamageErrors));
	TestTrue(TEXT("BaseDamage=-1 error names BaseDamage"),
		NegativeDamageErrors.ToString().Contains(TEXT("BaseDamage")));

	UAttackDefinition* InfiniteDamage = MakeLight01();
	InfiniteDamage->BaseDamage = InfValue;
	FText InfiniteDamageErrors;
	TestFalse(TEXT("BaseDamage=+Inf is rejected"), ValidateAttackDefinition(*InfiniteDamage, InfiniteDamageErrors));
	TestTrue(TEXT("BaseDamage=+Inf error names BaseDamage"),
		InfiniteDamageErrors.ToString().Contains(TEXT("BaseDamage")));

	UAttackDefinition* NanCoefficient = MakeLight01();
	NanCoefficient->AttackCoefficient = NanValue;
	FText NanCoefficientErrors;
	TestFalse(TEXT("AttackCoefficient=NaN is rejected"), ValidateAttackDefinition(*NanCoefficient, NanCoefficientErrors));
	TestTrue(TEXT("AttackCoefficient=NaN error names AttackCoefficient"),
		NanCoefficientErrors.ToString().Contains(TEXT("AttackCoefficient")));

	UAttackDefinition* ZeroCoefficient = MakeLight01();
	ZeroCoefficient->AttackCoefficient = 0.0f;
	FText ZeroCoefficientErrors;
	TestFalse(TEXT("AttackCoefficient=0 is rejected"), ValidateAttackDefinition(*ZeroCoefficient, ZeroCoefficientErrors));
	TestTrue(TEXT("AttackCoefficient=0 error names AttackCoefficient"),
		ZeroCoefficientErrors.ToString().Contains(TEXT("AttackCoefficient")));

	UAttackDefinition* NegativeCoefficient = MakeLight01();
	NegativeCoefficient->AttackCoefficient = -0.5f;
	FText NegativeCoefficientErrors;
	TestFalse(TEXT("AttackCoefficient=-0.5 is rejected"), ValidateAttackDefinition(*NegativeCoefficient, NegativeCoefficientErrors));
	TestTrue(TEXT("AttackCoefficient=-0.5 error names AttackCoefficient"),
		NegativeCoefficientErrors.ToString().Contains(TEXT("AttackCoefficient")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_007RejectsInvalidHitGeometryAndImpulses,
	"UEMMO.Tasks.M1_007.RejectsInvalidHitGeometryAndImpulses",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_007RejectsInvalidHitGeometryAndImpulses::RunTest(const FString& Parameters)
{
	UAttackDefinition* NegativeExtent = MakeLight01();
	NegativeExtent->HitHalfExtent = FVector(-85.0f, 50.0f, 70.0f);
	FText NegativeExtentErrors;
	TestFalse(TEXT("negative HitHalfExtent component is rejected"),
		ValidateAttackDefinition(*NegativeExtent, NegativeExtentErrors));
	TestTrue(TEXT("negative extent error names HitHalfExtent"),
		NegativeExtentErrors.ToString().Contains(TEXT("HitHalfExtent")));

	UAttackDefinition* ZeroExtent = MakeLight01();
	ZeroExtent->HitHalfExtent = FVector(85.0f, 0.0f, 70.0f);
	FText ZeroExtentErrors;
	TestFalse(TEXT("zero HitHalfExtent component is rejected"),
		ValidateAttackDefinition(*ZeroExtent, ZeroExtentErrors));
	TestTrue(TEXT("zero extent error names HitHalfExtent"),
		ZeroExtentErrors.ToString().Contains(TEXT("HitHalfExtent")));

	UAttackDefinition* NanExtent = MakeLight01();
	NanExtent->HitHalfExtent = FVector(85.0f, 50.0f, NanValue);
	FText NanExtentErrors;
	TestFalse(TEXT("NaN HitHalfExtent component is rejected"),
		ValidateAttackDefinition(*NanExtent, NanExtentErrors));
	TestTrue(TEXT("NaN extent error names HitHalfExtent"),
		NanExtentErrors.ToString().Contains(TEXT("HitHalfExtent")));

	UAttackDefinition* NanOffset = MakeLight01();
	NanOffset->HitOffsetFromFeet = FVector(NanValue, 0.0f, 90.0f);
	FText NanOffsetErrors;
	TestFalse(TEXT("NaN HitOffsetFromFeet component is rejected"),
		ValidateAttackDefinition(*NanOffset, NanOffsetErrors));
	TestTrue(TEXT("NaN offset error names HitOffsetFromFeet"),
		NanOffsetErrors.ToString().Contains(TEXT("HitOffsetFromFeet")));

	UAttackDefinition* InfiniteOffset = MakeLight01();
	InfiniteOffset->HitOffsetFromFeet = FVector(95.0f, InfValue, 90.0f);
	FText InfiniteOffsetErrors;
	TestFalse(TEXT("infinite HitOffsetFromFeet component is rejected"),
		ValidateAttackDefinition(*InfiniteOffset, InfiniteOffsetErrors));
	TestTrue(TEXT("infinite offset error names HitOffsetFromFeet"),
		InfiniteOffsetErrors.ToString().Contains(TEXT("HitOffsetFromFeet")));

	UAttackDefinition* NegativeKnockback = MakeLight01();
	NegativeKnockback->KnockbackSpeed = -5.0f;
	FText NegativeKnockbackErrors;
	TestFalse(TEXT("KnockbackSpeed=-5 is rejected"), ValidateAttackDefinition(*NegativeKnockback, NegativeKnockbackErrors));
	TestTrue(TEXT("negative knockback error names KnockbackSpeed"),
		NegativeKnockbackErrors.ToString().Contains(TEXT("KnockbackSpeed")));

	UAttackDefinition* InfiniteKnockback = MakeLight01();
	InfiniteKnockback->KnockbackSpeed = InfValue;
	FText InfiniteKnockbackErrors;
	TestFalse(TEXT("KnockbackSpeed=+Inf is rejected"), ValidateAttackDefinition(*InfiniteKnockback, InfiniteKnockbackErrors));
	TestTrue(TEXT("infinite knockback error names KnockbackSpeed"),
		InfiniteKnockbackErrors.ToString().Contains(TEXT("KnockbackSpeed")));

	UAttackDefinition* NegativeLaunch = MakeLight01();
	NegativeLaunch->LaunchSpeed = -1.0f;
	FText NegativeLaunchErrors;
	TestFalse(TEXT("LaunchSpeed=-1 is rejected"), ValidateAttackDefinition(*NegativeLaunch, NegativeLaunchErrors));
	TestTrue(TEXT("negative launch error names LaunchSpeed"),
		NegativeLaunchErrors.ToString().Contains(TEXT("LaunchSpeed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_007RejectsInvalidStunAndHitStop,
	"UEMMO.Tasks.M1_007.RejectsInvalidStunAndHitStop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_007RejectsInvalidStunAndHitStop::RunTest(const FString& Parameters)
{
	UAttackDefinition* NanStun = MakeLight01();
	NanStun->HitStunSeconds = NanValue;
	FText NanStunErrors;
	TestFalse(TEXT("HitStunSeconds=NaN is rejected"), ValidateAttackDefinition(*NanStun, NanStunErrors));
	TestTrue(TEXT("HitStunSeconds=NaN error names HitStunSeconds"),
		NanStunErrors.ToString().Contains(TEXT("HitStunSeconds")));

	UAttackDefinition* NegativeStun = MakeLight01();
	NegativeStun->HitStunSeconds = -0.1f;
	FText NegativeStunErrors;
	TestFalse(TEXT("HitStunSeconds=-0.1 is rejected"), ValidateAttackDefinition(*NegativeStun, NegativeStunErrors));
	TestTrue(TEXT("negative stun error names HitStunSeconds"),
		NegativeStunErrors.ToString().Contains(TEXT("HitStunSeconds")));

	UAttackDefinition* NanHitStop = MakeLight01();
	NanHitStop->HitStopSeconds = NanValue;
	FText NanHitStopErrors;
	TestFalse(TEXT("HitStopSeconds=NaN is rejected"), ValidateAttackDefinition(*NanHitStop, NanHitStopErrors));
	TestTrue(TEXT("HitStopSeconds=NaN error names HitStopSeconds"),
		NanHitStopErrors.ToString().Contains(TEXT("HitStopSeconds")));

	UAttackDefinition* NegativeHitStop = MakeLight01();
	NegativeHitStop->HitStopSeconds = -0.01f;
	FText NegativeHitStopErrors;
	TestFalse(TEXT("HitStopSeconds=-0.01 is rejected"), ValidateAttackDefinition(*NegativeHitStop, NegativeHitStopErrors));
	TestTrue(TEXT("negative hit stop error names HitStopSeconds"),
		NegativeHitStopErrors.ToString().Contains(TEXT("HitStopSeconds")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_007ClipSecondsRules,
	"UEMMO.Tasks.M1_007.ClipSecondsRules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_007ClipSecondsRules::RunTest(const FString& Parameters)
{
	// Placeholder animations skip the clip ordering check entirely.
	UAttackDefinition* PlaceholderReversed = MakeLight01();
	PlaceholderReversed->ClipStartSeconds = 5.0f;
	PlaceholderReversed->ClipEndSeconds = 2.0f;
	FText PlaceholderReversedErrors;
	TestTrue(TEXT("reversed clip seconds on a placeholder animation is accepted"),
		ValidateAttackDefinition(*PlaceholderReversed, PlaceholderReversedErrors));

	// Negative clip seconds are never finite-and-nonnegative, even on placeholders.
	UAttackDefinition* NegativeClip = MakeLight01();
	NegativeClip->ClipStartSeconds = -1.0f;
	FText NegativeClipErrors;
	TestFalse(TEXT("negative ClipStartSeconds is rejected"), ValidateAttackDefinition(*NegativeClip, NegativeClipErrors));
	TestTrue(TEXT("negative clip error names ClipStartSeconds"),
		NegativeClipErrors.ToString().Contains(TEXT("ClipStartSeconds")));

	// Reversed clip on a real (set, non-placeholder) animation is rejected and names the fields.
	UAttackDefinition* RealReversed = MakeLight01();
	RealReversed->Animation = MakeTestAnimation();
	RealReversed->bPlaceholderAnimation = false;
	RealReversed->ClipStartSeconds = 5.0f;
	RealReversed->ClipEndSeconds = 2.0f;
	FText RealReversedErrors;
	TestFalse(TEXT("reversed clip seconds on a real animation is rejected"),
		ValidateAttackDefinition(*RealReversed, RealReversedErrors));
	TestTrue(TEXT("reversed clip error names ClipEndSeconds"),
		RealReversedErrors.ToString().Contains(TEXT("ClipEndSeconds")));
	TestTrue(TEXT("reversed clip error names ClipStartSeconds"),
		RealReversedErrors.ToString().Contains(TEXT("ClipStartSeconds")));

	// Ascending clip seconds on a real animation are accepted.
	UAttackDefinition* RealAscending = MakeLight01();
	RealAscending->Animation = MakeTestAnimation();
	RealAscending->bPlaceholderAnimation = false;
	RealAscending->ClipStartSeconds = 2.0f;
	RealAscending->ClipEndSeconds = 5.0f;
	FText RealAscendingErrors;
	TestTrue(TEXT("ascending clip seconds on a real animation is accepted"),
		ValidateAttackDefinition(*RealAscending, RealAscendingErrors));

	// Equal start and end seconds are still valid (>=, not >).
	UAttackDefinition* RealEqual = MakeLight01();
	RealEqual->Animation = MakeTestAnimation();
	RealEqual->bPlaceholderAnimation = false;
	RealEqual->ClipStartSeconds = 2.0f;
	RealEqual->ClipEndSeconds = 2.0f;
	FText RealEqualErrors;
	TestTrue(TEXT("equal clip seconds on a real animation is accepted"),
		ValidateAttackDefinition(*RealEqual, RealEqualErrors));

	// A reversed clip is not ordered-checked when the animation reference is unset.
	UAttackDefinition* UnsetAnimationReversed = MakeLight01();
	UnsetAnimationReversed->bPlaceholderAnimation = false;
	UnsetAnimationReversed->ClipStartSeconds = 5.0f;
	UnsetAnimationReversed->ClipEndSeconds = 2.0f;
	FText UnsetAnimationReversedErrors;
	TestTrue(TEXT("reversed clip seconds without an animation reference is accepted"),
		ValidateAttackDefinition(*UnsetAnimationReversed, UnsetAnimationReversedErrors));

	// Non-finite clip seconds are rejected on a real animation.
	UAttackDefinition* NanClip = MakeLight01();
	NanClip->Animation = MakeTestAnimation();
	NanClip->bPlaceholderAnimation = false;
	NanClip->ClipEndSeconds = NanValue;
	FText NanClipErrors;
	TestFalse(TEXT("NaN ClipEndSeconds on a real animation is rejected"),
		ValidateAttackDefinition(*NanClip, NanClipErrors));
	TestTrue(TEXT("NaN clip error names ClipEndSeconds"),
		NanClipErrors.ToString().Contains(TEXT("ClipEndSeconds")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_007ErrorsJoinWithSemicolons,
	"UEMMO.Tasks.M1_007.ErrorsJoinWithSemicolons",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_007ErrorsJoinWithSemicolons::RunTest(const FString& Parameters)
{
	// Several problems at once are collected and joined with "; ".
	UAttackDefinition* Broken = MakeLight01();
	Broken->DurationFrames = -5;
	Broken->BaseDamage = -1.0f;
	Broken->KnockbackSpeed = -5.0f;
	FText Errors;
	TestFalse(TEXT("an attack with multiple problems is rejected"), ValidateAttackDefinition(*Broken, Errors));
	TestTrue(TEXT("DurationFrames problem is included"), Errors.ToString().Contains(TEXT("DurationFrames")));
	TestTrue(TEXT("BaseDamage problem is included"), Errors.ToString().Contains(TEXT("BaseDamage")));
	TestTrue(TEXT("KnockbackSpeed problem is included"), Errors.ToString().Contains(TEXT("KnockbackSpeed")));
	TestTrue(TEXT("problems are joined with \"; \""),
		Errors.ToString().Contains(TEXT("; ")));
	return true;
}

#endif
