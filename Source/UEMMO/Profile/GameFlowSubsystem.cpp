// M3-017: game flow state machine between the map select menu and the combat
// room (task card "single map select menu and room enter/leave"). GREEN
// implementation of the stubbed contract: the Menu -> Loading -> Room ->
// Result -> Menu transitions with the bLoading anti-double-open guard, the
// injectable map-open seam over the production OpenLevel path, the M2-011
// LeaveRoom semantics on the menu return, and the Initialize-time startup
// restore through the M3-015 chain (StartupLoad -> RestoreFromSave).

#include "GameFlowSubsystem.h"

#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "CoreGlobals.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/PackageName.h"

namespace
{
	// M3-017 test seam: the save service the NEXT Initialize consumes (rooted
	// while parked). Always null in production.
	UProfileSaveService* GM3_017_StartupSaveServiceOverride = nullptr;

	// The card's single selectable map (the M2-005 suite pins the same soft
	// reference inside Data/rooms.json).
	const TCHAR* GM3_017_TrainingArenaMapPath = TEXT("/Game/UEMMO/Maps/L_TrainingArena");

	// M3-030: the production mount's enter target - the boot world's
	// wave-combat room. RoomId matches the M2-009/M2-016 trigger default the
	// L_CombatRoom01 placement starts runs with (room_combat_01 + the 2+3
	// melee_grunt fallback waves), so the flow's enter identity and the
	// in-world run identity agree.
	const TCHAR* GM3_030_CombatRoomMapPath = TEXT("/Game/UEMMO/Maps/L_CombatRoom01");
	const TCHAR* GM3_030_CombatRoomId = TEXT("room_combat_01");
	const TCHAR* GM3_030_CombatRoomRewardTableId = TEXT("starter");

	// M5-018A: the system test room entry (the menu's second legal destination).
	const TCHAR* GM5_018A_SystemTestRoomMapPath = TEXT("/Game/UEMMO/Maps/L_SystemTestRoom");
	const TCHAR* GM5_018A_SystemTestRoomId = TEXT("room_system_test_01");
	const TCHAR* GM5_018A_SystemTestRoomRewardTableId = TEXT("starter");

	// The production save prefix of the M3-014/M3-015 chain (only ever used
	// outside automation; automation runs the isolated injected service).
	const TCHAR* GM3_017_ProductionSlotPrefix = TEXT("Profile_");
}

void UGameFlowSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	State = EGameFlowState::Menu;
	StateHistory.Reset();
	StateHistory.Add(EGameFlowState::Menu);
	bLoading = false;
	LastError.Reset();
	CurrentRoomDef = nullptr;
	CachedSelectableRoom = nullptr;
	CachedCombatRoom = nullptr;
	CachedSystemTestRoom = nullptr;
	StartupOutcome = FStartupLoadOutcome();
	bStartupProfileRestored = false;

	// M3-015 chain: the profile subsystem is an explicit collection dependency
	// so the startup restore below always finds it initialized.
	UProfileSubsystem* Profile = Collection.InitializeDependency<UProfileSubsystem>();

	// A parked automation service wins; production builds its own service on
	// the real prefix. Automation WITHOUT an injected service skips the real
	// pass entirely: machine save state must never leak into tests (and the
	// M3-003 "fresh instance has no profile" guarantee must hold there - the
	// synthesized NoSaveFound below therefore creates NO profile; only a REAL
	// NoSaveFound startup pass bootstraps the new-game profile, M3-029).
	UProfileSaveService* Service = ConsumeStartupSaveServiceOverride();
	bool bRanStartupPass = false;
	if (Service == nullptr && !GIsAutomationTesting && Profile != nullptr)
	{
		UProfileSaveService* ProductionService = NewObject<UProfileSaveService>(this);
		if (ProductionService->Initialize(GM3_017_ProductionSlotPrefix))
		{
			Service = ProductionService;
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("UEMMO M3-017: the production save service rejected the '%s' prefix; the startup restore is skipped."),
				GM3_017_ProductionSlotPrefix);
		}
	}

	if (Service != nullptr && Profile != nullptr)
	{
		bRanStartupPass = true;
		StartupOutcome = Service->StartupLoad();
		const bool bRecovered =
			StartupOutcome.Result == EStartupLoadResult::Recovered ||
			StartupOutcome.Result == EStartupLoadResult::RecoveredFallback;
		if (bRecovered)
		{
			// RestoreFromSave is all-or-nothing: a refusal keeps the "no
			// profile" state and never falls back to a reset (the M3-015
			// rule: a failed load never silently starts over).
			bStartupProfileRestored = Profile->RestoreFromSave(
				StartupOutcome.Snapshot,
				StartupOutcome.Inventory,
				StartupOutcome.PendingRewards,
				StartupOutcome.AppliedSettlementIds,
				StartupOutcome.EquippedMap);
			if (!bStartupProfileRestored)
			{
				UE_LOG(LogTemp, Error, TEXT("UEMMO M3-017: the startup snapshot was refused by RestoreFromSave; the profile stays empty and the outcome stays queryable."));
			}
		}
		else if (StartupOutcome.Result == EStartupLoadResult::RecoveryError)
		{
			// The corrupt evidence stays on disk, the save path is guarded and
			// the report stays readable through GetStartupLoadOutcome.
			UE_LOG(LogTemp, Error, TEXT("UEMMO M3-017: the startup recovery failed: %s"), *StartupOutcome.Summary);
		}
		else if (StartupOutcome.Result == EStartupLoadResult::NoSaveFound)
		{
			// M3-029: the fresh-install answer of the M3-015 pass is the legal
			// FIRST BOOT, and the flow bootstraps the new-game profile right
			// here (explicit NewProfile: fresh unique CharacterId, Level 1,
			// XP 0, empty containers). This is deliberately NOT the M3-003
			// "no automatic reset on failure" violation: NoSaveFound is not a
			// failure but the interface contract's "nothing stored yet - the
			// caller decides" answer, and the game flow IS that caller. The
			// RecoveryError branch above keeps the no-profile rule unchanged
			// (a corrupt save never silently starts over, M3-015).
			Profile->NewProfile();
			UE_LOG(LogTemp, Log, TEXT("UEMMO M3-029: first boot (no save found) - the game flow bootstrapped the new-game profile (character %s)."),
				*Profile->GetCharacterId().ToString());
		}
	}

	if (!bRanStartupPass)
	{
		// The automation skip path: the synthesized NoSaveFound outcome is a
		// test-isolation marker, NOT a real first boot - no profile is
		// created here (the M3-003 fresh-instance guarantee; only the real
		// pass's NoSaveFound branch above bootstraps, M3-029).
		StartupOutcome = FStartupLoadOutcome();
		StartupOutcome.Result = EStartupLoadResult::NoSaveFound;
		StartupOutcome.Summary = TEXT("M3-017: the startup restore pass did not run (automation without an injected save service; the real boot runs the Profile_ pass).");
	}

	UE_LOG(LogTemp, Log, TEXT("UEMMO M3-017: game flow initialized (startup restore: %s; summary: %s)"),
		bStartupProfileRestored ? TEXT("restored") : TEXT("none"),
		*StartupOutcome.Summary);
}

void UGameFlowSubsystem::Deinitialize()
{
	MapOpener = nullptr;
	Super::Deinitialize();
}

void UGameFlowSubsystem::SetMapOpenerForTests(FGameFlowMapOpener InOpener)
{
	MapOpener = MoveTemp(InOpener);
}

void UGameFlowSubsystem::SetStartupSaveServiceForTests(UProfileSaveService* Service)
{
	if (GM3_017_StartupSaveServiceOverride)
	{
		GM3_017_StartupSaveServiceOverride->RemoveFromRoot();
		GM3_017_StartupSaveServiceOverride = nullptr;
	}
	if (Service)
	{
		Service->AddToRoot();
		GM3_017_StartupSaveServiceOverride = Service;
	}
}

UProfileSaveService* UGameFlowSubsystem::ConsumeStartupSaveServiceOverride()
{
	UProfileSaveService* Service = GM3_017_StartupSaveServiceOverride;
	GM3_017_StartupSaveServiceOverride = nullptr;
	if (Service)
	{
		Service->RemoveFromRoot();
	}
	return Service;
}

