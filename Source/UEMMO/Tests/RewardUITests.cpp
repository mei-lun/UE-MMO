// M3-018: the settlement reward area of the result screen, the pending reward
// list and the claim feedback contract. The suites lock the card's behaviors:
//
//   1. The pure reward view model: five claim display states (Unclaimed /
//      Saving / Claimed / InventoryFull / Failed) derived from the pending
//      draft plus the real URewardService claim outcome - never invented.
//   2. The same settlement re-opened any number of times shows the SAME draft
//      items (the data source is the PendingRewards snapshot, BeginReward
//      answers idempotently and the display never re-rolls).
//   3. A Failed run result shows no reward area at all (service rejection +
//      collapsed widget row; the M2 empty-reward contract stays intact).
//   4. A save failure surfaces a readable error, NEVER a fake completion, and
//      the reward stays claimable (retry on the same pre-generated draft).
//   5. A full inventory keeps the item pending: the list + menu badge show
//      the retained count, the profile (GameInstance-level) still owns the
//      draft after returning to the menu, and a freed slot lets the SAME
//      instance claim (XP granted exactly once across both attempts).
//   6. The Claim button is a guarded one-shot request path: the visible half
//      disables the button on the first click, the HUD-side FRoomResultActionGuard
//      (the M2-012 reuse) drops the duplicate of a fast double click.
//   7. The pending rewards widget: rows with settlement id / XP / item names,
//      readable placeholders for missing definitions (no crash), an explicit
//      empty state and the shared full-bag prompt.
//
// Harness note: the storage doubles and the profile fixture are file-local
// copies with M3_018 names (RewardTransactionTests.cpp and every other suite
// stays unmodified and uniquely linkable). All claim tests run on the isolated
// "TestSlot_" prefix with an injected memory storage - the production
// "Profile_" pass is never touched. The HUD wiring itself (claim request path,
// menu badge draw) is thin glue over these tested pieces per the card.

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "../Items/InventoryModel.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../Persistence/ProfileSaveService.h"
#include "../Profile/ProfileSubsystem.h"
#include "../Profile/RewardService.h"
#include "../Room/RoomResult.h"
#include "../UI/PendingRewardsWidget.h"
#include "../UI/RoomResultWidget.h"

