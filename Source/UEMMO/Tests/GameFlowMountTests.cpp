// M3-030: the boot menu mount and the full game-flow loop. The suite locks
// the card's behaviors on the PRODUCTION HUD (the M3-017 flow machine behind
// it is already locked by the GameFlowTests suite):
//
//   1. The boot world presents the map select overlay: the flow starts in the
//      menu state at GameInstance init and the HUD's BeginPlay presents the
//      M3-017 MapSelectWidget exactly in that state (the card's "boot into
//      menu" - the overlay form, no dedicated menu map).
//   2. The mounted menu's enter click goes through the production enter path:
//      the flow's EnterRoom opens the WAVE-COMBAT ROOM (room_combat_01 /
//      L_CombatRoom01 - the boot world whose trigger chain starts the run),
//      exactly one world switch per click (the bLoading guard), and the
//      accepted enter dismisses the overlay (the room world owns the screen).
//   3. The full loop Menu -> Enter -> Room -> Cleared -> Result -> Menu closes
//      through the real HUD chain: the terminal OnRunEnded moves the flow to
//      the result state (the existing result screen presents), the Return
//      button returns through the flow (Result -> Menu, the M2-011 LeaveRoom
//      semantics inside) and the overlay re-presents.
//   4. The defeat path: the real player death fails the run, the result screen
//      shows Defeat, and the return closes the loop identically.
//   5. The GameInstance-level profile survives the whole loop untouched (the
//      M3-017 semantics observed through the mount).
//   6. A fast double-click on the mounted menu produces exactly ONE world
//      switch, and the re-presented menu is immediately retryable.
//
// Harness: the FTestWorldWrapper precedent (a real BeginPlay-initialized temp
// game world whose game instance owns every registered UGameInstanceSubsystem,
// the flow included) + the M3-017 counting map-opener simulator (a temporary
// test world cannot truly travel; the production OpenLevel path stays the
// user's H01 check). The run staging reuses the M2-012 debug staging chain
// (the session's own production terminal entries), and the defeat staging
// reuses the M2-010/M2-012 real player-death path. The capture companion
// (GameFlowStateScreenshots) runs only where a real game world with a
// viewport exists (the offscreen -game capture runs; headless automation
// skips it gracefully).
//
// Red/green note: against the pre-mount HUD (no menu presentation, no enter
// caller, no flow notification on the HUD chain) every suite fails on its
// concrete first assertion - the boot menu is absent and the loop cannot
// close. Exact red/green counts live in the task report.

#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/HealthComponent.h"
#include "../Enemy/EnemyDefinition.h"
#include "../Profile/GameFlowSubsystem.h"
#include "../Profile/ProfileSubsystem.h"
#include "../PrototypeCharacter.h"
#include "../PrototypeHUD.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../UI/MapSelectWidget.h"

