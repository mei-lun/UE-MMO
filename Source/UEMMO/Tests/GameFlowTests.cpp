// M3-017: the single-map select menu and the room enter/leave flow. The suite
// locks the card's behaviors:
//
//   1. The flow state machine accepts exactly the legal sequence
//      Menu -> Loading -> Room -> Result -> Menu and refuses every other
//      transition (including the re-entrant requests inside the Loading
//      window); refused requests record an error and append no history entry.
//   2. A fast double-click on enter produces exactly ONE world switch (the
//      bLoading guard; the injected map opener counts the switches).
//   3. Three enter/exit cycles in a row keep the GameInstance-level profile:
//      CharacterId, level, XP, the packed items and the equipped stat row
//      survive every world switch (returning to the menu never clears it).
//   4. A failed map open returns to the menu with a recorded error, never
//      clears the profile, and the menu stays immediately retryable.
//   5. The Initialize-time startup pass restores the profile through the
//      M3-015 chain (StartupLoad -> RestoreFromSave) when a save exists, and
//      a fresh game instance (automation without an injected service) starts
//      without a profile.
//   6. The native map select widget: exactly one TrainingArena entry, the
//      settings/volume text placeholder, and the click path's loading guard
//      plus the failure error line with button recovery.
//
// Harness: every stateful test runs through a REAL UGameInstance created the
// engine's FTestWorldWrapper way (the M3-003 suite precedent): a dedicated
// world context owning the instance, UWorld::SetGameInstance +
// SetCurrentWorld, then Init() - the production path that instantiates every
// registered UGameInstanceSubsystem (including the new flow subsystem and its
// collection dependency on the profile subsystem).
//
// Map-open seam: a temporary test world cannot truly travel, so the tests
// inject a counting FGameFlowMapOpener simulator (the production
// UGameplayStatics::OpenLevelBySoftObjectPtr path stays uncovered by
// automation; the real open effect is the user's H01 check).
//
// Save isolation: the startup test stores its save on the dedicated
// "M3_017Slot_" prefix through an in-memory ISaveStorage double - no real
// file is touched, and the production "Profile_" pass is skipped under
// automation by construction (machine saves never leak into tests).
//
// Stub-failure note: against the red stubs (EnterRoom/ReturnToMenu/
// NotifyRunEnded refuse everything, the selectable entry is null, the widget
// builds no controls) the state-machine, double-click, cycle, retry, startup
// and widget expectations fail on their concrete values; the pure refusal
// prelude of the illegal-transition test matches the stub contract. Exact
// red/green counts live in the task report.

#include "Misc/AutomationTest.h"

#include "../Items/InventoryModel.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../Persistence/ProfileSaveService.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Profile/ProfileSubsystem.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../UI/MapSelectWidget.h"

