// M3-015: startup load with corruption fallback (interface contract section
// 8). Pins UProfileSaveService::StartupLoad end to end, always on the
// dedicated "TestSlot_" prefix, and the UProfileSubsystem::RestoreFromSave
// integration:
// - a truly fresh prefix (no index, no slot files) reports NoSaveFound - the
//   CALLER decides to run NewProfile (never automatic);
// - a corrupt index-active slot falls back to the other valid slot and the
//   recovery report names the fallback reason;
// - a corrupt or missing index falls back to a generation scan (highest
//   valid generation wins);
// - both slots corrupt => RecoveryError with a per-slot problem list, the
//   corrupt files stay byte-identical (evidence) and the save path refuses
//   to write anything (a corrupt save is never a fresh install);
// - a future-schema slot (version above this build's) is detected BEFORE the
//   integrity check, reported, and guarded: the save path refuses to
//   overwrite that slot;
// - a recovered snapshot restores the full profile subsystem state
//   (identity, Level/XP, inventory, drafts, applied ids).
//
// Storage: every test runs on an in-memory ISaveStorage with direct
// corruption injection (deliberately-wrong integrity digest slots and a
// future-schema slot written straight into storage) - no real file is
// touched. Test doubles and helpers carry unique M3_015 names so this file
// never collides with the M3-014 suite (bUseUnity is off, but names stay
// unique on principle).
//
// Stub-failure note: against the red stubs (StartupLoad always reports
// NoSaveFound, RestoreFromSave always refuses) the fallback/corruption/guard
// tests fail on their concrete expectations while the fresh-state test pins
// the stub contract itself. Exact red/green counts live in the task report.

#include "Misc/AutomationTest.h"