#include "Components/TextBlock.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "../Items/InventoryModel.h"
#include "../Items/ItemInstance.h"
#include "Kismet/GameplayStatics.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M3_030
{
	// The production maps of the mounted loop: the boot/wave-combat world the
	// menu's enter opens (the trigger chain starts the run there) and the
	// M3-017 training-map identity the raw selectable entry carries (the mount
	// must NOT open it - the training map carries no wave trigger).
	const TCHAR* M3_030_CombatRoomMapPath = TEXT("/Game/UEMMO/Maps/L_CombatRoom01");
	const TCHAR* M3_030_CombatRoomId = TEXT("room_combat_01");

	// Deterministic item instance (the M3-003/M3-017 fixture style, M3_030 ids).
	static FItemInstance M3_030_MakeTestInstance(uint32 Seed)
	{
		FItemInstance Instance;
		Instance.InstanceId = FGuid(0x03300000u + Seed, 0x0BADu, Seed + 30u, Seed * 5u + 3u);
		Instance.DefinitionId = FName(TEXT("weapon_training"));
		Instance.RollSeed = static_cast<int64>(0x30 + Seed);
		Instance.Level = 1;
		return Instance;
	}

	// Transient room double for the session staging (one one-enemy wave; the
	// M2-012 debug staging precedent - the flow's enter definition is separate
	// and comes from the flow itself).
	static URoomDefinition* M3_030_MakeStageRoom(const FVector& BaseLocation)
	{
		URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Room->RoomId = FName(TEXT("room_m3_030_stage"));
		Room->RewardTableId = FName(TEXT("starter"));
		FRoomWaveDefinition Wave;
		Wave.EnemyId = FName(TEXT("melee_grunt"));
		Wave.Count = 1;
		Wave.SpawnLocations.Add(BaseLocation + FVector(420.0, 0.0, 88.0));
		Room->Waves.Add(Wave);
		return Room;
	}

	// Transient enemy double of the Data/enemies.json melee_grunt row.
	static UEnemyDefinition* M3_030_MakeStageEnemy()
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

	// Spawns the real prototype pawn (the M2-004 rig: its own health pool from
	// spawn; the death test drives the lethal path through the public API).
	static APrototypeCharacter* M3_030_SpawnPlayer(UWorld& World)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(), FVector(100.0, 0.0, 120.0), FRotator::ZeroRotator, Params);
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

	/**
	 * One temp game world with the production subsystems (the wrapper's game
	 * instance owns the flow and the profile), the real prototype pawn, the
	 * real HUD under test and the M3-017 counting map opener. Build() leaves
	 * the scene at the BOOT surface: flow Menu, the mount presenting (or, red,
	 * failing to present) the menu overlay, a prepared profile and the session
	 * holding the pawn.
	 */
	struct FM3_030_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		URoomSessionSubsystem* Session = nullptr;
		UGameFlowSubsystem* Flow = nullptr;
		UProfileSubsystem* Profile = nullptr;
		APrototypeCharacter* Player = nullptr;
		APrototypeHUD* Hud = nullptr;
		FGuid CharacterId;

		// The counting opener: every accepted enter request lands here once.
		int32 OpenCount = 0;
		FString LastOpenedPath;

		bool Build(FAutomationTestBase& Test)
		{
			if (!Test.TestTrue(TEXT("the test world is created (engine FTestWorldWrapper precedent)"),
				Wrapper.CreateTestWorld(EWorldType::Game)))
			{
				return false;
			}
			World = Wrapper.GetTestWorld();
			if (!Test.TestNotNull(TEXT("the test world is available"), World)
				|| !Test.TestTrue(TEXT("play begins in the test world (full actor initialization)"),
					Wrapper.BeginPlayInTestWorld()))
			{
				return false;
			}
			Session = World->GetSubsystem<URoomSessionSubsystem>();
			if (!Test.TestNotNull(TEXT("the room session subsystem exists in the test world"), Session))
			{
				return false;
			}
			UGameInstance* GameInstance = World->GetGameInstance();
			Flow = GameInstance ? GameInstance->GetSubsystem<UGameFlowSubsystem>() : nullptr;
			Profile = GameInstance ? GameInstance->GetSubsystem<UProfileSubsystem>() : nullptr;
			if (!Test.TestNotNull(TEXT("the game flow subsystem exists (M3-017)"), Flow)
				|| !Test.TestNotNull(TEXT("the profile subsystem exists"), Profile))
			{
				return false;
			}

			// The counting opener (the M3-017 simulator): a temporary world
			// cannot truly travel; the requested path is the assertion surface.
			Flow->SetMapOpenerForTests([this](const FString& MapPath)
			{
				++OpenCount;
				LastOpenedPath = MapPath;
				return true;
			});

			// A non-trivial profile the loop must preserve (the M3-017 cycle
			// fixture shape: identity, level 2 / 50 XP, one packed item).
			Profile->NewProfile();
			CharacterId = Profile->GetCharacterId();
			if (!Test.TestTrue(TEXT("the scene profile gains a level"), Profile->AddXP(150)))
			{
				return false;
			}
			if (Profile->GetInventory().TryAdd(M3_030_MakeTestInstance(0x71u)) != EInventoryAddResult::Added)
			{
				Test.AddError(TEXT("setup: the scene profile rejected the packed item"));
				return false;
			}

			Player = M3_030_SpawnPlayer(*World);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			Session->SetPlayer(Player);

			// The HUD under test: spawned like the game mode spawns it; its
			// BeginPlay runs the mount presentation for the boot surface.
			FActorSpawnParameters HudParams;
			Hud = World->SpawnActor<APrototypeHUD>(APrototypeHUD::StaticClass(),
				FVector::ZeroVector, FRotator::ZeroRotator, HudParams);
			if (!Test.TestNotNull(TEXT("the prototype HUD spawns in the test world"), Hud))
			{
				return false;
			}
			return true;
		}

		/** The mounted menu's real click path (null-safe; the mount owns it). */
		bool ClickMenuEnter(FAutomationTestBase& Test)
		{
			UMapSelectWidget* Menu = Hud ? Hud->PeekMenuWidget() : nullptr;
			if (!Test.TestNotNull(TEXT("the mounted map select widget is reachable"), Menu))
			{
				return false;
			}
			Menu->HandleEnterClicked();
			return true;
		}

		/** Asserts the profile survived the loop exactly as built. */
		void ExpectProfileIntact(FAutomationTestBase& Test, const TCHAR* What)
		{
			Test.TestTrue(FString::Printf(TEXT("%s: the profile still exists"), What), Profile->HasProfile());
			Test.TestTrue(FString::Printf(TEXT("%s: the CharacterId survived"), What),
				Profile->GetCharacterId() == CharacterId);
			Test.TestEqual(FString::Printf(TEXT("%s: the level survived"), What), Profile->GetLevel(), 2);
			Test.TestEqual(FString::Printf(TEXT("%s: the XP survived"), What), Profile->GetXP(), 50);
			Test.TestEqual(FString::Printf(TEXT("%s: the packed item survived"), What),
				Profile->GetInventory().Count(), 1);
		}
	};
}

