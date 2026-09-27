// M3-016: atomic commit of a reward claim and the profile save (interface
// contract section 8: the XP, the inventory items, the PendingRewards change
// and AppliedSettlementIds travel as ONE profile snapshot; the UI may confirm
// the claim only after that save committed). Pins URewardService::
// ClaimPendingAtomic end to end, always on the dedicated "TestSlot_" prefix:
// - the ordered flow: persist the pending draft FIRST ("StartRoom
//   semantics"), then claim in memory from that draft (XP exactly once,
//   original InstanceIds), then persist the post-claim snapshot;
// - the three interrupt windows of the claim-snapshot save are each injected
//   once - before the slot write, after the slot write (index not committed),
//   at the index commit (torn index) - and every restart (a NEW save service
//   + StartupLoad + RestoreFromSave into a FRESH profile, so the claim
//   idempotency is decided from persisted state alone) ends with the total XP
//   grown by exactly the design 50 and exactly one equipment piece (the
//   ORIGINAL InstanceId, no duplicate, no re-roll);
// - a full-inventory claim retains the pending item and that retained draft
//   survives a restart field-for-field (same InstanceId, same one-time roll);
// - a save failure never confirms the claim: the returned result is SaveFailed
//   and the in-memory claim is rolled back to the last committed state;
// - the normal path commits and reloads: a replayed BeginReward answers
//   AlreadyApplied from the persisted applied record and a replayed claim
//   answers AlreadyClaimed.
//
// Harness note: the storage doubles and fixture helpers are file-local copies
// with M3_016 names (SaveTransactionTests.cpp / SaveRecoveryTests.cpp /
// PendingRewardTests.cpp are never modified and every suite stays uniquely
// linkable). The profile fixtures are bare UGameInstance + UProfileSubsystem
// objects (the M3-015 precedent): ClaimPendingAtomic takes the profile and
// the save service explicitly, so no world or subsystem collection is needed.
//
// Stub-failure note: against the red stub (ClaimPendingAtomic always answers
// RejectedNoProfile and never saves) every test below fails on its concrete
// expectations - nothing is ever persisted, so the restart recovery and every
// claim assertion fail.

#include "Misc/AutomationTest.h"

