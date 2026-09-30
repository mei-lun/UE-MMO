// M3-020: the single-player progression loop regression. One scenario walks
// the FULL growth loop through the real service chain only - no test ever
// writes a post-upgrade/equip final state by hand:
//
//   first session:  GameFlow EnterRoom (injected map opener) -> session
//     StartRoom + BeginWaves -> two waves cleared through the legal lethal
//     ApplyDamage path -> Cleared -> the archived FRoomResult feeds
//     URewardService::BeginReward (settlement draft) ->
//     URewardService::ClaimPendingAtomic (XP +50 once, the ORIGINAL
//     pre-generated instance into the inventory) -> equip through the real
//     entries (FEquipmentModel::Equip slot mapping + FStatCalculator
//     recalculation + the pawn's TryEquipStatBonus wiring) -> UProfileSaveService::
//     SaveProfile commits the equipped snapshot;
//   "new process":  a NEW save service instance + StartupLoad +
//     RestoreFromSave (the M3-015/M3-016 restart model), driven through a
//     SECOND real game instance whose UGameFlowSubsystem::Initialize runs the
//     production startup chain (the M3-017 injected-service seam) - the
//     restored CharacterId/Level/XP/item InstanceId/equipment slot must match
//     the saved ones field for field;
//   second session: re-derive the equipped bonus from the restored bindings
//     (the design rule: RestoreFromSave resets the bonus row, the gameplay
//     layer re-derives it), EnterRoom again, and the same light_01 attack on
//     the same Defense-0 training enemy deducts 15 instead of the 10 measured
//     before the equipment existed - a real health-pool deduction through the
//     wired combat attributes, never a UI-only display.
//
// The interrupt/full-bag paths run on the bare service fixtures (the M3-016
// precedent, no world needed): a claim whose claim-snapshot slot write fails
// restarts into the persisted draft state and re-claims the XP exactly once;
// a full-inventory claim retains the pending item and the retained draft
// survives a restart with its original InstanceId.
//
// Harness: the engine FTestWorldWrapper precedent (RoomScenarioTests/
// ProfileCombatStatsTests) - manually ticked temp worlds with real game
// instances, real ticks, real walking physics and the injected session clock
// driven alongside every tick. Test doubles and helpers carry unique M3_020
// names (copied, never modified, from the M2-014/M3-010/M3-015/M3-016
// suites). Every storage is an in-memory ISaveStorage on the dedicated
// "M3_020Slot_" prefix and each test deletes its slots at the end - the
// player's real "Profile_" saves are unreachable by construction and repeat
// runs accumulate nothing.
//
// Determinism note: the starter drop table picks weapon_training (Attack +5,
// the card's equip case) only for some room seeds. The first session probes
// StartRoom runs (pure seed -> pick preview through the public
// URewardService::DeriveRewardSeed + FDropGenerator::GenerateReward functions)
// until a run whose seed yields the weapon is found; that run is then played
// out for real. The preview consumes nothing - BeginReward performs the actual
// generation from the archived run result.
//
// Each world scene records a progression event sequence (flow, probes, wave
// spawns/deaths, reward, equip, damage measurements) and the main test writes
// the combined JSON evidence under Artifacts/Tasks/M3-020/scenario-json.
#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/CombatHitTypes.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/EnemyDefinition.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Enemy/TrainingEnemy.h"
#include "../Items/DropGenerator.h"
#include "../Items/DropTable.h"
#include "../Items/EquipmentModel.h"
#include "../Items/InventoryModel.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../Items/StatCalculator.h"
#include "../Persistence/ProfileSaveService.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Profile/ProfileSubsystem.h"
#include "../Profile/RewardService.h"
#include "../PrototypeCharacter.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomResult.h"
#include "../Room/RoomSessionSubsystem.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Templates/Function.h"
#include "Tests/AutomationCommon.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_020
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M3_020_FrameSeconds = 1.0f / 60.0f;

	// Remote scene bases so the temp worlds can never collide with the other
	// suites' arenas (and never with each other: both scenes of the main test
	// stay alive at once). X horizontal, Y depth, Z height; the floor top sits
	// at the base Z.
	const FVector M3_020_SceneBaseA(96000.0, 61000.0, 600.0);
	const FVector M3_020_SceneBaseB(96000.0, 71000.0, 600.0);

	// Floor box: half thickness 100 cm, top face exactly at the scene base Z.
	const float M3_020_FloorHalfThickness = 100.0f;
	const float M3_020_FloorHalfExtentXY = 4000.0f;

	// Spawn heights above the floor top: characters drop a short distance and
	// settle onto the floor through real physics.
	const float M3_020_PlayerSpawnHeight = 120.0f;
	const float M3_020_EnemySpawnHeight = 90.0f;

	// The light_01 hit box offset X=95 and half extent X=85 (M1-009 assets)
	// cover base.X+10..base.X+180 for facing +1, so the training enemy feet
	// anchor at +95 stays reachable (the M3-010 placement convention).
	const float M3_020_EnemyFeetOffsetX = 95.0f;

	// The wave spawner's slot interval and the session's inter-wave wait (the
	// M2-007/M2-008 production constants the wave phase rides on).
	constexpr double M3_020_SpawnIntervalSeconds = 0.3;
	constexpr double M3_020_WaveGapSeconds = 1.0;

	// Loose birth-location band of the kill helper (cm), the M2-014 tolerance.
	const double M3_020_BirthLocationBandCm = 300.0;

	// Frame caps: the settle phase waits for the ground contact of both
	// characters; the hit wait covers a full press + light_01 start + active
	// window several times over (the M3-010 caps).
	constexpr int32 M3_020_MaxSettleFrames = 300;
	constexpr int32 M3_020_MaxHitWaitFrames = 600;

	// The light_01 damage expectations (Docs/01 section 8.2, M1-019):
	// damage = max(1, round((baseDamage + AttackPower * coefficient) * 100 /
	// (100 + max(0, Defense)))). light_01 carries baseDamage 10 with
	// coefficient 1.0, so with Defense 0: Attack 0 -> 10, Attack +5 -> 15
	// (the M3-010 measured baselines).
	constexpr float M3_020_BaseDamageLight01 = 10.0f;
	constexpr float M3_020_DamageAttack5 = 15.0f;

	// The equipment grant of the weapon_training definition (the pinned
	// training catalog values; RolledStats copy BaseStats with no variance).
	constexpr float M3_020_WeaponAttackGrant = 5.0f;

	// Probe budget for the weapon-seed search (the pick is 50% weapon: the
	// budget fails with probability 0.8^60, far below any flake concern).
	constexpr int32 M3_020_MaxWeaponProbes = 60;

	// The isolated save prefix of every service operation in this suite (never
	// "Profile_": the automation cannot reach the player's real saves).
	const TCHAR* M3_020_SlotPrefix = TEXT("M3_020Slot_");

	// The three M3_020Slot_ slot names every test operates on.
	static FString M3_020_SlotAName() { return FString(TEXT("M3_020Slot_A")); }
	static FString M3_020_SlotBName() { return FString(TEXT("M3_020Slot_B")); }
	static FString M3_020_IndexSlotName() { return FString(TEXT("M3_020Slot_Index")); }

	// -- Storage doubles (the M3-016 copies, renamed) ----------------------------

	/**
	 * In-memory ISaveStorage: every written slot is stored as an independent
	 * duplicate (rooted against GC), every read returns a fresh duplicate -
	 * the same semantics as disk bytes. Counts WriteSlot calls so the crash
	 * windows can be pinned to exact write ordinals.
	 */
	class FM3_020_MemoryStorage final : public ISaveStorage
	{
	public:
		virtual ~FM3_020_MemoryStorage() override
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
	 * Failure injection decorator over another storage (the M3-016 model):
	 * the atomic claim performs exactly two saves (draft persistence, claim
	 * snapshot), each = one slot write + one index commit, so with a fresh
	 * prefix the write ordinals are 1 = draft slot, 2 = draft index, 3 =
	 * claim-snapshot slot, 4 = claim-snapshot index. FailWriteOrdinal = 3
	 * models the process dying while the claim snapshot's slot write runs.
	 * Refused calls still consume an ordinal.
	 */
	class FM3_020_CrashStorage final : public ISaveStorage
	{
	public:
		explicit FM3_020_CrashStorage(ISaveStorage* InInner)
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

	private:
		ISaveStorage* Inner = nullptr;
	};

	// -- Service fixture helpers (the M3-015/M3-016 copies, renamed) --------------

	/** Creates a save service on the M3_020Slot_ prefix (storage optional). */
	static UProfileSaveService* M3_020_NewService(FAutomationTestBase& Test, ISaveStorage* Storage)
	{
		UProfileSaveService* Service = NewObject<UProfileSaveService>(GetTransientPackage());
		if (!Service)
		{
			Test.AddError(TEXT("setup: NewObject<UProfileSaveService> returned null"));
			return nullptr;
		}
		if (!Service->Initialize(M3_020_SlotPrefix))
		{
			Test.AddError(FString::Printf(TEXT("setup: Initialize('%s') rejected the prefix"), M3_020_SlotPrefix));
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
	struct FM3_020_ProfileFixture
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

		~FM3_020_ProfileFixture()
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
	 * The RESTART state (the M3-016 model): a brand-new save service over the
	 * same storage plus a brand-new profile whose memory is filled ONLY by
	 * RestoreFromSave from the startup load's recovered save - every restart
	 * assertion is decided from this persisted state alone, never from any
	 * earlier in-memory history.
	 */
	struct FM3_020_RestartedState
	{
		UGameInstance* Owner = nullptr;
		UProfileSaveService* SaveService = nullptr;
		UProfileSubsystem* Profile = nullptr;
		URewardService* Reward = nullptr;
		EStartupLoadResult LoadResult = EStartupLoadResult::RecoveryError;

		bool Setup(FAutomationTestBase& Test, ISaveStorage& Storage, const TCHAR* Scenario)
		{
			SaveService = M3_020_NewService(Test, &Storage);
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

		~FM3_020_RestartedState()
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
	 * M3_020Slot_ names are gone afterwards (nothing outside the prefix is
	 * reachable, so repeat runs accumulate nothing).
	 */
	static void M3_020_CleanupTestSlots(FAutomationTestBase& Test, UProfileSaveService& Service, ISaveStorage& Storage)
	{
		Service.DeleteTestSlots();
		Test.TestTrue(TEXT("cleanup: M3_020Slot_A deleted"), !Storage.DoesSlotExist(M3_020_SlotAName()));
		Test.TestTrue(TEXT("cleanup: M3_020Slot_B deleted"), !Storage.DoesSlotExist(M3_020_SlotBName()));
		Test.TestTrue(TEXT("cleanup: M3_020Slot_Index deleted"), !Storage.DoesSlotExist(M3_020_IndexSlotName()));
	}

	// -- Item fixtures ---------------------------------------------------------

	/** The three training definitions in a fresh catalog (mirrors Data/items.json). */
	static FItemDefinitionCatalog M3_020_MakeTrainingCatalog()
	{
		FItemDefinitionCatalog Catalog;

		FItemDefinition Weapon;
		Weapon.DefinitionId = FName(TEXT("weapon_training"));
		Weapon.DisplayName = TEXT("weapon_training");
		Weapon.Slot = EItemSlot::Weapon;
		Weapon.BaseStats.Attack = M3_020_WeaponAttackGrant;
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
	static FItemDefinition M3_020_MakeFillerDefinition()
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
	static TArray<FGuid> M3_020_FillInventory(FInventoryModel& Inventory, const FItemDefinition& Definition, int32 Count)
	{
		TArray<FGuid> FillerIds;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FItemInstance Filler = MakeItemInstance(Definition, /*RollSeed*/ 52000 + Index);
			FillerIds.Add(Filler.InstanceId);
			Inventory.TryAdd(Filler);
		}
		return FillerIds;
	}

	/** The stored instance with the given id, or nullptr when absent. */
	static const FItemInstance* M3_020_FindStored(const FInventoryModel& Inventory, const FGuid& InstanceId)
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

	/** Field-for-field instance equality (identity, definition, roll seed, level, stats). */
	static bool M3_020_InstancesEqual(const FItemInstance& A, const FItemInstance& B)
	{
		return A.InstanceId == B.InstanceId &&
			A.DefinitionId == B.DefinitionId &&
			A.RollSeed == B.RollSeed &&
			A.Level == B.Level &&
			A.RolledStats.Attack == B.RolledStats.Attack &&
			A.RolledStats.Defense == B.RolledStats.Defense &&
			A.RolledStats.MaxHP == B.RolledStats.MaxHP;
	}

	/**
	 * The gameplay-layer equipment stat derivation (the M3-005 semantics): the
	 * plain component sum of the equipped instances' rolled stats - the row
	 * the equip flow pushes into UProfileSubsystem::SetEquippedStatBonus
	 * (GetProfileSnapshot adds the level base row itself). Deliberately NOT
	 * routed through FStatCalculator::Recalculate with a zero base row: the
	 * calculator's final-MaxHP floor of 1 belongs to the FINAL row (base +
	 * bonus, recomputed inside GetProfileSnapshot) and would poison a pure
	 * bonus row with a phantom MaxHP +1.
	 */
	static FItemStats M3_020_ComputeEquippedRow(const FInventoryModel& Inventory, const FGuid& EquippedId)
	{
		FItemStats Sum;
		const FItemInstance* Instance = M3_020_FindStored(Inventory, EquippedId);
		if (Instance != nullptr)
		{
			// RolledStats are factory copies of validated definitions (finite,
			// >= 0); the defensive clamp mirrors the calculator's per-field
			// hardening so a poisoned stored row can never drain a stat.
			const float Fields[3] = { Instance->RolledStats.Attack, Instance->RolledStats.Defense, Instance->RolledStats.MaxHP };
			float* Targets[3] = { &Sum.Attack, &Sum.Defense, &Sum.MaxHP };
			for (int32 Field = 0; Field < 3; ++Field)
			{
				*Targets[Field] += FMath::IsFinite(Fields[Field]) ? FMath::Max(0.0f, Fields[Field]) : 0.0f;
			}
		}
		return Sum;
	}

	/** A finished-run result value; RunId is kept distinct alongside SettlementId. */
	static FRoomResult M3_020_MakeResult(uint64 RunId, uint64 SettlementId, int32 Seed, bool bCleared)
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

	// -- Progression event log (the M2-014 copy with progression event types) --

	/** One recorded scenario event: the scenario clock timestamp plus a stable type tag and free-text detail. */
	struct FM3_020_Event
	{
		double TimeSeconds = 0.0;
		FString Type;
		FString Detail;
	};

	/** The per-scene event log; the JSON writer serializes it as report evidence. */
	struct FM3_020_EventLog
	{
		TArray<FM3_020_Event> Events;

		void Add(double NowSeconds, const TCHAR* Type, const FString& Detail)
		{
			FM3_020_Event Event;
			Event.TimeSeconds = NowSeconds;
			Event.Type = Type;
			Event.Detail = Detail;
			Events.Add(Event);
		}

		/** One human-readable line per event (the AddInfo diagnostics). */
		FString Describe() const
		{
			FString Out;
			for (const FM3_020_Event& Event : Events)
			{
				Out += FString::Printf(TEXT("t=%.3fs %s (%s)\n"), Event.TimeSeconds, *Event.Type, *Event.Detail);
			}
			return Out;
		}
	};

	// Minimal JSON string escaping for the evidence writer (the M2-014 helper).
	static FString M3_020_JsonEscape(const FString& In)
	{
		FString Out;
		for (const TCHAR Character : In)
		{
			if (Character == TEXT('\\'))
			{
				Out += TEXT("\\\\");
			}
			else if (Character == TEXT('"'))
			{
				Out += TEXT("\\\"");
			}
			else if (Character == TEXT('\n'))
			{
				Out += TEXT("\\n");
			}
			else
			{
				Out.AppendChar(Character);
			}
		}
		return Out;
	}

	static FString M3_020_EventsJsonArray(const FM3_020_EventLog& Log)
	{
		FString Json = TEXT("[\n");
		for (int32 Index = 0; Index < Log.Events.Num(); ++Index)
		{
			const FM3_020_Event& Event = Log.Events[Index];
			Json += FString::Printf(TEXT("    { \"t\": %.6f, \"type\": \"%s\", \"detail\": \"%s\" }%s\n"),
				Event.TimeSeconds, *M3_020_JsonEscape(Event.Type), *M3_020_JsonEscape(Event.Detail),
				(Index + 1 < Log.Events.Num()) ? TEXT(",") : TEXT(""));
		}
		Json += TEXT("  ]");
		return Json;
	}

	/**
	 * Writes the combined progression evidence JSON (both scenes' event
	 * sequences plus the summary fields) under Artifacts/Tasks/M3-020/
	 * scenario-json. Returns false (and reports) when the file could not be
	 * written.
	 */
	static bool M3_020_WriteProgressionJson(FAutomationTestBase& Test, const TCHAR* FileName,
		const FM3_020_EventLog& LogA, const FM3_020_EventLog& LogB, const FString& ExtraFields)
	{
		FString Json = TEXT("{\n");
		Json += TEXT("  \"scenario\": \"full-progression-loop\",\n");
		Json += TEXT("  \"recorded_by\": \"UEMMO.Tasks.M3_020\",\n");
		if (!ExtraFields.IsEmpty())
		{
			Json += TEXT("  ") + ExtraFields + TEXT(",\n");
		}
		Json += TEXT("  \"events_run_a\": ") + M3_020_EventsJsonArray(LogA) + TEXT(",\n");
		Json += TEXT("  \"events_run_b\": ") + M3_020_EventsJsonArray(LogB) + TEXT("\n");
		Json += TEXT("}\n");

		const FString Directory = FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M3-020") / TEXT("scenario-json");
		IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);
		const FString Path = Directory / FileName;
		if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Test.AddError(FString::Printf(TEXT("could not write the progression evidence JSON to %s"), *Path));
			return false;
		}
		Test.AddInfo(FString::Printf(TEXT("progression evidence JSON written to %s"), *Path));
		return true;
	}

	// -- World scene helpers (the M2-014/M3-010 copies, renamed) ------------------

	// World-static blocking box (the floor shares the M2-010 builder shape).
	static AActor* M3_020_SpawnBoxActor(UWorld& World, const FVector& Center, const FVector& HalfExtent, const TCHAR* Name)
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
	static APrototypeCharacter* M3_020_SpawnPlayer(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			Base + FVector(0.0, 0.0, M3_020_PlayerSpawnHeight),
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

	// Spawns the real training enemy at the shared feet anchor in front of the
	// player (the light_01 hit box covers base.X+10..base.X+180 for facing +1).
	static ATrainingEnemy* M3_020_SpawnTrainingEnemy(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		ATrainingEnemy* Enemy = World.SpawnActor<ATrainingEnemy>(
			ATrainingEnemy::StaticClass(),
			Base + FVector(M3_020_EnemyFeetOffsetX, 0.0, M3_020_EnemySpawnHeight),
			FRotator::ZeroRotator, Params);
		if (Enemy == nullptr)
		{
			return nullptr;
		}
		if (UCharacterMovementComponent* Movement = Enemy->GetCharacterMovement())
		{
			Movement->bRunPhysicsWithNoController = true;
		}
		return Enemy;
	}

	// Definition double of the Data/enemies.json "melee_grunt" row (the
	// M2-008/M2-010 suite precedent).
	static UEnemyDefinition* M3_020_MakeEnemyDef()
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

	/**
	 * Hand-built room definition double. Variant 0 is the 2+3 wave shape of
	 * the room_training_01 scenario (the M2-014 arena); variant 1 is a
	 * one-entry shape for the second session (its run starts but spawns no
	 * waves). Both carry the selectable map path - GameFlowSubsystem::EnterRoom
	 * validates RoomId AND MapPath before accepting an entry.
	 */
	static URoomDefinition* M3_020_MakeRoom(const TCHAR* RoomId, const FVector& Base, int32 Variant)
	{
		URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Room->RoomId = FName(RoomId);
		Room->RewardTableId = FName(TEXT("reward_m3_020"));
		Room->MapPath = TSoftObjectPtr<UWorld>(FSoftObjectPath(TEXT("/Game/UEMMO/Maps/L_TrainingArena")));

		if (Variant == 0)
		{
			FRoomWaveDefinition Wave0;
			Wave0.EnemyId = FName(TEXT("melee_grunt"));
			Wave0.Count = 2;
			Wave0.SpawnLocations.Add(Base + FVector(300.0, 0.0, M3_020_EnemySpawnHeight));
			Wave0.SpawnLocations.Add(Base + FVector(450.0, -150.0, M3_020_EnemySpawnHeight));
			Room->Waves.Add(Wave0);

			FRoomWaveDefinition Wave1;
			Wave1.EnemyId = FName(TEXT("melee_grunt"));
			Wave1.Count = 3;
			Wave1.SpawnLocations.Add(Base + FVector(600.0, 100.0, M3_020_EnemySpawnHeight));
			Wave1.SpawnLocations.Add(Base + FVector(750.0, -100.0, M3_020_EnemySpawnHeight));
			Wave1.SpawnLocations.Add(Base + FVector(150.0, 300.0, M3_020_EnemySpawnHeight));
			Room->Waves.Add(Wave1);
		}
		else
		{
			FRoomWaveDefinition Wave0;
			Wave0.EnemyId = FName(TEXT("melee_grunt"));
			Wave0.Count = 1;
			Wave0.SpawnLocations.Add(Base + FVector(300.0, 0.0, M3_020_EnemySpawnHeight));
			Room->Waves.Add(Wave0);
		}
		return Room;
	}

	/**
	 * One world scene: the engine FTestWorldWrapper owns the manually ticked
	 * temp world with a REAL game instance (the wrapper creates and Init()s
	 * it), so the production UProfileSubsystem / UGameFlowSubsystem /
	 * URoomSessionSubsystem resolution runs exactly like the game's. The
	 * optional ProfileSetup runs BETWEEN world creation and play begin (the
	 * M3-010 convention); a parked startup save service (set by the caller
	 * BEFORE Build) is consumed by the flow's Initialize inside
	 * CreateTestWorld - the production restart chain.
	 */
	struct FM3_020_WorldScene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		UGameInstance* GameInstance = nullptr;
		UProfileSubsystem* Profile = nullptr;
		UGameFlowSubsystem* Flow = nullptr;
		URoomSessionSubsystem* Session = nullptr;
		APrototypeCharacter* Player = nullptr;
		UCombatComponent* PlayerCombat = nullptr;
		ATrainingEnemy* Target = nullptr;
		URewardService* Reward = nullptr;
		UProfileSaveService* SaveService = nullptr;
		URoomDefinition* Room = nullptr;

		// The counting map-open simulator the tests inject (a temp world can
		// never truly travel; the seam keeps the real OpenLevel path unused).
		int32 MapOpenCount = 0;
		FString LastOpenedMapPath;

		// The injected scenario clock and the progression event log.
		double ClockSeconds = 0.0;
		FM3_020_EventLog Log;

		// Delegate counters (the assertions read these only).
		int32 StartedCount = 0;
		int32 EndedCount = 0;
		TArray<FRoomResult> EndedResults;
		int32 PlayerDiedCount = 0;

		// Hit recorder: one entry per confirmed hit, in confirmation order.
		TArray<float> HitDamages;

		// Per-frame wave sampling state (the M2-014 pattern).
		int32 LastStartedWaveCount = 0;
		TArray<TWeakObjectPtr<AMeleeEnemy>> TrackedEnemies;
		int32 EnemySerial = 0;
		TArray<TWeakObjectPtr<AActor>> KilledEnemies;

		bool Build(FAutomationTestBase& Test, const TCHAR* Tag, const FVector& Base,
			const TCHAR* RoomId, int32 RoomVariant,
			const TFunction<void(UProfileSubsystem&)>& ProfileSetup)
		{
			if (!Test.TestTrue(TEXT("the manually ticked test world is created (engine FTestWorldWrapper precedent)"),
				Wrapper.CreateTestWorld(EWorldType::Game)))
			{
				return false;
			}
			World = Wrapper.GetTestWorld();
			if (!Test.TestNotNull(TEXT("the test world is available"), World))
			{
				return false;
			}
			GameInstance = World->GetGameInstance();
			Profile = GameInstance ? GameInstance->GetSubsystem<UProfileSubsystem>() : nullptr;
			Flow = GameInstance ? GameInstance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
			Session = World->GetSubsystem<URoomSessionSubsystem>();
			if (!Test.TestNotNull(TEXT("the profile subsystem exists in the test game instance"), Profile)
				|| !Test.TestNotNull(TEXT("the game flow subsystem exists in the test game instance"), Flow)
				|| !Test.TestNotNull(TEXT("the room session subsystem exists in the test world"), Session))
			{
				return false;
			}
			if (ProfileSetup != nullptr)
			{
				ProfileSetup(*Profile);
			}

			// The counting map-open simulator (production never travels in a
			// temp world; the seam keeps the flow's state machine observable).
			Flow->SetMapOpenerForTests([this](const FString& MapPath)
			{
				++MapOpenCount;
				LastOpenedMapPath = MapPath;
				Log.Add(ClockSeconds, TEXT("MapOpened"),
					FString::Printf(TEXT("map %s"), *MapPath));
				return true;
			});

			Session->OnRunStarted().AddLambda([this]()
			{
				++StartedCount;
				Log.Add(ClockSeconds, TEXT("RunStarted"),
					FString::Printf(TEXT("run %llu seed %d"), Session->GetRunId(), Session->GetSeed()));
			});
			Session->OnRunEnded().AddLambda([this](const FRoomResult& Result)
			{
				++EndedCount;
				EndedResults.Add(Result);
				Log.Add(ClockSeconds, TEXT("RunEnded"),
					FString::Printf(TEXT("run %llu cleared=%d killed=%d"),
						Result.RunId, Result.bCleared ? 1 : 0, Result.KilledCount));
			});

			if (!Test.TestTrue(TEXT("play begins in the test world (full actor initialization)"),
				Wrapper.BeginPlayInTestWorld()))
			{
				return false;
			}
			AActor* Floor = M3_020_SpawnBoxActor(*World,
				Base - FVector(0.0, 0.0, M3_020_FloorHalfThickness),
				FVector(M3_020_FloorHalfExtentXY, M3_020_FloorHalfExtentXY, M3_020_FloorHalfThickness),
				TEXT("M3_020_Floor"));
			if (Floor == nullptr)
			{
				Test.AddError(TEXT("the floor failed to spawn"));
				return false;
			}
			// A hand-built world never runs the map-load collision tree pass;
			// build it once before the first tick (the M1-041 pattern).
			World->EnsureCollisionTreeIsBuilt();

			Player = M3_020_SpawnPlayer(*World, Base);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			PlayerCombat = Player->GetCombat();
			if (!Test.TestNotNull(TEXT("the player carries the combat component"), PlayerCombat))
			{
				return false;
			}
			Player->PlayerDied.AddLambda([this]()
			{
				++PlayerDiedCount;
				Log.Add(ClockSeconds, TEXT("PlayerDied"),
					FString::Printf(TEXT("player death #%d"), PlayerDiedCount));
			});
			Target = M3_020_SpawnTrainingEnemy(*World, Base);
			if (!Test.TestNotNull(TEXT("the training enemy spawns"), Target))
			{
				return false;
			}
			if (UCombatComponent* TargetCombat = Target->GetCombatComponent())
			{
				// The same-Defense target of every damage comparison: Defense 0.
				TargetCombat->SetCombatStats(0.0f, 0.0f);
			}
			if (PlayerCombat != nullptr)
			{
				PlayerCombat->OnHitConfirmed.AddLambda([this](const FCombatHit& Hit)
				{
					HitDamages.Add(Hit.Damage);
					Log.Add(ClockSeconds, TEXT("HitConfirmed"),
						FString::Printf(TEXT("damage %.1f"), Hit.Damage));
				});
			}
			// The registration under test (the death handler binds here).
			Session->SetPlayer(Player);
			Room = M3_020_MakeRoom(RoomId, Base, RoomVariant);

			return Settle(Test);
		}

		/** Explicit teardown: clears the static startup seam before the world dies. */
		void TearDown()
		{
			UGameFlowSubsystem::SetStartupSaveServiceForTests(nullptr);
			if (SaveService)
			{
				SaveService->RemoveFromRoot();
				SaveService = nullptr;
			}
			if (Reward)
			{
				Reward->RemoveFromRoot();
				Reward = nullptr;
			}
		}

		/** Attaches the save service (rooted) and the reward service to the scene. */
		bool AttachPersistence(FAutomationTestBase& Test, ISaveStorage& Storage)
		{
			SaveService = M3_020_NewService(Test, &Storage);
			if (!Test.TestNotNull(TEXT("the scene save service is constructible"), SaveService))
			{
				return false;
			}
			SaveService->AddToRoot();
			Reward = GameInstance ? NewObject<URewardService>(GameInstance) : nullptr;
			if (!Test.TestNotNull(TEXT("the scene reward service is constructible"), Reward))
			{
				return false;
			}
			Reward->AddToRoot();
			Reward->BindProfile(Profile);
			return true;
		}

		/** Ticks the world until both characters settled onto the floor. */
		bool Settle(FAutomationTestBase& Test)
		{
			if (Player == nullptr || Target == nullptr)
			{
				return false;
			}
			UCharacterMovementComponent* PlayerMovement = Player->GetCharacterMovement();
			UCharacterMovementComponent* TargetMovement = Target->GetCharacterMovement();
			for (int32 Frame = 0; Frame < M3_020_MaxSettleFrames; ++Frame)
			{
				if (!Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M3_020_FrameSeconds)))
				{
					return false;
				}
				const bool bPlayerGrounded = PlayerMovement != nullptr
					&& PlayerMovement->MovementMode == MOVE_Walking
					&& PlayerMovement->Velocity.Size() < 1.0f;
				const bool bTargetGrounded = TargetMovement != nullptr
					&& TargetMovement->MovementMode == MOVE_Walking
					&& TargetMovement->Velocity.Size() < 1.0f;
				if (bPlayerGrounded && bTargetGrounded)
				{
					return true;
				}
			}
			Test.AddError(TEXT("the characters never settled onto the floor within the settle cap"));
			return false;
		}

		float TargetHealth() const
		{
			const UHealthComponent* TargetHealth = Target ? Target->GetHealthComponent() : nullptr;
			return (TargetHealth != nullptr) ? TargetHealth->GetHealth() : -1.0f;
		}

		/**
		 * Presses Light ONCE through the production intent entry (scheduled
		 * 0.10 s ahead, the M1-041 convention) and ticks until the next hit is
		 * confirmed. OutDamage/OutHpBefore/OutHpAfter receive the applied
		 * damage and the target pool around the hit. Waits for the attack to
		 * return to Free so a following press can start cleanly.
		 */
		bool PressLightAndWaitForHit(FAutomationTestBase& Test, const TCHAR* What,
			float& OutDamage, float& OutHpBefore, float& OutHpAfter)
		{
			OutHpBefore = TargetHealth();
			const double PressAt = World->GetTimeSeconds() + 0.10;
			const int32 HitsBefore = HitDamages.Num();
			bool bPressed = false;
			for (int32 Frame = 0; Frame < M3_020_MaxHitWaitFrames; ++Frame)
			{
				if (!bPressed && World->GetTimeSeconds() + 1e-6 >= PressAt)
				{
					Player->SubmitCombatInput(ECombatInput::Light);
					bPressed = true;
				}
				if (!Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M3_020_FrameSeconds)))
				{
					return false;
				}
				if (HitDamages.Num() > HitsBefore)
				{
					OutDamage = HitDamages[HitsBefore];
					OutHpAfter = TargetHealth();
					Log.Add(ClockSeconds, TEXT("DamageMeasured"),
						FString::Printf(TEXT("%s: damage %.1f, hp %.1f -> %.1f"), What, OutDamage, OutHpBefore, OutHpAfter));
					return WaitAttackIdle(Test);
				}
			}
			Test.AddError(FString::Printf(TEXT("the light press never landed a hit within the wait cap (%s)"), What));
			return false;
		}

		/** Ticks until the combat component is Free again (the next press gate). */
		bool WaitAttackIdle(FAutomationTestBase& Test)
		{
			for (int32 Frame = 0; Frame < M3_020_MaxHitWaitFrames; ++Frame)
			{
				const FCombatSnapshot Snapshot = PlayerCombat->GetSnapshot();
				if (Snapshot.ActionState == ECombatActionState::Free && Snapshot.InstanceId == 0)
				{
					return true;
				}
				if (!Test.TestTrue(TEXT("the test world ticks"), Wrapper.TickTestWorld(M3_020_FrameSeconds)))
				{
					return false;
				}
			}
			Test.AddError(TEXT("the attack never returned to Free within the wait cap"));
			return false;
		}

		/** One event-log line per fresh world observation, after the pump. */
		void Sample()
		{
			if (Session != nullptr)
			{
				const int32 Started = Session->GetStartedWaveCount();
				if (Started > LastStartedWaveCount)
				{
					Log.Add(ClockSeconds, TEXT("WaveStarted"),
						FString::Printf(TEXT("wave %d"), Session->GetCurrentWaveIndex()));
					LastStartedWaveCount = Started;
				}
			}
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				AMeleeEnemy* Enemy = *It;
				if (!IsValid(Enemy))
				{
					continue;
				}
				bool bTracked = false;
				for (const TWeakObjectPtr<AMeleeEnemy>& Known : TrackedEnemies)
				{
					if (Known.Get() == Enemy)
					{
						bTracked = true;
						break;
					}
				}
				if (bTracked)
				{
					continue;
				}
				TrackedEnemies.Add(Enemy);
				const int32 Serial = ++EnemySerial;
				Log.Add(ClockSeconds, TEXT("EnemySpawned"),
					FString::Printf(TEXT("enemy #%d (%s)"), Serial, *Enemy->GetName()));
				if (Enemy->GetHealthComponent() != nullptr)
				{
					Enemy->GetHealthComponent()->OnDied.AddLambda([this, Serial]()
					{
						Log.Add(ClockSeconds, TEXT("EnemyDied"),
							FString::Printf(TEXT("enemy #%d"), Serial));
					});
				}
			}
		}

		/** Injects one exact clock value and samples the world right after. */
		void InjectClock(double NowSeconds)
		{
			ClockSeconds = NowSeconds;
			Session->SetSessionClockSeconds(NowSeconds);
			Sample();
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
			const double StepSeconds = static_cast<double>(M3_020_FrameSeconds);
			double Remaining = Seconds;
			while (Remaining > 1e-9)
			{
				const double StepNow = FMath::Min(StepSeconds, Remaining);
				InjectClock(ClockSeconds + StepNow);
				if (!TickSeconds(Test, M3_020_FrameSeconds))
				{
					return false;
				}
				Sample();
				Remaining -= StepNow;
			}
			return true;
		}

		/** Anchors/continues the CURRENT wave and advances past every slot of it. */
		bool SpawnCurrentWaveFully(FAutomationTestBase& Test, const URoomDefinition& RoomDef, int32 WaveIndex)
		{
			const int32 Count = FMath::Max(0, RoomDef.Waves[WaveIndex].Count);
			const double Span = M3_020_SpawnIntervalSeconds * static_cast<double>(Count - 1) + 0.05;
			return AdvanceSeconds(Test, Span);
		}

		// -- World bookkeeping helpers ------------------------------------

		/** Alive melee enemies (corpses deliberately not counted). */
		int32 AliveEnemies() const
		{
			int32 Count = 0;
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				UHealthComponent* Health = It->GetHealthComponent();
				if (Health != nullptr && Health->IsAlive())
				{
					++Count;
				}
			}
			return Count;
		}

		/** Every melee enemy actor in the world including corpses. */
		int32 TotalEnemies() const
		{
			int32 Count = 0;
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				++Count;
			}
			return Count;
		}

		/** Every actor in the world (the per-run leak check). */
		int32 TotalActors() const
		{
			int32 Count = 0;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				++Count;
			}
			return Count;
		}

		/**
		 * Finds the alive, never-yet-killed melee enemy closest to the given
		 * configured spawn location (the M2-014 closest-live-candidate rule).
		 */
		AMeleeEnemy* FindNextAliveEnemyAt(const FVector& Location) const
		{
			AMeleeEnemy* Best = nullptr;
			double BestDistSquared = TNumericLimits<double>::Max();
			for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
			{
				AMeleeEnemy* Enemy = *It;
				UHealthComponent* Health = (Enemy != nullptr) ? Enemy->GetHealthComponent() : nullptr;
				if (Health == nullptr || !Health->IsAlive())
				{
					continue;
				}
				bool bAlreadyKilled = false;
				for (const TWeakObjectPtr<AActor>& Killed : KilledEnemies)
				{
					if (Killed.Get() == Enemy)
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
		 * Finds the next alive, never-yet-killed melee enemy at or near the
		 * given configured spawn location and kills it through its health
		 * component (the legal lethal ApplyDamage path - the death flows
		 * through the spawner's real death binding into the session
		 * bookkeeping).
		 */
		bool KillNextEnemyAt(FAutomationTestBase& Test, const FVector& Location, const TCHAR* What)
		{
			AMeleeEnemy* Enemy = FindNextAliveEnemyAt(Location);
			if (!Test.TestNotNull(What, Enemy))
			{
				return false;
			}
			const double DistSquared = FVector::DistSquared(Enemy->GetActorLocation(), Location);
			if (!Test.TestTrue(FString::Printf(TEXT("%s was born at or near the configured spawn location (dist %.1f cm)"),
				What, FMath::Sqrt(DistSquared)),
				DistSquared <= M3_020_BirthLocationBandCm * M3_020_BirthLocationBandCm))
			{
				return false;
			}
			Enemy->GetHealthComponent()->ApplyDamage(9999.0f);
			KilledEnemies.Add(Enemy);
			return Test.TestTrue(TEXT("the enemy died from the applied damage"),
				Enemy->GetHealthComponent() != nullptr && !Enemy->GetHealthComponent()->IsAlive());
		}

		/**
		 * Probes StartRoom runs until the run's seed derives the
		 * weapon_training reward (the card's equip case). The pick preview is
		 * a pure function evaluation - URewardService::DeriveRewardSeed plus
		 * FDropGenerator::GenerateReward on the pinned starter table - and
		 * consumes nothing; BeginReward performs the real generation from the
		 * archived result later. A non-weapon probe is recycled with
		 * LeaveRoom (StartRoom accepts the post-exit Exiting state).
		 */
		bool StartWeaponRewardRun(FAutomationTestBase& Test, const FItemDefinitionCatalog& Catalog)
		{
			const FDropTable StarterTable = MakeStarterDropTable();
			for (int32 Probe = 1; Probe <= M3_020_MaxWeaponProbes; ++Probe)
			{
				if (!Test.TestTrue(FString::Printf(TEXT("reward probe %d: StartRoom accepts the run"), Probe),
					Session->StartRoom(Room)))
				{
					return false;
				}
				const int32 RoomSeed = Session->GetSeed();
				const int64 RewardSeed = URewardService::DeriveRewardSeed(RoomSeed);
				const FDropRewardResult Preview = FDropGenerator::GenerateReward(
					RewardSeed, Session->GetSettlementId(), StarterTable, Catalog);
				Log.Add(ClockSeconds, TEXT("RewardProbed"),
					FString::Printf(TEXT("probe %d seed %d -> %s"), Probe, RoomSeed,
						Preview.bSuccess ? *Preview.Instance.DefinitionId.ToString() : TEXT("<rejected>")));
				if (Preview.bSuccess && Preview.Instance.DefinitionId == FName(TEXT("weapon_training")))
				{
					return true;
				}
				if (!Test.TestTrue(FString::Printf(TEXT("reward probe %d: LeaveRoom recycles the run"), Probe),
					Session->LeaveRoom()))
				{
					return false;
				}
			}
			Test.AddError(FString::Printf(TEXT("no weapon_training reward seed was found within %d probes"), M3_020_MaxWeaponProbes));
			return false;
		}

		/**
		 * Plays the CURRENT run to a full clear: wave 0 (2 enemies) born and
		 * killed, the 1.0 s gap elapses on the injected clock, wave 1 (3
		 * enemies) born and killed, Cleared. Every kill flows through the
		 * legal lethal ApplyDamage path.
		 */
		bool ClearBothWaves(FAutomationTestBase& Test)
		{
			const FVector& LocW0A = Room->Waves[0].SpawnLocations[0];
			const FVector& LocW0B = Room->Waves[0].SpawnLocations[1];
			const FVector& LocW1A = Room->Waves[1].SpawnLocations[0];
			const FVector& LocW1B = Room->Waves[1].SpawnLocations[1];
			const FVector& LocW1C = Room->Waves[1].SpawnLocations[2];

			InjectClock(ClockSeconds);
			if (!SpawnCurrentWaveFully(Test, *Room, 0))
			{
				return false;
			}
			if (!Test.TestEqual(TEXT("both wave-0 enemies were birthed"), Session->GetSpawnedEnemyCount(), 2))
			{
				return false;
			}
			if (!KillNextEnemyAt(Test, LocW0A, TEXT("the first wave-0 enemy was found at its location"))
				|| !KillNextEnemyAt(Test, LocW0B, TEXT("the second wave-0 enemy was found at its location")))
			{
				return false;
			}
			if (!Test.TestEqual(TEXT("both wave-0 deaths were counted"), Session->GetKilledCount(), 2))
			{
				return false;
			}

			if (!AdvanceSeconds(Test, M3_020_WaveGapSeconds + 0.05))
			{
				return false;
			}
			if (!Test.TestEqual(TEXT("past the wait the wave index is 1"), Session->GetCurrentWaveIndex(), 1))
			{
				return false;
			}
			if (!SpawnCurrentWaveFully(Test, *Room, 1))
			{
				return false;
			}
			if (!Test.TestEqual(TEXT("all five enemies were birthed for the run (2+3)"), Session->GetSpawnedEnemyCount(), 5))
			{
				return false;
			}
			if (!KillNextEnemyAt(Test, LocW1A, TEXT("the first wave-1 enemy was found at its location"))
				|| !KillNextEnemyAt(Test, LocW1B, TEXT("the second wave-1 enemy was found at its location"))
				|| !KillNextEnemyAt(Test, LocW1C, TEXT("the third wave-1 enemy was found at its location")))
			{
				return false;
			}
			return Test.TestTrue(TEXT("the last death cleared the run"),
				Session->GetState() == ERoomSessionState::Cleared);
		}
	};
}

using namespace UE::UEMMO::Tasks::M3_020;

// 1. The full progression loop: first session (enter -> two waves cleared ->
//    settled -> claimed -> equipped -> saved), the "new process" restore
//    through a second real game instance's flow Initialize, and the second
//    session's room entry where the same light_01 attack on the same
//    Defense-0 enemy deducts 15 instead of the 10 measured before the
//    equipment existed. The reward seed is probed so the drafted piece is the
//    Attack+5 weapon_training definition.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_020FullLoopRewardEquipSaveRestoreDamageReflectsEquipment,
	"UEMMO.Tasks.M3_020.FullLoopRewardEquipSaveRestoreDamageReflectsEquipment",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_020FullLoopRewardEquipSaveRestoreDamageReflectsEquipment::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;

	// Defensive: a leaked startup seam from any earlier suite must never
	// silently restore into the FIRST session's flow.
	UGameFlowSubsystem::SetStartupSaveServiceForTests(nullptr);

	FM3_020_MemoryStorage Storage;
	const FItemDefinitionCatalog Catalog = M3_020_MakeTrainingCatalog();

	// -- First session: fresh profile, baseline damage, full loop --------------
	FM3_020_WorldScene SceneA;
	if (!SceneA.Build(*this, TEXT("RunA"), M3_020_SceneBaseA, TEXT("room_m3_020_loop_a"), 0,
		[](UProfileSubsystem& Profile)
		{
			// The caller's explicit NewProfile (the M3-015 rule: a fresh
			// state never mints a profile by itself).
			Profile.NewProfile();
		}))
	{
		SceneA.TearDown();
		return true;
	}
	if (!SceneA.AttachPersistence(*this, Storage))
	{
		SceneA.TearDown();
		return true;
	}
	const FGuid CharacterIdA = SceneA.Profile->GetCharacterId();
	TestTrue(TEXT("the first session mints a valid character identity"), CharacterIdA.IsValid());
	TestEqual(TEXT("the new profile starts at level 1"), SceneA.Profile->GetLevel(), 1);
	TestEqual(TEXT("the new profile starts at 0 XP"), SceneA.Profile->GetXP(), 0);

	// The pre-equipment damage baseline: the same light_01 attack on the same
	// Defense-0 enemy deducts exactly the base damage 10 (Attack 0).
	float Damage0 = -1.0f;
	float HpBefore0 = -1.0f;
	float HpAfter0 = -1.0f;
	if (!SceneA.PressLightAndWaitForHit(*this, TEXT("pre-equip"), Damage0, HpBefore0, HpAfter0))
	{
		SceneA.TearDown();
		return true;
	}
	TestEqual(TEXT("the pre-equipment light_01 hit deducts exactly the base damage 10"),
		Damage0, M3_020_BaseDamageLight01, 0.01f);
	TestEqual(TEXT("the pre-equipment pool dropped from 100 to 90"), HpAfter0, 90.0f, 0.01f);
	SceneA.Log.Add(SceneA.ClockSeconds, TEXT("DamageBaseline"),
		FString::Printf(TEXT("pre-equip damage %.1f (hp %.1f -> %.1f)"), Damage0, HpBefore0, HpAfter0));

	// Enter the room through the flow state machine (the map select -> room
	// chain), then run the wave progression through the session.
	if (!TestTrue(TEXT("EnterRoom accepts the first session's menu entry"), SceneA.Flow->EnterRoom(SceneA.Room)))
	{
		SceneA.TearDown();
		return true;
	}
	TestTrue(TEXT("the flow reached the room state"), SceneA.Flow->GetState() == EGameFlowState::Room);
	TestEqual(TEXT("the map opener ran exactly once"), SceneA.MapOpenCount, 1);
	TestTrue(TEXT("the opener received the selectable map path"),
		SceneA.LastOpenedMapPath == FString(TEXT("/Game/UEMMO/Maps/L_TrainingArena")));

	// Probe a run whose seed drafts the weapon, then clear it for real. Every
	// probe's StartRoom fires its own OnRunStarted (the probe runs are legal
	// runs that are recycled), so the cleared-run assertions compare against
	// the probe baseline instead of a hard-coded 1.
	if (!SceneA.StartWeaponRewardRun(*this, Catalog))
	{
		SceneA.TearDown();
		return true;
	}
	const int32 StartsBeforeClearedRun = SceneA.StartedCount;
	const int32 EndsBeforeClearedRun = SceneA.EndedCount;
	const uint64 RunIdA = SceneA.Session->GetRunId();
	if (!TestTrue(TEXT("BeginWaves starts the progression from wave 0"), SceneA.Session->BeginWaves(SceneA.Room, M3_020_MakeEnemyDef())))
	{
		SceneA.TearDown();
		return true;
	}
	if (!SceneA.ClearBothWaves(*this))
	{
		SceneA.TearDown();
		return true;
	}
	TestEqual(TEXT("no additional start event fired for the cleared run beyond its own StartRoom"),
		SceneA.StartedCount, StartsBeforeClearedRun);
	TestEqual(TEXT("exactly one end event fired for the cleared run"), SceneA.EndedCount, EndsBeforeClearedRun + 1);
	TestTrue(TEXT("zero alive enemies remain after the clear"), SceneA.AliveEnemies() == 0);

	FRoomResult ResultA;
	if (!TestTrue(TEXT("the cleared run's result is queryable from the archive"), SceneA.Session->GetRunResult(RunIdA, ResultA)))
	{
		SceneA.TearDown();
		return true;
	}
	TestTrue(TEXT("the archived result records Cleared"), ResultA.bCleared);
	TestTrue(TEXT("the cleared run is reward eligible"), SceneA.Session->IsRewardEligible(RunIdA));
	TestEqual(TEXT("the archived result counts exactly the five real deaths"), ResultA.KilledCount, 5);

	// The legal flow cycle out of the finished room: Room -> Result -> Menu.
	TestTrue(TEXT("NotifyRunEnded moves the room to the result surface"), SceneA.Flow->NotifyRunEnded());
	TestTrue(TEXT("the flow shows the result state"), SceneA.Flow->GetState() == EGameFlowState::Result);
	TestTrue(TEXT("ReturnToMenu returns to the menu"), SceneA.Flow->ReturnToMenu());
	TestTrue(TEXT("the flow is back in the menu state"), SceneA.Flow->GetState() == EGameFlowState::Menu);
	SceneA.Log.Add(SceneA.ClockSeconds, TEXT("FlowMenu"), TEXT("the first session returned to the menu"));

	// The settlement: the archived Cleared result feeds the real BeginReward.
	const FRewardBeginOutcome Begin = SceneA.Reward->BeginReward(ResultA, Catalog);
	if (!TestEqual(TEXT("the settlement began with a pending draft"), Begin.Result, ERewardBeginResult::Applied))
	{
		SceneA.TearDown();
		return true;
	}
	if (Begin.Draft.Items.Num() != 1)
	{
		Test.AddError(FString::Printf(TEXT("the draft carries %d items instead of exactly one"), Begin.Draft.Items.Num()));
		SceneA.TearDown();
		return true;
	}
	const FItemInstance RewardItem = Begin.Draft.Items[0];
	TestEqual(TEXT("the drafted reward piece is the weapon_training kind the probe selected"),
		RewardItem.DefinitionId, FName(TEXT("weapon_training")));
	const int32 XPBeforeClaim = SceneA.Profile->GetXP();
	SceneA.Log.Add(SceneA.ClockSeconds, TEXT("RewardBegan"),
		FString::Printf(TEXT("settlement %llu xp %d item %s (%s)"),
			Begin.Draft.SettlementId, Begin.Draft.XP, *RewardItem.InstanceId.ToString(), *RewardItem.DefinitionId.ToString()));

	// The atomic claim: the XP and the ORIGINAL pre-generated instance commit
	// as one snapshot save.
	const FRewardClaimAtomicOutcome Claim = SceneA.Reward->ClaimPendingAtomic(
		ResultA.SettlementId, SceneA.Profile, SceneA.SaveService);
	TestEqual(TEXT("the atomic claim committed"), Claim.Result, ERewardClaimAtomicResult::Claimed);
	TestTrue(TEXT("the claim granted the settlement XP"), Claim.bGrantedXP);
	TestTrue(TEXT("the claim's snapshot save committed"), Claim.bSaveCommitted);
	TestEqual(TEXT("exactly one equipment piece was stored"), Claim.ClaimedItemCount, 1);
	TestEqual(TEXT("the XP grew by exactly the design 50"),
		SceneA.Profile->GetXP() - XPBeforeClaim, URewardService::RewardXPPerClear);
	TestEqual(TEXT("the level stayed 1 (a double grant would have leveled)"), SceneA.Profile->GetLevel(), 1);
	TestTrue(TEXT("the stored piece is the ORIGINAL instance (InstanceId unchanged)"),
		M3_020_FindStored(SceneA.Profile->GetInventory(), RewardItem.InstanceId) != nullptr);
	TestEqual(TEXT("no pending draft is left"), SceneA.Profile->GetPendingRewards().Num(), 0);
	TestTrue(TEXT("the settlement is recorded as applied"), SceneA.Profile->IsSettlementApplied(ResultA.SettlementId));
	SceneA.Log.Add(SceneA.ClockSeconds, TEXT("RewardClaimed"),
		FString::Printf(TEXT("xp %d -> %d, item %s"), XPBeforeClaim, SceneA.Profile->GetXP(), *RewardItem.InstanceId.ToString()));

	// Equip through the real entries: the FEquipmentModel slot mapping (the
	// catalog confirms the slot match), the FStatCalculator row derivation
	// and the pawn's TryEquipStatBonus wiring (refused while a run is Running;
	// the run is Cleared and the flow is back in the menu here).
	FEquipmentModel EquipmentA;
	EquipmentA.SetDefinitionCatalog(&Catalog);
	TestEqual(TEXT("the equip mapping accepted the weapon instance into the weapon slot"),
		EquipmentA.Equip(EItemSlot::Weapon, RewardItem.InstanceId, SceneA.Profile->GetInventory()),
		EEquipmentEquipResult::Equipped);
	const FItemStats EquipRowA = M3_020_ComputeEquippedRow(SceneA.Profile->GetInventory(), RewardItem.InstanceId);
	TestTrue(TEXT("the derived equipment row equals the weapon's rolled stats (Attack 5, no phantom MaxHP)"),
		FMath::IsNearlyEqual(EquipRowA.Attack, M3_020_WeaponAttackGrant, 0.01f)
		&& FMath::IsNearlyEqual(EquipRowA.Defense, RewardItem.RolledStats.Defense, 0.01f)
		&& FMath::IsNearlyEqual(EquipRowA.MaxHP, RewardItem.RolledStats.MaxHP, 0.01f));
	if (!TestTrue(TEXT("the pawn equip entry accepted the derived row outside a running room"),
		SceneA.Player->TryEquipStatBonus(EquipRowA)))
	{
		SceneA.TearDown();
		return true;
	}
	TestTrue(TEXT("the profile stored the equipped row"), SceneA.Profile->GetEquippedStatBonus().Attack == EquipRowA.Attack);
	TestEqual(TEXT("the equip wired Attack 5 into the combat component"),
		SceneA.PlayerCombat->GetAttackPower(), M3_020_WeaponAttackGrant, 0.01f);
	SceneA.Log.Add(SceneA.ClockSeconds, TEXT("Equipped"),
		FString::Printf(TEXT("attack %.1f defense %.1f maxhp %.1f"),
			EquipRowA.Attack, EquipRowA.Defense, EquipRowA.MaxHP));

	// The post-equipment damage: the SAME attack on the SAME enemy deducts 15
	// (the pool continues 90 -> 75; the deduction is +5).
	float Damage1 = -1.0f;
	float HpBefore1 = -1.0f;
	float HpAfter1 = -1.0f;
	if (!SceneA.PressLightAndWaitForHit(*this, TEXT("post-equip"), Damage1, HpBefore1, HpAfter1))
	{
		SceneA.TearDown();
		return true;
	}
	TestEqual(TEXT("the post-equipment light_01 hit deducts exactly 15"),
		Damage1, M3_020_DamageAttack5, 0.01f);
	TestEqual(TEXT("the post-equipment pool dropped from 90 to 75"), HpAfter1, 75.0f, 0.01f);
	TestTrue(TEXT("the equip raised the same attack's deduction by exactly the weapon grant"),
		FMath::IsNearlyEqual(Damage1 - Damage0, M3_020_WeaponAttackGrant, 0.01f));

	// Save the equipped profile: the request mirrors the live state plus the
	// equipment binding; the applied-id set travels from the last committed
	// save (the same rule the atomic claim uses).
	const FProfileLoadOutcome LastCommitted = SceneA.SaveService->LoadActiveProfile();
	TestEqual(TEXT("the claim's committed save loads back"),
		static_cast<int32>(LastCommitted.Result), static_cast<int32>(EProfileLoadResult::Success));
	FProfileSaveRequest SaveRequest;
	SaveRequest.Snapshot = SceneA.Profile->GetProfileSnapshot();
	SaveRequest.Inventory = SceneA.Profile->GetInventory();
	SaveRequest.PendingRewards = SceneA.Profile->GetPendingRewards();
	SaveRequest.AppliedSettlementIds = LastCommitted.AppliedSettlementIds;
	SaveRequest.EquippedMap.Add(EItemSlot::Weapon, RewardItem.InstanceId);
	const FProfileSaveOutcome Save = SceneA.SaveService->SaveProfile(SaveRequest);
	TestEqual(TEXT("the equipped profile save committed"),
		static_cast<int32>(Save.Result), static_cast<int32>(EProfileSaveResult::Success));
	SceneA.Log.Add(SceneA.ClockSeconds, TEXT("ProfileSaved"),
		FString::Printf(TEXT("generation %d xp %d attack %.0f"),
			Save.Generation, SaveRequest.Snapshot.XP, static_cast<double>(SaveRequest.Snapshot.Attack)));

	// The committed read-back: the equipped snapshot is durable with the
	// binding and the applied record.
	const FProfileLoadOutcome Committed = SceneA.SaveService->LoadActiveProfile();
	TestEqual(TEXT("the equipped save loads back"),
		static_cast<int32>(Committed.Result), static_cast<int32>(EProfileLoadResult::Success));
	if (Committed.Result == EProfileLoadResult::Success)
	{
		TestTrue(TEXT("the committed save keeps the equipped weapon binding"),
			Committed.EquippedMap.Contains(EItemSlot::Weapon) && Committed.EquippedMap[EItemSlot::Weapon] == RewardItem.InstanceId);
		TestTrue(TEXT("the committed save keeps the applied record"),
			Committed.AppliedSettlementIds.Contains(ResultA.SettlementId));
		// The serializer never stores derived stats: the loaded snapshot
		// carries the recomputed LEVEL BASE row (the M3-015 precedent) - the
		// equipment bonus is re-derived by the gameplay layer from the
		// bindings, never fossilized into the save.
		TestEqual(TEXT("the loaded snapshot's attack is the recomputed level base row"),
			Committed.Snapshot.Attack, UProfileSubsystem::GetAttackForLevel(Committed.Snapshot.Level));
		TestEqual(TEXT("the loaded snapshot's max HP is the recomputed level base row"),
			Committed.Snapshot.MaxHP, UProfileSubsystem::GetMaxHPForLevel(Committed.Snapshot.Level));
	}
	const int32 SaveGeneration = Save.Generation;

	// -- "New process": a new save service + the second real game instance ----
	//    The flow's Initialize runs the production startup chain (the M3-017
	//    injected-service seam) and restores the profile before any actor
	//    exists - the closest automation model of a restart.
	UProfileSaveService* ReloadService = M3_020_NewService(Test, &Storage);
	if (!ReloadService)
	{
		SceneA.TearDown();
		return true;
	}
	ReloadService->AddToRoot();
	UGameFlowSubsystem::SetStartupSaveServiceForTests(ReloadService);

	FM3_020_WorldScene SceneB;
	if (!SceneB.Build(*this, TEXT("RunB"), M3_020_SceneBaseB, TEXT("room_m3_020_loop_b"), 1,
		TFunction<void(UProfileSubsystem&)>()))
	{
		SceneB.TearDown();
		ReloadService->RemoveFromRoot();
		SceneA.TearDown();
		return true;
	}

	// The restarted state: identity, progress, item instance id and the
	// equipment slot match the saved ones field for field (the startup
	// outcome is the loaded save; the live profile is its RestoreFromSave).
	TestTrue(TEXT("the restarted flow restored the saved profile"), SceneB.Flow->WasStartupProfileRestored());
	const FStartupLoadOutcome& Loaded = SceneB.Flow->GetStartupLoadOutcome();
	TestEqual(TEXT("the startup outcome is the direct recovery"),
		static_cast<int32>(Loaded.Result), static_cast<int32>(EStartupLoadResult::Recovered));
	TestEqual(TEXT("the restart loaded the committed generation"), Loaded.Generation, SaveGeneration);
	TestTrue(TEXT("the restored CharacterId is the first session's identity"),
		Loaded.Snapshot.CharacterId == CharacterIdA);
	TestEqual(TEXT("the restored level matches the save"), Loaded.Snapshot.Level, 1);
	TestEqual(TEXT("the restored XP matches the save"), Loaded.Snapshot.XP, XPBeforeClaim + URewardService::RewardXPPerClear);
	TestTrue(TEXT("the restored inventory keeps the ORIGINAL item instance id"),
		Loaded.Inventory.Contains(RewardItem.InstanceId));
	TestTrue(TEXT("the restored equipment slot keeps the weapon binding"),
		Loaded.EquippedMap.Contains(EItemSlot::Weapon) && Loaded.EquippedMap[EItemSlot::Weapon] == RewardItem.InstanceId);
	TestTrue(TEXT("the live restored profile has a profile"), SceneB.Profile->HasProfile());
	TestTrue(TEXT("the live restored profile keeps the CharacterId"),
		SceneB.Profile->GetCharacterId() == CharacterIdA);
	TestEqual(TEXT("the live restored profile keeps the level"), SceneB.Profile->GetLevel(), 1);
	TestEqual(TEXT("the live restored profile keeps the XP"),
		SceneB.Profile->GetXP(), XPBeforeClaim + URewardService::RewardXPPerClear);
	TestTrue(TEXT("the live restored inventory keeps the ORIGINAL item instance id"),
		SceneB.Profile->GetInventory().Contains(RewardItem.InstanceId));
	TestTrue(TEXT("the restored settlement is still recorded as applied"),
		SceneB.Profile->IsSettlementApplied(ResultA.SettlementId));
	SceneB.Log.Add(SceneB.ClockSeconds, TEXT("RestartRestored"),
		FString::Printf(TEXT("generation %d character %s"), Loaded.Generation, *CharacterIdA.ToString()));

	// The documented restore semantics: the equipped bonus ROW resets to zero
	// (never fossilized) while the binding survives - the gameplay layer
	// re-derives the row from the restored bindings before combat.
	const FItemStats& RestoredRow = SceneB.Profile->GetEquippedStatBonus();
	TestTrue(TEXT("the restored bonus row is the zero row (the gameplay layer re-derives it)"),
		RestoredRow.Attack == 0.0f && RestoredRow.Defense == 0.0f && RestoredRow.MaxHP == 0.0f);
	TestEqual(TEXT("the respawned pawn wired the restored snapshot's base attack (0)"),
		SceneB.PlayerCombat->GetAttackPower(), 0.0f, 0.01f);

	// The gameplay-layer re-derivation: the same real equip entries on the
	// restored inventory and binding (no hand-written final row).
	FEquipmentModel EquipmentB;
	EquipmentB.SetDefinitionCatalog(&Catalog);
	TestEqual(TEXT("the re-equipped mapping accepted the restored weapon instance"),
		EquipmentB.Equip(EItemSlot::Weapon, RewardItem.InstanceId, SceneB.Profile->GetInventory()),
		EEquipmentEquipResult::Equipped);
	const FItemStats EquipRowB = M3_020_ComputeEquippedRow(SceneB.Profile->GetInventory(), RewardItem.InstanceId);
	if (!TestTrue(TEXT("the pawn equip entry accepted the re-derived row"),
		SceneB.Player->TryEquipStatBonus(EquipRowB)))
	{
		SceneB.TearDown();
		ReloadService->RemoveFromRoot();
		SceneA.TearDown();
		return true;
	}
	TestEqual(TEXT("the re-derived equip wired Attack 5 into the combat component"),
		SceneB.PlayerCombat->GetAttackPower(), M3_020_WeaponAttackGrant, 0.01f);
	SceneB.Log.Add(SceneB.ClockSeconds, TEXT("BonusReDerived"),
		FString::Printf(TEXT("attack %.1f"), EquipRowB.Attack));

	// The second room entry through the flow: the accepted StartRoom re-loads
	// the snapshot and restores the pool to the current max (the M3-010
	// enter-the-dungeon load point) - then the damage reflects the equipment
	// through the wired combat attributes, not through any UI surface.
	if (!TestTrue(TEXT("EnterRoom accepts the second session's menu entry"), SceneB.Flow->EnterRoom(SceneB.Room)))
	{
		SceneB.TearDown();
		ReloadService->RemoveFromRoot();
		SceneA.TearDown();
		return true;
	}
	TestTrue(TEXT("the restarted flow reached the room state"), SceneB.Flow->GetState() == EGameFlowState::Room);
	if (!TestTrue(TEXT("the second session's room run starts"), SceneB.Session->StartRoom(SceneB.Room)))
	{
		SceneB.TearDown();
		ReloadService->RemoveFromRoot();
		SceneA.TearDown();
		return true;
	}
	TestEqual(TEXT("the run start restored the pool to the current max 100"),
		SceneB.Player->GetHealth()->GetHealth(), 100.0f, 0.01f);

	float Damage2 = -1.0f;
	float HpBefore2 = -1.0f;
	float HpAfter2 = -1.0f;
	if (!SceneB.PressLightAndWaitForHit(*this, TEXT("after-restart"), Damage2, HpBefore2, HpAfter2))
	{
		SceneB.TearDown();
		ReloadService->RemoveFromRoot();
		SceneA.TearDown();
		return true;
	}
	TestEqual(TEXT("the restarted session's light_01 hit deducts exactly 15"),
		Damage2, M3_020_DamageAttack5, 0.01f);
	TestEqual(TEXT("the restarted session's pool dropped from 100 to 85"), HpAfter2, 85.0f, 0.01f);
	TestTrue(TEXT("the damage across the restart reflects the equipment (+5 vs the pre-equip baseline)"),
		FMath::IsNearlyEqual(Damage2 - Damage0, M3_020_WeaponAttackGrant, 0.01f));
	TestTrue(TEXT("the wired combat attribute is the equipped attack (not a UI display)"),
		FMath::IsNearlyEqual(SceneB.PlayerCombat->GetAttackPower(), M3_020_WeaponAttackGrant, 0.01f));
	TestTrue(TEXT("ReturnToMenu closes the second session"), SceneB.Flow->ReturnToMenu());
	TestTrue(TEXT("the restarted flow is back in the menu state"), SceneB.Flow->GetState() == EGameFlowState::Menu);

	// -- Evidence: the combined progression JSON -------------------------------
	FString Extra;
	Extra += FString::Printf(TEXT("\"character_id_run_a\": \"%s\",\n  \"character_id_run_b\": \"%s\",\n"),
		*CharacterIdA.ToString(), *SceneB.Profile->GetCharacterId().ToString());
	Extra += FString::Printf(TEXT("\"character_id_identical\": %s,\n"),
		(SceneB.Profile->GetCharacterId() == CharacterIdA) ? TEXT("true") : TEXT("false"));
	Extra += FString::Printf(TEXT("\"item_definition\": \"%s\",\n  \"item_instance_id\": \"%s\",\n"),
		*M3_020_JsonEscape(RewardItem.DefinitionId.ToString()), *RewardItem.InstanceId.ToString());
	Extra += FString::Printf(TEXT("\"xp_before_claim\": %d,\n  \"xp_after_claim\": %d,\n"), XPBeforeClaim, SceneA.Profile->GetXP());
	Extra += FString::Printf(TEXT("\"damage_before_equip\": %.1f,\n  \"damage_after_equip\": %.1f,\n  \"damage_after_restart\": %.1f,\n"),
		Damage0, Damage1, Damage2);
	Extra += FString::Printf(TEXT("\"damage_delta_equip\": %.1f,\n  \"save_generation\": %d,\n  \"restore_generation\": %d"),
		Damage1 - Damage0, SaveGeneration, Loaded.Generation);
	if (!M3_020_WriteProgressionJson(*this, TEXT("m3-020-progression.json"), SceneA.Log, SceneB.Log, Extra))
	{
		// The evidence write failure is already reported; the behavioral
		// assertions above carried the verdict.
	}

	AddInfo(TEXT("First session event order:\n") + SceneA.Log.Describe());
	AddInfo(TEXT("Second session event order:\n") + SceneB.Log.Describe());

	SceneB.TearDown();
	ReloadService->RemoveFromRoot();
	SceneA.TearDown();
	M3_020_CleanupTestSlots(*this, *ReloadService, Storage);
	return true;
}