#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_017
{
	// World names are uniquified: several suites can live in one process and
	// this suite keeps two sessions alive in the startup test.
	static int32 M3_017_WorldCounter = 0;

	// The map package of the card's single selectable entry (the M2-005 suite
	// pins the same soft reference inside Data/rooms.json).
	const TCHAR* M3_017_TrainingArenaPath = TEXT("/Game/UEMMO/Maps/L_TrainingArena");

	// The isolated save prefix of the startup test (never "Profile_": the
	// automation cannot reach the player's real saves by construction).
	const TCHAR* M3_017_SlotPrefix = TEXT("M3_017Slot_");

	/**
	 * One real game instance + its current world, wired exactly like the
	 * engine's FTestWorldWrapper and the M3-003 suite. Create() initializes
	 * the instance (which creates every UGameInstanceSubsystem through the
	 * production collection, the flow subsystem included); TearDown() shuts
	 * everything down in the engine wrapper's order and always clears the
	 * static startup seam so no test can leak it.
	 */
	struct FM3_017_FlowSession
	{
		UWorld* World = nullptr;
		FWorldContext* Context = nullptr;
		UGameInstance* GameInstance = nullptr;
		UProfileSubsystem* Profile = nullptr;
		UGameFlowSubsystem* Flow = nullptr;

		static FM3_017_FlowSession Create(FAutomationTestBase& Test, const TCHAR* Tag)
		{
			FM3_017_FlowSession Session;
			++M3_017_WorldCounter;
			const FName WorldName = FName(*FString::Printf(TEXT("M3_017_TestWorld_%s_%d"), Tag, M3_017_WorldCounter));
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
			Session.Flow = Session.GameInstance->GetSubsystem<UGameFlowSubsystem>();
			Test.TestNotNull(TEXT("the profile subsystem exists in the game instance's collection"), Session.Profile);
			Test.TestNotNull(TEXT("the game flow subsystem exists in the game instance's collection"), Session.Flow);
			return Session;
		}

		/** Full teardown in the engine FTestWorldWrapper::DestroyTestWorld order. */
		void TearDown()
		{
			// The static startup seam must never outlive a test.
			UGameFlowSubsystem::SetStartupSaveServiceForTests(nullptr);
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
			Flow = nullptr;
		}
	};

	/**
	 * In-memory ISaveStorage double of the M3-015 suite (unique M3_017 names):
	 * every written slot is stored as an independent rooted duplicate and
	 * every read returns a fresh duplicate - the same semantics as disk bytes.
	 */
	class FM3_017_MemoryStorage final : public ISaveStorage
	{
	public:
		virtual ~FM3_017_MemoryStorage() override
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

	/** Deterministic item instance (the M3-003 fixture style, M3_017 ids). */
	static FItemInstance M3_017_MakeTestInstance(uint32 Seed)
	{
		FItemInstance Instance;
		Instance.InstanceId = FGuid(0x03170000u + Seed, 0x0BADu, Seed + 17u, Seed * 7u + 1u);
		Instance.DefinitionId = FName(TEXT("weapon_training"));
		Instance.RollSeed = static_cast<int64>(0x17 + Seed);
		Instance.Level = 1;
		return Instance;
	}

	/** Adds one item through the profile's inventory; reports a concrete error. */
	static bool M3_017_AddOrReport(FAutomationTestBase& Test, UProfileSubsystem* Profile,
		const FItemInstance& Instance, const TCHAR* What)
	{
		const EInventoryAddResult Result = Profile->GetInventory().TryAdd(Instance);
		if (Result != EInventoryAddResult::Added)
		{
			Test.AddError(FString::Printf(TEXT("setup: profile TryAdd(%s) returned result code %d instead of Added"),
				What, static_cast<int32>(Result)));
			return false;
		}
		return true;
	}

	/**
	 * Builds the deterministic save request of the startup test: level 3 /
	 * 40 XP, two packed items, one weapon binding, one draft, one applied id.
	 */
	static FProfileSaveRequest M3_017_MakeSaveRequest(FAutomationTestBase& Test)
	{
		FProfileSaveRequest Request;
		Request.Snapshot.CharacterId = FGuid(0x0317FEEDu, 0x2468u, 0x1357BDF0u, 0x0BADF00Du);
		Request.Snapshot.Level = 3;
		Request.Snapshot.XP = 40;
		Request.Snapshot.MaxHP = UProfileSubsystem::GetMaxHPForLevel(3);
		Request.Snapshot.Attack = UProfileSubsystem::GetAttackForLevel(3);
		Request.Snapshot.Defense = UProfileSubsystem::GetDefenseForLevel(3);
		for (int32 Index = 0; Index < 2; ++Index)
		{
			const FItemInstance Instance = M3_017_MakeTestInstance(static_cast<uint32>(0xA0 + Index));
			if (Request.Inventory.TryAdd(Instance) != EInventoryAddResult::Added)
			{
				Test.AddError(FString::Printf(TEXT("fixture: save request TryAdd(item %d) was rejected"), Index));
			}
			if (Index == 0)
			{
				Request.EquippedMap.Add(EItemSlot::Weapon, Instance.InstanceId);
			}
		}
		FPendingReward Draft;
		Draft.SettlementId = 7317ull;
		Draft.XP = 50;
		Request.PendingRewards.Add(Draft);
		Request.AppliedSettlementIds.Add(8317ull);
		return Request;
	}

	/** Creates a save service on the isolated prefix with the given storage. */
	static UProfileSaveService* M3_017_NewService(FAutomationTestBase& Test, ISaveStorage& Storage)
	{
		UProfileSaveService* Service = NewObject<UProfileSaveService>(GetTransientPackage());
		if (!Service)
		{
			Test.AddError(TEXT("setup: NewObject<UProfileSaveService> returned null"));
			return nullptr;
		}
		if (!Service->Initialize(M3_017_SlotPrefix))
		{
			Test.AddError(TEXT("setup: the save service rejected the M3_017Slot_ prefix"));
			return nullptr;
		}
		Service->SetStorage(&Storage);
		return Service;
	}
}

using namespace UE::UEMMO::Tasks::M3_017;

// 1. The legal full cycle: Menu -> Loading -> Room -> Result -> Menu, with the
//    map opener observing the Loading window and the room session's real
//    LeaveRoom semantics running on the menu return.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_017StateMachineAcceptsLegalSequence,
	"UEMMO.Tasks.M3_017.StateMachineAcceptsLegalSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_017StateMachineAcceptsLegalSequence::RunTest(const FString& Parameters)
{
	FM3_017_FlowSession Session = FM3_017_FlowSession::Create(*this, TEXT("Legal"));
	if (Session.Flow == nullptr || Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}

	// A fresh flow starts in the menu with the seeded history entry.
	TestTrue(TEXT("a fresh flow starts in the menu state"),
		Session.Flow->GetState() == EGameFlowState::Menu);
	TestEqual(TEXT("the state history starts with one entry"),
		Session.Flow->GetStateHistory().Num(), 1);
	if (Session.Flow->GetStateHistory().Num() == 1)
	{
		TestEqual(TEXT("the seeded history entry is the menu state"),
			static_cast<int32>(Session.Flow->GetStateHistory()[0]), static_cast<int32>(EGameFlowState::Menu));
	}
	TestFalse(TEXT("no load is in flight on a fresh flow"), Session.Flow->IsLoading());
	TestTrue(TEXT("a fresh flow has no recorded error"), Session.Flow->GetLastError().IsEmpty());

	// The card's one-map menu: exactly one selectable entry, the TrainingArena.
	URoomDefinition* RoomDef = Session.Flow->GetSelectableRoomDefinition();
	if (!TestNotNull(TEXT("the single selectable room definition exists"), RoomDef))
	{
		Session.TearDown();
		return true;
	}
	TestEqual(TEXT("the menu list holds exactly one map"),
		UGameFlowSubsystem::SelectableRoomCount, 1);
	TestEqual(TEXT("the selectable entry carries the TrainingArena map path"),
		RoomDef->MapPath.ToSoftObjectPath().ToString(), FString(M3_017_TrainingArenaPath));
	TestTrue(TEXT("the selectable entry carries a room id"), !RoomDef->RoomId.IsNone());

	// Injected map opener: counts the world switches, records the requested
	// path, and proves the flow is already Loading while the switch runs.
	int32 OpenCount = 0;
	FString OpenedPath;
	bool bWasLoadingDuringOpen = false;
	Session.Flow->SetMapOpenerForTests([&OpenCount, &OpenedPath, &bWasLoadingDuringOpen, &Session](const FString& MapPath)
	{
		++OpenCount;
		OpenedPath = MapPath;
		bWasLoadingDuringOpen = Session.Flow->IsLoading();
		return true;
	});

	TestTrue(TEXT("EnterRoom accepts the menu entry"), Session.Flow->EnterRoom(RoomDef));
	TestEqual(TEXT("the map opener ran exactly once"), OpenCount, 1);
	TestEqual(TEXT("the opener received the TrainingArena path"),
		OpenedPath, FString(M3_017_TrainingArenaPath));
	TestTrue(TEXT("the flow was Loading while the map opened"), bWasLoadingDuringOpen);
	TestTrue(TEXT("the flow is in the room state after the accepted open"),
		Session.Flow->GetState() == EGameFlowState::Room);
	TestFalse(TEXT("no load is in flight after the accepted open"), Session.Flow->IsLoading());
	TestTrue(TEXT("the current room definition is the entered one"),
		Session.Flow->GetCurrentRoomDefinition() == RoomDef);
	TestTrue(TEXT("a successful enter clears the error line"), Session.Flow->GetLastError().IsEmpty());

	// A room run starts through the existing session chain (the flow only
	// tracks the surface state); the terminal notification then moves the flow
	// to Result.
	URoomSessionSubsystem* RoomSession = Session.World ? Session.World->GetSubsystem<URoomSessionSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("the room session subsystem exists in the test world"), RoomSession))
	{
		Session.TearDown();
		return true;
	}
	TestTrue(TEXT("the room session accepted the run of the entered room"),
		RoomSession->StartRoom(RoomDef));

	TestTrue(TEXT("NotifyRunEnded moves Room to Result"), Session.Flow->NotifyRunEnded());
	TestTrue(TEXT("the flow shows the result state"),
		Session.Flow->GetState() == EGameFlowState::Result);

	// Result -> Menu: the return calls the session's existing LeaveRoom (the
	// M2-011 exit semantics move the run into Exiting) and the flow lands in
	// Menu.
	TestTrue(TEXT("ReturnToMenu moves Result to Menu"), Session.Flow->ReturnToMenu());
	TestTrue(TEXT("the flow is back in the menu state"),
		Session.Flow->GetState() == EGameFlowState::Menu);
	TestTrue(TEXT("the menu return dropped the current room definition"),
		Session.Flow->GetCurrentRoomDefinition() == nullptr);
	TestTrue(TEXT("the room session's run scope entered the exit state (LeaveRoom ran)"),
		RoomSession->GetState() == ERoomSessionState::Exiting);
	TestTrue(TEXT("a clean cycle leaves no error line"), Session.Flow->GetLastError().IsEmpty());

	// The full accepted history reads Menu, Loading, Room, Result, Menu.
	const TArray<EGameFlowState>& History = Session.Flow->GetStateHistory();
	TestEqual(TEXT("the accepted sequence history holds exactly 5 entries"), History.Num(), 5);
	if (History.Num() == 5)
	{
		TestEqual(TEXT("history[0] is Menu"), static_cast<int32>(History[0]), static_cast<int32>(EGameFlowState::Menu));
		TestEqual(TEXT("history[1] is Loading"), static_cast<int32>(History[1]), static_cast<int32>(EGameFlowState::Loading));
		TestEqual(TEXT("history[2] is Room"), static_cast<int32>(History[2]), static_cast<int32>(EGameFlowState::Room));
		TestEqual(TEXT("history[3] is Result"), static_cast<int32>(History[3]), static_cast<int32>(EGameFlowState::Result));
		TestEqual(TEXT("history[4] is Menu"), static_cast<int32>(History[4]), static_cast<int32>(EGameFlowState::Menu));
	}

	Session.TearDown();
	return true;
}

