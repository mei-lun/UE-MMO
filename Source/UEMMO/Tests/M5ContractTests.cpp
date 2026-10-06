#include "Misc/AutomationTest.h"

#include "UObject/UnrealType.h"

#include "../Combat/System/CombatEventTypes.h"

#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS

// M5-002 acceptance tests: the frozen shared value types of the data-driven
// combat expansion. Pure logic - no World is created and no actor is spawned
// here; every assertion runs on plain structs, aliases and enums.
//
// Acceptance mapping:
// - EventKeyDedup      : different source or epoch with the same sequence never
//                        collides (equality + hash + ledger round trip).
// - InvalidIds         : invalid ids are recognizable (all-zero sentinels).
// - NoActorPointers    : the event key carries no World/Actor pointer member
//                        (compile-time static asserts + runtime mirrors).
// - HitDecisionCoverage: the decision enum covers exactly its four frozen values.
// - LocalTickIsNotAClock: FCombatTick stays a counter and is never a seconds clock.

// ---------------------------------------------------------------------------
// Compile-time section. These asserts fail the build the moment a contract
// drifts: an id alias changes width, the key grows a pointer/reference member,
// or the key stops being a trivially copyable value snapshot.
// ---------------------------------------------------------------------------

static_assert(std::is_same_v<FCombatEpoch, uint64>, "FCombatEpoch must stay a 64-bit epoch id");
static_assert(std::is_same_v<FEntityId, uint64>, "FEntityId must stay a 64-bit world entity id");
static_assert(std::is_same_v<FShotId, uint64>, "FShotId must stay a 64-bit per-epoch shot id");
static_assert(std::is_same_v<FPelletIndex, uint8>, "FPelletIndex must stay an 8-bit pellet index");
static_assert(std::is_same_v<FTargetId, uint64>, "FTargetId must stay a 64-bit target id");
static_assert(std::is_same_v<FCombatTick, uint32>, "FCombatTick must stay a 32-bit local tick counter");
static_assert(!std::is_floating_point_v<FCombatTick>, "FCombatTick is a counter, never a seconds clock");

static_assert(!std::is_pointer_v<decltype(FCombatEventKey::Epoch)>, "FCombatEventKey::Epoch must not be a pointer");
static_assert(!std::is_pointer_v<decltype(FCombatEventKey::EntityId)>, "FCombatEventKey::EntityId must not be a pointer");
static_assert(!std::is_pointer_v<decltype(FCombatEventKey::ShotId)>, "FCombatEventKey::ShotId must not be a pointer");
static_assert(!std::is_pointer_v<decltype(FCombatEventKey::PelletIndex)>, "FCombatEventKey::PelletIndex must not be a pointer");
static_assert(!std::is_pointer_v<decltype(FCombatEventKey::TargetId)>, "FCombatEventKey::TargetId must not be a pointer");
static_assert(std::is_trivially_copyable_v<FCombatEventKey>, "FCombatEventKey must stay a trivially copyable value snapshot");
static_assert(std::is_default_constructible_v<FCombatEventKey>, "FCombatEventKey must stay default constructible");