// 2. Interrupted claim: the claim-snapshot slot write is injected to fail
//    (write ordinal 3 - after the draft persistence, before any claim data
//    reached storage). The call reports SaveFailed; the restart (a NEW save
//    service + StartupLoad + RestoreFromSave into a FRESH profile) recovers
//    the persisted draft state, and the re-claim grants the XP exactly the
//    design 50 once and stores exactly the ORIGINAL instance.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_020InterruptedClaimWriteGrantsXPExactlyOnceAfterRestart,
	"UEMMO.Tasks.M3_020.InterruptedClaimWriteGrantsXPExactlyOnceAfterRestart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_020InterruptedClaimWriteGrantsXPExactlyOnceAfterRestart::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_020_MemoryStorage Storage;
	FM3_020_ProfileFixture Fixture;
	if (!Fixture.Setup(Test))
	{
		return true;
	}
	UProfileSaveService* Service = M3_020_NewService(Test, &Storage);
	if (!Service)
	{
		return true;
	}

	const FRoomResult Result = M3_020_MakeResult(/*RunId*/ 5201, /*SettlementId*/ 5201, /*Seed*/ 52011, true);
	const FRewardBeginOutcome Begin = Fixture.Reward->BeginReward(Result, M3_020_MakeTrainingCatalog());
	Test.TestEqual(TEXT("setup: the settlement began with a pending draft"), Begin.Result, ERewardBeginResult::Applied);
	if (Begin.Result != ERewardBeginResult::Applied || Begin.Draft.Items.Num() != 1)
	{
		M3_020_CleanupTestSlots(Test, *Service, Storage);
		return true;
	}
	const FItemInstance Original = Begin.Draft.Items[0];
	const int32 XPBefore = Fixture.Profile->GetXP();

	// The interrupt: writes 1-2 are the draft save (slot + index), write 3 is
	// the claim snapshot's slot write and never completes.
	FM3_020_CrashStorage Crash(&Storage);
	Crash.FailWriteOrdinal = 3;
	Service->SetStorage(&Crash);
	const FRewardClaimAtomicOutcome Interrupted = Fixture.Reward->ClaimPendingAtomic(5201, Fixture.Profile, Service);
	Test.TestEqual(TEXT("the interrupted claim does not confirm success"),
		Interrupted.Result, ERewardClaimAtomicResult::SaveFailed);
	Test.TestFalse(TEXT("the interrupted claim committed no claim save"), Interrupted.bSaveCommitted);

	// RESTART: the claim idempotency is decided from the persisted state alone.
	FM3_020_RestartedState Restarted;
	if (!Restarted.Setup(Test, Storage, TEXT("interrupted-claim restart")))
	{
		M3_020_CleanupTestSlots(Test, *Service, Storage);
		return true;
	}
	Test.TestEqual(TEXT("the restart recovered the index-active draft save"),
		static_cast<int32>(Restarted.LoadResult), static_cast<int32>(EStartupLoadResult::Recovered));
	Test.TestEqual(TEXT("the restored XP is the pre-claim XP"), Restarted.Profile->GetXP(), XPBefore);
	Test.TestFalse(TEXT("the settlement is not recorded as applied yet"), Restarted.Profile->IsSettlementApplied(5201));
	Test.TestEqual(TEXT("the restored pending draft survived"), Restarted.Profile->GetPendingRewards().Num(), 1);
	if (Restarted.Profile->GetPendingRewards().Num() == 1 && Restarted.Profile->GetPendingRewards()[0].Items.Num() == 1)
	{
		Test.TestTrue(TEXT("the restored draft item is the ORIGINAL pre-generated instance"),
			M3_020_InstancesEqual(Restarted.Profile->GetPendingRewards()[0].Items[0], Original));
	}

	// The re-claim from the persisted state: exactly one grant, exactly the
	// original item, no duplicate.
	const FRewardClaimAtomicOutcome Reclaim = Restarted.Reward->ClaimPendingAtomic(5201, Restarted.Profile, Restarted.SaveService);
	Test.TestEqual(TEXT("the re-claim after the restart succeeds"), Reclaim.Result, ERewardClaimAtomicResult::Claimed);
	Test.TestTrue(TEXT("the re-claim grants the XP"), Reclaim.bGrantedXP);
	Test.TestEqual(TEXT("the total XP across the interruption is exactly the design 50"),
		Restarted.Profile->GetXP() - XPBefore, URewardService::RewardXPPerClear);
	Test.TestEqual(TEXT("the level stayed 1 (a double grant would have leveled)"), Restarted.Profile->GetLevel(), 1);
	Test.TestEqual(TEXT("exactly one equipment piece was stored"), Restarted.Profile->GetInventory().Count(), 1);
	Test.TestTrue(TEXT("the stored piece is the ORIGINAL instance (InstanceId unchanged)"),
		M3_020_FindStored(Restarted.Profile->GetInventory(), Original.InstanceId) != nullptr);
	Test.TestEqual(TEXT("no pending draft is left"), Restarted.Profile->GetPendingRewards().Num(), 0);
	Test.TestTrue(TEXT("the settlement is recorded as applied"), Restarted.Profile->IsSettlementApplied(5201));

	M3_020_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// 3. Full inventory: the atomic claim into a full bag retains the pending
