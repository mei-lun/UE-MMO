// M3-014: A/B dual-slot save transaction with index commit (interface
// contract section 8). Pins UProfileSaveService end to end, always on the
// dedicated "TestSlot_" prefix (player "Profile_" saves are unreachable by
// construction and pinned by an isolation test):
// - the save transaction writes the INACTIVE slot, stamps generation +
//   integrity fields, verifies the read-back and only then commits the index;
// - three injected failure paths (slot write failure, corrupted read-back,
//   index write failure) each leave the old valid save loadable and the
//   activity index untouched (a partially written slot is overwritten next);
// - success commits the index to the new generation; consecutive saves
//   alternate A -> B -> A with rising generations;
// - a second BeginSave while a save is in progress MERGES into the queued
//   transaction (newest snapshot wins, nothing stacks);
// - the snapshot is captured once: caller-side mutations after BeginSave
//   never leak into the in-flight save;
// - a missing/corrupt index falls back to a valid slot; all-corrupt data
//   fails explicitly; a fresh prefix reports FailedNoData;
// - one smoke test exercises the REAL UGameplayStatics storage path on
//   TestSlot_ slots and deletes exactly those slots afterwards.
//
// Storage: every logic test runs on an in-memory ISaveStorage (duplicated
// USaveGame objects per slot, rooted) with a fault-injection decorator for
// the three failure paths - no real file is touched. Cleanup deletes only
// the three TestSlot_ names and asserts their absence afterwards.
//
// Stub-failure note: against the red stubs (A: every entry point fails;
// B: SaveProfile fakes Success without touching storage) every test fails
// on its concrete expectations - fake success cannot satisfy reload,
// raw-index and write-count assertions.

#include "Misc/AutomationTest.h"

#include "../Persistence/ProfileSaveService.h"
#include "../Profile/ProfileSubsystem.h"

