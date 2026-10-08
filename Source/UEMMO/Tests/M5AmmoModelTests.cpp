// M5-021: the magazine, shared reserve and reload model (M5 interface
// contract section 1, owner 021; section 7 "存档与实例"). Pins
// Weapons/AmmoModel.h:
//
// - Per-instance magazines: each item instance owns its own magazine slot;
//   the initial load is explicit and a duplicate initialization is refused
//   without granting rounds.
// - Shared reserves: rounds live in per-ammo-id pools shared across all
//   instances of that kind; two kinds never touch each other's pool.
// - Reload is a two-phase transaction: BeginReload only opens the window
//   (caller-supplied clock, never a World timer) and moves nothing; the
//   rounds move exactly once, atomically, on CompleteReload with
//   granted = min(capacity - loaded, reserve); CancelReload closes the window
//   and moves nothing.
// - Explicit refusals: unknown ammo, negative/beyond-capacity rounds, unknown
//   instances, a full magazine, an empty reserve, duplicate registration /
//   initialization / reload and stray completion/cancel all refuse with a
//   named error and leave the model unchanged.
//
// Pure logic only: no World, no UE assets, no wall clocks.

#include "Misc/AutomationTest.h"

#include <limits>

#include "../Weapons/AmmoModel.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_021
{
	/** A stable non-zero instance identity for one test step. */
	FGuid MakeInstanceId(uint32 Seed)
	{
		return FGuid(Seed, Seed + 1, Seed + 2, Seed + 3);
	}

	/**
	 * Registers the two isolated pools of the acceptance scenarios: ammo_small
	 * (5 rounds, the drained pool) and ammo_large (20 rounds, the remainder
	 * pool). Returns false after failing the test when a registration refuses.
	 */
	bool RegisterSamplePools(FAutomationTestBase& Automation, FAmmoModel& Model, const FString& ContextText)
	{
		FString Error;
		const bool bSmall = Model.RegisterAmmoType(TEXT("ammo_small"), 5, 5, &Error);
		Automation.TestTrue(ContextText + TEXT(": ammo_small registers"), bSmall);
		const bool bLarge = Model.RegisterAmmoType(TEXT("ammo_large"), 999, 20, &Error);
		Automation.TestTrue(ContextText + TEXT(": ammo_large registers"), bLarge);
		return bSmall && bLarge;
	}
}

using namespace UE::UEMMO::Tasks::M5_021;

