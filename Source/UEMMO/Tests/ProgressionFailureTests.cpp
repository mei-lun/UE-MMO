// M3-021: the centralized FAILURE regression of the growth loop. Six failure
// classes, one independent test slot each, every one a readable re-pin of a
// previously verified semantic through the REAL service entries (no product
// source is touched here; the "failure" is injected into test doubles or
// corrupted test copies - the player's real "Profile_" saves are unreachable
// by construction because every service runs on the dedicated "M3_021Slot_"
// prefix over an in-memory storage):
//
//   1. FULL BAG (M3-009 semantics): a claim into a full 30-slot inventory
//      answers InventoryFull (never a claim success shape), retains the
//      pending item with its original InstanceId, grants the settlement XP
//      exactly once and commits the retention state; a restart keeps the
//      retained draft field-for-field and freeing one slot lets the SAME
//      pre-generated instance claim with NO second XP.
//   2. CORRUPT SAVE (M3-015 semantics): a tampered index-active slot payload
//      falls back to the other valid slot (the recovery report names the
//      fallback reason and the corrupt file stays as evidence); BOTH slots
//      corrupt => RecoveryError with a per-slot problem list, the evidence
//      stays byte-identical, the save path refuses to write and the caller
//      never receives a silently minted profile.
//   3. DUPLICATE SETTLEMENT (M3-008/M3-016 semantics): replaying BeginReward
//      answers AlreadyApplied with the ORIGINAL draft (no re-roll) and a
//      replayed claim answers AlreadyClaimed - the XP and the item set can
//      never double.
//   4. EXIT MID-WAVE (M2-011 semantics): LeaveRoom during wave generation
//      zeroes the pending births, produces no late spawn (injected far-future
//      clock OR real ticks), fires no settlement end event and StartRoom
//      re-enters into a fully working fresh run.
//   5. QUICK CLICKS (M2-012/M3-018 semantics): of two fast requests only the
//      FIRST is processed by the one-shot guard - the claim half drives the
//      production wiring shape (FRoomResultActionGuard + the real
//      URewardService::ClaimPendingAtomic), the retry half drives the REAL
//      APrototypeHUD::HandleRetryRequested path; the duplicate is dropped
//      (and counted) with no second XP, no second item, no second restart.
//   6. INTERRUPTED SAVE (M3-016 semantics): the three claim-snapshot save
//      interrupt windows (before the slot write, after the slot write before
//      the index commit, at the index commit as a torn index) each restart
//      idempotently - the total XP across the failure is exactly the design
//      50 and exactly the ORIGINAL pre-generated instance, never a duplicate.
//
// Every test compares the combined item-id set (inventory + all pending
// drafts) and the XP against the pre-failure snapshot: after each failure the
// set is exactly the expected one (nothing lost, nothing invented, nothing
// duplicated) and the XP moved by exactly 50 or exactly 0 (the M3-021 card
// invariant). Cleanup deletes ONLY this suite's own M3_021Slot_ paths.
//
// Harness: the file-local copies of the M3-014/015/016/020 scaffolding carry
// unique M3_021 names (the original suites are never modified) and the world
// tests reuse the engine FTestWorldWrapper precedent (a manually ticked temp
// world with a real game instance, real ticks, real walking physics and the
// injected session clock driven alongside every tick).

#include "Misc/AutomationTest.h"