#include "UObject/UObjectGlobals.h"
#include "Kismet/GameplayStatics.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_014
{
	// -- Test storage doubles ---------------------------------------------------

	/**
	 * In-memory ISaveStorage: every written slot is stored as an independent
	 * duplicate (rooted against GC), every read returns a fresh duplicate -
	 * the same semantics as disk bytes (caller mutations never leak into the
	 * stored copy, and a reader can freely tamper its returned object).
	 * Counts WriteSlot calls so tests can pin exactly how many writes one
	 * transaction performs.
	 */
	class FM3_014_MemoryStorage final : public ISaveStorage
	{
	public:
		virtual ~FM3_014_MemoryStorage() override
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

		/** All currently stored slot names (unordered). */
		TArray<FString> SlotNames() const
		{
			TArray<FString> Names;
			for (const TPair<FString, USaveGame*>& Pair : Slots)
			{
				Names.Add(Pair.Key);
			}
			return Names;
		}

	private:
		TMap<FString, USaveGame*> Slots;
	};

	/**
	 * Fault-injection decorator over another storage. Three injection knobs
	 * model the task card's failure paths without touching the wrapped data:
	 * - FailWriteSlotName: WriteSlot refuses that one exact slot name (the
	 *   "storage refused the write" path);
	 * - bFailAllWrites: every write fails (storage unavailable);
	 * - TamperReadSlotName: ReadSlot corrupts the returned slot save's
	 *   stored integrity digest - the modeled equivalent of the file's bits
	 *   going bad between the write and the service's read-back.
	 */
	class FM3_014_FaultStorage final : public ISaveStorage
	{
	public:
		explicit FM3_014_FaultStorage(ISaveStorage* InInner)
			: Inner(InInner)
		{
		}

		virtual bool WriteSlot(const FString& SlotName, USaveGame* Data) override
		{
			if (bFailAllWrites || (!FailWriteSlotName.IsEmpty() && FailWriteSlotName == SlotName))
			{
				return false;
			}
			return Inner->WriteSlot(SlotName, Data);
		}

		virtual USaveGame* ReadSlot(const FString& SlotName) override
		{
			USaveGame* Loaded = Inner->ReadSlot(SlotName);
			if (Loaded && !TamperReadSlotName.IsEmpty() && TamperReadSlotName == SlotName)
			{
				if (UProfileSlotSaveGame* Slot = Cast<UProfileSlotSaveGame>(Loaded))
				{
					Slot->PayloadDigest ^= 0xDEADBEFu; // tampered integrity field
				}
			}
			return Loaded;
		}

		virtual bool DeleteSlot(const FString& SlotName) override
		{
			return Inner->DeleteSlot(SlotName);
		}

		virtual bool DoesSlotExist(const FString& SlotName) override
		{
			return Inner->DoesSlotExist(SlotName);
		}

		/** When true, every write fails regardless of the slot name. */
		bool bFailAllWrites = false;

		/** Exact slot name whose write is refused (empty = no targeted failure). */
		FString FailWriteSlotName;

		/** Exact slot name whose read returns a corrupted integrity field (empty = none). */
		FString TamperReadSlotName;

	private:
		ISaveStorage* Inner = nullptr;
	};

	// -- Fixture helpers ---------------------------------------------------------

	/** The three TestSlot_ slot names every test of this suite operates on. */
	static FString M3_014_SlotAName() { return FString(TEXT("TestSlot_A")); }
	static FString M3_014_SlotBName() { return FString(TEXT("TestSlot_B")); }
	static FString M3_014_IndexSlotName() { return FString(TEXT("TestSlot_Index")); }

	/** Fixed character identity of the automation fixture (never all-zero). */
	static FGuid M3_014_CharacterId()
	{
		return FGuid(0xA0B0C0D0u, 0x1357u, 0x2468ACE0u, 0x0F1E2D3Cu);
	}

	/**
	 * Builds one deterministic item instance (same binary-exact stat style as
	 * the M3-013 fixture so snapshot strings round trip bit-exactly).
	 */
	static FItemInstance M3_014_MakeInstance(uint32 Seed, const TCHAR* DefinitionName,
		float Attack, float Defense, float MaxHP, int32 ItemLevel)
	{
		FItemInstance Instance;
		Instance.InstanceId = FGuid(0x5EED0000u + Seed, 0xBEEFu, 0x00D00Du, Seed * 13u + 7u);
		Instance.DefinitionId = FName(DefinitionName);
		Instance.RollSeed = static_cast<int64>(0x3FEDCBA987654321ULL) + static_cast<int64>(Seed);
		Instance.RolledStats.Attack = Attack;
		Instance.RolledStats.Defense = Defense;
		Instance.RolledStats.MaxHP = MaxHP;
		Instance.Level = ItemLevel;
		return Instance;
	}

	/**
	 * Builds a deterministic save request for (Level, XP, ItemCount); distinct
	 * levels produce distinct payloads (own instance ids, draft, applied id),
	 * so consecutive saves in one test never carry identical content.
	 */
	static FProfileSaveRequest M3_014_MakeRequest(FAutomationTestBase& Test, int32 Level, int32 XP, int32 ItemCount)
	{
		FProfileSaveRequest Request;
		Request.Snapshot.CharacterId = M3_014_CharacterId();
		Request.Snapshot.Level = Level;
		Request.Snapshot.XP = XP;
		Request.Snapshot.MaxHP = UProfileSubsystem::GetMaxHPForLevel(Level);
		Request.Snapshot.Attack = UProfileSubsystem::GetAttackForLevel(Level);
		Request.Snapshot.Defense = UProfileSubsystem::GetDefenseForLevel(Level);
		for (int32 Index = 0; Index < ItemCount; ++Index)
		{
			const FItemInstance Instance = M3_014_MakeInstance(static_cast<uint32>(Level * 100 + Index),
				TEXT("weapon_training"), 1.0f + 0.5f * Index, 0.25f * Index, 2.5f * Index, 1);
			if (Request.Inventory.TryAdd(Instance) != EInventoryAddResult::Added)
			{
				Test.AddError(FString::Printf(TEXT("fixture: TryAdd(item %d) was rejected"), Index));
			}
			if (Index == 0)
			{
				Request.EquippedMap.Add(EItemSlot::Weapon, Instance.InstanceId);
			}
		}
		FPendingReward Draft;
		Draft.SettlementId = 7000ull + static_cast<uint64>(Level);
		Draft.XP = 50;
		Draft.Items.Add(M3_014_MakeInstance(static_cast<uint32>(9000 + Level), TEXT("armor_leather"), 0.0f, 1.5f, 10.0f, 1));
		Request.PendingRewards.Add(Draft);
		Request.AppliedSettlementIds.Add(8000ull + static_cast<uint64>(Level));
		return Request;
	}

	/** Creates the service on the given prefix (storage optional). */
	static UProfileSaveService* M3_014_NewService(FAutomationTestBase& Test, ISaveStorage* Storage, const TCHAR* Prefix)
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
	 * Asserts the loaded outcome carries exactly the request's captured
	 * profile state (identity, progress, recomputed stat row, inventory,
	 * equipment, drafts, applied ids).
	 */
	static void M3_014_ExpectRestoredMatchesRequest(FAutomationTestBase& Test,
		const FProfileLoadOutcome& Loaded, const FProfileSaveRequest& Request, const TCHAR* Scenario)
	{
		Test.TestTrue(FString::Printf(TEXT("%s: CharacterId identical"), Scenario),
			Loaded.Snapshot.CharacterId == Request.Snapshot.CharacterId);
		Test.TestEqual(FString::Printf(TEXT("%s: Level identical"), Scenario),
			Loaded.Snapshot.Level, Request.Snapshot.Level);
		Test.TestEqual(FString::Printf(TEXT("%s: XP identical"), Scenario),
			Loaded.Snapshot.XP, Request.Snapshot.XP);
		Test.TestEqual(FString::Printf(TEXT("%s: MaxHP is the recomputed level row"), Scenario),
			Loaded.Snapshot.MaxHP, UProfileSubsystem::GetMaxHPForLevel(Request.Snapshot.Level));
		Test.TestEqual(FString::Printf(TEXT("%s: Attack is the recomputed level row"), Scenario),
			Loaded.Snapshot.Attack, UProfileSubsystem::GetAttackForLevel(Request.Snapshot.Level));
		Test.TestEqual(FString::Printf(TEXT("%s: Defense is the recomputed level row"), Scenario),
			Loaded.Snapshot.Defense, UProfileSubsystem::GetDefenseForLevel(Request.Snapshot.Level));
		Test.TestEqual(FString::Printf(TEXT("%s: restored inventory item count"), Scenario),
			Loaded.Inventory.Count(), Request.Inventory.Count());
		Test.TestEqual(FString::Printf(TEXT("%s: snapshot carries the inventory copy"), Scenario),
			Loaded.Snapshot.Inventory.Count(), Request.Inventory.Count());
		for (const FItemInstance& Expected : Request.Inventory.GetAll())
		{
			Test.TestTrue(FString::Printf(TEXT("%s: restored inventory keeps %s"), Scenario, *Expected.InstanceId.ToString()),
				Loaded.Inventory.Contains(Expected.InstanceId));
		}
		Test.TestEqual(FString::Printf(TEXT("%s: restored equipment binding count"), Scenario),
			Loaded.EquippedMap.Num(), Request.EquippedMap.Num());
		for (const TPair<EItemSlot, FGuid>& Pair : Request.EquippedMap)
		{
			const FGuid* Found = Loaded.EquippedMap.Find(Pair.Key);
			Test.TestTrue(FString::Printf(TEXT("%s: restored slot %d keeps its InstanceId"), Scenario, static_cast<int32>(Pair.Key)),
				Found != nullptr && *Found == Pair.Value);
		}
		Test.TestEqual(FString::Printf(TEXT("%s: restored draft count"), Scenario),
			Loaded.PendingRewards.Num(), Request.PendingRewards.Num());
		for (const FPendingReward& Expected : Request.PendingRewards)
		{
			Test.TestTrue(FString::Printf(TEXT("%s: restored draft %llu"), Scenario, Expected.SettlementId),
				Loaded.PendingRewards.ContainsByPredicate([&Expected](const FPendingReward& Candidate)
				{
					return Candidate.SettlementId == Expected.SettlementId && Candidate.XP == Expected.XP
						&& Candidate.Items.Num() == Expected.Items.Num();
				}));
		}
		for (const uint64 AppliedId : Request.AppliedSettlementIds)
		{
			Test.TestTrue(FString::Printf(TEXT("%s: restored applied id %llu"), Scenario, AppliedId),
				Loaded.AppliedSettlementIds.Contains(AppliedId));
		}
	}

	/** Reads the raw index object straight from storage (bypassing the service). */
	static UProfileIndexSaveGame* M3_014_ReadRawIndex(ISaveStorage& Storage)
	{
		return Cast<UProfileIndexSaveGame>(Storage.ReadSlot(M3_014_IndexSlotName()));
	}

	/**
	 * Cleanup: deletes the service's test slots and asserts the three
	 * TestSlot_ names are gone afterwards (acceptance: tests delete only the
	 * explicitly named test slots and leave everything else alone).
	 */
	static void M3_014_CleanupTestSlots(FAutomationTestBase& Test, UProfileSaveService& Service, ISaveStorage& Storage)
	{
		Service.DeleteTestSlots();
		Test.TestTrue(TEXT("cleanup: TestSlot_A deleted"), !Storage.DoesSlotExist(M3_014_SlotAName()));
		Test.TestTrue(TEXT("cleanup: TestSlot_B deleted"), !Storage.DoesSlotExist(M3_014_SlotBName()));
		Test.TestTrue(TEXT("cleanup: TestSlot_Index deleted"), !Storage.DoesSlotExist(M3_014_IndexSlotName()));
	}
}

