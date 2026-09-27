// M2-012: the room result screen and its retry button. The suite locks the
// card's four behaviors:
//
//   1. The pure view model derives the Victory/Defeat headline, the time and
//      kill summary from the session's real FRoomResult - and the reward line
//      stays empty for BOTH outcomes (no reward data exists in M2, nothing
//      may be invented).
//   2. The retry/return request paths are one-shot: of two fast clicks only
//      the first is processed (guard counts), and a re-armed screen accepts
//      again.
//   3. The input-focus contract is a pure tracked state: capture to UI once
//      per presentation, restore to game exactly once on dismissal.
//   4. The HUD integration: a real terminal OnRunEnded presents the native
//      result screen (headless-safe CreateWidget smoke included); the retry
//      request really restarts the failed run through URoomRetryService and
//      restores the input flag; the return path really leaves the room once.
//
// Test 8 is the M2-009 ExitStateScreenshots capture companion: in a real game
// world with a viewport it stages the session to Cleared/Failed through the
// session's own production terminal entries and requests one screenshot per
// state plus one after the real retry (the -RenderOffscreen capture run
// produces the images; headless -nullrhi automation runs skip the capture).
//
// The world tests reuse the M2-010 RoomRetryTests scaffold precedent (a real
// BeginPlay-initialized temp game world with the production
// URoomSessionSubsystem and the shipped-health prototype pawn); the room /
// enemy definitions are transient doubles of the room_training_01 /
// Data/enemies.json shape, the M2-007+ suite precedent.
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/Widget.h"

#include "../Combat/HealthComponent.h"
#include "../Enemy/EnemyDefinition.h"
#include "../PrototypeCharacter.h"
#include "../PrototypeHUD.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomRetryService.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../UI/RoomResultWidget.h"