using namespace UE::UEMMO::Tasks::M3_030;

// 1. Acceptance: the boot world presents the menu overlay, and the mounted
//    menu's enter click drives the production enter path against the
//    wave-combat room (exactly one switch, the overlay dismisses).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_030BootMenuPresentsAndEntersCombatRoom,
	"UEMMO.Tasks.M3_030.BootMenuPresentsAndEntersCombatRoom",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_030BootMenuPresentsAndEntersCombatRoom::RunTest(const FString& Parameters)
{
	FM3_030_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}

	// Boot surface: the fresh flow sits in the menu state and the mount
	// presents the map select overlay for it.
	TestTrue(TEXT("a fresh flow starts in the menu state"),
		Scene.Flow->GetState() == EGameFlowState::Menu);
	TestTrue(TEXT("the boot HUD presents the menu overlay"),
		Scene.Hud->HasMenuScreen());
	UMapSelectWidget* Menu = Scene.Hud->PeekMenuWidget();
	if (!TestNotNull(TEXT("the mounted map select widget exists"), Menu))
	{
		return true;
	}
	if (Menu->PeekEnterButton() != nullptr)
	{
		TestTrue(TEXT("the mounted menu's enter button is armed"),
			Menu->PeekEnterButton()->GetIsEnabled());
	}

	// The real click path: exactly one world switch, requested for the
	// wave-combat room (the boot world's trigger chain), NOT the training-map
	// identity of the raw selectable entry.
	if (!Scene.ClickMenuEnter(*this))
	{
		return true;
	}
	TestEqual(TEXT("the mounted menu click requested exactly one world switch"), Scene.OpenCount, 1);
	TestEqual(TEXT("the mounted enter opened the wave-combat room map"),
		Scene.LastOpenedPath, FString(M3_030_CombatRoomMapPath));
	TestTrue(TEXT("the flow entered the room state through the mounted menu"),
		Scene.Flow->GetState() == EGameFlowState::Room);
	const URoomDefinition* Entered = Scene.Flow->GetCurrentRoomDefinition();
	TestNotNull(TEXT("the flow carries the entered room definition"), Entered);
	if (Entered != nullptr)
	{
		TestEqual(TEXT("the entered room is the combat room identity"),
			Entered->RoomId.ToString(), FString(M3_030_CombatRoomId));
	}

	// The accepted enter dismisses the overlay (the room world owns the
	// screen; the new world's HUD would present nothing for the room state).
	TestFalse(TEXT("the accepted enter dismissed the menu overlay"), Scene.Hud->HasMenuScreen());

	// The duplicate click (the room is active) requests nothing further.
	Menu->HandleEnterClicked();
	TestEqual(TEXT("the duplicate click requested no further switch"), Scene.OpenCount, 1);

	Scene.ExpectProfileIntact(*this, TEXT("after the enter"));
	return true;
}