#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_018
{
	// -- Test storage doubles (the M3_016 file-local precedent, renamed) ---------

	/**
	 * In-memory ISaveStorage: every written slot is stored as an independent
	 * rooted duplicate, every read returns a fresh duplicate - disk semantics.
	 */
	class FM3_018_MemoryStorage final : public ISaveStorage
	{
	public:
		virtual ~FM3_018_MemoryStorage() override
		{
			for (TPair<FString, USaveGame*>& Pair : Slots)
			{
				if (Pair.Value)
				{
					Pair.Value->RemoveFromRoot();
				}
			}
		}

		virtual bool WriteSlot(const FString& SlotName, USaveGame* Data) override
		{
			if (!Data)
			{
				return false;
			}
			USaveGame* Copy = DuplicateObject(Data, GetTransientPackage());
			if (!Copy)
			{
				return false;
			}
			Copy->AddToRoot();
			USaveGame** Existing = Slots.Find(SlotName);
			if (Existing && *Existing)
			{
				(*Existing)->RemoveFromRoot();
			}
			Slots.Add(SlotName, Copy);
			return true;
		}

		virtual USaveGame* ReadSlot(const FString& SlotName) override
		{
			USaveGame* const* Found = Slots.Find(SlotName);
			return Found ? DuplicateObject(*Found, GetTransientPackage()) : nullptr;
		}

		virtual bool DeleteSlot(const FString& SlotName) override
		{
			USaveGame** Existing = Slots.Find(SlotName);
			if (!Existing)
			{
				return false;
			}
			if (*Existing)
			{
				(*Existing)->RemoveFromRoot();
			}
			Slots.Remove(SlotName);
			return true;
		}

		virtual bool DoesSlotExist(const FString& SlotName) override
		{
			return Slots.Contains(SlotName);
		}

	private:
		TMap<FString, USaveGame*> Slots;
	};

	/**
	 * Failure-injection decorator: refuses the write with the given 1-based
	 * ordinal (the atomic claim writes 1=draft slot, 2=draft index, 3=claim
	 * slot, 4=claim index; refusing 3 models the failing claim-snapshot save)
	 * or, with bFailAllWrites, refuses everything (an unavailable storage).
	 */
	class FM3_018_FlakyStorage final : public ISaveStorage
	{
	public:
		explicit FM3_018_FlakyStorage(ISaveStorage* InInner)
			: Inner(InInner)
		{
		}

		virtual bool WriteSlot(const FString& SlotName, USaveGame* Data) override
		{
			++WriteCounter;
			if (bFailAllWrites || (FailWriteOrdinal > 0 && WriteCounter == FailWriteOrdinal))
			{
				return false;
			}
			return Inner->WriteSlot(SlotName, Data);
		}

		virtual USaveGame* ReadSlot(const FString& SlotName) override
		{
			return Inner->ReadSlot(SlotName);
		}

		virtual bool DeleteSlot(const FString& SlotName) override
		{
			return Inner->DeleteSlot(SlotName);
		}

		virtual bool DoesSlotExist(const FString& SlotName) override
		{
			return Inner->DoesSlotExist(SlotName);
		}

		/** 1-based count of WriteSlot invocations through this decorator. */
		int32 WriteCounter = 0;

		/** The 1-based write ordinal whose write is refused (0 = none). */
		int32 FailWriteOrdinal = 0;

		/** When true, every write fails regardless of the ordinal. */
		bool bFailAllWrites = false;

	private:
		ISaveStorage* Inner = nullptr;
	};

	// -- Fixture helpers ---------------------------------------------------------

	/** Creates the save service on the isolated automation prefix. */
	static UProfileSaveService* M3_018_NewService(FAutomationTestBase& Test, ISaveStorage* Storage)
	{
		UProfileSaveService* Service = NewObject<UProfileSaveService>(GetTransientPackage());
		if (!Service)
		{
			Test.AddError(TEXT("setup: NewObject<UProfileSaveService> returned null"));
			return nullptr;
		}
		if (!Service->Initialize(TEXT("TestSlot_")))
		{
			Test.AddError(TEXT("setup: Initialize('TestSlot_') rejected the prefix"));
			return nullptr;
		}
		if (Storage)
		{
			Service->SetStorage(Storage);
		}
		return Service;
	}

	/**
	 * Bare profile fixture (the M3_016 precedent): a UGameInstance owner, a
	 * UProfileSubsystem and a URewardService, all rooted; setup mints a fresh
	 * level-1 profile and binds the reward service to it. No world needed:
	 * BeginReward/ClaimPendingAtomic take the profile explicitly.
	 */
	struct FM3_018_ProfileFixture
	{
		UGameInstance* Owner = nullptr;
		UProfileSubsystem* Profile = nullptr;
		URewardService* Reward = nullptr;

		bool Setup(FAutomationTestBase& Test)
		{
			Owner = NewObject<UGameInstance>(GEngine);
			Profile = Owner ? NewObject<UProfileSubsystem>(Owner) : nullptr;
			Reward = Owner ? NewObject<URewardService>(Owner) : nullptr;
			if (!Test.TestNotNull(TEXT("the profile subsystem fixture is constructible without a world"), Profile)
				|| !Test.TestNotNull(TEXT("the reward service fixture is constructible"), Reward))
			{
				return false;
			}
			Owner->AddToRoot();
			Profile->AddToRoot();
			Reward->AddToRoot();
			Reward->BindProfile(Profile);
			Profile->NewProfile();
			return true;
		}

		~FM3_018_ProfileFixture()
		{
			if (Reward)
			{
				Reward->RemoveFromRoot();
			}
			if (Profile)
			{
				Profile->RemoveFromRoot();
			}
			if (Owner)
			{
				Owner->RemoveFromRoot();
			}
		}
	};

	/** The three Data/items.json definitions in a fresh catalog (the starter drop table resolves). */
	static FItemDefinitionCatalog M3_018_MakeCatalog()
	{
		FItemDefinitionCatalog Catalog;

		FItemDefinition Weapon;
		Weapon.DefinitionId = FName(TEXT("weapon_training"));
		Weapon.DisplayName = TEXT("Training Sword");
		Weapon.Slot = EItemSlot::Weapon;
		Weapon.BaseStats.Attack = 5.0f;
		Weapon.BaseStats.Defense = 0.0f;
		Weapon.BaseStats.MaxHP = 0.0f;
		Weapon.IconPath = TEXT("");
		Weapon.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Weapon, nullptr);

		FItemDefinition Armor;
		Armor.DefinitionId = FName(TEXT("armor_training"));
		Armor.DisplayName = TEXT("Training Armor");
		Armor.Slot = EItemSlot::Armor;
		Armor.BaseStats.Attack = 0.0f;
		Armor.BaseStats.Defense = 3.0f;
		Armor.BaseStats.MaxHP = 0.0f;
		Armor.IconPath = TEXT("");
		Armor.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Armor, nullptr);

		FItemDefinition Charm;
		Charm.DefinitionId = FName(TEXT("charm_training"));
		Charm.DisplayName = TEXT("Training Charm");
		Charm.Slot = EItemSlot::Accessory;
		Charm.BaseStats.Attack = 0.0f;
		Charm.BaseStats.Defense = 0.0f;
		Charm.BaseStats.MaxHP = 20.0f;
		Charm.IconPath = TEXT("");
		Charm.Rarity = EItemRarity::Normal;
		Catalog.AddDefinition(Charm, nullptr);

		return Catalog;
	}

	/** A finished-run result value (the M3_016 shape). */
	static FRoomResult M3_018_MakeResult(uint64 RunId, uint64 SettlementId, int32 Seed, bool bCleared)
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

	/** A local filler definition (never in any catalog) for capacity setup. */
	static FItemDefinition M3_018_MakeFillerDefinition()
	{
		FItemDefinition Filler;
		Filler.DefinitionId = FName(TEXT("filler_m3_018"));
		Filler.DisplayName = TEXT("filler_m3_018");
		Filler.Slot = EItemSlot::Weapon;
		Filler.BaseStats.Attack = 1.0f;
		Filler.BaseStats.Defense = 0.0f;
		Filler.BaseStats.MaxHP = 0.0f;
		Filler.IconPath = TEXT("");
		Filler.Rarity = EItemRarity::Normal;
		return Filler;
	}

	/** Fills the inventory with Count distinct valid instances. */
	static void M3_018_FillInventory(FInventoryModel& Inventory, const FItemDefinition& Definition, int32 Count)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Inventory.TryAdd(MakeItemInstance(Definition, 51000 + Index));
		}
	}

	/** Field-for-field instance equality (identity, definition, roll seed, stats). */
	static bool M3_018_InstancesEqual(const FItemInstance& A, const FItemInstance& B)
	{
		return A.InstanceId == B.InstanceId &&
			A.DefinitionId == B.DefinitionId &&
			A.RollSeed == B.RollSeed &&
			A.RolledStats.Attack == B.RolledStats.Attack &&
			A.RolledStats.Defense == B.RolledStats.Defense &&
			A.RolledStats.MaxHP == B.RolledStats.MaxHP;
	}

	/** Field-for-field draft equality (identity, XP, item instances in order). */
	static bool M3_018_DraftsEqual(const FPendingReward& A, const FPendingReward& B)
	{
		if (A.SettlementId != B.SettlementId || A.XP != B.XP || A.Items.Num() != B.Items.Num())
		{
			return false;
		}
		for (int32 Index = 0; Index < A.Items.Num(); ++Index)
		{
			if (!M3_018_InstancesEqual(A.Items[Index], B.Items[Index]))
			{
				return false;
			}
		}
		return true;
	}

	/** Deletes the service's test slots (the isolated-prefix cleanup). */
	static void M3_018_CleanupTestSlots(FAutomationTestBase& Test, UProfileSaveService& Service)
	{
		Service.DeleteTestSlots();
		Test.TestTrue(TEXT("cleanup: the TestSlot_ slots are deleted"), Service.GetSlotPrefix().Equals(TEXT("TestSlot_")));
	}

	/** One widget test world (the M3_011 helper precedent). */
	static UWorld* M3_018_MakeWidgetWorld(FAutomationTestBase& Test, FTestWorldWrapper& Wrapper)
	{
		if (!Test.TestTrue(TEXT("the widget test world is created (engine FTestWorldWrapper precedent)"),
			Wrapper.CreateTestWorld(EWorldType::Game)))
		{
			return nullptr;
		}
		return Wrapper.GetTestWorld();
	}
}

using namespace UE::UEMMO::Tasks::M3_018;

