// M5-040: the atomic single-seat occupancy/permission model (interface
// contract section 1 owner 040, section 8 "载具权限"). The suite runs the
// model entirely in memory - no World, no map, no Possess, no actors - with
// the vehicle/seat ids taken from the shipped M5-037 single-seat ground data
// and synthetic FGuids as World-minted occupant identities, locking:
//
//   1. Atomic double-send: the identical enter request succeeds exactly once;
//      the re-send is refused with AlreadyInThisSeat and changes nothing.
//      A second occupant is refused with SeatOccupied while the holder keeps
//      the seat (and gets in once the holder left).
//   2. One seat per occupant: a seated occupant is refused on a second seat
//      with OccupantAlreadySeated (even when that seat is also taken - the
//      refusal-check order is fixed and deterministic).
//   3. Injected gates: the distance / vehicle-validity / drive-fire
//      permission gates refuse with their own reasons (DistanceTooFar /
//      VehicleInvalid / DriveDenied) and leave the state unchanged; they
//      receive the request ids as a value snapshot; unset gates allow (the
//      bare model is permissive by default).
//   4. Idempotent exits: repeated LeaveSeat is a safe no-op; vehicle cleanup
//      (ClearVehicle) frees occupancy while the registration survives, so a
//      NEW occupant can take the same vehicle again; Reset returns the full
//      factory baseline (registrations included).
//   5. Structural refusals: unusable ids (NAME_None / zero GUID), unknown
//      seats and duplicate registrations are refused with explicit reasons
//      and human-readable detail lines naming the ids.

#include "Misc/AutomationTest.h"

#include <type_traits>

#include "../Vehicles/SeatModel.h"

#if WITH_DEV_AUTOMATION_TESTS

// The pins of the header, repeated at the test site (M5-010 precedent): the
// model speaks ids only - never actor pointers.
static_assert(std::is_trivially_copyable_v<FVehicleSeatEnterRequest>, "FVehicleSeatEnterRequest must stay a trivially copyable value snapshot");
static_assert(std::is_trivially_copyable_v<FVehicleSeatKey>, "FVehicleSeatKey must stay a trivially copyable value snapshot");
static_assert(!std::is_pointer_v<decltype(FVehicleSeatEnterRequest::OccupantId)>, "FVehicleSeatEnterRequest must stay pointer-free");
static_assert(std::is_same_v<std::underlying_type_t<EVehicleSeatRejectReason>, uint8>, "seat reject reasons stay an append-only uint8 enum");

namespace UE::UEMMO::Tasks::M5_040
{
	// The shipped single-seat ground vehicle of the M5-037 data (vehicles.json
	// / vehicle_seats.json) - the real production ids, not test slang.
	constexpr TCHAR VehicleOneId[] = TEXT("vehicle_wheeled_sample");
	constexpr TCHAR SeatOneId[] = TEXT("seat_wheeled_driver");

	// A second synthetic single-seat vehicle for cross-vehicle scenarios.
	constexpr TCHAR VehicleTwoId[] = TEXT("vehicle_wheeled_second");
	constexpr TCHAR SeatTwoId[] = TEXT("seat_wheeled_second");

	/** A stable non-zero occupant identity (World-minted, model-opaque). */
	FGuid MakeOccupant(uint32 Seed)
	{
		return FGuid(Seed, Seed + 1, Seed + 2, Seed + 3);
	}

	/** The enter request for one (vehicle, seat, occupant) triple. */
	FVehicleSeatEnterRequest MakeRequest(const TCHAR* VehicleId, const TCHAR* SeatId, FGuid OccupantId)
	{
		FVehicleSeatEnterRequest Request;
		Request.VehicleId = FName(VehicleId);
		Request.SeatId = FName(SeatId);
		Request.OccupantId = OccupantId;
		return Request;
	}

