// M3-009: claiming pending rewards with full-inventory retention (interface
// contract section 8: items that fit enter the inventory by their
// pre-generated InstanceId, items that do not fit stay pending unchanged;
// the XP is granted exactly once per settlement). Locks the URewardService::
// TryClaimPending contract:
// - a claim into a FULL inventory reports InventoryFull, stores nothing and
//   retains the pending item field-for-field (the original InstanceId and
//   the one-time roll - nothing is lost);
// - after freeing a slot the retry claims exactly the ORIGINAL instance (no
//   new FGuid, no random refresh) and the consumed draft is removed while
//   the applied-id record stays (IsSettlementApplied stays true);
// - the XP is granted exactly once per settlement across repeat clicks (the
//   total stays the design 50, never 100), even when the first attempt ran
//   into a full inventory;
// - an unknown SettlementId fails explicitly without side effects, and a
//   settlement that was never bound to a profile is rejected;
// - a multi-item draft partially claims: stored items leave the draft,
//   rejected items stay retained with their original identity.
//
// Harness note: the profile-integration tests run through a REAL UGameInstance
// created the engine's own FTestWorldWrapper way (same pattern as the M3-008
// suite: NewObject<UGameInstance>(GEngine), a dedicated world context owning
// the instance, UWorld::SetGameInstance + SetCurrentWorld, then Init()). All
// helpers are file-local copies with M3_009 names (RewardDraftTests.cpp is
// never modified and every suite stays uniquely linkable).
//
// Stub-failure note: the red stub refuses every TryClaimPending with a
// constant RejectedNoProfile and never grants XP, so every test below fails
// on values (assertion red, not build red); only the no-profile rejection at
// the top of UnknownSettlementIdFailsWithoutSideEffects matches the stub.

#include "Misc/AutomationTest.h"