// 1. Acceptance: the pure reward view model carries the five display states -
//    Unclaimed right after a draft bind, and the claim outcome mapping
//    Claimed/InventoryFull/Failed (Saving is the widget's in-flight state,
//    pinned by the save-failure suite below). Applying an outcome updates the
//    state and the readable status while the draft snapshot stays untouched.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_018RewardViewModelDerivesFiveClaimStates,
	"UEMMO.Tasks.M3_018.RewardViewModelDerivesFiveClaimStates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_018RewardViewModelDerivesFiveClaimStates::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_018_ProfileFixture Fixture;
	if (!Fixture.Setup(Test))
	{
		return true;
	}
	const FItemDefinitionCatalog Catalog = M3_018_MakeCatalog();

	const FRoomResult Result = M3_018_MakeResult(/*RunId*/ 18101, /*SettlementId*/ 18101, /*Seed*/ 181011, true);
	const FRewardBeginOutcome Begin = Fixture.Reward->BeginReward(Result, Catalog);
	Test.TestEqual(TEXT("setup: the settlement began with a pending draft"), Begin.Result, ERewardBeginResult::Applied);
	if (Begin.Result != ERewardBeginResult::Applied || Begin.Draft.Items.Num() < 1)
	{
		return true;
	}

	// Unclaimed: the fresh draft-bound view model.
	const FRoomRewardViewModel Unclaimed = MakeRoomRewardViewModelFromDraft(Begin.Draft, &Catalog);
	Test.TestTrue(TEXT("the draft-bound reward view model is valid"), Unclaimed.bValid);
	Test.TestEqual(TEXT("the fresh view model starts Unclaimed"),
		Unclaimed.ClaimState, ERoomRewardClaimState::Unclaimed);
	Test.TestTrue(TEXT("the Unclaimed state carries no status line yet"), Unclaimed.StatusText.IsEmpty());
	Test.TestEqual(TEXT("the view model carries the draft XP (design 50)"), Unclaimed.XP, URewardService::RewardXPPerClear);
	Test.TestTrue(TEXT("the reward line names the XP"), Unclaimed.RewardText.Contains(TEXT("XP 50")));
	Test.TestEqual(TEXT("the view model carries one line per draft item"),
		Unclaimed.Items.Num(), Begin.Draft.Items.Num());
	if (Begin.Draft.Items.Num() > 0 && Unclaimed.Items.Num() == Begin.Draft.Items.Num())
	{
		const FItemDefinition* Def = Catalog.Find(Begin.Draft.Items[0].DefinitionId);
		Test.TestTrue(TEXT("the reward line names the item's definition display name"),
			Def != nullptr && Unclaimed.RewardText.Contains(Def->DisplayName));
	}

	// The outcome -> state mapping (pure).
	FRewardClaimAtomicOutcome Outcome; // default RejectedNoProfile
	Test.TestEqual(TEXT("a rejected claim (no profile) derives Failed"),
		DeriveRoomRewardClaimState(Outcome), ERoomRewardClaimState::Failed);

	Outcome = FRewardClaimAtomicOutcome();
	Outcome.Result = ERewardClaimAtomicResult::Claimed;
	Test.TestEqual(TEXT("a committed claim derives Claimed"),
		DeriveRoomRewardClaimState(Outcome), ERoomRewardClaimState::Claimed);

	Outcome = FRewardClaimAtomicOutcome();
	Outcome.Result = ERewardClaimAtomicResult::PartiallyClaimed;
	Test.TestEqual(TEXT("a partial claim derives Claimed"),
		DeriveRoomRewardClaimState(Outcome), ERoomRewardClaimState::Claimed);

	Outcome = FRewardClaimAtomicOutcome();
	Outcome.Result = ERewardClaimAtomicResult::AlreadyClaimed;
	Test.TestEqual(TEXT("an already-claimed answer derives Claimed (persisted truth, not a fake)"),
		DeriveRoomRewardClaimState(Outcome), ERoomRewardClaimState::Claimed);

	Outcome = FRewardClaimAtomicOutcome();
	Outcome.Result = ERewardClaimAtomicResult::InventoryFull;
	Test.TestEqual(TEXT("a full-inventory claim derives InventoryFull"),
		DeriveRoomRewardClaimState(Outcome), ERoomRewardClaimState::InventoryFull);

	Outcome = FRewardClaimAtomicOutcome();
	Outcome.Result = ERewardClaimAtomicResult::SaveFailed;
	Outcome.Error = TEXT("test save failure");
	Test.TestEqual(TEXT("a save failure derives Failed"),
		DeriveRoomRewardClaimState(Outcome), ERoomRewardClaimState::Failed);

	Outcome = FRewardClaimAtomicOutcome();
	Outcome.Result = ERewardClaimAtomicResult::UnknownSettlement;
	Test.TestEqual(TEXT("an unknown settlement derives Failed"),
		DeriveRoomRewardClaimState(Outcome), ERoomRewardClaimState::Failed);

	Outcome = FRewardClaimAtomicOutcome();
	Outcome.Result = ERewardClaimAtomicResult::RejectedNoSaveService;
	Test.TestEqual(TEXT("a missing save service derives Failed"),
		DeriveRoomRewardClaimState(Outcome), ERoomRewardClaimState::Failed);

	// Apply: a committed claim names the stored items, the snapshot stays.
	FRewardClaimAtomicOutcome Saved;
	Saved.Result = ERewardClaimAtomicResult::Claimed;
	Saved.ClaimedItemCount = 1;
	Saved.bGrantedXP = true;
	Saved.bSaveCommitted = true;
	const FRoomRewardViewModel Claimed = ApplyRoomRewardClaimOutcome(Unclaimed, Saved);
	Test.TestEqual(TEXT("the applied view model is Claimed"), Claimed.ClaimState, ERoomRewardClaimState::Claimed);
	Test.TestTrue(TEXT("the claimed status names the inventory move"),
		Claimed.StatusText.Contains(TEXT("entered the inventory")));
	Test.TestEqual(TEXT("the claimed snapshot keeps the draft items"), Claimed.Items.Num(), Unclaimed.Items.Num());
	if (Claimed.Items.Num() == Unclaimed.Items.Num() && Unclaimed.Items.Num() > 0)
	{
		Test.TestEqual(TEXT("the claimed snapshot keeps the item name"),
			Claimed.Items[0].DisplayName, Unclaimed.Items[0].DisplayName);
	}

	// Apply: the full bag shows the shared full-bag prompt.
	FRewardClaimAtomicOutcome Full;
	Full.Result = ERewardClaimAtomicResult::InventoryFull;
	Full.RetainedItemCount = 1;
	Full.bGrantedXP = true;
	Full.bSaveCommitted = true;
	const FRoomRewardViewModel FullVM = ApplyRoomRewardClaimOutcome(Unclaimed, Full);
	Test.TestEqual(TEXT("the applied view model is InventoryFull"), FullVM.ClaimState, ERoomRewardClaimState::InventoryFull);
	Test.TestEqual(TEXT("the full-bag status is the shared prompt"),
		FullVM.StatusText, MakeRewardInventoryFullText());
	Test.TestTrue(TEXT("the full-bag prompt is readable"), !FullVM.StatusText.IsEmpty());

	// Apply: a save failure names the reason and never a completion.
	FRewardClaimAtomicOutcome SaveFailed;
	SaveFailed.Result = ERewardClaimAtomicResult::SaveFailed;
	SaveFailed.Error = TEXT("the claim-snapshot save failed (test injection)");
	const FRoomRewardViewModel FailedVM = ApplyRoomRewardClaimOutcome(Unclaimed, SaveFailed);
	Test.TestEqual(TEXT("the applied view model is Failed"), FailedVM.ClaimState, ERoomRewardClaimState::Failed);
	Test.TestTrue(TEXT("the failed status keeps the reward retryable"),
		FailedVM.StatusText.Contains(TEXT("stays pending for retry")));
	Test.TestTrue(TEXT("the failed status carries the reason"), FailedVM.StatusText.Contains(SaveFailed.Error));
	Test.TestTrue(TEXT("the failed status invents no completion"),
		!FailedVM.StatusText.Contains(TEXT("entered the inventory")));
	return true;
}

