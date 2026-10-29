// M6-004: the packaged room-chain smoke for the second combat room. Two
// suites:
//
//   1. NewRoomPackagedArtifacts (no travel) - the production artifacts of the
//      new level travel and wire correctly: the M6-001 catalog carries the
//      room_combat_02 row (identity + 3+3 waves), the cloned
//      L_CombatRoom02 map package exists and its single RoomTrigger
//      references the row's OWN generated DA_room_combat_02.
//
//   2. NewRoomRealTravelChain (ClientContext; the real chain) - the REAL
//      production faces in the real game: the flow enters the new room
//      (a true OpenLevel travel - no injected opener), the traveled world is
//      L_CombatRoom02 with an active room session whose wave started, the
//      production MarkCleared settles the run Cleared with a settlement id,
//      and ReturnToMenu travels back with the flow in the Menu state.
//
// Harness: the suites run inside the packaged game's boot world (-game). The
// headless editor runs execute suite 1 (no travel needed) and skip suite 2
// silently when there is no real game world to travel from (the same
// ClientContext companion shape as M5-045's capture).

#include "Misc/AutomationTest.h"

#include "../PrototypeCharacter.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Profile/ProfileSubsystem.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../Room/RoomTrigger.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M6_004
{
	/** The new row's identity (mirrors the Data/rooms.json row). */
	const TCHAR* GM6_004_NewRoomId = TEXT("room_combat_02");
	const TCHAR* GM6_004_NewMapPackage = TEXT("/Game/UEMMO/Maps/L_CombatRoom02");
	const TCHAR* GM6_004_NewAssetObjectPath = TEXT("/Game/UEMMO/Rooms/DA_room_combat_02.DA_room_combat_02");

	/** Finds the catalog entry of the new room (null when absent). */
	const URoomDefinition* FindNewRoomDefinition(UGameFlowSubsystem& Flow)
	{
		for (const URoomDefinition* Entry : Flow.GetSelectableRoomDefinitions())
		{
			if (Entry != nullptr && Entry->RoomId == FName(GM6_004_NewRoomId))
			{
				return Entry;
			}
		}
		return nullptr;
	}
}

// M6-005: the first-boot surface - the game boots into the dedicated menu
// map (the walkable shell with no combat orchestration; the menu overlay is
// the flow's Menu-state surface on top of it).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM6_005BootMapIsMainMenu,
	"UEMMO.Tasks.M6_005.BootMapIsMainMenu",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_005BootMapIsMainMenu::RunTest(const FString& Parameters)
{
	UWorld* PlayWorld = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
	if (!TestNotNull(TEXT("the play world resolves"), PlayWorld))
	{
		return true;
	}
	TestTrue(TEXT("the game boots into the dedicated menu map"),
		PlayWorld->GetMapName().Contains(TEXT("L_MainMenu")));
	// The menu surface carries no combat orchestration: a fresh session
	// subsystem in the boot world must hold no room assets (the map itself
	// has no trigger/exit - verified on disk by the clone's verify pass).
	URoomSessionSubsystem* Session = PlayWorld->GetSubsystem<URoomSessionSubsystem>();
	TestTrue(TEXT("the menu boot world starts with an idle session"),
		Session != nullptr && Session->GetState() == ERoomSessionState::Idle);
	return true;
}

// Suite 1: the packaged artifacts - catalog row + cloned map wiring.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM6_004NewRoomPackagedArtifacts,
	"UEMMO.Tasks.M6_004.NewRoomPackagedArtifacts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_004NewRoomPackagedArtifacts::RunTest(const FString& Parameters)
{
	UWorld* PlayWorld = GEngine ? GEngine->GetCurrentPlayWorld() : nullptr;
	UGameInstance* GameInstance = PlayWorld ? PlayWorld->GetGameInstance() : nullptr;
	UGameFlowSubsystem* Flow = GameInstance ? GameInstance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("the game flow subsystem resolves"), Flow))
	{
		return true;
	}

	// The catalog row travels and carries the new row's identity.
	const URoomDefinition* NewRoom = UE::UEMMO::Tasks::M6_004::FindNewRoomDefinition(*Flow);
	if (!TestNotNull(TEXT("the catalog carries the room_combat_02 row"), NewRoom))
	{
		return true;
	}
	TestEqual(TEXT("the new row's map package is the clone"),
		NewRoom->MapPath.ToSoftObjectPath().GetLongPackageName(),
		FString(UE::UEMMO::Tasks::M6_004::GM6_004_NewMapPackage));
	TestEqual(TEXT("the new row carries its display name"),
		NewRoom->DisplayName, FString(TEXT("CombatRoom02")));
	TestEqual(TEXT("the new row carries two waves"), NewRoom->Waves.Num(), 2);
	if (NewRoom->Waves.Num() == 2)
	{
		TestEqual(TEXT("wave 0 spawns three grunts"), NewRoom->Waves[0].Count, 3);
		TestEqual(TEXT("wave 1 spawns three grunts"), NewRoom->Waves[1].Count, 3);
	}

	// The cloned map travels and its single trigger references the row's OWN
	// generated asset (the PrepareRooms per-row wiring, reloaded from disk).
	UWorld* ClonedMap = LoadObject<UWorld>(nullptr, UE::UEMMO::Tasks::M6_004::GM6_004_NewMapPackage);
	if (!TestNotNull(TEXT("the cloned map package loads"), ClonedMap))
	{
		return true;
	}
	if (!TestTrue(TEXT("the cloned map carries a persistent level"), ClonedMap->PersistentLevel != nullptr))
	{
		return true;
	}
	ARoomTrigger* Trigger = nullptr;
	for (AActor* Actor : ClonedMap->PersistentLevel->Actors)
	{
		if (Actor != nullptr && Actor->IsA<ARoomTrigger>())
		{
			Trigger = Cast<ARoomTrigger>(Actor);
			break;
		}
	}
	if (!TestNotNull(TEXT("the cloned map carries its RoomTrigger"), Trigger))
	{
		return true;
	}
	TestEqual(TEXT("the cloned trigger references the row's own asset"),
		Trigger->GetRoomDefinitionSource().ToSoftObjectPath().ToString(),
		FString(UE::UEMMO::Tasks::M6_004::GM6_004_NewAssetObjectPath));
	return true;
}