bool UGameFlowSubsystem::EnterRoom(const URoomDefinition* RoomDef)
{
	// The state gate doubles as the card's bLoading guard: only the menu
	// accepts an entry, and while a load is in flight every further request is
	// refused - so a fast double-click can never open the map twice.
	if (State != EGameFlowState::Menu || bLoading)
	{
		RecordError(FString::Printf(TEXT("M3-017: EnterRoom refused in state %d (only the menu accepts an entry, and a load in progress must finish first)."),
			static_cast<int32>(State)));
		return false;
	}
	if (RoomDef == nullptr)
	{
		RecordError(TEXT("M3-017: EnterRoom refused - the room definition is null."));
		return false;
	}
	const FSoftObjectPath MapPath = RoomDef->MapPath.ToSoftObjectPath();
	if (RoomDef->RoomId.IsNone() || MapPath.ToString().IsEmpty())
	{
		RecordError(FString::Printf(TEXT("M3-017: EnterRoom refused - the room definition '%s' carries no RoomId or no MapPath."),
			*RoomDef->RoomId.ToString()));
		return false;
	}

	// Menu -> Loading: from here until the open resolves, no further enter is
	// accepted (the bLoading window).
	bLoading = true;
	CurrentRoomDef = RoomDef;
	TransitionTo(EGameFlowState::Loading);

	const bool bOpenOk = MapOpener
		? MapOpener(MapPath.ToString())
		: OpenRoomMapProduction(MapPath.ToString());
	if (!bOpenOk)
	{
		// Failed load: back to Menu with the error recorded. The profile is a
		// GameInstance-level subsystem and is untouched by construction; the
		// menu is immediately retryable (the card's acceptance item).
		bLoading = false;
		CurrentRoomDef = nullptr;
		RecordError(FString::Printf(TEXT("M3-017: the room map '%s' failed to open; returned to the menu."),
			*MapPath.ToString()));
		TransitionTo(EGameFlowState::Menu);
		UE_LOG(LogTemp, Warning, TEXT("UEMMO M3-017: the room map '%s' failed to open; the flow returned to the menu."),
			*MapPath.ToString());
		return false;
	}

	bLoading = false;
	TransitionTo(EGameFlowState::Room);
	LastError.Reset();
	UE_LOG(LogTemp, Log, TEXT("UEMMO M3-017: entered room '%s' via '%s'."),
		*RoomDef->RoomId.ToString(), *MapPath.ToString());
	return true;
}

bool UGameFlowSubsystem::ReturnToMenu()
{
	if (State != EGameFlowState::Room && State != EGameFlowState::Result)
	{
		RecordError(FString::Printf(TEXT("M3-017: ReturnToMenu refused in state %d (only Room and Result can leave)."),
			static_cast<int32>(State)));
		return false;
	}

	// The M2-011 leave semantics run against the CURRENT world's room session:
	// cancels the still-pending wave births, drops the player-death binding
	// and cleans the run scope; idempotent, and a no-op from Idle. This is
	// deliberately NOT a world switch and NOT a profile event: the menu map
	// (and the widget re-presentation) belongs to the later menu wiring task,
	// and the GameInstance-level profile survives by construction.
	if (UGameInstance* GameInstance = GetGameInstance())
	{
		if (UWorld* World = GameInstance->GetWorld())
		{
			if (URoomSessionSubsystem* RoomSession = World->GetSubsystem<URoomSessionSubsystem>())
			{
				RoomSession->LeaveRoom();
			}
		}
	}

	CurrentRoomDef = nullptr;
	TransitionTo(EGameFlowState::Menu);
	return true;
}

bool UGameFlowSubsystem::NotifyRunEnded()
{
	if (State != EGameFlowState::Room)
	{
		RecordError(FString::Printf(TEXT("M3-017: NotifyRunEnded refused in state %d (only a room run ends into the result surface)."),
			static_cast<int32>(State)));
		return false;
	}
	TransitionTo(EGameFlowState::Result);
	return true;
}