// ---------------------------------------------------------------------------
// EventKeyDedup
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_002EventKeyDedup,
	"UEMMO.Tasks.M5_002.EventKeyDedup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_002EventKeyDedup::RunTest(const FString& Parameters)
{
	// Baseline key: epoch 1, source entity 100, first shot, pellet 0, target 200.
	const FCombatEventKey Base{1, 100, 1, 0, 200};

	// Identical five-tuples compare equal and hash identically: the dedup
	// contract is "five equal fields mean the same hit".
	FCombatEventKey Copy = Base;
	TestTrue(TEXT("identical five-tuples compare equal"), Base == Copy);
	TestFalse(TEXT("identical five-tuples are never unequal"), Base != Copy);
	TestEqual(TEXT("identical five-tuples hash identically"), GetTypeHash(Base), GetTypeHash(Copy));

	// The same sequence from a different source entity must not collide:
	// two capabilities whose local sequences both equal 1 stay distinct keys.
	FCombatEventKey OtherSource = Base;
	OtherSource.EntityId = 101;
	TestTrue(TEXT("same sequence from a different source is a different key"), Base != OtherSource);
	TestTrue(TEXT("different source yields a different hash"), GetTypeHash(Base) != GetTypeHash(OtherSource));

	// The same sequence in a different epoch must not collide: a stale
	// generation's keys can never merge into the live generation.
	FCombatEventKey NextEpoch = Base;
	NextEpoch.Epoch = Base.Epoch + 1;
	TestTrue(TEXT("same sequence in a different epoch is a different key"), Base != NextEpoch);
	TestTrue(TEXT("different epoch yields a different hash"), GetTypeHash(Base) != GetTypeHash(NextEpoch));

	// Pellet and target split keys as well: one shot may damage one target
	// through several pellets, each with its own damage event.
	FCombatEventKey OtherPellet = Base;
	OtherPellet.PelletIndex = 1;
	TestTrue(TEXT("same shot through a different pellet is a different key"), Base != OtherPellet);
	FCombatEventKey OtherTarget = Base;
	OtherTarget.TargetId = 201;
	TestTrue(TEXT("same shot hitting a different target is a different key"), Base != OtherTarget);
	TestTrue(TEXT("different target yields a different hash"), GetTypeHash(Base) != GetTypeHash(OtherTarget));

	// Ledger round trip: a TSet keyed by the event struct finds an identical
	// key by value, refuses to grow on a duplicate, and does not contain
	// unrecorded keys.
	TSet<FCombatEventKey> Ledger;
	Ledger.Add(Base);
	TestTrue(TEXT("a recorded key is found again by value"), Ledger.Contains(Copy));
	const int32 CountBefore = Ledger.Num();
	Ledger.Add(Copy);
	TestEqual(TEXT("re-recording an identical key does not grow the ledger"), Ledger.Num(), CountBefore);
	TestFalse(TEXT("an unrecorded key (different source) is absent"), Ledger.Contains(OtherSource));
	TestFalse(TEXT("an unrecorded key (different epoch) is absent"), Ledger.Contains(NextEpoch));
	return true;
}