#include "../Profile/ProfileSubsystem.h"
#include "../Profile/RewardService.h"
#include "../Items/InventoryModel.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../Room/RoomResult.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_009
{
	// World names are uniquified: several suites can live in one process and
	// two of this suite's own worlds can be alive in one test.
	static int32 M3_009_WorldCounter = 0;

	// One real game instance + its current world, wired exactly like the
	// engine's FTestWorldWrapper (see the M3-008 harness comment). Used only
	// where the profile's GameInstance lifetime is the subject.
	struct FM3_009_ProfileSession
	{
		UWorld* World = nullptr;
		FWorldContext* Context = nullptr;
		UGameInstance* GameInstance = nullptr;
		UProfileSubsystem* Profile = nullptr;

		static FM3_009_ProfileSession Create(FAutomationTestBase& Test, const TCHAR* Tag)
		{
			FM3_009_ProfileSession Session;
			++M3_009_WorldCounter;
			const FName WorldName = FName(*FString::Printf(TEXT("M3_009_TestWorld_%s_%d"), Tag, M3_009_WorldCounter));
			Session.World = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, WorldName);
			if (!Test.TestNotNull(TEXT("a private test world is available"), Session.World))
			{
				return Session;
			}
			Session.GameInstance = NewObject<UGameInstance>(GEngine);
			Session.Context = &GEngine->CreateNewWorldContext(EWorldType::Game);
			Session.Context->OwningGameInstance = Session.GameInstance;
			Session.World->SetGameInstance(Session.GameInstance);
			Session.Context->SetCurrentWorld(Session.World);
			// The production path that creates every registered
			// UGameInstanceSubsystem (engine FTestWorldWrapper precedent).
			Session.GameInstance->Init();
			Session.Profile = Session.GameInstance->GetSubsystem<UProfileSubsystem>();
			Test.TestNotNull(TEXT("the profile subsystem exists in the game instance's collection"), Session.Profile);
			return Session;
		}

		/** Full teardown in the engine FTestWorldWrapper::DestroyTestWorld order. */
		void TearDown()
		{
			if (World)
			{
				World->RemoveFromRoot();
				if (GameInstance)
				{
					GameInstance->Shutdown();
					GameInstance = nullptr;
				}
				if (Context)
				{
					GEngine->DestroyWorldContext(World);
					Context = nullptr;
				}
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
				World = nullptr;
			}
			Profile = nullptr;
		}
	};

	/** The three training definitions in a fresh catalog (mirrors Data/items.json). */
	static FItemDefinitionCatalog M3_009_MakeTrainingCatalog()
	{
		FItemDefinitionCatalog Catalog;

		FItemDefinition Weapon;
		Weapon.DefinitionId = FName(TEXT("weapon_training"));
		Weapon.DisplayName = TEXT("weapon_training");
		Weapon.Slot = EItemSlot::Weapon;
		Weapon.BaseStats.Attack = 5.0f;
		Weapon.BaseStats.Defense = 0.0f;
		Weapon.BaseStats.MaxHP = 0.0f;
		Weapon.IconPath = TEXT("");
		Weapon.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Weapon, nullptr);

		FItemDefinition Armor;
		Armor.DefinitionId = FName(TEXT("armor_training"));
		Armor.DisplayName = TEXT("armor_training");
		Armor.Slot = EItemSlot::Armor;
		Armor.BaseStats.Attack = 0.0f;
		Armor.BaseStats.Defense = 3.0f;
		Armor.BaseStats.MaxHP = 0.0f;
		Armor.IconPath = TEXT("");
		Armor.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Armor, nullptr);

		FItemDefinition Charm;
		Charm.DefinitionId = FName(TEXT("charm_training"));
		Charm.DisplayName = TEXT("charm_training");
		Charm.Slot = EItemSlot::Accessory;
		Charm.BaseStats.Attack = 0.0f;
		Charm.BaseStats.Defense = 0.0f;
		Charm.BaseStats.MaxHP = 20.0f;
		Charm.IconPath = TEXT("");
		Charm.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Charm, nullptr);

		return Catalog;
	}

	/** A finished-run result value; RunId is kept distinct alongside SettlementId. */
	static FRoomResult M3_009_MakeResult(uint64 RunId, uint64 SettlementId, int32 Seed, bool bCleared)
	{
		FRoomResult Result;
		Result.RunId = RunId;
		Result.SettlementId = SettlementId;
		Result.RoomId = FName(TEXT("training_room"));
		Result.Seed = Seed;
		Result.bCleared = bCleared;
		Result.KilledCount = 3;
		Result.ElapsedSeconds = 12.5;
		return Result;
	}

	/** A local filler definition (never added to any catalog) for capacity setup. */
	static FItemDefinition M3_009_MakeFillerDefinition()
	{
		FItemDefinition Filler;
		Filler.DefinitionId = FName(TEXT("filler_training"));
		Filler.DisplayName = TEXT("filler_training");
		Filler.Slot = EItemSlot::Weapon;
		Filler.BaseStats.Attack = 1.0f;
		Filler.BaseStats.Defense = 0.0f;
		Filler.BaseStats.MaxHP = 0.0f;
		Filler.IconPath = TEXT("");
		Filler.Rarity = EItemRarity::Normal;
		return Filler;
	}

	/** Fills the inventory with Count distinct valid instances; returns their InstanceIds in insertion order. */
	static TArray<FGuid> M3_009_FillInventory(FInventoryModel& Inventory, const FItemDefinition& Definition, int32 Count)
	{
		TArray<FGuid> FillerIds;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FItemInstance Filler = MakeItemInstance(Definition, /*RollSeed*/ 60000 + Index);
			FillerIds.Add(Filler.InstanceId);
			Inventory.TryAdd(Filler);
		}
		return FillerIds;
	}

	/** Field-for-field instance equality (identity, definition, roll seed, level, stats). */
	static bool M3_009_InstancesEqual(const FItemInstance& A, const FItemInstance& B)
	{
		return A.InstanceId == B.InstanceId &&
			A.DefinitionId == B.DefinitionId &&
			A.RollSeed == B.RollSeed &&
			A.Level == B.Level &&
			A.RolledStats.Attack == B.RolledStats.Attack &&
			A.RolledStats.Defense == B.RolledStats.Defense &&
			A.RolledStats.MaxHP == B.RolledStats.MaxHP;
	}

	/** The stored instance with the given id, or nullptr when absent. */
	static const FItemInstance* M3_009_FindStored(const FInventoryModel& Inventory, const FGuid& InstanceId)
	{
		const TArray<FItemInstance>& All = Inventory.GetAll();
		for (int32 Index = 0; Index < All.Num(); ++Index)
		{
			if (All[Index].InstanceId == InstanceId)
			{
				return &All[Index];
			}
		}
		return nullptr;
	}

	/**
	 * Shared setup: creates the session, mints a profile and binds a fresh
	 * URewardService to it. Returns a null service (after TestNotNull) when
	 * the harness itself is broken; callers bail out on nullptr.
	 */
	static URewardService* M3_009_MakeBoundService(FAutomationTestBase& Test, FM3_009_ProfileSession& Session, const TCHAR* Tag)
	{
		if (Session.Profile)
		{
			Session.Profile->NewProfile();
		}
		URewardService* Service = NewObject<URewardService>(Session.GameInstance);
		Test.TestNotNull(TEXT("the reward service object is created"), Service);
		if (Service)
		{
			Service->BindProfile(Session.Profile);
		}
		return Service;
	}
}