	/**
	 * Registers both sample seats; returns false after failing the test when
	 * a registration refuses (every later step would read confusingly).
	 */
	bool RegisterSampleSeats(FAutomationTestBase& Automation, FSeatOccupancyModel& Model, const FString& ContextText)
	{
		EVehicleSeatRejectReason Reason = EVehicleSeatRejectReason::None;
		const bool bOne = Model.RegisterSeat(FName(VehicleOneId), FName(SeatOneId), Reason);
		const bool bTwo = Model.RegisterSeat(FName(VehicleTwoId), FName(SeatTwoId), Reason);
		Automation.TestTrue(ContextText + TEXT(": both sample seats register"), bOne && bTwo);
		Automation.TestEqual(TEXT("two seats are registered"), Model.GetNumRegisteredSeats(), 2);
		return bOne && bTwo;
	}
}

using namespace UE::UEMMO::Tasks::M5_040;

// ---------------------------------------------------------------------------
// DoubleEnterOccupiesOnce - acceptance: 双enter只占一次
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_040DoubleEnterOccupiesOnce,
	"UEMMO.Tasks.M5_040.DoubleEnterOccupiesOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_040DoubleEnterOccupiesOnce::RunTest(const FString& Parameters)
{
	FSeatOccupancyModel Model;
	if (!RegisterSampleSeats(*this, Model, TEXT("setup")))
	{
		return false;
	}

	// The bare model (no gates set) is permissive: the first enter is granted.
	const FGuid OccupantA = MakeOccupant(1);
	EVehicleSeatRejectReason Reason = EVehicleSeatRejectReason::None;
	FString Detail;
	TestTrue(TEXT("the first enter is accepted"),
		Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantA), Reason, &Detail));
	TestEqual(TEXT("the accepted enter carries no rejection"), Reason, EVehicleSeatRejectReason::None);
	TestTrue(TEXT("an accepted enter leaves the detail empty"), Detail.IsEmpty());
	TestEqual(TEXT("one seat is occupied"), Model.GetNumOccupiedSeats(), 1);

	FGuid Seated = FGuid(0, 0, 0, 0);
	TestTrue(TEXT("the seat reports its occupant"), Model.FindSeatOccupant(FName(VehicleOneId), FName(SeatOneId), Seated));
	TestTrue(TEXT("the reported occupant is the accepted one"), Seated == OccupantA);

	// The atomic double-send: the identical request succeeds exactly once -
	// the re-send observes the committed state and is refused by name.
	Reason = EVehicleSeatRejectReason::None;
	TestFalse(TEXT("the identical second enter is refused"),
		Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantA), Reason, &Detail));
	TestEqual(TEXT("the double-send names AlreadyInThisSeat"), Reason, EVehicleSeatRejectReason::AlreadyInThisSeat);
	TestTrue(TEXT("the refusal detail is human-readable"), !Detail.IsEmpty());
	TestTrue(TEXT("the refusal detail names the seat"), Detail.Contains(SeatOneId));
	TestTrue(TEXT("the refusal detail names the vehicle"), Detail.Contains(VehicleOneId));
	TestEqual(TEXT("the refused re-send recorded no second occupation"), Model.GetNumOccupiedSeats(), 1);
	TestEqual(TEXT("no seat registration was added by the re-send"), Model.GetNumRegisteredSeats(), 2);

	Seated = FGuid(0, 0, 0, 0);
	TestTrue(TEXT("the seat still reports its occupant"), Model.FindSeatOccupant(FName(VehicleOneId), FName(SeatOneId), Seated));
	TestTrue(TEXT("the occupant is unchanged after the double-send"), Seated == OccupantA);
	return true;
}