//    item (InventoryFull - never a claim success shape), grants the XP once
//    and commits the retention state; the retained draft survives a restart
//    field for field (same InstanceId, same one-time roll) and freeing one
//    slot lets the re-claim store exactly the ORIGINAL instance with no
//    second XP.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_020FullInventoryClaimRetainsPendingAcrossRestart,
	"UEMMO.Tasks.M3_020.FullInventoryClaimRetainsPendingAcrossRestart",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_020FullInventoryClaimRetainsPendingAcrossRestart::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	FM3_020_MemoryStorage Storage;
	FM3_020_ProfileFixture Fixture;
	if (!Fixture.Setup(Test))
	{
		return true;
	}
	UProfileSaveService* Service = M3_020_NewService(Test, &Storage);
	if (!Service)
	{
		return true;
	}

	// Fill the inventory to the full 30-slot capacity, then settle one run.
	const FItemDefinition FillerDef = M3_020_MakeFillerDefinition();
	const TArray<FGuid> Fillers = M3_020_FillInventory(Fixture.Profile->GetInventory(), FillerDef, FInventoryModel::Capacity);
	Test.TestEqual(TEXT("setup: the inventory is full"), Fixture.Profile->GetInventory().Count(), FInventoryModel::Capacity);

	const FRoomResult Result = M3_020_MakeResult(/*RunId*/ 5202, /*SettlementId*/ 5202, /*Seed*/ 52021, true);
	const FRewardBeginOutcome Begin = Fixture.Reward->BeginReward(Result, M3_020_MakeTrainingCatalog());
	Test.TestEqual(TEXT("setup: the settlement began with a pending draft"), Begin.Result, ERewardBeginResult::Applied);
	if (Begin.Result != ERewardBeginResult::Applied || Begin.Draft.Items.Num() != 1)
	{
		M3_020_CleanupTestSlots(Test, *Service, Storage);
		return true;
	}
	const FItemInstance Original = Begin.Draft.Items[0];
	const int32 XPBefore = Fixture.Profile->GetXP();

	// The full-bag claim: nothing is stored, the item is retained, and the
	// retention state (XP + applied id + retained draft) commits as one
	// snapshot.
	const FRewardClaimAtomicOutcome Claim = Fixture.Reward->ClaimPendingAtomic(5202, Fixture.Profile, Service);
	Test.TestEqual(TEXT("the full-bag claim reports InventoryFull (never a claim success)"),
		Claim.Result, ERewardClaimAtomicResult::InventoryFull);
	Test.TestEqual(TEXT("the full-bag claim stored nothing"), Claim.ClaimedItemCount, 0);
	Test.TestEqual(TEXT("the full-bag claim retains the pending item"), Claim.RetainedItemCount, 1);
	Test.TestTrue(TEXT("the first attempt granted the settlement XP"), Claim.bGrantedXP);
	Test.TestTrue(TEXT("the retention state is committed"), Claim.bSaveCommitted);
	Test.TestEqual(TEXT("the in-memory XP grew by the design 50"),
		Fixture.Profile->GetXP() - XPBefore, URewardService::RewardXPPerClear);
	Test.TestTrue(TEXT("the settlement is recorded as applied"), Fixture.Profile->IsSettlementApplied(5202));
	Test.TestEqual(TEXT("the draft stays pending"), Fixture.Profile->GetPendingRewards().Num(), 1);

	// RESTART: the retained draft (with the ORIGINAL item) must survive.
	FM3_020_RestartedState Restarted;
	if (!Restarted.Setup(Test, Storage, TEXT("full-bag restart")))
	{
		M3_020_CleanupTestSlots(Test, *Service, Storage);
		return true;
	}
	Test.TestEqual(TEXT("the restart recovered the committed retention state"),
		static_cast<int32>(Restarted.LoadResult), static_cast<int32>(EStartupLoadResult::Recovered));
	Test.TestEqual(TEXT("the restored pending draft survived the restart"), Restarted.Profile->GetPendingRewards().Num(), 1);
	if (Restarted.Profile->GetPendingRewards().Num() == 1 && Restarted.Profile->GetPendingRewards()[0].Items.Num() == 1)
	{
		Test.TestTrue(TEXT("the retained item survived field-for-field (same InstanceId, same one-time roll)"),
			M3_020_InstancesEqual(Restarted.Profile->GetPendingRewards()[0].Items[0], Original));
	}
	Test.TestEqual(TEXT("the restored XP is the once-granted total"),
		Restarted.Profile->GetXP(), XPBefore + URewardService::RewardXPPerClear);
	Test.TestTrue(TEXT("the restored applied record survives"), Restarted.Profile->IsSettlementApplied(5202));
	Test.TestEqual(TEXT("the restored inventory is still full"), Restarted.Profile->GetInventory().Count(), FInventoryModel::Capacity);

	// Free one slot and re-claim: ONLY the original instance enters, no XP
	// again (the total across both attempts stays the design 50).
	const EInventoryRemoveResult Removed = Restarted.Profile->GetInventory().Remove(Fillers[0]);
	Test.TestEqual(TEXT("one freed slot makes room"), Removed, EInventoryRemoveResult::Removed);
	const FRewardClaimAtomicOutcome Reclaim = Restarted.Reward->ClaimPendingAtomic(5202, Restarted.Profile, Restarted.SaveService);
	Test.TestEqual(TEXT("the re-claim after freeing a slot claims the draft"), Reclaim.Result, ERewardClaimAtomicResult::Claimed);
	Test.TestEqual(TEXT("the re-claim stored exactly one item"), Reclaim.ClaimedItemCount, 1);
	Test.TestFalse(TEXT("the re-claim grants no XP again"), Reclaim.bGrantedXP);
	Test.TestEqual(TEXT("the total XP across both attempts is exactly the design 50"),
		Restarted.Profile->GetXP() - XPBefore, URewardService::RewardXPPerClear);
	Test.TestEqual(TEXT("the inventory is full again, holding the claimed instance"),
		Restarted.Profile->GetInventory().Count(), FInventoryModel::Capacity);
	Test.TestTrue(TEXT("the stored item is the ORIGINAL instance (same InstanceId)"),
		M3_020_FindStored(Restarted.Profile->GetInventory(), Original.InstanceId) != nullptr);
	Test.TestEqual(TEXT("the consumed draft is gone"), Restarted.Profile->GetPendingRewards().Num(), 0);

	M3_020_CleanupTestSlots(Test, *Service, Storage);
	return true;
}