/**
 * Acceptance: reserve pools register once with their caps, magazines
 * initialize once with an explicit load, and every invalid input (duplicate
 * registration/initialization, bad id text, negative or over-limit rounds,
 * unknown ammo, zero instance id, capacity outside 1..999) is refused with
 * the model left unchanged.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_021RegisterAndInitialize,
	"UEMMO.Tasks.M5_021.RegisterAndInitialize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_021RegisterAndInitialize::RunTest(const FString& Parameters)
{
	FAmmoModel Model;
	FString Error;

	// Legal registration lands the initial stock.
	TestTrue(TEXT("ammo_small registers"), Model.RegisterAmmoType(TEXT("ammo_small"), 5, 5, &Error));
	TestEqual(TEXT("ammo_small starts with 5 rounds"), Model.GetReserveRounds(TEXT("ammo_small")), 5);

	// Duplicate registration refuses and keeps the original pool untouched.
	TestFalse(TEXT("duplicate registration refuses"), Model.RegisterAmmoType(TEXT("ammo_small"), 999, 999, &Error));
	TestEqual(TEXT("duplicate registration leaves the pool"), Model.GetReserveRounds(TEXT("ammo_small")), 5);

	// Id text rule: uppercase and empty ids never register.
	TestFalse(TEXT("uppercase id refuses"), Model.RegisterAmmoType(TEXT("AMMO_BAD"), 5, 5, &Error));
	TestFalse(TEXT("empty id refuses"), Model.RegisterAmmoType(TEXT(""), 5, 5, &Error));

	// Reserve bounds: negative caps, over-limit caps and out-of-range stocks.
	TestFalse(TEXT("negative MaxReserve refuses"), Model.RegisterAmmoType(TEXT("ammo_neg"), -1, 0, &Error));
	TestFalse(TEXT("over-limit MaxReserve refuses"), Model.RegisterAmmoType(TEXT("ammo_over"), MaxAmmoReserve + 1, 0, &Error));
	TestFalse(TEXT("negative initial stock refuses"), Model.RegisterAmmoType(TEXT("ammo_neg_init"), 10, -1, &Error));
	TestFalse(TEXT("initial stock beyond MaxReserve refuses"), Model.RegisterAmmoType(TEXT("ammo_over_init"), 10, 11, &Error));
	TestEqual(TEXT("refused registrations leave no pool"), Model.GetReserveRounds(TEXT("ammo_neg")), -1);

	// Legal initialization with the explicit load.
	const FGuid InstanceId = MakeInstanceId(1);
	TestTrue(TEXT("instance initializes 12/3"), Model.InitializeInstance(InstanceId, TEXT("ammo_small"), 12, 3, &Error));
	const FMagazineState* Magazine = Model.FindMagazine(InstanceId);
	TestNotNull(TEXT("FindMagazine returns the slot"), Magazine);
	if (Magazine)
	{
		TestEqual(TEXT("magazine capacity"), Magazine->Capacity, 12);
		TestEqual(TEXT("magazine loaded rounds"), Magazine->LoadedRounds, 3);
		TestTrue(TEXT("magazine ammo kind"), Magazine->AmmoId == TEXT("ammo_small"));
	}

	// Duplicate initialization refuses, even with different arguments.
	TestFalse(TEXT("duplicate initialization refuses"), Model.InitializeInstance(InstanceId, TEXT("ammo_small"), 12, 3, &Error));
	TestFalse(TEXT("re-initialization with other arguments refuses"), Model.InitializeInstance(InstanceId, TEXT("ammo_large"), 12, 12, &Error));

	// Load bounds: negative and beyond-capacity initial loads refuse.
	const FGuid NegativeId = MakeInstanceId(2);
	TestFalse(TEXT("negative initial load refuses"), Model.InitializeInstance(NegativeId, TEXT("ammo_small"), 12, -1, &Error));
	const FGuid OverId = MakeInstanceId(3);
	TestFalse(TEXT("beyond-capacity load refuses"), Model.InitializeInstance(OverId, TEXT("ammo_small"), 12, 13, &Error));

	// Cross references: the ammo kind must be registered; the zero guid is unusable.
	const FGuid UnknownAmmoId = MakeInstanceId(4);
	TestFalse(TEXT("unknown ammo kind refuses"), Model.InitializeInstance(UnknownAmmoId, TEXT("ammo_unknown"), 12, 3, &Error));
	TestFalse(TEXT("all-zero instance id refuses"), Model.InitializeInstance(FGuid(), TEXT("ammo_small"), 12, 3, &Error));

	// Capacity bounds: a magazine that cannot hold a round and over-limit caps.
	const FGuid ZeroCapId = MakeInstanceId(5);
	TestFalse(TEXT("zero capacity refuses"), Model.InitializeInstance(ZeroCapId, TEXT("ammo_small"), 0, 0, &Error));
	const FGuid BigCapId = MakeInstanceId(6);
	TestFalse(TEXT("over-limit capacity refuses"), Model.InitializeInstance(BigCapId, TEXT("ammo_small"), MaxAmmoMagazineSize + 1, 0, &Error));

	// Refused initializations leave no slot behind.
	TestNull(TEXT("refused initialization leaves no slot"), Model.FindMagazine(NegativeId));
	TestEqual(TEXT("only the legal instance exists"), Model.NumMagazines(), 1);
	return true;
}

/**
 * Acceptance: 12-capacity loaded 3 over a 5-round reserve reloads to 8/0;
 * the same load over a 20-round reserve caps at the magazine (12) and leaves
 * 11 in the pool. The two pools never touch each other.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_021ReloadTransfersExactAmount,
	"UEMMO.Tasks.M5_021.ReloadTransfersExactAmount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_021ReloadTransfersExactAmount::RunTest(const FString& Parameters)
{
	FAmmoModel Model;
	if (!RegisterSamplePools(*this, Model, TEXT("setup")))
	{
		return false;
	}
	FString Error;

	// Small pool: the reserve drains completely into the magazine.
	const FGuid SmallId = MakeInstanceId(1);
	TestTrue(TEXT("small instance initializes"), Model.InitializeInstance(SmallId, TEXT("ammo_small"), 12, 3, &Error));
	FAmmoReloadOutcome Outcome = Model.BeginReload(SmallId, 0.0, 1.0);
	TestTrue(TEXT("BeginReload accepts a partial magazine with reserve"), Outcome.bSuccess);
	Outcome = Model.CompleteReload(SmallId);
	TestTrue(TEXT("CompleteReload succeeds"), Outcome.bSuccess);
	TestEqual(TEXT("granted equals the reserve"), Outcome.RoundsTransferred, 5);
	TestEqual(TEXT("loaded after reload"), Outcome.LoadedRounds, 8);
	TestEqual(TEXT("reserve after reload"), Outcome.ReserveRounds, 0);
	TestEqual(TEXT("pool drained"), Model.GetReserveRounds(TEXT("ammo_small")), 0);

	// Large pool: the magazine caps the transfer, the pool keeps the remainder.
	const FGuid LargeId = MakeInstanceId(2);
	TestTrue(TEXT("large instance initializes"), Model.InitializeInstance(LargeId, TEXT("ammo_large"), 12, 3, &Error));
	Outcome = Model.BeginReload(LargeId, 0.0, 1.0);
	TestTrue(TEXT("BeginReload accepts the second instance"), Outcome.bSuccess);
	Outcome = Model.CompleteReload(LargeId);
	TestTrue(TEXT("second CompleteReload succeeds"), Outcome.bSuccess);
	TestEqual(TEXT("granted capped by the magazine"), Outcome.RoundsTransferred, 9);
	TestEqual(TEXT("loaded capped at capacity"), Outcome.LoadedRounds, 12);
	TestEqual(TEXT("reserve keeps the remainder"), Outcome.ReserveRounds, 11);

	// Pool isolation: draining ammo_small never touched ammo_large.
	TestEqual(TEXT("pools stay isolated"), Model.GetReserveRounds(TEXT("ammo_large")), 11);
	return true;
}

/**
 * Acceptance: BeginReload opens the window and moves nothing; the rounds move
 * only when the completion callback reports the deadline.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_021ReloadTransfersOnlyAtCompletion,
	"UEMMO.Tasks.M5_021.ReloadTransfersOnlyAtCompletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_021ReloadTransfersOnlyAtCompletion::RunTest(const FString& Parameters)
{
	FAmmoModel Model;
	if (!RegisterSamplePools(*this, Model, TEXT("setup")))
	{
		return false;
	}
	FString Error;

	const FGuid InstanceId = MakeInstanceId(1);
	TestTrue(TEXT("instance initializes"), Model.InitializeInstance(InstanceId, TEXT("ammo_small"), 12, 3, &Error));

	const FAmmoReloadOutcome Begin = Model.BeginReload(InstanceId, 10.0, 2.0);
	TestTrue(TEXT("BeginReload succeeds"), Begin.bSuccess);
	TestEqual(TEXT("BeginReload transfers nothing"), Begin.RoundsTransferred, 0);

	// The window is open but the accounts are untouched.
	const FMagazineState* Magazine = Model.FindMagazine(InstanceId);
	TestNotNull(TEXT("magazine still present"), Magazine);
	if (Magazine)
	{
		TestEqual(TEXT("loaded untouched mid-reload"), Magazine->LoadedRounds, 3);
	}
	TestEqual(TEXT("reserve untouched mid-reload"), Model.GetReserveRounds(TEXT("ammo_small")), 5);
	TestTrue(TEXT("window is open"), Model.IsReloading(InstanceId));

	// Completion performs the one atomic transfer.
	const FAmmoReloadOutcome End = Model.CompleteReload(InstanceId);
	TestTrue(TEXT("CompleteReload succeeds"), End.bSuccess);
	TestEqual(TEXT("completion grants the reserve"), End.RoundsTransferred, 5);
	TestEqual(TEXT("loaded after completion"), End.LoadedRounds, 8);
	TestEqual(TEXT("reserve after completion"), End.ReserveRounds, 0);
	return true;
}

/**
 * Acceptance: a full magazine and an empty reserve refuse the reload with
 * their named causes, duplicate windows refuse, invalid window inputs refuse,
 * and every refusal leaves the model unchanged.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_021ReloadRefusals,
	"UEMMO.Tasks.M5_021.ReloadRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_021ReloadRefusals::RunTest(const FString& Parameters)
{
	FAmmoModel Model;
	if (!RegisterSamplePools(*this, Model, TEXT("setup")))
	{
		return false;
	}
	FString Error;

	// An ammo kind with a permanently empty pool.
	TestTrue(TEXT("ammo_empty registers"), Model.RegisterAmmoType(TEXT("ammo_empty"), 100, 0, &Error));

	// Unknown instance.
	FAmmoReloadOutcome Outcome = Model.BeginReload(MakeInstanceId(99), 0.0, 1.0);
	TestFalse(TEXT("unknown instance refuses"), Outcome.bSuccess);
	TestEqual(TEXT("unknown instance cause"), Outcome.Error, EAmmoModelError::UnknownInstance);

	// Full magazine: nothing to refill.
	const FGuid FullId = MakeInstanceId(1);
	TestTrue(TEXT("full instance initializes"), Model.InitializeInstance(FullId, TEXT("ammo_small"), 12, 12, &Error));
	Outcome = Model.BeginReload(FullId, 0.0, 1.0);
	TestFalse(TEXT("full magazine refuses"), Outcome.bSuccess);
	TestEqual(TEXT("full magazine cause"), Outcome.Error, EAmmoModelError::MagazineFull);
	TestFalse(TEXT("no window opened for the full magazine"), Model.IsReloading(FullId));

	// Empty reserve: the pool cannot grant a single round.
	const FGuid EmptyId = MakeInstanceId(2);
	TestTrue(TEXT("empty-pool instance initializes"), Model.InitializeInstance(EmptyId, TEXT("ammo_empty"), 12, 3, &Error));
	Outcome = Model.BeginReload(EmptyId, 0.0, 1.0);
	TestFalse(TEXT("empty reserve refuses"), Outcome.bSuccess);
	TestEqual(TEXT("empty reserve cause"), Outcome.Error, EAmmoModelError::NoReserveAmmo);
	TestFalse(TEXT("no window opened for the empty pool"), Model.IsReloading(EmptyId));

	// Duplicate window: the second BeginReload refuses while the first is open.
	const FGuid BusyId = MakeInstanceId(3);
	TestTrue(TEXT("busy instance initializes"), Model.InitializeInstance(BusyId, TEXT("ammo_large"), 12, 3, &Error));
	TestTrue(TEXT("first BeginReload succeeds"), Model.BeginReload(BusyId, 0.0, 1.0).bSuccess);
	Outcome = Model.BeginReload(BusyId, 5.0, 1.0);
	TestFalse(TEXT("duplicate window refuses"), Outcome.bSuccess);
	TestEqual(TEXT("duplicate window cause"), Outcome.Error, EAmmoModelError::DuplicateReload);

	// Invalid window inputs on a fresh instance: negative and non-finite durations.
	const FGuid TimeId = MakeInstanceId(4);
	TestTrue(TEXT("time instance initializes"), Model.InitializeInstance(TimeId, TEXT("ammo_large"), 12, 3, &Error));
	Outcome = Model.BeginReload(TimeId, 0.0, -1.0);
	TestFalse(TEXT("negative duration refuses"), Outcome.bSuccess);
	TestEqual(TEXT("negative duration cause"), Outcome.Error, EAmmoModelError::InvalidDuration);
	Outcome = Model.BeginReload(TimeId, std::numeric_limits<double>::quiet_NaN(), 1.0);
	TestFalse(TEXT("non-finite clock refuses"), Outcome.bSuccess);
	TestEqual(TEXT("non-finite clock cause"), Outcome.Error, EAmmoModelError::InvalidDuration);
	TestFalse(TEXT("invalid inputs opened no window"), Model.IsReloading(TimeId));

	// Every refusal left the pools untouched.
	TestEqual(TEXT("ammo_large untouched"), Model.GetReserveRounds(TEXT("ammo_large")), 20);
	TestEqual(TEXT("ammo_empty untouched"), Model.GetReserveRounds(TEXT("ammo_empty")), 0);
	return true;
}

/**
 * Acceptance: cancelling the window (equipment switch / death path) closes it
 * and moves nothing; loaded and reserve stay exactly as they were and a fresh
 * window may open afterwards.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_021CancelReloadLeavesAmmoUntouched,
	"UEMMO.Tasks.M5_021.CancelReloadLeavesAmmoUntouched",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_021CancelReloadLeavesAmmoUntouched::RunTest(const FString& Parameters)
{
	FAmmoModel Model;
	if (!RegisterSamplePools(*this, Model, TEXT("setup")))
	{
		return false;
	}
	FString Error;

	const FGuid InstanceId = MakeInstanceId(1);
	TestTrue(TEXT("instance initializes"), Model.InitializeInstance(InstanceId, TEXT("ammo_small"), 12, 3, &Error));
	TestTrue(TEXT("BeginReload succeeds"), Model.BeginReload(InstanceId, 5.0, 10.0).bSuccess);

	TestTrue(TEXT("CancelReload succeeds"), Model.CancelReload(InstanceId, &Error));

	// The accounts are exactly as before the window opened.
	const FMagazineState* Magazine = Model.FindMagazine(InstanceId);
	TestNotNull(TEXT("magazine still present"), Magazine);
	if (Magazine)
	{
		TestEqual(TEXT("loaded unchanged by the cancel"), Magazine->LoadedRounds, 3);
	}
	TestEqual(TEXT("reserve unchanged by the cancel"), Model.GetReserveRounds(TEXT("ammo_small")), 5);
	TestFalse(TEXT("window closed"), Model.IsReloading(InstanceId));
	TestEqual(TEXT("no remaining time after the cancel"), Model.GetRemainingReloadSeconds(InstanceId, 7.0), 0.0);

	// A fresh window may open after the cancellation.
	const FAmmoReloadOutcome Retry = Model.BeginReload(InstanceId, 5.0, 10.0);
	TestTrue(TEXT("a fresh window opens after the cancel"), Retry.bSuccess);
	TestTrue(TEXT("cancelling the fresh window succeeds"), Model.CancelReload(InstanceId, &Error));

	// Stray cancellations refuse: no window, unknown instance.
	const FGuid IdleId = MakeInstanceId(2);
	TestTrue(TEXT("idle instance initializes"), Model.InitializeInstance(IdleId, TEXT("ammo_small"), 12, 3, &Error));
	TestFalse(TEXT("cancel without a window refuses"), Model.CancelReload(IdleId, &Error));
	TestFalse(TEXT("cancel of an unknown instance refuses"), Model.CancelReload(MakeInstanceId(42), &Error));
	return true;
}

/**
 * Acceptance: the completion callback moves rounds exactly once - a repeated
 * completion is a named refusal and grants nothing, and a repeated
 * initialization never tops the magazine up.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_021DoubleCompletionGrantsNothing,
	"UEMMO.Tasks.M5_021.DoubleCompletionGrantsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_021DoubleCompletionGrantsNothing::RunTest(const FString& Parameters)
{
	FAmmoModel Model;
	if (!RegisterSamplePools(*this, Model, TEXT("setup")))
	{
		return false;
	}
	FString Error;

	const FGuid InstanceId = MakeInstanceId(1);
	TestTrue(TEXT("instance initializes"), Model.InitializeInstance(InstanceId, TEXT("ammo_small"), 12, 3, &Error));
	TestTrue(TEXT("BeginReload succeeds"), Model.BeginReload(InstanceId, 0.0, 1.0).bSuccess);

	const FAmmoReloadOutcome First = Model.CompleteReload(InstanceId);
	TestTrue(TEXT("first completion succeeds"), First.bSuccess);
	TestEqual(TEXT("first completion grants"), First.RoundsTransferred, 5);

	const FAmmoReloadOutcome Second = Model.CompleteReload(InstanceId);
	TestFalse(TEXT("second completion refuses"), Second.bSuccess);
	TestEqual(TEXT("second completion cause"), Second.Error, EAmmoModelError::NoActiveReload);
	TestEqual(TEXT("second completion grants nothing"), Second.RoundsTransferred, 0);

	// The accounts stayed at the first completion's result.
	const FMagazineState* Magazine = Model.FindMagazine(InstanceId);
	TestNotNull(TEXT("magazine still present"), Magazine);
	if (Magazine)
	{
		TestEqual(TEXT("loaded unchanged by the second completion"), Magazine->LoadedRounds, 8);
	}
	TestEqual(TEXT("reserve unchanged by the second completion"), Model.GetReserveRounds(TEXT("ammo_small")), 0);

	// Re-initialization is refused and never tops the magazine up.
	TestFalse(TEXT("duplicate initialization refuses"), Model.InitializeInstance(InstanceId, TEXT("ammo_small"), 12, 0, &Error));
	Magazine = Model.FindMagazine(InstanceId);
	if (Magazine)
	{
		TestEqual(TEXT("loaded unchanged by the refused re-initialization"), Magazine->LoadedRounds, 8);
	}
	return true;
}

/**
 * Acceptance: the remaining reload time is a pure query over the open window
 * (caller clock in, remaining seconds out, clamped at the deadline) and reads
 * 0.0 without an open window or for an unknown instance.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_021RemainingReloadTime,
	"UEMMO.Tasks.M5_021.RemainingReloadTime",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_021RemainingReloadTime::RunTest(const FString& Parameters)
{
	FAmmoModel Model;
	if (!RegisterSamplePools(*this, Model, TEXT("setup")))
	{
		return false;
	}
	FString Error;

	const FGuid InstanceId = MakeInstanceId(1);
	TestTrue(TEXT("instance initializes"), Model.InitializeInstance(InstanceId, TEXT("ammo_small"), 12, 3, &Error));
	TestEqual(TEXT("no window reads zero"), Model.GetRemainingReloadSeconds(InstanceId, 100.0), 0.0);

	TestTrue(TEXT("BeginReload succeeds"), Model.BeginReload(InstanceId, 100.0, 2.5).bSuccess);
	TestEqual(TEXT("remaining at the window start"), Model.GetRemainingReloadSeconds(InstanceId, 100.0), 2.5);
	TestEqual(TEXT("remaining one second in"), Model.GetRemainingReloadSeconds(InstanceId, 101.0), 1.5);
	TestEqual(TEXT("remaining at the deadline"), Model.GetRemainingReloadSeconds(InstanceId, 102.5), 0.0);
	TestEqual(TEXT("remaining past the deadline stays clamped"), Model.GetRemainingReloadSeconds(InstanceId, 103.0), 0.0);

	// The window stays open past the deadline: the completion callback remains
	// the caller's duty, the query never completes anything.
	TestTrue(TEXT("the window is still open at the deadline"), Model.IsReloading(InstanceId));

	// Unknown instances read the same zero as instances without a window.
	const FGuid UnknownId = MakeInstanceId(77);
	TestEqual(TEXT("unknown instance reads zero"), Model.GetRemainingReloadSeconds(UnknownId, 1.0), 0.0);
	TestFalse(TEXT("unknown instance is not reloading"), Model.IsReloading(UnknownId));
	return true;
}

/**
 * Acceptance: Reset drops every magazine, window and pool in one step - the
 * session-teardown state is completely empty afterwards.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_021ResetClearsSessionState,
	"UEMMO.Tasks.M5_021.ResetClearsSessionState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_021ResetClearsSessionState::RunTest(const FString& Parameters)
{
	FAmmoModel Model;
	if (!RegisterSamplePools(*this, Model, TEXT("setup")))
	{
		return false;
	}
	FString Error;

	const FGuid InstanceId = MakeInstanceId(1);
	TestTrue(TEXT("instance initializes"), Model.InitializeInstance(InstanceId, TEXT("ammo_small"), 12, 3, &Error));
	TestTrue(TEXT("BeginReload succeeds"), Model.BeginReload(InstanceId, 0.0, 1.0).bSuccess);
	TestEqual(TEXT("pre-reset state exists"), Model.NumMagazines(), 1);

	Model.Reset();

	TestEqual(TEXT("no magazines after reset"), Model.NumMagazines(), 0);
	TestEqual(TEXT("no pools after reset"), Model.NumAmmoTypes(), 0);
	TestNull(TEXT("no slot after reset"), Model.FindMagazine(InstanceId));
	TestEqual(TEXT("no reserve after reset"), Model.GetReserveRounds(TEXT("ammo_small")), -1);
	TestFalse(TEXT("no window after reset"), Model.IsReloading(InstanceId));
	return true;
}

#endif