// ---------------------------------------------------------------------------
// SecondOccupantRefused - acceptance: 第二角色拒绝（原因枚举断言）
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_040SecondOccupantRefused,
	"UEMMO.Tasks.M5_040.SecondOccupantRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_040SecondOccupantRefused::RunTest(const FString& Parameters)
{
	FSeatOccupancyModel Model;
	if (!RegisterSampleSeats(*this, Model, TEXT("setup")))
	{
		return false;
	}

	const FGuid OccupantA = MakeOccupant(1);
	const FGuid OccupantB = MakeOccupant(2);
	EVehicleSeatRejectReason Reason = EVehicleSeatRejectReason::None;
	FString Detail;
	TestTrue(TEXT("the first occupant takes the seat"),
		Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantA), Reason, &Detail));

	// One seat, one occupant: the second occupant is refused with an explicit
	// reason while the holder's state stays untouched.
	Reason = EVehicleSeatRejectReason::None;
	TestFalse(TEXT("the second occupant is refused"),
		Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantB), Reason, &Detail));
	TestEqual(TEXT("the second-occupant refusal names SeatOccupied"), Reason, EVehicleSeatRejectReason::SeatOccupied);
	TestTrue(TEXT("the refusal detail names the occupied seat"), Detail.Contains(SeatOneId));

	FVehicleSeatKey SeatOfB;
	TestFalse(TEXT("the refused occupant stayed seatless"), Model.FindOccupantSeat(OccupantB, SeatOfB));
	FGuid Seated = FGuid(0, 0, 0, 0);
	TestTrue(TEXT("the holder is still seated"), Model.FindSeatOccupant(FName(VehicleOneId), FName(SeatOneId), Seated));
	TestTrue(TEXT("the holder is still occupant A"), Seated == OccupantA);
	TestEqual(TEXT("still exactly one occupation"), Model.GetNumOccupiedSeats(), 1);

	// The refusal was the live occupancy, not a permanent lock: once the
	// holder leaves, the waiting occupant gets in.
	FVehicleSeatLeaveResult Leave;
	TestTrue(TEXT("the holder leaves"), Model.LeaveSeat(OccupantA, &Leave));
	TestTrue(TEXT("the leave reports the vacated vehicle"), Leave.VehicleId == FName(VehicleOneId));
	TestTrue(TEXT("the leave reports the vacated seat"), Leave.SeatId == FName(SeatOneId));

	Reason = EVehicleSeatRejectReason::None;
	TestTrue(TEXT("the second occupant enters the freed seat"),
		Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantB), Reason, &Detail));
	TestTrue(TEXT("the freed seat holds occupant B"), Model.FindSeatOccupant(FName(VehicleOneId), FName(SeatOneId), Seated) && Seated == OccupantB);
	return true;
}