// 2. Every transition outside the legal graph is refused: nothing is requested
//    and no history entry is appended; the refusal is readable as an error.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_017StateMachineRejectsIllegalTransitions,
	"UEMMO.Tasks.M3_017.StateMachineRejectsIllegalTransitions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_017StateMachineRejectsIllegalTransitions::RunTest(const FString& Parameters)
{
	FM3_017_FlowSession Session = FM3_017_FlowSession::Create(*this, TEXT("Illegal"));
	if (Session.Flow == nullptr || Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	UGameFlowSubsystem* Flow = Session.Flow;

	// From the menu: nothing may end a run or leave a room.
	TestTrue(TEXT("the test starts from the menu state"), Flow->GetState() == EGameFlowState::Menu);
	TestFalse(TEXT("NotifyRunEnded is refused from Menu"), Flow->NotifyRunEnded());
	TestFalse(TEXT("ReturnToMenu is refused from Menu"), Flow->ReturnToMenu());
	TestEqual(TEXT("the history is unchanged by the menu refusals"),
		Flow->GetStateHistory().Num(), 1);

	// EnterRoom argument guards: no world switch may ever be requested for a
	// null or map-less definition.
	int32 OpenCount = 0;
	Flow->SetMapOpenerForTests([&OpenCount](const FString&)
	{
		++OpenCount;
		return true;
	});
	TestFalse(TEXT("EnterRoom refuses a null definition"), Flow->EnterRoom(nullptr));
	URoomDefinition* Broken = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
	Broken->RoomId = FName(TEXT("room_m3_017_broken"));
	// MapPath stays empty (the malformed resource-path guard).
	TestFalse(TEXT("EnterRoom refuses a definition without a map path"), Flow->EnterRoom(Broken));
	TestEqual(TEXT("no world switch was requested for the invalid definitions"), OpenCount, 0);
	TestTrue(TEXT("a refused enter records an error"), !Flow->GetLastError().IsEmpty());
	TestEqual(TEXT("the history is unchanged by the argument refusals"),
		Flow->GetStateHistory().Num(), 1);

	// Inside the Loading window: every re-entrant request is refused (the
	// bLoading guard of the card's anti-double-open rule).
	URoomDefinition* RoomDef = Flow->GetSelectableRoomDefinition();
	if (!TestNotNull(TEXT("the selectable room definition exists"), RoomDef))
	{
		Session.TearDown();
		return true;
	}
	bool bReenterAccepted = true;
	bool bReenterNotifyAccepted = true;
	bool bReenterReturnAccepted = true;
	Flow->SetMapOpenerForTests([&](const FString&)
	{
		++OpenCount;
		bReenterAccepted = Flow->EnterRoom(RoomDef);
		bReenterNotifyAccepted = Flow->NotifyRunEnded();
		bReenterReturnAccepted = Flow->ReturnToMenu();
		return true;
	});
	TestTrue(TEXT("EnterRoom accepts the valid entry from the menu"), Flow->EnterRoom(RoomDef));
	TestFalse(TEXT("the re-entrant EnterRoom inside the loading window was refused"), bReenterAccepted);
	TestFalse(TEXT("NotifyRunEnded inside the loading window was refused"), bReenterNotifyAccepted);
	TestFalse(TEXT("ReturnToMenu inside the loading window was refused"), bReenterReturnAccepted);
	TestEqual(TEXT("the loading window still produced exactly one switch"), OpenCount, 1);
	TestTrue(TEXT("the flow reached the room state exactly once"),
		Flow->GetState() == EGameFlowState::Room);

	// From Room: entering again is refused (no second switch).
	TestFalse(TEXT("EnterRoom from Room is refused"), Flow->EnterRoom(RoomDef));
	TestEqual(TEXT("no further switch was requested from Room"), OpenCount, 1);

	// From Result: the run end is one-shot and entering again is refused.
	TestTrue(TEXT("NotifyRunEnded moves the run into Result"), Flow->NotifyRunEnded());
	TestFalse(TEXT("a second NotifyRunEnded from Result is refused"), Flow->NotifyRunEnded());
	TestFalse(TEXT("EnterRoom from Result is refused"), Flow->EnterRoom(RoomDef));
	TestEqual(TEXT("no further switch was requested from Result"), OpenCount, 1);

	// After the menu return the machine is back to its start rules.
	TestTrue(TEXT("ReturnToMenu returns from Result"), Flow->ReturnToMenu());
	TestFalse(TEXT("NotifyRunEnded is refused from the restored Menu"), Flow->NotifyRunEnded());
	TestEqual(TEXT("the history holds only the accepted transitions (5 entries)"),
		Flow->GetStateHistory().Num(), 5);

	Session.TearDown();
	return true;
}

// 3. Acceptance: a fast double-click on enter produces exactly ONE world
//    switch - the second click lands inside the Loading window and is refused.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_017FastDoubleEnterOpensMapOnce,
	"UEMMO.Tasks.M3_017.FastDoubleEnterOpensMapOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_017FastDoubleEnterOpensMapOnce::RunTest(const FString& Parameters)
{
	FM3_017_FlowSession Session = FM3_017_FlowSession::Create(*this, TEXT("Double"));
	if (Session.Flow == nullptr || Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	UGameFlowSubsystem* Flow = Session.Flow;

	URoomDefinition* RoomDef = Flow->GetSelectableRoomDefinition();
	if (!TestNotNull(TEXT("the selectable room definition exists"), RoomDef))
	{
		Session.TearDown();
		return true;
	}

	// The opener simulates the loading window: the second click of the fast
	// double-click happens WHILE the first open is executing.
	int32 OpenCount = 0;
	bool bSecondClickRefused = true;
	bool bLoadingOnSecondClick = false;
	Flow->SetMapOpenerForTests([&](const FString&)
	{
		++OpenCount;
		bLoadingOnSecondClick = Flow->IsLoading();
		bSecondClickRefused = !Flow->EnterRoom(RoomDef);
		return true;
	});

	TestTrue(TEXT("the first click of the double-click enters the room"),
		Flow->EnterRoom(RoomDef));
	TestTrue(TEXT("the second click landed inside the loading window"), bLoadingOnSecondClick);
	TestTrue(TEXT("the second click was refused by the loading guard"), bSecondClickRefused);
	TestEqual(TEXT("the fast double-click created exactly one world switch"), OpenCount, 1);
	TestTrue(TEXT("the flow is in the room state after the double-click"),
		Flow->GetState() == EGameFlowState::Room);

	// A third click after the accepted entry requests no further switch.
	TestFalse(TEXT("a third click from the room state is refused"), Flow->EnterRoom(RoomDef));
	TestEqual(TEXT("still exactly one world switch after the third click"), OpenCount, 1);

	Session.TearDown();
	return true;
}

// 4. Acceptance: three enter/exit cycles in a row keep the profile - the
//    CharacterId, the progress, the packed items and the equipped stat row all
//    survive the world switches (the GameInstance lifecycle is the guarantee).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_017ThreeEnterExitCyclesKeepProfile,
	"UEMMO.Tasks.M3_017.ThreeEnterExitCyclesKeepProfile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_017ThreeEnterExitCyclesKeepProfile::RunTest(const FString& Parameters)
{
	FM3_017_FlowSession Session = FM3_017_FlowSession::Create(*this, TEXT("Cycles"));
	if (Session.Flow == nullptr || Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	UGameFlowSubsystem* Flow = Session.Flow;
	UProfileSubsystem* Profile = Session.Profile;

	// Build a non-trivial profile: identity, level 2 with 50 XP, two packed
	// items and an equipped stat row (the M3-005 equipment bonus surface).
	Profile->NewProfile();
	const FGuid CharacterId = Profile->GetCharacterId();
	if (!TestTrue(TEXT("the cycle-test profile gains a level"), Profile->AddXP(150)) ||
		!M3_017_AddOrReport(*this, Profile, M3_017_MakeTestInstance(0x51u), TEXT("CycleItemA")) ||
		!M3_017_AddOrReport(*this, Profile, M3_017_MakeTestInstance(0x52u), TEXT("CycleItemB")))
	{
		Session.TearDown();
		return true;
	}
	FItemStats EquippedRow;
	EquippedRow.Attack = 3.5f;
	EquippedRow.Defense = 1.25f;
	EquippedRow.MaxHP = 40.0f;
	Profile->SetEquippedStatBonus(EquippedRow);

	URoomDefinition* RoomDef = Flow->GetSelectableRoomDefinition();
	if (!TestNotNull(TEXT("the selectable room definition exists"), RoomDef))
	{
		Session.TearDown();
		return true;
	}

	int32 OpenCount = 0;
	Flow->SetMapOpenerForTests([&OpenCount](const FString&)
	{
		++OpenCount;
		return true;
	});

	for (int32 Cycle = 1; Cycle <= 3; ++Cycle)
	{
		if (!TestTrue(FString::Printf(TEXT("cycle %d: EnterRoom accepts the menu entry"), Cycle),
			Flow->EnterRoom(RoomDef)))
		{
			break;
		}
		TestTrue(FString::Printf(TEXT("cycle %d: the flow reached the room state"), Cycle),
			Flow->GetState() == EGameFlowState::Room);
		TestTrue(FString::Printf(TEXT("cycle %d: the run end moves the flow to Result"), Cycle),
			Flow->NotifyRunEnded());
		TestTrue(FString::Printf(TEXT("cycle %d: ReturnToMenu returns to the menu"), Cycle),
			Flow->ReturnToMenu());
		TestTrue(FString::Printf(TEXT("cycle %d: the flow is back in the menu state"), Cycle),
			Flow->GetState() == EGameFlowState::Menu);

		// The GameInstance-level profile survived the world switch untouched.
		TestTrue(FString::Printf(TEXT("cycle %d: the profile still exists"), Cycle), Profile->HasProfile());
		TestTrue(FString::Printf(TEXT("cycle %d: the CharacterId survived"), Cycle),
			Profile->GetCharacterId() == CharacterId);
		TestEqual(FString::Printf(TEXT("cycle %d: the level survived"), Cycle), Profile->GetLevel(), 2);
		TestEqual(FString::Printf(TEXT("cycle %d: the XP survived"), Cycle), Profile->GetXP(), 50);
		TestEqual(FString::Printf(TEXT("cycle %d: both packed items survived"), Cycle),
			Profile->GetInventory().Count(), 2);
		TestTrue(FString::Printf(TEXT("cycle %d: item A is still packed"), Cycle),
			Profile->GetInventory().Contains(M3_017_MakeTestInstance(0x51u).InstanceId));
		TestTrue(FString::Printf(TEXT("cycle %d: item B is still packed"), Cycle),
			Profile->GetInventory().Contains(M3_017_MakeTestInstance(0x52u).InstanceId));
		const FItemStats& KeptRow = Profile->GetEquippedStatBonus();
		TestTrue(FString::Printf(TEXT("cycle %d: the equipped stat row survived"), Cycle),
			FMath::IsNearlyEqual(KeptRow.Attack, 3.5f) && FMath::IsNearlyEqual(KeptRow.Defense, 1.25f) &&
			FMath::IsNearlyEqual(KeptRow.MaxHP, 40.0f));
	}

	TestEqual(TEXT("three cycles requested exactly three world switches"), OpenCount, 3);
	const TArray<EGameFlowState>& History = Session.Flow->GetStateHistory();
	// Menu (seed) + 3 x (Loading, Room, Result, Menu) = 13 accepted entries.
	TestEqual(TEXT("the history holds the 13 accepted entries of the three cycles"),
		History.Num(), 13);

	Session.TearDown();
	return true;
}

// 5. Acceptance: a failed map open returns to the menu with a recorded error,
//    never clears the profile, and the menu stays immediately retryable.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_017FailedLoadReturnsToMenuWithProfileIntact,
	"UEMMO.Tasks.M3_017.FailedLoadReturnsToMenuWithProfileIntact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_017FailedLoadReturnsToMenuWithProfileIntact::RunTest(const FString& Parameters)
{
	FM3_017_FlowSession Session = FM3_017_FlowSession::Create(*this, TEXT("FailLoad"));
	if (Session.Flow == nullptr || Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}
	UGameFlowSubsystem* Flow = Session.Flow;
	UProfileSubsystem* Profile = Session.Profile;

	// A profile with progress and one packed item must survive the failure.
	Profile->NewProfile();
	const FGuid CharacterId = Profile->GetCharacterId();
	if (!TestTrue(TEXT("the failure-test profile gains a level"), Profile->AddXP(150)) ||
		!M3_017_AddOrReport(*this, Profile, M3_017_MakeTestInstance(0x61u), TEXT("FailItem")))
	{
		Session.TearDown();
		return true;
	}

	URoomDefinition* RoomDef = Flow->GetSelectableRoomDefinition();
	if (!TestNotNull(TEXT("the selectable room definition exists"), RoomDef))
	{
		Session.TearDown();
		return true;
	}

	// The failing open (the simulated resource-path error): one switch is
	// requested, the open fails, the flow falls back to the menu.
	int32 OpenCount = 0;
	Flow->SetMapOpenerForTests([&OpenCount](const FString&)
	{
		++OpenCount;
		return false;
	});

	TestFalse(TEXT("EnterRoom reports the failed map open"), Flow->EnterRoom(RoomDef));
	TestEqual(TEXT("the failed open requested exactly one switch"), OpenCount, 1);
	TestTrue(TEXT("the flow fell back to the menu state"),
		Flow->GetState() == EGameFlowState::Menu);
	TestFalse(TEXT("no load is in flight after the failure"), Flow->IsLoading());
	TestTrue(TEXT("the failure recorded an error"), !Flow->GetLastError().IsEmpty());
	TestTrue(TEXT("the error names the map that failed to open"),
		Flow->GetLastError().Contains(TEXT("L_TrainingArena")));
	TestTrue(TEXT("the failure dropped the current room definition"),
		Flow->GetCurrentRoomDefinition() == nullptr);
	// The history shows the accepted Menu -> Loading -> Menu fallback round trip.
	const TArray<EGameFlowState>& History = Flow->GetStateHistory();
	TestEqual(TEXT("the fallback history holds Menu, Loading, Menu"), History.Num(), 3);
	if (History.Num() == 3)
	{
		TestEqual(TEXT("fallback history[1] is Loading"), static_cast<int32>(History[1]), static_cast<int32>(EGameFlowState::Loading));
		TestEqual(TEXT("fallback history[2] is Menu"), static_cast<int32>(History[2]), static_cast<int32>(EGameFlowState::Menu));
	}

	// The profile is untouched (the card: a failed load never clears it).
	TestTrue(TEXT("the profile still exists after the failed load"), Profile->HasProfile());
	TestTrue(TEXT("the CharacterId survived the failed load"),
		Profile->GetCharacterId() == CharacterId);
	TestEqual(TEXT("the level survived the failed load"), Profile->GetLevel(), 2);
	TestEqual(TEXT("the XP survived the failed load"), Profile->GetXP(), 50);
	TestEqual(TEXT("the packed item survived the failed load"),
		Profile->GetInventory().Count(), 1);
	TestTrue(TEXT("the packed instance id survived the failed load"),
		Profile->GetInventory().Contains(M3_017_MakeTestInstance(0x61u).InstanceId));

	// The menu stays operational: a retry with a good open succeeds.
	int32 RetryCount = 0;
	Flow->SetMapOpenerForTests([&RetryCount](const FString&)
	{
		++RetryCount;
		return true;
	});
	TestTrue(TEXT("a retry after the failure enters the room"), Flow->EnterRoom(RoomDef));
	TestEqual(TEXT("the retry requested exactly one switch"), RetryCount, 1);
	TestTrue(TEXT("the retry reached the room state"),
		Flow->GetState() == EGameFlowState::Room);
	TestTrue(TEXT("the successful retry cleared the error line"), Flow->GetLastError().IsEmpty());

	// The retry's room closes cleanly back to the menu.
	TestTrue(TEXT("the retry's run end moves to Result"), Flow->NotifyRunEnded());
	TestTrue(TEXT("the retry's return lands in the menu"), Flow->ReturnToMenu());
	TestTrue(TEXT("the retry cycle ends in the menu state"),
		Flow->GetState() == EGameFlowState::Menu);

	Session.TearDown();
	return true;
}

// 6. The Initialize-time startup pass (the M3-015 chain): a committed save on
//    the isolated prefix is restored through StartupLoad + RestoreFromSave at
//    GameInstance::Init; a fresh instance without an injected service starts
//    without a profile (automation never reads the player's real saves).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_017StartupLoadRestoresProfileAtInit,
	"UEMMO.Tasks.M3_017.StartupLoadRestoresProfileAtInit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_017StartupLoadRestoresProfileAtInit::RunTest(const FString& Parameters)
{
	// Build one committed save on the isolated in-memory prefix.
	FM3_017_MemoryStorage Storage;
	UProfileSaveService* Writer = M3_017_NewService(*this, Storage);
	if (Writer == nullptr)
	{
		return true;
	}
	const FProfileSaveRequest Request = M3_017_MakeSaveRequest(*this);
	const FProfileSaveOutcome SaveOutcome = Writer->SaveProfile(Request);
	if (!TestTrue(TEXT("the fixture save committed on the M3_017Slot_ prefix"),
		SaveOutcome.Result == EProfileSaveResult::Success))
	{
		return true;
	}

	// The next flow Initialize consumes the injected service and restores.
	UProfileSaveService* Reader = M3_017_NewService(*this, Storage);
	if (Reader == nullptr)
	{
		return true;
	}
	UGameFlowSubsystem::SetStartupSaveServiceForTests(Reader);

	FM3_017_FlowSession Session = FM3_017_FlowSession::Create(*this, TEXT("Startup"));
	if (Session.Flow == nullptr || Session.Profile == nullptr)
	{
		Session.TearDown();
		return true;
	}

	// The restored profile state (identity, progress, inventory).
	TestTrue(TEXT("Initialize restored the saved profile"), Session.Profile->HasProfile());
	TestTrue(TEXT("the restored CharacterId matches the save"),
		Session.Profile->GetCharacterId() == Request.Snapshot.CharacterId);
	TestEqual(TEXT("the restored level matches the save"), Session.Profile->GetLevel(), 3);
	TestEqual(TEXT("the restored XP matches the save"), Session.Profile->GetXP(), 40);
	TestEqual(TEXT("the restored inventory holds both saved items"),
		Session.Profile->GetInventory().Count(), 2);
	TestTrue(TEXT("the restored inventory contains saved item A"),
		Session.Profile->GetInventory().Contains(M3_017_MakeTestInstance(0xA0u).InstanceId));
	TestTrue(TEXT("the restored inventory contains saved item B"),
		Session.Profile->GetInventory().Contains(M3_017_MakeTestInstance(0xA1u).InstanceId));
	const FItemStats& RestoredRow = Session.Profile->GetEquippedStatBonus();
	TestTrue(TEXT("the restored equipment bonus row is the zero row (the gameplay layer re-derives it)"),
		RestoredRow.Attack == 0.0f && RestoredRow.Defense == 0.0f && RestoredRow.MaxHP == 0.0f);

	// The flow reports the restore and the structured startup outcome.
	TestTrue(TEXT("the flow reports the startup restore"), Session.Flow->WasStartupProfileRestored());
	const FStartupLoadOutcome& Outcome = Session.Flow->GetStartupLoadOutcome();
	TestEqual(TEXT("the startup outcome is the direct recovery"),
		static_cast<int32>(Outcome.Result), static_cast<int32>(EStartupLoadResult::Recovered));
	TestEqual(TEXT("the recovery came from slot A"), Outcome.SlotIndex, 0);
	TestEqual(TEXT("the recovery came from generation 1"), Outcome.Generation, 1);

	Session.TearDown();

	// The fresh case: an instance without an injected service starts empty
	// (the production "Profile_" pass is skipped under automation by design).
	FM3_017_FlowSession Fresh = FM3_017_FlowSession::Create(*this, TEXT("StartupFresh"));
	if (Fresh.Flow != nullptr && Fresh.Profile != nullptr)
	{
		TestFalse(TEXT("a fresh game instance starts without a profile"), Fresh.Profile->HasProfile());
		TestFalse(TEXT("the fresh flow restored nothing"), Fresh.Flow->WasStartupProfileRestored());
		TestEqual(TEXT("the fresh startup outcome is NoSaveFound"),
			static_cast<int32>(Fresh.Flow->GetStartupLoadOutcome().Result),
			static_cast<int32>(EStartupLoadResult::NoSaveFound));
	}
	Fresh.TearDown();
	return true;
}

// 7. The native map select widget: exactly one TrainingArena entry, the
//    settings/volume placeholder, the loading guard and the failure error
//    line with button recovery.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_017WidgetMenuSingleMapAndLoadGuard,
	"UEMMO.Tasks.M3_017.WidgetMenuSingleMapAndLoadGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_017WidgetMenuSingleMapAndLoadGuard::RunTest(const FString& Parameters)
{
	FM3_017_FlowSession Session = FM3_017_FlowSession::Create(*this, TEXT("Widget"));
	if (Session.Flow == nullptr || Session.Profile == nullptr || Session.World == nullptr)
	{
		Session.TearDown();
		return true;
	}
	UGameFlowSubsystem* Flow = Session.Flow;
	Flow->GetSelectableRoomDefinition(); // warm the single entry cache

	UMapSelectWidget* Widget = CreateWidget<UMapSelectWidget>(Session.World, UMapSelectWidget::StaticClass());
	if (!TestNotNull(TEXT("the native map select widget is created in a plain game world (no UMG asset)"), Widget))
	{
		Session.TearDown();
		return true;
	}
	// Engine 5.8 runs NativeOnInitialized only with a valid player context;
	// BindMenu is the presentation entry that builds the tree (the M2-012
	// BindResult safety-net precedent) and arms a fresh presentation.
	Widget->BindMenu();

	// The card's one-map menu surface.
	TestEqual(TEXT("the widget declares exactly one map entry"),
		UMapSelectWidget::MapEntryCount, 1);
	TestNotNull(TEXT("the title block exists"), Widget->PeekTitleBlock());
	if (Widget->PeekTitleBlock() != nullptr)
	{
		TestEqual(TEXT("the title names the map select menu"),
			Widget->PeekTitleBlock()->GetText().ToString(), FString(TEXT("Map Select")));
	}
	TestNotNull(TEXT("the single map entry block exists"), Widget->PeekMapEntryBlock());
	if (Widget->PeekMapEntryBlock() != nullptr)
	{
		TestEqual(TEXT("the single entry is the TrainingArena row"),
			Widget->PeekMapEntryBlock()->GetText().ToString(), FString(TEXT("TrainingArena")));
	}
	TestNotNull(TEXT("the settings/volume placeholder block exists"),
		Widget->PeekSettingsPlaceholderBlock());
	if (Widget->PeekSettingsPlaceholderBlock() != nullptr)
	{
		TestTrue(TEXT("the placeholder names the unimplemented settings entry"),
			Widget->PeekSettingsPlaceholderBlock()->GetText().ToString().Contains(TEXT("Settings")));
	}
	TestNotNull(TEXT("the enter button exists"), Widget->PeekEnterButton());
	if (Widget->PeekEnterButton() != nullptr)
	{
		TestTrue(TEXT("the enter button starts enabled"), Widget->PeekEnterButton()->GetIsEnabled());
	}
	if (Widget->PeekEnterLabel() != nullptr)
	{
		TestEqual(TEXT("the enter button carries its label"),
			Widget->PeekEnterLabel()->GetText().ToString(), FString(TEXT("Enter")));
	}
	if (Widget->PeekErrorBlock() != nullptr)
	{
		TestTrue(TEXT("the error line starts collapsed"),
			Widget->PeekErrorBlock()->GetVisibility() == ESlateVisibility::Collapsed);
	}

	// The click path with a successful open: one switch, the flow enters the
	// room, the button stays disabled (the visible loading guard half).
	int32 OpenCount = 0;
	Flow->SetMapOpenerForTests([&OpenCount](const FString&)
	{
		++OpenCount;
		return true;
	});
	Widget->HandleEnterClicked();
	TestEqual(TEXT("the click requested exactly one world switch"), OpenCount, 1);
	TestTrue(TEXT("the flow entered the room through the widget click"),
		Flow->GetState() == EGameFlowState::Room);
	if (Widget->PeekEnterButton() != nullptr)
	{
		TestFalse(TEXT("the button stays disabled after the accepted entry"),
			Widget->PeekEnterButton()->GetIsEnabled());
	}
	// A duplicate click while the room is active requests nothing further.
	Widget->HandleEnterClicked();
	TestEqual(TEXT("the duplicate click requested no further switch"), OpenCount, 1);

	// Back to the menu: a re-presented menu re-arms through BindMenu (the
	// error line clears and the button re-enables).
	TestTrue(TEXT("the flow returns to the menu"), Flow->ReturnToMenu());
	Widget->BindMenu();
	if (Widget->PeekEnterButton() != nullptr)
	{
		TestTrue(TEXT("the re-presented menu re-enabled the enter button"),
			Widget->PeekEnterButton()->GetIsEnabled());
	}
	if (Widget->PeekErrorBlock() != nullptr)
	{
		TestTrue(TEXT("the re-presented menu starts with a collapsed error line"),
			Widget->PeekErrorBlock()->GetVisibility() == ESlateVisibility::Collapsed);
	}

	// The click path with a FAILED open: the error line shows the flow's
	// reason and the button recovers (the card: the failure stays retryable).
	Flow->SetMapOpenerForTests([&OpenCount](const FString&)
	{
		++OpenCount;
		return false;
	});
	Widget->HandleEnterClicked();
	TestEqual(TEXT("the failed click requested one switch"), OpenCount, 2);
	TestTrue(TEXT("the flow fell back to the menu after the failed click"),
		Flow->GetState() == EGameFlowState::Menu);
	if (Widget->PeekErrorBlock() != nullptr)
	{
		TestTrue(TEXT("the error line became visible after the failure"),
			Widget->PeekErrorBlock()->GetVisibility() != ESlateVisibility::Collapsed);
		TestTrue(TEXT("the error line shows the flow's failure reason"),
			Widget->PeekErrorBlock()->GetText().ToString().Contains(TEXT("failed to open")));
	}
	if (Widget->PeekEnterButton() != nullptr)
	{
		TestTrue(TEXT("the button recovered after the failure"),
			Widget->PeekEnterButton()->GetIsEnabled());
	}

	// The recovered menu enters again (the retry path).
	Flow->SetMapOpenerForTests([&OpenCount](const FString&)
	{
		++OpenCount;
		return true;
	});
	Widget->HandleEnterClicked();
	TestEqual(TEXT("the retry requested one more switch"), OpenCount, 3);
	TestTrue(TEXT("the retry entered the room"), Flow->GetState() == EGameFlowState::Room);

	Session.TearDown();
	return true;
}

#endif