// Suite 2: the real travel chain (ClientContext only; headless skips).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM6_004NewRoomRealTravelChain,
	"UEMMO.Tasks.M6_004.NewRoomRealTravelChain",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_004NewRoomRealTravelChain::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	UGameFlowSubsystem* Flow = GameInstance ? GameInstance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
	if (World == nullptr || World->WorldType != EWorldType::Game || Flow == nullptr
		|| Flow->GetState() != EGameFlowState::Menu)
	{
		AddInfo(TEXT("real travel chain skipped: no real game world in the Menu state"));
		return true;
	}
	const URoomDefinition* NewRoom = UE::UEMMO::Tasks::M6_004::FindNewRoomDefinition(*Flow);
	if (!TestNotNull(TEXT("the catalog carries the room_combat_02 row"), NewRoom))
	{
		return true;
	}

	// THE ENTER (the real production face): the flow accepts the row and the
	// REAL OpenLevelBySoftObjectPtr travel issues (no injected opener - the
	// packaged game travels for real).
	if (!TestTrue(TEXT("the flow accepted the new room's enter"), Flow->EnterRoom(NewRoom)))
	{
		return true;
	}

	// The travel lands: the new world is the clone and the flow is in the
	// Room state (the run itself starts on the trigger region walk-in below).
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(3.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow]()
	{
		UWorld* Traveled = GWorld;
		if (!TestNotNull(TEXT("the traveled world exists"), Traveled))
		{
			return true;
		}
		TestTrue(TEXT("the travel landed on the cloned map"),
			Traveled->GetMapName().Contains(TEXT("L_CombatRoom02")));
		TestTrue(TEXT("the flow is in the Room state after the travel"),
			Flow->GetState() == EGameFlowState::Room);
		TestNotNull(TEXT("the traveled world owns a room session"),
			Traveled->GetSubsystem<URoomSessionSubsystem>());
		return true;
	}));

	// THE WALK-IN + THE VICTORY (the production faces): the traveled room
	// activates through the trigger's activation region, so the pawn walks
	// forward until the session starts (a real PlayerEnteredRegion -> wave 0)
	// and MarkCleared settles the run immediately - the same face the
	// wave-cleared rule routes into, before the spawned grunts can reach the
	// pawn. Returning false keeps the latent ticking per frame (the walk).
	TSharedPtr<int32> WalkFrames = MakeShared<int32>(0);
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow, WalkFrames]()
	{
		UWorld* Traveled = GWorld;
		URoomSessionSubsystem* Session = Traveled != nullptr
			? Traveled->GetSubsystem<URoomSessionSubsystem>() : nullptr;
		if (Session == nullptr)
		{
			AddError(TEXT("the walk-in lost the room session"));
			return true;
		}
		if (Session->GetState() == ERoomSessionState::Running)
		{
			// THE ACTIVATION landed: settle the run Cleared at once.
			TestTrue(TEXT("the traveled run names the new room"),
				Session->GetRoomId() == FName(TEXT("room_combat_02")));
			TestTrue(TEXT("wave 0 started in the traveled room"), Session->GetCurrentWaveIndex() >= 0);
			TestTrue(TEXT("the production MarkCleared accepts"), Session->MarkCleared());
			if (APrototypeCharacter* Pawn = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0)))
			{
				Pawn->RecordPlanarMoveInput(0.0f, 0.0f);
			}
			return true;
		}
		// The activation region lies ahead of the spawn: walk straight in.
		if (APrototypeCharacter* Pawn = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0)))
		{
			Pawn->RecordPlanarMoveInput(1.0f, 0.0f);
		}
		++(*WalkFrames);
		if (*WalkFrames > 3000)
		{
			AddError(FString::Printf(TEXT("the walk-in never activated the room session (state=%d after 3000 walk frames)"),
				static_cast<int32>(Session->GetState())));
			if (APrototypeCharacter* Pawn = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(GWorld, 0)))
			{
				Pawn->RecordPlanarMoveInput(0.0f, 0.0f);
			}
			return true;
		}
		return false;
	}));

	// The settled outcome: Cleared with the one-time settlement identity.
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this]()
	{
		UWorld* Traveled = GWorld;
		URoomSessionSubsystem* Session = Traveled != nullptr
			? Traveled->GetSubsystem<URoomSessionSubsystem>() : nullptr;
		if (!TestNotNull(TEXT("the session persists for the settle"), Session))
		{
			return true;
		}
		TestTrue(TEXT("the session settled into the Cleared state"),
			Session->GetState() == ERoomSessionState::Cleared);
		const FRoomResult& Result = Session->GetLastResult();
		TestTrue(TEXT("the settled run result is Cleared"), Result.bCleared);
		TestTrue(TEXT("the settled run carries a settlement id"), Result.SettlementId != 0);
		TestTrue(TEXT("the settled run names the new room"),
			Result.RoomId == FName(TEXT("room_combat_02")));
		return true;
	}));

	// THE RETURN: the flow travels back and lands in the Menu state (the
	// free-select list re-presents on the boot world).
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow]()
	{
		UWorld* Traveled = GWorld;
		if (!TestNotNull(TEXT("the world exists for the return"), Traveled))
		{
			return true;
		}
		TestTrue(TEXT("the production ReturnToMenu accepts"), Flow->ReturnToMenu());
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(3.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow]()
	{
		UWorld* Back = GWorld;
		TestTrue(TEXT("the return traveled back (a real world exists)"), Back != nullptr);
		TestTrue(TEXT("the flow is back in the Menu state"),
			Flow->GetState() == EGameFlowState::Menu);
		return true;
	}));
	return true;
}