// ---------------------------------------------------------------------------
// GateRefusals - acceptance: 权限非法拒绝（注入拒绝策略）；距离/车辆有效可注入
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_040GateRefusals,
	"UEMMO.Tasks.M5_040.GateRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_040GateRefusals::RunTest(const FString& Parameters)
{
	FSeatOccupancyModel Model;
	if (!RegisterSampleSeats(*this, Model, TEXT("setup")))
	{
		return false;
	}

	const FGuid OccupantA = MakeOccupant(1);
	EVehicleSeatRejectReason Reason = EVehicleSeatRejectReason::None;
	FString Detail;

	// Unset gates allow (default all-allow, card requirement): the bare model
	// grants the enter; the occupant leaves again to reset the scenario.
	TestTrue(TEXT("an unset gate set allows the enter"),
		Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantA), Reason, &Detail));
	FVehicleSeatLeaveResult Leave;
	TestTrue(TEXT("the occupant leaves for the gate scenarios"), Model.LeaveSeat(OccupantA, &Leave));

	// Permission gate refuses (drive/fire rights - the M5-041 wiring point).
	bool bGateCalled = false;
	FVehicleSeatPolicyContext Captured;
	Model.SetPermissionGate([&bGateCalled, &Captured](const FVehicleSeatPolicyContext& Context)
	{
		bGateCalled = true;
		Captured = Context;
		return false;
	});
	Reason = EVehicleSeatRejectReason::None;
	TestFalse(TEXT("a refusing permission gate blocks the enter"),
		Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantA), Reason, &Detail));
	TestEqual(TEXT("the permission refusal names DriveDenied"), Reason, EVehicleSeatRejectReason::DriveDenied);
	TestTrue(TEXT("the permission gate was consulted"), bGateCalled);
	TestTrue(TEXT("the gate context carries the request's vehicle"), Captured.Request.VehicleId == FName(VehicleOneId));
	TestTrue(TEXT("the gate context carries the request's seat"), Captured.Request.SeatId == FName(SeatOneId));
	TestTrue(TEXT("the gate context carries the request's occupant"), Captured.Request.OccupantId == OccupantA);
	TestEqual(TEXT("the refused enter left no occupation"), Model.GetNumOccupiedSeats(), 0);

	// Vehicle-validity gate refuses (hull alive - the M5-039 wiring point).
	Model.SetPermissionGate([](const FVehicleSeatPolicyContext&) { return true; });
	Model.SetVehicleValidityGate([](const FVehicleSeatPolicyContext&) { return false; });
	Reason = EVehicleSeatRejectReason::None;
	TestFalse(TEXT("a refusing vehicle-validity gate blocks the enter"),
		Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantA), Reason, &Detail));
	TestEqual(TEXT("the validity refusal names VehicleInvalid"), Reason, EVehicleSeatRejectReason::VehicleInvalid);
	TestEqual(TEXT("the refused enter still left no occupation"), Model.GetNumOccupiedSeats(), 0);

	// Distance gate refuses (entry proximity - the M5-038/041 wiring point).
	Model.SetVehicleValidityGate([](const FVehicleSeatPolicyContext&) { return true; });
	Model.SetDistanceGate([](const FVehicleSeatPolicyContext&) { return false; });
	Reason = EVehicleSeatRejectReason::None;
	TestFalse(TEXT("a refusing distance gate blocks the enter"),
		Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantA), Reason, &Detail));
	TestEqual(TEXT("the distance refusal names DistanceTooFar"), Reason, EVehicleSeatRejectReason::DistanceTooFar);
	TestEqual(TEXT("the refused enter still left no occupation"), Model.GetNumOccupiedSeats(), 0);

	// All gates allow again: the enter is granted once more (injection, not a
	// permanent lock).
	Model.SetDistanceGate([](const FVehicleSeatPolicyContext&) { return true; });
	Reason = EVehicleSeatRejectReason::None;
	TestTrue(TEXT("an all-allow gate set grants the enter again"),
		Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantA), Reason, &Detail));
	TestEqual(TEXT("the granted enter carries no rejection"), Reason, EVehicleSeatRejectReason::None);
	TestEqual(TEXT("exactly one occupation after the gate round trip"), Model.GetNumOccupiedSeats(), 1);
	return true;
}

