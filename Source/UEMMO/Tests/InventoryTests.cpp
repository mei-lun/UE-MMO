// M3-002: 30-slot inventory model (interface contract section 8). Pins the
// empty-inventory behavior, the 30/31 capacity boundary, duplicate InstanceId
// rejection, remove-then-re-add identity consistency and the untouched
// container after failed mutations. Pure checks: this file only builds plain
// structs and calls plain functions; it never touches UE assets, worlds or
// wall clocks.
//
// Stub-failure note: the empty-inventory test asserts behavior the red stub
// already satisfies (nothing stored, remove always NotFound), so the red run
// fails the other five tests while this one passes.

#include "Misc/AutomationTest.h"

#include "../Items/InventoryModel.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_002
{
	/**
	 * Builds a valid instance with a deterministic FGuid derived from Seed, so
	 * distinct seeds yield distinct identities and every assertion about order
	 * and content is reproducible (no FGuid::NewGuid randomness).
	 */
	static FItemInstance MakeM3_002TestInstance(uint32 Seed, const TCHAR* DefinitionName)
	{
		FItemInstance Instance;
		Instance.InstanceId = FGuid(Seed, 0xC0FFEEu, Seed + 7u, Seed * 3u + 1u);
		Instance.DefinitionId = FName(DefinitionName);
		Instance.RollSeed = static_cast<int64>(Seed);
		Instance.RolledStats.Attack = static_cast<float>(Seed);
		Instance.Level = 1;
		return Instance;
	}

	/**
	 * Setup helper: adds one instance and reports a concrete error naming the
	 * entry when TryAdd does not return Added. Tests bail out (early return)
	 * when the fixtures cannot be created, so they never dereference slots
	 * that were never filled.
	 */
	static bool AddM3_002OrReport(FAutomationTestBase& Test, FInventoryModel& Model,
		const FItemInstance& Instance, const TCHAR* What)
	{
		const EInventoryAddResult Result = Model.TryAdd(Instance);
		if (Result != EInventoryAddResult::Added)
		{
			Test.AddError(FString::Printf(TEXT("setup: TryAdd(%s) returned result code %d instead of Added"),
				What, static_cast<int32>(Result)));
			return false;
		}
		return true;
	}
}