#include "../Combat/HealthComponent.h"
#include "../Enemy/EnemyDefinition.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Items/InventoryModel.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../Persistence/ProfileSaveService.h"
#include "../Profile/ProfileSubsystem.h"
#include "../Profile/RewardService.h"
#include "../PrototypeCharacter.h"
#include "../PrototypeHUD.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomResult.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../UI/RoomResultWidget.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Tests/AutomationCommon.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_021
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M3_021_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp worlds can never collide with the other
	// suites' arenas. X horizontal, Y depth, Z height; the floor top sits at
	// the base Z.
	const FVector M3_021_SceneBase(104000.0, 81000.0, 600.0);

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M3_021_FloorHalfThickness = 100.0f;
	const float M3_021_FloorHalfExtentXY = 4000.0f;

	// Spawn heights above the floor top.
	const float M3_021_PlayerSpawnHeight = 120.0f;
	const float M3_021_EnemySpawnHeight = 90.0f;

	// The wave spawner's slot interval and the session's inter-wave wait (the
	// M2-007/M2-008 production constants).
	constexpr double M3_021_SpawnIntervalSeconds = 0.3;
	constexpr double M3_021_WaveGapSeconds = 1.0;

	// Loose birth-location band of the kill helper (cm), the M2-014 tolerance.
	const double M3_021_BirthLocationBandCm = 300.0;

	// Frame caps: the settle phase waits for the ground contact.
	constexpr int32 M3_021_MaxSettleFrames = 300;

	// The isolated save prefix of every service operation in this suite (never
	// "Profile_": the automation cannot reach the player's real saves).
	const TCHAR* M3_021_SlotPrefix = TEXT("M3_021Slot_");

	// The three M3_021Slot_ slot names every storage-backed test operates on.
	static FString M3_021_SlotAName() { return FString(TEXT("M3_021Slot_A")); }
	static FString M3_021_SlotBName() { return FString(TEXT("M3_021Slot_B")); }
	static FString M3_021_IndexSlotName() { return FString(TEXT("M3_021Slot_Index")); }

	// -- Storage doubles (the M3-016 copies, renamed) ----------------------------

	/**
	 * In-memory ISaveStorage: every written slot is stored as an independent
	 * duplicate (rooted against GC), every read returns a fresh duplicate -
	 * the same semantics as disk bytes. Counts WriteSlot calls so the crash
	 * windows can be pinned to exact write ordinals.
	 */
	class FM3_021_MemoryStorage final : public ISaveStorage
	{
	public:
		virtual ~FM3_021_MemoryStorage() override
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
	 * Failure injection decorator over another storage (the M3-016 model).
	 * The atomic claim performs exactly two saves (draft persistence, claim
	 * snapshot), each = one slot write + one index commit, so with a fresh
	 * prefix the write ordinals are 1 = draft slot, 2 = draft index, 3 =
	 * claim-snapshot slot, 4 = claim-snapshot index. The three interrupt
	 * windows of the card live on the claim-snapshot save: FailWriteOrdinal 3
	 * (crash BEFORE the slot write), FailWriteOrdinal 4 (crash AFTER the slot
	 * write, the index never commits), TearIndexWriteOrdinal 4 (the index
	 * commit tears - the file ends up garbage the startup scan must survive).
	 * Refused calls still consume an ordinal.
	 */
	class FM3_021_CrashStorage final : public ISaveStorage
	{
	public:
		explicit FM3_021_CrashStorage(ISaveStorage* InInner)
			: Inner(InInner)
		{
		}

		virtual bool WriteSlot(const FString& SlotName, USaveGame* Data) override
		{
			++WriteCounter;
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

	private:
		ISaveStorage* Inner = nullptr;
	};

	// -- Service fixture helpers (the M3-015/M3-016 copies, renamed) --------------

	/** Creates a save service on the M3_021Slot_ prefix (storage optional). */
	static UProfileSaveService* M3_021_NewService(FAutomationTestBase& Test, ISaveStorage* Storage)
	{
		UProfileSaveService* Service = NewObject<UProfileSaveService>(GetTransientPackage());
		if (!Service)
		{
			Test.AddError(TEXT("setup: NewObject<UProfileSaveService> returned null"));
			return nullptr;
		}
		if (!Service->Initialize(M3_021_SlotPrefix))
		{
			Test.AddError(FString::Printf(TEXT("setup: Initialize('%s') rejected the prefix"), M3_021_SlotPrefix));
			return nullptr;
		}
		if (Storage)
		{
			Service->SetStorage(Storage);
		}
		return Service;
	}

	/**
	 * Bare profile fixture (the M3-016 precedent): a UGameInstance owner, a
	 * UProfileSubsystem and a URewardService, all rooted, none Init()ed - no
	 * world is needed because ClaimPendingAtomic takes the profile and the
	 * save service explicitly. Setup mints a fresh level-1 profile and binds
	 * the reward service to it.
	 */
	struct FM3_021_ProfileFixture
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

		~FM3_021_ProfileFixture()
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
	 * Bare subsystem fixture WITHOUT a profile (the M3-015 precedent): the
	 * both-slots-corrupt flavor proves the recovery error never mints a
	 * profile by itself - the caller's subsystem stays empty unless the
	 * caller explicitly runs NewProfile.
	 */
	struct FM3_021_BareSubsystemFixture
	{
		UGameInstance* Owner = nullptr;
		UProfileSubsystem* Profile = nullptr;

		bool Setup(FAutomationTestBase& Test)
		{
			Owner = NewObject<UGameInstance>(GEngine);
			Profile = Owner ? NewObject<UProfileSubsystem>(Owner) : nullptr;
			if (!Test.TestNotNull(TEXT("the bare profile subsystem is constructible without a world"), Profile))
			{
				return false;
			}
			Owner->AddToRoot();
			Profile->AddToRoot();
			return true;
		}

		~FM3_021_BareSubsystemFixture()
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
	 * The RESTART state (the M3-016 model): a brand-new save service over the
	 * same storage plus a brand-new profile whose memory is filled ONLY by
	 * RestoreFromSave from the startup load's recovered save - every restart
	 * assertion is decided from this persisted state alone, never from any
	 * earlier in-memory history.
	 */
	struct FM3_021_RestartedState
	{
		UGameInstance* Owner = nullptr;
		UProfileSaveService* SaveService = nullptr;
		UProfileSubsystem* Profile = nullptr;
		URewardService* Reward = nullptr;
		EStartupLoadResult LoadResult = EStartupLoadResult::RecoveryError;

		bool Setup(FAutomationTestBase& Test, ISaveStorage& Storage, const TCHAR* Scenario)
		{
			SaveService = M3_021_NewService(Test, &Storage);
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

		~FM3_021_RestartedState()
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
	 * Cleanup: deletes the service's test slots and asserts the three
	 * M3_021Slot_ names are gone afterwards (nothing outside the prefix is
	 * reachable, so repeat runs accumulate nothing and the player's real
	 * saves are untouched by construction).
	 */
	static void M3_021_CleanupTestSlots(FAutomationTestBase& Test, UProfileSaveService& Service, ISaveStorage& Storage)
	{
		Service.DeleteTestSlots();
		Test.TestTrue(TEXT("cleanup: M3_021Slot_A deleted"), !Storage.DoesSlotExist(M3_021_SlotAName()));
		Test.TestTrue(TEXT("cleanup: M3_021Slot_B deleted"), !Storage.DoesSlotExist(M3_021_SlotBName()));
		Test.TestTrue(TEXT("cleanup: M3_021Slot_Index deleted"), !Storage.DoesSlotExist(M3_021_IndexSlotName()));
	}

	// -- Item fixtures ---------------------------------------------------------

	/** The three training definitions in a fresh catalog (mirrors Data/items.json). */
	static FItemDefinitionCatalog M3_021_MakeTrainingCatalog()
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

	/** A local filler definition (never added to any catalog) for capacity setup. */
	static FItemDefinition M3_021_MakeFillerDefinition()
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
	static TArray<FGuid> M3_021_FillInventory(FInventoryModel& Inventory, const FItemDefinition& Definition, int32 Count)
	{
		TArray<FGuid> FillerIds;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FItemInstance Filler = MakeItemInstance(Definition, /*RollSeed*/ 61000 + Index);
			FillerIds.Add(Filler.InstanceId);
			Inventory.TryAdd(Filler);
		}
		return FillerIds;
	}

	/** Field-for-field instance equality (identity, definition, roll seed, level, stats). */
	static bool M3_021_InstancesEqual(const FItemInstance& A, const FItemInstance& B)
	{
		return A.InstanceId == B.InstanceId &&
			A.DefinitionId == B.DefinitionId &&
			A.RollSeed == B.RollSeed &&
			A.Level == B.Level &&
			A.RolledStats.Attack == B.RolledStats.Attack &&
			A.RolledStats.Defense == B.RolledStats.Defense &&
			A.RolledStats.MaxHP == B.RolledStats.MaxHP;
	}

	/** A finished-run result value; RunId is kept distinct alongside SettlementId. */
	static FRoomResult M3_021_MakeResult(uint64 RunId, uint64 SettlementId, int32 Seed, bool bCleared)
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

	// -- The card invariant helpers (item set + XP comparison) --------------------

	/**
	 * One progress snapshot: the XP plus the COMBINED item-id set of the
	 * inventory AND every pending draft. The combined set is the loss/duplication
	 * detector: an item that vanishes from a draft without entering the
	 * inventory (or a second copy of a granted instance) moves the set.
	 */
	struct FM3_021_ProgressState
	{
		int32 XP = 0;
		int32 PendingDraftCount = 0;
		TSet<FGuid> AllItemIds;
	};

	static FM3_021_ProgressState M3_021_CaptureProgressState(UProfileSubsystem& Profile)
	{
		FM3_021_ProgressState State;
		State.XP = Profile.GetXP();
		State.PendingDraftCount = Profile.GetPendingRewards().Num();
		for (const FItemInstance& Item : Profile.GetInventory().GetAll())
		{
			State.AllItemIds.Add(Item.InstanceId);
		}
		for (const FPendingReward& Draft : Profile.GetPendingRewards())
		{
			for (const FItemInstance& Item : Draft.Items)
			{
				State.AllItemIds.Add(Item.InstanceId);
			}
		}
		return State;
	}

	/**
	 * The card invariant, asserted after every failure: the XP moved by
	 * EXACTLY ExpectedXPDelta (50 or 0 on the failure paths) and the combined
	 * item-id set equals Before + ExpectedAddedIds - nothing lost, nothing
	 * invented, nothing duplicated. ExpectedAddedIds lists only ids that were
	 * NOT in the combined set before: a claim MOVES the pre-generated
	 * instance from its draft into the inventory (the id stays in the
	 * combined set), so a claim path passes {} here and asserts the
	 * inventory/draft membership explicitly next to the call.
	 */
	static void M3_021_ExpectXPAndItems(FAutomationTestBase& Test, const FM3_021_ProgressState& Before,
		UProfileSubsystem& Profile, int32 ExpectedXPDelta, const TArray<FGuid>& ExpectedAddedIds, const TCHAR* What)
	{
		const FM3_021_ProgressState After = M3_021_CaptureProgressState(Profile);
		Test.TestEqual(FString::Printf(TEXT("%s: the XP moved by exactly the expected amount"), What),
			After.XP - Before.XP, ExpectedXPDelta);
		const int32 ExpectedCount = Before.AllItemIds.Num() + ExpectedAddedIds.Num();
		Test.TestEqual(FString::Printf(TEXT("%s: the combined item set holds exactly the expected count"), What),
			After.AllItemIds.Num(), ExpectedCount);
		for (const FGuid& BeforeId : Before.AllItemIds)
		{
			Test.TestTrue(FString::Printf(TEXT("%s: the pre-failure item %s is still present (nothing lost)"),
				What, *BeforeId.ToString()), After.AllItemIds.Contains(BeforeId));
		}
		for (const FGuid& AddedId : ExpectedAddedIds)
		{
			Test.TestTrue(FString::Printf(TEXT("%s: the expected item %s is present (nothing invented beyond it)"),
				What, *AddedId.ToString()), After.AllItemIds.Contains(AddedId));
		}
	}

	// -- Save fixtures for the corruption flavor (the M3-015 copies, renamed) ------

	/** Fixed character identity of the automation fixture (never all-zero). */
	static FGuid M3_021_CharacterId()
	{
		return FGuid(0x51CE0FA0u, 0x2468u, 0x1357BDF0u, 0x0A2B3C4Du);
	}

	/**
	 * Builds one deterministic instance (same binary-exact stat style as the
	 * M3-013/M3-014/M3-015 fixtures so snapshot strings round trip bit-exactly).
	 */
	static FItemInstance M3_021_MakeInstance(uint32 Seed, const TCHAR* DefinitionName,
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
	 * so the two saves of the corruption test never carry identical content.
	 */
	static FProfileSaveRequest M3_021_MakeSaveRequest(FAutomationTestBase& Test, int32 Level, int32 XP, int32 ItemCount)
	{
		FProfileSaveRequest Request;
		Request.Snapshot.CharacterId = M3_021_CharacterId();
		Request.Snapshot.Level = Level;
		Request.Snapshot.XP = XP;
		Request.Snapshot.MaxHP = UProfileSubsystem::GetMaxHPForLevel(Level);
		Request.Snapshot.Attack = UProfileSubsystem::GetAttackForLevel(Level);
		Request.Snapshot.Defense = UProfileSubsystem::GetDefenseForLevel(Level);
		for (int32 Index = 0; Index < ItemCount; ++Index)
		{
			const FItemInstance Instance = M3_021_MakeInstance(static_cast<uint32>(Level * 120 + Index),
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
		Draft.SettlementId = 7200ull + static_cast<uint64>(Level);
		Draft.XP = 50;
		Draft.Items.Add(M3_021_MakeInstance(static_cast<uint32>(9200 + Level), TEXT("armor_training"), 0.0f, 2.5f, 20.0f, 1));
		Request.PendingRewards.Add(Draft);
		Request.AppliedSettlementIds.Add(8200ull + static_cast<uint64>(Level));
		return Request;
	}

	/**
	 * Writes a CORRUPT slot save straight into storage (the on-disk corruption
	 * model): a well-shaped UProfileSlotSaveGame whose stored integrity digest
	 * deliberately does not match its payload. The distinctive tag values
	 * (guid, level, digest, generation) are what the evidence-preservation
	 * checks compare against afterwards.
	 */
	static bool M3_021_WriteCorruptSlot(ISaveStorage& Storage, const FString& SlotName,
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

	/** Reads a stored slot save back for the evidence checks (bypassing the service). */
	static UProfileSlotSaveGame* M3_021_ReadRawSlot(ISaveStorage& Storage, const FString& SlotName)
	{
		return Cast<UProfileSlotSaveGame>(Storage.ReadSlot(SlotName));
	}

	/**
	 * Asserts the startup outcome carries exactly the request's captured
	 * profile state (identity, progress, recomputed stat row, inventory) - the
	 * fallback recovery must restore a COMPLETE coherent earlier state, never
	 * a mixture of the two generations.
	 */
	static void M3_021_ExpectStartupLoadMatchesRequest(FAutomationTestBase& Test,
		const FStartupLoadOutcome& Loaded, const FProfileSaveRequest& Request, const TCHAR* Scenario)
	{
		Test.TestTrue(FString::Printf(TEXT("%s: CharacterId identical"), Scenario),
			Loaded.Snapshot.CharacterId == Request.Snapshot.CharacterId);
		Test.TestEqual(FString::Printf(TEXT("%s: Level identical"), Scenario),
			Loaded.Snapshot.Level, Request.Snapshot.Level);
		Test.TestEqual(FString::Printf(TEXT("%s: XP identical"), Scenario),
			Loaded.Snapshot.XP, Request.Snapshot.XP);
		Test.TestEqual(FString::Printf(TEXT("%s: restored inventory item count"), Scenario),
			Loaded.Inventory.Count(), Request.Inventory.Count());
		for (const FItemInstance& Expected : Request.Inventory.GetAll())
		{
			Test.TestTrue(FString::Printf(TEXT("%s: restored inventory keeps %s"), Scenario, *Expected.InstanceId.ToString()),
				Loaded.Inventory.Contains(Expected.InstanceId));
		}
	}

	// -- World scene (the M2-011/M3-020 copies, reduced to the session duties) ------

	// World-static blocking box (the floor shares the M2-010 builder shape).
	static AActor* M3_021_SpawnBoxActor(UWorld& World, const FVector& Center, const FVector& HalfExtent, const TCHAR* Name)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(AActor::StaticClass(), Center, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Box = NewObject<UBoxComponent>(Actor, Name);
		Actor->SetRootComponent(Box);
		Box->SetMobility(EComponentMobility::Movable);
		Box->SetBoxExtent(HalfExtent);
		Box->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Box->RegisterComponent();
		Box->SetWorldLocation(Center);
		return Actor;
	}

	// Spawns the real prototype pawn above the floor top and enables
	// no-controller physics so it settles like a possessed game pawn.
	static APrototypeCharacter* M3_021_SpawnPlayer(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			Base + FVector(0.0, 0.0, M3_021_PlayerSpawnHeight),
			FRotator::ZeroRotator, Params);
		if (Player == nullptr)
		{
			return nullptr;
		}
		if (UCharacterMovementComponent* Movement = Player->GetCharacterMovement())
		{
			Movement->bRunPhysicsWithNoController = true;
			if (!Movement->IsActive())
			{
				Movement->Activate(/*bReset*/ true);
			}
			if (Movement->MovementMode == MOVE_None)
			{
				Movement->SetDefaultMovementMode();
			}
		}
		return Player;
	}

	// Definition double of the Data/enemies.json "melee_grunt" row (the
	// M2-008/M2-010 suite precedent; AttackPower 0 keeps the pawn safe).
	static UEnemyDefinition* M3_021_MakeEnemyDef()
	{
		UEnemyDefinition* Def = NewObject<UEnemyDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Def->EnemyId = FName(TEXT("melee_grunt"));
		Def->MaxHP = 60.0f;
		Def->AttackPower = 0.0f;
		Def->MoveSpeed = 220.0f;
		Def->AttackRangeX = 160.0f;
		Def->AlignYTolerance = 35.0f;
		Def->TelegraphSeconds = 0.35f;
		Def->SpawnGraceSeconds = 0.5f;
		Def->MeleeAttackId = FName(TEXT("light_01"));
		return Def;
	}

	// Hand-built definition double of the room_training_01 shape (2+3 waves,
	// the M2-007/M2-008/M2-010 precedent): wave 0 two enemies, wave 1 three
	// enemies, all inside the floor rectangle of this suite's arena.
	static URoomDefinition* M3_021_MakeRoom(const TCHAR* RoomId, const FVector& Base)
	{
		URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Room->RoomId = FName(RoomId);
		Room->RewardTableId = FName(TEXT("reward_m3_021"));

		FRoomWaveDefinition Wave0;
		Wave0.EnemyId = FName(TEXT("melee_grunt"));
		Wave0.Count = 2;
		Wave0.SpawnLocations.Add(Base + FVector(-300.0, 0.0, M3_021_EnemySpawnHeight));
		Wave0.SpawnLocations.Add(Base + FVector(300.0, 0.0, M3_021_EnemySpawnHeight));
		Room->Waves.Add(Wave0);

		FRoomWaveDefinition Wave1;
		Wave1.EnemyId = FName(TEXT("melee_grunt"));
		Wave1.Count = 3;
		Wave1.SpawnLocations.Add(Base + FVector(-500.0, 0.0, M3_021_EnemySpawnHeight));
		Wave1.SpawnLocations.Add(Base + FVector(0.0, 0.0, M3_021_EnemySpawnHeight));
		Wave1.SpawnLocations.Add(Base + FVector(500.0, 0.0, M3_021_EnemySpawnHeight));
		Room->Waves.Add(Wave1);
		return Room;
	}

	// Number of melee enemies in the world INCLUDING corpses.
	static int32 M3_021_CountAllEnemies(UWorld& World)
	{
		int32 Count = 0;
		for (TActorIterator<AMeleeEnemy> It(&World); It; ++It)
		{
			++Count;
		}
		return Count;
	}

	// Number of ALIVE melee enemies in the world (corpses not counted).
	static int32 M3_021_CountAliveEnemies(UWorld& World)
	{
		int32 Count = 0;
		for (TActorIterator<AMeleeEnemy> It(&World); It; ++It)
		{
			UHealthComponent* Health = It->GetHealthComponent();
			if (Health != nullptr && Health->IsAlive())
			{
				++Count;
			}
		}
		return Count;
	}

	/**
	 * Finds the alive, never-yet-killed melee enemy closest to the given
	 * configured spawn location (the M2-014 closest-live-candidate rule).
	 */
	static AMeleeEnemy* M3_021_FindNextAliveEnemyAt(UWorld& World, const FVector& Location, const TArray<AActor*>& AlreadyKilled)
	{
		AMeleeEnemy* Best = nullptr;
		double BestDistSquared = TNumericLimits<double>::Max();
		for (TActorIterator<AMeleeEnemy> It(&World); It; ++It)
		{
			AMeleeEnemy* Enemy = *It;
			UHealthComponent* Health = (Enemy != nullptr) ? Enemy->GetHealthComponent() : nullptr;
			if (Health == nullptr || !Health->IsAlive())
			{
				continue;
			}
			bool bAlreadyKilled = false;
			for (const AActor* Killed : AlreadyKilled)
			{
				if (Killed == Enemy)
				{
					bAlreadyKilled = true;
					break;
				}
			}
			if (bAlreadyKilled)
			{
				continue;
			}
			const double DistSquared = FVector::DistSquared(Enemy->GetActorLocation(), Location);
			if (DistSquared < BestDistSquared)
			{
				BestDistSquared = DistSquared;
				Best = Enemy;
			}
		}
		return Best;
	}

	/**
	 * One temp game world with its real session subsystem, a real floor and
	 * the shipped-health prototype pawn (the M2-011 scene, reduced to the
	 * duties this suite needs). The world ticks for real; the wave
	 * progression is driven by injecting the session clock alongside every
	 * tick (the per-frame duty a game frame driver would perform).
	 */
	struct FM3_021_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		URoomSessionSubsystem* Session = nullptr;
		APrototypeCharacter* Player = nullptr;
		URoomDefinition* Room = nullptr;
		UEnemyDefinition* Enemy = nullptr;
		APrototypeHUD* Hud = nullptr;
		double ClockSeconds = 0.0;

		// Recorded session and pawn events (the assertions read these only).
		int32 StartedCount = 0;
		int32 EndedCount = 0;
		TArray<FRoomResult> EndedResults;
		int32 PlayerDiedCount = 0;

		// The enemies this scene already killed (the kill helper's filter).
		TArray<AActor*> KilledEnemies;

		bool Build(FAutomationTestBase& Test, const TCHAR* RoomId, bool bWithHud)
		{
			if (!Test.TestTrue(TEXT("the test world is created (engine FTestWorldWrapper precedent)"),
				Wrapper.CreateTestWorld(EWorldType::Game)))
			{
				return false;
			}
			World = Wrapper.GetTestWorld();
			if (!Test.TestNotNull(TEXT("the test world is available"), World))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("play begins in the test world (full actor initialization)"),
				Wrapper.BeginPlayInTestWorld()))
			{
				return false;
			}
			Session = World->GetSubsystem<URoomSessionSubsystem>();
			if (!Test.TestNotNull(TEXT("the room session subsystem exists in the test world"), Session))
			{
				return false;
			}
			Session->OnRunStarted().AddLambda([this]()
			{
				++StartedCount;
			});
			Session->OnRunEnded().AddLambda([this](const FRoomResult& Result)
			{
				++EndedCount;
				EndedResults.Add(Result);
			});

			AActor* Floor = M3_021_SpawnBoxActor(*World,
				M3_021_SceneBase - FVector(0.0, 0.0, M3_021_FloorHalfThickness),
				FVector(M3_021_FloorHalfExtentXY, M3_021_FloorHalfExtentXY, M3_021_FloorHalfThickness),
				TEXT("M3_021_Floor"));
			if (Floor == nullptr)
			{
				Test.AddError(TEXT("the floor failed to spawn"));
				return false;
			}
			// A hand-built world never runs the map-load collision tree pass;
			// build it once before the first tick (the M1-041 pattern).
			World->EnsureCollisionTreeIsBuilt();

			Player = M3_021_SpawnPlayer(*World, M3_021_SceneBase);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			Player->PlayerDied.AddLambda([this]()
			{
				++PlayerDiedCount;
			});
			// The registration under test: the death handler binds here.
			Session->SetPlayer(Player);

			Room = M3_021_MakeRoom(RoomId, Player->GetActorLocation());
			Enemy = M3_021_MakeEnemyDef();

			if (!SettlePlayer(Test))
			{
				return false;
			}

			if (bWithHud)
			{
				// The HUD under test: spawned like the game mode spawns it; its
				// BeginPlay binds the session's OnRunEnded (the M2-012 scene).
				FActorSpawnParameters HudParams;
				Hud = World->SpawnActor<APrototypeHUD>(APrototypeHUD::StaticClass(),
					FVector::ZeroVector, FRotator::ZeroRotator, HudParams);
				if (!Test.TestNotNull(TEXT("the prototype HUD spawns in the test world"), Hud))
				{
					return false;
				}
				Hud->SetRoomRetryContext(Room, Enemy);
			}
			return true;
		}

		/** Ticks the world until the player settled onto the floor. */
		bool SettlePlayer(FAutomationTestBase& Test)
		{
			if (Player == nullptr)
			{
				return false;
			}
			UCharacterMovementComponent* PlayerMovement = Player->GetCharacterMovement();
			for (int32 Frame = 0; Frame < M3_021_MaxSettleFrames; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M3_021_FrameSeconds)))
				{
					return false;
				}
				const bool bPlayerGrounded = PlayerMovement != nullptr
					&& PlayerMovement->MovementMode == MOVE_Walking
					&& PlayerMovement->Velocity.Size() < 1.0f;
				if (bPlayerGrounded)
				{
					return true;
				}
			}
			Test.AddError(TEXT("the player never settled onto the floor within the settle cap"));
			return false;
		}

		/** Ticks the world once with the requested delta. */
		bool TickSeconds(FAutomationTestBase& Test, float DeltaSeconds)
		{
			return Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(DeltaSeconds));
		}

		/**
		 * Advances the world with real 60 fps ticks while injecting the
		 * session clock alongside (the same per-frame duty a game driver
		 * performs), so due births, the inter-wave wait and the settlement
		 * elapse through the production pump path.
		 */
		bool AdvanceSeconds(FAutomationTestBase& Test, double Seconds)
		{
			const double StepSeconds = static_cast<double>(M3_021_FrameSeconds);
			double Remaining = Seconds;
			while (Remaining > 1e-9)
			{
				const double StepNow = FMath::Min(StepSeconds, Remaining);
				ClockSeconds += StepNow;
				Session->SetSessionClockSeconds(ClockSeconds);
				if (!TickSeconds(Test, M3_021_FrameSeconds))
				{
					return false;
				}
				Remaining -= StepNow;
			}
			return true;
		}

		/** Injects one exact clock value (the anchor / probe entries). */
		void InjectClock(double NowSeconds)
		{
			ClockSeconds = NowSeconds;
			Session->SetSessionClockSeconds(NowSeconds);
		}

		/** Anchors/continues the CURRENT wave and advances past every slot of it. */
		bool SpawnCurrentWaveFully(FAutomationTestBase& Test, int32 WaveIndex)
		{
			const int32 Count = FMath::Max(0, Room->Waves[WaveIndex].Count);
			const double Span = M3_021_SpawnIntervalSeconds * static_cast<double>(Count - 1) + 0.05;
			return AdvanceSeconds(Test, Span);
		}

		/**
		 * Finds the next alive, never-yet-killed melee enemy at or near the
		 * given configured spawn location and kills it through its health
		 * component (the legal lethal ApplyDamage path - the death flows
		 * through the spawner's real death binding into the session
		 * bookkeeping).
		 */
		bool KillNextEnemyAt(FAutomationTestBase& Test, const FVector& Location, const TCHAR* What)
		{
			AMeleeEnemy* EnemyToKill = M3_021_FindNextAliveEnemyAt(*World, Location, KilledEnemies);
			if (!Test.TestNotNull(What, EnemyToKill))
			{
				return false;
			}
			const double DistSquared = FVector::DistSquared(EnemyToKill->GetActorLocation(), Location);
			if (!Test.TestTrue(FString::Printf(TEXT("%s was born at or near the configured spawn location (dist %.1f cm)"),
				What, FMath::Sqrt(DistSquared)),
				DistSquared <= M3_021_BirthLocationBandCm * M3_021_BirthLocationBandCm))
			{
				return false;
			}
			EnemyToKill->GetHealthComponent()->ApplyDamage(9999.0f);
			KilledEnemies.Add(EnemyToKill);
			return Test.TestTrue(TEXT("the enemy died from the applied damage"),
				EnemyToKill->GetHealthComponent() != nullptr && !EnemyToKill->GetHealthComponent()->IsAlive());
		}

		/**
		 * Starts one run and fails it through the real player-death path (the
		 * M2-012 failure shape the retry guard is verified from).
		 */
		bool FailRunThroughPlayerDeath(FAutomationTestBase& Test)
		{
			if (!Test.TestTrue(TEXT("the run starts"), Session->StartRoom(Room)))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("the progression begins"), Session->BeginWaves(Room, Enemy)))
			{
				return false;
			}
			if (!Test.TestTrue(TEXT("the lethal damage lands"),
				Player->GetHealth()->ApplyDamage(9999.0f) > 0.0f))
			{
				return false;
			}
			return Test.TestTrue(TEXT("the player death failed the run"),
				Session->GetState() == ERoomSessionState::Failed);
		}
	};
}