// ---------------------------------------------------------------------------
// RepeatedLeaveSafe - acceptance: 重复leave安全
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_040RepeatedLeaveSafe,
	"UEMMO.Tasks.M5_040.RepeatedLeaveSafe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_040RepeatedLeaveSafe::RunTest(const FString& Parameters)
{
	FSeatOccupancyModel Model;
	if (!RegisterSampleSeats(*this, Model, TEXT("setup")))
	{
		return false;
	}

	const FGuid OccupantA = MakeOccupant(1);
	const FGuid OccupantB = MakeOccupant(2);
	EVehicleSeatRejectReason Reason = EVehicleSeatRejectReason::None;
	FString Detail;
	TestTrue(TEXT("the occupant enters"),
		Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantA), Reason, &Detail));

	// The first leave ends the occupation and reports what was vacated.
	FVehicleSeatLeaveResult Leave;
	TestTrue(TEXT("the first leave ends the occupation"), Model.LeaveSeat(OccupantA, &Leave));
	TestTrue(TEXT("the leave result reports success"), Leave.bLeft);
	TestTrue(TEXT("the leave result names the vehicle"), Leave.VehicleId == FName(VehicleOneId));
	TestTrue(TEXT("the leave result names the seat"), Leave.SeatId == FName(SeatOneId));
	TestEqual(TEXT("the vacated seat is free"), Model.GetNumOccupiedSeats(), 0);

	// The repeated leave is the safe no-op: false, no state change, and the
	// optional result is left untouched (only written on success).
	Leave.bLeft = false;
	Leave.VehicleId = NAME_None;
	Leave.SeatId = NAME_None;
	TestFalse(TEXT("the repeated leave is refused as nothing-to-do"), Model.LeaveSeat(OccupantA, &Leave));
	TestFalse(TEXT("the untouched result still reports no leave"), Leave.bLeft);
	TestTrue(TEXT("the untouched result keeps its vehicle field"), Leave.VehicleId == NAME_None);
	TestTrue(TEXT("the untouched result keeps its seat field"), Leave.SeatId == NAME_None);
	TestEqual(TEXT("the repeated leave changed no occupancy"), Model.GetNumOccupiedSeats(), 0);
	TestEqual(TEXT("the repeated leave removed no registration"), Model.GetNumRegisteredSeats(), 2);

	// A leave for an occupant that never sat is the same safe no-op.
	TestFalse(TEXT("a never-seated occupant's leave is refused"), Model.LeaveSeat(OccupantB));
	TestEqual(TEXT("the model is still empty of occupations"), Model.GetNumOccupiedSeats(), 0);

	// The occupant can enter again after the leave round trip (leave is a
	// release, not a ban).
	Reason = EVehicleSeatRejectReason::None;
	TestTrue(TEXT("the same occupant enters again after leaving"),
		Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantA), Reason, &Detail));
	TestEqual(TEXT("one occupation after the re-entry"), Model.GetNumOccupiedSeats(), 1);
	return true;
}

// ---------------------------------------------------------------------------
// VehicleCleanupAllowsNewOccupant - acceptance: 销毁清理后可占新车辆
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_040VehicleCleanupAllowsNewOccupant,
	"UEMMO.Tasks.M5_040.VehicleCleanupAllowsNewOccupant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_040VehicleCleanupAllowsNewOccupant::RunTest(const FString& Parameters)
{
	FSeatOccupancyModel Model;
	if (!RegisterSampleSeats(*this, Model, TEXT("setup")))
	{
		return false;
	}

	const FGuid OccupantA = MakeOccupant(1);
	const FGuid OccupantB = MakeOccupant(2);
	EVehicleSeatRejectReason Reason = EVehicleSeatRejectReason::None;
	FString Detail;
	TestTrue(TEXT("occupant A takes the shipped vehicle's seat"),
		Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantA), Reason, &Detail));

	// Vehicle destroy/unload: the occupancy drops, the registration stays -
	// the vehicle id can be occupied again by a new occupant.
	TestEqual(TEXT("the cleanup freed exactly the vehicle's occupied seat"), Model.ClearVehicle(FName(VehicleOneId)), 1);
	TestEqual(TEXT("no occupation remains"), Model.GetNumOccupiedSeats(), 0);
	TestEqual(TEXT("the seat registration survived the cleanup"), Model.GetNumRegisteredSeats(), 2);

	FGuid Seated = FGuid(0, 0, 0, 0);
	TestFalse(TEXT("the cleaned seat reports no occupant"), Model.FindSeatOccupant(FName(VehicleOneId), FName(SeatOneId), Seated));
	FVehicleSeatKey SeatKey;
	TestFalse(TEXT("the cleaned vehicle's occupant is seatless"), Model.FindOccupantSeat(OccupantA, SeatKey));

	Reason = EVehicleSeatRejectReason::None;
	TestTrue(TEXT("a NEW occupant takes the same vehicle after the cleanup"),
		Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantB), Reason, &Detail));
	TestTrue(TEXT("the re-occupied seat holds occupant B"),
		Model.FindSeatOccupant(FName(VehicleOneId), FName(SeatOneId), Seated) && Seated == OccupantB);

	// The re-occupied vehicle cleans up again: B's seat is released the same
	// way (the cleanup is per-call over CURRENT occupancy, not a once-per-
	// vehicle latch), and an unknown vehicle frees nothing.
	TestEqual(TEXT("a cleanup over the re-occupied vehicle frees B's seat"),
		Model.ClearVehicle(FName(VehicleOneId)), 1);
	TestEqual(TEXT("an unknown vehicle cleanup frees nothing"), Model.ClearVehicle(FName(TEXT("vehicle_unknown"))), 0);
	TestTrue(TEXT("the re-cleaned seat reports no occupant"),
		!Model.FindSeatOccupant(FName(VehicleOneId), FName(SeatOneId), Seated) || Seated != OccupantB);

	// World unload (Reset): the full baseline - registrations included. An
	// enter on a wiped model is refused as an unknown seat until the world
	// wiring re-registers the seat it spawned.
	Model.Reset();
	TestEqual(TEXT("the reset dropped every registration"), Model.GetNumRegisteredSeats(), 0);
	TestEqual(TEXT("the reset dropped every occupation"), Model.GetNumOccupiedSeats(), 0);
	Reason = EVehicleSeatRejectReason::None;
	TestFalse(TEXT("an enter after the reset names UnknownSeat"),
		Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantB), Reason, &Detail));
	TestEqual(TEXT("the post-reset refusal names UnknownSeat"), Reason, EVehicleSeatRejectReason::UnknownSeat);

	Reason = EVehicleSeatRejectReason::None;
	TestTrue(TEXT("the re-registered seat is enterable again"),
		Model.RegisterSeat(FName(VehicleOneId), FName(SeatOneId), Reason)
			&& Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantB), Reason, &Detail));
	TestEqual(TEXT("one occupation after the rebuild cycle"), Model.GetNumOccupiedSeats(), 1);
	return true;
}