// 2. Acceptance: the SAME settlement re-opened any number of times shows the
//    SAME items - a repeat BeginReward answers AlreadyApplied with the
//    ORIGINAL draft (no re-roll), the view models are identical and the widget
//    rebinding renders the exact same reward line.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_018SameSettlementReopenShowsIdenticalDraftItems,
	"UEMMO.Tasks.M3_018.SameSettlementReopenShowsIdenticalDraftItems",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_018SameSettlementReopenShowsIdenticalDraftItems::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_018_ProfileFixture Fixture;
	if (!Fixture.Setup(Test))
	{
		return true;
	}
	const FItemDefinitionCatalog Catalog = M3_018_MakeCatalog();

	const FRoomResult Result = M3_018_MakeResult(/*RunId*/ 18202, /*SettlementId*/ 18202, /*Seed*/ 182021, true);
	const FRewardBeginOutcome First = Fixture.Reward->BeginReward(Result, Catalog);
	Test.TestEqual(TEXT("the first settlement begins with a draft"), First.Result, ERewardBeginResult::Applied);
	if (First.Result != ERewardBeginResult::Applied || First.Draft.Items.Num() < 1)
	{
		return true;
	}

	// Re-open the same settlement twice more: idempotent, original draft.
	const FRewardBeginOutcome Second = Fixture.Reward->BeginReward(Result, Catalog);
	const FRewardBeginOutcome Third = Fixture.Reward->BeginReward(Result, Catalog);
	Test.TestEqual(TEXT("the second settlement answer is AlreadyApplied"), Second.Result, ERewardBeginResult::AlreadyApplied);
	Test.TestEqual(TEXT("the third settlement answer is AlreadyApplied"), Third.Result, ERewardBeginResult::AlreadyApplied);
	Test.TestTrue(TEXT("the second answer carries the ORIGINAL draft (field-for-field, no re-roll)"),
		M3_018_DraftsEqual(Second.Draft, First.Draft));
	Test.TestTrue(TEXT("the third answer carries the ORIGINAL draft"),
		M3_018_DraftsEqual(Third.Draft, First.Draft));
	Test.TestEqual(TEXT("no extra draft was stored"), Fixture.Profile->GetPendingRewards().Num(), 1);

	// The display view models built from each returned draft are identical.
	const FRoomRewardViewModel VM1 = MakeRoomRewardViewModelFromDraft(First.Draft, &Catalog);
	const FRoomRewardViewModel VM2 = MakeRoomRewardViewModelFromDraft(Second.Draft, &Catalog);
	const FRoomRewardViewModel VM3 = MakeRoomRewardViewModelFromDraft(Third.Draft, &Catalog);
	Test.TestTrue(TEXT("the first view model is valid"), VM1.bValid);
	Test.TestEqual(TEXT("the repeated-open view models show the identical reward line"), VM2.RewardText, VM1.RewardText);
	Test.TestEqual(TEXT("the third open shows the identical reward line"), VM3.RewardText, VM1.RewardText);
	if (VM1.Items.Num() == VM2.Items.Num() && VM1.Items.Num() > 0)
	{
		Test.TestEqual(TEXT("the repeated-open item name is identical"), VM2.Items[0].DisplayName, VM1.Items[0].DisplayName);
		Test.TestEqual(TEXT("the repeated-open item stats are identical"), VM2.Items[0].StatsText, VM1.Items[0].StatsText);
	}

	// The widget rebinding renders the same line on every presentation.
	FTestWorldWrapper Wrapper;
	UWorld* World = M3_018_MakeWidgetWorld(Test, Wrapper);
	if (!Test.TestNotNull(TEXT("the widget test world is available"), World))
	{
		return true;
	}
	URoomResultWidget* Widget = CreateWidget<URoomResultWidget>(World, URoomResultWidget::StaticClass());
	if (!Test.TestNotNull(TEXT("the native result widget is created"), Widget))
	{
		return true;
	}
	Widget->BindResult(Result);
	Widget->BindReward(VM1);
	if (!Test.TestNotNull(TEXT("the reward block exists"), Widget->PeekRewardBlock())
		|| !Test.TestNotNull(TEXT("the claim button exists"), Widget->PeekClaimButton()))
	{
		return true;
	}
	Test.TestEqual(TEXT("the reward row shows the draft line"),
		Widget->PeekRewardBlock()->GetText().ToString(), VM1.RewardText);
	Test.TestTrue(TEXT("the reward row is visible while the reward is pending"),
		Widget->PeekRewardBlock()->GetVisibility() != ESlateVisibility::Collapsed);
	Test.TestTrue(TEXT("the claim button starts enabled"), Widget->PeekClaimButton()->GetIsEnabled());

	Widget->BindReward(VM2);
	Test.TestEqual(TEXT("the re-opened screen shows the identical reward line"),
		Widget->PeekRewardBlock()->GetText().ToString(), VM1.RewardText);
	Widget->BindReward(VM3);
	Test.TestEqual(TEXT("the third presentation shows the identical reward line"),
		Widget->PeekRewardBlock()->GetText().ToString(), VM1.RewardText);
	return true;
}