// ---------------------------------------------------------------------------
// InvalidIds
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_002InvalidIds,
	"UEMMO.Tasks.M5_002.InvalidIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_002InvalidIds::RunTest(const FString& Parameters)
{
	// 0 is the frozen invalid sentinel of every 64-bit combat id (sequences
	// start at 1), so an all-zero id is always recognizable.
	TestEqual(TEXT("InvalidCombatEpoch is 0"), InvalidCombatEpoch, static_cast<FCombatEpoch>(0));
	TestFalse(TEXT("epoch 0 is recognizable as invalid"), IsValidCombatEpoch(InvalidCombatEpoch));
	TestTrue(TEXT("epoch 1 is valid"), IsValidCombatEpoch(1));

	TestEqual(TEXT("InvalidCombatEntityId is 0"), InvalidCombatEntityId, static_cast<FEntityId>(0));
	TestFalse(TEXT("all-zero EntityId is recognizable as invalid"), IsValidCombatEntityId(InvalidCombatEntityId));
	TestTrue(TEXT("nonzero EntityId is valid"), IsValidCombatEntityId(1));

	TestEqual(TEXT("InvalidCombatShotId is 0"), InvalidCombatShotId, static_cast<FShotId>(0));
	TestFalse(TEXT("shot id 0 is recognizable as invalid"), IsValidCombatShotId(InvalidCombatShotId));
	TestTrue(TEXT("shot id 1 is valid"), IsValidCombatShotId(1));

	TestEqual(TEXT("InvalidCombatTargetId is 0"), InvalidCombatTargetId, static_cast<FTargetId>(0));
	TestFalse(TEXT("target id 0 is recognizable as invalid"), IsValidCombatTargetId(InvalidCombatTargetId));
	TestTrue(TEXT("nonzero target id is valid"), IsValidCombatTargetId(1));

	// Pellets are 0..254; 255 is the reserved "no pellet" sentinel.
	TestEqual(TEXT("InvalidCombatPelletIndex is 255"), InvalidCombatPelletIndex, static_cast<FPelletIndex>(255));
	TestFalse(TEXT("pellet sentinel 255 is invalid"), IsValidCombatPelletIndex(InvalidCombatPelletIndex));
	TestTrue(TEXT("pellet 0 is valid"), IsValidCombatPelletIndex(0));
	TestTrue(TEXT("pellet 254 is valid"), IsValidCombatPelletIndex(254));

	// A default-constructed key is the all-invalid key and is recognizable.
	FCombatEventKey DefaultKey;
	TestFalse(TEXT("default-constructed key is not usable"), IsUsableCombatEventKey(DefaultKey));
	TestFalse(TEXT("default key epoch is invalid"), IsValidCombatEpoch(DefaultKey.Epoch));
	TestFalse(TEXT("default key EntityId is invalid"), IsValidCombatEntityId(DefaultKey.EntityId));
	TestFalse(TEXT("default key ShotId is invalid"), IsValidCombatShotId(DefaultKey.ShotId));
	TestFalse(TEXT("default key TargetId is invalid"), IsValidCombatTargetId(DefaultKey.TargetId));

	// Zeroing exactly one id field keeps the whole key unusable, so a half
	// populated key can never sneak into a ledger as a real event.
	const FCombatEventKey Usable{1, 1, 1, 0, 1};
	TestTrue(TEXT("fully populated key is usable"), IsUsableCombatEventKey(Usable));
	FCombatEventKey ZeroedEpoch = Usable;
	ZeroedEpoch.Epoch = 0;
	TestFalse(TEXT("zeroed epoch makes the key unusable"), IsUsableCombatEventKey(ZeroedEpoch));
	FCombatEventKey ZeroedEntity = Usable;
	ZeroedEntity.EntityId = 0;
	TestFalse(TEXT("zeroed EntityId makes the key unusable"), IsUsableCombatEventKey(ZeroedEntity));
	FCombatEventKey ZeroedShot = Usable;
	ZeroedShot.ShotId = 0;
	TestFalse(TEXT("zeroed ShotId makes the key unusable"), IsUsableCombatEventKey(ZeroedShot));
	FCombatEventKey ZeroedTarget = Usable;
	ZeroedTarget.TargetId = 0;
	TestFalse(TEXT("zeroed TargetId makes the key unusable"), IsUsableCombatEventKey(ZeroedTarget));
	return true;
}

// ---------------------------------------------------------------------------
// NoActorPointers
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_002NoActorPointers,
	"UEMMO.Tasks.M5_002.NoActorPointers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_002NoActorPointers::RunTest(const FString& Parameters)
{
	// The file-scope static asserts above already fail the build on any
	// pointer member; the runtime mirrors below keep the contract visible in
	// the automation report.
	TestTrue(TEXT("FCombatEventKey is trivially copyable (a value snapshot)"), std::is_trivially_copyable_v<FCombatEventKey>);
	TestTrue(TEXT("FCombatEventKey is default constructible"), std::is_default_constructible_v<FCombatEventKey>);
	TestTrue(TEXT("FCombatEventKey::Epoch is not a pointer"), !std::is_pointer_v<decltype(FCombatEventKey::Epoch)>);
	TestTrue(TEXT("FCombatEventKey::EntityId is not a pointer"), !std::is_pointer_v<decltype(FCombatEventKey::EntityId)>);
	TestTrue(TEXT("FCombatEventKey::ShotId is not a pointer"), !std::is_pointer_v<decltype(FCombatEventKey::ShotId)>);
	TestTrue(TEXT("FCombatEventKey::PelletIndex is not a pointer"), !std::is_pointer_v<decltype(FCombatEventKey::PelletIndex)>);
	TestTrue(TEXT("FCombatEventKey::TargetId is not a pointer"), !std::is_pointer_v<decltype(FCombatEventKey::TargetId)>);
	return true;
}