// ---------------------------------------------------------------------------
// OccupantLimitedToOneSeat - acceptance: 同角色第二座拒绝
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_040OccupantLimitedToOneSeat,
	"UEMMO.Tasks.M5_040.OccupantLimitedToOneSeat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_040OccupantLimitedToOneSeat::RunTest(const FString& Parameters)
{
	FSeatOccupancyModel Model;
	if (!RegisterSampleSeats(*this, Model, TEXT("setup")))
	{
		return false;
	}

	const FGuid OccupantA = MakeOccupant(1);
	const FGuid OccupantB = MakeOccupant(2);
	EVehicleSeatRejectReason Reason = EVehicleSeatRejectReason::None;
	FString Detail;
	TestTrue(TEXT("occupant A takes the first vehicle's seat"),
		Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantA), Reason, &Detail));

	// One occupant, one seat - across vehicles: the second seat of ANOTHER
	// vehicle is refused with the occupant-side reason.
	Reason = EVehicleSeatRejectReason::None;
	TestFalse(TEXT("the same occupant is refused on a second vehicle's seat"),
		Model.TryEnterSeat(MakeRequest(VehicleTwoId, SeatTwoId, OccupantA), Reason, &Detail));
	TestEqual(TEXT("the second-seat refusal names OccupantAlreadySeated"), Reason, EVehicleSeatRejectReason::OccupantAlreadySeated);
	TestEqual(TEXT("the refusal created no second occupation"), Model.GetNumOccupiedSeats(), 1);

	// The held seat is untouched and precisely reported.
	FVehicleSeatKey SeatKey;
	TestTrue(TEXT("the occupant's seat is still findable"), Model.FindOccupantSeat(OccupantA, SeatKey));
	TestTrue(TEXT("the held seat is still the first vehicle's"), SeatKey == FVehicleSeatKey{FName(VehicleOneId), FName(SeatOneId)});

	// The refused seat stays available for a different occupant.
	Reason = EVehicleSeatRejectReason::None;
	TestTrue(TEXT("another occupant takes the second vehicle's seat"),
		Model.TryEnterSeat(MakeRequest(VehicleTwoId, SeatTwoId, OccupantB), Reason, &Detail));
	TestEqual(TEXT("two vehicles, two occupants, two seats"), Model.GetNumOccupiedSeats(), 2);

	// Deterministic reason priority: A is refused on the now-taken second
	// seat with the OCCUPANT-side reason (the model's own occupant state is
	// checked before the seat-side conflict).
	Reason = EVehicleSeatRejectReason::None;
	TestFalse(TEXT("a seated occupant stays refused with its own reason"),
		Model.TryEnterSeat(MakeRequest(VehicleTwoId, SeatTwoId, OccupantA), Reason, &Detail));
	TestEqual(TEXT("the occupant-side reason outranks the seat conflict"), Reason, EVehicleSeatRejectReason::OccupantAlreadySeated);
	return true;
}