using namespace UE::UEMMO::Tasks::M3_021;

// -- Failure 1: full bag (M3-009 semantics) ---------------------------------------
// The claim into a full 30-slot inventory answers InventoryFull (never a
// claim success shape), retains the pending item, grants the XP exactly once
// and commits the retention state; the retained draft survives a restart
// field-for-field and freeing one slot lets the SAME pre-generated instance
// claim with NO second XP. The combined item set and the XP are compared
// against the pre-failure snapshot after every step.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_021FullBagClaimRetainsPendingXPGrantsOnce,
	"UEMMO.Tasks.M3_021.FullBagClaimRetainsPendingXPGrantsOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_021FullBagClaimRetainsPendingXPGrantsOnce::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_021_MemoryStorage Storage;
	FM3_021_ProfileFixture Fixture;
	if (!Fixture.Setup(Test))
	{
		return true;
	}
	UProfileSaveService* Service = M3_021_NewService(Test, &Storage);
	if (!Service)
	{
		return true;
	}

	// Setup: full 30-slot inventory, then one settled run drafts the reward.
	const FItemDefinition FillerDef = M3_021_MakeFillerDefinition();
	const TArray<FGuid> Fillers = M3_021_FillInventory(Fixture.Profile->GetInventory(), FillerDef, FInventoryModel::Capacity);
	Test.TestEqual(TEXT("setup: the inventory is full"), Fixture.Profile->GetInventory().Count(), FInventoryModel::Capacity);

	const FRoomResult Result = M3_021_MakeResult(/*RunId*/ 6201, /*SettlementId*/ 6201, /*Seed*/ 62011, true);
	const FRewardBeginOutcome Begin = Fixture.Reward->BeginReward(Result, M3_021_MakeTrainingCatalog());
	Test.TestEqual(TEXT("setup: the settlement began with a pending draft"), Begin.Result, ERewardBeginResult::Applied);
	if (Begin.Result != ERewardBeginResult::Applied || Begin.Draft.Items.Num() != 1)
	{
		M3_021_CleanupTestSlots(Test, *Service, Storage);
		return true;
	}
	const FItemInstance Original = Begin.Draft.Items[0];
	const FM3_021_ProgressState Before = M3_021_CaptureProgressState(*Fixture.Profile);

	// The failure: the full-bag claim. Nothing is stored, the item is
	// retained, the XP is granted once, the retention state commits.
	const FRewardClaimAtomicOutcome Claim = Fixture.Reward->ClaimPendingAtomic(6201, Fixture.Profile, Service);
	Test.TestEqual(TEXT("the full-bag claim reports InventoryFull (never a claim success)"),
		Claim.Result, ERewardClaimAtomicResult::InventoryFull);
	Test.TestEqual(TEXT("the full-bag claim stored nothing"), Claim.ClaimedItemCount, 0);
	Test.TestEqual(TEXT("the full-bag claim retains the pending item"), Claim.RetainedItemCount, 1);
	Test.TestTrue(TEXT("the first attempt granted the settlement XP"), Claim.bGrantedXP);
	Test.TestTrue(TEXT("the retention state is committed"), Claim.bSaveCommitted);
	Test.TestTrue(TEXT("the settlement is recorded as applied"), Fixture.Profile->IsSettlementApplied(6201));
	Test.TestEqual(TEXT("the draft stays pending"), Fixture.Profile->GetPendingRewards().Num(), 1);
	if (Fixture.Profile->GetPendingRewards().Num() == 1 && Fixture.Profile->GetPendingRewards()[0].Items.Num() == 1)
	{
		Test.TestTrue(TEXT("the retained draft item is field-for-field the ORIGINAL (no re-roll, no loss)"),
			M3_021_InstancesEqual(Fixture.Profile->GetPendingRewards()[0].Items[0], Original));
	}
	M3_021_ExpectXPAndItems(Test, Before, *Fixture.Profile, URewardService::RewardXPPerClear, {},
		TEXT("after the full-bag claim failure"));

	// RESTART: the retained draft (with the ORIGINAL item) must survive.
	FM3_021_RestartedState Restarted;
	if (!Restarted.Setup(Test, Storage, TEXT("full-bag restart")))
	{
		M3_021_CleanupTestSlots(Test, *Service, Storage);
		return true;
	}
	Test.TestEqual(TEXT("the restart recovered the committed retention state"),
		static_cast<int32>(Restarted.LoadResult), static_cast<int32>(EStartupLoadResult::Recovered));
	Test.TestEqual(TEXT("the restored pending draft survived the restart"), Restarted.Profile->GetPendingRewards().Num(), 1);
	if (Restarted.Profile->GetPendingRewards().Num() == 1 && Restarted.Profile->GetPendingRewards()[0].Items.Num() == 1)
	{
		Test.TestTrue(TEXT("the retained item survived field-for-field (same InstanceId, same one-time roll)"),
			M3_021_InstancesEqual(Restarted.Profile->GetPendingRewards()[0].Items[0], Original));
	}
	Test.TestEqual(TEXT("the restored XP is the once-granted total"),
		Restarted.Profile->GetXP(), URewardService::RewardXPPerClear);
	Test.TestTrue(TEXT("the restored applied record survives"), Restarted.Profile->IsSettlementApplied(6201));
	Test.TestEqual(TEXT("the restored inventory is still full"), Restarted.Profile->GetInventory().Count(), FInventoryModel::Capacity);

	// Free one slot (a deliberate player action, not a failure) and re-claim.
	const EInventoryRemoveResult Removed = Restarted.Profile->GetInventory().Remove(Fillers[0]);
	Test.TestEqual(TEXT("one freed slot makes room"), Removed, EInventoryRemoveResult::Removed);
	const FM3_021_ProgressState AfterFree = M3_021_CaptureProgressState(*Restarted.Profile);
	const FRewardClaimAtomicOutcome Reclaim = Restarted.Reward->ClaimPendingAtomic(6201, Restarted.Profile, Restarted.SaveService);
	Test.TestEqual(TEXT("the re-claim after freeing a slot claims the draft"), Reclaim.Result, ERewardClaimAtomicResult::Claimed);
	Test.TestEqual(TEXT("the re-claim stored exactly one item"), Reclaim.ClaimedItemCount, 1);
	Test.TestFalse(TEXT("the re-claim grants no XP again"), Reclaim.bGrantedXP);
	Test.TestEqual(TEXT("the consumed draft is gone"), Restarted.Profile->GetPendingRewards().Num(), 0);
	Test.TestTrue(TEXT("the stored item is the ORIGINAL instance (same InstanceId, no re-roll)"),
		Restarted.Profile->GetInventory().Contains(Original.InstanceId));
	// The claim MOVED the pre-generated instance from the draft into the
	// inventory: the combined set only lost the deliberately removed filler.
	M3_021_ExpectXPAndItems(Test, AfterFree, *Restarted.Profile, 0, {},
		TEXT("after the re-claim (the freed filler left, the ORIGINAL instance moved into the inventory)"));

	M3_021_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- Failure 2: corrupt save (M3-015 semantics) ------------------------------------