// 2. Acceptance: the full loop Menu -> Enter -> Room -> Cleared -> Result ->
//    Menu closes through the real HUD chain, and the profile survives.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_030FullLoopMenuEnterClearedResultMenu,
	"UEMMO.Tasks.M3_030.FullLoopMenuEnterClearedResultMenu",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_030FullLoopMenuEnterClearedResultMenu::RunTest(const FString& Parameters)
{
	FM3_030_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}

	// Menu -> Enter -> Room (the mounted menu's real click path).
	TestTrue(TEXT("the boot HUD presents the menu overlay"), Scene.Hud->HasMenuScreen());
	if (!Scene.ClickMenuEnter(*this))
	{
		return true;
	}
	if (!TestTrue(TEXT("the flow reached the room state"), Scene.Flow->GetState() == EGameFlowState::Room))
	{
		return true;
	}
	TestFalse(TEXT("the menu overlay is dismissed in the room state"), Scene.Hud->HasMenuScreen());

	// The run runs on the session's real chain (the M2-012 debug staging
	// shape: the session's own production entries; nothing is faked).
	Scene.Session->SetSessionClockSeconds(Scene.World->GetTimeSeconds());
	URoomDefinition* StageRoom = M3_030_MakeStageRoom(Scene.Player->GetActorLocation());
	UEnemyDefinition* StageEnemy = M3_030_MakeStageEnemy();
	if (!TestTrue(TEXT("the staged run starts"), Scene.Session->StartRoom(StageRoom))
		|| !TestTrue(TEXT("the staged progression begins"), Scene.Session->BeginWaves(StageRoom, StageEnemy)))
	{
		return true;
	}

	// Cleared -> Result through the real OnRunEnded broadcast reaching the
	// real HUD: the flow moves to the result state and the result screen
	// presents (the existing M2-012 chain, now flow-aware).
	if (!TestTrue(TEXT("the staged run clears through the real MarkCleared"), Scene.Session->MarkCleared()))
	{
		return true;
	}
	TestTrue(TEXT("the flow moved to the result state on the cleared run"),
		Scene.Flow->GetState() == EGameFlowState::Result);
	TestTrue(TEXT("the victory screen presented"), Scene.Hud->HasRoomResultScreen());
	TestEqual(TEXT("the result headline is the Victory truth"),
		Scene.Hud->PeekRoomResultViewModel().HeadlineText, FString(TEXT("Victory")));
	TestFalse(TEXT("the menu overlay stays hidden in the result state"), Scene.Hud->HasMenuScreen());

	// Return -> Menu through the real request path: the flow returns (the
	// M2-011 LeaveRoom semantics run inside), the screen dismisses and the
	// overlay re-presents (the loop closes; the next enter reloads the room).
	Scene.Hud->HandleReturnRequested();
	TestTrue(TEXT("the return moved the flow back to the menu state"),
		Scene.Flow->GetState() == EGameFlowState::Menu);
	TestFalse(TEXT("the return dismissed the result screen"), Scene.Hud->HasRoomResultScreen());
	TestTrue(TEXT("the return re-presented the menu overlay"), Scene.Hud->HasMenuScreen());
	TestTrue(TEXT("the room session left the run scope (Exiting)"),
		Scene.Session->GetState() == ERoomSessionState::Exiting);

	// The full accepted history reads Menu, Loading, Room, Result, Menu.
	const TArray<EGameFlowState>& History = Scene.Flow->GetStateHistory();
	TestEqual(TEXT("the full-loop history holds exactly 5 accepted entries"), History.Num(), 5);
	if (History.Num() == 5)
	{
		TestEqual(TEXT("history[0] is Menu"), static_cast<int32>(History[0]), static_cast<int32>(EGameFlowState::Menu));
		TestEqual(TEXT("history[1] is Loading"), static_cast<int32>(History[1]), static_cast<int32>(EGameFlowState::Loading));
		TestEqual(TEXT("history[2] is Room"), static_cast<int32>(History[2]), static_cast<int32>(EGameFlowState::Room));
		TestEqual(TEXT("history[3] is Result"), static_cast<int32>(History[3]), static_cast<int32>(EGameFlowState::Result));
		TestEqual(TEXT("history[4] is Menu"), static_cast<int32>(History[4]), static_cast<int32>(EGameFlowState::Menu));
	}

	Scene.ExpectProfileIntact(*this, TEXT("after the full loop"));
	return true;
}

