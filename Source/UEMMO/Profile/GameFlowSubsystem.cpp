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
	StartupOutcome = FStartupLoadOutcome();
	bStartupProfileRestored = false;

	// M3-015 chain: the profile subsystem is an explicit collection dependency
	// so the startup restore below always finds it initialized.
	UProfileSubsystem* Profile = Collection.InitializeDependency<UProfileSubsystem>();

	// A parked automation service wins; production builds its own service on
	// the real prefix. Automation WITHOUT an injected service skips the real
	// pass entirely: machine save state must never leak into tests (and the
	// M3-003 "fresh instance has no profile" guarantee must hold there).
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
		// NoSaveFound: the legal fresh state; creating a profile stays the
		// caller's explicit decision (never automatic).
	}

	if (!bRanStartupPass)
	{
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