// 3. Acceptance: a Failed run result never shows a reward area: BeginReward
//    rejects it, the M2 reward line stays empty and the widget collapses the
//    whole reward area (row + claim button) for an invalid reward view model.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_018FailedResultShowsNoRewardArea,
	"UEMMO.Tasks.M3_018.FailedResultShowsNoRewardArea",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_018FailedResultShowsNoRewardArea::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_018_ProfileFixture Fixture;
	if (!Fixture.Setup(Test))
	{
		return true;
	}
	const FItemDefinitionCatalog Catalog = M3_018_MakeCatalog();

	// The service rejects a Failed result before anything else happens.
	const FRoomResult Failed = M3_018_MakeResult(/*RunId*/ 18303, /*SettlementId*/ 18303, /*Seed*/ 183031, false);
	const FRewardBeginOutcome Begin = Fixture.Reward->BeginReward(Failed, Catalog);
	Test.TestEqual(TEXT("a failed result is rejected by BeginReward"),
		Begin.Result, ERewardBeginResult::RejectedFailedResult);
	Test.TestTrue(TEXT("the rejection names the reason"), !Begin.Error.IsEmpty());
	Test.TestEqual(TEXT("no draft was stored for the failed result"),
		Fixture.Profile->GetPendingRewards().Num(), 0);

	// The M2 contract stays intact: the result view model invents no reward.
	const FRoomResultViewModel ResultVM = MakeRoomResultViewModel(Failed);
	Test.TestTrue(TEXT("the defeat view model carries no reward line"), ResultVM.RewardText.IsEmpty());

	// The widget collapses the whole reward area for an invalid view model.
	FTestWorldWrapper Wrapper;
	UWorld* World = M3_018_MakeWidgetWorld(Test, Wrapper);
	if (!Test.TestNotNull(TEXT("the widget test world is available"), World))
	{
		return true;
	}
	URoomResultWidget* Widget = CreateWidget<URoomResultWidget>(World, URoomResultWidget::StaticClass());
	if (!Test.TestNotNull(TEXT("the native result widget is created"), Widget))
	{
		return true;
	}
	Widget->BindResult(Failed);
	Widget->BindReward(FRoomRewardViewModel());
	if (!Test.TestNotNull(TEXT("the reward block exists"), Widget->PeekRewardBlock())
		|| !Test.TestNotNull(TEXT("the claim button exists"), Widget->PeekClaimButton()))
	{
		return true;
	}
	Test.TestTrue(TEXT("the defeat screen keeps the reward row collapsed"),
		Widget->PeekRewardBlock()->GetVisibility() == ESlateVisibility::Collapsed);
	Test.TestTrue(TEXT("the defeat screen keeps the claim button collapsed"),
		Widget->PeekClaimButton()->GetVisibility() == ESlateVisibility::Collapsed);

	// Even a victory presentation without a draft shows no reward area.
	FRoomResult Victory = Failed;
	Victory.bCleared = true;
	Widget->BindResult(Victory);
	Widget->BindReward(FRoomRewardViewModel());
	Test.TestTrue(TEXT("a draft-less victory keeps the reward row collapsed"),
		Widget->PeekRewardBlock()->GetVisibility() == ESlateVisibility::Collapsed);
	Test.TestTrue(TEXT("a draft-less victory keeps the claim button collapsed"),
		Widget->PeekClaimButton()->GetVisibility() == ESlateVisibility::Collapsed);
	return true;
}

// 4. Acceptance: a save failure surfaces a readable error, NEVER a fake
//    completion, and the reward stays claimable: the Saving state shows while
//    the claim is in flight, the failed outcome re-arms the Claim button and
//    the retry on the healthy storage claims the SAME draft exactly once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_018SaveFailureShowsReadableErrorAndStaysRetryable,
	"UEMMO.Tasks.M3_018.SaveFailureShowsReadableErrorAndStaysRetryable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_018SaveFailureShowsReadableErrorAndStaysRetryable::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_018_MemoryStorage Storage;
	FM3_018_ProfileFixture Fixture;
	if (!Fixture.Setup(Test))
	{
		return true;
	}
	UProfileSaveService* Service = M3_018_NewService(Test, &Storage);
	if (!Service)
	{
		return true;
	}
	const FItemDefinitionCatalog Catalog = M3_018_MakeCatalog();

	const FRoomResult Result = M3_018_MakeResult(/*RunId*/ 18404, /*SettlementId*/ 18404, /*Seed*/ 184041, true);
	const FRewardBeginOutcome Begin = Fixture.Reward->BeginReward(Result, Catalog);
	Test.TestEqual(TEXT("setup: the settlement began with a pending draft"), Begin.Result, ERewardBeginResult::Applied);
	if (Begin.Result != ERewardBeginResult::Applied || Begin.Draft.Items.Num() < 1)
	{
		M3_018_CleanupTestSlots(Test, *Service);
		return true;
	}
	const FItemInstance Original = Begin.Draft.Items[0];
	const int32 XPBefore = Fixture.Profile->GetXP();
	const FRoomRewardViewModel VM = MakeRoomRewardViewModelFromDraft(Begin.Draft, &Catalog);

	// The claim-snapshot save fails (write 3 = the claim slot write; the draft
	// save in writes 1-2 committed). The outcome is a failure - the UI truth.
	FM3_018_FlakyStorage Flaky(&Storage);
	Flaky.FailWriteOrdinal = 3;
	Service->SetStorage(&Flaky);
	const FRewardClaimAtomicOutcome Failed = Fixture.Reward->ClaimPendingAtomic(18404, Fixture.Profile, Service);
	Test.TestEqual(TEXT("the failing save is reported SaveFailed"),
		Failed.Result, ERewardClaimAtomicResult::SaveFailed);
	Test.TestFalse(TEXT("the failed claim committed no claim save"), Failed.bSaveCommitted);
	Test.TestTrue(TEXT("the failed claim names the failing step"), !Failed.Error.IsEmpty());
	Test.TestEqual(TEXT("the failed claim stored nothing"), Failed.ClaimedItemCount, 0);

	// The pure display derivation of the failure.
	const FRoomRewardViewModel FailedVM = ApplyRoomRewardClaimOutcome(VM, Failed);
	Test.TestEqual(TEXT("the failed view model is Failed"), FailedVM.ClaimState, ERoomRewardClaimState::Failed);
	Test.TestTrue(TEXT("the failed status keeps the reward retryable"),
		FailedVM.StatusText.Contains(TEXT("stays pending for retry")));
	Test.TestTrue(TEXT("the failed status invents no completion"),
		!FailedVM.StatusText.Contains(TEXT("entered the inventory")));
	Test.TestEqual(TEXT("the failed snapshot keeps the original item count"), FailedVM.Items.Num(), VM.Items.Num());

	// The widget feedback: Saving while in flight, Failed with a re-armed
	// Claim button afterwards.
	FTestWorldWrapper Wrapper;
	UWorld* World = M3_018_MakeWidgetWorld(Test, Wrapper);
	if (!Test.TestNotNull(TEXT("the widget test world is available"), World))
	{
		M3_018_CleanupTestSlots(Test, *Service);
		return true;
	}
	URoomResultWidget* Widget = CreateWidget<URoomResultWidget>(World, URoomResultWidget::StaticClass());
	if (!Test.TestNotNull(TEXT("the native result widget is created"), Widget))
	{
		M3_018_CleanupTestSlots(Test, *Service);
		return true;
	}
	Widget->BindResult(Result);
	Widget->BindReward(VM);
	if (!Test.TestNotNull(TEXT("the claim button exists"), Widget->PeekClaimButton())
		|| !Test.TestNotNull(TEXT("the reward status block exists"), Widget->PeekRewardStatusBlock()))
	{
		M3_018_CleanupTestSlots(Test, *Service);
		return true;
	}
	Widget->SetRewardClaimSaving();
	Test.TestEqual(TEXT("the in-flight claim shows Saving"),
		Widget->PeekRewardViewModel().ClaimState, ERoomRewardClaimState::Saving);
	Test.TestFalse(TEXT("the Saving state disables the claim button"), Widget->PeekClaimButton()->GetIsEnabled());
	Widget->ApplyRewardClaimOutcome(Failed);
	Test.TestEqual(TEXT("the failed outcome shows Failed"),
		Widget->PeekRewardViewModel().ClaimState, ERoomRewardClaimState::Failed);
	Test.TestTrue(TEXT("the failed status is readable"), !Widget->PeekRewardViewModel().StatusText.IsEmpty());
	Test.TestTrue(TEXT("the failed outcome re-arms the claim button (retry possible)"),
		Widget->PeekClaimButton()->GetIsEnabled());

	// The retry on the healthy storage claims the SAME pre-generated item and
	// the XP exactly once across both attempts.
	Service->SetStorage(&Storage);
	const FRewardClaimAtomicOutcome Retry = Fixture.Reward->ClaimPendingAtomic(18404, Fixture.Profile, Service);
	Test.TestEqual(TEXT("the retry after the save failure succeeds"), Retry.Result, ERewardClaimAtomicResult::Claimed);
	Test.TestTrue(TEXT("the retry committed its snapshot"), Retry.bSaveCommitted);
	Widget->ApplyRewardClaimOutcome(Retry);
	Test.TestEqual(TEXT("the retry outcome shows Claimed"),
		Widget->PeekRewardViewModel().ClaimState, ERoomRewardClaimState::Claimed);
	Test.TestTrue(TEXT("the claimed status names the inventory move"),
		Widget->PeekRewardViewModel().StatusText.Contains(TEXT("entered the inventory")));
	Test.TestFalse(TEXT("a committed claim keeps the claim button disabled"),
		Widget->PeekClaimButton()->GetIsEnabled());
	Test.TestEqual(TEXT("the total XP across failure and retry is exactly the design 50"),
		Fixture.Profile->GetXP() - XPBefore, URewardService::RewardXPPerClear);
	Test.TestEqual(TEXT("the claimed draft is consumed"), Fixture.Profile->GetPendingRewards().Num(), 0);
	M3_018_CleanupTestSlots(Test, *Service);
	return true;
}