// ---------------------------------------------------------------------------
// HitDecisionCoverage
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_002HitDecisionCoverage,
	"UEMMO.Tasks.M5_002.HitDecisionCoverage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_002HitDecisionCoverage::RunTest(const FString& Parameters)
{
	// The frozen ordinals (interface contract section 0; append only, never
	// renumber, so persisted logs and ledger entries keep their meaning).
	TestEqual(TEXT("Apply is 0"), static_cast<uint8>(EHitDecision::Apply), 0);
	TestEqual(TEXT("Block_Invulnerable is 1"), static_cast<uint8>(EHitDecision::Block_Invulnerable), 1);
	TestEqual(TEXT("Block_Immune is 2"), static_cast<uint8>(EHitDecision::Block_Immune), 2);
	TestEqual(TEXT("Block_Ignore is 3"), static_cast<uint8>(EHitDecision::Block_Ignore), 3);

	// Reflection proves the enum exposes exactly the four frozen values, so
	// the pairwise check below cannot silently miss an appended decision.
	// The engine appends one hidden, fully qualified "<Enum>::_MAX" sentinel
	// when it constructs a UHT-registered enum; sentinel entries are skipped
	// here and only real enumerators are counted.
	const UEnum* DecisionEnum = StaticEnum<EHitDecision>();
	if (!TestNotNull(TEXT("EHitDecision is a reflected UENUM"), DecisionEnum))
	{
		return false;
	}
	int32 RealValueCount = 0;
	for (int32 Index = 0; Index < DecisionEnum->NumEnums(); ++Index)
	{
		const FString ReflectedName = DecisionEnum->GetNameStringByIndex(Index);
		if (ReflectedName.EndsWith(TEXT("_MAX")))
		{
			continue;
		}
		++RealValueCount;
		const EHitDecision Reflected = static_cast<EHitDecision>(DecisionEnum->GetValueByIndex(Index));
		TestTrue(TEXT("every reflected decision is one of the four frozen values"),
			Reflected == EHitDecision::Apply
			|| Reflected == EHitDecision::Block_Invulnerable
			|| Reflected == EHitDecision::Block_Immune
			|| Reflected == EHitDecision::Block_Ignore);
	}
	TestEqual(TEXT("EHitDecision reflects exactly the four frozen values"), RealValueCount, 4);

	// The four named decisions are pairwise distinct.
	const EHitDecision All[] = {EHitDecision::Apply, EHitDecision::Block_Invulnerable, EHitDecision::Block_Immune, EHitDecision::Block_Ignore};
	static_assert(UE_ARRAY_COUNT(All) == 4, "the coverage list must hold exactly the four frozen decisions");
	for (int32 Outer = 0; Outer < static_cast<int32>(UE_ARRAY_COUNT(All)); ++Outer)
	{
		for (int32 Inner = Outer + 1; Inner < static_cast<int32>(UE_ARRAY_COUNT(All)); ++Inner)
		{
			TestTrue(TEXT("frozen decisions are pairwise distinct"), All[Outer] != All[Inner]);
		}
	}
	return true;
}

// ---------------------------------------------------------------------------
// LocalTickIsNotAClock
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_002LocalTickIsNotAClock,
	"UEMMO.Tasks.M5_002.LocalTickIsNotAClock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_002LocalTickIsNotAClock::RunTest(const FString& Parameters)
{
	// FCombatTick is the EpochTick counter's value type: a per-component
	// monotonic bookkeeping counter. It is not a global clock and not
	// seconds; GameClockSeconds (UWorld::GetTimeSeconds, frozen by Pause)
	// stays the only seconds-based clock and the two are never mixed.
	TestTrue(TEXT("FCombatTick is a 32-bit unsigned counter"), std::is_same_v<FCombatTick, uint32>);
	TestTrue(TEXT("FCombatTick is not a floating-point seconds clock"), !std::is_floating_point_v<FCombatTick>);
	TestTrue(TEXT("FCombatTick is not GameClockSeconds' double"), !std::is_same_v<FCombatTick, double>);
	TestTrue(TEXT("FCombatTick is not the 64-bit epoch id"), !std::is_same_v<FCombatTick, FCombatEpoch>);

	// Within one component the counter orders events monotonically.
	FCombatTick Early = 4;
	FCombatTick Later = 5;
	TestTrue(TEXT("a later tick orders after an earlier tick of the same component"), Later > Early);
	return true;
}

#endif