using namespace UE::UEMMO::Tasks::M3_009;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_009FullInventoryClaimRetainsPendingItem,
	"UEMMO.Tasks.M3_009.FullInventoryClaimRetainsPendingItem",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_009FullInventoryClaimRetainsPendingItem::RunTest(const FString& Parameters)
{
	FM3_009_ProfileSession Session = FM3_009_ProfileSession::Create(*this, TEXT("FullBag"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	URewardService* Service = M3_009_MakeBoundService(*this, Session, TEXT("FullBag"));
	if (Service == nullptr)
	{
		Session.TearDown();
		return true;
	}

	// Fill the inventory to the full 30-slot capacity, then settle one run.
	const FItemDefinition FillerDef = M3_009_MakeFillerDefinition();
	const TArray<FGuid> Fillers = M3_009_FillInventory(Session.Profile->GetInventory(), FillerDef, FInventoryModel::Capacity);
	TestEqual(TEXT("the harness filled the inventory to the full capacity"),
		Session.Profile->GetInventory().Count(), FInventoryModel::Capacity);
	TestTrue(TEXT("the harness actually stored distinct filler instances"), Fillers.Num() == FInventoryModel::Capacity);

	const FRoomResult Result = M3_009_MakeResult(/*RunId*/ 21, /*SettlementId*/ 2101, /*Seed*/ 21001, /*bCleared*/ true);
	const FRewardBeginOutcome Begin = Service->BeginReward(Result, M3_009_MakeTrainingCatalog());
	TestEqual(TEXT("the settlement began with a pending draft"), Begin.Result, ERewardBeginResult::Applied);
	if (Begin.Result != ERewardBeginResult::Applied || Begin.Draft.Items.Num() != 1)
	{
		Session.TearDown();
		return true;
	}
	const FItemInstance Original = Begin.Draft.Items[0];
	const int32 XPBefore = Session.Profile->GetXP();

	// The claim into the FULL inventory: the item cannot enter, the caller
	// must see InventoryFull and the pending draft must retain the item.
	const FRewardClaimOutcome Claim = Service->TryClaimPending(2101, Session.Profile->GetInventory());

	TestEqual(TEXT("a claim into a full inventory reports InventoryFull"), Claim.Result, ERewardClaimResult::InventoryFull);
	TestEqual(TEXT("the full-bag claim stored nothing"), Claim.ClaimedItemCount, 0);
	TestEqual(TEXT("the full-bag claim retains the pending item"), Claim.RetainedItemCount, 1);
	TestTrue(TEXT("the full-bag claim names the retention in its message"), Claim.Error.Contains(TEXT("retained")));
	TestEqual(TEXT("the full inventory is unchanged after the claim"),
		Session.Profile->GetInventory().Count(), FInventoryModel::Capacity);
	TestTrue(TEXT("the original InstanceId never entered the inventory"),
		!Session.Profile->GetInventory().Contains(Original.InstanceId));

	// Nothing lost: the draft is still pending and carries the ORIGINAL
	// instance field-for-field (same InstanceId, same one-time roll).
	TestEqual(TEXT("the settlement's draft is still pending"), Session.Profile->GetPendingRewards().Num(), 1);
	if (Session.Profile->GetPendingRewards().Num() == 1 && Session.Profile->GetPendingRewards()[0].Items.Num() == 1)
	{
		TestTrue(TEXT("the retained pending item is field-for-field the original instance"),
			M3_009_InstancesEqual(Session.Profile->GetPendingRewards()[0].Items[0], Original));
	}

	// XP is settlement progress, not inventory payload: the first claim
	// attempt granted the draft XP exactly once even with a full bag.
	TestTrue(TEXT("the first claim attempt granted the settlement XP"), Claim.bGrantedXP);
	TestEqual(TEXT("the profile XP grew by the draft XP (50)"),
		Session.Profile->GetXP() - XPBefore, URewardService::RewardXPPerClear);
	TestTrue(TEXT("the settlement id is recorded as applied"), Session.Profile->IsSettlementApplied(2101));

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_009FreeingOneSlotClaimsTheOriginalInstanceOnly,
	"UEMMO.Tasks.M3_009.FreeingOneSlotClaimsTheOriginalInstanceOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_009FreeingOneSlotClaimsTheOriginalInstanceOnly::RunTest(const FString& Parameters)
{
	FM3_009_ProfileSession Session = FM3_009_ProfileSession::Create(*this, TEXT("FreeSlot"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	URewardService* Service = M3_009_MakeBoundService(*this, Session, TEXT("FreeSlot"));
	if (Service == nullptr)
	{
		Session.TearDown();
		return true;
	}

	const FItemDefinition FillerDef = M3_009_MakeFillerDefinition();
	const TArray<FGuid> Fillers = M3_009_FillInventory(Session.Profile->GetInventory(), FillerDef, FInventoryModel::Capacity);

	const FRoomResult Result = M3_009_MakeResult(/*RunId*/ 22, /*SettlementId*/ 2202, /*Seed*/ 22002, /*bCleared*/ true);
	const FRewardBeginOutcome Begin = Service->BeginReward(Result, M3_009_MakeTrainingCatalog());
	TestEqual(TEXT("the settlement began with a pending draft"), Begin.Result, ERewardBeginResult::Applied);
	if (Begin.Result != ERewardBeginResult::Applied || Begin.Draft.Items.Num() != 1)
	{
		Session.TearDown();
		return true;
	}
	const FItemInstance Original = Begin.Draft.Items[0];
	const int32 XPBefore = Session.Profile->GetXP();

	// First attempt against the full bag: retained, nothing stored.
	const FRewardClaimOutcome FullClaim = Service->TryClaimPending(2202, Session.Profile->GetInventory());
	TestEqual(TEXT("the first claim hits the full inventory"), FullClaim.Result, ERewardClaimResult::InventoryFull);
	TestEqual(TEXT("the first claim stores nothing"), FullClaim.ClaimedItemCount, 0);

	// Free exactly one slot, then re-claim.
	const EInventoryRemoveResult Removed = Session.Profile->GetInventory().Remove(Fillers[0]);
	TestEqual(TEXT("one freed slot makes room"), Removed, EInventoryRemoveResult::Removed);
	TestEqual(TEXT("the inventory has exactly one free slot now"),
		Session.Profile->GetInventory().Count(), FInventoryModel::Capacity - 1);

	const FRewardClaimOutcome Retry = Service->TryClaimPending(2202, Session.Profile->GetInventory());

	TestEqual(TEXT("the re-claim after freeing a slot claims the draft"), Retry.Result, ERewardClaimResult::Claimed);
	TestEqual(TEXT("the re-claim stored exactly one item"), Retry.ClaimedItemCount, 1);
	TestEqual(TEXT("the re-claim retains nothing"), Retry.RetainedItemCount, 0);
	TestFalse(TEXT("the re-claim grants no XP again"), Retry.bGrantedXP);
	TestEqual(TEXT("the inventory is full again, holding the claimed instance"),
		Session.Profile->GetInventory().Count(), FInventoryModel::Capacity);

	// ONLY the original instance entered: same InstanceId (no new FGuid), no
	// re-roll, no random refresh - field-for-field the pre-generated roll.
	const FItemInstance* Stored = M3_009_FindStored(Session.Profile->GetInventory(), Original.InstanceId);
	TestNotNull(TEXT("the claimed instance is the ORIGINAL instance (same InstanceId, no new FGuid)"), Stored);
	if (Stored != nullptr)
	{
		TestTrue(TEXT("the stored instance is field-for-field the original (no re-roll, no random refresh)"),
			M3_009_InstancesEqual(*Stored, Original));
	}

	// The consumed draft is removed; the applied record and the once-granted
	// XP stay (the total across BOTH attempts is the design 50, never 100).
	TestEqual(TEXT("the fully consumed draft was removed from pending"),
		Session.Profile->GetPendingRewards().Num(), 0);
	TestTrue(TEXT("the settlement stays recorded as applied"), Session.Profile->IsSettlementApplied(2202));
	TestEqual(TEXT("the total XP granted across both claim attempts is 50"),
		Session.Profile->GetXP() - XPBefore, URewardService::RewardXPPerClear);

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_009XPGrantedOncePerSettlementAcrossRepeats,
	"UEMMO.Tasks.M3_009.XPGrantedOncePerSettlementAcrossRepeats",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_009XPGrantedOncePerSettlementAcrossRepeats::RunTest(const FString& Parameters)
{
	FM3_009_ProfileSession Session = FM3_009_ProfileSession::Create(*this, TEXT("XPOnce"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	URewardService* Service = M3_009_MakeBoundService(*this, Session, TEXT("XPOnce"));
	if (Service == nullptr)
	{
		Session.TearDown();
		return true;
	}

	// Roomy inventory: the item fits, so the FIRST claim succeeds outright.
	const FRoomResult Result = M3_009_MakeResult(/*RunId*/ 23, /*SettlementId*/ 2303, /*Seed*/ 23003, /*bCleared*/ true);
	const FRewardBeginOutcome Begin = Service->BeginReward(Result, M3_009_MakeTrainingCatalog());
	TestEqual(TEXT("the settlement began with a pending draft"), Begin.Result, ERewardBeginResult::Applied);
	if (Begin.Result != ERewardBeginResult::Applied || Begin.Draft.Items.Num() != 1)
	{
		Session.TearDown();
		return true;
	}
	const FItemInstance Original = Begin.Draft.Items[0];
	const int32 XPBefore = Session.Profile->GetXP();

	const FRewardClaimOutcome First = Service->TryClaimPending(2303, Session.Profile->GetInventory());
	TestEqual(TEXT("the first claim succeeds"), First.Result, ERewardClaimResult::Claimed);
	TestTrue(TEXT("the first claim grants the XP"), First.bGrantedXP);
	TestEqual(TEXT("the first claim grows the XP by the design 50"),
		Session.Profile->GetXP() - XPBefore, URewardService::RewardXPPerClear);

	// Repeat clicks: fully claimed already - no XP again, no second item.
	for (int32 Repeat = 0; Repeat < 3; ++Repeat)
	{
		const FRewardClaimOutcome Again = Service->TryClaimPending(2303, Session.Profile->GetInventory());
		TestEqual(FString::Printf(TEXT("repeat %d answers AlreadyClaimed"), Repeat),
			Again.Result, ERewardClaimResult::AlreadyClaimed);
		TestFalse(FString::Printf(TEXT("repeat %d grants no XP again"), Repeat), Again.bGrantedXP);
		TestEqual(FString::Printf(TEXT("repeat %d stores no item again"), Repeat), Again.ClaimedItemCount, 0);
		TestEqual(FString::Printf(TEXT("repeat %d leaves the XP total at 50"), Repeat),
			Session.Profile->GetXP() - XPBefore, URewardService::RewardXPPerClear);
	}

	TestEqual(TEXT("exactly one item was ever stored (no duplicate equipment)"),
		Session.Profile->GetInventory().Count(), 1);
	TestTrue(TEXT("the stored item is the original instance"),
		M3_009_FindStored(Session.Profile->GetInventory(), Original.InstanceId) != nullptr);
	TestEqual(TEXT("the pending list holds no leftover drafts"),
		Session.Profile->GetPendingRewards().Num(), 0);

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_009FullyConsumedDraftRemovedWhileAppliedIdStays,
	"UEMMO.Tasks.M3_009.FullyConsumedDraftRemovedWhileAppliedIdStays",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_009FullyConsumedDraftRemovedWhileAppliedIdStays::RunTest(const FString& Parameters)
{
	FM3_009_ProfileSession Session = FM3_009_ProfileSession::Create(*this, TEXT("Applied"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	URewardService* Service = M3_009_MakeBoundService(*this, Session, TEXT("Applied"));
	if (Service == nullptr)
	{
		Session.TearDown();
		return true;
	}

	const FRoomResult Result = M3_009_MakeResult(/*RunId*/ 24, /*SettlementId*/ 2404, /*Seed*/ 24004, /*bCleared*/ true);
	const FRewardBeginOutcome Begin = Service->BeginReward(Result, M3_009_MakeTrainingCatalog());
	TestEqual(TEXT("the settlement began with a pending draft"), Begin.Result, ERewardBeginResult::Applied);
	if (Begin.Result != ERewardBeginResult::Applied)
	{
		Session.TearDown();
		return true;
	}

	const FRewardClaimOutcome Claim = Service->TryClaimPending(2404, Session.Profile->GetInventory());
	TestEqual(TEXT("the claim succeeds"), Claim.Result, ERewardClaimResult::Claimed);

	// The empty draft is removed, but the settled id's record STAYS: this is
	// the IsSettlementApplied semantics the XP-once and BeginReward guards
	// rely on. An unrelated id is never recorded.
	TestEqual(TEXT("the consumed draft is removed from pending"),
		Session.Profile->GetPendingRewards().Num(), 0);
	TestTrue(TEXT("the claimed settlement id is recorded as applied"),
		Session.Profile->IsSettlementApplied(2404));
	TestTrue(TEXT("an unrelated id is not recorded as applied"),
		!Session.Profile->IsSettlementApplied(424242));

	// M3-008 header sub-case (b): replaying BeginReward for a CLAIMED
	// settlement answers AlreadyApplied with a default (empty) draft - no
	// draft, no re-roll, nothing stored again.
	const FRewardBeginOutcome Replay = Service->BeginReward(Result, M3_009_MakeTrainingCatalog());
	TestEqual(TEXT("the claimed settlement's BeginReward replay answers AlreadyApplied"),
		Replay.Result, ERewardBeginResult::AlreadyApplied);
	TestEqual(TEXT("the claimed settlement's replay carries no pending draft"), Replay.Draft.Items.Num(), 0);
	TestEqual(TEXT("the replay added no draft"), Session.Profile->GetPendingRewards().Num(), 0);

	// And the claim itself stays idempotent: repeat TryClaimPending answers
	// AlreadyClaimed and changes nothing.
	const FRewardClaimOutcome Repeat = Service->TryClaimPending(2404, Session.Profile->GetInventory());
	TestEqual(TEXT("a repeat claim after full consumption answers AlreadyClaimed"),
		Repeat.Result, ERewardClaimResult::AlreadyClaimed);
	TestEqual(TEXT("the repeat claim stores nothing"), Repeat.ClaimedItemCount, 0);
	TestEqual(TEXT("the repeat claim retains nothing"), Repeat.RetainedItemCount, 0);
	TestEqual(TEXT("the inventory still holds exactly the one claimed item"),
		Session.Profile->GetInventory().Count(), 1);

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_009UnknownSettlementIdFailsWithoutSideEffects,
	"UEMMO.Tasks.M3_009.UnknownSettlementIdFailsWithoutSideEffects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_009UnknownSettlementIdFailsWithoutSideEffects::RunTest(const FString& Parameters)
{
	FM3_009_ProfileSession Session = FM3_009_ProfileSession::Create(*this, TEXT("Unknown"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}

	// Bound service but NO profile yet: there is nothing to claim from.
	URewardService* Service = NewObject<URewardService>(Session.GameInstance);
	TestNotNull(TEXT("the reward service object is created"), Service);
	if (Service == nullptr)
	{
		Session.TearDown();
		return true;
	}
	Service->BindProfile(Session.Profile);
	const FRewardClaimOutcome NoProfile = Service->TryClaimPending(2505, Session.Profile->GetInventory());
	TestEqual(TEXT("a claim without any profile is rejected"), NoProfile.Result, ERewardClaimResult::RejectedNoProfile);
	TestTrue(TEXT("the no-profile rejection names the missing profile"), NoProfile.Error.Contains(TEXT("profile")));

	// With a profile and an unrelated pending draft: an unknown settlement id
	// fails explicitly and touches nothing.
	Session.Profile->NewProfile();
	const FRoomResult Result = M3_009_MakeResult(/*RunId*/ 25, /*SettlementId*/ 2506, /*Seed*/ 25006, /*bCleared*/ true);
	const FRewardBeginOutcome Begin = Service->BeginReward(Result, M3_009_MakeTrainingCatalog());
	TestEqual(TEXT("the unrelated settlement began with a pending draft"), Begin.Result, ERewardBeginResult::Applied);
	const int32 XPBefore = Session.Profile->GetXP();

	const FRewardClaimOutcome Unknown = Service->TryClaimPending(999999, Session.Profile->GetInventory());
	TestEqual(TEXT("an unknown settlement id fails explicitly"), Unknown.Result, ERewardClaimResult::UnknownSettlement);
	TestTrue(TEXT("the unknown-id failure names the missing settlement"),
		Unknown.Error.Contains(TEXT("no pending reward draft")));
	TestEqual(TEXT("the unknown claim stored nothing"), Unknown.ClaimedItemCount, 0);
	TestEqual(TEXT("the unknown claim granted no XP"), Session.Profile->GetXP() - XPBefore, 0);
	TestEqual(TEXT("the unrelated draft survived untouched"),
		Session.Profile->GetPendingRewards().Num(), 1);
	TestTrue(TEXT("the unknown id is not recorded as applied"),
		!Session.Profile->IsSettlementApplied(999999));

	// Repeating the unknown claim stays a safe, explicit failure (no crash).
	const FRewardClaimOutcome UnknownAgain = Service->TryClaimPending(999999, Session.Profile->GetInventory());
	TestEqual(TEXT("repeating the unknown claim fails identically"),
		UnknownAgain.Result, ERewardClaimResult::UnknownSettlement);
	TestEqual(TEXT("repeats still changed nothing"),
		Session.Profile->GetPendingRewards().Num(), 1);
	TestEqual(TEXT("repeats still granted no XP"), Session.Profile->GetXP() - XPBefore, 0);

	Session.TearDown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_009MultiItemDraftPartiallyClaimsRetainingFailures,
	"UEMMO.Tasks.M3_009.MultiItemDraftPartiallyClaimsRetainingFailures",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_009MultiItemDraftPartiallyClaimsRetainingFailures::RunTest(const FString& Parameters)
{
	FM3_009_ProfileSession Session = FM3_009_ProfileSession::Create(*this, TEXT("Partial"));
	if (Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	URewardService* Service = M3_009_MakeBoundService(*this, Session, TEXT("Partial"));
	if (Service == nullptr)
	{
		Session.TearDown();
		return true;
	}

	// One free slot, two pending items: the first item fits, the second is
	// retained. BeginReward produces exactly one item per settlement by
	// design, so the multi-item partial-claim semantics of the claim
	// interface are exercised by injecting a two-item draft through the
	// profile's backing container (the same container the service writes).
	const FItemDefinition FillerDef = M3_009_MakeFillerDefinition();
	const TArray<FGuid> Fillers = M3_009_FillInventory(Session.Profile->GetInventory(), FillerDef, FInventoryModel::Capacity - 1);
	TestEqual(TEXT("the harness left exactly one free slot"),
		Session.Profile->GetInventory().Count(), FInventoryModel::Capacity - 1);

	FPendingReward Draft;
	Draft.SettlementId = 2607;
	Draft.XP = URewardService::RewardXPPerClear;
	Draft.Items.Add(MakeItemInstance(FillerDef, /*RollSeed*/ 71));
	Draft.Items.Add(MakeItemInstance(FillerDef, /*RollSeed*/ 72));
	Session.Profile->GetPendingRewards().Add(Draft);
	const FItemInstance ItemA = Draft.Items[0];
	const FItemInstance ItemB = Draft.Items[1];
	const int32 XPBefore = Session.Profile->GetXP();

	const FRewardClaimOutcome First = Service->TryClaimPending(2607, Session.Profile->GetInventory());
	TestEqual(TEXT("a two-item draft with one free slot partially claims"),
		First.Result, ERewardClaimResult::PartiallyClaimed);
	TestEqual(TEXT("exactly one item entered the inventory"), First.ClaimedItemCount, 1);
	TestEqual(TEXT("exactly one item stayed pending"), First.RetainedItemCount, 1);
	TestTrue(TEXT("the first claim granted the XP once"), First.bGrantedXP);
	TestEqual(TEXT("the inventory is now full"), Session.Profile->GetInventory().Count(), FInventoryModel::Capacity);
	TestTrue(TEXT("the stored item is the first draft item (draft order)"),
		Session.Profile->GetInventory().Contains(ItemA.InstanceId));

	// The draft survives with ONLY the retained item, field-for-field the
	// original ItemB (same InstanceId, same one-time roll).
	TestEqual(TEXT("the partially claimed draft survives in pending"),
		Session.Profile->GetPendingRewards().Num(), 1);
	if (Session.Profile->GetPendingRewards().Num() == 1 && Session.Profile->GetPendingRewards()[0].Items.Num() == 1)
	{
		TestTrue(TEXT("the retained item is the original second item"),
			M3_009_InstancesEqual(Session.Profile->GetPendingRewards()[0].Items[0], ItemB));
	}

	// Free one slot and re-claim: ONLY the original ItemB enters.
	const EInventoryRemoveResult Removed = Session.Profile->GetInventory().Remove(Fillers[0]);
	TestEqual(TEXT("one freed slot makes room"), Removed, EInventoryRemoveResult::Removed);
	const FRewardClaimOutcome Second = Service->TryClaimPending(2607, Session.Profile->GetInventory());
	TestEqual(TEXT("the re-claim consumes the retained item"), Second.Result, ERewardClaimResult::Claimed);
	TestEqual(TEXT("the re-claim stored exactly one item"), Second.ClaimedItemCount, 1);
	TestEqual(TEXT("the re-claim retains nothing"), Second.RetainedItemCount, 0);
	TestFalse(TEXT("the re-claim grants no XP again"), Second.bGrantedXP);
	TestEqual(TEXT("the inventory is full again"), Session.Profile->GetInventory().Count(), FInventoryModel::Capacity);

	const FItemInstance* StoredB = M3_009_FindStored(Session.Profile->GetInventory(), ItemB.InstanceId);
	TestNotNull(TEXT("the retained item entered as the ORIGINAL instance (same InstanceId, no new FGuid)"), StoredB);
	if (StoredB != nullptr)
	{
		TestTrue(TEXT("the stored retained item is field-for-field the original (no random refresh)"),
			M3_009_InstancesEqual(*StoredB, ItemB));
	}

	TestEqual(TEXT("the fully consumed draft is removed"), Session.Profile->GetPendingRewards().Num(), 0);
	TestTrue(TEXT("the settlement stays recorded as applied"), Session.Profile->IsSettlementApplied(2607));
	TestEqual(TEXT("the total XP across both claims is 50"),
		Session.Profile->GetXP() - XPBefore, URewardService::RewardXPPerClear);

	Session.TearDown();
	return true;
}

#endif