// 5. Acceptance: a full inventory keeps the item pending: the claim answers
//    InventoryFull, the pending list and the menu badge show the retained
//    count with the shared full-bag prompt, the profile (GameInstance-level,
//    untouched by the menu return) still owns the ORIGINAL draft afterwards,
//    and a freed slot lets the claim succeed with the XP granted exactly once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_018InventoryFullKeepsPendingAndClaimableAcrossMenuReturn,
	"UEMMO.Tasks.M3_018.InventoryFullKeepsPendingAndClaimableAcrossMenuReturn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_018InventoryFullKeepsPendingAndClaimableAcrossMenuReturn::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_018_MemoryStorage Storage;
	FM3_018_ProfileFixture Fixture;
	if (!Fixture.Setup(Test))
	{
		return true;
	}
	UProfileSaveService* Service = M3_018_NewService(Test, &Storage);
	if (!Service)
	{
		return true;
	}
	const FItemDefinitionCatalog Catalog = M3_018_MakeCatalog();

	// Fill the inventory to capacity, then settle one cleared run.
	const FItemDefinition FillerDef = M3_018_MakeFillerDefinition();
	M3_018_FillInventory(Fixture.Profile->GetInventory(), FillerDef, FInventoryModel::Capacity);
	Test.TestEqual(TEXT("setup: the inventory is full"),
		Fixture.Profile->GetInventory().Count(), FInventoryModel::Capacity);
	const FGuid FirstFillerId = Fixture.Profile->GetInventory().GetAll()[0].InstanceId;

	const FRoomResult Result = M3_018_MakeResult(/*RunId*/ 18505, /*SettlementId*/ 18505, /*Seed*/ 185051, true);
	const FRewardBeginOutcome Begin = Fixture.Reward->BeginReward(Result, Catalog);
	Test.TestEqual(TEXT("setup: the settlement began with a pending draft"), Begin.Result, ERewardBeginResult::Applied);
	if (Begin.Result != ERewardBeginResult::Applied || Begin.Draft.Items.Num() < 1)
	{
		M3_018_CleanupTestSlots(Test, *Service);
		return true;
	}
	const FItemInstance Original = Begin.Draft.Items[0];
	const int32 XPBefore = Fixture.Profile->GetXP();

	// The claim into the full bag: nothing stored, the item retained, the
	// retention state committed.
	const FRewardClaimAtomicOutcome Claim = Fixture.Reward->ClaimPendingAtomic(18505, Fixture.Profile, Service);
	Test.TestEqual(TEXT("the full-bag claim answers InventoryFull"),
		Claim.Result, ERewardClaimAtomicResult::InventoryFull);
	Test.TestEqual(TEXT("the full-bag claim stored nothing"), Claim.ClaimedItemCount, 0);
	Test.TestEqual(TEXT("the full-bag claim retains the item"), Claim.RetainedItemCount, 1);
	Test.TestTrue(TEXT("the first attempt granted the settlement XP"), Claim.bGrantedXP);
	Test.TestTrue(TEXT("the retention state is committed"), Claim.bSaveCommitted);

	// The reward area shows the full-bag prompt.
	const FRoomRewardViewModel VM = MakeRoomRewardViewModelFromDraft(Begin.Draft, &Catalog);
	const FRoomRewardViewModel FullVM = ApplyRoomRewardClaimOutcome(VM, Claim);
	Test.TestEqual(TEXT("the reward area is InventoryFull"), FullVM.ClaimState, ERoomRewardClaimState::InventoryFull);
	Test.TestEqual(TEXT("the reward area shows the shared full-bag prompt"),
		FullVM.StatusText, MakeRewardInventoryFullText());

	// The pending list + the menu badge show the retained count.
	const FPendingRewardListViewModel ListVM = MakePendingRewardListViewModel(
		Fixture.Profile->GetPendingRewards(), &Catalog, /*bInventoryFull*/ true);
	Test.TestTrue(TEXT("the pending list view model is valid"), ListVM.bValid);
	Test.TestEqual(TEXT("the pending list shows one retained draft"), ListVM.Rows.Num(), 1);
	if (ListVM.Rows.Num() == 1)
	{
		Test.TestEqual(TEXT("the pending row carries the settlement id"), ListVM.Rows[0].SettlementId, 18505ull);
		Test.TestTrue(TEXT("the pending row names the settlement"), ListVM.Rows[0].SettlementText.Contains(TEXT("18505")));
		Test.TestTrue(TEXT("the pending row names the pending XP"), ListVM.Rows[0].XPText.Contains(TEXT("XP 50")));
		Test.TestTrue(TEXT("the pending row names the retained item"), ListVM.Rows[0].ItemsText.Contains(VM.Items[0].DisplayName));
	}
	Test.TestEqual(TEXT("the pending list shows the full-bag prompt"), ListVM.StatusText, MakeRewardInventoryFullText());
	Test.TestEqual(TEXT("the menu badge shows the pending count"),
		MakePendingRewardsBadgeText(Fixture.Profile->GetPendingRewards().Num()), FString(TEXT("Pending rewards: 1")));

	// The pending widget renders the retained row and the prompt.
	FTestWorldWrapper Wrapper;
	UWorld* World = M3_018_MakeWidgetWorld(Test, Wrapper);
	if (!Test.TestNotNull(TEXT("the widget test world is available"), World))
	{
		M3_018_CleanupTestSlots(Test, *Service);
		return true;
	}
	UPendingRewardsWidget* PendingWidget = CreateWidget<UPendingRewardsWidget>(World, UPendingRewardsWidget::StaticClass());
	if (!Test.TestNotNull(TEXT("the pending rewards widget is created"), PendingWidget))
	{
		M3_018_CleanupTestSlots(Test, *Service);
		return true;
	}
	PendingWidget->BindPendingRewards(Fixture.Profile->GetPendingRewards(), &Catalog, /*bInventoryFull*/ true);
	Test.TestEqual(TEXT("the pending widget built one row"), PendingWidget->PeekRowCount(), 1);
	if (PendingWidget->PeekStatusBlock() != nullptr)
	{
		Test.TestEqual(TEXT("the pending widget shows the full-bag prompt"),
			PendingWidget->PeekStatusBlock()->GetText().ToString(), MakeRewardInventoryFullText());
	}

	// Return-to-menu semantics: the GameInstance-level profile keeps the draft
	// with the ORIGINAL pre-generated instance (nothing lost, nothing re-rolled).
	Test.TestEqual(TEXT("the profile still owns the pending draft"), Fixture.Profile->GetPendingRewards().Num(), 1);
	if (Fixture.Profile->GetPendingRewards().Num() == 1 && Fixture.Profile->GetPendingRewards()[0].Items.Num() == 1)
	{
		Test.TestTrue(TEXT("the retained item is field-for-field the ORIGINAL"),
			M3_018_InstancesEqual(Fixture.Profile->GetPendingRewards()[0].Items[0], Original));
	}

	// Free one slot (the bag was emptied on the inventory screen) and claim
	// again: the SAME instance enters, no XP again, the badge clears.
	const EInventoryRemoveResult Removed = Fixture.Profile->GetInventory().Remove(FirstFillerId);
	Test.TestEqual(TEXT("one freed slot makes room"), Removed, EInventoryRemoveResult::Removed);
	const FRewardClaimAtomicOutcome Reclaim = Fixture.Reward->ClaimPendingAtomic(18505, Fixture.Profile, Service);
	Test.TestEqual(TEXT("the re-claim after freeing a slot succeeds"), Reclaim.Result, ERewardClaimAtomicResult::Claimed);
	Test.TestEqual(TEXT("the re-claim stored exactly one item"), Reclaim.ClaimedItemCount, 1);
	Test.TestFalse(TEXT("the re-claim grants no XP again"), Reclaim.bGrantedXP);
	Test.TestEqual(TEXT("the total XP across both attempts is exactly the design 50"),
		Fixture.Profile->GetXP() - XPBefore, URewardService::RewardXPPerClear);
	Test.TestEqual(TEXT("the draft is consumed"), Fixture.Profile->GetPendingRewards().Num(), 0);
	Test.TestTrue(TEXT("the stored item is the ORIGINAL instance"),
		Fixture.Profile->GetInventory().Contains(Original.InstanceId));

	// The menu badge and the pending list reflect the cleared state.
	Test.TestTrue(TEXT("the menu badge is empty with nothing pending"),
		MakePendingRewardsBadgeText(Fixture.Profile->GetPendingRewards().Num()).IsEmpty());
	const FPendingRewardListViewModel ClearedList = MakePendingRewardListViewModel(
		Fixture.Profile->GetPendingRewards(), &Catalog, false);
	Test.TestTrue(TEXT("the cleared pending list is empty"), ClearedList.IsEmpty());
	M3_018_CleanupTestSlots(Test, *Service);
	return true;
}