#include "../Persistence/ProfileSaveService.h"
#include "../Profile/ProfileSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_015
{
	// -- Test storage double ------------------------------------------------------

	/**
	 * In-memory ISaveStorage: every written slot is stored as an independent
	 * duplicate (rooted against GC), every read returns a fresh duplicate -
	 * the same semantics as disk bytes (caller mutations never leak into the
	 * stored copy, and a reader can freely tamper its returned object).
	 * Counts WriteSlot calls so tests can pin exactly how many writes one
	 * operation performs (a startup load must perform ZERO).
	 */
	class FM3_015_MemoryStorage final : public ISaveStorage
	{
	public:
		virtual ~FM3_015_MemoryStorage() override
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

	// -- Fixture helpers ------------------------------------------------------------

	/** The three TestSlot_ slot names every test of this suite operates on. */
	static FString M3_015_SlotAName() { return FString(TEXT("TestSlot_A")); }
	static FString M3_015_SlotBName() { return FString(TEXT("TestSlot_B")); }
	static FString M3_015_IndexSlotName() { return FString(TEXT("TestSlot_Index")); }

	/** Fixed character identity of the automation fixture (never all-zero). */
	static FGuid M3_015_CharacterId()
	{
		return FGuid(0xD1E0F0A0u, 0x2468u, 0x1357BDF0u, 0x1A2B3C4Du);
	}

	/**
	 * Builds one deterministic item instance (same binary-exact stat style as
	 * the M3-013/M3-014 fixtures so snapshot strings round trip bit-exactly).
	 */
	static FItemInstance M3_015_MakeInstance(uint32 Seed, const TCHAR* DefinitionName,
		float Attack, float Defense, float MaxHP, int32 ItemLevel)
	{
		FItemInstance Instance;
		Instance.InstanceId = FGuid(0xFADE0000u + Seed, 0xC0FFEEu, 0x00BEADu, Seed * 17u + 3u);
		Instance.DefinitionId = FName(DefinitionName);
		Instance.RollSeed = static_cast<int64>(0x2CDEF0123456789AULL) + static_cast<int64>(Seed);
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
	static FProfileSaveRequest M3_015_MakeRequest(FAutomationTestBase& Test, int32 Level, int32 XP, int32 ItemCount)
	{
		FProfileSaveRequest Request;
		Request.Snapshot.CharacterId = M3_015_CharacterId();
		Request.Snapshot.Level = Level;
		Request.Snapshot.XP = XP;
		Request.Snapshot.MaxHP = UProfileSubsystem::GetMaxHPForLevel(Level);
		Request.Snapshot.Attack = UProfileSubsystem::GetAttackForLevel(Level);
		Request.Snapshot.Defense = UProfileSubsystem::GetDefenseForLevel(Level);
		for (int32 Index = 0; Index < ItemCount; ++Index)
		{
			const FItemInstance Instance = M3_015_MakeInstance(static_cast<uint32>(Level * 110 + Index),
				TEXT("weapon_training"), 2.0f + 0.25f * Index, 0.5f * Index, 5.0f * Index, 1);
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
		Draft.SettlementId = 7100ull + static_cast<uint64>(Level);
		Draft.XP = 50;
		Draft.Items.Add(M3_015_MakeInstance(static_cast<uint32>(9100 + Level), TEXT("armor_leather"), 0.0f, 2.5f, 20.0f, 1));
		Request.PendingRewards.Add(Draft);
		Request.AppliedSettlementIds.Add(8100ull + static_cast<uint64>(Level));
		return Request;
	}

	/** Creates the service on the given prefix (storage optional). */
	static UProfileSaveService* M3_015_NewService(FAutomationTestBase& Test, ISaveStorage* Storage, const TCHAR* Prefix)
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
	 * Asserts the startup outcome carries exactly the request's captured
	 * profile state (identity, progress, recomputed stat row, inventory,
	 * equipment, drafts, applied ids).
	 */
	static void M3_015_ExpectStartupLoadMatchesRequest(FAutomationTestBase& Test,
		const FStartupLoadOutcome& Loaded, const FProfileSaveRequest& Request, const TCHAR* Scenario)
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

	/**
	 * Writes a CORRUPT slot save straight into storage (the on-disk
	 * corruption model): a well-shaped UProfileSlotSaveGame whose stored
	 * integrity digest deliberately does not match its payload. The
	 * distinctive tag values (guid, level, digest, generation) are what the
	 * evidence-preservation tests compare against after a refused save.
	 */
	static bool M3_015_WriteCorruptSlot(ISaveStorage& Storage, const FString& SlotName,
		const FGuid& TagId, int32 TagLevel, int32 TagGeneration, uint32 TagDigest)
	{
		UProfileSlotSaveGame* Corrupt = NewObject<UProfileSlotSaveGame>(GetTransientPackage());
		Corrupt->SchemaVersion = UProfileSaveGame::CurrentSchemaVersion;
		Corrupt->CharacterId = TagId;
		Corrupt->CharacterIdSummary = TagId.ToString();
		Corrupt->Level = TagLevel;
		Corrupt->PayloadInstanceCount = 0;
		Corrupt->PayloadDigest = TagDigest; // deliberately WRONG: the digest gate must reject it
		Corrupt->SlotGeneration = TagGeneration;
		return Storage.WriteSlot(SlotName, Corrupt);
	}

	/**
	 * Writes a FUTURE-SCHEMA slot save (SchemaVersion above this build's)
	 * straight into storage. The digest is never consulted: version detection
	 * must precede the integrity check so an old build cannot misread a newer
	 * build's save as plain corruption and overwrite it.
	 */
	static bool M3_015_WriteFutureSchemaSlot(ISaveStorage& Storage, const FString& SlotName, int32 TagGeneration)
	{
		UProfileSlotSaveGame* Future = NewObject<UProfileSlotSaveGame>(GetTransientPackage());
		Future->SchemaVersion = UProfileSaveGame::CurrentSchemaVersion + 1;
		Future->CharacterId = M3_015_CharacterId();
		Future->CharacterIdSummary = M3_015_CharacterId().ToString();
		Future->Level = 5;
		Future->PayloadInstanceCount = 0;
		Future->PayloadDigest = 0;
		Future->SlotGeneration = TagGeneration;
		return Storage.WriteSlot(SlotName, Future);
	}

	/**
	 * Reads a stored slot save back for the evidence checks (bypassing the
	 * service): the caller inspects the raw stored fields.
	 */
	static UProfileSlotSaveGame* M3_015_ReadRawSlot(ISaveStorage& Storage, const FString& SlotName)
	{
		return Cast<UProfileSlotSaveGame>(Storage.ReadSlot(SlotName));
	}

	/** Reads the raw index object straight from storage (bypassing the service). */
	static UProfileIndexSaveGame* M3_015_ReadRawIndex(ISaveStorage& Storage)
	{
		return Cast<UProfileIndexSaveGame>(Storage.ReadSlot(M3_015_IndexSlotName()));
	}

	/**
	 * Minimal subsystem fixture (the M3-006 precedent): a bare UGameInstance
	 * owner, never Init()ed - the profile starts in the "no profile" state.
	 */
	struct FM3_015_SubsystemFixture
	{
		UGameInstance* Owner = nullptr;
		UProfileSubsystem* Profile = nullptr;

		bool Setup(FAutomationTestBase& Test)
		{
			Owner = NewObject<UGameInstance>(GEngine);
			Profile = Owner ? NewObject<UProfileSubsystem>(Owner) : nullptr;
			if (!Test.TestNotNull(TEXT("the profile subsystem object is constructible without a world"), Profile))
			{
				return false;
			}
			Owner->AddToRoot();
			Profile->AddToRoot();
			return true;
		}

		~FM3_015_SubsystemFixture()
		{
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
	 * Cleanup: deletes the service's test slots and asserts the three
	 * TestSlot_ names are gone afterwards.
	 */
	static void M3_015_CleanupTestSlots(FAutomationTestBase& Test, UProfileSaveService& Service, ISaveStorage& Storage)
	{
		Service.DeleteTestSlots();
		Test.TestTrue(TEXT("cleanup: TestSlot_A deleted"), !Storage.DoesSlotExist(M3_015_SlotAName()));
		Test.TestTrue(TEXT("cleanup: TestSlot_B deleted"), !Storage.DoesSlotExist(M3_015_SlotBName()));
		Test.TestTrue(TEXT("cleanup: TestSlot_Index deleted"), !Storage.DoesSlotExist(M3_015_IndexSlotName()));
	}
}

using namespace UE::UEMMO::Tasks::M3_015;

// -- 1: fresh state => NoSaveFound, then the explicit NewProfile chain ------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_015FreshStateReportsNoSaveFoundThenNewProfileChain,
	"UEMMO.Tasks.M3_015.FreshStateReportsNoSaveFoundThenNewProfileChain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_015FreshStateReportsNoSaveFoundThenNewProfileChain::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_015_MemoryStorage Storage;
	UProfileSaveService* Service = M3_015_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	// A startup load on a completely empty prefix reports NoSaveFound - a
	// fresh state, not corruption, and never an automatic new profile.
	const int32 WritesBefore = Storage.WriteCallCount;
	const FStartupLoadOutcome Fresh = Service->StartupLoad();
	Test.TestEqual(TEXT("a fresh prefix reports NoSaveFound"),
		static_cast<int32>(Fresh.Result), static_cast<int32>(EStartupLoadResult::NoSaveFound));
	Test.TestEqual(TEXT("no slot is reported on a fresh state"), Fresh.SlotIndex, -1);
	Test.TestEqual(TEXT("no generation is reported on a fresh state"), Fresh.Generation, 0);
	Test.TestFalse(TEXT("no CharacterId is restored on a fresh state"), Fresh.Snapshot.CharacterId.IsValid());
	Test.TestEqual(TEXT("no inventory is restored on a fresh state"), Fresh.Inventory.Count(), 0);
	Test.TestTrue(TEXT("the fresh-state report carries a summary"), !Fresh.Summary.IsEmpty());
	Test.TestEqual(TEXT("no corrupt slot is reported on a fresh state"), Fresh.CorruptedSlotReports.Num(), 0);
	Test.TestEqual(TEXT("no future-schema slot is reported on a fresh state"), Fresh.FutureSchemaSlotNames.Num(), 0);
	Test.TestEqual(TEXT("the startup load itself never writes"), Storage.WriteCallCount - WritesBefore, 0);

	// The caller decides: an explicit NewProfile is the only path that creates
	// the level-1 profile (StartupLoad never built one).
	FM3_015_SubsystemFixture Subsystem;
	if (!Subsystem.Setup(Test))
	{
		return true;
	}
	Test.TestFalse(TEXT("the subsystem starts without a profile"), Subsystem.Profile->HasProfile());
	Subsystem.Profile->NewProfile();
	Test.TestTrue(TEXT("the caller's explicit NewProfile creates the profile"), Subsystem.Profile->HasProfile());
	Test.TestEqual(TEXT("the new profile starts at level 1"), Subsystem.Profile->GetLevel(), 1);
	Test.TestEqual(TEXT("the new profile starts at 0 XP"), Subsystem.Profile->GetXP(), 0);
	Test.TestTrue(TEXT("the new profile has a valid identity"), Subsystem.Profile->GetCharacterId().IsValid());

	// The chain works: the level-1 profile is saved and the next startup load
	// recovers it exactly.
	const FProfileSaveRequest Level1Request = M3_015_MakeRequest(Test, 1, 0, 0);
	const FProfileSaveOutcome FirstSave = Service->SaveProfile(Level1Request);
	Test.TestEqual(TEXT("the first save commits"),
		static_cast<int32>(FirstSave.Result), static_cast<int32>(EProfileSaveResult::Success));
	const FStartupLoadOutcome Recovered = Service->StartupLoad();
	Test.TestEqual(TEXT("the next startup load recovers the saved profile"),
		static_cast<int32>(Recovered.Result), static_cast<int32>(EStartupLoadResult::Recovered));
	if (Recovered.Result == EStartupLoadResult::Recovered)
	{
		M3_015_ExpectStartupLoadMatchesRequest(Test, Recovered, Level1Request, TEXT("fresh chain"));
		Test.TestEqual(TEXT("the recovered slot is A"), Recovered.SlotIndex, 0);
		Test.TestEqual(TEXT("the recovered generation is 1"), Recovered.Generation, 1);
		Test.TestTrue(TEXT("a direct load reports no fallback reason"), Recovered.FallbackReason.IsEmpty());
	}

	M3_015_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- 2: corrupt index-active slot => fall back to the other valid slot ------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_015CorruptActiveSlotFallsBackToOtherSlot,
	"UEMMO.Tasks.M3_015.CorruptActiveSlotFallsBackToOtherSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_015CorruptActiveSlotFallsBackToOtherSlot::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_015_MemoryStorage Storage;
	UProfileSaveService* Service = M3_015_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	const FProfileSaveRequest FirstRequest = M3_015_MakeRequest(Test, 2, 10, 2);
	const FProfileSaveRequest SecondRequest = M3_015_MakeRequest(Test, 3, 30, 3);
	Test.TestEqual(TEXT("setup: first save committed to A"),
		static_cast<int32>(Service->SaveProfile(FirstRequest).WrittenSlotIndex), 0);
	Test.TestEqual(TEXT("setup: second save committed to B"),
		static_cast<int32>(Service->SaveProfile(SecondRequest).WrittenSlotIndex), 1);

	// The index-active slot B goes bad ON DISK (deliberately-wrong integrity
	// digest with distinctive tag values).
	const FGuid TagIdB(0x0BAD0000u, 0xB000u, 0xB0000000u, 0x000000B1u);
	Test.TestTrue(TEXT("setup: the corrupt slot B was written"),
		M3_015_WriteCorruptSlot(Storage, M3_015_SlotBName(), TagIdB, 43, 8, 0xBADF00Du));

	const FStartupLoadOutcome Loaded = Service->StartupLoad();
	Test.TestEqual(TEXT("the corrupt active slot falls back to the other slot"),
		static_cast<int32>(Loaded.Result), static_cast<int32>(EStartupLoadResult::RecoveredFallback));
	if (Loaded.Result == EStartupLoadResult::RecoveredFallback)
	{
		M3_015_ExpectStartupLoadMatchesRequest(Test, Loaded, FirstRequest, TEXT("fallback recovery"));
		Test.TestEqual(TEXT("the fallback came from slot A"), Loaded.SlotIndex, 0);
		Test.TestEqual(TEXT("the fallback carries slot A's generation"), Loaded.Generation, 1);
		Test.TestTrue(TEXT("the fallback reason names the corrupt active slot"),
			Loaded.FallbackReason.Contains(M3_015_SlotBName()));
		Test.TestTrue(TEXT("the fallback reason names the failure"), !Loaded.FallbackReason.IsEmpty());
		Test.TestTrue(TEXT("the summary carries the recovery report"), !Loaded.Summary.IsEmpty());
		Test.TestEqual(TEXT("the corrupt slot is in the problem list"), Loaded.CorruptedSlotReports.Num(), 1);
	}

	// Evidence: the corrupt slot B file still exists (a load never deletes).
	Test.TestTrue(TEXT("the corrupt slot B file is preserved"), Storage.DoesSlotExist(M3_015_SlotBName()));

	// With a valid recoverable state the save path is NOT guarded: the next
	// transaction writes the inactive slot (A per the index) and recovers the
	// newest state; the corrupt B stays as evidence.
	const FProfileSaveRequest ThirdRequest = M3_015_MakeRequest(Test, 4, 40, 4);
	const FProfileSaveOutcome ThirdSave = Service->SaveProfile(ThirdRequest);
	Test.TestEqual(TEXT("saving still works after the fallback"),
		static_cast<int32>(ThirdSave.Result), static_cast<int32>(EProfileSaveResult::Success));
	Test.TestEqual(TEXT("the next save writes the inactive slot A"), ThirdSave.WrittenSlotIndex, 0);
	Test.TestEqual(TEXT("the next save commits generation 3"), ThirdSave.Generation, 3);
	const FStartupLoadOutcome Reloaded = Service->StartupLoad();
	Test.TestEqual(TEXT("the newest save loads after the recovery save"),
		static_cast<int32>(Reloaded.Result), static_cast<int32>(EStartupLoadResult::Recovered));
	if (Reloaded.Result == EStartupLoadResult::Recovered)
	{
		M3_015_ExpectStartupLoadMatchesRequest(Test, Reloaded, ThirdRequest, TEXT("after recovery save"));
	}
	Test.TestTrue(TEXT("the corrupt slot B evidence is still on disk"), Storage.DoesSlotExist(M3_015_SlotBName()));

	M3_015_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- 3: corrupt/missing index => generation scan picks the highest valid slot -----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_015CorruptIndexPicksHighestGenerationValidSlot,
	"UEMMO.Tasks.M3_015.CorruptIndexPicksHighestGenerationValidSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_015CorruptIndexPicksHighestGenerationValidSlot::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_015_MemoryStorage Storage;
	UProfileSaveService* Service = M3_015_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	const FProfileSaveRequest FirstRequest = M3_015_MakeRequest(Test, 2, 10, 2);
	const FProfileSaveRequest SecondRequest = M3_015_MakeRequest(Test, 3, 30, 3);
	Test.TestEqual(TEXT("setup: first save committed"),
		static_cast<int32>(Service->SaveProfile(FirstRequest).Result), static_cast<int32>(EProfileSaveResult::Success));
	Test.TestEqual(TEXT("setup: second save committed"),
		static_cast<int32>(Service->SaveProfile(SecondRequest).Result), static_cast<int32>(EProfileSaveResult::Success));

	// The index exists but its content is garbage (invalid active slot, wrong
	// array size) - the scan must compare the SLOT payloads' generations.
	UProfileIndexSaveGame* Bogus = NewObject<UProfileIndexSaveGame>(GetTransientPackage());
	Bogus->ActiveSlotIndex = 7;
	Bogus->SlotGenerations.Add(1);
	Test.TestTrue(TEXT("setup: the corrupt index was written"), Storage.WriteSlot(M3_015_IndexSlotName(), Bogus));

	const FStartupLoadOutcome Loaded = Service->StartupLoad();
	Test.TestEqual(TEXT("the corrupt index falls back to the generation scan"),
		static_cast<int32>(Loaded.Result), static_cast<int32>(EStartupLoadResult::RecoveredFallback));
	if (Loaded.Result == EStartupLoadResult::RecoveredFallback)
	{
		// Slot A carries generation 1, slot B generation 2: B must win.
		M3_015_ExpectStartupLoadMatchesRequest(Test, Loaded, SecondRequest, TEXT("scan recovery"));
		Test.TestEqual(TEXT("the scan picked the highest generation slot B"), Loaded.SlotIndex, 1);
		Test.TestEqual(TEXT("the scan reports generation 2"), Loaded.Generation, 2);
		Test.TestTrue(TEXT("the index problem is reported"), !Loaded.IndexProblem.IsEmpty());
		Test.TestTrue(TEXT("the fallback reason names the index problem"), !Loaded.FallbackReason.IsEmpty());
		Test.TestTrue(TEXT("the summary carries the recovery report"), !Loaded.Summary.IsEmpty());
	}

	// Second flavor: the index file is MISSING entirely - same scan, same
	// winner, but the reason names the missing index.
	Test.TestTrue(TEXT("setup: the index slot was deleted"), Storage.DeleteSlot(M3_015_IndexSlotName()));
	const FStartupLoadOutcome LoadedMissing = Service->StartupLoad();
	Test.TestEqual(TEXT("the missing index falls back to the generation scan"),
		static_cast<int32>(LoadedMissing.Result), static_cast<int32>(EStartupLoadResult::RecoveredFallback));
	if (LoadedMissing.Result == EStartupLoadResult::RecoveredFallback)
	{
		M3_015_ExpectStartupLoadMatchesRequest(Test, LoadedMissing, SecondRequest, TEXT("missing index recovery"));
		Test.TestEqual(TEXT("the missing-index scan picked slot B"), LoadedMissing.SlotIndex, 1);
		Test.TestEqual(TEXT("the missing-index scan reports generation 2"), LoadedMissing.Generation, 2);
		Test.TestTrue(TEXT("the missing-index fallback reason is reported"), !LoadedMissing.FallbackReason.IsEmpty());
	}

	// A save after the index loss rebuilds the index from the surviving slot
	// payloads and protects the surviving copy (M3-014 semantics preserved).
	const FProfileSaveRequest ThirdRequest = M3_015_MakeRequest(Test, 4, 40, 4);
	const FProfileSaveOutcome ThirdSave = Service->SaveProfile(ThirdRequest);
	Test.TestEqual(TEXT("saving still works after the index loss"),
		static_cast<int32>(ThirdSave.Result), static_cast<int32>(EProfileSaveResult::Success));
	Test.TestEqual(TEXT("the rebuilt save went to slot A with generation 3"),
		ThirdSave.Generation, 3);
	UProfileIndexSaveGame* RawIndex = M3_015_ReadRawIndex(Storage);
	Test.TestNotNull(TEXT("the index was rebuilt"), RawIndex);
	if (RawIndex)
	{
		Test.TestEqual(TEXT("the rebuilt index points at slot A"), RawIndex->ActiveSlotIndex, 0);
	}

	M3_015_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- 4: both slots corrupt => RecoveryError, evidence preserved, writes refused ---

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_015BothSlotsCorruptPreservesEvidenceAndRefusesWrites,
	"UEMMO.Tasks.M3_015.BothSlotsCorruptPreservesEvidenceAndRefusesWrites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_015BothSlotsCorruptPreservesEvidenceAndRefusesWrites::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_015_MemoryStorage Storage;
	UProfileSaveService* Service = M3_015_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	const FProfileSaveRequest FirstRequest = M3_015_MakeRequest(Test, 2, 10, 2);
	const FProfileSaveRequest SecondRequest = M3_015_MakeRequest(Test, 3, 30, 3);
	Test.TestEqual(TEXT("setup: first save committed"),
		static_cast<int32>(Service->SaveProfile(FirstRequest).Result), static_cast<int32>(EProfileSaveResult::Success));
	Test.TestEqual(TEXT("setup: second save committed"),
		static_cast<int32>(Service->SaveProfile(SecondRequest).Result), static_cast<int32>(EProfileSaveResult::Success));

	// BOTH slot files go bad on disk, each with distinctive evidence fields.
	const FGuid TagIdA(0x0BAD0000u, 0xA000u, 0xA0000000u, 0x000000A1u);
	const FGuid TagIdB(0x0BAD0000u, 0xB000u, 0xB0000000u, 0x000000B1u);
	Test.TestTrue(TEXT("setup: the corrupt slot A was written"),
		M3_015_WriteCorruptSlot(Storage, M3_015_SlotAName(), TagIdA, 42, 7, 0xBADF00Du));
	Test.TestTrue(TEXT("setup: the corrupt slot B was written"),
		M3_015_WriteCorruptSlot(Storage, M3_015_SlotBName(), TagIdB, 43, 8, 0xFEEDFACEu));

	const int32 WritesBefore = Storage.WriteCallCount;
	const FStartupLoadOutcome Loaded = Service->StartupLoad();
	Test.TestEqual(TEXT("the startup load itself never writes"), Storage.WriteCallCount - WritesBefore, 0);

	// The startup pass reports an explicit RECOVERY ERROR - never NoSaveFound
	// (a corrupt save must not masquerade as a fresh install) and never a
	// restored profile.
	Test.TestEqual(TEXT("two corrupt slots report RecoveryError"),
		static_cast<int32>(Loaded.Result), static_cast<int32>(EStartupLoadResult::RecoveryError));
	Test.TestTrue(TEXT("RecoveryError is not a fresh state"),
		Loaded.Result != EStartupLoadResult::NoSaveFound);
	Test.TestEqual(TEXT("no slot is reported on the recovery error"), Loaded.SlotIndex, -1);
	Test.TestEqual(TEXT("no generation is reported on the recovery error"), Loaded.Generation, 0);
	Test.TestFalse(TEXT("no CharacterId is restored on the recovery error"), Loaded.Snapshot.CharacterId.IsValid());
	Test.TestEqual(TEXT("no inventory is restored on the recovery error"), Loaded.Inventory.Count(), 0);
	Test.TestEqual(TEXT("no drafts are restored on the recovery error"), Loaded.PendingRewards.Num(), 0);
	Test.TestEqual(TEXT("no applied ids are restored on the recovery error"), Loaded.AppliedSettlementIds.Num(), 0);
	Test.TestEqual(TEXT("the problem list names both corrupt slots"), Loaded.CorruptedSlotReports.Num(), 2);
	Test.TestTrue(TEXT("the recovery error carries a summary"), !Loaded.Summary.IsEmpty());

	// The save path is now GUARDED: the transaction refuses to overwrite the
	// corrupt evidence - nothing at all is written (no slot, no index).
	const FProfileSaveRequest RefusedRequest = M3_015_MakeRequest(Test, 4, 40, 4);
	const FProfileSaveOutcome Refused = Service->SaveProfile(RefusedRequest);
	Test.TestEqual(TEXT("saving with two corrupt slots is refused by the write guard"),
		static_cast<int32>(Refused.Result), static_cast<int32>(EProfileSaveResult::FailedGuardedSlot));
	Test.TestTrue(TEXT("the refusal names the guarded slot and reason"), !Refused.Message.IsEmpty());
	Test.TestEqual(TEXT("the refused save wrote nothing at all"), Storage.WriteCallCount - WritesBefore, 0);

	// Evidence check: both corrupt files still exist with byte-identical tag
	// fields (nothing was overwritten or wiped).
	UProfileSlotSaveGame* RawA = M3_015_ReadRawSlot(Storage, M3_015_SlotAName());
	Test.TestNotNull(TEXT("the corrupt slot A evidence is preserved"), RawA);
	if (RawA)
	{
		Test.TestEqual(TEXT("slot A evidence: level tag unchanged"), RawA->Level, 42);
		Test.TestTrue(TEXT("slot A evidence: character id tag unchanged"), RawA->CharacterId == TagIdA);
		Test.TestEqual(TEXT("slot A evidence: digest tag unchanged"), RawA->PayloadDigest, 0xBADF00Du);
		Test.TestEqual(TEXT("slot A evidence: generation tag unchanged"), RawA->SlotGeneration, 7);
	}
	UProfileSlotSaveGame* RawB = M3_015_ReadRawSlot(Storage, M3_015_SlotBName());
	Test.TestNotNull(TEXT("the corrupt slot B evidence is preserved"), RawB);
	if (RawB)
	{
		Test.TestEqual(TEXT("slot B evidence: level tag unchanged"), RawB->Level, 43);
		Test.TestTrue(TEXT("slot B evidence: character id tag unchanged"), RawB->CharacterId == TagIdB);
		Test.TestEqual(TEXT("slot B evidence: digest tag unchanged"), RawB->PayloadDigest, 0xFEEDFACEu);
		Test.TestEqual(TEXT("slot B evidence: generation tag unchanged"), RawB->SlotGeneration, 8);
	}
	UProfileIndexSaveGame* RawIndex = M3_015_ReadRawIndex(Storage);
	Test.TestNotNull(TEXT("the index file is preserved"), RawIndex);
	if (RawIndex)
	{
		Test.TestEqual(TEXT("the index still points where it pointed"), RawIndex->ActiveSlotIndex, 1);
	}

	// Repeated startup loads stay consistent (the guard re-arms identically).
	const FStartupLoadOutcome Reloaded = Service->StartupLoad();
	Test.TestEqual(TEXT("a repeated startup load still reports the recovery error"),
		static_cast<int32>(Reloaded.Result), static_cast<int32>(EStartupLoadResult::RecoveryError));

	M3_015_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- 5: future schema => explicit rejection + the save path refuses overwrite -----

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_015FutureSchemaSlotRejectedAndGuardedFromOverwrite,
	"UEMMO.Tasks.M3_015.FutureSchemaSlotRejectedAndGuardedFromOverwrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_015FutureSchemaSlotRejectedAndGuardedFromOverwrite::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;

	// Flavor 1: the index-active slot is valid, the INACTIVE slot holds a
	// future-schema save. The load recovers the active slot, but the
	// future-schema file is reported and guarded.
	FM3_015_MemoryStorage Storage1;
	UProfileSaveService* Service1 = M3_015_NewService(Test, &Storage1, TEXT("TestSlot_"));
	if (!Service1)
	{
		return true;
	}
	const FProfileSaveRequest FirstRequest = M3_015_MakeRequest(Test, 2, 10, 2);
	Test.TestEqual(TEXT("flavor 1 setup: the save committed to A"),
		static_cast<int32>(Service1->SaveProfile(FirstRequest).WrittenSlotIndex), 0);
	Test.TestTrue(TEXT("flavor 1 setup: the future-schema slot B was written"),
		M3_015_WriteFutureSchemaSlot(Storage1, M3_015_SlotBName(), 99));

	const FStartupLoadOutcome Loaded1 = Service1->StartupLoad();
	Test.TestEqual(TEXT("flavor 1: the valid active slot still loads"),
		static_cast<int32>(Loaded1.Result), static_cast<int32>(EStartupLoadResult::Recovered));
	Test.TestEqual(TEXT("flavor 1: the future-schema slot is reported"),
		Loaded1.FutureSchemaSlotNames.Num() == 1 && Loaded1.FutureSchemaSlotNames.Contains(M3_015_SlotBName()), true);

	const FProfileSaveRequest RefusedRequest = M3_015_MakeRequest(Test, 3, 30, 3);
	const FProfileSaveOutcome Refused1 = Service1->SaveProfile(RefusedRequest);
	Test.TestEqual(TEXT("flavor 1: the save refusing to overwrite the future-schema slot"),
		static_cast<int32>(Refused1.Result), static_cast<int32>(EProfileSaveResult::FailedGuardedSlot));
	Test.TestTrue(TEXT("flavor 1: the refusal names the schema reason"), !Refused1.Message.IsEmpty());
	UProfileSlotSaveGame* RawFuture1 = M3_015_ReadRawSlot(Storage1, M3_015_SlotBName());
	Test.TestNotNull(TEXT("flavor 1: the future-schema file is preserved"), RawFuture1);
	if (RawFuture1)
	{
		Test.TestEqual(TEXT("flavor 1: the future-schema file keeps its version"),
			RawFuture1->SchemaVersion, UProfileSaveGame::CurrentSchemaVersion + 1);
		Test.TestEqual(TEXT("flavor 1: the future-schema file keeps its generation tag"), RawFuture1->SlotGeneration, 99);
	}
	M3_015_CleanupTestSlots(Test, *Service1, Storage1);

	// Flavor 2: the index-ACTIVE slot holds the future schema (the card's
	// literal case: load meets version > 1 => recovery error + guard). The
	// other slot does not exist, so the next save recovers into it while the
	// future-schema file stays untouched.
	FM3_015_MemoryStorage Storage2;
	UProfileSaveService* Service2 = M3_015_NewService(Test, &Storage2, TEXT("TestSlot_"));
	if (!Service2)
	{
		return true;
	}
	Test.TestEqual(TEXT("flavor 2 setup: the save committed to A"),
		static_cast<int32>(Service2->SaveProfile(FirstRequest).WrittenSlotIndex), 0);
	Test.TestTrue(TEXT("flavor 2 setup: the active slot A was replaced with a future-schema save"),
		M3_015_WriteFutureSchemaSlot(Storage2, M3_015_SlotAName(), 99));

	const FStartupLoadOutcome Loaded2 = Service2->StartupLoad();
	Test.TestEqual(TEXT("flavor 2: a future-schema active slot reports the recovery error"),
		static_cast<int32>(Loaded2.Result), static_cast<int32>(EStartupLoadResult::RecoveryError));
	Test.TestTrue(TEXT("flavor 2: the recovery error is not a fresh state"),
		Loaded2.Result != EStartupLoadResult::NoSaveFound);
	Test.TestEqual(TEXT("flavor 2: the future-schema slot is reported"),
		Loaded2.FutureSchemaSlotNames.Num() == 1 && Loaded2.FutureSchemaSlotNames.Contains(M3_015_SlotAName()), true);
	Test.TestEqual(TEXT("flavor 2: the future-schema slot is in the problem list"), Loaded2.CorruptedSlotReports.Num(), 1);
	Test.TestTrue(TEXT("flavor 2: the summary carries the recovery report"), !Loaded2.Summary.IsEmpty());

	// The unguarded other slot may still receive the transaction save: the
	// future-schema file A is not the target.
	const FProfileSaveRequest RecoverySaveRequest = M3_015_MakeRequest(Test, 3, 30, 3);
	const FProfileSaveOutcome RecoverySave = Service2->SaveProfile(RecoverySaveRequest);
	Test.TestEqual(TEXT("flavor 2: the save recovers into the unguarded slot B"),
		static_cast<int32>(RecoverySave.Result), static_cast<int32>(EProfileSaveResult::Success));
	Test.TestEqual(TEXT("flavor 2: the recovery save went to slot B"), RecoverySave.WrittenSlotIndex, 1);

	// The guard re-arms on the next startup load and keeps protecting the
	// future-schema slot for the whole session.
	const FStartupLoadOutcome Reloaded2 = Service2->StartupLoad();
	Test.TestEqual(TEXT("flavor 2: the next startup load recovers slot B"),
		static_cast<int32>(Reloaded2.Result), static_cast<int32>(EStartupLoadResult::Recovered));
	Test.TestEqual(TEXT("flavor 2: the future-schema slot A is still reported"),
		Reloaded2.FutureSchemaSlotNames.Num() == 1 && Reloaded2.FutureSchemaSlotNames.Contains(M3_015_SlotAName()), true);

	const FProfileSaveRequest LaterRequest = M3_015_MakeRequest(Test, 4, 40, 4);
	const FProfileSaveOutcome Refused2 = Service2->SaveProfile(LaterRequest);
	Test.TestEqual(TEXT("flavor 2: the next save (target A) is refused by the guard"),
		static_cast<int32>(Refused2.Result), static_cast<int32>(EProfileSaveResult::FailedGuardedSlot));
	UProfileSlotSaveGame* RawFuture2 = M3_015_ReadRawSlot(Storage2, M3_015_SlotAName());
	Test.TestNotNull(TEXT("flavor 2: the future-schema file A is still preserved"), RawFuture2);
	if (RawFuture2)
	{
		Test.TestEqual(TEXT("flavor 2: the future-schema file A keeps its version"),
			RawFuture2->SchemaVersion, UProfileSaveGame::CurrentSchemaVersion + 1);
	}

	M3_015_CleanupTestSlots(Test, *Service2, Storage2);
	return true;
}

// -- 6: a recovered snapshot restores the full profile subsystem state ------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_015RecoveredOutcomeRestoresProfileSubsystemState,
	"UEMMO.Tasks.M3_015.RecoveredOutcomeRestoresProfileSubsystemState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_015RecoveredOutcomeRestoresProfileSubsystemState::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_015_MemoryStorage Storage;
	UProfileSaveService* Service = M3_015_NewService(Test, &Storage, TEXT("TestSlot_"));
	if (!Service)
	{
		return true;
	}

	const FProfileSaveRequest SavedRequest = M3_015_MakeRequest(Test, 3, 45, 3);
	Test.TestEqual(TEXT("setup: the save committed"),
		static_cast<int32>(Service->SaveProfile(SavedRequest).Result), static_cast<int32>(EProfileSaveResult::Success));

	FM3_015_SubsystemFixture Subsystem;
	if (!Subsystem.Setup(Test))
	{
		return true;
	}
	Test.TestFalse(TEXT("the subsystem starts without a profile"), Subsystem.Profile->HasProfile());

	// Rejection flavor: an invalid snapshot changes NOTHING (all-or-nothing).
	const FStartupLoadOutcome Nothing;
	Test.TestFalse(TEXT("RestoreFromSave rejects a snapshot without a valid CharacterId"),
		Subsystem.Profile->RestoreFromSave(Nothing.Snapshot, Nothing.Inventory, Nothing.PendingRewards,
			Nothing.AppliedSettlementIds, Nothing.EquippedMap));
	Test.TestFalse(TEXT("the rejected restore left the subsystem without a profile"), Subsystem.Profile->HasProfile());

	// The startup load recovers the saved snapshot and the restore applies it
	// in one piece.
	const FStartupLoadOutcome Loaded = Service->StartupLoad();
	Test.TestEqual(TEXT("the startup load recovers the saved profile"),
		static_cast<int32>(Loaded.Result), static_cast<int32>(EStartupLoadResult::Recovered));
	if (Loaded.Result != EStartupLoadResult::Recovered)
	{
		M3_015_CleanupTestSlots(Test, *Service, Storage);
		return true;
	}
	Test.TestTrue(TEXT("RestoreFromSave accepts the recovered snapshot"),
		Subsystem.Profile->RestoreFromSave(Loaded.Snapshot, Loaded.Inventory, Loaded.PendingRewards,
			Loaded.AppliedSettlementIds, Loaded.EquippedMap));

	// The subsystem state now matches the save exactly - identity included
	// (a restore never mints a new id; only NewProfile does).
	Test.TestTrue(TEXT("the subsystem has a profile after the restore"), Subsystem.Profile->HasProfile());
	Test.TestTrue(TEXT("the restored CharacterId is the saved one"),
		Subsystem.Profile->GetCharacterId() == M3_015_CharacterId());
	Test.TestEqual(TEXT("the restored level is the saved level"), Subsystem.Profile->GetLevel(), 3);
	Test.TestEqual(TEXT("the restored XP is the saved XP"), Subsystem.Profile->GetXP(), 45);
	Test.TestEqual(TEXT("the restored inventory count matches"), Subsystem.Profile->GetInventory().Count(), 3);
	for (const FItemInstance& Expected : SavedRequest.Inventory.GetAll())
	{
		Test.TestTrue(FString::Printf(TEXT("the restored inventory keeps %s"), *Expected.InstanceId.ToString()),
			Subsystem.Profile->GetInventory().Contains(Expected.InstanceId));
	}
	Test.TestEqual(TEXT("the restored draft count matches"), Subsystem.Profile->GetPendingRewards().Num(), 1);
	if (Subsystem.Profile->GetPendingRewards().Num() == 1)
	{
		const FPendingReward& Draft = Subsystem.Profile->GetPendingRewards()[0];
		Test.TestEqual(TEXT("the restored draft keeps its settlement id"),
			Draft.SettlementId, 7100ull + 3ull);
		Test.TestEqual(TEXT("the restored draft keeps its XP"), Draft.XP, 50);
		Test.TestEqual(TEXT("the restored draft keeps its item count"), Draft.Items.Num(), 1);
	}
	Test.TestTrue(TEXT("the restored applied id is marked applied"),
		Subsystem.Profile->IsSettlementApplied(8100ull + 3ull));
	Test.TestFalse(TEXT("an unrelated settlement id is not applied"),
		Subsystem.Profile->IsSettlementApplied(99999ull));

	// The read-only snapshot mirrors the restored state.
	const FProfileSnapshot Snapshot = Subsystem.Profile->GetProfileSnapshot();
	Test.TestEqual(TEXT("the snapshot mirrors the restored level"), Snapshot.Level, 3);
	Test.TestEqual(TEXT("the snapshot mirrors the restored XP"), Snapshot.XP, 45);
	Test.TestEqual(TEXT("the snapshot carries the restored inventory copy"), Snapshot.Inventory.Count(), 3);

	M3_015_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

#endif