// 3. Acceptance: the defeat path - the real player death fails the run, the
//    result screen shows Defeat, and the return closes the loop identically.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_030PlayerDeathResultDefeatAndReturnToMenu,
	"UEMMO.Tasks.M3_030.PlayerDeathResultDefeatAndReturnToMenu",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_030PlayerDeathResultDefeatAndReturnToMenu::RunTest(const FString& Parameters)
{
	FM3_030_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}

	// Menu -> Enter -> Room.
	TestTrue(TEXT("the boot HUD presents the menu overlay"), Scene.Hud->HasMenuScreen());
	if (!Scene.ClickMenuEnter(*this))
	{
		return true;
	}
	if (!TestTrue(TEXT("the flow reached the room state"), Scene.Flow->GetState() == EGameFlowState::Room))
	{
		return true;
	}

	// The run runs, and the real player death fails it (the M2-010 lethal
	// path: the pawn's health pool -> PlayerDied -> the session's FailRun).
	Scene.Session->SetSessionClockSeconds(Scene.World->GetTimeSeconds());
	URoomDefinition* StageRoom = M3_030_MakeStageRoom(Scene.Player->GetActorLocation());
	UEnemyDefinition* StageEnemy = M3_030_MakeStageEnemy();
	if (!TestTrue(TEXT("the staged run starts"), Scene.Session->StartRoom(StageRoom))
		|| !TestTrue(TEXT("the staged progression begins"), Scene.Session->BeginWaves(StageRoom, StageEnemy)))
	{
		return true;
	}
	if (!TestTrue(TEXT("the lethal damage lands"),
		Scene.Player->GetHealth()->ApplyDamage(9999.0f) > 0.0f))
	{
		return true;
	}
	TestTrue(TEXT("the player death failed the run"),
		Scene.Session->GetState() == ERoomSessionState::Failed);

	// Defeat -> Result: the flow tracks the terminal run and the real HUD
	// shows the Defeat screen.
	TestTrue(TEXT("the flow moved to the result state on the failed run"),
		Scene.Flow->GetState() == EGameFlowState::Result);
	TestTrue(TEXT("the defeat screen presented"), Scene.Hud->HasRoomResultScreen());
	TestEqual(TEXT("the result headline is the Defeat truth"),
		Scene.Hud->PeekRoomResultViewModel().HeadlineText, FString(TEXT("Defeat")));

	// Return -> Menu (the same closing move as the victory loop).
	Scene.Hud->HandleReturnRequested();
	TestTrue(TEXT("the return moved the flow back to the menu state"),
		Scene.Flow->GetState() == EGameFlowState::Menu);
	TestFalse(TEXT("the return dismissed the defeat screen"), Scene.Hud->HasRoomResultScreen());
	TestTrue(TEXT("the return re-presented the menu overlay"), Scene.Hud->HasMenuScreen());

	Scene.ExpectProfileIntact(*this, TEXT("after the defeat loop"));
	return true;
}