// Flavor 1: the index-active slot payload is tampered => StartupLoad falls
// back to the OTHER valid slot, the recovery report names the reason and the
// corrupt file stays as evidence; the restored state is the complete coherent
// earlier generation (never a mixture, never invented data). Flavor 2: BOTH
// slots corrupt => RecoveryError with a per-slot problem list, byte-identical
// evidence, a refused save path and NO silently minted profile.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_021CorruptSaveFallsBackAndBothBadReportsRecoveryError,
	"UEMMO.Tasks.M3_021.CorruptSaveFallsBackAndBothBadReportsRecoveryError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_021CorruptSaveFallsBackAndBothBadReportsRecoveryError::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;

	// -- Flavor 1: the tampered ACTIVE slot falls back to the other slot -------
	FM3_021_MemoryStorage Storage1;
	UProfileSaveService* Service1 = M3_021_NewService(Test, &Storage1);
	if (!Service1)
	{
		return true;
	}
	const FProfileSaveRequest FirstRequest = M3_021_MakeSaveRequest(Test, 2, 10, 2);
	const FProfileSaveRequest SecondRequest = M3_021_MakeSaveRequest(Test, 3, 30, 3);
	Test.TestEqual(TEXT("flavor 1 setup: first save committed to A"),
		static_cast<int32>(Service1->SaveProfile(FirstRequest).WrittenSlotIndex), 0);
	Test.TestEqual(TEXT("flavor 1 setup: second save committed to B"),
		static_cast<int32>(Service1->SaveProfile(SecondRequest).WrittenSlotIndex), 1);

	// The failure: the index-active slot B payload goes bad (deliberately
	// wrong integrity digest with distinctive tag values).
	const FGuid TagIdB(0x0BAD0000u, 0xB000u, 0xB0000000u, 0x000000B1u);
	Test.TestTrue(TEXT("flavor 1 setup: the corrupt slot B was written"),
		M3_021_WriteCorruptSlot(Storage1, M3_021_SlotBName(), TagIdB, 43, 8, 0xBADF00Du));

	const FStartupLoadOutcome Loaded1 = Service1->StartupLoad();
	Test.TestEqual(TEXT("the corrupt active slot falls back to the other slot"),
		static_cast<int32>(Loaded1.Result), static_cast<int32>(EStartupLoadResult::RecoveredFallback));
	if (Loaded1.Result == EStartupLoadResult::RecoveredFallback)
	{
		// The fallback restores the complete coherent EARLIER generation.
		M3_021_ExpectStartupLoadMatchesRequest(Test, Loaded1, FirstRequest, TEXT("fallback recovery"));
		Test.TestEqual(TEXT("the fallback came from slot A"), Loaded1.SlotIndex, 0);
		Test.TestEqual(TEXT("the fallback carries slot A's generation"), Loaded1.Generation, 1);
		Test.TestTrue(TEXT("the recovery report names the corrupt active slot"),
			Loaded1.FallbackReason.Contains(M3_021_SlotBName()));
		Test.TestTrue(TEXT("the summary carries the recovery report"), !Loaded1.Summary.IsEmpty());
		Test.TestEqual(TEXT("the corrupt slot is in the problem list"), Loaded1.CorruptedSlotReports.Num(), 1);
	}
	// Evidence: the corrupt slot B file still exists (a load never deletes).
	Test.TestTrue(TEXT("the corrupt slot B evidence is preserved"), Storage1.DoesSlotExist(M3_021_SlotBName()));

	// The restart model on the same storage: the NEXT process recovers the
	// same earlier generation (the fallback re-arms; nothing is invented).
	FM3_021_RestartedState Restarted1;
	if (!Restarted1.Setup(Test, Storage1, TEXT("corrupt-active restart")))
	{
		M3_021_CleanupTestSlots(Test, *Service1, Storage1);
		return true;
	}
	Test.TestEqual(TEXT("the restart recovered by fallback again"),
		static_cast<int32>(Restarted1.LoadResult), static_cast<int32>(EStartupLoadResult::RecoveredFallback));
	Test.TestEqual(TEXT("the restored profile carries the earlier generation's XP"), Restarted1.Profile->GetXP(), 10);
	Test.TestEqual(TEXT("the restored profile carries the earlier generation's item count"),
		Restarted1.Profile->GetInventory().Count(), 2);
	for (const FItemInstance& Expected : FirstRequest.Inventory.GetAll())
	{
		Test.TestTrue(FString::Printf(TEXT("the restored inventory keeps exactly the earlier item %s"),
			*Expected.InstanceId.ToString()), Restarted1.Profile->GetInventory().Contains(Expected.InstanceId));
	}
	M3_021_CleanupTestSlots(Test, *Service1, Storage1);

	// -- Flavor 2: BOTH slots corrupt => RecoveryError, no silent reset --------
	FM3_021_MemoryStorage Storage2;
	UProfileSaveService* Service2 = M3_021_NewService(Test, &Storage2);
	if (!Service2)
	{
		return true;
	}
	Test.TestEqual(TEXT("flavor 2 setup: first save committed"),
		static_cast<int32>(Service2->SaveProfile(FirstRequest).Result), static_cast<int32>(EProfileSaveResult::Success));
	Test.TestEqual(TEXT("flavor 2 setup: second save committed"),
		static_cast<int32>(Service2->SaveProfile(SecondRequest).Result), static_cast<int32>(EProfileSaveResult::Success));

	const FGuid TagIdA(0x0BAD0000u, 0xA000u, 0xA0000000u, 0x000000A1u);
	Test.TestTrue(TEXT("flavor 2 setup: the corrupt slot A was written"),
		M3_021_WriteCorruptSlot(Storage2, M3_021_SlotAName(), TagIdA, 42, 7, 0xBADF00Du));
	Test.TestTrue(TEXT("flavor 2 setup: the corrupt slot B was written"),
		M3_021_WriteCorruptSlot(Storage2, M3_021_SlotBName(), TagIdB, 43, 8, 0xFEEDFACEu));

	const int32 WritesBefore = Storage2.WriteCallCount;
	const FStartupLoadOutcome Loaded2 = Service2->StartupLoad();
	Test.TestEqual(TEXT("the startup load itself never writes"), Storage2.WriteCallCount - WritesBefore, 0);
	Test.TestEqual(TEXT("two corrupt slots report RecoveryError"),
		static_cast<int32>(Loaded2.Result), static_cast<int32>(EStartupLoadResult::RecoveryError));
	Test.TestTrue(TEXT("RecoveryError is never a fresh state (no silent new save)"),
		Loaded2.Result != EStartupLoadResult::NoSaveFound);
	Test.TestFalse(TEXT("no CharacterId is restored on the recovery error"), Loaded2.Snapshot.CharacterId.IsValid());
	Test.TestEqual(TEXT("no inventory is restored on the recovery error"), Loaded2.Inventory.Count(), 0);
	Test.TestEqual(TEXT("the problem list names both corrupt slots"), Loaded2.CorruptedSlotReports.Num(), 2);
	Test.TestTrue(TEXT("the recovery error carries a summary"), !Loaded2.Summary.IsEmpty());

	// The save path is GUARDED: the transaction refuses to overwrite the
	// corrupt evidence - nothing at all is written.
	const FProfileSaveRequest RefusedRequest = M3_021_MakeSaveRequest(Test, 4, 40, 4);
	const FProfileSaveOutcome Refused = Service2->SaveProfile(RefusedRequest);
	Test.TestEqual(TEXT("saving with two corrupt slots is refused by the write guard"),
		static_cast<int32>(Refused.Result), static_cast<int32>(EProfileSaveResult::FailedGuardedSlot));
	Test.TestTrue(TEXT("the refusal names the guarded slot and reason"), !Refused.Message.IsEmpty());
	Test.TestEqual(TEXT("the refused save wrote nothing at all"), Storage2.WriteCallCount - WritesBefore, 0);

	// Evidence: both corrupt files still exist with byte-identical tag fields.
	UProfileSlotSaveGame* RawA = M3_021_ReadRawSlot(Storage2, M3_021_SlotAName());
	Test.TestNotNull(TEXT("the corrupt slot A evidence is preserved"), RawA);
	if (RawA)
	{
		Test.TestEqual(TEXT("slot A evidence: level tag unchanged"), RawA->Level, 42);
		Test.TestEqual(TEXT("slot A evidence: digest tag unchanged"), RawA->PayloadDigest, 0xBADF00Du);
		Test.TestEqual(TEXT("slot A evidence: generation tag unchanged"), RawA->SlotGeneration, 7);
	}
	UProfileSlotSaveGame* RawB = M3_021_ReadRawSlot(Storage2, M3_021_SlotBName());
	Test.TestNotNull(TEXT("the corrupt slot B evidence is preserved"), RawB);
	if (RawB)
	{
		Test.TestEqual(TEXT("slot B evidence: level tag unchanged"), RawB->Level, 43);
		Test.TestEqual(TEXT("slot B evidence: digest tag unchanged"), RawB->PayloadDigest, 0xFEEDFACEu);
		Test.TestEqual(TEXT("slot B evidence: generation tag unchanged"), RawB->SlotGeneration, 8);
	}

	// NO silent NewProfile: the recovery error carries no profile and the
	// caller's subsystem stays empty unless the caller explicitly decides.
	FM3_021_BareSubsystemFixture Bare;
	if (Bare.Setup(Test))
	{
		Test.TestFalse(TEXT("the caller's subsystem starts without a profile"), Bare.Profile->HasProfile());
		Test.TestFalse(TEXT("RestoreFromSave refuses the empty recovery-error outcome"),
			Bare.Profile->RestoreFromSave(Loaded2.Snapshot, Loaded2.Inventory, Loaded2.PendingRewards,
				Loaded2.AppliedSettlementIds, Loaded2.EquippedMap));
		Test.TestFalse(TEXT("the refused restore left the subsystem without a profile (no silent NewProfile)"),
			Bare.Profile->HasProfile());
	}

	M3_021_CleanupTestSlots(Test, *Service2, Storage2);
	return true;
}