#include "Components/BoxComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M2_012
{
	// Spawn height above the (virtual) floor top; the world tests never tick
	// physics-critical motion, so no floor geometry is needed here.
	const float M2_012_PlayerSpawnHeight = 120.0f;

	// Definition double of the Data/enemies.json "melee_grunt" row (the
	// M2-007/M2-008/M2-010 suite precedent for runtime definitions).
	static UEnemyDefinition* M2_012_MakeEnemyDef()
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

	// Transient room double with one one-enemy wave anchored at BaseLocation
	// (BeginWaves must accept the definition so the real retry path can
	// restart the run; no clock injection happens here, so nothing is born).
	static URoomDefinition* M2_012_MakeRoom(const FVector& BaseLocation)
	{
		URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Room->RoomId = FName(TEXT("room_m2_012_stage"));
		Room->RewardTableId = FName(TEXT("starter"));

		FRoomWaveDefinition Wave;
		Wave.EnemyId = FName(TEXT("melee_grunt"));
		Wave.Count = 1;
		Wave.SpawnLocations.Add(BaseLocation + FVector(420.0, 0.0, M2_012_PlayerSpawnHeight));
		Room->Waves.Add(Wave);
		return Room;
	}

	// Spawns the real prototype pawn (the M2-004 rig: the pawn carries its own
	// UHealthComponent from spawn; no runtime attachment).
	static APrototypeCharacter* M2_012_SpawnPlayer(UWorld& World)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(),
			FVector(100.0, 0.0, M2_012_PlayerSpawnHeight),
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

	static bool M2_012_CanCaptureScreenshot()
	{
		return GEngine != nullptr && GEngine->GameViewport != nullptr && FApp::CanEverRender();
	}

	// Requests a screenshot only where rendering exists; creates the target
	// directory first (FScreenshotRequest does not create directories).
	static void M2_012_RequestScreenshotIfPossible(const FString& AbsolutePath)
	{
		if (!M2_012_CanCaptureScreenshot())
		{
			UE_LOG(LogTemp, Display, TEXT("M2_012 screenshot skipped (no rendering): %s"), *AbsolutePath);
			return;
		}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsolutePath), /*Tree*/ true);
	// bShowUI=true: the result screen is a Slate/UMG layer, so the capture
	// must include the UI (the M1-035 smoke precedent captures with UI too).
	FScreenshotRequest::RequestScreenshot(AbsolutePath, /*bShowUI*/ true, /*bAddFilenameSuffix*/ false);
		UE_LOG(LogTemp, Display, TEXT("M2_012 screenshot requested: %s"), *AbsolutePath);
	}

	// Latent step: requests one screenshot (state was set before it runs).
	struct FM2_012_ScreenshotLatentCommand : public IAutomationLatentCommand
	{
		FString AbsolutePath;

		explicit FM2_012_ScreenshotLatentCommand(const FString& InAbsolutePath)
			: AbsolutePath(InAbsolutePath)
		{
		}

		virtual bool Update() override
		{
			M2_012_RequestScreenshotIfPossible(AbsolutePath);
			return true;
		}
	};

	// Latent staging step of the capture companion. Action 1 resets the
	// finished run and stages a fresh run to FAILED through the session's own
	// production entries (StartRoom + BeginWaves + FailRun); action 2 drives
	// the HUD's real retry request path. Every step logs the session state as
	// evidence; the assertions live in the headless suites.
	struct FM2_012_SessionActionLatentCommand : public IAutomationLatentCommand
	{
		TWeakObjectPtr<UWorld> WorldPtr;
		TWeakObjectPtr<URoomSessionSubsystem> SessionPtr;
		TWeakObjectPtr<APrototypeHUD> HudPtr;
		TWeakObjectPtr<const URoomDefinition> RoomPtr;
		TWeakObjectPtr<UEnemyDefinition> EnemyPtr;
		int32 Action = 0;

		FM2_012_SessionActionLatentCommand(UWorld* InWorld, URoomSessionSubsystem* InSession,
			APrototypeHUD* InHud, const URoomDefinition* InRoom, UEnemyDefinition* InEnemy, int32 InAction)
			: WorldPtr(InWorld), SessionPtr(InSession), HudPtr(InHud), RoomPtr(InRoom), EnemyPtr(InEnemy), Action(InAction)
		{
		}

		virtual bool Update() override
		{
			UWorld* World = WorldPtr.Get();
			URoomSessionSubsystem* Session = SessionPtr.Get();
			APrototypeHUD* Hud = HudPtr.Get();
			if (World == nullptr || Session == nullptr || Hud == nullptr)
			{
				UE_LOG(LogTemp, Warning, TEXT("M2_012 capture staging skipped (world/session/hud gone)"));
				return true;
			}
			if (Action == 1)
			{
				Session->ResetToIdle();
				Session->SetSessionClockSeconds(World->GetTimeSeconds());
				Session->StartRoom(RoomPtr.Get());
				Session->BeginWaves(RoomPtr.Get(), EnemyPtr.Get());
				Session->FailRun();
				UE_LOG(LogTemp, Display, TEXT("M2_012 capture staged FAILED run (state=%d, headline=%s)"),
					static_cast<int32>(Session->GetState()), *Hud->PeekRoomResultViewModel().HeadlineText);
			}
			else if (Action == 2)
			{
				Hud->HandleRetryRequested();
				UE_LOG(LogTemp, Display, TEXT("M2_012 capture drove the real retry (state=%d, RunId=%llu, screenUp=%s)"),
					static_cast<int32>(Session->GetState()), Session->GetRunId(),
					Hud->HasRoomResultScreen() ? TEXT("yes") : TEXT("no"));
			}
			return true;
		}
	};

	// One temp game world with its real session subsystem, the real prototype
	// pawn and the spawned HUD under test (the game mode spawns the same HUD
	// class in-game; BeginPlay binds the session's OnRunEnded).
	struct FM2_012_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		URoomSessionSubsystem* Session = nullptr;
		APrototypeCharacter* Player = nullptr;
		URoomDefinition* Room = nullptr;
		UEnemyDefinition* Enemy = nullptr;
		APrototypeHUD* Hud = nullptr;
		int32 StartedCount = 0;
		int32 EndedCount = 0;
		FRoomResult LastResult;

		bool Build(FAutomationTestBase& Test)
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
				LastResult = Result;
			});

			Player = M2_012_SpawnPlayer(*World);
			if (!Test.TestNotNull(TEXT("the real prototype character spawns"), Player))
			{
				return false;
			}
			Session->SetPlayer(Player);

			Room = M2_012_MakeRoom(Player->GetActorLocation());
			Enemy = M2_012_MakeEnemyDef();

			// The HUD under test: spawned like the game mode spawns it; its
			// BeginPlay binds the session's OnRunEnded.
			FActorSpawnParameters HudParams;
			Hud = World->SpawnActor<APrototypeHUD>(APrototypeHUD::StaticClass(),
				FVector::ZeroVector, FRotator::ZeroRotator, HudParams);
			if (!Test.TestNotNull(TEXT("the prototype HUD spawns in the test world"), Hud))
			{
				return false;
			}
			Hud->SetRoomRetryContext(Room, Enemy);
			return true;
		}

		/** Starts one run and fails it through the real player-death path. */
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