// 4. Acceptance: the mounted menu keeps the anti-double-click guard (exactly
//    ONE world switch per fast double click) and stays retryable after the
//    machine returns to the menu.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_030MountedMenuAntiDoubleClickAndRetry,
	"UEMMO.Tasks.M3_030.MountedMenuAntiDoubleClickAndRetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM3_030MountedMenuAntiDoubleClickAndRetry::RunTest(const FString& Parameters)
{
	FM3_030_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}

	UMapSelectWidget* Menu = Scene.Hud->PeekMenuWidget();
	if (!TestTrue(TEXT("the boot HUD presents the menu overlay"), Scene.Hud->HasMenuScreen())
		|| !TestNotNull(TEXT("the mounted map select widget exists"), Menu))
	{
		return true;
	}

	// The fast double click: the first request enters, the second lands while
	// the room is active and is refused - exactly one world switch.
	Menu->HandleEnterClicked();
	Menu->HandleEnterClicked();
	TestEqual(TEXT("the fast double click requested exactly one world switch"), Scene.OpenCount, 1);
	TestTrue(TEXT("the flow is in the room state after the double click"),
		Scene.Flow->GetState() == EGameFlowState::Room);
	TestFalse(TEXT("the overlay is dismissed after the double click"), Scene.Hud->HasMenuScreen());

	// The machine returns to the menu and the presentation seam re-arms the
	// mount: the menu is immediately retryable (the M3-017 retryability, now
	// observed through the mounted overlay).
	TestTrue(TEXT("the machine return to the menu is accepted"), Scene.Flow->ReturnToMenu());
	Scene.Hud->RefreshMenuPresentation();
	TestTrue(TEXT("the re-presented menu overlay exists"), Scene.Hud->HasMenuScreen());
	Menu = Scene.Hud->PeekMenuWidget();
	if (!TestNotNull(TEXT("the re-presented widget is reachable"), Menu))
	{
		return true;
	}
	if (Menu->PeekEnterButton() != nullptr)
	{
		TestTrue(TEXT("the re-presented enter button is armed"), Menu->PeekEnterButton()->GetIsEnabled());
	}
	Menu->HandleEnterClicked();
	TestEqual(TEXT("the retry requested exactly one more switch"), Scene.OpenCount, 2);
	TestTrue(TEXT("the retry reached the room state"), Scene.Flow->GetState() == EGameFlowState::Room);

	return true;
}

// 5. Capture companion (the M2-012 RoomResultScreenshots precedent): in a
//    real game world with a viewport, drive the real production loop on the
//    real boot HUD and request one screenshot per game-flow state - menu
//    (the boot overlay), game (the room after the enter click), result (the
//    staged Cleared victory screen) - plus the re-presented menu after the
//    real return. Headless -nullrhi automation runs skip the capture.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM3_030GameFlowStateScreenshots,
	"UEMMO.Tasks.M3_030.GameFlowStateScreenshots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