// Suite 3: the rendered capture companion (ClientContext only; headless
// skips). Two screenshots: the boot menu (the free-select list now carries
// the three levels) and the traveled new room's live world.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM6_004NewRoomRenderCapture,
	"UEMMO.Tasks.M6_004.NewRoomRenderCapture",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_004NewRoomRenderCapture::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	UGameFlowSubsystem* Flow = GameInstance ? GameInstance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
	if (World == nullptr || GEngine == nullptr || GEngine->GameViewport == nullptr
		|| World->WorldType != EWorldType::Game || Flow == nullptr
		|| Flow->GetState() != EGameFlowState::Menu)
	{
		AddInfo(TEXT("new room render capture skipped: no rendered game world in the Menu state"));
		return true;
	}

	const FString Directory = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M6-004") / TEXT("scenario-json") / TEXT("render"));
	IFileManager::Get().MakeDirectory(*Directory, /*Tree*/ true);

	// Screenshot 1: the boot menu (the free-select list carries 3 levels).
	// The packaged render pass boots slower than the editor (shader compile)
	// - the wait must clear the overlay's presentation. The flush wait after
	// the request matters: a queued screenshot lands on a LATER frame, and a
	// travel before that frame would capture the destination world instead.
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(4.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Directory]()
	{
		if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
		{
			return true;
		}
		FScreenshotRequest::RequestScreenshot(Directory / TEXT("capture-menu-3-rooms.png"), true, false);
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));

	// Travel to the new room (the real production enter), then screenshot the
	// live cloned world.
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow]()
	{
		const URoomDefinition* NewRoom = UE::UEMMO::Tasks::M6_004::FindNewRoomDefinition(*Flow);
		if (!TestNotNull(TEXT("the capture travel finds the new row"), NewRoom))
		{
			return true;
		}
		TestTrue(TEXT("the capture travel accepted the enter"), Flow->EnterRoom(NewRoom));
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(4.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([Directory]()
	{
		if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
		{
			return true;
		}
		FScreenshotRequest::RequestScreenshot(Directory / TEXT("capture-new-room.png"), true, false);
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));

	// Return for a clean exit state.
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Flow]()
	{
		TestTrue(TEXT("the capture's return accepted"), Flow->ReturnToMenu());
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([this, Directory]()
	{
		if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
		{
			return true;
		}
		for (const TCHAR* Name : {TEXT("capture-menu-3-rooms.png"), TEXT("capture-new-room.png")})
		{
			const FString CaptureFile = Directory / Name;
			TestTrue(FString::Printf(TEXT("the capture was written: %s"), *CaptureFile),
				IFileManager::Get().FileExists(*CaptureFile));
		}
		return true;
	}));
	return true;
}

#endif
