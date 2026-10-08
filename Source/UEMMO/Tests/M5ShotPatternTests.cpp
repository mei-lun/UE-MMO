// M5-025: the multi-pellet shot pattern - deterministic directions and the
// world origin of one committed shot (M5 interface contract section 4, owner
// 025 "ShotPattern"). Pins Weapons/ShotPattern.h through pure value tests
// (no World - the caller injects the feet location and the optional resolved
// muzzle socket location):
//
// - The same seed replays the same volley byte-identically, back to back
//   (a shared/global stream would leak state and break the replay);
//   different seeds spread differently ("散布使用Shot专有seed，不扰全局掉落
//   随机数"); every pellet carries a unique FPelletIndex 0..N-1.
// - Zero spread is the exact straight line (Facing, 0, 0) ("散布0为直线").
// - The facing mirrors X only: same-seed volleys at Facing +1/-1 share the Y
//   (纵深) and Z components pellet by pellet and flip only X; the feet offset
//   mirrors X the same way while Y depth and Z height pass through.
// - The optional socket: resolved socket wins, a missing optional socket
//   falls back with a named, locatable detail, a missing required socket
//   refuses the whole plan.
// - Atomic planning: an out-of-range pellet count or an insufficient pellet
//   capacity refuses with zero output ("无半串/半扣弹"); a full volley
//   consumes exactly one round ("单次霰弹耗一发").

#include "Misc/AutomationTest.h"

#include "../Weapons/ShotPattern.h"
#include "../Weapons/WeaponTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_025
{
	// The directed-suite spread inside the frozen 0..45 degree bound; wide
	// enough that same-seed/different-seed volleys reliably diverge.
	constexpr float TestSpreadDegrees = 12.0f;
	constexpr float AngleEpsilon = 1e-4f;
	constexpr float UnitEpsilon = 1e-6f;

	/** A feet position with distinct, non-zero components on all three axes. */
	const FVector FeetLocation(100.0, 200.0, 300.0);

	/** True when the direction is unit-length within the test epsilon. */
	bool IsUnitDirection(const FVector& Direction)
	{
		return FMath::Abs(static_cast<float>(Direction.Size()) - 1.0f) < UnitEpsilon;
	}

	/** The angular deviation of the direction from its facing baseline, in degrees. */
	double DeviationDegrees(const FVector& Direction, float Facing)
	{
		const FVector Baseline(static_cast<double>(Facing), 0.0, 0.0);
		const double Dot = FMath::Clamp(Direction | Baseline, -1.0, 1.0);
		return FMath::RadiansToDegrees(FMath::Acos(Dot));
	}
}

using namespace UE::UEMMO::Tasks::M5_025;