#include "../Persistence/ProfileSaveService.h"
#include "../Profile/ProfileSubsystem.h"
#include "../Profile/RewardService.h"
#include "../Items/InventoryModel.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../Room/RoomResult.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_016
{
	// -- Test storage doubles ---------------------------------------------------

	/**
	 * In-memory ISaveStorage: every written slot is stored as an independent
	 * duplicate (rooted against GC), every read returns a fresh duplicate -
	 * the same semantics as disk bytes. Counts WriteSlot calls so the crash
	 * windows can be pinned to exact write ordinals.
	 */
	class FM3_016_MemoryStorage final : public ISaveStorage
	{
	public:
		virtual ~FM3_016_MemoryStorage() override
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
			++WriteCallCount;
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

		/** Number of WriteSlot invocations (slot writes AND index commits). */
		int32 WriteCallCount = 0;

	private:
		TMap<FString, USaveGame*> Slots;
	};

	/**
	 * Crash / failure injection decorator over another storage. The atomic
	 * claim performs exactly two saves (draft persistence, claim snapshot),
	 * each = one slot write + one index commit, so with a fresh prefix the
	 * write ordinals are: 1 = draft slot, 2 = draft index, 3 = claim-snapshot
	 * slot, 4 = claim-snapshot index. The card's three interrupt windows are
	 * modeled on the claim-snapshot save:
	 * - FailWriteOrdinal = 3: the crash lands BEFORE the claim-snapshot slot
	 *   write (nothing of it reaches storage);
	 * - FailWriteOrdinal = 4: the crash lands AFTER the slot write and BEFORE
	 *   the index commit (uncommitted slot content lingers, the index stays);
	 * - TearIndexWriteOrdinal = 4: the crash lands AT the index commit - the
	 *   index file ends up garbage (a torn write the storage accepted), which
	 *   the startup pass must survive via its generation scan.
	 * bFailAllWrites models a plain unavailable storage (the save-failure
	 * test). Refused calls still consume an ordinal.
	 */
	class FM3_016_CrashStorage final : public ISaveStorage
	{
	public:
		explicit FM3_016_CrashStorage(ISaveStorage* InInner)
			: Inner(InInner)
		{
		}

		virtual bool WriteSlot(const FString& SlotName, USaveGame* Data) override
		{
			++WriteCounter;
			if (bFailAllWrites)
			{
				return false;
			}
			if (FailWriteOrdinal > 0 && WriteCounter == FailWriteOrdinal)
			{
				return false;
			}
			if (TearIndexWriteOrdinal > 0 && WriteCounter == TearIndexWriteOrdinal)
			{
				// The torn index commit: the file exists but its content is
				// garbage (invalid active slot, wrong array shape).
				UProfileIndexSaveGame* Torn = NewObject<UProfileIndexSaveGame>(GetTransientPackage());
				Torn->ActiveSlotIndex = 7;
				Torn->SlotGenerations.Add(1);
				return Inner->WriteSlot(SlotName, Torn);
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

		/** The 1-based write ordinal whose written index content is replaced by garbage (0 = none). */
		int32 TearIndexWriteOrdinal = 0;

		/** When true, every write fails regardless of the ordinal. */
		bool bFailAllWrites = false;

	private:
		ISaveStorage* Inner = nullptr;
	};

	// -- Fixture helpers ---------------------------------------------------------

	/** The three TestSlot_ slot names every test of this suite operates on. */
	static FString M3_016_SlotAName() { return FString(TEXT("TestSlot_A")); }
	static FString M3_016_SlotBName() { return FString(TEXT("TestSlot_B")); }
	static FString M3_016_IndexSlotName() { return FString(TEXT("TestSlot_Index")); }

	/** Creates the save service on the automation prefix (storage optional). */
	static UProfileSaveService* M3_016_NewService(FAutomationTestBase& Test, ISaveStorage* Storage, const TCHAR* Prefix)
	{
		UProfileSaveService* Service = NewObject<UProfileSaveService>(GetTransientPackage());
		if (!Service)
		{
			Test.AddError(TEXT("setup: NewObject<UProfileSaveService> returned null"));
			return nullptr;
		}
		if (!Service->Initialize(Prefix))
		{
			Test.AddError(FString::Printf(TEXT("setup: Initialize('%s') rejected the prefix"), Prefix));
			return nullptr;
		}
		if (Storage)
		{
			Service->SetStorage(Storage);
		}
		return Service;
	}

	/**
	 * Bare profile fixture (the M3-015 precedent): a UGameInstance owner, a
	 * UProfileSubsystem and a URewardService, all rooted, none Init()ed - no
	 * world is needed because ClaimPendingAtomic takes the profile and the
	 * save service explicitly. Setup mints a fresh level-1 profile and binds
	 * the reward service to it.
	 */
	struct FM3_016_ProfileFixture
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

		~FM3_016_ProfileFixture()
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

	/**
	 * The RESTART state: a brand-new save service over the same storage plus
	 * a brand-new profile whose memory is filled ONLY by RestoreFromSave from
	 * the startup load's recovered save - the claim idempotency of every
	 * restart test is decided from this persisted state alone, never from any
	 * earlier in-memory history.
	 */
	struct FM3_016_RestartedState
	{
		UGameInstance* Owner = nullptr;
		UProfileSaveService* SaveService = nullptr;
		UProfileSubsystem* Profile = nullptr;
		URewardService* Reward = nullptr;
		EStartupLoadResult LoadResult = EStartupLoadResult::RecoveryError;

		bool Setup(FAutomationTestBase& Test, ISaveStorage& Storage, const TCHAR* Scenario)
		{
			SaveService = M3_016_NewService(Test, &Storage, TEXT("TestSlot_"));
			if (!SaveService)
			{
				return false;
			}
			const FStartupLoadOutcome Loaded = SaveService->StartupLoad();
			LoadResult = Loaded.Result;
			if (Loaded.Result != EStartupLoadResult::Recovered && Loaded.Result != EStartupLoadResult::RecoveredFallback)
			{
				Test.AddError(FString::Printf(TEXT("%s: the restart's startup load did not recover a save (result %d: %s)"),
					Scenario, static_cast<int32>(Loaded.Result), *Loaded.Summary));
				return false;
			}
			Owner = NewObject<UGameInstance>(GEngine);
			Profile = Owner ? NewObject<UProfileSubsystem>(Owner) : nullptr;
			Reward = Owner ? NewObject<URewardService>(Owner) : nullptr;
			if (!Test.TestNotNull(TEXT("the restarted profile is constructible"), Profile)
				|| !Test.TestNotNull(TEXT("the restarted reward service is constructible"), Reward))
			{
				return false;
			}
			Owner->AddToRoot();
			Profile->AddToRoot();
			Reward->AddToRoot();
			Reward->BindProfile(Profile);
			Test.TestFalse(TEXT("the restarted profile starts without any in-memory state"), Profile->HasProfile());
			if (!Profile->RestoreFromSave(Loaded.Snapshot, Loaded.Inventory, Loaded.PendingRewards,
				Loaded.AppliedSettlementIds, Loaded.EquippedMap))
			{
				Test.AddError(FString::Printf(TEXT("%s: RestoreFromSave refused the recovered save"), Scenario));
				return false;
			}
			return true;
		}

		~FM3_016_RestartedState()
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

	/** The three training definitions in a fresh catalog (mirrors Data/items.json). */
	static FItemDefinitionCatalog M3_016_MakeTrainingCatalog()
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
	static FRoomResult M3_016_MakeResult(uint64 RunId, uint64 SettlementId, int32 Seed, bool bCleared)
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
	static FItemDefinition M3_016_MakeFillerDefinition()
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
	static TArray<FGuid> M3_016_FillInventory(FInventoryModel& Inventory, const FItemDefinition& Definition, int32 Count)
	{
		TArray<FGuid> FillerIds;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FItemInstance Filler = MakeItemInstance(Definition, /*RollSeed*/ 46000 + Index);
			FillerIds.Add(Filler.InstanceId);
			Inventory.TryAdd(Filler);
		}
		return FillerIds;
	}

	/** Field-for-field instance equality (identity, definition, roll seed, level, stats). */
	static bool M3_016_InstancesEqual(const FItemInstance& A, const FItemInstance& B)
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
	static const FItemInstance* M3_016_FindStored(const FInventoryModel& Inventory, const FGuid& InstanceId)
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
	 * Begins one settlement on the fixture (the real BeginReward flow) and
	 * returns the ORIGINAL pre-generated instance via OutInstance. Returns
	 * false (after TestNotNull-style errors) when the draft is not usable.
	 */
	static bool M3_016_BeginSettlement(FAutomationTestBase& Test, FM3_016_ProfileFixture& Fixture,
		uint64 SettlementId, FItemInstance& OutInstance)
	{
		const FRoomResult Result = M3_016_MakeResult(/*RunId*/ SettlementId, SettlementId, static_cast<int32>(SettlementId) * 10 + 1, true);
		const FRewardBeginOutcome Begin = Fixture.Reward->BeginReward(Result, M3_016_MakeTrainingCatalog());
		Test.TestEqual(TEXT("setup: the settlement began with a pending draft"), Begin.Result, ERewardBeginResult::Applied);
		if (Begin.Result != ERewardBeginResult::Applied || Begin.Draft.Items.Num() != 1)
		{
			return false;
		}
		OutInstance = Begin.Draft.Items[0];
		return true;
	}

	/**
	 * Cleanup: deletes the service's test slots and asserts the three
	 * TestSlot_ names are gone afterwards (tests delete only the explicitly
	 * named test slots and leave everything else alone).
	 */
	static void M3_016_CleanupTestSlots(FAutomationTestBase& Test, UProfileSaveService& Service, ISaveStorage& Storage)
	{
		Service.DeleteTestSlots();
		Test.TestTrue(TEXT("cleanup: TestSlot_A deleted"), !Storage.DoesSlotExist(M3_016_SlotAName()));
		Test.TestTrue(TEXT("cleanup: TestSlot_B deleted"), !Storage.DoesSlotExist(M3_016_SlotBName()));
		Test.TestTrue(TEXT("cleanup: TestSlot_Index deleted"), !Storage.DoesSlotExist(M3_016_IndexSlotName()));
	}
}

using namespace UE::UEMMO::Tasks::M3_016;

// -- Interrupt window 1: crash before the claim-snapshot slot write --------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_016InterruptBeforeClaimSlotWriteGrantsXPOnceAfterRestart,
	"UEMMO.Tasks.M3_016.InterruptBeforeClaimSlotWriteGrantsXPOnceAfterRestart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_016InterruptBeforeClaimSlotWriteGrantsXPOnceAfterRestart::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_016_MemoryStorage Storage;
	FM3_016_ProfileFixture Fixture;
	if (!Fixture.Setup(Test))
	{
		return true;
	}
	UProfileSaveService* Service = M3_016_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	FItemInstance Original;
	if (!M3_016_BeginSettlement(Test, Fixture, /*SettlementId*/ 3101, Original))
	{
		return true;
	}
	const int32 XPBefore = Fixture.Profile->GetXP();

	// Crash window 1 (before the claim-snapshot slot write): writes 1-2 are
	// the draft save (slot + index), write 3 is the claim snapshot's slot
	// write and never completes. The call models the process dying mid-save.
	FM3_016_CrashStorage Crash(&Storage);
	Crash.FailWriteOrdinal = 3;
	Service->SetStorage(&Crash);
	const FRewardClaimAtomicOutcome Interrupted = Fixture.Reward->ClaimPendingAtomic(3101, Fixture.Profile, Service);
	Test.TestEqual(TEXT("the interrupted claim does not confirm success"),
		Interrupted.Result, ERewardClaimAtomicResult::SaveFailed);
	Test.TestFalse(TEXT("the interrupted claim committed no claim save"), Interrupted.bSaveCommitted);

	// RESTART: a new save service over the same storage; the fresh profile
	// memory is restored ONLY from the persisted state.
	FM3_016_RestartedState Restarted;
	if (!Restarted.Setup(Test, Storage, TEXT("before-write restart")))
	{
		M3_016_CleanupTestSlots(Test, *Service, Storage);
		return true;
	}
	Test.TestEqual(TEXT("the restart recovered the index-active save"),
		static_cast<int32>(Restarted.LoadResult), static_cast<int32>(EStartupLoadResult::Recovered));

	// The persisted state is the pre-claim draft state: the draft (with the
	// ORIGINAL instance) survived, no XP was granted, nothing applied.
	Test.TestEqual(TEXT("the restored XP is the pre-claim XP"), Restarted.Profile->GetXP(), XPBefore);
	Test.TestEqual(TEXT("the restored pending draft survived"), Restarted.Profile->GetPendingRewards().Num(), 1);
	if (Restarted.Profile->GetPendingRewards().Num() == 1 && Restarted.Profile->GetPendingRewards()[0].Items.Num() == 1)
	{
		Test.TestTrue(TEXT("the restored draft item is the ORIGINAL pre-generated instance"),
			M3_016_InstancesEqual(Restarted.Profile->GetPendingRewards()[0].Items[0], Original));
	}
	Test.TestFalse(TEXT("the settlement is not recorded as applied yet"), Restarted.Profile->IsSettlementApplied(3101));

	// Re-claim from the persisted state: exactly one grant, exactly the
	// original item, no duplicate.
	const FRewardClaimAtomicOutcome Reclaim = Restarted.Reward->ClaimPendingAtomic(3101, Restarted.Profile, Restarted.SaveService);
	Test.TestEqual(TEXT("the re-claim after the restart succeeds"), Reclaim.Result, ERewardClaimAtomicResult::Claimed);
	Test.TestTrue(TEXT("the re-claim grants the XP"), Reclaim.bGrantedXP);
	Test.TestTrue(TEXT("the re-claim's snapshot save committed"), Reclaim.bSaveCommitted);
	Test.TestEqual(TEXT("the total XP across the interruption is exactly the design 50"),
		Restarted.Profile->GetXP() - XPBefore, URewardService::RewardXPPerClear);
	Test.TestEqual(TEXT("the level stayed 1 (a double grant would have leveled)"), Restarted.Profile->GetLevel(), 1);
	Test.TestEqual(TEXT("exactly one equipment piece was stored"), Restarted.Profile->GetInventory().Count(), 1);
	Test.TestTrue(TEXT("the stored piece is the ORIGINAL instance (InstanceId unchanged)"),
		M3_016_FindStored(Restarted.Profile->GetInventory(), Original.InstanceId) != nullptr);
	Test.TestEqual(TEXT("no pending draft is left"), Restarted.Profile->GetPendingRewards().Num(), 0);
	Test.TestTrue(TEXT("the settlement is recorded as applied"), Restarted.Profile->IsSettlementApplied(3101));

	M3_016_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- Interrupt window 2: crash after the slot write, before the index commit ------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_016InterruptAfterClaimSlotWriteGrantsXPOnceAfterRestart,
	"UEMMO.Tasks.M3_016.InterruptAfterClaimSlotWriteGrantsXPOnceAfterRestart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_016InterruptAfterClaimSlotWriteGrantsXPOnceAfterRestart::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_016_MemoryStorage Storage;
	FM3_016_ProfileFixture Fixture;
	if (!Fixture.Setup(Test))
	{
		return true;
	}
	UProfileSaveService* Service = M3_016_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	FItemInstance Original;
	if (!M3_016_BeginSettlement(Test, Fixture, /*SettlementId*/ 3202, Original))
	{
		return true;
	}
	const int32 XPBefore = Fixture.Profile->GetXP();

	// Crash window 2 (after the claim-snapshot slot write, before the index
	// commit): write 3 (the inactive slot) succeeds, write 4 (the index) is
	// refused. The slot content exists but was NEVER committed.
	FM3_016_CrashStorage Crash(&Storage);
	Crash.FailWriteOrdinal = 4;
	Service->SetStorage(&Crash);
	const FRewardClaimAtomicOutcome Interrupted = Fixture.Reward->ClaimPendingAtomic(3202, Fixture.Profile, Service);
	Test.TestEqual(TEXT("the interrupted claim does not confirm success"),
		Interrupted.Result, ERewardClaimAtomicResult::SaveFailed);
	Test.TestFalse(TEXT("the interrupted claim committed no claim save"), Interrupted.bSaveCommitted);

	// The uncommitted slot content lingers; the index still points at the
	// draft-state save (the commit point never moved).
	Test.TestTrue(TEXT("the uncommitted claim-snapshot slot content exists"),
		Storage.DoesSlotExist(M3_016_SlotBName()));

	// RESTART from the persisted state.
	FM3_016_RestartedState Restarted;
	if (!Restarted.Setup(Test, Storage, TEXT("after-slot-write restart")))
	{
		M3_016_CleanupTestSlots(Test, *Service, Storage);
		return true;
	}
	Test.TestEqual(TEXT("the restart recovered the index-active save (not the uncommitted slot)"),
		static_cast<int32>(Restarted.LoadResult), static_cast<int32>(EStartupLoadResult::Recovered));

	// The persisted state is the pre-claim draft state again.
	Test.TestEqual(TEXT("the restored XP is the pre-claim XP"), Restarted.Profile->GetXP(), XPBefore);
	Test.TestFalse(TEXT("the settlement is not recorded as applied yet"), Restarted.Profile->IsSettlementApplied(3202));
	Test.TestEqual(TEXT("the restored pending draft survived"), Restarted.Profile->GetPendingRewards().Num(), 1);

	// Re-claim: the first save of the re-claim overwrites the stale
	// uncommitted slot, the second commits the claim snapshot.
	const FRewardClaimAtomicOutcome Reclaim = Restarted.Reward->ClaimPendingAtomic(3202, Restarted.Profile, Restarted.SaveService);
	Test.TestEqual(TEXT("the re-claim after the restart succeeds"), Reclaim.Result, ERewardClaimAtomicResult::Claimed);
	Test.TestTrue(TEXT("the re-claim grants the XP"), Reclaim.bGrantedXP);
	Test.TestEqual(TEXT("the total XP across the interruption is exactly the design 50"),
		Restarted.Profile->GetXP() - XPBefore, URewardService::RewardXPPerClear);
	Test.TestEqual(TEXT("the level stayed 1 (a double grant would have leveled)"), Restarted.Profile->GetLevel(), 1);
	Test.TestEqual(TEXT("exactly one equipment piece was stored"), Restarted.Profile->GetInventory().Count(), 1);
	Test.TestTrue(TEXT("the stored piece is the ORIGINAL instance (InstanceId unchanged)"),
		M3_016_FindStored(Restarted.Profile->GetInventory(), Original.InstanceId) != nullptr);

	// The committed state (read fresh from storage) carries the claimed
	// state - the stale uncommitted slot did not resurrect anything.
	const FProfileLoadOutcome Committed = Restarted.SaveService->LoadActiveProfile();
	Test.TestEqual(TEXT("the committed save loads"), static_cast<int32>(Committed.Result), static_cast<int32>(EProfileLoadResult::Success));
	if (Committed.Result == EProfileLoadResult::Success)
	{
		Test.TestEqual(TEXT("the committed XP is the once-granted total"), Committed.Snapshot.XP, XPBefore + URewardService::RewardXPPerClear);
		Test.TestEqual(TEXT("the committed inventory holds exactly one item"), Committed.Inventory.Count(), 1);
		Test.TestTrue(TEXT("the committed applied record survives"), Committed.AppliedSettlementIds.Contains(3202ull));
		Test.TestEqual(TEXT("the committed pending list is empty"), Committed.PendingRewards.Num(), 0);
	}

	M3_016_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- Interrupt window 3: crash at the index commit (torn index) -------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_016InterruptAtIndexCommitRecoversCommittedClaimOnRestart,
	"UEMMO.Tasks.M3_016.InterruptAtIndexCommitRecoversCommittedClaimOnRestart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_016InterruptAtIndexCommitRecoversCommittedClaimOnRestart::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_016_MemoryStorage Storage;
	FM3_016_ProfileFixture Fixture;
	if (!Fixture.Setup(Test))
	{
		return true;
	}
	UProfileSaveService* Service = M3_016_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	FItemInstance Original;
	if (!M3_016_BeginSettlement(Test, Fixture, /*SettlementId*/ 3303, Original))
	{
		return true;
	}
	const int32 XPBefore = Fixture.Profile->GetXP();

	// Crash window 3 (at the index commit): the index file ends up garbage
	// (a torn write the storage accepted), while the verified claim-snapshot
	// payload stays in the inactive slot. The service itself saw a successful
	// index write and confirms the claim - the torn commit is discovered on
	// the next startup pass, whose generation scan must recover the claim.
	FM3_016_CrashStorage Crash(&Storage);
	Crash.TearIndexWriteOrdinal = 4;
	Service->SetStorage(&Crash);
	const FRewardClaimAtomicOutcome CommittedBlind = Fixture.Reward->ClaimPendingAtomic(3303, Fixture.Profile, Service);
	Test.TestEqual(TEXT("the service confirmed the claim from its storage view (the torn index is invisible to it)"),
		CommittedBlind.Result, ERewardClaimAtomicResult::Claimed);
	Test.TestTrue(TEXT("the confirmed claim's snapshot save reported a commit"), CommittedBlind.bSaveCommitted);

	// The index on disk is garbage; the claim payload survives in its slot.
	UProfileIndexSaveGame* RawIndex = Cast<UProfileIndexSaveGame>(Storage.ReadSlot(M3_016_IndexSlotName()));
	Test.TestNotNull(TEXT("the torn index file exists"), RawIndex);
	if (RawIndex)
	{
		Test.TestEqual(TEXT("the torn index is not a usable commit (invalid active slot)"),
			RawIndex->ActiveSlotIndex, 7);
	}

	// RESTART: the startup pass falls back to the generation scan and
	// recovers the committed claim snapshot.
	FM3_016_RestartedState Restarted;
	if (!Restarted.Setup(Test, Storage, TEXT("torn-index restart")))
	{
		M3_016_CleanupTestSlots(Test, *Service, Storage);
		return true;
	}
	Test.TestEqual(TEXT("the restart recovered the claim save by scan (fallback)"),
		static_cast<int32>(Restarted.LoadResult), static_cast<int32>(EStartupLoadResult::RecoveredFallback));

	// The claim survived the torn commit: XP exactly +50, exactly the
	// original piece, applied recorded, draft consumed.
	Test.TestEqual(TEXT("the restored XP is the once-granted total"),
		Restarted.Profile->GetXP(), XPBefore + URewardService::RewardXPPerClear);
	Test.TestEqual(TEXT("exactly one equipment piece was restored"), Restarted.Profile->GetInventory().Count(), 1);
	Test.TestTrue(TEXT("the restored piece is the ORIGINAL instance (InstanceId unchanged)"),
		M3_016_FindStored(Restarted.Profile->GetInventory(), Original.InstanceId) != nullptr);
	Test.TestTrue(TEXT("the restored applied record survives"), Restarted.Profile->IsSettlementApplied(3303));
	Test.TestEqual(TEXT("no pending draft was restored"), Restarted.Profile->GetPendingRewards().Num(), 0);

	// Re-claim after the restart: the idempotency decision comes from the
	// PERSISTED applied record (the fresh profile has no other source) - no
	// second grant, no second item.
	const FRewardClaimAtomicOutcome Reclaim = Restarted.Reward->ClaimPendingAtomic(3303, Restarted.Profile, Restarted.SaveService);
	Test.TestEqual(TEXT("the re-claim answers AlreadyClaimed from the persisted state"),
		Reclaim.Result, ERewardClaimAtomicResult::AlreadyClaimed);
	Test.TestFalse(TEXT("the re-claim grants no XP again"), Reclaim.bGrantedXP);
	Test.TestFalse(TEXT("the re-claim performs no save"), Reclaim.bSaveCommitted);
	Test.TestEqual(TEXT("the total XP stays exactly the design 50"),
		Restarted.Profile->GetXP() - XPBefore, URewardService::RewardXPPerClear);
	Test.TestEqual(TEXT("still exactly one equipment piece (no duplicate)"),
		Restarted.Profile->GetInventory().Count(), 1);

	M3_016_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- Full inventory: the retained pending item survives a restart with its id -----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_016FullInventoryRetainedItemSurvivesRestartWithSameId,
	"UEMMO.Tasks.M3_016.FullInventoryRetainedItemSurvivesRestartWithSameId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_016FullInventoryRetainedItemSurvivesRestartWithSameId::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_016_MemoryStorage Storage;
	FM3_016_ProfileFixture Fixture;
	if (!Fixture.Setup(Test))
	{
		return true;
	}
	UProfileSaveService* Service = M3_016_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	// Fill the inventory to the full 30-slot capacity, then settle one run.
	const FItemDefinition FillerDef = M3_016_MakeFillerDefinition();
	const TArray<FGuid> Fillers = M3_016_FillInventory(Fixture.Profile->GetInventory(), FillerDef, FInventoryModel::Capacity);
	Test.TestEqual(TEXT("setup: the inventory is full"), Fixture.Profile->GetInventory().Count(), FInventoryModel::Capacity);

	FItemInstance Original;
	if (!M3_016_BeginSettlement(Test, Fixture, /*SettlementId*/ 3404, Original))
	{
		return true;
	}
	const int32 XPBefore = Fixture.Profile->GetXP();

	// The atomic claim into the FULL inventory: nothing is stored, the item
	// is retained, and the retention state (XP + applied id + retained draft)
	// is committed as one snapshot.
	const FRewardClaimAtomicOutcome Claim = Fixture.Reward->ClaimPendingAtomic(3404, Fixture.Profile, Service);
	Test.TestEqual(TEXT("the full-bag claim reports InventoryFull (never a claim success)"),
		Claim.Result, ERewardClaimAtomicResult::InventoryFull);
	Test.TestEqual(TEXT("the full-bag claim stored nothing"), Claim.ClaimedItemCount, 0);
	Test.TestEqual(TEXT("the full-bag claim retains the pending item"), Claim.RetainedItemCount, 1);
	Test.TestTrue(TEXT("the first attempt granted the settlement XP"), Claim.bGrantedXP);
	Test.TestTrue(TEXT("the retention state is committed"), Claim.bSaveCommitted);
	Test.TestEqual(TEXT("the in-memory XP grew by the design 50"),
		Fixture.Profile->GetXP() - XPBefore, URewardService::RewardXPPerClear);
	Test.TestTrue(TEXT("the settlement is recorded as applied"), Fixture.Profile->IsSettlementApplied(3404));
	Test.TestEqual(TEXT("the draft stays pending"), Fixture.Profile->GetPendingRewards().Num(), 1);

	// RESTART: the retained draft (with the ORIGINAL item) must survive.
	FM3_016_RestartedState Restarted;
	if (!Restarted.Setup(Test, Storage, TEXT("full-bag restart")))
	{
		M3_016_CleanupTestSlots(Test, *Service, Storage);
		return true;
	}
	Test.TestEqual(TEXT("the restart recovered the committed retention state"),
		static_cast<int32>(Restarted.LoadResult), static_cast<int32>(EStartupLoadResult::Recovered));
	Test.TestEqual(TEXT("the restored pending draft survived the restart"), Restarted.Profile->GetPendingRewards().Num(), 1);
	if (Restarted.Profile->GetPendingRewards().Num() == 1 && Restarted.Profile->GetPendingRewards()[0].Items.Num() == 1)
	{
		Test.TestTrue(TEXT("the retained item survived field-for-field (same InstanceId, same one-time roll)"),
			M3_016_InstancesEqual(Restarted.Profile->GetPendingRewards()[0].Items[0], Original));
	}
	Test.TestEqual(TEXT("the restored XP is the once-granted total"),
		Restarted.Profile->GetXP(), XPBefore + URewardService::RewardXPPerClear);
	Test.TestTrue(TEXT("the restored applied record survives"), Restarted.Profile->IsSettlementApplied(3404));
	Test.TestEqual(TEXT("the restored inventory is still full"), Restarted.Profile->GetInventory().Count(), FInventoryModel::Capacity);

	// Free one slot and re-claim: ONLY the original instance enters, no XP
	// again (the total across both attempts stays the design 50).
	const EInventoryRemoveResult Removed = Restarted.Profile->GetInventory().Remove(Fillers[0]);
	Test.TestEqual(TEXT("one freed slot makes room"), Removed, EInventoryRemoveResult::Removed);
	const FRewardClaimAtomicOutcome Reclaim = Restarted.Reward->ClaimPendingAtomic(3404, Restarted.Profile, Restarted.SaveService);
	Test.TestEqual(TEXT("the re-claim after freeing a slot claims the draft"), Reclaim.Result, ERewardClaimAtomicResult::Claimed);
	Test.TestEqual(TEXT("the re-claim stored exactly one item"), Reclaim.ClaimedItemCount, 1);
	Test.TestFalse(TEXT("the re-claim grants no XP again"), Reclaim.bGrantedXP);
	Test.TestEqual(TEXT("the total XP across both attempts is exactly the design 50"),
		Restarted.Profile->GetXP() - XPBefore, URewardService::RewardXPPerClear);
	Test.TestEqual(TEXT("the inventory is full again, holding the claimed instance"),
		Restarted.Profile->GetInventory().Count(), FInventoryModel::Capacity);
	Test.TestTrue(TEXT("the stored item is the ORIGINAL instance (same InstanceId)"),
		M3_016_FindStored(Restarted.Profile->GetInventory(), Original.InstanceId) != nullptr);
	Test.TestEqual(TEXT("the consumed draft is gone"), Restarted.Profile->GetPendingRewards().Num(), 0);

	M3_016_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- Save failure: the return value never confirms the claim, memory rolls back ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_016SaveFailureNeverConfirmsClaimAndRollsBack,
	"UEMMO.Tasks.M3_016.SaveFailureNeverConfirmsClaimAndRollsBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_016SaveFailureNeverConfirmsClaimAndRollsBack::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_016_MemoryStorage Storage;
	FM3_016_ProfileFixture Fixture;
	if (!Fixture.Setup(Test))
	{
		return true;
	}
	UProfileSaveService* Service = M3_016_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	FItemInstance Original;
	if (!M3_016_BeginSettlement(Test, Fixture, /*SettlementId*/ 3505, Original))
	{
		return true;
	}
	const int32 XPBefore = Fixture.Profile->GetXP();

	// The draft save (writes 1-2) commits; the claim-snapshot save fails at
	// its slot write (write 3). The in-memory claim must be rolled back and
	// the RETURN VALUE must be a failure - the UI truth is the return value,
	// so a failed save can never surface as a successful claim.
	FM3_016_CrashStorage Crash(&Storage);
	Crash.FailWriteOrdinal = 3;
	Service->SetStorage(&Crash);
	const FRewardClaimAtomicOutcome Failed = Fixture.Reward->ClaimPendingAtomic(3505, Fixture.Profile, Service);
	Test.TestEqual(TEXT("a failed snapshot save is reported as SaveFailed (never claim success)"),
		Failed.Result, ERewardClaimAtomicResult::SaveFailed);
	Test.TestFalse(TEXT("the failed claim committed no claim save"), Failed.bSaveCommitted);
	Test.TestTrue(TEXT("the failure names the failing save step"), !Failed.Error.IsEmpty());
	Test.TestEqual(TEXT("the failed claim stored nothing"), Failed.ClaimedItemCount, 0);

	// The in-memory commit was rolled back to the last committed state: the
	// draft stays pending UNCHANGED (original instance, no re-roll), no XP
	// was granted, nothing applied, the inventory is untouched.
	Test.TestEqual(TEXT("the XP was rolled back to the pre-claim value"), Fixture.Profile->GetXP(), XPBefore);
	Test.TestFalse(TEXT("the settlement is not recorded as applied"), Fixture.Profile->IsSettlementApplied(3505));
	Test.TestEqual(TEXT("the draft is still pending"), Fixture.Profile->GetPendingRewards().Num(), 1);
	if (Fixture.Profile->GetPendingRewards().Num() == 1 && Fixture.Profile->GetPendingRewards()[0].Items.Num() == 1)
	{
		Test.TestTrue(TEXT("the retained draft item is field-for-field the ORIGINAL (no re-roll)"),
			M3_016_InstancesEqual(Fixture.Profile->GetPendingRewards()[0].Items[0], Original));
	}
	Test.TestFalse(TEXT("the original instance never entered the inventory"),
		Fixture.Profile->GetInventory().Contains(Original.InstanceId));

	// The retry on the rolled-back state claims the SAME pre-generated item
	// (no re-roll) and grants the XP exactly once. The storage is healthy
	// again (the crash injection is removed).
	Service->SetStorage(&Storage);
	const FRewardClaimAtomicOutcome Retry = Fixture.Reward->ClaimPendingAtomic(3505, Fixture.Profile, Service);
	Test.TestEqual(TEXT("the retry after the save failure succeeds"), Retry.Result, ERewardClaimAtomicResult::Claimed);
	Test.TestTrue(TEXT("the retry grants the XP"), Retry.bGrantedXP);
	Test.TestTrue(TEXT("the retry's snapshot save committed"), Retry.bSaveCommitted);
	Test.TestEqual(TEXT("the total XP across the failure and the retry is exactly the design 50"),
		Fixture.Profile->GetXP() - XPBefore, URewardService::RewardXPPerClear);
	Test.TestEqual(TEXT("exactly one equipment piece was stored"), Fixture.Profile->GetInventory().Count(), 1);
	Test.TestTrue(TEXT("the stored piece is the ORIGINAL instance (InstanceId unchanged)"),
		M3_016_FindStored(Fixture.Profile->GetInventory(), Original.InstanceId) != nullptr);

	// RESTART: the persisted state is the claimed state (the retry's commit).
	FM3_016_RestartedState Restarted;
	if (!Restarted.Setup(Test, Storage, TEXT("save-failure restart")))
	{
		M3_016_CleanupTestSlots(Test, *Service, Storage);
		return true;
	}
	Test.TestEqual(TEXT("the restored XP is the once-granted total"),
		Restarted.Profile->GetXP(), XPBefore + URewardService::RewardXPPerClear);
	Test.TestTrue(TEXT("the restored piece is the ORIGINAL instance"),
		M3_016_FindStored(Restarted.Profile->GetInventory(), Original.InstanceId) != nullptr);
	Test.TestTrue(TEXT("the restored applied record survives"), Restarted.Profile->IsSettlementApplied(3505));
	Test.TestEqual(TEXT("no pending draft was restored"), Restarted.Profile->GetPendingRewards().Num(), 0);

	M3_016_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- Normal path: the claim commits, reloads and replays idempotently -------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_016NormalClaimCommitsSnapshotAndReloadsIdempotent,
	"UEMMO.Tasks.M3_016.NormalClaimCommitsSnapshotAndReloadsIdempotent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_016NormalClaimCommitsSnapshotAndReloadsIdempotent::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_016_MemoryStorage Storage;
	FM3_016_ProfileFixture Fixture;
	if (!Fixture.Setup(Test))
	{
		return true;
	}
	UProfileSaveService* Service = M3_016_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	const FRoomResult Result = M3_016_MakeResult(/*RunId*/ 3606, /*SettlementId*/ 3606, /*Seed*/ 36061, true);
	const FRewardBeginOutcome Begin = Fixture.Reward->BeginReward(Result, M3_016_MakeTrainingCatalog());
	Test.TestEqual(TEXT("setup: the settlement began with a pending draft"), Begin.Result, ERewardBeginResult::Applied);
	if (Begin.Result != ERewardBeginResult::Applied || Begin.Draft.Items.Num() != 1)
	{
		return true;
	}
	const FItemInstance Original = Begin.Draft.Items[0];
	const int32 XPBefore = Fixture.Profile->GetXP();

	// The normal path: claim + save succeed, the caller may confirm.
	const FRewardClaimAtomicOutcome Claim = Fixture.Reward->ClaimPendingAtomic(3606, Fixture.Profile, Service);
	Test.TestEqual(TEXT("the normal claim succeeds"), Claim.Result, ERewardClaimAtomicResult::Claimed);
	Test.TestEqual(TEXT("the normal claim stored exactly one item"), Claim.ClaimedItemCount, 1);
	Test.TestEqual(TEXT("the normal claim retains nothing"), Claim.RetainedItemCount, 0);
	Test.TestTrue(TEXT("the normal claim grants the XP"), Claim.bGrantedXP);
	Test.TestTrue(TEXT("the normal claim's snapshot save committed"), Claim.bSaveCommitted);
	Test.TestTrue(TEXT("the success carries no error"), Claim.Error.IsEmpty());

	// The in-memory state agrees with the confirmation.
	Test.TestEqual(TEXT("the XP grew by the design 50"),
		Fixture.Profile->GetXP() - XPBefore, URewardService::RewardXPPerClear);
	Test.TestEqual(TEXT("exactly one equipment piece was stored"), Fixture.Profile->GetInventory().Count(), 1);
	Test.TestTrue(TEXT("the stored piece is the ORIGINAL instance"),
		M3_016_FindStored(Fixture.Profile->GetInventory(), Original.InstanceId) != nullptr);
	Test.TestEqual(TEXT("the draft was consumed"), Fixture.Profile->GetPendingRewards().Num(), 0);
	Test.TestTrue(TEXT("the settlement is recorded as applied"), Fixture.Profile->IsSettlementApplied(3606));

	// RESTART: the committed snapshot restores exactly.
	FM3_016_RestartedState Restarted;
	if (!Restarted.Setup(Test, Storage, TEXT("normal restart")))
	{
		M3_016_CleanupTestSlots(Test, *Service, Storage);
		return true;
	}
	Test.TestEqual(TEXT("the restart recovered the committed claim"),
		static_cast<int32>(Restarted.LoadResult), static_cast<int32>(EStartupLoadResult::Recovered));
	Test.TestEqual(TEXT("the restored XP is the once-granted total"),
		Restarted.Profile->GetXP(), XPBefore + URewardService::RewardXPPerClear);
	Test.TestEqual(TEXT("exactly one equipment piece was restored"), Restarted.Profile->GetInventory().Count(), 1);
	Test.TestTrue(TEXT("the restored piece is the ORIGINAL instance"),
		M3_016_FindStored(Restarted.Profile->GetInventory(), Original.InstanceId) != nullptr);
	Test.TestTrue(TEXT("the restored applied record survives"), Restarted.Profile->IsSettlementApplied(3606));
	Test.TestEqual(TEXT("no pending draft was restored"), Restarted.Profile->GetPendingRewards().Num(), 0);

	// Replay guards from the persisted state: a replayed BeginReward answers
	// AlreadyApplied with no draft (no re-roll, no second item) and a
	// replayed TryClaimPending answers AlreadyClaimed.
	const FRewardBeginOutcome Replay = Restarted.Reward->BeginReward(Result, M3_016_MakeTrainingCatalog());
	Test.TestEqual(TEXT("the replayed BeginReward answers AlreadyApplied from the persisted state"),
		Replay.Result, ERewardBeginResult::AlreadyApplied);
	Test.TestEqual(TEXT("the replay adds no draft"), Restarted.Profile->GetPendingRewards().Num(), 0);
	const FRewardClaimOutcome ClaimAgain = Restarted.Reward->TryClaimPending(3606, Restarted.Profile->GetInventory());
	Test.TestEqual(TEXT("the replayed TryClaimPending answers AlreadyClaimed"),
		ClaimAgain.Result, ERewardClaimResult::AlreadyClaimed);
	Test.TestEqual(TEXT("the replay stored no second item"), Restarted.Profile->GetInventory().Count(), 1);

	M3_016_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

#endif