// 4. Three consecutive loop runs on one world never accumulate anything:
//    every run enters through the flow (Menu -> Room), spawns exactly 5,
//    kills exactly 5, clears exactly once, adds exactly 5 enemy actors and
//    ends with zero alive enemies; the inter-wave wait is re-armed per run
//    (no stale timer) and the flow lands back in the menu after every run
//    with no delegate stacking.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_020ThreeConsecutiveLoopRunsLeaveNoResidue,
	"UEMMO.Tasks.M3_020.ThreeConsecutiveLoopRunsLeaveNoResidue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_020ThreeConsecutiveLoopRunsLeaveNoResidue::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;

	// Defensive: a leaked startup seam from any earlier suite must never
	// silently restore into this suite's fresh flow.
	UGameFlowSubsystem::SetStartupSaveServiceForTests(nullptr);

	FM3_020_WorldScene Scene;
	if (!Scene.Build(*this, TEXT("LoopRuns"), M3_020_SceneBaseA, TEXT("room_m3_020_loop_runs"), 0,
		TFunction<void(UProfileSubsystem&)>()))
	{
		Scene.TearDown();
		return true;
	}
	UEnemyDefinition* Def = M3_020_MakeEnemyDef();
	const FVector& LocW0A = Scene.Room->Waves[0].SpawnLocations[0];
	const FVector& LocW0B = Scene.Room->Waves[0].SpawnLocations[1];
	const FVector& LocW1A = Scene.Room->Waves[1].SpawnLocations[0];
	const FVector& LocW1B = Scene.Room->Waves[1].SpawnLocations[1];
	const FVector& LocW1C = Scene.Room->Waves[1].SpawnLocations[2];

	uint64 PreviousRunId = 0;
	for (int32 RunIndex = 1; RunIndex <= 3; ++RunIndex)
	{
		const FString Context = FString::Printf(TEXT("run %d"), RunIndex);
		const int32 EnemiesBefore = Scene.TotalEnemies();
		const int32 ActorsBefore = Scene.TotalActors();

		if (!Test.TestTrue(FString::Printf(TEXT("%s enters through the flow"), *Context),
			Scene.Flow->EnterRoom(Scene.Room)))
		{
			break;
		}
		if (!Test.TestTrue(FString::Printf(TEXT("%s: the flow reached the room state"), *Context),
			Scene.Flow->GetState() == EGameFlowState::Room))
		{
			break;
		}
		if (!Test.TestTrue(FString::Printf(TEXT("%s starts"), *Context), Scene.Session->StartRoom(Scene.Room)))
		{
			break;
		}
		const uint64 RunId = Scene.Session->GetRunId();
		Test.TestTrue(FString::Printf(TEXT("%s carries a strictly larger run id"), *Context), RunId > PreviousRunId);
		PreviousRunId = RunId;
		if (!Test.TestTrue(FString::Printf(TEXT("%s begins its waves"), *Context), Scene.Session->BeginWaves(Scene.Room, Def)))
		{
			break;
		}

		// Wave 0: both born, both killed.
		Scene.InjectClock(Scene.ClockSeconds);
		if (!Scene.SpawnCurrentWaveFully(*this, *Scene.Room, 0))
		{
			break;
		}
		Test.TestEqual(FString::Printf(TEXT("%s birthed exactly two wave-0 enemies"), *Context),
			Scene.Session->GetSpawnedEnemyCount(), 2);
		if (!Scene.KillNextEnemyAt(*this, LocW0A, TEXT("the first wave-0 enemy was found"))
			|| !Scene.KillNextEnemyAt(*this, LocW0B, TEXT("the second wave-0 enemy was found")))
		{
			break;
		}

		// Timer freshness: 0.5 s after the wave-0 deaths the next wave must
		// not have started (a stale armed wait from an earlier run would fire
		// on the very first injection after the kill).
		if (!Scene.AdvanceSeconds(*this, 0.5))
		{
			break;
		}
		Test.TestEqual(FString::Printf(TEXT("%s: 0.5 s into the wait the wave index is still 0 (no stale timer)"), *Context),
			Scene.Session->GetCurrentWaveIndex(), 0);

		// Wave 1: past the full gap, three born, three killed, one clear.
		if (!Scene.AdvanceSeconds(*this, M3_020_WaveGapSeconds + 0.05))
		{
			break;
		}
		if (!Scene.SpawnCurrentWaveFully(*this, *Scene.Room, 1))
		{
			break;
		}
		Test.TestEqual(FString::Printf(TEXT("%s birthed all five of its enemies (2+3)"), *Context),
			Scene.Session->GetSpawnedEnemyCount(), 5);
		if (!Scene.KillNextEnemyAt(*this, LocW1A, TEXT("the first wave-1 enemy was found"))
			|| !Scene.KillNextEnemyAt(*this, LocW1B, TEXT("the second wave-1 enemy was found"))
			|| !Scene.KillNextEnemyAt(*this, LocW1C, TEXT("the third wave-1 enemy was found")))
		{
			break;
		}

		// Per-run settlement and world bookkeeping: one clear, five kills,
		// exactly five new actors (the corpses), zero alive enemies.
		Test.TestTrue(FString::Printf(TEXT("%s cleared"), *Context), Scene.Session->GetState() == ERoomSessionState::Cleared);
		Test.TestEqual(FString::Printf(TEXT("%s counted exactly five kills"), *Context), Scene.Session->GetKilledCount(), 5);
		Test.TestEqual(FString::Printf(TEXT("%s fired exactly its own end event"), *Context), Scene.EndedCount, RunIndex);
		Test.TestEqual(FString::Printf(TEXT("%s fired exactly its own start event"), *Context), Scene.StartedCount, RunIndex);
		if (Scene.EndedResults.Num() == RunIndex)
		{
			Test.TestTrue(FString::Printf(TEXT("%s's end result is Cleared"), *Context),
				Scene.EndedResults[RunIndex - 1].bCleared);
			Test.TestEqual(FString::Printf(TEXT("%s's end result keeps its run id"), *Context),
				Scene.EndedResults[RunIndex - 1].RunId, RunId);
		}
		Test.TestTrue(FString::Printf(TEXT("%s is reward eligible"), *Context), Scene.Session->IsRewardEligible(RunId));
		Test.TestEqual(FString::Printf(TEXT("%s left zero alive enemies"), *Context), Scene.AliveEnemies(), 0);
		Test.TestEqual(FString::Printf(TEXT("%s added exactly five enemy actors to the world"), *Context),
			Scene.TotalEnemies() - EnemiesBefore, 5);
		Test.TestEqual(FString::Printf(TEXT("%s added exactly five actors in total"), *Context),
			Scene.TotalActors() - ActorsBefore, 5);

		// The between-runs flow: the result surface then the menu return
		// (which runs the session's LeaveRoom) before the next enter.
		if (!Test.TestTrue(FString::Printf(TEXT("%s moves to the result surface"), *Context), Scene.Flow->NotifyRunEnded()))
		{
			break;
		}
		if (!Test.TestTrue(FString::Printf(TEXT("%s returns to the menu"), *Context), Scene.Flow->ReturnToMenu()))
		{
			break;
		}
		if (!Test.TestTrue(FString::Printf(TEXT("%s: the flow is back in the menu state"), *Context),
			Scene.Flow->GetState() == EGameFlowState::Menu))
		{
			break;
		}
	}

	// No delegate stacking and no extra settlement across the whole loop.
	Test.TestEqual(TEXT("three runs produced exactly three start events"), Scene.StartedCount, 3);
	Test.TestEqual(TEXT("three runs produced exactly three end events"), Scene.EndedCount, 3);
	Test.TestEqual(TEXT("the world never saw a player death"), Scene.PlayerDiedCount, 0);
	AddInfo(TEXT("Three-run loop event order:\n") + Scene.Log.Describe());

	Scene.TearDown();
	return true;
}

#endif