// 6. Acceptance: the Claim request path is one-shot: the widget handler
//    broadcasts exactly once per armed presentation and disables the button
//    (the visible anti-double-click half); the HUD-side one-shot guard is the
//    M2-012 FRoomResultActionGuard reuse (first request accepted, duplicate
//    dropped and counted, re-armed presentation accepts again).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_018ClaimButtonGuardsFastDoubleClicks,
	"UEMMO.Tasks.M3_018.ClaimButtonGuardsFastDoubleClicks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_018ClaimButtonGuardsFastDoubleClicks::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;

	// A manual draft snapshot (display data only - no service needed here).
	const FItemDefinitionCatalog Catalog = M3_018_MakeCatalog();
	const FItemDefinition* Weapon = Catalog.Find(FName(TEXT("weapon_training")));
	if (!Test.TestNotNull(TEXT("setup: the weapon definition resolves"), Weapon))
	{
		return true;
	}
	FPendingReward Draft;
	Draft.SettlementId = 18606;
	Draft.XP = URewardService::RewardXPPerClear;
	Draft.Items.Add(MakeItemInstance(*Weapon, /*RollSeed*/ 186));

	FTestWorldWrapper Wrapper;
	UWorld* World = M3_018_MakeWidgetWorld(Test, Wrapper);
	if (!Test.TestNotNull(TEXT("the widget test world is available"), World))
	{
		return true;
	}
	URoomResultWidget* Widget = CreateWidget<URoomResultWidget>(World, URoomResultWidget::StaticClass());
	if (!Test.TestNotNull(TEXT("the native result widget is created"), Widget))
	{
		return true;
	}
	FRoomResult Victory;
	Victory.RunId = 18606;
	Victory.SettlementId = 18606;
	Victory.bCleared = true;
	Victory.KilledCount = 3;
	Victory.ElapsedSeconds = 9.0;
	Widget->BindResult(Victory);
	Widget->BindReward(MakeRoomRewardViewModelFromDraft(Draft, &Catalog));
	if (!Test.TestNotNull(TEXT("the claim button exists"), Widget->PeekClaimButton()))
	{
		return true;
	}
	Test.TestTrue(TEXT("the bound reward arms the claim button"), Widget->PeekClaimButton()->GetIsEnabled());

	// The fast double click at the click-entry level.
	int32 BroadcastCount = 0;
	Widget->RewardClaimRequested.AddLambda([&BroadcastCount]()
	{
		++BroadcastCount;
	});
	Widget->HandleClaimButtonClicked();
	Widget->HandleClaimButtonClicked();
	Test.TestEqual(TEXT("the double click broadcast the claim request exactly once"), BroadcastCount, 1);
	Test.TestFalse(TEXT("the first click disabled the claim button"), Widget->PeekClaimButton()->GetIsEnabled());
	Test.TestEqual(TEXT("the widget state stays Unclaimed (the HUD pushes the claim flow from the request)"),
		Widget->PeekRewardViewModel().ClaimState, ERoomRewardClaimState::Unclaimed);

	// The HUD-side machine half: the reused M2-012 one-shot guard.
	FRoomResultActionGuard Guard;
	Guard.ReArm();
	Test.TestTrue(TEXT("the re-armed guard accepts the first request"), Guard.TryAccept());
	Test.TestFalse(TEXT("the guard drops the duplicate of a fast double click"), Guard.TryAccept());
	Test.TestEqual(TEXT("the guard counted one accepted request"), Guard.GetAcceptedCount(), 1);
	Test.TestEqual(TEXT("the guard counted the dropped duplicate"), Guard.GetRejectedCount(), 1);
	Guard.ReArm();
	Test.TestTrue(TEXT("a re-armed presentation accepts one new request"), Guard.TryAccept());
	return true;
}