using namespace UE::UEMMO::Tasks::M3_014;

// -- Failure path 1: the inactive slot write fails -----------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_014WriteSlotFailureKeepsOldSlotLoadable,
	"UEMMO.Tasks.M3_014.WriteSlotFailureKeepsOldSlotLoadable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_014WriteSlotFailureKeepsOldSlotLoadable::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_014_MemoryStorage Storage;
	UProfileSaveService* Service = M3_014_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	const FProfileSaveRequest FirstRequest = M3_014_MakeRequest(Test, 2, 10, 2);
	const FProfileSaveOutcome FirstSave = Service->SaveProfile(FirstRequest);
	Test.TestEqual(TEXT("setup: first save committed"),
		static_cast<int32>(FirstSave.Result), static_cast<int32>(EProfileSaveResult::Success));
	Test.TestEqual(TEXT("setup: first save generation is 1"), FirstSave.Generation, 1);
	Test.TestEqual(TEXT("setup: first save went to slot A"), FirstSave.WrittenSlotIndex, 0);

	// Injected failure: the storage refuses the write to the (inactive) slot B.
	FM3_014_FaultStorage Faulty(&Storage);
	Faulty.FailWriteSlotName = M3_014_SlotBName();
	Service->SetStorage(&Faulty);

	const FProfileSaveRequest SecondRequest = M3_014_MakeRequest(Test, 3, 30, 3);
	const FProfileSaveOutcome SecondSave = Service->SaveProfile(SecondRequest);
	Test.TestEqual(TEXT("write failure is reported with its own result code"),
		static_cast<int32>(SecondSave.Result), static_cast<int32>(EProfileSaveResult::FailedWriteSlot));
	Test.TestTrue(TEXT("write failure reports a reason"), !SecondSave.Message.IsEmpty());

	// The old valid save is still loadable and the index still points at it.
	const FProfileLoadOutcome Reloaded = Service->LoadActiveProfile();
	Test.TestEqual(TEXT("old save still loads after the failed write"),
		static_cast<int32>(Reloaded.Result), static_cast<int32>(EProfileLoadResult::Success));
	if (Reloaded.Result == EProfileLoadResult::Success)
	{
		M3_014_ExpectRestoredMatchesRequest(Test, Reloaded, FirstRequest, TEXT("after failed write"));
		Test.TestEqual(TEXT("old generation is unchanged"), Reloaded.Generation, 1);
		Test.TestEqual(TEXT("old slot is unchanged"), Reloaded.SlotIndex, 0);
	}

	// Second flavor: storage fully unavailable (every write fails).
	FM3_014_FaultStorage Unavailable(&Storage);
	Unavailable.bFailAllWrites = true;
	Service->SetStorage(&Unavailable);
	const FProfileSaveOutcome ThirdSave = Service->SaveProfile(SecondRequest);
	Test.TestEqual(TEXT("unavailable storage reports the write failure"),
		static_cast<int32>(ThirdSave.Result), static_cast<int32>(EProfileSaveResult::FailedWriteSlot));
	const FProfileLoadOutcome ReloadedAgain = Service->LoadActiveProfile();
	Test.TestEqual(TEXT("old save still loads with unavailable storage"),
		static_cast<int32>(ReloadedAgain.Result), static_cast<int32>(EProfileLoadResult::Success));

	// The activity index was never moved.
	UProfileIndexSaveGame* RawIndex = M3_014_ReadRawIndex(Storage);
	Test.TestNotNull(TEXT("raw index still exists"), RawIndex);
	if (RawIndex)
	{
		Test.TestEqual(TEXT("index still points at slot A"), RawIndex->ActiveSlotIndex, 0);
		Test.TestEqual(TEXT("index still records generation 1 for A"), RawIndex->SlotGenerations.Num() == 2 ? RawIndex->SlotGenerations[0] : -1, 1);
		Test.TestEqual(TEXT("index still records generation 0 for B"), RawIndex->SlotGenerations.Num() == 2 ? RawIndex->SlotGenerations[1] : -1, 0);
	}

	M3_014_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- Failure path 2: the read-back integrity check fails ------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_014ReadBackIntegrityFailureKeepsOldSlotLoadable,
	"UEMMO.Tasks.M3_014.ReadBackIntegrityFailureKeepsOldSlotLoadable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_014ReadBackIntegrityFailureKeepsOldSlotLoadable::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_014_MemoryStorage Storage;
	UProfileSaveService* Service = M3_014_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	const FProfileSaveRequest FirstRequest = M3_014_MakeRequest(Test, 2, 10, 2);
	const FProfileSaveOutcome FirstSave = Service->SaveProfile(FirstRequest);
	Test.TestEqual(TEXT("setup: first save committed"),
		static_cast<int32>(FirstSave.Result), static_cast<int32>(EProfileSaveResult::Success));

	// Injected failure: the freshly written slot's stored integrity digest is
	// tampered before the service reads it back (corrupted file bits).
	FM3_014_FaultStorage Faulty(&Storage);
	Faulty.TamperReadSlotName = M3_014_SlotBName();
	Service->SetStorage(&Faulty);

	const FProfileSaveRequest SecondRequest = M3_014_MakeRequest(Test, 3, 30, 3);
	const FProfileSaveOutcome SecondSave = Service->SaveProfile(SecondRequest);
	Test.TestEqual(TEXT("read-back integrity failure is reported with its own result code"),
		static_cast<int32>(SecondSave.Result), static_cast<int32>(EProfileSaveResult::FailedReadBack));
	Test.TestTrue(TEXT("read-back failure reports a reason"), !SecondSave.Message.IsEmpty());

	// The slot content WAS written (but never committed): it simply waits to
	// be overwritten by the next save.
	Test.TestTrue(TEXT("the uncommitted slot content exists in storage"),
		Storage.DoesSlotExist(M3_014_SlotBName()));

	// The index was not moved: the old save is still the active one.
	UProfileIndexSaveGame* RawIndex = M3_014_ReadRawIndex(Storage);
	Test.TestNotNull(TEXT("raw index still exists"), RawIndex);
	if (RawIndex)
	{
		Test.TestEqual(TEXT("index still points at slot A"), RawIndex->ActiveSlotIndex, 0);
		Test.TestEqual(TEXT("index still records generation 1 for A"), RawIndex->SlotGenerations.Num() == 2 ? RawIndex->SlotGenerations[0] : -1, 1);
	}
	const FProfileLoadOutcome Reloaded = Service->LoadActiveProfile();
	Test.TestEqual(TEXT("old save still loads after the failed read-back"),
		static_cast<int32>(Reloaded.Result), static_cast<int32>(EProfileLoadResult::Success));
	if (Reloaded.Result == EProfileLoadResult::Success)
	{
		M3_014_ExpectRestoredMatchesRequest(Test, Reloaded, FirstRequest, TEXT("after failed read-back"));
		Test.TestEqual(TEXT("old generation is unchanged"), Reloaded.Generation, 1);
	}

	M3_014_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- Failure path 3: the index commit write fails -------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_014IndexWriteFailureKeepsOldSlotLoadableAndIndexUntouched,
	"UEMMO.Tasks.M3_014.IndexWriteFailureKeepsOldSlotLoadableAndIndexUntouched",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_014IndexWriteFailureKeepsOldSlotLoadableAndIndexUntouched::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_014_MemoryStorage Storage;
	UProfileSaveService* Service = M3_014_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	const FProfileSaveRequest FirstRequest = M3_014_MakeRequest(Test, 2, 10, 2);
	const FProfileSaveOutcome FirstSave = Service->SaveProfile(FirstRequest);
	Test.TestEqual(TEXT("setup: first save committed"),
		static_cast<int32>(FirstSave.Result), static_cast<int32>(EProfileSaveResult::Success));

	// Injected failure: the index commit write fails after slot B was written
	// and verified.
	FM3_014_FaultStorage Faulty(&Storage);
	Faulty.FailWriteSlotName = M3_014_IndexSlotName();
	Service->SetStorage(&Faulty);

	const FProfileSaveRequest SecondRequest = M3_014_MakeRequest(Test, 3, 30, 3);
	const FProfileSaveOutcome SecondSave = Service->SaveProfile(SecondRequest);
	Test.TestEqual(TEXT("index write failure is reported with its own result code"),
		static_cast<int32>(SecondSave.Result), static_cast<int32>(EProfileSaveResult::FailedWriteIndex));
	Test.TestTrue(TEXT("index failure reports a reason"), !SecondSave.Message.IsEmpty());

	// Slot B holds verified but UNCOMMITTED content (generation 2).
	UProfileSlotSaveGame* SlotB = Cast<UProfileSlotSaveGame>(Storage.ReadSlot(M3_014_SlotBName()));
	Test.TestNotNull(TEXT("the inactive slot content was written before the index failed"), SlotB);
	if (SlotB)
	{
		Test.TestEqual(TEXT("the uncommitted slot carries generation 2"), SlotB->SlotGeneration, 2);
	}

	// The index is untouched and the old save still loads as active.
	UProfileIndexSaveGame* RawIndex = M3_014_ReadRawIndex(Storage);
	Test.TestNotNull(TEXT("raw index still exists"), RawIndex);
	if (RawIndex)
	{
		Test.TestEqual(TEXT("index still points at slot A"), RawIndex->ActiveSlotIndex, 0);
		Test.TestEqual(TEXT("index still records generation 1 for A"), RawIndex->SlotGenerations.Num() == 2 ? RawIndex->SlotGenerations[0] : -1, 1);
	}
	const FProfileLoadOutcome Reloaded = Service->LoadActiveProfile();
	Test.TestEqual(TEXT("old save still loads after the failed index commit"),
		static_cast<int32>(Reloaded.Result), static_cast<int32>(EProfileLoadResult::Success));
	if (Reloaded.Result == EProfileLoadResult::Success)
	{
		M3_014_ExpectRestoredMatchesRequest(Test, Reloaded, FirstRequest, TEXT("after failed index commit"));
		Test.TestEqual(TEXT("old generation is unchanged"), Reloaded.Generation, 1);
	}

	// The failed slot is simply overwritten by the next successful save.
	Service->SetStorage(&Storage); // back to healthy storage
	const FProfileSaveRequest ThirdRequest = M3_014_MakeRequest(Test, 4, 40, 4);
	const FProfileSaveOutcome ThirdSave = Service->SaveProfile(ThirdRequest);
	Test.TestEqual(TEXT("the next save succeeds and reuses the failed slot"),
		static_cast<int32>(ThirdSave.Result), static_cast<int32>(EProfileSaveResult::Success));
	Test.TestEqual(TEXT("the next save writes slot B again"), ThirdSave.WrittenSlotIndex, 1);
	Test.TestEqual(TEXT("the next save commits generation 2"), ThirdSave.Generation, 2);
	const FProfileLoadOutcome ReloadedNew = Service->LoadActiveProfile();
	Test.TestEqual(TEXT("the newest save loads"),
		static_cast<int32>(ReloadedNew.Result), static_cast<int32>(EProfileLoadResult::Success));
	if (ReloadedNew.Result == EProfileLoadResult::Success)
	{
		M3_014_ExpectRestoredMatchesRequest(Test, ReloadedNew, ThirdRequest, TEXT("after retry"));
	}

	M3_014_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- Success path ---------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_014SuccessfulSaveCommitsIndexToNewGeneration,
	"UEMMO.Tasks.M3_014.SuccessfulSaveCommitsIndexToNewGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_014SuccessfulSaveCommitsIndexToNewGeneration::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_014_MemoryStorage Storage;
	UProfileSaveService* Service = M3_014_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	const FProfileSaveRequest FirstRequest = M3_014_MakeRequest(Test, 2, 10, 2);
	const FProfileSaveOutcome SaveOutcome = Service->SaveProfile(FirstRequest);
	Test.TestEqual(TEXT("save succeeds"),
		static_cast<int32>(SaveOutcome.Result), static_cast<int32>(EProfileSaveResult::Success));
	Test.TestEqual(TEXT("success reports generation 1"), SaveOutcome.Generation, 1);
	Test.TestEqual(TEXT("success reports slot A"), SaveOutcome.WrittenSlotIndex, 0);
	Test.TestTrue(TEXT("success carries no failure message"), SaveOutcome.Message.IsEmpty());

	// The committed index points at the new slot and generation.
	UProfileIndexSaveGame* RawIndex = M3_014_ReadRawIndex(Storage);
	Test.TestNotNull(TEXT("index was committed"), RawIndex);
	if (RawIndex)
	{
		Test.TestEqual(TEXT("index points at slot A"), RawIndex->ActiveSlotIndex, 0);
		Test.TestEqual(TEXT("index records generation 1 for A"), RawIndex->SlotGenerations.Num() == 2 ? RawIndex->SlotGenerations[0] : -1, 1);
	}

	// LoadActiveProfile restores the exact saved state.
	const FProfileLoadOutcome Loaded = Service->LoadActiveProfile();
	Test.TestEqual(TEXT("active profile loads"),
		static_cast<int32>(Loaded.Result), static_cast<int32>(EProfileLoadResult::Success));
	if (Loaded.Result == EProfileLoadResult::Success)
	{
		M3_014_ExpectRestoredMatchesRequest(Test, Loaded, FirstRequest, TEXT("after successful save"));
		Test.TestEqual(TEXT("loaded generation is 1"), Loaded.Generation, 1);
		Test.TestEqual(TEXT("loaded from slot A"), Loaded.SlotIndex, 0);
	}

	M3_014_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- A/B alternation -------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_014ConsecutiveSavesAlternateSlotsAndRiseGeneration,
	"UEMMO.Tasks.M3_014.ConsecutiveSavesAlternateSlotsAndRiseGeneration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_014ConsecutiveSavesAlternateSlotsAndRiseGeneration::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_014_MemoryStorage Storage;
	UProfileSaveService* Service = M3_014_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	const FProfileSaveRequest FirstRequest = M3_014_MakeRequest(Test, 2, 10, 2);
	const FProfileSaveRequest SecondRequest = M3_014_MakeRequest(Test, 3, 30, 3);
	const FProfileSaveRequest ThirdRequest = M3_014_MakeRequest(Test, 4, 40, 4);

	const FProfileSaveRequest* Requests[3] = { &FirstRequest, &SecondRequest, &ThirdRequest };
	const int32 ExpectedGenerations[3] = { 1, 2, 3 };
	const int32 ExpectedSlots[3] = { 0, 1, 0 }; // A -> B -> A
	for (int32 Step = 0; Step < 3; ++Step)
	{
		const FProfileSaveOutcome SaveOutcome = Service->SaveProfile(*Requests[Step]);
		Test.TestEqual(FString::Printf(TEXT("save %d succeeds"), Step + 1),
			static_cast<int32>(SaveOutcome.Result), static_cast<int32>(EProfileSaveResult::Success));
		Test.TestEqual(FString::Printf(TEXT("save %d reports generation %d"), Step + 1, ExpectedGenerations[Step]),
			SaveOutcome.Generation, ExpectedGenerations[Step]);
		Test.TestEqual(FString::Printf(TEXT("save %d reports slot index %d"), Step + 1, ExpectedSlots[Step]),
			SaveOutcome.WrittenSlotIndex, ExpectedSlots[Step]);

		const FProfileLoadOutcome Loaded = Service->LoadActiveProfile();
		Test.TestEqual(FString::Printf(TEXT("load %d succeeds"), Step + 1),
			static_cast<int32>(Loaded.Result), static_cast<int32>(EProfileLoadResult::Success));
		if (Loaded.Result == EProfileLoadResult::Success)
		{
			M3_014_ExpectRestoredMatchesRequest(Test, Loaded, *Requests[Step],
				*FString::Printf(TEXT("after save %d"), Step + 1));
			Test.TestEqual(FString::Printf(TEXT("load %d reports generation %d"), Step + 1, ExpectedGenerations[Step]),
				Loaded.Generation, ExpectedGenerations[Step]);
			Test.TestEqual(FString::Printf(TEXT("load %d reads slot index %d"), Step + 1, ExpectedSlots[Step]),
				Loaded.SlotIndex, ExpectedSlots[Step]);
		}
	}

	// Final index state: active A, generation 3 on A, 2 on B.
	UProfileIndexSaveGame* RawIndex = M3_014_ReadRawIndex(Storage);
	Test.TestNotNull(TEXT("index was committed after three saves"), RawIndex);
	if (RawIndex)
	{
		Test.TestEqual(TEXT("index points back at slot A"), RawIndex->ActiveSlotIndex, 0);
		Test.TestEqual(TEXT("index records generation 3 for A"), RawIndex->SlotGenerations.Num() == 2 ? RawIndex->SlotGenerations[0] : -1, 3);
		Test.TestEqual(TEXT("index records generation 2 for B"), RawIndex->SlotGenerations.Num() == 2 ? RawIndex->SlotGenerations[1] : -1, 2);
	}

	M3_014_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- Concurrency guard: merge instead of stacking --------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_014SecondRequestWhileSaveInProgressMerges,
	"UEMMO.Tasks.M3_014.SecondRequestWhileSaveInProgressMerges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_014SecondRequestWhileSaveInProgressMerges::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_014_MemoryStorage Storage;
	UProfileSaveService* Service = M3_014_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	const FProfileSaveRequest SupersededRequest = M3_014_MakeRequest(Test, 2, 10, 2);
	const FProfileSaveRequest NewestRequest = M3_014_MakeRequest(Test, 3, 30, 3);
	const FProfileSaveRequest LaterRequest = M3_014_MakeRequest(Test, 4, 40, 4);

	const FProfileSaveOutcome FirstBegin = Service->BeginSave(SupersededRequest);
	Test.TestEqual(TEXT("first BeginSave queues the transaction"),
		static_cast<int32>(FirstBegin.Result), static_cast<int32>(EProfileSaveResult::Queued));
	Test.TestTrue(TEXT("a save is in progress after BeginSave"), Service->IsSaveInProgress());

	const FProfileSaveOutcome SecondBegin = Service->BeginSave(NewestRequest);
	Test.TestEqual(TEXT("second BeginSave while in progress is merged, not stacked"),
		static_cast<int32>(SecondBegin.Result), static_cast<int32>(EProfileSaveResult::Merged));
	Test.TestTrue(TEXT("still exactly one transaction in progress"), Service->IsSaveInProgress());

	const int32 WritesBefore = Storage.WriteCallCount;
	const FProfileSaveOutcome Committed = Service->ProcessPendingSave();
	Test.TestEqual(TEXT("the single merged transaction commits"),
		static_cast<int32>(Committed.Result), static_cast<int32>(EProfileSaveResult::Success));
	Test.TestEqual(TEXT("the merged transaction is generation 1"), Committed.Generation, 1);
	Test.TestFalse(TEXT("no save is in progress after the commit"), Service->IsSaveInProgress());
	Test.TestEqual(TEXT("one transaction performs exactly one slot write plus one index write"),
		Storage.WriteCallCount - WritesBefore, 2);

	// The NEWEST state won the merge (the superseded snapshot is gone).
	const FProfileLoadOutcome Loaded = Service->LoadActiveProfile();
	Test.TestEqual(TEXT("the merged snapshot loads"),
		static_cast<int32>(Loaded.Result), static_cast<int32>(EProfileLoadResult::Success));
	if (Loaded.Result == EProfileLoadResult::Success)
	{
		M3_014_ExpectRestoredMatchesRequest(Test, Loaded, NewestRequest, TEXT("merged transaction"));
	}

	// Nothing stacked: the next request is the SECOND transaction (generation 2).
	const FProfileSaveOutcome ThirdBegin = Service->BeginSave(LaterRequest);
	Test.TestEqual(TEXT("next BeginSave queues normally"),
		static_cast<int32>(ThirdBegin.Result), static_cast<int32>(EProfileSaveResult::Queued));
	const FProfileSaveOutcome SecondCommit = Service->ProcessPendingSave();
	Test.TestEqual(TEXT("the second transaction commits generation 2, not 3"),
		SecondCommit.Generation, 2);

	M3_014_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- Prefix isolation -------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_014TestSlotPrefixNeverTouchesPlayerSlots,
	"UEMMO.Tasks.M3_014.TestSlotPrefixNeverTouchesPlayerSlots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_014TestSlotPrefixNeverTouchesPlayerSlots::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_014_MemoryStorage Storage;

	// A (simulated) player profile service on the production prefix sharing
	// the same storage: its Profile_A / Profile_Index slots must survive.
	UProfileSaveService* PlayerService = M3_014_NewService(Test, &Storage, TEXT("Profile_"));
	if (!PlayerService)
	{
		return true;
	}
	const FProfileSaveRequest PlayerRequest = M3_014_MakeRequest(Test, 5, 60, 2);
	const FProfileSaveOutcome PlayerSave = PlayerService->SaveProfile(PlayerRequest);
	Test.TestEqual(TEXT("setup: the player profile save committed"),
		static_cast<int32>(PlayerSave.Result), static_cast<int32>(EProfileSaveResult::Success));

	const TArray<FString> NamesBefore = Storage.SlotNames();
	Test.TestTrue(TEXT("setup: the player slot exists"),
		NamesBefore.Contains(TEXT("Profile_A")) && NamesBefore.Contains(TEXT("Profile_Index")));

	// The automation service operates strictly on its own TestSlot_ prefix.
	UProfileSaveService* TestService = M3_014_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!TestService)
	{
		return true;
	}
	const FProfileSaveRequest FirstRequest = M3_014_MakeRequest(Test, 2, 10, 2);
	const FProfileSaveRequest SecondRequest = M3_014_MakeRequest(Test, 3, 30, 3);
	Test.TestEqual(TEXT("test-prefix save 1 commits"),
		static_cast<int32>(TestService->SaveProfile(FirstRequest).Result), static_cast<int32>(EProfileSaveResult::Success));
	Test.TestEqual(TEXT("test-prefix save 2 commits"),
		static_cast<int32>(TestService->SaveProfile(SecondRequest).Result), static_cast<int32>(EProfileSaveResult::Success));

	// The player save is byte-for-byte untouched: it still loads with its
	// original data and generation through the player service.
	const FProfileLoadOutcome PlayerReload = PlayerService->LoadActiveProfile();
	Test.TestEqual(TEXT("the player save still loads after test-prefix saves"),
		static_cast<int32>(PlayerReload.Result), static_cast<int32>(EProfileLoadResult::Success));
	if (PlayerReload.Result == EProfileLoadResult::Success)
	{
		M3_014_ExpectRestoredMatchesRequest(Test, PlayerReload, PlayerRequest, TEXT("player save untouched"));
		Test.TestEqual(TEXT("the player save keeps generation 1"), PlayerReload.Generation, 1);
	}

	// Cleanup removes ONLY the TestSlot_ slots.
	const int32 Removed = TestService->DeleteTestSlots();
	Test.TestEqual(TEXT("cleanup removed exactly the three test slots"), Removed, 3);
	const TArray<FString> NamesAfter = Storage.SlotNames();
	Test.TestEqual(TEXT("only the player slots remain"), NamesAfter.Num(), 2);
	Test.TestTrue(TEXT("Profile_A survived the cleanup"), NamesAfter.Contains(TEXT("Profile_A")));
	Test.TestTrue(TEXT("Profile_Index survived the cleanup"), NamesAfter.Contains(TEXT("Profile_Index")));
	for (const FString& Name : NamesAfter)
	{
		Test.TestFalse(FString::Printf(TEXT("no slot named %s survives the cleanup"), *Name), Name.StartsWith(TEXT("TestSlot_")));
	}

	// And the player profile is still fully loadable afterwards.
	const FProfileLoadOutcome PlayerReloadAgain = PlayerService->LoadActiveProfile();
	Test.TestEqual(TEXT("the player save still loads after the cleanup"),
		static_cast<int32>(PlayerReloadAgain.Result), static_cast<int32>(EProfileLoadResult::Success));

	return true;
}