/**
 * Acceptance: the same seed and inputs replay the same directions byte for
 * byte across back-to-back calls (a shared stream would leak state), a
 * different seed spreads differently, and every pellet owns a unique
 * FPelletIndex ("同seed方向一致，不同pellet不共用ID").
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_025SameSeedReproducibleDirections,
	"UEMMO.Tasks.M5_025.SameSeedReproducibleDirections",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_025SameSeedReproducibleDirections::RunTest(const FString& Parameters)
{
	FShotPatternRequest Request;
	Request.PelletCount = 8;
	Request.SpreadDegrees = TestSpreadDegrees;
	Request.Seed = 0x0250A11Cu;
	Request.AvailablePelletSlots = 8;
	Request.Facing = 1.0f;

	// Back-to-back identical replays: any hidden shared state (a global or
	// static stream) would make the second call diverge - this is also the
	// "does not disturb shared randomness" proof the card pins.
	FShotPatternRequest FirstRequest = Request;
	const FShotPatternPlan First = PlanShotPattern(FirstRequest);
	FShotPatternRequest SecondRequest = Request;
	const FShotPatternPlan Second = PlanShotPattern(SecondRequest);
	TestTrue(TEXT("the first replay plans"), First.bPlanned);
	TestTrue(TEXT("the second replay plans"), Second.bPlanned);
	TestEqual(TEXT("the replays agree on the volley size"), Second.Pellets.Num(), First.Pellets.Num());
	for (int32 Index = 0; Index < First.Pellets.Num(); ++Index)
	{
		TestEqual(TEXT("the replay reuses the same pellet index"),
			static_cast<int32>(Second.Pellets[Index].PelletIndex), static_cast<int32>(First.Pellets[Index].PelletIndex));
		TestTrue(TEXT("the replay repeats the same direction"),
			Second.Pellets[Index].Direction.Equals(First.Pellets[Index].Direction));
	}

	// A different seed spreads differently: the directions diverge somewhere.
	FShotPatternRequest OtherSeedRequest = Request;
	OtherSeedRequest.Seed = 0x0250B22Du;
	const FShotPatternPlan OtherSeed = PlanShotPattern(OtherSeedRequest);
	TestTrue(TEXT("the other seed plans"), OtherSeed.bPlanned);
	bool bAnyDirectionDiffers = false;
	for (int32 Index = 0; Index < First.Pellets.Num(); ++Index)
	{
		if (!OtherSeed.Pellets[Index].Direction.Equals(First.Pellets[Index].Direction))
		{
			bAnyDirectionDiffers = true;
		}
	}
	TestTrue(TEXT("a different seed produces different directions"), bAnyDirectionDiffers);

	// Every pellet owns a unique FPelletIndex covering 0..N-1 exactly once.
	TArray<bool> Seen;
	Seen.Init(false, First.Pellets.Num());
	for (const FShotPellet& Pellet : First.Pellets)
	{
		TestTrue(TEXT("the pellet index is inside the volley"), static_cast<int32>(Pellet.PelletIndex) < Seen.Num());
		if (static_cast<int32>(Pellet.PelletIndex) < Seen.Num())
		{
			TestFalse(TEXT("no two pellets share an index"), Seen[Pellet.PelletIndex]);
			Seen[Pellet.PelletIndex] = true;
		}
	}
	for (int32 Index = 0; Index < Seen.Num(); ++Index)
	{
		TestTrue(FString::Printf(TEXT("pellet index %d was issued exactly once"), Index), Seen[Index]);
	}
	return true;
}

/**
 * Acceptance: zero spread is the exact straight line along the facing - every
 * pellet points (Facing, 0, 0) with no Y depth and no Z height ("散布0为直线").
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_025ZeroSpreadIsStraightLine,
	"UEMMO.Tasks.M5_025.ZeroSpreadIsStraightLine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_025ZeroSpreadIsStraightLine::RunTest(const FString& Parameters)
{
	FShotPatternRequest Request;
	Request.PelletCount = 8;
	Request.SpreadDegrees = 0.0f;
	Request.Seed = 0x0250C33Eu;
	Request.AvailablePelletSlots = 8;
	Request.Facing = 1.0f;

	const FShotPatternPlan Forward = PlanShotPattern(Request);
	TestTrue(TEXT("the forward volley plans"), Forward.bPlanned);
	for (const FShotPellet& Pellet : Forward.Pellets)
	{
		TestTrue(TEXT("the forward pellet is the exact +X line"),
			Pellet.Direction.Equals(FVector(1.0, 0.0, 0.0)));
	}

	// The mirrored baseline: zero spread never invents depth or height.
	Request.Facing = -1.0f;
	const FShotPatternPlan Backward = PlanShotPattern(Request);
	TestTrue(TEXT("the backward volley plans"), Backward.bPlanned);
	for (const FShotPellet& Pellet : Backward.Pellets)
	{
		TestTrue(TEXT("the backward pellet is the exact -X line"),
			Pellet.Direction.Equals(FVector(-1.0, 0.0, 0.0)));
	}
	return true;
}

/**
 * Acceptance: with spread, the facing mirrors X only - same-seed volleys at
 * Facing +1 and -1 share every pellet's Y (纵深) and Z components and flip
 * only X ("Facing只镜像X，不锁Y"); the feet offset mirrors X the same way
 * while the Y depth and Z height offsets pass through ("Y纵深/玩家高度起点
 * 正确").
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_025FacingMirrorsXOnlyNotY,
	"UEMMO.Tasks.M5_025.FacingMirrorsXOnlyNotY",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_025FacingMirrorsXOnlyNotY::RunTest(const FString& Parameters)
{
	const int32 PelletCount = 32;
	FShotPatternRequest Forward;
	Forward.PelletCount = PelletCount;
	Forward.SpreadDegrees = TestSpreadDegrees;
	Forward.Seed = 0x0250D44Fu;
	Forward.AvailablePelletSlots = PelletCount;
	Forward.Facing = 1.0f;
	const FShotPatternPlan FacingPlus = PlanShotPattern(Forward);

	Forward.Facing = -1.0f;
	const FShotPatternPlan FacingMinus = PlanShotPattern(Forward);
	if (!TestTrue(TEXT("the +1 volley plans"), FacingPlus.bPlanned)
		|| !TestTrue(TEXT("the -1 volley plans"), FacingMinus.bPlanned)
		|| FacingPlus.Pellets.Num() != PelletCount
		|| FacingMinus.Pellets.Num() != PelletCount)
	{
		AddError(TEXT("cannot compare facings without two planned volleys"));
		return false;
	}

	for (int32 Index = 0; Index < PelletCount; ++Index)
	{
		const FShotPellet& Plus = FacingPlus.Pellets[Index];
		const FShotPellet& Minus = FacingMinus.Pellets[Index];
		// X mirrors, Y (depth) and Z (height) do not.
		TestTrue(FString::Printf(TEXT("pellet %d Y is not mirrored by the facing"), Index),
			FMath::IsNearlyEqual(Minus.Direction.Y, Plus.Direction.Y, AngleEpsilon));
		TestTrue(FString::Printf(TEXT("pellet %d Z is not mirrored by the facing"), Index),
			FMath::IsNearlyEqual(Minus.Direction.Z, Plus.Direction.Z, AngleEpsilon));
		TestTrue(FString::Printf(TEXT("pellet %d X mirrors"), Index),
			FMath::IsNearlyEqual(Minus.Direction.X, -Plus.Direction.X, AngleEpsilon));
		// Every pellet stays inside the configured spread cone.
		TestTrue(FString::Printf(TEXT("pellet %d stays inside the spread cone"), Index),
			DeviationDegrees(Plus.Direction, 1.0f) <= static_cast<double>(TestSpreadDegrees) + AngleEpsilon);
		TestTrue(FString::Printf(TEXT("pellet %d is unit length"), Index), IsUnitDirection(Plus.Direction));
	}

	// The origin: the feet offset mirrors X only - Y depth and Z height pass
	// through at both facings ("±Facing与Y纵深/玩家高度起点正确").
	FShotPatternRequest OriginRequest;
	OriginRequest.PelletCount = 1;
	OriginRequest.SpreadDegrees = 0.0f;
	OriginRequest.Seed = 1u;
	OriginRequest.AvailablePelletSlots = 1;
	OriginRequest.Origin.FeetOffsetCm = FVector(20.0, 30.0, 80.0);
	OriginRequest.FeetWorldLocation = FeetLocation;

	OriginRequest.Facing = 1.0f;
	const FShotPatternPlan OriginPlus = PlanShotPattern(OriginRequest);
	TestTrue(TEXT("the +1 origin plans"), OriginPlus.bPlanned);
	TestTrue(TEXT("the +1 origin is the feet plus the offset"),
		OriginPlus.Origin.WorldLocation.Equals(FeetLocation + FVector(20.0, 30.0, 80.0)));

	OriginRequest.Facing = -1.0f;
	const FShotPatternPlan OriginMinus = PlanShotPattern(OriginRequest);
	TestTrue(TEXT("the -1 origin plans"), OriginMinus.bPlanned);
	TestTrue(TEXT("the -1 origin mirrors X, keeps the Y depth and the Z height"),
		OriginMinus.Origin.WorldLocation.Equals(FeetLocation + FVector(-20.0, 30.0, 80.0)));
	return true;
}

/**
 * Acceptance: the socket is an optional reference - a resolved socket wins,
 * a missing optional socket falls back to the offset with a named, locatable
 * detail, and a missing required socket refuses the whole plan
 * ("缺必需socket可定位").
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_025SocketOptionalMissingIsLocatable,
	"UEMMO.Tasks.M5_025.SocketOptionalMissingIsLocatable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_025SocketOptionalMissingIsLocatable::RunTest(const FString& Parameters)
{
	const FVector SocketWorld(500.0, 210.0, 380.0);
	FShotPatternRequest Request;
	Request.PelletCount = 1;
	Request.SpreadDegrees = 0.0f;
	Request.Seed = 1u;
	Request.AvailablePelletSlots = 1;
	Request.Origin.FeetOffsetCm = FVector(20.0, 0.0, 80.0);
	Request.Origin.MuzzleSocketName = TEXT("muzzle_hand_r");
	Request.Facing = 1.0f;
	Request.FeetWorldLocation = FeetLocation;

	// A resolved socket wins over the feet offset.
	const FVector ResolvedSocket = SocketWorld;
	Request.MuzzleSocketWorldLocation = &ResolvedSocket;
	const FShotPatternPlan WithSocket = PlanShotPattern(Request);
	TestTrue(TEXT("the socket volley plans"), WithSocket.bPlanned);
	TestTrue(TEXT("the origin uses the socket"), WithSocket.Origin.bUsedSocket);
	TestFalse(TEXT("the socket origin is not a fallback"), WithSocket.Origin.bSocketFellBack);
	TestTrue(TEXT("the socket origin is the resolved socket location"),
		WithSocket.Origin.WorldLocation.Equals(SocketWorld));

	// A missing optional socket falls back to the offset; the detail names
	// the socket so the problem is locatable.
	Request.MuzzleSocketWorldLocation = nullptr;
	const FShotPatternPlan Fallback = PlanShotPattern(Request);
	TestTrue(TEXT("the fallback volley plans"), Fallback.bPlanned);
	TestFalse(TEXT("the fallback did not claim the socket"), Fallback.Origin.bUsedSocket);
	TestTrue(TEXT("the fallback is flagged"), Fallback.Origin.bSocketFellBack);
	TestEqual(TEXT("the fallback names the socket"), Fallback.Origin.FallbackSocketName,
		FName(TEXT("muzzle_hand_r")));
	TestTrue(TEXT("the fallback detail is locatable"), Fallback.Origin.FallbackSocketName != NAME_None);
	TestTrue(TEXT("the fallback origin is the feet offset"),
		Fallback.Origin.WorldLocation.Equals(FeetLocation + FVector(20.0, 0.0, 80.0)));

	// A missing required socket refuses the whole plan with zero output.
	Request.Origin.bRequireSocket = true;
	const FShotPatternPlan Refused = PlanShotPattern(Request);
	TestFalse(TEXT("the required-socket volley is refused"), Refused.bPlanned);
	TestEqual(TEXT("the refusal names the missing socket"), static_cast<int32>(Refused.Reject),
		static_cast<int32>(EShotPatternReject::RequiredSocketMissing));
	TestEqual(TEXT("the refusal deducts nothing"), Refused.RoundsConsumed, 0);
	TestEqual(TEXT("the refusal issues no pellets"), Refused.Pellets.Num(), 0);
	TestTrue(TEXT("the refusal detail names the socket"), Refused.RejectDetail.Contains(TEXT("muzzle_hand_r")));

	// No socket named: the plain feet-offset path, no fallback flag.
	Request.Origin.MuzzleSocketName = NAME_None;
	Request.Origin.bRequireSocket = false;
	const FShotPatternPlan Plain = PlanShotPattern(Request);
	TestTrue(TEXT("the plain offset volley plans"), Plain.bPlanned);
	TestFalse(TEXT("the plain origin is not a socket"), Plain.Origin.bUsedSocket);
	TestFalse(TEXT("the plain origin is not a fallback"), Plain.Origin.bSocketFellBack);
	TestTrue(TEXT("the plain origin is the feet offset"),
		Plain.Origin.WorldLocation.Equals(FeetLocation + FVector(20.0, 0.0, 80.0)));
	return true;
}

/**
 * Acceptance: planning is atomic - an out-of-range pellet count (below 1 or
 * beyond MaxWeaponPelletCount) is refused by name, and an insufficient pellet
 * capacity refuses the whole volley ("pellet_count越界拒绝；全体容量不足无
 * 半串/半扣弹").
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_025PelletCountAndCapacityAtomic,
	"UEMMO.Tasks.M5_025.PelletCountAndCapacityAtomic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_025PelletCountAndCapacityAtomic::RunTest(const FString& Parameters)
{
	FShotPatternRequest Request;
	Request.PelletCount = 8;
	Request.SpreadDegrees = TestSpreadDegrees;
	Request.Seed = 0x0250E55Au;
	Request.AvailablePelletSlots = 8;
	Request.Facing = 1.0f;

	// Out-of-range pellet counts are refused by name, with zero output.
	const int32 OutOfRangeCounts[] = { 0, -1, MaxWeaponPelletCount + 1 };
	for (const int32 BadCount : OutOfRangeCounts)
	{
		Request.PelletCount = BadCount;
		const FShotPatternPlan Refused = PlanShotPattern(Request);
		const FString What = FString::Printf(TEXT("pellet count %d is refused"), BadCount);
		TestFalse(What, Refused.bPlanned);
		TestEqual(FString::Printf(TEXT("%s names the bounds"), *What),
			static_cast<int32>(Refused.Reject), static_cast<int32>(EShotPatternReject::InvalidPelletCount));
		TestEqual(FString::Printf(TEXT("%s deducts nothing"), *What), Refused.RoundsConsumed, 0);
		TestEqual(FString::Printf(TEXT("%s issues no pellets"), *What), Refused.Pellets.Num(), 0);
	}
	Request.PelletCount = 8;

	// An insufficient capacity refuses the whole volley: no half volley and
	// no partial deduction.
	Request.AvailablePelletSlots = 4;
	const FShotPatternPlan Insufficient = PlanShotPattern(Request);
	TestFalse(TEXT("the under-capacity volley is refused"), Insufficient.bPlanned);
	TestEqual(TEXT("the under-capacity refusal names the capacity"), static_cast<int32>(Insufficient.Reject),
		static_cast<int32>(EShotPatternReject::CapacityInsufficient));
	TestEqual(TEXT("the under-capacity refusal deducts nothing"), Insufficient.RoundsConsumed, 0);
	TestEqual(TEXT("the under-capacity refusal issues no pellets"), Insufficient.Pellets.Num(), 0);

	// Exactly-fitting capacity plans the full volley.
	Request.AvailablePelletSlots = 8;
	const FShotPatternPlan Exact = PlanShotPattern(Request);
	TestTrue(TEXT("the exact-capacity volley plans"), Exact.bPlanned);
	TestEqual(TEXT("the exact-capacity volley is complete"), Exact.Pellets.Num(), 8);
	return true;
}

/**
 * Acceptance: one trigger, one round - a full multi-pellet volley consumes
 * exactly one round while every pellet gets its own unique index and a unit
 * direction inside the configured cone ("单次霰弹耗一发，各PelletIndex唯一").
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_025FullVolleyConsumesOneRound,
	"UEMMO.Tasks.M5_025.FullVolleyConsumesOneRound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_025FullVolleyConsumesOneRound::RunTest(const FString& Parameters)
{
	FShotPatternRequest Request;
	Request.PelletCount = 8;
	Request.SpreadDegrees = TestSpreadDegrees;
	Request.Seed = 0x0250F66Bu;
	Request.AvailablePelletSlots = 8;
	Request.Facing = -1.0f;
	Request.Origin.FeetOffsetCm = FVector(20.0, 0.0, 80.0);
	Request.FeetWorldLocation = FeetLocation;

	const FShotPatternPlan Volley = PlanShotPattern(Request);
	TestTrue(TEXT("the volley plans"), Volley.bPlanned);
	TestEqual(TEXT("the full volley consumes exactly one round"), Volley.RoundsConsumed, 1);
	TestEqual(TEXT("the volley carries every pellet"), Volley.Pellets.Num(), 8);

	// The single-pellet shot stays index 0.
	FShotPatternRequest Single = Request;
	Single.PelletCount = 1;
	Single.AvailablePelletSlots = 1;
	const FShotPatternPlan SinglePlan = PlanShotPattern(Single);
	if (!TestTrue(TEXT("the single pellet plans"), SinglePlan.bPlanned)
		|| SinglePlan.Pellets.Num() != 1)
	{
		AddError(TEXT("cannot check the single-pellet index without a planned shot"));
		return false;
	}
	TestEqual(TEXT("the single pellet is index 0"),
		static_cast<int32>(SinglePlan.Pellets[0].PelletIndex), 0);

	// Every pellet is unique, unit length and inside the cone.
	TArray<bool> Seen;
	Seen.Init(false, 8);
	for (const FShotPellet& Pellet : Volley.Pellets)
	{
		const int32 Index = static_cast<int32>(Pellet.PelletIndex);
		TestTrue(TEXT("the pellet index is inside the volley"), Index < Seen.Num());
		if (Index >= 0 && Index < Seen.Num())
		{
			TestFalse(FString::Printf(TEXT("pellet index %d is unique"), Index), Seen[Index]);
			Seen[Index] = true;
		}
		TestTrue(TEXT("the pellet direction is unit length"), IsUnitDirection(Pellet.Direction));
		TestTrue(TEXT("the pellet stays inside the spread cone"),
			DeviationDegrees(Pellet.Direction, Request.Facing)
				<= static_cast<double>(TestSpreadDegrees) + AngleEpsilon);
	}
	return true;
}

#endif