// -- Failure 3: duplicate settlement (M3-008/M3-016 semantics) ---------------------
// A replayed BeginReward for the same SettlementId answers AlreadyApplied
// with the ORIGINAL draft (no re-roll, no second item) and a replayed claim
// answers AlreadyClaimed - the XP and the item set can never double, both
// live and across a restart from the persisted state alone.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_021SettlementIdReplayNeverDuplicatesXPOrItems,
	"UEMMO.Tasks.M3_021.SettlementIdReplayNeverDuplicatesXPOrItems",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_021SettlementIdReplayNeverDuplicatesXPOrItems::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_021_MemoryStorage Storage;
	FM3_021_ProfileFixture Fixture;
	if (!Fixture.Setup(Test))
	{
		return true;
	}
	UProfileSaveService* Service = M3_021_NewService(Test, &Storage);
	if (!Service)
	{
		return true;
	}
	const FItemDefinitionCatalog Catalog = M3_021_MakeTrainingCatalog();

	const FRoomResult Result = M3_021_MakeResult(/*RunId*/ 6301, /*SettlementId*/ 6301, /*Seed*/ 63011, true);
	const FRewardBeginOutcome Begin = Fixture.Reward->BeginReward(Result, Catalog);
	Test.TestEqual(TEXT("setup: the settlement began with a pending draft"), Begin.Result, ERewardBeginResult::Applied);
	if (Begin.Result != ERewardBeginResult::Applied || Begin.Draft.Items.Num() != 1)
	{
		M3_021_CleanupTestSlots(Test, *Service, Storage);
		return true;
	}
	const FItemInstance Original = Begin.Draft.Items[0];
	const FM3_021_ProgressState Before = M3_021_CaptureProgressState(*Fixture.Profile);

	// The replayed settlement: AlreadyApplied with the ORIGINAL draft, no
	// second draft, no second item.
	const FRewardBeginOutcome Replay = Fixture.Reward->BeginReward(Result, Catalog);
	Test.TestEqual(TEXT("the replayed BeginReward answers AlreadyApplied"),
		Replay.Result, ERewardBeginResult::AlreadyApplied);
	if (Replay.Result == ERewardBeginResult::AlreadyApplied)
	{
		Test.TestEqual(TEXT("the replayed draft keeps the same settlement id"),
			Replay.Draft.SettlementId, Begin.Draft.SettlementId);
		Test.TestTrue(TEXT("the replayed draft is field-for-field the ORIGINAL (no re-roll)"),
			Replay.Draft.Items.Num() == 1 && M3_021_InstancesEqual(Replay.Draft.Items[0], Original));
	}
	Test.TestEqual(TEXT("the replay adds no second draft"), Fixture.Profile->GetPendingRewards().Num(), 1);
	M3_021_ExpectXPAndItems(Test, Before, *Fixture.Profile, 0, {}, TEXT("after the replayed BeginReward"));

	// The first claim: XP +50 once, exactly the original instance stored.
	const FRewardClaimAtomicOutcome Claim = Fixture.Reward->ClaimPendingAtomic(6301, Fixture.Profile, Service);
	Test.TestEqual(TEXT("the first claim succeeds"), Claim.Result, ERewardClaimAtomicResult::Claimed);
	Test.TestTrue(TEXT("the first claim grants the XP"), Claim.bGrantedXP);
	Test.TestEqual(TEXT("the first claim stored exactly one item"), Claim.ClaimedItemCount, 1);
	Test.TestEqual(TEXT("the first claim consumed the draft"), Fixture.Profile->GetPendingRewards().Num(), 0);
	Test.TestTrue(TEXT("the inventory holds the ORIGINAL instance (same InstanceId, no re-roll)"),
		Fixture.Profile->GetInventory().Contains(Original.InstanceId));
	// The claim MOVED the pre-generated instance from the draft into the
	// inventory: the combined set itself stays exactly the same.
	M3_021_ExpectXPAndItems(Test, Before, *Fixture.Profile, URewardService::RewardXPPerClear, {},
		TEXT("after the first claim"));

	// The replayed claim: AlreadyClaimed from the applied record - no second
	// XP, no second item, no save.
	const FM3_021_ProgressState AfterClaim = M3_021_CaptureProgressState(*Fixture.Profile);
	const FRewardClaimAtomicOutcome ClaimAgain = Fixture.Reward->ClaimPendingAtomic(6301, Fixture.Profile, Service);
	Test.TestEqual(TEXT("the replayed claim answers AlreadyClaimed"),
		ClaimAgain.Result, ERewardClaimAtomicResult::AlreadyClaimed);
	Test.TestFalse(TEXT("the replayed claim grants no XP again"), ClaimAgain.bGrantedXP);
	Test.TestFalse(TEXT("the replayed claim performs no save"), ClaimAgain.bSaveCommitted);
	M3_021_ExpectXPAndItems(Test, AfterClaim, *Fixture.Profile, 0, {}, TEXT("after the replayed claim"));

	// The post-claim BeginReward replay: AlreadyApplied with a DEFAULT draft
	// (a claimed settlement no longer carries a pending draft).
	const FRewardBeginOutcome ReplayAfterClaim = Fixture.Reward->BeginReward(Result, Catalog);
	Test.TestEqual(TEXT("the post-claim BeginReward replay answers AlreadyApplied"),
		ReplayAfterClaim.Result, ERewardBeginResult::AlreadyApplied);
	Test.TestEqual(TEXT("the post-claim replay carries no draft"), ReplayAfterClaim.Draft.Items.Num(), 0);
	Test.TestEqual(TEXT("no draft reappeared"), Fixture.Profile->GetPendingRewards().Num(), 0);

	// RESTART: the persisted state alone decides the replay guards.
	FM3_021_RestartedState Restarted;
	if (!Restarted.Setup(Test, Storage, TEXT("replay restart")))
	{
		M3_021_CleanupTestSlots(Test, *Service, Storage);
		return true;
	}
	Test.TestEqual(TEXT("the restart recovered the committed claim"),
		static_cast<int32>(Restarted.LoadResult), static_cast<int32>(EStartupLoadResult::Recovered));
	Test.TestTrue(TEXT("the restarted inventory holds the ORIGINAL instance"),
		Restarted.Profile->GetInventory().Contains(Original.InstanceId));
	// The claimed instance's id is the SAME one the draft pre-generated: the
	// combined set is unchanged, the XP carries the single design grant.
	M3_021_ExpectXPAndItems(Test, Before, *Restarted.Profile, URewardService::RewardXPPerClear, {},
		TEXT("after the restart (the persisted total)"));
	const FRewardClaimAtomicOutcome ReclaimAfterRestart = Restarted.Reward->ClaimPendingAtomic(6301, Restarted.Profile, Restarted.SaveService);
	Test.TestEqual(TEXT("the re-claim after the restart answers AlreadyClaimed"),
		ReclaimAfterRestart.Result, ERewardClaimAtomicResult::AlreadyClaimed);
	Test.TestFalse(TEXT("the re-claim after the restart grants no XP"), ReclaimAfterRestart.bGrantedXP);
	const FRewardBeginOutcome ReplayAfterRestart = Restarted.Reward->BeginReward(Result, Catalog);
	Test.TestEqual(TEXT("the replayed BeginReward after the restart answers AlreadyApplied"),
		ReplayAfterRestart.Result, ERewardBeginResult::AlreadyApplied);
	Test.TestEqual(TEXT("the replay after the restart adds no draft"), Restarted.Profile->GetPendingRewards().Num(), 0);
	M3_021_ExpectXPAndItems(Test, AfterClaim, *Restarted.Profile, 0, {}, TEXT("after the restart replays"));

	M3_021_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// -- Failure 4: exit map mid-wave (M2-011 semantics) -------------------------------