URoomDefinition* UGameFlowSubsystem::GetSelectableRoomDefinition()
{
	if (CachedSelectableRoom == nullptr)
	{
		// The card's single selectable map: the Data/rooms.json
		// room_training_01 identity as a transient definition double (the
		// M2-007+ runtime-definition precedent). Only the identity fields
		// matter for the map open; the wave content stays with the room
		// session's existing chain (documented in the task report).
		URoomDefinition* Room = NewObject<URoomDefinition>(this, NAME_None, RF_Transient);
		Room->RoomId = FName(TEXT("room_training_01"));
		Room->MapPath = TSoftObjectPtr<UWorld>(FSoftObjectPath(GM3_017_TrainingArenaMapPath));
		Room->RewardTableId = FName(TEXT("starter"));
		CachedSelectableRoom = Room;
	}
	return CachedSelectableRoom;
}

URoomDefinition* UGameFlowSubsystem::GetCombatRoomDefinition()
{
	if (CachedCombatRoom == nullptr)
	{
		// The M3-030 mount's enter target: the wave-combat room the boot
		// world's trigger chain runs (see the header comment). Only the
		// identity fields matter for the map open; the wave content stays
		// with the ARoomTrigger's existing fallback chain (documented in the
		// task report).
		URoomDefinition* Room = NewObject<URoomDefinition>(this, NAME_None, RF_Transient);
		Room->RoomId = FName(GM3_030_CombatRoomId);
		Room->MapPath = TSoftObjectPtr<UWorld>(FSoftObjectPath(GM3_030_CombatRoomMapPath));
		Room->RewardTableId = FName(GM3_030_CombatRoomRewardTableId);
		CachedCombatRoom = Room;
	}
	return CachedCombatRoom;
}

URoomDefinition* UGameFlowSubsystem::GetSystemTestRoomDefinition()
{
	if (CachedSystemTestRoom == nullptr)
	{
		// The M5-018A menu's second legal entry: the config-driven test room
		// the driver actor on L_SystemTestRoom runs. Only the identity fields
		// matter for the map open; the room content stays with the map's
		// driver actor and its source table (documented in the task report).
		URoomDefinition* Room = NewObject<URoomDefinition>(this, NAME_None, RF_Transient);
		Room->RoomId = FName(GM5_018A_SystemTestRoomId);
		Room->MapPath = TSoftObjectPtr<UWorld>(FSoftObjectPath(GM5_018A_SystemTestRoomMapPath));
		Room->RewardTableId = FName(GM5_018A_SystemTestRoomRewardTableId);
		CachedSystemTestRoom = Room;
	}
	return CachedSystemTestRoom;
}

bool UGameFlowSubsystem::OpenRoomMapProduction(const FString& MapPath)
{
	if (MapPath.IsEmpty())
	{
		return false;
	}
	UGameInstance* GameInstance = GetGameInstance();
	if (GameInstance == nullptr)
	{
		return false;
	}
	// The resource-path guard: refuse a package that does not exist BEFORE any
	// travel is issued, so a bad map path reads as the retryable "failed open"
	// (error line, menu state) instead of a silent stay on the current world.
	if (!FPackageName::DoesPackageExist(MapPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("UEMMO M3-017: the room map package '%s' does not exist."), *MapPath);
		return false;
	}
	// The real absolute travel. Its asynchronous completion (and any
	// streaming-time failure) is not observable here - the real open effect is
	// the user's H01 check (documented in the task report).
	UGameplayStatics::OpenLevelBySoftObjectPtr(GameInstance,
		TSoftObjectPtr<UWorld>(FSoftObjectPath(MapPath)), /*bAbsolute*/ true);
	return true;
}

void UGameFlowSubsystem::TransitionTo(EGameFlowState NewState)
{
	const EGameFlowState OldState = State;
	State = NewState;
	StateHistory.Add(NewState);
	UE_LOG(LogTemp, Verbose, TEXT("UEMMO M3-017: game flow %d -> %d."),
		static_cast<int32>(OldState), static_cast<int32>(NewState));
}

void UGameFlowSubsystem::RecordError(const FString& Message)
{
	LastError = Message;
}