// ---------------------------------------------------------------------------
// ReentryIdempotentNoSideEffects - acceptance: 幂等重入同座无副作用
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_040ReentryIdempotentNoSideEffects,
	"UEMMO.Tasks.M5_040.ReentryIdempotentNoSideEffects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_040ReentryIdempotentNoSideEffects::RunTest(const FString& Parameters)
{
	FSeatOccupancyModel Model;
	if (!RegisterSampleSeats(*this, Model, TEXT("setup")))
	{
		return false;
	}

	const FGuid OccupantA = MakeOccupant(1);
	const FGuid OccupantB = MakeOccupant(2);
	EVehicleSeatRejectReason Reason = EVehicleSeatRejectReason::None;
	FString Detail;
	TestTrue(TEXT("occupant A takes the seat"),
		Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantA), Reason, &Detail));

	// A storm of identical re-entries: every one is refused by name and
	// every one changes nothing - no second grant, no counter drift, no
	// registration churn.
	for (int32 Attempt = 0; Attempt < 3; ++Attempt)
	{
		Reason = EVehicleSeatRejectReason::None;
		TestFalse(FString::Printf(TEXT("re-entry attempt %d is refused"), Attempt + 1),
			Model.TryEnterSeat(MakeRequest(VehicleOneId, SeatOneId, OccupantA), Reason, &Detail));
		TestEqual(TEXT("the re-entry names AlreadyInThisSeat"), Reason, EVehicleSeatRejectReason::AlreadyInThisSeat);
	}

	FGuid Seated = FGuid(0, 0, 0, 0);
	TestTrue(TEXT("the seat is still occupied"), Model.FindSeatOccupant(FName(VehicleOneId), FName(SeatOneId), Seated));
	TestTrue(TEXT("the occupant is still exactly A"), Seated == OccupantA);
	TestEqual(TEXT("still exactly one occupation"), Model.GetNumOccupiedSeats(), 1);
	TestEqual(TEXT("still exactly two registrations"), Model.GetNumRegisteredSeats(), 2);

	// The re-entry storm poisoned nothing: the other seat is still enterable
	// by a fresh occupant.
	Reason = EVehicleSeatRejectReason::None;
	TestTrue(TEXT("the other seat is unaffected by the re-entry storm"),
		Model.TryEnterSeat(MakeRequest(VehicleTwoId, SeatTwoId, OccupantB), Reason, &Detail));
	TestEqual(TEXT("two independent occupations"), Model.GetNumOccupiedSeats(), 2);
	return true;
}