namespace UE::UEMMO::Tasks::M3_030
{
	// Requests one screenshot only where rendering exists (the M2-012 helper
	// shape; FScreenshotRequest does not create directories).
	static void M3_030_RequestScreenshotIfPossible(const FString& AbsolutePath)
	{
		if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
		{
			UE_LOG(LogTemp, Display, TEXT("M3_030 screenshot skipped (no rendering): %s"), *AbsolutePath);
			return;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsolutePath), /*Tree*/ true);
		// bShowUI=true: the menu overlay and the result screen are UMG layers.
		FScreenshotRequest::RequestScreenshot(AbsolutePath, /*bShowUI*/ true, /*bAddFilenameSuffix*/ false);
		UE_LOG(LogTemp, Display, TEXT("M3_030 screenshot requested: %s"), *AbsolutePath);
	}

	// One screenshot step (the state is set before it runs).
	struct FM3_030_ScreenshotLatentCommand : public IAutomationLatentCommand
	{
		FString AbsolutePath;

		explicit FM3_030_ScreenshotLatentCommand(const FString& InAbsolutePath)
			: AbsolutePath(InAbsolutePath)
		{
		}

		virtual bool Update() override
		{
			M3_030_RequestScreenshotIfPossible(AbsolutePath);
			return true;
		}
	};

	/**
	 * Re-resolving latent step base: the enter click travels to the boot map
	 * (a real world reload), so every step re-resolves the world / HUD /
	 * session from the engine at execution time - never a cached pointer
	 * across the travel.
	 */
	struct FM3_030_Resolve
	{
		static APrototypeHUD* Hud(UWorld*& OutWorld)
		{
			OutWorld = (GEngine != nullptr && GEngine->GameViewport != nullptr)
				? GEngine->GameViewport->GetWorld() : GWorld;
			if (OutWorld == nullptr)
			{
				return nullptr;
			}
			APlayerController* PC = UGameplayStatics::GetPlayerController(OutWorld, 0);
			return PC ? Cast<APrototypeHUD>(PC->GetHUD()) : nullptr;
		}
	};

	// Step: the mounted menu's real click path (the production opener then
	// travels to the boot map; the flow lands in the room state).
	struct FM3_030_EnterClickLatentCommand : public IAutomationLatentCommand
	{
		virtual bool Update() override
		{
			UWorld* World = nullptr;
			APrototypeHUD* Hud = FM3_030_Resolve::Hud(World);
			UMapSelectWidget* Menu = Hud ? Hud->PeekMenuWidget() : nullptr;
			if (Menu == nullptr)
			{
				UE_LOG(LogTemp, Warning, TEXT("M3_030 capture: the enter click skipped (no mounted menu)"));
				return true;
			}
			Menu->HandleEnterClicked();
			UGameFlowSubsystem* Flow = (World != nullptr && World->GetGameInstance() != nullptr)
				? World->GetGameInstance()->GetSubsystem<UGameFlowSubsystem>() : nullptr;
			UE_LOG(LogTemp, Display, TEXT("M3_030 capture: the enter click ran (flow state=%d)"),
				Flow ? static_cast<int32>(Flow->GetState()) : -1);
			return true;
		}
	};

	// Step: stage the CURRENT world's session to Cleared through its own
	// production entries (the M2-012 capture staging) so the real HUD
	// presents the victory screen.
	struct FM3_030_StageClearLatentCommand : public IAutomationLatentCommand
	{
		virtual bool Update() override
		{
			UWorld* World = nullptr;
			APrototypeHUD* Hud = FM3_030_Resolve::Hud(World);
			if (World == nullptr || Hud == nullptr)
			{
				UE_LOG(LogTemp, Warning, TEXT("M3_030 capture: the clear staging skipped (no world/hud)"));
				return true;
			}
			URoomSessionSubsystem* Session = World->GetSubsystem<URoomSessionSubsystem>();
			APrototypeCharacter* Player = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0));
			if (Session == nullptr || Player == nullptr || Session->GetState() != ERoomSessionState::Idle)
			{
				UE_LOG(LogTemp, Warning, TEXT("M3_030 capture: the clear staging skipped (session/player unavailable, state=%d)"),
					Session ? static_cast<int32>(Session->GetState()) : -1);
				return true;
			}
			URoomDefinition* Room = M3_030_MakeStageRoom(Player->GetActorLocation());
			UEnemyDefinition* Enemy = M3_030_MakeStageEnemy();
			Session->SetSessionClockSeconds(World->GetTimeSeconds());
			Session->StartRoom(Room);
			Session->BeginWaves(Room, Enemy);
			Session->SetSessionClockSeconds(World->GetTimeSeconds());
			Session->MarkCleared();
			UE_LOG(LogTemp, Display, TEXT("M3_030 capture: staged CLEARED run (state=%d, headline=%s, screenUp=%s)"),
				static_cast<int32>(Session->GetState()),
				*Hud->PeekRoomResultViewModel().HeadlineText,
				Hud->HasRoomResultScreen() ? TEXT("yes") : TEXT("no"));
			return true;
		}
	};

	// Step: the real return request path closes the loop (Result -> Menu, the
	// overlay re-presents).
	struct FM3_030_ReturnLatentCommand : public IAutomationLatentCommand
	{
		virtual bool Update() override
		{
			UWorld* World = nullptr;
			APrototypeHUD* Hud = FM3_030_Resolve::Hud(World);
			if (Hud == nullptr)
			{
				UE_LOG(LogTemp, Warning, TEXT("M3_030 capture: the return skipped (no hud)"));
				return true;
			}
			Hud->HandleReturnRequested();
			UGameFlowSubsystem* Flow = (World != nullptr && World->GetGameInstance() != nullptr)
				? World->GetGameInstance()->GetSubsystem<UGameFlowSubsystem>() : nullptr;
			UE_LOG(LogTemp, Display, TEXT("M3_030 capture: the return ran (flow state=%d, menuUp=%s)"),
				Flow ? static_cast<int32>(Flow->GetState()) : -1,
				Hud->HasMenuScreen() ? TEXT("yes") : TEXT("no"));
			return true;
		}
	};
}