using namespace UE::UEMMO::Tasks::M2_012;

// 1. Acceptance: the view model copies the cleared result truth - the Victory
//    headline, the exact time and kill values, and an empty reward line (no
//    reward data exists in M2, nothing may be invented).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_012ViewModelVictoryFillsFromRoomResult,
	"UEMMO.Tasks.M2_012.ViewModelVictoryFillsFromRoomResult",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_012ViewModelVictoryFillsFromRoomResult::RunTest(const FString& Parameters)
{
	FRoomResult Result;
	Result.RunId = 7;
	Result.RoomId = FName(TEXT("room_m2_012_stage"));
	Result.bCleared = true;
	Result.KilledCount = 4;
	Result.ElapsedSeconds = 12.5;

	const FRoomResultViewModel ViewModel = MakeRoomResultViewModel(Result);
	TestTrue(TEXT("the filled view model is valid"), ViewModel.bValid);
	TestEqual(TEXT("the cleared result shows the Victory headline"), ViewModel.HeadlineText, FString(TEXT("Victory")));
	TestEqual(TEXT("the elapsed time is copied from the result"), ViewModel.ElapsedSeconds, 12.5);
	TestEqual(TEXT("the kill count is copied from the result"), ViewModel.KilledCount, 4);
	TestTrue(TEXT("the summary carries the result time"), ViewModel.SummaryText.Contains(TEXT("12.5")));
	TestTrue(TEXT("the summary carries the result kills"), ViewModel.SummaryText.Contains(TEXT("4")));
	TestTrue(TEXT("no reward text is invented for the victory"), ViewModel.RewardText.IsEmpty());
	return true;
}

// 2. Acceptance: the failed run shows Defeat and carries no reward section at
//    all - neither an invented reward line nor fake equipment counts; a
//    default view model (never filled) stays empty and invalid (the stub
//    shape the presentation must never show).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_012ViewModelDefeatShowsNoReward,
	"UEMMO.Tasks.M2_012.ViewModelDefeatShowsNoReward",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_012ViewModelDefeatShowsNoReward::RunTest(const FString& Parameters)
{
	FRoomResult Result;
	Result.bCleared = false;
	Result.KilledCount = 1;
	Result.ElapsedSeconds = 3.2;

	const FRoomResultViewModel ViewModel = MakeRoomResultViewModel(Result);
	TestTrue(TEXT("the filled view model is valid"), ViewModel.bValid);
	TestEqual(TEXT("the failed result shows the Defeat headline"), ViewModel.HeadlineText, FString(TEXT("Defeat")));
	TestTrue(TEXT("the defeat view carries no reward line"), ViewModel.RewardText.IsEmpty());
	TestTrue(TEXT("the defeat headline names no reward"), !ViewModel.HeadlineText.Contains(TEXT("Reward")));
	TestTrue(TEXT("the defeat summary names no reward"), !ViewModel.SummaryText.Contains(TEXT("Reward")));
	TestEqual(TEXT("the defeat still reports the real kill count"), ViewModel.KilledCount, 1);

	// The never-filled stub shape: invalid and empty (never presentable).
	FRoomResultViewModel Empty;
	TestFalse(TEXT("the default view model is not valid"), Empty.bValid);
	TestTrue(TEXT("the default view model has no headline"), Empty.HeadlineText.IsEmpty());
	TestTrue(TEXT("the default view model has no reward line"), Empty.RewardText.IsEmpty());
	return true;
}