// LeaveRoom during wave generation zeroes the pending births, produces no
// late spawn (injected far-future clock OR real ticks), fires no settlement
// end event and never touches the profile (an exit is NOT a settlement: the
// combined item set and the XP stay exactly as they were). StartRoom then
// re-enters into a fully working fresh run that clears end to end.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_021ExitMidWaveNoLateSpawnReentryClean,
	"UEMMO.Tasks.M3_021.ExitMidWaveNoLateSpawnReentryClean",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_021ExitMidWaveNoLateSpawnReentryClean::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;

	// The profile fixture pins the "an exit is not a settlement" invariant:
	// a pre-staged filler item plus one pending draft must survive the exit
	// UNCHANGED (no XP, no item move, no applied record).
	FM3_021_ProfileFixture Fixture;
	if (!Fixture.Setup(Test))
	{
		return true;
	}
	const FItemDefinition FillerDef = M3_021_MakeFillerDefinition();
	Test.TestEqual(TEXT("setup: the staged filler entered the inventory"),
		Fixture.Profile->GetInventory().TryAdd(MakeItemInstance(FillerDef, 64000)), EInventoryAddResult::Added);
	const FRoomResult StagedResult = M3_021_MakeResult(/*RunId*/ 6404, /*SettlementId*/ 6404, /*Seed*/ 64041, true);
	const FRewardBeginOutcome StagedBegin = Fixture.Reward->BeginReward(StagedResult, M3_021_MakeTrainingCatalog());
	Test.TestEqual(TEXT("setup: the staged draft is pending"), StagedBegin.Result, ERewardBeginResult::Applied);
	const FM3_021_ProgressState BeforeExit = M3_021_CaptureProgressState(*Fixture.Profile);

	FM3_021_Scene Scene;
	if (!Scene.Build(*this, TEXT("room_m3_021_exit"), /*bWithHud*/ false))
	{
		return true;
	}
	URoomDefinition* Room = Scene.Room;
	UEnemyDefinition* Def = Scene.Enemy;

	// The run starts and wave 0 is MID-GENERATION when the player exits.
	if (!Test.TestTrue(TEXT("the run starts"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 OldRunId = Scene.Session->GetRunId();
	const uint64 OldSettlementId = Scene.Session->GetSettlementId();
	if (!Test.TestTrue(TEXT("the progression begins from wave 0"), Scene.Session->BeginWaves(Room, Def)))
	{
		return true;
	}
	Scene.InjectClock(0.0);
	Test.TestEqual(TEXT("the anchor injection birthed the first wave-0 enemy"),
		Scene.Session->GetSpawnedEnemyCount(), 1);
	Test.TestEqual(TEXT("the wave is mid-generation (one enemy still unborn)"),
		Scene.Session->GetRunPendingSpawnCount(), 1);
	Test.TestTrue(TEXT("the run is Running before the leave"),
		Scene.Session->GetState() == ERoomSessionState::Running);

	// The failure: the exit mid-generation.
	if (!Test.TestTrue(TEXT("LeaveRoom accepts the exit from the Running run"), Scene.Session->LeaveRoom()))
	{
		return true;
	}
	Test.TestTrue(TEXT("the session is Exiting after the leave"),
		Scene.Session->GetState() == ERoomSessionState::Exiting);
	Test.TestEqual(TEXT("the unborn spawns were cleared with the room (PendingSpawns 0)"),
		Scene.Session->GetRunPendingSpawnCount(), 0);
	Test.TestEqual(TEXT("the exit fired no settlement end event"), Scene.EndedCount, 0);
	Test.TestEqual(TEXT("the exit fired no extra start event"), Scene.StartedCount, 1);

	// No late spawn: a far-future clock injection births nothing.
	Scene.InjectClock(10.0);
	Test.TestEqual(TEXT("the far-future injection birthed no new enemy"),
		Scene.Session->GetSpawnedEnemyCount(), 1);
	Test.TestEqual(TEXT("the far-future injection left no pending spawns"),
		Scene.Session->GetRunPendingSpawnCount(), 0);

	// No late spawn through real ticks either: the cancelled wave never resumes.
	if (!Scene.AdvanceSeconds(*this, 2.0))
	{
		return true;
	}
	Test.TestEqual(TEXT("no new spawn happened during the 2 s ticked wait"),
		Scene.Session->GetSpawnedEnemyCount(), 1);
	Test.TestEqual(TEXT("the world holds exactly the one pre-leave enemy"),
		M3_021_CountAllEnemies(*Scene.World), 1);
	Test.TestTrue(TEXT("the session stays Exiting through the ticks"),
		Scene.Session->GetState() == ERoomSessionState::Exiting);
	Test.TestEqual(TEXT("still no end event after the ticks"), Scene.EndedCount, 0);

	// The pre-exit enemy actor STAYED alive in the world (the exit cleared
	// the run's references WITHOUT destroying actors - the M2-011 semantics).
	// Kill it while Exiting: a dead run's death counts and settles nothing,
	// and the arena is unambiguous for the re-entry's location lookups.
	AMeleeEnemy* Leftover = nullptr;
	for (TActorIterator<AMeleeEnemy> It(Scene.World); It; ++It)
	{
		UHealthComponent* Health = It->GetHealthComponent();
		if (Health != nullptr && Health->IsAlive())
		{
			Leftover = *It;
			break;
		}
	}
	if (!Test.TestNotNull(TEXT("the pre-exit leftover enemy was found"), Leftover))
	{
		return true;
	}
	Leftover->GetHealthComponent()->ApplyDamage(9999.0f);
	Test.TestTrue(TEXT("the leftover died from the applied damage"),
		Leftover->GetHealthComponent() != nullptr && !Leftover->GetHealthComponent()->IsAlive());
	Test.TestEqual(TEXT("the stale leftover death while Exiting counted no kill"),
		Scene.Session->GetKilledCount(), 0);
	Test.TestTrue(TEXT("the leftover death while Exiting left the session Exiting"),
		Scene.Session->GetState() == ERoomSessionState::Exiting);
	Test.TestEqual(TEXT("the leftover death while Exiting fired no end event"), Scene.EndedCount, 0);

	// The card invariant: the exit moved NO profile data (no settlement).
	M3_021_ExpectXPAndItems(Test, BeforeExit, *Fixture.Profile, 0, {}, TEXT("after the mid-wave exit"));
	Test.TestFalse(TEXT("the exit recorded no settlement as applied"),
		Fixture.Profile->IsSettlementApplied(6404));

	// Re-entry: StartRoom from Exiting starts a fully working fresh run.
	if (!Test.TestTrue(TEXT("StartRoom from Exiting starts a fresh run"), Scene.Session->StartRoom(Room)))
	{
		return true;
	}
	const uint64 NewRunId = Scene.Session->GetRunId();
	Test.TestTrue(TEXT("the re-entry got a new, larger RunId"), NewRunId > OldRunId);
	Test.TestTrue(TEXT("the re-entry got a fresh SettlementId (no reward loss)"),
		Scene.Session->GetSettlementId() > OldSettlementId);
	if (!Test.TestTrue(TEXT("the re-entry's progression begins"), Scene.Session->BeginWaves(Room, Def)))
	{
		return true;
	}

	// The re-entered run clears end to end: wave 0 (2) and wave 1 (3) born
	// and killed through the legal lethal path, then Cleared.
	Scene.InjectClock(Scene.ClockSeconds);
	if (!Scene.SpawnCurrentWaveFully(*this, 0))
	{
		return true;
	}
	Test.TestEqual(TEXT("the re-entry birthed both wave-0 enemies"), Scene.Session->GetSpawnedEnemyCount(), 2);
	if (!Scene.KillNextEnemyAt(*this, Room->Waves[0].SpawnLocations[0], TEXT("the first wave-0 enemy was found"))
		|| !Scene.KillNextEnemyAt(*this, Room->Waves[0].SpawnLocations[1], TEXT("the second wave-0 enemy was found")))
	{
		return true;
	}
	if (!Scene.AdvanceSeconds(*this, M3_021_WaveGapSeconds + 0.05))
	{
		return true;
	}
	if (!Scene.SpawnCurrentWaveFully(*this, 1))
	{
		return true;
	}
	Test.TestEqual(TEXT("the re-entry birthed all five of its enemies"), Scene.Session->GetSpawnedEnemyCount(), 5);
	if (!Scene.KillNextEnemyAt(*this, Room->Waves[1].SpawnLocations[0], TEXT("the first wave-1 enemy was found"))
		|| !Scene.KillNextEnemyAt(*this, Room->Waves[1].SpawnLocations[1], TEXT("the second wave-1 enemy was found"))
		|| !Scene.KillNextEnemyAt(*this, Room->Waves[1].SpawnLocations[2], TEXT("the third wave-1 enemy was found")))
	{
		return true;
	}
	Test.TestTrue(TEXT("the re-entered run cleared"),
		Scene.Session->GetState() == ERoomSessionState::Cleared);
	Test.TestEqual(TEXT("the re-entry counted exactly five kills"), Scene.Session->GetKilledCount(), 5);
	Test.TestEqual(TEXT("the re-entry fired exactly its own end event"), Scene.EndedCount, 1);
	Test.TestTrue(TEXT("the re-entered run is reward eligible"), Scene.Session->IsRewardEligible(NewRunId));
	FRoomResult ReentryResult;
	Test.TestTrue(TEXT("the re-entered run's result is archived"), Scene.Session->GetRunResult(NewRunId, ReentryResult));
	if (ReentryResult.RunId == NewRunId)
	{
		Test.TestTrue(TEXT("the archived re-entry result records Cleared"), ReentryResult.bCleared);
	}
	Test.TestEqual(TEXT("zero alive enemies remain after the re-entry clear"),
		M3_021_CountAliveEnemies(*Scene.World), 0);

	// The re-entry itself still moved no profile data (settling is the
	// caller's explicit BeginReward decision, never an exit/re-entry side
	// effect).
	M3_021_ExpectXPAndItems(Test, BeforeExit, *Fixture.Profile, 0, {}, TEXT("after the clean re-entry"));
	return true;
}

// -- Failure 5: quick clicks (M2-012/M3-018 semantics) -----------------------------
// Of two fast requests only the FIRST is processed by the one-shot guard:
// the CLAIM half drives the production wiring shape (FRoomResultActionGuard
// plus the real URewardService::ClaimPendingAtomic - the exact structure of
// APrototypeHUD::HandleRewardClaimRequested; the HUD path itself is NOT
// driven because its internal save service is pinned to the production
// "Profile_" prefix with no storage seam, and this suite must never touch the
// player's real saves), the RETRY half drives the REAL
// APrototypeHUD::HandleRetryRequested path in a world scene. The duplicate is
// dropped and counted with no second XP, no second item, no second restart.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_021QuickClickGuardsProcessOnlyFirstRequest,
	"UEMMO.Tasks.M3_021.QuickClickGuardsProcessOnlyFirstRequest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_021QuickClickGuardsProcessOnlyFirstRequest::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;

	// -- Claim half: the guard wiring of the production claim call site --------
	FM3_021_MemoryStorage Storage;
	FM3_021_ProfileFixture Fixture;
	if (!Fixture.Setup(Test))
	{
		return true;
	}
	UProfileSaveService* Service = M3_021_NewService(Test, &Storage);
	if (!Service)
	{
		return true;
	}

	const FRoomResult Result = M3_021_MakeResult(/*RunId*/ 6405, /*SettlementId*/ 6405, /*Seed*/ 64051, true);
	const FRewardBeginOutcome Begin = Fixture.Reward->BeginReward(Result, M3_021_MakeTrainingCatalog());
	Test.TestEqual(TEXT("claim half setup: the settlement began with a pending draft"),
		Begin.Result, ERewardBeginResult::Applied);
	if (Begin.Result != ERewardBeginResult::Applied || Begin.Draft.Items.Num() != 1)
	{
		M3_021_CleanupTestSlots(Test, *Service, Storage);
		return true;
	}
	const FItemInstance Original = Begin.Draft.Items[0];
	const FM3_021_ProgressState Before = M3_021_CaptureProgressState(*Fixture.Profile);

	// The production wiring shape: the guard consumes the first request, the
	// duplicate of a fast double click never reaches the service.
	FRoomResultActionGuard Guard;
	Guard.ReArm();
	int32 ServiceCalls = 0;
	auto RequestClaim = [&Guard, &Fixture, &Service, &ServiceCalls]()
	{
		if (Guard.TryAccept())
		{
			++ServiceCalls;
			return Fixture.Reward->ClaimPendingAtomic(6405, Fixture.Profile, Service);
		}
		FRewardClaimAtomicOutcome Dropped;
		return Dropped;
	};

	// The fast double click.
	const FRewardClaimAtomicOutcome First = RequestClaim();
	const FRewardClaimAtomicOutcome Second = RequestClaim();

	Test.TestEqual(TEXT("the first click claimed"), First.Result, ERewardClaimAtomicResult::Claimed);
	Test.TestTrue(TEXT("the first click granted the XP"), First.bGrantedXP);
	Test.TestTrue(TEXT("the duplicate click was dropped before the service (the guard processed one request)"),
		ServiceCalls == 1);
	Test.TestEqual(TEXT("the guard counted one accepted request"), Guard.GetAcceptedCount(), 1);
	Test.TestEqual(TEXT("the guard counted the dropped duplicate"), Guard.GetRejectedCount(), 1);
	Test.TestEqual(TEXT("the claimed draft was consumed"), Fixture.Profile->GetPendingRewards().Num(), 0);
	Test.TestTrue(TEXT("the inventory holds the ORIGINAL instance (same InstanceId, no re-roll)"),
		Fixture.Profile->GetInventory().Contains(Original.InstanceId));
	// The claim MOVED the pre-generated instance from the draft into the
	// inventory: the combined set itself stays exactly the same (one XP, one
	// instance, no duplicate despite the double click).
	M3_021_ExpectXPAndItems(Test, Before, *Fixture.Profile, URewardService::RewardXPPerClear, {},
		TEXT("after the claim double click"));

	// The idempotent backstop: even a request that bypasses the guard cannot
	// double anything - the persisted-state gate answers AlreadyClaimed.
	const FM3_021_ProgressState AfterDoubleClick = M3_021_CaptureProgressState(*Fixture.Profile);
	const FRewardClaimAtomicOutcome Bypass = Fixture.Reward->ClaimPendingAtomic(6405, Fixture.Profile, Service);
	Test.TestEqual(TEXT("the guard-bypassing repeat answers AlreadyClaimed"),
		Bypass.Result, ERewardClaimAtomicResult::AlreadyClaimed);
	Test.TestFalse(TEXT("the bypass granted no XP"), Bypass.bGrantedXP);
	M3_021_ExpectXPAndItems(Test, AfterDoubleClick, *Fixture.Profile, 0, {}, TEXT("after the bypass probe"));

	M3_021_CleanupTestSlots(Test, *Service, Storage);

	// -- Retry half: the REAL HUD request path in a world scene ---------------
	FM3_021_Scene Scene;
	if (!Scene.Build(*this, TEXT("room_m3_021_retry"), /*bWithHud*/ true))
	{
		return true;
	}
	if (!Scene.FailRunThroughPlayerDeath(*this))
	{
		return true;
	}
	Test.TestEqual(TEXT("the failure fired exactly one end event"), Scene.EndedCount, 1);
	Test.TestTrue(TEXT("the real HUD prepared the result screen"), Scene.Hud->HasRoomResultScreen());
	Test.TestEqual(TEXT("the pawn broadcast its own death lifecycle"), Scene.PlayerDiedCount, 1);

	// The fast double click at the request level (both clicks hit the same
	// handler the Retry button forwards to).
	const uint64 FailedRunId = Scene.Session->GetRunId();
	Scene.Hud->HandleRetryRequested();
	const uint64 RetriedRunId = Scene.Session->GetRunId();
	Scene.Hud->HandleRetryRequested();

	Test.TestEqual(TEXT("the duplicate retry request started no further run"),
		Scene.Session->GetRunId(), RetriedRunId);
	Test.TestTrue(TEXT("the first retry restarted the failed run with a new RunId"), RetriedRunId > FailedRunId);
	Test.TestTrue(TEXT("the session runs again after the retry"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	Test.TestEqual(TEXT("exactly two runs started (failed + retried)"), Scene.StartedCount, 2);
	Test.TestEqual(TEXT("still exactly one end event (the retry run has not ended)"), Scene.EndedCount, 1);
	Test.TestFalse(TEXT("the result screen was dismissed by the retry"), Scene.Hud->HasRoomResultScreen());
	Test.TestTrue(TEXT("the input focus returned to the game after the retry"),
		Scene.Hud->PeekRoomResultInputFocus().GetPhase() == ERoomResultInputPhase::RestoredToGame);
	Test.TestEqual(TEXT("the input switch happened exactly once per direction"),
		Scene.Hud->PeekRoomResultInputFocus().GetCaptureCount()
		+ Scene.Hud->PeekRoomResultInputFocus().GetRestoreCount(), 2);
	Test.TestTrue(TEXT("the player was revived and can move again"), Scene.Player->GetHealth()->IsAlive());
	return true;
}

// -- Failure 6: interrupted save (M3-016 semantics) --------------------------------
// The three interrupt windows of the claim-snapshot save, each on its own
// fresh fixture: (1) the crash BEFORE the claim-snapshot slot write, (2) the
// crash AFTER the slot write with the index never committing, (3) the index
// commit TORN into garbage. Every restart (a NEW save service + StartupLoad +
// RestoreFromSave into a fresh profile) ends with the total XP grown by
// exactly the design 50 and exactly the ORIGINAL pre-generated instance -
// never a duplicate, never a re-roll.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_021InterruptedSaveThreeWindowsRestartIdempotent,
	"UEMMO.Tasks.M3_021.InterruptedSaveThreeWindowsRestartIdempotent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_021InterruptedSaveThreeWindowsRestartIdempotent::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;

	// -- Window 1: crash BEFORE the claim-snapshot slot write (ordinal 3) ------
	{
		FM3_021_MemoryStorage Storage;
		FM3_021_ProfileFixture Fixture;
		if (!Fixture.Setup(Test))
		{
			return true;
		}
		UProfileSaveService* Service = M3_021_NewService(Test, &Storage);
		if (!Service)
		{
			return true;
		}
		const FRoomResult Result = M3_021_MakeResult(/*RunId*/ 6601, /*SettlementId*/ 6601, /*Seed*/ 66011, true);
		const FRewardBeginOutcome Begin = Fixture.Reward->BeginReward(Result, M3_021_MakeTrainingCatalog());
		Test.TestEqual(TEXT("window 1 setup: the settlement began with a pending draft"),
			Begin.Result, ERewardBeginResult::Applied);
		if (Begin.Result != ERewardBeginResult::Applied || Begin.Draft.Items.Num() != 1)
		{
			M3_021_CleanupTestSlots(Test, *Service, Storage);
			return true;
		}
		const FItemInstance Original = Begin.Draft.Items[0];
		const FM3_021_ProgressState Before = M3_021_CaptureProgressState(*Fixture.Profile);

		FM3_021_CrashStorage Crash(&Storage);
		Crash.FailWriteOrdinal = 3;
		Service->SetStorage(&Crash);
		const FRewardClaimAtomicOutcome Interrupted = Fixture.Reward->ClaimPendingAtomic(6601, Fixture.Profile, Service);
		Test.TestEqual(TEXT("window 1: the interrupted claim does not confirm success"),
			Interrupted.Result, ERewardClaimAtomicResult::SaveFailed);
		Test.TestFalse(TEXT("window 1: the interrupted claim committed no claim save"), Interrupted.bSaveCommitted);
		M3_021_ExpectXPAndItems(Test, Before, *Fixture.Profile, 0, {}, TEXT("window 1: after the interrupted claim"));

		// RESTART: the persisted draft state re-claims the XP exactly once.
		FM3_021_RestartedState Restarted;
		if (!Restarted.Setup(Test, Storage, TEXT("window 1 restart")))
		{
			M3_021_CleanupTestSlots(Test, *Service, Storage);
			return true;
		}
		Test.TestEqual(TEXT("window 1: the restart recovered the index-active draft save"),
			static_cast<int32>(Restarted.LoadResult), static_cast<int32>(EStartupLoadResult::Recovered));
		Test.TestEqual(TEXT("window 1: the restored pending draft survived"),
			Restarted.Profile->GetPendingRewards().Num(), 1);
		Test.TestFalse(TEXT("window 1: the settlement is not recorded as applied yet"),
			Restarted.Profile->IsSettlementApplied(6601));
		const FRewardClaimAtomicOutcome Reclaim = Restarted.Reward->ClaimPendingAtomic(6601, Restarted.Profile, Restarted.SaveService);
		Test.TestEqual(TEXT("window 1: the re-claim after the restart succeeds"),
			Reclaim.Result, ERewardClaimAtomicResult::Claimed);
		Test.TestEqual(TEXT("window 1: the re-claim consumed the draft"),
			Restarted.Profile->GetPendingRewards().Num(), 0);
		Test.TestTrue(TEXT("window 1: the inventory holds the ORIGINAL instance (same InstanceId, no re-roll)"),
			Restarted.Profile->GetInventory().Contains(Original.InstanceId));
		// The re-claim MOVED the pre-generated instance from the persisted
		// draft into the inventory: the combined set stays exactly the same.
		M3_021_ExpectXPAndItems(Test, Before, *Restarted.Profile, URewardService::RewardXPPerClear, {},
			TEXT("window 1: the total across the interruption"));
		M3_021_CleanupTestSlots(Test, *Service, Storage);
	}

	// -- Window 2: crash AFTER the slot write, the index never commits ---------
	{
		FM3_021_MemoryStorage Storage;
		FM3_021_ProfileFixture Fixture;
		if (!Fixture.Setup(Test))
		{
			return true;
		}
		UProfileSaveService* Service = M3_021_NewService(Test, &Storage);
		if (!Service)
		{
			return true;
		}
		const FRoomResult Result = M3_021_MakeResult(/*RunId*/ 6602, /*SettlementId*/ 6602, /*Seed*/ 66021, true);
		const FRewardBeginOutcome Begin = Fixture.Reward->BeginReward(Result, M3_021_MakeTrainingCatalog());
		Test.TestEqual(TEXT("window 2 setup: the settlement began with a pending draft"),
			Begin.Result, ERewardBeginResult::Applied);
		if (Begin.Result != ERewardBeginResult::Applied || Begin.Draft.Items.Num() != 1)
		{
			M3_021_CleanupTestSlots(Test, *Service, Storage);
			return true;
		}
		const FItemInstance Original = Begin.Draft.Items[0];
		const FM3_021_ProgressState Before = M3_021_CaptureProgressState(*Fixture.Profile);

		FM3_021_CrashStorage Crash(&Storage);
		Crash.FailWriteOrdinal = 4;
		Service->SetStorage(&Crash);
		const FRewardClaimAtomicOutcome Interrupted = Fixture.Reward->ClaimPendingAtomic(6602, Fixture.Profile, Service);
		Test.TestEqual(TEXT("window 2: the interrupted claim does not confirm success"),
			Interrupted.Result, ERewardClaimAtomicResult::SaveFailed);
		Test.TestFalse(TEXT("window 2: the interrupted claim committed no claim save"), Interrupted.bSaveCommitted);
		Test.TestTrue(TEXT("window 2: the uncommitted claim-snapshot slot content lingers"),
			Storage.DoesSlotExist(M3_021_SlotBName()));
		M3_021_ExpectXPAndItems(Test, Before, *Fixture.Profile, 0, {}, TEXT("window 2: after the interrupted claim"));

		FM3_021_RestartedState Restarted;
		if (!Restarted.Setup(Test, Storage, TEXT("window 2 restart")))
		{
			M3_021_CleanupTestSlots(Test, *Service, Storage);
			return true;
		}
		Test.TestEqual(TEXT("window 2: the restart recovered the index-active save (not the uncommitted slot)"),
			static_cast<int32>(Restarted.LoadResult), static_cast<int32>(EStartupLoadResult::Recovered));
		Test.TestFalse(TEXT("window 2: the settlement is not recorded as applied yet"),
			Restarted.Profile->IsSettlementApplied(6602));
		const FRewardClaimAtomicOutcome Reclaim = Restarted.Reward->ClaimPendingAtomic(6602, Restarted.Profile, Restarted.SaveService);
		Test.TestEqual(TEXT("window 2: the re-claim after the restart succeeds"),
			Reclaim.Result, ERewardClaimAtomicResult::Claimed);
		Test.TestEqual(TEXT("window 2: the re-claim consumed the draft"),
			Restarted.Profile->GetPendingRewards().Num(), 0);
		Test.TestTrue(TEXT("window 2: the inventory holds the ORIGINAL instance (same InstanceId, no re-roll)"),
			Restarted.Profile->GetInventory().Contains(Original.InstanceId));
		// The re-claim MOVED the pre-generated instance from the persisted
		// draft into the inventory: the combined set stays exactly the same.
		M3_021_ExpectXPAndItems(Test, Before, *Restarted.Profile, URewardService::RewardXPPerClear, {},
			TEXT("window 2: the total across the interruption"));

		// The committed state carries the claim - the stale uncommitted slot
		// did not resurrect anything.
		const FProfileLoadOutcome Committed = Restarted.SaveService->LoadActiveProfile();
		Test.TestEqual(TEXT("window 2: the committed save loads"),
			static_cast<int32>(Committed.Result), static_cast<int32>(EProfileLoadResult::Success));
		if (Committed.Result == EProfileLoadResult::Success)
		{
			Test.TestEqual(TEXT("window 2: the committed XP is the once-granted total"),
				Committed.Snapshot.XP, URewardService::RewardXPPerClear);
			Test.TestEqual(TEXT("window 2: the committed inventory holds exactly one item"),
				Committed.Inventory.Count(), 1);
			Test.TestTrue(TEXT("window 2: the committed applied record survives"),
				Committed.AppliedSettlementIds.Contains(6602ull));
		}
		M3_021_CleanupTestSlots(Test, *Service, Storage);
	}

	// -- Window 3: the index commit TORN into garbage (ordinal 4) ---------------
	{
		FM3_021_MemoryStorage Storage;
		FM3_021_ProfileFixture Fixture;
		if (!Fixture.Setup(Test))
		{
			return true;
		}
		UProfileSaveService* Service = M3_021_NewService(Test, &Storage);
		if (!Service)
		{
			return true;
		}
		const FRoomResult Result = M3_021_MakeResult(/*RunId*/ 6603, /*SettlementId*/ 6603, /*Seed*/ 66031, true);
		const FRewardBeginOutcome Begin = Fixture.Reward->BeginReward(Result, M3_021_MakeTrainingCatalog());
		Test.TestEqual(TEXT("window 3 setup: the settlement began with a pending draft"),
			Begin.Result, ERewardBeginResult::Applied);
		if (Begin.Result != ERewardBeginResult::Applied || Begin.Draft.Items.Num() != 1)
		{
			M3_021_CleanupTestSlots(Test, *Service, Storage);
			return true;
		}
		const FItemInstance Original = Begin.Draft.Items[0];
		const FM3_021_ProgressState Before = M3_021_CaptureProgressState(*Fixture.Profile);

		FM3_021_CrashStorage Crash(&Storage);
		Crash.TearIndexWriteOrdinal = 4;
		Service->SetStorage(&Crash);
		const FRewardClaimAtomicOutcome CommittedBlind = Fixture.Reward->ClaimPendingAtomic(6603, Fixture.Profile, Service);
		Test.TestEqual(TEXT("window 3: the service confirmed the claim from its storage view (the torn index is invisible to it)"),
			CommittedBlind.Result, ERewardClaimAtomicResult::Claimed);
		Test.TestTrue(TEXT("window 3: the confirmed claim's snapshot save reported a commit"),
			CommittedBlind.bSaveCommitted);
		UProfileIndexSaveGame* RawIndex = Cast<UProfileIndexSaveGame>(Storage.ReadSlot(M3_021_IndexSlotName()));
		Test.TestNotNull(TEXT("window 3: the torn index file exists"), RawIndex);
		if (RawIndex)
		{
			Test.TestEqual(TEXT("window 3: the torn index is not a usable commit (invalid active slot)"),
				RawIndex->ActiveSlotIndex, 7);
		}

		// RESTART: the startup pass falls back to the generation scan and
		// recovers the committed claim snapshot.
		FM3_021_RestartedState Restarted;
		if (!Restarted.Setup(Test, Storage, TEXT("window 3 restart")))
		{
			M3_021_CleanupTestSlots(Test, *Service, Storage);
			return true;
		}
		Test.TestEqual(TEXT("window 3: the restart recovered the claim save by scan (fallback)"),
			static_cast<int32>(Restarted.LoadResult), static_cast<int32>(EStartupLoadResult::RecoveredFallback));
		Test.TestEqual(TEXT("window 3: no pending draft was restored"),
			Restarted.Profile->GetPendingRewards().Num(), 0);
		Test.TestTrue(TEXT("window 3: the restored inventory holds the ORIGINAL instance (same InstanceId, no re-roll)"),
			Restarted.Profile->GetInventory().Contains(Original.InstanceId));
		// The committed claim MOVED the pre-generated instance into the
		// inventory: the combined set stays exactly the same.
		M3_021_ExpectXPAndItems(Test, Before, *Restarted.Profile, URewardService::RewardXPPerClear, {},
			TEXT("window 3: the recovered total across the torn commit"));
		const FRewardClaimAtomicOutcome Reclaim = Restarted.Reward->ClaimPendingAtomic(6603, Restarted.Profile, Restarted.SaveService);
		Test.TestEqual(TEXT("window 3: the re-claim answers AlreadyClaimed from the persisted state"),
			Reclaim.Result, ERewardClaimAtomicResult::AlreadyClaimed);
		Test.TestFalse(TEXT("window 3: the re-claim grants no XP again"), Reclaim.bGrantedXP);
		M3_021_ExpectXPAndItems(Test, Before, *Restarted.Profile, URewardService::RewardXPPerClear, {},
			TEXT("window 3: the total stays exactly the design 50"));
		M3_021_CleanupTestSlots(Test, *Service, Storage);
	}
	return true;
}

#endif