bool FUEMMOTasksM3_030GameFlowStateScreenshots::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	if (World == nullptr || GEngine == nullptr || GEngine->GameViewport == nullptr
		|| World->WorldType != EWorldType::Game || !FApp::CanEverRender())
	{
		// The capture needs a rendered game world; this suite also drives the
		// REAL enter travel on the shared boot world, so a headless -nullrhi
		// regression run skips it entirely (the M2-012 capture-companion
		// precedent of render-only evidence).
		AddInfo(TEXT("screenshot capture skipped: no running rendered game world/viewport"));
		return true;
	}
	APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0);
	APrototypeHUD* Hud = PC ? Cast<APrototypeHUD>(PC->GetHUD()) : nullptr;
	UGameFlowSubsystem* Flow = (World->GetGameInstance() != nullptr)
		? World->GetGameInstance()->GetSubsystem<UGameFlowSubsystem>() : nullptr;
	if (PC == nullptr || Hud == nullptr || Flow == nullptr)
	{
		AddInfo(TEXT("screenshot capture skipped: no player controller / HUD / flow subsystem"));
		return true;
	}
	// The boot surface must be the menu overlay (the fresh process boots into
	// the menu state; a prior suite's room run would falsify the capture).
	if (Flow->GetState() != EGameFlowState::Menu || !Hud->HasMenuScreen())
	{
		AddInfo(FString::Printf(TEXT("screenshot capture skipped: the boot surface is not the menu (state=%d, menuUp=%s)"),
			static_cast<int32>(Flow->GetState()), Hud->HasMenuScreen() ? TEXT("yes") : TEXT("no")));
		return true;
	}

	const FString Directory = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M3-030"));

	// Capture cadence: FScreenshotRequest is DEFERRED (the capture lands at
	// the end of the frame it is requested in), and the automation queue
	// drains every immediately-true latent command within the same frame - so
	// each screenshot request is followed by a settle wait BEFORE the next
	// state change runs, otherwise the capture trails one step behind (the
	// first capture round showed exactly that).

	// State 1: the boot menu overlay (badge + enter button over the room).
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_030_ScreenshotLatentCommand(Directory / TEXT("m3-030-state-menu.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));

	// State 2: the game state - the real enter click travels to the combat
	// room (a fresh world); the overlay is dismissed there.
	ADD_LATENT_AUTOMATION_COMMAND(FM3_030_EnterClickLatentCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_030_ScreenshotLatentCommand(Directory / TEXT("m3-030-state-game.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));

	// State 3: the result state - the staged Cleared run presents the victory
	// screen through the real OnRunEnded chain.
	ADD_LATENT_AUTOMATION_COMMAND(FM3_030_StageClearLatentCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_030_ScreenshotLatentCommand(Directory / TEXT("m3-030-state-result.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));

	// Closing move: the real return re-presents the menu overlay.
	ADD_LATENT_AUTOMATION_COMMAND(FM3_030_ReturnLatentCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
	ADD_LATENT_AUTOMATION_COMMAND(FM3_030_ScreenshotLatentCommand(Directory / TEXT("m3-030-state-menu-again.png")));
	return true;
}

#endif