// 3. Acceptance: of two fast retry requests only the FIRST is processed (the
//    request counter increments once); the dropped duplicate is counted; a
//    re-armed presentation accepts exactly one request again.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_012RetryGuardProcessesDoubleClickOnce,
	"UEMMO.Tasks.M2_012.RetryGuardProcessesDoubleClickOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_012RetryGuardProcessesDoubleClickOnce::RunTest(const FString& Parameters)
{
	FRoomResultActionGuard Guard;
	int32 HandledRequests = 0;
	auto RequestRetry = [&Guard, &HandledRequests]()
	{
		if (Guard.TryAccept())
		{
			++HandledRequests;
		}
	};

	// Before the first presentation nothing is accepted (the disarmed shape);
	// the guard counts every refused request, including this early probe.
	RequestRetry();
	TestEqual(TEXT("no request is processed before the screen is armed"), HandledRequests, 0);
	TestEqual(TEXT("the disarmed request was refused and counted"), Guard.GetRejectedCount(), 1);

	// The presentation re-arms; the fast double click arrives.
	Guard.ReArm();
	RequestRetry();
	RequestRetry();
	TestEqual(TEXT("the double request was processed exactly once"), HandledRequests, 1);
	TestEqual(TEXT("the guard counted one accepted request"), Guard.GetAcceptedCount(), 1);
	TestEqual(TEXT("the guard counted the dropped duplicate (plus the disarmed probe)"),
		Guard.GetRejectedCount(), 2);

	// A fresh presentation (retry failed another run later) arms again.
	Guard.ReArm();
	RequestRetry();
	TestEqual(TEXT("the re-armed screen accepted one new request"), HandledRequests, 2);
	TestEqual(TEXT("the accepted count advanced exactly once"), Guard.GetAcceptedCount(), 2);
	return true;
}

// 4. Acceptance: the input-focus contract as a pure state flag - capture to
//    UI once per presentation, restore to game exactly once on dismissal; a
//    duplicate capture/restore never moves the phase or the counts again, and
//    a new presentation captures again after a restore.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_012InputFocusTrackerRestoresAfterExit,
	"UEMMO.Tasks.M2_012.InputFocusTrackerRestoresAfterExit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_012InputFocusTrackerRestoresAfterExit::RunTest(const FString& Parameters)
{
	FRoomResultInputFocusTracker Tracker;
	TestTrue(TEXT("the tracker starts in the game phase"),
		Tracker.GetPhase() == ERoomResultInputPhase::InGame);

	// A restore without a capture is inert.
	TestFalse(TEXT("a restore without a capture is refused"), Tracker.RestoreToGame());
	TestEqual(TEXT("no restore was counted"), Tracker.GetRestoreCount(), 0);

	// First presentation: exactly one capture.
	TestTrue(TEXT("the first capture transitions to the UI phase"), Tracker.CaptureToUI());
	TestTrue(TEXT("the screen phase is CapturedToUI"),
		Tracker.GetPhase() == ERoomResultInputPhase::CapturedToUI);
	TestEqual(TEXT("the capture was counted once"), Tracker.GetCaptureCount(), 1);
	TestFalse(TEXT("a duplicate capture while captured is refused"), Tracker.CaptureToUI());
	TestEqual(TEXT("the duplicate capture was not counted"), Tracker.GetCaptureCount(), 1);

	// Dismissal: exactly one restore back to the game.
	TestTrue(TEXT("the restore transitions back to the game phase"), Tracker.RestoreToGame());
	TestTrue(TEXT("the phase is RestoredToGame"),
		Tracker.GetPhase() == ERoomResultInputPhase::RestoredToGame);
	TestEqual(TEXT("the restore was counted once"), Tracker.GetRestoreCount(), 1);
	TestFalse(TEXT("a duplicate restore is refused"), Tracker.RestoreToGame());
	TestEqual(TEXT("the duplicate restore was not counted"), Tracker.GetRestoreCount(), 1);

	// The next presentation (a later run ended) captures again.
	TestTrue(TEXT("a new presentation captures the input again"), Tracker.CaptureToUI());
	TestEqual(TEXT("the second capture was counted"), Tracker.GetCaptureCount(), 2);
	return true;
}