// -- Consistent snapshot ------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_014CallerMutationAfterBeginDoesNotChangeSnapshot,
	"UEMMO.Tasks.M3_014.CallerMutationAfterBeginDoesNotChangeSnapshot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_014CallerMutationAfterBeginDoesNotChangeSnapshot::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_014_MemoryStorage Storage;
	UProfileSaveService* Service = M3_014_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	FProfileSaveRequest Request = M3_014_MakeRequest(Test, 2, 10, 2);
	const FProfileSaveOutcome Begin = Service->BeginSave(Request);
	Test.TestEqual(TEXT("BeginSave queues the transaction"),
		static_cast<int32>(Begin.Result), static_cast<int32>(EProfileSaveResult::Queued));

	// Mutate the caller's data while the transaction is in progress: the
	// already-captured snapshot must not change.
	const FItemInstance Extra = M3_014_MakeInstance(7777u, TEXT("accessory_ring"), 9.0f, 9.0f, 9.0f, 1);
	Test.TestEqual(TEXT("setup: the extra item was added to the caller's copy"),
		static_cast<int32>(Request.Inventory.TryAdd(Extra)), static_cast<int32>(EInventoryAddResult::Added));
	Request.Snapshot.XP = 999;

	const FProfileSaveOutcome Committed = Service->ProcessPendingSave();
	Test.TestEqual(TEXT("the transaction commits the captured snapshot"),
		static_cast<int32>(Committed.Result), static_cast<int32>(EProfileSaveResult::Success));

	const FProfileLoadOutcome Loaded = Service->LoadActiveProfile();
	Test.TestEqual(TEXT("the committed save loads"),
		static_cast<int32>(Loaded.Result), static_cast<int32>(EProfileLoadResult::Success));
	if (Loaded.Result == EProfileLoadResult::Success)
	{
		M3_014_ExpectRestoredMatchesRequest(Test, Loaded, M3_014_MakeRequest(Test, 2, 10, 2), TEXT("captured snapshot"));
		Test.TestEqual(TEXT("the post-capture item did not leak into the save"), Loaded.Inventory.Count(), 2);
		Test.TestFalse(TEXT("the post-capture instance is absent"), Loaded.Inventory.Contains(Extra.InstanceId));
		Test.TestEqual(TEXT("the post-capture XP did not leak into the save"), Loaded.Snapshot.XP, 10);
	}

	M3_014_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- Index missing: fallback to a valid slot ------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_014MissingIndexFallsBackToValidSlot,
	"UEMMO.Tasks.M3_014.MissingIndexFallsBackToValidSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_014MissingIndexFallsBackToValidSlot::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_014_MemoryStorage Storage;
	UProfileSaveService* Service = M3_014_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	const FProfileSaveRequest FirstRequest = M3_014_MakeRequest(Test, 2, 10, 2);
	Test.TestEqual(TEXT("setup: the save committed"),
		static_cast<int32>(Service->SaveProfile(FirstRequest).Result), static_cast<int32>(EProfileSaveResult::Success));

	// The index disappears (the exact slot file alone is deleted).
	Test.TestTrue(TEXT("setup: the index slot was deleted"), Storage.DeleteSlot(M3_014_IndexSlotName()));

	const FProfileLoadOutcome Loaded = Service->LoadActiveProfile();
	Test.TestEqual(TEXT("a valid slot is recovered when the index is missing"),
		static_cast<int32>(Loaded.Result), static_cast<int32>(EProfileLoadResult::SuccessFallback));
	if (Loaded.Result == EProfileLoadResult::SuccessFallback)
	{
		M3_014_ExpectRestoredMatchesRequest(Test, Loaded, FirstRequest, TEXT("recovered slot"));
		Test.TestEqual(TEXT("the recovered slot's generation is reported"), Loaded.Generation, 1);
		Test.TestEqual(TEXT("the recovered slot index is reported"), Loaded.SlotIndex, 0);
		Test.TestTrue(TEXT("the recovery names the reason"), !Loaded.Message.IsEmpty());
	}

	// A save after the index loss rebuilds the state from the surviving slot
	// payload (digest + generation): it writes the OTHER slot with a
	// generation above the surviving one, so the surviving save is never
	// overwritten by the transaction.
	const FProfileSaveRequest SecondRequest = M3_014_MakeRequest(Test, 3, 30, 3);
	const FProfileSaveOutcome SecondSave = Service->SaveProfile(SecondRequest);
	Test.TestEqual(TEXT("saving still works after the index loss"),
		static_cast<int32>(SecondSave.Result), static_cast<int32>(EProfileSaveResult::Success));
	Test.TestEqual(TEXT("the rebuilt generation is above the surviving slot's"),
		SecondSave.Generation, 2);
	Test.TestEqual(TEXT("the rebuilt save protects the surviving slot and writes the other one"),
		SecondSave.WrittenSlotIndex, 1);
	UProfileSlotSaveGame* SurvivingSlotA = Cast<UProfileSlotSaveGame>(Storage.ReadSlot(M3_014_SlotAName()));
	Test.TestNotNull(TEXT("the surviving slot A content is untouched"), SurvivingSlotA);
	if (SurvivingSlotA)
	{
		Test.TestEqual(TEXT("the surviving slot A keeps generation 1"), SurvivingSlotA->SlotGeneration, 1);
	}

	M3_014_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- Corrupt index: fallback on load, refuse to save -----------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_014CorruptIndexFallsBackAndRefusesSave,
	"UEMMO.Tasks.M3_014.CorruptIndexFallsBackAndRefusesSave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_014CorruptIndexFallsBackAndRefusesSave::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_014_MemoryStorage Storage;
	UProfileSaveService* Service = M3_014_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	const FProfileSaveRequest FirstRequest = M3_014_MakeRequest(Test, 2, 10, 2);
	Test.TestEqual(TEXT("setup: the save committed"),
		static_cast<int32>(Service->SaveProfile(FirstRequest).Result), static_cast<int32>(EProfileSaveResult::Success));

	// The index exists but its content is garbage (invalid active slot, wrong
	// array size): unreadable for every service purpose.
	UProfileIndexSaveGame* Bogus = NewObject<UProfileIndexSaveGame>(GetTransientPackage());
	Bogus->ActiveSlotIndex = 7;
	Bogus->SlotGenerations.Add(1);
	Test.TestTrue(TEXT("setup: the corrupt index was written"), Storage.WriteSlot(M3_014_IndexSlotName(), Bogus));

	// Loading falls back to the surviving valid slot.
	const FProfileLoadOutcome Loaded = Service->LoadActiveProfile();
	Test.TestEqual(TEXT("a valid slot is recovered when the index is corrupt"),
		static_cast<int32>(Loaded.Result), static_cast<int32>(EProfileLoadResult::SuccessFallback));
	if (Loaded.Result == EProfileLoadResult::SuccessFallback)
	{
		M3_014_ExpectRestoredMatchesRequest(Test, Loaded, FirstRequest, TEXT("recovered slot"));
	}

	// Saving REFUSES to guess: with the index unreadable the service cannot
	// know which slot holds the only good copy, so it fails without touching
	// any slot.
	const FProfileSaveRequest SecondRequest = M3_014_MakeRequest(Test, 3, 30, 3);
	const FProfileSaveOutcome Refused = Service->SaveProfile(SecondRequest);
	Test.TestEqual(TEXT("saving with a corrupt index is refused"),
		static_cast<int32>(Refused.Result), static_cast<int32>(EProfileSaveResult::FailedIndexState));
	Test.TestTrue(TEXT("the refusal names the reason"), !Refused.Message.IsEmpty());

	// The old data is still there and still recovers.
	const FProfileLoadOutcome Reloaded = Service->LoadActiveProfile();
	Test.TestEqual(TEXT("the old save still recovers after the refused save"),
		static_cast<int32>(Reloaded.Result), static_cast<int32>(EProfileLoadResult::SuccessFallback));
	if (Reloaded.Result == EProfileLoadResult::SuccessFallback)
	{
		M3_014_ExpectRestoredMatchesRequest(Test, Reloaded, FirstRequest, TEXT("after refused save"));
	}

	M3_014_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- Both slots corrupt: explicit failure -----------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_014BothSlotsCorruptFailsExplicitly,
	"UEMMO.Tasks.M3_014.BothSlotsCorruptFailsExplicitly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_014BothSlotsCorruptFailsExplicitly::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_014_MemoryStorage Storage;
	UProfileSaveService* Service = M3_014_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	// Data exists but is garbage in both slots (base UProfileSaveGame objects
	// are not slot saves; their level values are invalid anyway), plus an
	// index pointing at the garbage.
	UProfileSaveGame* GarbageA = NewObject<UProfileSaveGame>(GetTransientPackage());
	GarbageA->Level = 999;
	UProfileSaveGame* GarbageB = NewObject<UProfileSaveGame>(GetTransientPackage());
	GarbageB->Level = 0;
	UProfileIndexSaveGame* BogusIndex = NewObject<UProfileIndexSaveGame>(GetTransientPackage());
	BogusIndex->ActiveSlotIndex = 0;
	BogusIndex->SlotGenerations.Add(1);
	BogusIndex->SlotGenerations.Add(1);
	Test.TestTrue(TEXT("setup: garbage slot A written"), Storage.WriteSlot(M3_014_SlotAName(), GarbageA));
	Test.TestTrue(TEXT("setup: garbage slot B written"), Storage.WriteSlot(M3_014_SlotBName(), GarbageB));
	Test.TestTrue(TEXT("setup: index written"), Storage.WriteSlot(M3_014_IndexSlotName(), BogusIndex));

	const FProfileLoadOutcome Loaded = Service->LoadActiveProfile();
	Test.TestEqual(TEXT("all-corrupt data fails explicitly"),
		static_cast<int32>(Loaded.Result), static_cast<int32>(EProfileLoadResult::FailedCorrupt));
	Test.TestTrue(TEXT("the failure names the reason"), !Loaded.Message.IsEmpty());
	Test.TestFalse(TEXT("no CharacterId is restored on failure"), Loaded.Snapshot.CharacterId.IsValid());
	Test.TestEqual(TEXT("no generation is reported on failure"), Loaded.Generation, 0);
	Test.TestEqual(TEXT("no slot is reported on failure"), Loaded.SlotIndex, -1);
	Test.TestEqual(TEXT("no inventory is restored on failure"), Loaded.Inventory.Count(), 0);

	// Fresh-state flavor: with truly nothing stored the load reports
	// FailedNoData (a fresh state, not corruption).
	FM3_014_MemoryStorage EmptyStorage;
	UProfileSaveService* FreshService = M3_014_NewService(Test, &EmptyStorage, TEXT("TestSlot_"));
	if (FreshService)
	{
		const FProfileLoadOutcome FreshLoad = FreshService->LoadActiveProfile();
		Test.TestEqual(TEXT("a fresh prefix reports FailedNoData"),
			static_cast<int32>(FreshLoad.Result), static_cast<int32>(EProfileLoadResult::FailedNoData));
		Test.TestTrue(TEXT("the fresh-state message names the reason"), !FreshLoad.Message.IsEmpty());
	}

	M3_014_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- Real storage smoke test (UGameplayStatics path, TestSlot_ only) --------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_014RealGameplayStaticsStorageSmokeOnTestSlots,
	"UEMMO.Tasks.M3_014.RealGameplayStaticsStorageSmokeOnTestSlots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_014RealGameplayStaticsStorageSmokeOnTestSlots::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	// No SetStorage call: the service uses its default real storage
	// (UGameplayStatics::SaveGameToSlot / LoadGameFromSlot).
	UProfileSaveService* Service = M3_014_NewService(Test, nullptr, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	// Start clean: only the three TestSlot_ names are removed.
	Service->DeleteTestSlots();
	Test.TestFalse(TEXT("smoke: TestSlot_A absent after cleanup"), UGameplayStatics::DoesSaveGameExist(M3_014_SlotAName(), 0));
	Test.TestFalse(TEXT("smoke: TestSlot_B absent after cleanup"), UGameplayStatics::DoesSaveGameExist(M3_014_SlotBName(), 0));
	Test.TestFalse(TEXT("smoke: TestSlot_Index absent after cleanup"), UGameplayStatics::DoesSaveGameExist(M3_014_IndexSlotName(), 0));

	const FProfileSaveRequest FirstRequest = M3_014_MakeRequest(Test, 2, 10, 2);
	const FProfileSaveOutcome FirstSave = Service->SaveProfile(FirstRequest);
	Test.TestEqual(TEXT("smoke: the real storage path commits the save"),
		static_cast<int32>(FirstSave.Result), static_cast<int32>(EProfileSaveResult::Success));
	Test.TestEqual(TEXT("smoke: generation 1"), FirstSave.Generation, 1);

	const FProfileSaveRequest SecondRequest = M3_014_MakeRequest(Test, 3, 30, 3);
	Test.TestEqual(TEXT("smoke: the second real save commits"),
		static_cast<int32>(Service->SaveProfile(SecondRequest).Result), static_cast<int32>(EProfileSaveResult::Success));

	const FProfileLoadOutcome Loaded = Service->LoadActiveProfile();
	Test.TestEqual(TEXT("smoke: the real storage path loads the active save"),
		static_cast<int32>(Loaded.Result), static_cast<int32>(EProfileLoadResult::Success));
	if (Loaded.Result == EProfileLoadResult::Success)
	{
		M3_014_ExpectRestoredMatchesRequest(Test, Loaded, SecondRequest, TEXT("smoke reload"));
		Test.TestEqual(TEXT("smoke: generation 2 after two saves"), Loaded.Generation, 2);
	}

	// Cleanup deletes exactly the three TestSlot_ slots from the real storage.
	const int32 Removed = Service->DeleteTestSlots();
	Test.TestEqual(TEXT("smoke: cleanup removed the three test slots"), Removed, 3);
	Test.TestFalse(TEXT("smoke: TestSlot_A deleted"), UGameplayStatics::DoesSaveGameExist(M3_014_SlotAName(), 0));
	Test.TestFalse(TEXT("smoke: TestSlot_B deleted"), UGameplayStatics::DoesSaveGameExist(M3_014_SlotBName(), 0));
	Test.TestFalse(TEXT("smoke: TestSlot_Index deleted"), UGameplayStatics::DoesSaveGameExist(M3_014_IndexSlotName(), 0));
	return true;
}

#endif