// 7. Acceptance: the pending rewards widget renders one row per draft with the
//    settlement id, the pending XP and the item names; missing definitions
//    degrade to readable placeholders (no crash), an item-less draft shows an
//    explicit "(no items)" line, the empty list shows the empty state and the
//    full-bag state shows the shared prompt.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_018PendingRewardsWidgetSmokeRowsPlaceholdersAndFullBag,
	"UEMMO.Tasks.M3_018.PendingRewardsWidgetSmokeRowsPlaceholdersAndFullBag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_018PendingRewardsWidgetSmokeRowsPlaceholdersAndFullBag::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;

	FTestWorldWrapper Wrapper;
	UWorld* World = M3_018_MakeWidgetWorld(Test, Wrapper);
	if (!Test.TestNotNull(TEXT("the widget test world is available"), World))
	{
		return true;
	}
	UPendingRewardsWidget* Widget = CreateWidget<UPendingRewardsWidget>(World, UPendingRewardsWidget::StaticClass());
	if (!Test.TestNotNull(TEXT("the native pending rewards widget is created (no UMG asset)"), Widget))
	{
		return true;
	}

	const FItemDefinitionCatalog Catalog = M3_018_MakeCatalog();
	const FItemDefinition* Weapon = Catalog.Find(FName(TEXT("weapon_training")));
	if (!Test.TestNotNull(TEXT("setup: the weapon definition resolves"), Weapon))
	{
		return true;
	}

	// Three drafts: a known definition, a stale unknown one, an item-less one.
	FPendingReward Known;
	Known.SettlementId = 18701;
	Known.XP = URewardService::RewardXPPerClear;
	Known.Items.Add(MakeItemInstance(*Weapon, /*RollSeed*/ 201));

	FPendingReward Stale;
	Stale.SettlementId = 18702;
	Stale.XP = URewardService::RewardXPPerClear;
	FItemInstance StaleInstance = MakeItemInstance(*Weapon, /*RollSeed*/ 202);
	StaleInstance.DefinitionId = FName(TEXT("stale_missing_definition"));
	Stale.Items.Add(StaleInstance);

	FPendingReward Itemless;
	Itemless.SettlementId = 18703;
	Itemless.XP = URewardService::RewardXPPerClear;

	TArray<FPendingReward> Drafts;
	Drafts.Add(Known);
	Drafts.Add(Stale);
	Drafts.Add(Itemless);

	Widget->BindPendingRewards(Drafts, &Catalog, /*bInventoryFull*/ false);
	if (!Test.TestNotNull(TEXT("the title block exists"), Widget->PeekTitleBlock())
		|| !Test.TestNotNull(TEXT("the empty block exists"), Widget->PeekEmptyBlock())
		|| !Test.TestNotNull(TEXT("the status block exists"), Widget->PeekStatusBlock()))
	{
		return true;
	}
	Test.TestEqual(TEXT("the title names the pending list"),
		Widget->PeekTitleBlock()->GetText().ToString(), FString(TEXT("Pending Rewards")));
	Test.TestEqual(TEXT("one row per pending draft was built"), Widget->PeekRowCount(), 3);
	if (Widget->PeekRowText(0) != nullptr)
	{
		const FString Row0 = Widget->PeekRowText(0)->GetText().ToString();
		Test.TestTrue(TEXT("the known row names the settlement"), Row0.Contains(TEXT("18701")));
		Test.TestTrue(TEXT("the known row names the pending XP"), Row0.Contains(TEXT("XP 50")));
		Test.TestTrue(TEXT("the known row names the item"), Row0.Contains(TEXT("Training Sword")));
	}
	if (Widget->PeekRowText(1) != nullptr)
	{
		const FString Row1 = Widget->PeekRowText(1)->GetText().ToString();
		Test.TestTrue(TEXT("the stale row names the settlement"), Row1.Contains(TEXT("18702")));
		Test.TestTrue(TEXT("the stale definition degrades to the readable placeholder"),
			Row1.Contains(TEXT("<unknown item>")));
	}
	if (Widget->PeekRowText(2) != nullptr)
	{
		Test.TestTrue(TEXT("the item-less draft shows the explicit placeholder"),
			Widget->PeekRowText(2)->GetText().ToString().Contains(TEXT("(no items)")));
	}
	Test.TestTrue(TEXT("a not-full bag shows no status prompt"), Widget->PeekViewModel().StatusText.IsEmpty());

	// The full-bag presentation shows the shared prompt.
	Widget->BindPendingRewards(Drafts, &Catalog, /*bInventoryFull*/ true);
	Test.TestEqual(TEXT("the full-bag status is the shared prompt"),
		Widget->PeekViewModel().StatusText, MakeRewardInventoryFullText());
	if (Widget->PeekStatusBlock() != nullptr)
	{
		Test.TestEqual(TEXT("the status block carries the prompt"),
			Widget->PeekStatusBlock()->GetText().ToString(), MakeRewardInventoryFullText());
		Test.TestTrue(TEXT("the status block is visible while the bag is full"),
			Widget->PeekStatusBlock()->GetVisibility() != ESlateVisibility::Collapsed);
	}

	// The empty presentation shows the empty state.
	Widget->BindPendingRewards(TArray<FPendingReward>(), &Catalog, false);
	Test.TestTrue(TEXT("the empty list view model is empty"), Widget->PeekViewModel().IsEmpty());
	Test.TestEqual(TEXT("no rows were built for the empty list"), Widget->PeekRowCount(), 0);
	if (Widget->PeekEmptyBlock() != nullptr)
	{
		Test.TestTrue(TEXT("the empty state is visible for an empty list"),
			Widget->PeekEmptyBlock()->GetVisibility() != ESlateVisibility::Collapsed);
		Test.TestTrue(TEXT("the empty state is readable"),
			!Widget->PeekEmptyBlock()->GetText().ToString().IsEmpty());
	}

	// The pure list view model agrees with the widget (placeholder + no crash
	// even without a catalog).
	const FPendingRewardListViewModel NullCatalogVM = MakePendingRewardListViewModel(Drafts, nullptr, false);
	Test.TestTrue(TEXT("the catalog-less list view model is valid"), NullCatalogVM.bValid);
	Test.TestEqual(TEXT("the catalog-less list keeps every row"), NullCatalogVM.Rows.Num(), 3);
	if (NullCatalogVM.Rows.Num() == 3)
	{
		Test.TestTrue(TEXT("the catalog-less known row degrades to the placeholder too"),
			NullCatalogVM.Rows[0].ItemsText.Contains(TEXT("<unknown item>")));
	}
	return true;
}

#endif