// 5. Acceptance: the native widget is created in a plain game world
//    (CreateWidget smoke, no UMG asset) and BindResult fills the headline and
//    summary blocks from the result while the reward row stays collapsed for
//    BOTH outcomes; a rebind (fresh presentation) re-enables the buttons.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_012WidgetSmokeCreateBindRewardHidden,
	"UEMMO.Tasks.M2_012.WidgetSmokeCreateBindRewardHidden",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_012WidgetSmokeCreateBindRewardHidden::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Wrapper;
	if (!TestTrue(TEXT("the test world is created"), Wrapper.CreateTestWorld(EWorldType::Game)))
	{
		return true;
	}
	UWorld* World = Wrapper.GetTestWorld();
	if (!TestNotNull(TEXT("the test world is available"), World))
	{
		return true;
	}

	URoomResultWidget* Widget = CreateWidget<URoomResultWidget>(World, URoomResultWidget::StaticClass());
	if (!TestNotNull(TEXT("the native result widget is created in a plain game world (no UMG asset)"), Widget))
	{
		return true;
	}

	// Defeat first: the headline/summary carry the result, the reward row
	// stays collapsed and the buttons start enabled.
	FRoomResult Defeat;
	Defeat.bCleared = false;
	Defeat.KilledCount = 2;
	Defeat.ElapsedSeconds = 4.0;
	Widget->BindResult(Defeat);
	TestNotNull(TEXT("the headline text block exists"), Widget->PeekHeadlineBlock());
	TestNotNull(TEXT("the summary text block exists"), Widget->PeekSummaryBlock());
	TestNotNull(TEXT("the reward text block exists"), Widget->PeekRewardBlock());
	if (Widget->PeekHeadlineBlock() != nullptr)
	{
		TestEqual(TEXT("the headline block shows Defeat"),
			Widget->PeekHeadlineBlock()->GetText().ToString(), FString(TEXT("Defeat")));
	}
	if (Widget->PeekSummaryBlock() != nullptr)
	{
		TestTrue(TEXT("the summary block carries the real kill count"),
			Widget->PeekSummaryBlock()->GetText().ToString().Contains(TEXT("2")));
	}
	if (Widget->PeekRewardBlock() != nullptr)
	{
		TestTrue(TEXT("the reward row is collapsed for the defeat (no invented rewards)"),
			Widget->PeekRewardBlock()->GetVisibility() == ESlateVisibility::Collapsed);
	}
	if (Widget->PeekRetryButton() != nullptr && Widget->PeekReturnButton() != nullptr)
	{
		TestTrue(TEXT("the Retry button is enabled after the bind"), Widget->PeekRetryButton()->GetIsEnabled());
		TestTrue(TEXT("the Return button is enabled after the bind"), Widget->PeekReturnButton()->GetIsEnabled());
	}
	if (Widget->PeekRetryLabel() != nullptr)
	{
		TestEqual(TEXT("the Retry button carries its label"),
			Widget->PeekRetryLabel()->GetText().ToString(), FString(TEXT("Retry")));
	}
	if (Widget->PeekReturnLabel() != nullptr)
	{
		TestEqual(TEXT("the Return button carries its label"),
			Widget->PeekReturnLabel()->GetText().ToString(), FString(TEXT("Return")));
	}

	// A victory rebind (fresh presentation): the headline switches, the reward
	// row stays collapsed (no reward data exists yet) and the buttons re-arm.
	FRoomResult Victory;
	Victory.bCleared = true;
	Victory.KilledCount = 5;
	Victory.ElapsedSeconds = 21.0;
	Widget->BindResult(Victory);
	if (Widget->PeekHeadlineBlock() != nullptr)
	{
		TestEqual(TEXT("the headline block shows Victory after the rebind"),
			Widget->PeekHeadlineBlock()->GetText().ToString(), FString(TEXT("Victory")));
	}
	if (Widget->PeekRewardBlock() != nullptr)
	{
		TestTrue(TEXT("the reward row stays collapsed for the victory too (nothing invented)"),
			Widget->PeekRewardBlock()->GetVisibility() == ESlateVisibility::Collapsed);
	}
	if (Widget->PeekRetryButton() != nullptr)
	{
		TestTrue(TEXT("the rebind re-enabled the Retry button"), Widget->PeekRetryButton()->GetIsEnabled());
	}
	return true;
}