// ---------------------------------------------------------------------------
// UnusableAndUnknownRefused - acceptance: 拒绝原因输出明确
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_040UnusableAndUnknownRefused,
	"UEMMO.Tasks.M5_040.UnusableAndUnknownRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_040UnusableAndUnknownRefused::RunTest(const FString& Parameters)
{
	FSeatOccupancyModel Model;
	if (!RegisterSampleSeats(*this, Model, TEXT("setup")))
	{
		return false;
	}

	const FGuid OccupantA = MakeOccupant(1);
	EVehicleSeatRejectReason Reason = EVehicleSeatRejectReason::None;
	FString Detail;

	// Unusable identities never reach the maps (half-empty request, HitLedger
	// precedent): a NAME_None vehicle, a NAME_None seat and a zero GUID are
	// each refused with the same identity reason and a readable detail.
	FVehicleSeatEnterRequest NoVehicle = MakeRequest(VehicleOneId, SeatOneId, OccupantA);
	NoVehicle.VehicleId = NAME_None;
	Reason = EVehicleSeatRejectReason::None;
	TestFalse(TEXT("a NAME_None vehicle is refused"), Model.TryEnterSeat(NoVehicle, Reason, &Detail));
	TestEqual(TEXT("the NAME_None vehicle names UnusableIds"), Reason, EVehicleSeatRejectReason::UnusableIds);
	TestTrue(TEXT("the identity refusal detail is human-readable"), !Detail.IsEmpty());

	FVehicleSeatEnterRequest NoSeat = MakeRequest(VehicleOneId, SeatOneId, OccupantA);
	NoSeat.SeatId = NAME_None;
	Reason = EVehicleSeatRejectReason::None;
	TestFalse(TEXT("a NAME_None seat is refused"), Model.TryEnterSeat(NoSeat, Reason, &Detail));
	TestEqual(TEXT("the NAME_None seat names UnusableIds"), Reason, EVehicleSeatRejectReason::UnusableIds);

	FVehicleSeatEnterRequest NoOccupant = MakeRequest(VehicleOneId, SeatOneId, FGuid(0, 0, 0, 0));
	Reason = EVehicleSeatRejectReason::None;
	TestFalse(TEXT("a zero GUID occupant is refused"), Model.TryEnterSeat(NoOccupant, Reason, &Detail));
	TestEqual(TEXT("the zero GUID names UnusableIds"), Reason, EVehicleSeatRejectReason::UnusableIds);

	// Structural refusals: a typo'd seat and a typo'd vehicle are refused as
	// unknown seats - no implicit seat is created by an enter.
	Reason = EVehicleSeatRejectReason::None;
	TestFalse(TEXT("an unregistered seat of a known vehicle is refused"),
		Model.TryEnterSeat(MakeRequest(VehicleOneId, TEXT("seat_unknown"), OccupantA), Reason, &Detail));
	TestEqual(TEXT("the unknown seat names UnknownSeat"), Reason, EVehicleSeatRejectReason::UnknownSeat);
	Reason = EVehicleSeatRejectReason::None;
	TestFalse(TEXT("a seat of an unregistered vehicle is refused"),
		Model.TryEnterSeat(MakeRequest(TEXT("vehicle_unknown"), SeatOneId, OccupantA), Reason, &Detail));
	TestEqual(TEXT("the unknown vehicle's seat names UnknownSeat"), Reason, EVehicleSeatRejectReason::UnknownSeat);
	TestEqual(TEXT("no implicit seat was registered"), Model.GetNumRegisteredSeats(), 2);

	// Registration hygiene: a duplicate pair and unusable ids are refused.
	Reason = EVehicleSeatRejectReason::None;
	TestFalse(TEXT("a duplicate seat registration is refused"), Model.RegisterSeat(FName(VehicleOneId), FName(SeatOneId), Reason));
	TestEqual(TEXT("the duplicate registration names DuplicateSeat"), Reason, EVehicleSeatRejectReason::DuplicateSeat);
	Reason = EVehicleSeatRejectReason::None;
	TestFalse(TEXT("a NAME_None registration is refused"), Model.RegisterSeat(NAME_None, FName(TEXT("seat_x")), Reason));
	TestEqual(TEXT("the unusable registration names UnusableIds"), Reason, EVehicleSeatRejectReason::UnusableIds);

	// Every refusal above left the model exactly as it started.
	TestEqual(TEXT("no occupation was recorded anywhere"), Model.GetNumOccupiedSeats(), 0);
	FVehicleSeatKey SeatKey;
	TestFalse(TEXT("the probed occupant stayed seatless"), Model.FindOccupantSeat(OccupantA, SeatKey));
	return true;
}

#endif