using namespace UE::UEMMO::Tasks::M3_002;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_002EmptyInventoryReportsZeroAndRemoveNotFound,
	"UEMMO.Tasks.M3_002.EmptyInventoryReportsZeroAndRemoveNotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_002EmptyInventoryReportsZeroAndRemoveNotFound::RunTest(const FString& Parameters)
{
	// A fresh model must be empty, and removing an arbitrary id from it is a
	// clean NotFound (no crash, no state change).
	FInventoryModel Model;
	TestEqual(TEXT("a fresh inventory holds 0 items"), Model.Count(), 0);
	TestTrue(TEXT("GetAll of a fresh inventory is empty"), Model.GetAll().Num() == 0);
	TestTrue(TEXT("a fresh inventory contains no instance"), !Model.Contains(FGuid(1u, 2u, 3u, 4u)));
	TestTrue(TEXT("GetByIndex on an empty inventory returns nullptr"), Model.GetByIndex(0) == nullptr);

	const FGuid MissingId(0x12345678u, 0xAABBCCDDu, 0x11223344u, 0x55667788u);
	TestTrue(TEXT("Remove of an arbitrary id on an empty inventory returns NotFound"),
		Model.Remove(MissingId) == EInventoryRemoveResult::NotFound);
	TestEqual(TEXT("the failed removal leaves the count at 0"), Model.Count(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_002ThirtiethAddedThirtyFirstFullKeepsCallerItem,
	"UEMMO.Tasks.M3_002.ThirtiethAddedThirtyFirstFullKeepsCallerItem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_002ThirtiethAddedThirtyFirstFullKeepsCallerItem::RunTest(const FString& Parameters)
{
	// Exactly 30 distinct instances fit; the 31st is rejected as InventoryFull
	// with the container untouched and the caller still owning its instance.
	FInventoryModel Model;
	for (int32 Index = 1; Index <= FInventoryModel::Capacity; ++Index)
	{
		const FItemInstance Instance = MakeM3_002TestInstance(
			static_cast<uint32>(Index), TEXT("weapon_training"));
		if (!AddM3_002OrReport(*this, Model, Instance, *FString::Printf(TEXT("item #%d"), Index)))
		{
			return true;
		}
	}
	TestEqual(TEXT("after 30 successful adds the inventory holds 30 items"), Model.Count(), 30);
	TestEqual(TEXT("the model's capacity constant is 30"), FInventoryModel::Capacity, 30);

	FItemInstance ThirtyFirst = MakeM3_002TestInstance(0x7Fu, TEXT("armor_training"));
	const FGuid ThirtyFirstId = ThirtyFirst.InstanceId;
	ThirtyFirst.Level = 4;
	ThirtyFirst.RolledStats.Attack = 12.0f;

	const EInventoryAddResult FullResult = Model.TryAdd(ThirtyFirst);
	TestTrue(TEXT("the 31st add returns InventoryFull"), FullResult == EInventoryAddResult::InventoryFull);
	TestEqual(TEXT("the rejected 31st add leaves the inventory at 30 items"), Model.Count(), 30);
	TestTrue(TEXT("the rejected instance is not stored"), !Model.Contains(ThirtyFirstId));
	TestTrue(TEXT("the caller keeps the rejected instance (InstanceId unchanged)"),
		ThirtyFirst.InstanceId == ThirtyFirstId);
	TestEqual(TEXT("the caller keeps the rejected instance (Level unchanged)"), ThirtyFirst.Level, 4);
	TestEqual(TEXT("the caller keeps the rejected instance (Attack unchanged)"), ThirtyFirst.RolledStats.Attack, 12.0f);
	TestTrue(TEXT("GetAll still reports 30 entries after the rejection"), Model.GetAll().Num() == 30);

	// Freeing one slot lets the very same caller instance in again.
	const FGuid FirstId = Model.GetByIndex(0)->InstanceId;
	TestTrue(TEXT("removing one stored item returns Removed"),
		Model.Remove(FirstId) == EInventoryRemoveResult::Removed);
	TestTrue(TEXT("after one removal the previously rejected instance is accepted"),
		Model.TryAdd(ThirtyFirst) == EInventoryAddResult::Added);
	TestEqual(TEXT("the inventory holds 30 items again"), Model.Count(), 30);
	TestTrue(TEXT("the re-added instance is now contained"), Model.Contains(ThirtyFirstId));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_002DuplicateInstanceIdKeepsFirstCopy,
	"UEMMO.Tasks.M3_002.DuplicateInstanceIdKeepsFirstCopy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_002DuplicateInstanceIdKeepsFirstCopy::RunTest(const FString& Parameters)
{
	// Adding the same InstanceId twice stores exactly one entry and keeps the
	// first copy's payload; the duplicate never overwrites it.
	FInventoryModel Model;
	FItemInstance First = MakeM3_002TestInstance(0x21u, TEXT("weapon_training"));
	First.RolledStats.Attack = 5.0f;
	if (!AddM3_002OrReport(*this, Model, First, TEXT("First")))
	{
		return true;
	}
	TestEqual(TEXT("the inventory holds 1 item"), Model.Count(), 1);

	FItemInstance SameIdCopy = First;
	SameIdCopy.RolledStats.Attack = 99.0f;
	TestTrue(TEXT("adding a second instance with the same InstanceId returns Duplicate"),
		Model.TryAdd(SameIdCopy) == EInventoryAddResult::Duplicate);
	TestEqual(TEXT("the duplicate did not change the inventory size"), Model.Count(), 1);

	const FItemInstance* Stored = Model.GetByIndex(0);
	TestTrue(TEXT("the stored entry is readable at index 0"), Stored != nullptr);
	if (Stored)
	{
		TestTrue(TEXT("the stored copy keeps the first version's InstanceId"),
			Stored->InstanceId == First.InstanceId);
		TestEqual(TEXT("the stored copy keeps the first version's Attack"), Stored->RolledStats.Attack, 5.0f);
	}
	TestEqual(TEXT("the caller's duplicate instance is untouched"), SameIdCopy.RolledStats.Attack, 99.0f);

	// Identity is InstanceId alone: another DefinitionId with the same id is
	// still a duplicate, not a second slot.
	FItemInstance Rebranded = First;
	Rebranded.DefinitionId = FName(TEXT("armor_training"));
	TestTrue(TEXT("the same InstanceId with another DefinitionId is still a Duplicate"),
		Model.TryAdd(Rebranded) == EInventoryAddResult::Duplicate);
	TestEqual(TEXT("the identity check leaves the inventory at 1 item"), Model.Count(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_002RemoveThenReaddKeepsIdentityAndOrder,
	"UEMMO.Tasks.M3_002.RemoveThenReaddKeepsIdentityAndOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_002RemoveThenReaddKeepsIdentityAndOrder::RunTest(const FString& Parameters)
{
	// Removing one item and adding ids again (the same id and a fresh id) must
	// leave no identity confusion: slots keep a stable, reproducible order and
	// every stored payload stays intact.
	FInventoryModel Model;
	const FItemInstance A = MakeM3_002TestInstance(0xA1u, TEXT("weapon_training"));
	const FItemInstance B = MakeM3_002TestInstance(0xB2u, TEXT("armor_training"));
	const FItemInstance C = MakeM3_002TestInstance(0xC3u, TEXT("charm_training"));
	if (!AddM3_002OrReport(*this, Model, A, TEXT("A")) ||
		!AddM3_002OrReport(*this, Model, B, TEXT("B")) ||
		!AddM3_002OrReport(*this, Model, C, TEXT("C")))
	{
		return true;
	}
	TestEqual(TEXT("three items are stored"), Model.Count(), 3);

	TestTrue(TEXT("removing the middle item B returns Removed"),
		Model.Remove(B.InstanceId) == EInventoryRemoveResult::Removed);
	TestEqual(TEXT("two items remain after the removal"), Model.Count(), 2);
	TestTrue(TEXT("B is no longer contained"), !Model.Contains(B.InstanceId));

	// Remaining order is stable: A then C, nothing reordered.
	TestTrue(TEXT("slot 0 still holds A after the removal"), Model.GetByIndex(0) != nullptr &&
		Model.GetByIndex(0)->InstanceId == A.InstanceId);
	TestTrue(TEXT("slot 1 holds C after the removal"), Model.GetByIndex(1) != nullptr &&
		Model.GetByIndex(1)->InstanceId == C.InstanceId);

	// The removed InstanceId can be added again: no stale identity state.
	TestTrue(TEXT("the removed InstanceId can be added again"),
		Model.TryAdd(B) == EInventoryAddResult::Added);
	TestEqual(TEXT("the inventory holds 3 items again"), Model.Count(), 3);
	TestTrue(TEXT("B is contained again"), Model.Contains(B.InstanceId));

	// Re-adds append: A, C keep their slots, B lands at the end.
	TestTrue(TEXT("slot 0 is still A after the re-add"), Model.GetByIndex(0) != nullptr &&
		Model.GetByIndex(0)->InstanceId == A.InstanceId);
	TestTrue(TEXT("slot 1 is still C after the re-add"), Model.GetByIndex(1) != nullptr &&
		Model.GetByIndex(1)->InstanceId == C.InstanceId);
	TestTrue(TEXT("slot 2 is the re-added B"), Model.GetByIndex(2) != nullptr &&
		Model.GetByIndex(2)->InstanceId == B.InstanceId);
	TestEqual(TEXT("the re-added B keeps its DefinitionId"),
		Model.GetByIndex(2)->DefinitionId, FName(TEXT("armor_training")));
	TestEqual(TEXT("the re-added B keeps its RollSeed"), Model.GetByIndex(2)->RollSeed,
		static_cast<int64>(0xB2u));

	// A fresh id after all that still gets its own slot; four distinct items.
	const FItemInstance D = MakeM3_002TestInstance(0xD4u, TEXT("weapon_training"));
	if (!AddM3_002OrReport(*this, Model, D, TEXT("D")))
	{
		return true;
	}
	TestEqual(TEXT("four items are stored"), Model.Count(), 4);
	TestTrue(TEXT("slot 3 is D"), Model.GetByIndex(3) != nullptr &&
		Model.GetByIndex(3)->InstanceId == D.InstanceId);
	TestTrue(TEXT("A, B, C, D carry four distinct instance ids"),
		A.InstanceId != B.InstanceId && B.InstanceId != C.InstanceId &&
		C.InstanceId != D.InstanceId && A.InstanceId != D.InstanceId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_002RemoveMissingKeepsOthersUntouched,
	"UEMMO.Tasks.M3_002.RemoveMissingKeepsOthersUntouched",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_002RemoveMissingKeepsOthersUntouched::RunTest(const FString& Parameters)
{
	// A failed removal must be a no-op: count, membership, order and payload
	// of the stored items all stay exactly as they were.
	FInventoryModel Model;
	const FItemInstance A = MakeM3_002TestInstance(0x51u, TEXT("weapon_training"));
	const FItemInstance B = MakeM3_002TestInstance(0x52u, TEXT("armor_training"));
	if (!AddM3_002OrReport(*this, Model, A, TEXT("A")) ||
		!AddM3_002OrReport(*this, Model, B, TEXT("B")))
	{
		return true;
	}

	const FGuid Stranger(0xDEADu, 0xBEEFu, 0xCAFEu, 0xF00Du);
	TestTrue(TEXT("removing an id that was never added returns NotFound"),
		Model.Remove(Stranger) == EInventoryRemoveResult::NotFound);
	TestEqual(TEXT("the count is unchanged after the failed removal"), Model.Count(), 2);
	TestTrue(TEXT("A survives a failed removal"), Model.Contains(A.InstanceId));
	TestTrue(TEXT("B survives a failed removal"), Model.Contains(B.InstanceId));
	TestTrue(TEXT("slot order is unchanged after the failed removal"),
		Model.GetByIndex(0) != nullptr && Model.GetByIndex(0)->InstanceId == A.InstanceId &&
		Model.GetByIndex(1) != nullptr && Model.GetByIndex(1)->InstanceId == B.InstanceId);

	// The all-zero guid is never a legal identity, so removing it is NotFound.
	const FItemInstance* StoredB = Model.GetByIndex(1);
	TestTrue(TEXT("removing the all-zero guid returns NotFound"),
		Model.Remove(FGuid()) == EInventoryRemoveResult::NotFound);
	TestEqual(TEXT("the count is still 2 after the zero-guid removal"), Model.Count(), 2);
	TestTrue(TEXT("B's payload is intact after both failed removals"), StoredB != nullptr &&
		StoredB->DefinitionId == FName(TEXT("armor_training")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_002AllZeroGuidRejectedAsInvalidInstance,
	"UEMMO.Tasks.M3_002.AllZeroGuidRejectedAsInvalidInstance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_002AllZeroGuidRejectedAsInvalidInstance::RunTest(const FString& Parameters)
{
	// The all-zero FGuid is never a legal business identity, so adding such an
	// instance must fail with InvalidInstance (not Duplicate/InventoryFull)
	// and store nothing; a following valid add must still work.
	FInventoryModel Model;
	FItemInstance Zero = MakeM3_002TestInstance(0x61u, TEXT("weapon_training"));
	Zero.InstanceId = FGuid();
	TestTrue(TEXT("an instance with the all-zero guid is rejected as InvalidInstance"),
		Model.TryAdd(Zero) == EInventoryAddResult::InvalidInstance);
	TestEqual(TEXT("the rejected instance was not stored"), Model.Count(), 0);
	TestTrue(TEXT("GetAll is still empty after the rejection"), Model.GetAll().Num() == 0);

	const FItemInstance Valid = MakeM3_002TestInstance(0x62u, TEXT("armor_training"));
	if (!AddM3_002OrReport(*this, Model, Valid, TEXT("Valid")))
	{
		return true;
	}
	TestEqual(TEXT("the inventory holds exactly the valid instance"), Model.Count(), 1);
	TestTrue(TEXT("the stored entry is the valid one"), Model.GetByIndex(0) != nullptr &&
		Model.GetByIndex(0)->InstanceId == Valid.InstanceId);
	return true;
}

#endif