// 6. Acceptance: a real terminal OnRunEnded presents the screen (widget
//    created, view model = the real Defeat result, input captured to UI);
//    the fast double retry request is processed exactly ONCE: the real
//    URoomRetryService::RetryRoom restarts the failed run (new RunId, player
//    revived), the duplicate starts nothing further, the screen is dismissed
//    and the input flag returns to the game.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_012HudRunEndedShowsScreenAndSingleRetry,
	"UEMMO.Tasks.M2_012.HudRunEndedShowsScreenAndSingleRetry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_012HudRunEndedShowsScreenAndSingleRetry::RunTest(const FString& Parameters)
{
	FM2_012_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	if (!Scene.FailRunThroughPlayerDeath(*this))
	{
		return true;
	}

	// The real broadcast reached the HUD: the screen is prepared with the
	// session's real result and the input flag moved to the UI once.
	TestEqual(TEXT("the failure fired exactly one end event"), Scene.EndedCount, 1);
	TestTrue(TEXT("the real HUD prepared the result screen"), Scene.Hud->HasRoomResultScreen());
	const FRoomResultViewModel& ViewModel = Scene.Hud->PeekRoomResultViewModel();
	TestEqual(TEXT("the view model shows the Defeat headline"), ViewModel.HeadlineText, FString(TEXT("Defeat")));
	TestFalse(TEXT("the view model records the failure"), ViewModel.bCleared);
	TestEqual(TEXT("the view model keeps the real (zero) kill count"), ViewModel.KilledCount, 0);
	TestTrue(TEXT("the input was captured to the UI"),
		Scene.Hud->PeekRoomResultInputFocus().GetPhase() == ERoomResultInputPhase::CapturedToUI);

	// The player is dead before the retry.
	TestTrue(TEXT("the player is dead before the retry"), !Scene.Player->GetHealth()->IsAlive());

	// The fast double click at the request level (both clicks hit the same
	// handler the Retry button forwards to).
	const uint64 FailedRunId = Scene.Session->GetRunId();
	Scene.Hud->HandleRetryRequested();
	const uint64 RetriedRunId = Scene.Session->GetRunId();
	Scene.Hud->HandleRetryRequested();

	TestEqual(TEXT("the duplicate retry request started no further run"),
		Scene.Session->GetRunId(), RetriedRunId);
	TestTrue(TEXT("the first retry restarted the failed run with a new RunId"), RetriedRunId > FailedRunId);
	TestTrue(TEXT("the session runs again after the retry"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("exactly two runs started (failed + retried)"), Scene.StartedCount, 2);
	TestEqual(TEXT("still exactly one end event (the retry run has not ended)"), Scene.EndedCount, 1);
	TestFalse(TEXT("the result screen was dismissed by the retry"), Scene.Hud->HasRoomResultScreen());
	TestTrue(TEXT("the input focus returned to the game after the retry"),
		Scene.Hud->PeekRoomResultInputFocus().GetPhase() == ERoomResultInputPhase::RestoredToGame);
	TestEqual(TEXT("the input switch happened exactly once per direction"),
		Scene.Hud->PeekRoomResultInputFocus().GetCaptureCount()
		+ Scene.Hud->PeekRoomResultInputFocus().GetRestoreCount(), 2);
	TestTrue(TEXT("the player was revived and can move again"), Scene.Player->GetHealth()->IsAlive());
	return true;
}

// 7. Acceptance: the Return path is also one-shot and really leaves: the
//    first request dismisses the screen and moves the session to Exiting
//    (an exit is NOT a settlement - no end event); the duplicate is dropped
//    and the input flag restored exactly once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_012HudReturnLeavesRoomOnce,
	"UEMMO.Tasks.M2_012.HudReturnLeavesRoomOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_012HudReturnLeavesRoomOnce::RunTest(const FString& Parameters)
{
	FM2_012_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	if (!Scene.FailRunThroughPlayerDeath(*this))
	{
		return true;
	}
	TestTrue(TEXT("the failure presented the result screen"), Scene.Hud->HasRoomResultScreen());

	// The fast double click on Return at the request level.
	Scene.Hud->HandleReturnRequested();
	Scene.Hud->HandleReturnRequested();

	TestFalse(TEXT("the result screen was dismissed by the return"), Scene.Hud->HasRoomResultScreen());
	TestTrue(TEXT("the session left the room (Exiting)"),
		Scene.Session->GetState() == ERoomSessionState::Exiting);
	TestEqual(TEXT("the exit is not a settlement (still one end event)"), Scene.EndedCount, 1);
	TestTrue(TEXT("the input focus returned to the game after the return"),
		Scene.Hud->PeekRoomResultInputFocus().GetPhase() == ERoomResultInputPhase::RestoredToGame);
	TestEqual(TEXT("the restore happened exactly once"),
		Scene.Hud->PeekRoomResultInputFocus().GetRestoreCount(), 1);
	return true;
}

// 8. Capture companion (the M2-009 ExitStateScreenshots precedent): in a real
//    game world with a viewport, stage the real session to Cleared and Failed
//    through its own production terminal entries, present the real HUD result
//    screen each time, then drive the real retry - one screenshot per state.
//    Headless -nullrhi automation runs skip the capture gracefully.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_012RoomResultScreenshots,
	"UEMMO.Tasks.M2_012.RoomResultScreenshots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_012RoomResultScreenshots::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	if (World == nullptr || GEngine == nullptr || GEngine->GameViewport == nullptr
		|| World->WorldType != EWorldType::Game)
	{
		AddInfo(TEXT("screenshot capture skipped: no running game world/viewport"));
		return true;
	}
	APlayerController* PC = UGameplayStatics::GetPlayerController(World, 0);
	APrototypeCharacter* Player = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0));
	APrototypeHUD* Hud = PC ? Cast<APrototypeHUD>(PC->GetHUD()) : nullptr;
	URoomSessionSubsystem* Session = World->GetSubsystem<URoomSessionSubsystem>();
	if (PC == nullptr || Player == nullptr || Hud == nullptr || Session == nullptr
		|| Session->GetState() != ERoomSessionState::Idle)
	{
		AddInfo(TEXT("screenshot capture skipped: no player/HUD or the game session is not Idle"));
		return true;
	}

	// Transient definition doubles anchored at the player (the M2-007+
	// precedent); registered as the retry context so the staged retry is real.
	URoomDefinition* Room = M2_012_MakeRoom(Player->GetActorLocation());
	UEnemyDefinition* Enemy = M2_012_MakeEnemyDef();
	Hud->SetRoomRetryContext(Room, Enemy);

	// Stage one real run to CLEARED through the session's own entries; the
	// OnRunEnded broadcast reaches the real HUD and presents the screen.
	Session->SetSessionClockSeconds(World->GetTimeSeconds());
	if (!TestTrue(TEXT("the capture run starts"), Session->StartRoom(Room)))
	{
		return true;
	}
	if (!TestTrue(TEXT("the capture progression begins"), Session->BeginWaves(Room, Enemy)))
	{
		return true;
	}
	Session->SetSessionClockSeconds(World->GetTimeSeconds());
	if (!TestTrue(TEXT("the capture run clears through the real MarkCleared"), Session->MarkCleared()))
	{
		return true;
	}
	TestTrue(TEXT("the real HUD prepared the victory screen"), Hud->HasRoomResultScreen());
	TestEqual(TEXT("the real HUD shows the Victory headline"),
		Hud->PeekRoomResultViewModel().HeadlineText, FString(TEXT("Victory")));

	const FString Directory = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M2-012"));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
	ADD_LATENT_AUTOMATION_COMMAND(FM2_012_ScreenshotLatentCommand(Directory / TEXT("room-result-victory.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));

	// Reset + fresh run staged to FAILED through the real entries (defeat).
	ADD_LATENT_AUTOMATION_COMMAND(FM2_012_SessionActionLatentCommand(World, Session, Hud, Room, Enemy, 1));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
	ADD_LATENT_AUTOMATION_COMMAND(FM2_012_ScreenshotLatentCommand(Directory / TEXT("room-result-defeat.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));

	// The real retry request path (screen dismissed, run restarted, input back).
	ADD_LATENT_AUTOMATION_COMMAND(FM2_012_SessionActionLatentCommand(World, Session, Hud, Room, Enemy, 2));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
	ADD_LATENT_AUTOMATION_COMMAND(FM2_012_ScreenshotLatentCommand(Directory / TEXT("room-result-after-retry.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));
	return true;
}

#endif
