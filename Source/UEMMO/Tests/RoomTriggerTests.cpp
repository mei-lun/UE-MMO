// M2-009: room activation trigger and exit state (interface contract section
// 7). The suite locks four behaviors of the two new Room actors:
//
//   1. ARoomTrigger starts exactly ONE run no matter how often the player
//      re-enters the activation volume - the rejection of every repeated
//      Start comes from the session's own acceptance rule (only Idle may
//      start a run; the running RunId is never reset), and only the player
//      pawn (APrototypeCharacter) can start anything.
//   2. ARoomExit is EVENT-DRIVEN by the session: locked while a run is
//      Running (and after a Failed run), unlocked exactly when a Cleared
//      result fires OnRunEnded - never a timer guess.
//   3. A locked exit cannot be exited as a victory: player overlap while
//      Running produces no BeginExit signal and no end event.
//   4. After Cleared, the first player overlap signals BeginExit (the session
//      moves to Exiting) and further overlaps stay inert (exactly once).
//
// Test 5 checks the saved map asset itself: L_CombatRoom01 (the duplicate of
// the untouched L_TrainingArena) carries exactly one trigger and one exit
// after a plain -game LoadObject reload.
//
// Test 6 is a capture companion: in a real game world with a viewport it
// drives one real ARoomExit through its locked/unlocked states and requests
// one screenshot per state (the -RenderOffscreen capture run produces the
// images; headless -nullrhi automation runs skip the capture gracefully).
//
// The tests reuse the engine FTestWorldWrapper precedent from the M2-006/
// M2-007/M2-008 suites (a real, BeginPlay-initialized temp game world with
// the production URoomSessionSubsystem) and add the real Tick mode: overlap
// events need the world to tick, so every player move is followed by
// TickTestWorld.
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/Paths.h"

#include "Engine/World.h"
#include "Engine/Level.h"
#include "EngineUtils.h"
#include "CollisionShape.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Tests/AutomationCommon.h"
#include "UObject/UObjectGlobals.h"
#include "UnrealClient.h"

#include "../Enemy/TrainingEnemy.h"
#include "../PrototypeCharacter.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"
#include "../Room/RoomTrigger.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M2_009
{
	// One temp game world with its real session subsystem per test (the
	// FTestWorldWrapper precedent). Unlike the M2-008 scenes this suite Ticks
	// the world: overlap dispatch needs real world ticks.
	struct FM2_009_Scene
	{
		FTestWorldWrapper Wrapper;
		UWorld* World = nullptr;
		URoomSessionSubsystem* Session = nullptr;

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
			return true;
		}

		// Real tick mode: advances the world by two frames so teleport-driven
		// overlap events are dispatched before the next assertion.
		void TickTwice()
		{
			Wrapper.TickTestWorld(0.016f);
			Wrapper.TickTestWorld(0.016f);
		}
	};

	// Minimal room definition (the M2-006 precedent): StartRoom only reads
	// the room id from it.
	static URoomDefinition* M2_009_MakeRoom(FName RoomId)
	{
		URoomDefinition* Room = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		Room->RoomId = RoomId;
		return Room;
	}

	// Recorded session events (the assertions read this struct only).
	struct FM2_009_RunEvents
	{
		int32 StartedCount = 0;
		int32 EndedCount = 0;
		TArray<FRoomResult> EndedResults;

		void Bind(URoomSessionSubsystem& Session)
		{
			Session.OnRunStarted().AddLambda([this]()
			{
				++StartedCount;
			});
			Session.OnRunEnded().AddLambda([this](const FRoomResult& Result)
			{
				++EndedCount;
				EndedResults.Add(Result);
			});
		}
	};

	// Spawns the real prototype character and makes it hold still in the
	// ticked (floor-less) test world: zero gravity and explicit overlap
	// generation on the capsule (the trigger detects the pawn through it).
	static APrototypeCharacter* M2_009_SpawnPlayer(FAutomationTestBase& Test, UWorld& World, const FVector& Location)
	{
		FActorSpawnParameters Params;
		APrototypeCharacter* Player = World.SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (!Test.TestNotNull(TEXT("the prototype character spawns"), Player))
		{
			return nullptr;
		}
		if (!Player->HasActorBegunPlay())
		{
			Player->DispatchBeginPlay();
		}
		Player->GetCharacterMovement()->GravityScale = 0.0f;
		Player->GetCapsuleComponent()->SetGenerateOverlapEvents(true);
		return Player;
	}

	// Teleports the player and ticks the world so the overlap state settles.
	// A manually ticked test world can lag the physics scene behind component
	// teleports; the fresh overlap query on the moved capsule (at its current
	// component transform) performs the same discovery the engine's physics
	// step performs every real frame and dispatches Begin/End overlap events
	// to both components of the pair.
	static void M2_009_MoveAndTick(FM2_009_Scene& Scene, APrototypeCharacter* Player, const FVector& Location)
	{
		Player->SetActorLocation(Location, /*bSweep*/ false);
		Scene.TickTwice();
		Player->GetCapsuleComponent()->UpdateOverlaps();
	}

	// Spawns a passive training enemy as the non-player overlap probe (its
	// capsule really overlaps, so a broken player-only filter would fire).
	static ATrainingEnemy* M2_009_SpawnNonPlayerProbe(FAutomationTestBase& Test, UWorld& World, const FVector& Location)
	{
		FActorSpawnParameters Params;
		ATrainingEnemy* Enemy = World.SpawnActor<ATrainingEnemy>(
			ATrainingEnemy::StaticClass(), Location, FRotator::ZeroRotator, Params);
		if (!Test.TestNotNull(TEXT("the non-player probe (training enemy) spawns"), Enemy))
		{
			return nullptr;
		}
		if (!Enemy->HasActorBegunPlay())
		{
			Enemy->DispatchBeginPlay();
		}
		Enemy->GetCharacterMovement()->GravityScale = 0.0f;
		Enemy->GetCapsuleComponent()->SetGenerateOverlapEvents(true);
		return Enemy;
	}

	// Fixed test geometry: the trigger covers x in [-250, 50] around y=z=0
	// (its actor sits at (-100, 0, 88)); the exit sits at (600, 0, 100). The
	// player walks between -800 (outside) and the volumes' centers.
	constexpr float M2_009TriggerLocationX = -100.0f;
	constexpr float M2_009OutsideLocationX = -800.0f;
	constexpr float M2_009ExitLocationX = 600.0f;
	constexpr float M2_009PlayerZ = 100.0f;

	static bool M2_009_CanCaptureScreenshot()
	{
		return GEngine != nullptr && GEngine->GameViewport != nullptr && FApp::CanEverRender();
	}

	// Requests a screenshot only where rendering exists; creates the target
	// directory first (FScreenshotRequest does not create directories).
	static void M2_009_RequestScreenshotIfPossible(const FString& AbsolutePath)
	{
		if (!M2_009_CanCaptureScreenshot())
		{
			UE_LOG(LogTemp, Display, TEXT("M2_009 screenshot skipped (no rendering): %s"), *AbsolutePath);
			return;
		}
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsolutePath), /*Tree*/ true);
		FScreenshotRequest::RequestScreenshot(AbsolutePath, /*bShowUI*/ false, /*bAddFilenameSuffix*/ false);
		UE_LOG(LogTemp, Display, TEXT("M2_009 screenshot requested: %s"), *AbsolutePath);
	}

	// Latent step: clears the running run so the exit turns green between
	// the two screenshots.
	struct FM2_009_MarkClearedLatentCommand : public IAutomationLatentCommand
	{
		TWeakObjectPtr<URoomSessionSubsystem> SessionPtr;

		explicit FM2_009_MarkClearedLatentCommand(URoomSessionSubsystem* InSession)
			: SessionPtr(InSession)
		{
		}

		virtual bool Update() override
		{
			if (URoomSessionSubsystem* Session = SessionPtr.Get())
			{
				Session->MarkCleared();
			}
			return true;
		}
	};

	// Latent step: requests one screenshot (state was set by the test before).
	struct FM2_009_ScreenshotLatentCommand : public IAutomationLatentCommand
	{
		FString AbsolutePath;

		explicit FM2_009_ScreenshotLatentCommand(const FString& InAbsolutePath)
			: AbsolutePath(InAbsolutePath)
		{
		}

		virtual bool Update() override
		{
			M2_009_RequestScreenshotIfPossible(AbsolutePath);
			return true;
		}
	};
}

using namespace UE::UEMMO::Tasks::M2_009;

// 1. Acceptance: entering the activation volume starts exactly one run; every
//    later re-entry (and any non-player overlap) is inert - the RunId never
//    changes, OnRunStarted never fires twice and the session stays Running.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_009RepeatedTriggerOverlapsStartExactlyOneRun,
	"UEMMO.Tasks.M2_009.RepeatedTriggerOverlapsStartExactlyOneRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_009RepeatedTriggerOverlapsStartExactlyOneRun::RunTest(const FString& Parameters)
{
	FM2_009_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	FM2_009_RunEvents Events;
	Events.Bind(*Scene.Session);

	const FVector Outside(M2_009OutsideLocationX, 0.0f, M2_009PlayerZ);
	const FVector Inside(M2_009TriggerLocationX, 0.0f, M2_009PlayerZ);

	APrototypeCharacter* Player = M2_009_SpawnPlayer(*this, *Scene.World, Outside);
	if (Player == nullptr)
	{
		return true;
	}

	// The trigger spawns at its own actor location with production defaults.
	FActorSpawnParameters TriggerParams;
	ARoomTrigger* Trigger = Scene.World->SpawnActor<ARoomTrigger>(
		ARoomTrigger::StaticClass(), FVector(M2_009TriggerLocationX, 0.0f, 88.0f), FRotator::ZeroRotator, TriggerParams);
	if (!TestNotNull(TEXT("the room trigger spawns"), Trigger)
		|| !TestNotNull(TEXT("the trigger carries an activation volume"), Trigger->GetActivationVolume()))
	{
		return true;
	}
	TestTrue(TEXT("the trigger session resolves through the world"),
		Trigger->GetSession() == Scene.Session);
	Scene.TickTwice();

	// Non-player probe: a training enemy standing inside the volume must not
	// start anything (the player-pawn filter).
	ATrainingEnemy* Probe = M2_009_SpawnNonPlayerProbe(*this, *Scene.World, Inside);
	if (Probe == nullptr)
	{
		return true;
	}
	Scene.TickTwice();
	TestTrue(TEXT("a non-player overlap leaves the session Idle"),
		Scene.Session->GetState() == ERoomSessionState::Idle);
	TestEqual(TEXT("a non-player overlap fired no start event"), Events.StartedCount, 0);

	// Player standing outside: still Idle.
	M2_009_MoveAndTick(Scene, Player, Outside);
	TestTrue(TEXT("the player outside the volume leaves the session Idle"),
		Scene.Session->GetState() == ERoomSessionState::Idle);
	TestEqual(TEXT("no start event before any player entry"), Events.StartedCount, 0);

	// First player entry: exactly one run starts.
	M2_009_MoveAndTick(Scene, Player, Inside);
	TestTrue(TEXT("the first player entry starts the run (session Running)"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	TestEqual(TEXT("exactly one start event fired"), Events.StartedCount, 1);
	const uint64 RunId = Scene.Session->GetRunId();
	TestTrue(TEXT("the run id was assigned"), RunId != 0);
	TestTrue(TEXT("the trigger's room id reached the session (transient definition fallback)"),
		Scene.Session->GetRoomId() == Trigger->GetRoomId());

	// Three further in/out cycles: nothing changes anymore.
	for (int32 Cycle = 0; Cycle < 3; ++Cycle)
	{
		M2_009_MoveAndTick(Scene, Player, Outside);
		M2_009_MoveAndTick(Scene, Player, Inside);
		TestTrue(TEXT("a re-entry keeps the session Running"),
			Scene.Session->GetState() == ERoomSessionState::Running);
		TestEqual(TEXT("a re-entry fired no second start event"), Events.StartedCount, 1);
		TestEqual(TEXT("a re-entry kept the run id"), Scene.Session->GetRunId(), RunId);
		TestEqual(TEXT("no end event fired while re-entering"), Events.EndedCount, 0);
	}
	return true;
}

// 2. Acceptance: the exit state is event-driven from the session - locked
//    while Running (and after a Failed run), unlocked only by the Cleared
//    result of OnRunEnded, relocked by the next RunStarted. No timer is
//    involved anywhere.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_009ExitLocksWhileRunningAndUnlocksOnCleared,
	"UEMMO.Tasks.M2_009.ExitLocksWhileRunningAndUnlocksOnCleared",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_009ExitLocksWhileRunningAndUnlocksOnCleared::RunTest(const FString& Parameters)
{
	FM2_009_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	FM2_009_RunEvents Events;
	Events.Bind(*Scene.Session);

	FActorSpawnParameters ExitParams;
	ARoomExit* Exit = Scene.World->SpawnActor<ARoomExit>(
		ARoomExit::StaticClass(), FVector(M2_009ExitLocationX, 0.0f, 100.0f), FRotator::ZeroRotator, ExitParams);
	if (!TestNotNull(TEXT("the room exit spawns"), Exit)
		|| !TestNotNull(TEXT("the exit carries an exit volume"), Exit->GetExitVolume()))
	{
		return true;
	}
	TestTrue(TEXT("the exit session resolves through the world"),
		Exit->GetSession() == Scene.Session);
	Scene.TickTwice();

	// Idle: the door starts locked and only session events may open it.
	TestFalse(TEXT("the exit is locked while the session is Idle"), Exit->IsExitUnlocked());

	// Running: locked (a timer guess would have opened it by now).
	if (!TestTrue(TEXT("StartRoom accepts the run the exit reacts to"), Scene.Session->StartRoom(M2_009_MakeRoom(FName(TEXT("room_combat_01"))))))
	{
		return true;
	}
	Scene.TickTwice();
	TestFalse(TEXT("the exit is locked while the run is Running"), Exit->IsExitUnlocked());

	// Cleared: the OnRunEnded result unlocks the door.
	TestTrue(TEXT("MarkCleared ends the run"), Scene.Session->MarkCleared());
	TestTrue(TEXT("the session is Cleared"), Scene.Session->GetState() == ERoomSessionState::Cleared);
	TestTrue(TEXT("the Cleared result unlocked the exit"), Exit->IsExitUnlocked());
	TestEqual(TEXT("exactly one end event fired"), Events.EndedCount, 1);

	// A new run relocks the door (OnRunStarted); a Failed run keeps it locked.
	TestTrue(TEXT("ResetToIdle returns the session to Idle"), Scene.Session->ResetToIdle());
	TestTrue(TEXT("StartRoom accepts a second run"), Scene.Session->StartRoom(M2_009_MakeRoom(FName(TEXT("room_combat_01")))));
	Scene.TickTwice();
	TestFalse(TEXT("the next RunStarted relocked the exit"), Exit->IsExitUnlocked());
	TestTrue(TEXT("FailRun ends the second run"), Scene.Session->FailRun());
	TestTrue(TEXT("the session is Failed"), Scene.Session->GetState() == ERoomSessionState::Failed);
	TestFalse(TEXT("a Failed result keeps the exit locked"), Exit->IsExitUnlocked());
	TestEqual(TEXT("the failed run fired the second end event"), Events.EndedCount, 2);
	if (Events.EndedResults.Num() == 2)
	{
		TestTrue(TEXT("the first end result was Cleared"), Events.EndedResults[0].bCleared);
		TestFalse(TEXT("the second end result was Failed"), Events.EndedResults[1].bCleared);
	}
	return true;
}

// 3. Acceptance: an uncleared room cannot be exited as a victory - while the
//    run is Running the exit is locked, so the player overlap produces no
//    BeginExit signal, no end event and no state change.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_009LockedExitOverlapYieldsNoVictoryAndNoExit,
	"UEMMO.Tasks.M2_009.LockedExitOverlapYieldsNoVictoryAndNoExit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_009LockedExitOverlapYieldsNoVictoryAndNoExit::RunTest(const FString& Parameters)
{
	FM2_009_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	FM2_009_RunEvents Events;
	Events.Bind(*Scene.Session);

	APrototypeCharacter* Player = M2_009_SpawnPlayer(*this, *Scene.World,
		FVector(M2_009TriggerLocationX, 0.0f, M2_009PlayerZ));
	if (Player == nullptr)
	{
		return true;
	}
	FActorSpawnParameters ExitParams;
	ARoomExit* Exit = Scene.World->SpawnActor<ARoomExit>(
		ARoomExit::StaticClass(), FVector(M2_009ExitLocationX, 0.0f, 100.0f), FRotator::ZeroRotator, ExitParams);
	if (!TestNotNull(TEXT("the room exit spawns"), Exit))
	{
		return true;
	}
	Scene.TickTwice();

	if (!TestTrue(TEXT("the run starts"), Scene.Session->StartRoom(M2_009_MakeRoom(FName(TEXT("room_combat_01"))))))
	{
		return true;
	}
	const uint64 RunId = Scene.Session->GetRunId();
	TestFalse(TEXT("the exit is locked while Running"), Exit->IsExitUnlocked());

	// The player walks into the locked exit: nothing may happen.
	M2_009_MoveAndTick(Scene, Player, FVector(M2_009ExitLocationX, 0.0f, M2_009PlayerZ));
	TestTrue(TEXT("the locked exit overlap left the run Running"),
		Scene.Session->GetState() == ERoomSessionState::Running);
	TestFalse(TEXT("the locked exit overlap produced no victory end event"), Events.EndedCount != 0);
	TestEqual(TEXT("the locked exit overlap kept the run id"), Scene.Session->GetRunId(), RunId);
	TestFalse(TEXT("the exit is still locked after the overlap"), Exit->IsExitUnlocked());
	return true;
}

// 4. Acceptance: after Cleared the first exit overlap signals BeginExit (the
//    session moves to Exiting) and every further overlap stays inert - the
//    signal is emitted exactly once.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_009ClearedExitOverlapSignalsBeginExitExactlyOnce,
	"UEMMO.Tasks.M2_009.ClearedExitOverlapSignalsBeginExitExactlyOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_009ClearedExitOverlapSignalsBeginExitExactlyOnce::RunTest(const FString& Parameters)
{
	FM2_009_Scene Scene;
	if (!Scene.Build(*this))
	{
		return true;
	}
	FM2_009_RunEvents Events;
	Events.Bind(*Scene.Session);

	APrototypeCharacter* Player = M2_009_SpawnPlayer(*this, *Scene.World,
		FVector(M2_009TriggerLocationX, 0.0f, M2_009PlayerZ));
	if (Player == nullptr)
	{
		return true;
	}
	FActorSpawnParameters ExitParams;
	ARoomExit* Exit = Scene.World->SpawnActor<ARoomExit>(
		ARoomExit::StaticClass(), FVector(M2_009ExitLocationX, 0.0f, 100.0f), FRotator::ZeroRotator, ExitParams);
	if (!TestNotNull(TEXT("the room exit spawns"), Exit))
	{
		return true;
	}
	Scene.TickTwice();

	if (!TestTrue(TEXT("the run starts"), Scene.Session->StartRoom(M2_009_MakeRoom(FName(TEXT("room_combat_01"))))))
	{
		return true;
	}
	TestTrue(TEXT("the run is cleared on purpose (setup)"), Scene.Session->MarkCleared());
	TestTrue(TEXT("the Cleared result unlocked the exit"), Exit->IsExitUnlocked());

	// First overlap of the unlocked exit: exactly one BeginExit signal.
	M2_009_MoveAndTick(Scene, Player, FVector(M2_009ExitLocationX, 0.0f, M2_009PlayerZ));
	TestTrue(TEXT("the unlocked exit overlap moved the session to Exiting"),
		Scene.Session->GetState() == ERoomSessionState::Exiting);
	TestEqual(TEXT("no end event fired around the exit signal"), Events.EndedCount, 1);

	// Leaving and re-entering: the session is already Exiting, so the exit
	// never signals again (exactly once).
	M2_009_MoveAndTick(Scene, Player, FVector(M2_009TriggerLocationX, 0.0f, M2_009PlayerZ));
	M2_009_MoveAndTick(Scene, Player, FVector(M2_009ExitLocationX, 0.0f, M2_009PlayerZ));
	TestTrue(TEXT("a re-entry keeps the session Exiting"),
		Scene.Session->GetState() == ERoomSessionState::Exiting);
	TestEqual(TEXT("still exactly one end event"), Events.EndedCount, 1);
	return true;
}

// 5. Acceptance: the saved combat map survives a reload - a fresh LoadObject
//    of /Game/UEMMO/Maps/L_CombatRoom01 carries exactly one RoomTrigger and
//    one RoomExit (plus the player start it inherited).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_009CombatRoomMapCarriesTriggerAndExit,
	"UEMMO.Tasks.M2_009.CombatRoomMapCarriesTriggerAndExit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_009CombatRoomMapCarriesTriggerAndExit::RunTest(const FString& Parameters)
{
	// Plain -game LoadObject: the map package loads with its persistent level
	// actors; no OpenLevel, no world tick needed for a class inventory. A
	// loaded-but-foreign world asset is not level-initialized, so the actors
	// are read straight from the persistent level's actor array instead of a
	// TActorIterator (which needs an initialized current level).
	UWorld* CombatMap = LoadObject<UWorld>(nullptr, TEXT("/Game/UEMMO/Maps/L_CombatRoom01.L_CombatRoom01"));
	if (!TestNotNull(TEXT("the combat map asset loads (L_CombatRoom01)"), CombatMap))
	{
		// The map is created by the editor script later in the red->green
		// flow; a missing asset is a hard failure of this acceptance item.
		AddError(TEXT("L_CombatRoom01 is missing - run Scripts/Editor/create_room_assets.py"));
		return true;
	}
	ULevel* CombatLevel = CombatMap->PersistentLevel;
	if (!TestNotNull(TEXT("the reloaded map exposes its persistent level"), CombatLevel))
	{
		return true;
	}

	int32 TriggerCount = 0;
	int32 ExitCount = 0;
	int32 PlayerStartCount = 0;
	for (AActor* Actor : CombatLevel->Actors)
	{
		if (Actor == nullptr)
		{
			continue;
		}
		if (Actor->IsA(ARoomTrigger::StaticClass()))
		{
			++TriggerCount;
		}
		else if (Actor->IsA(ARoomExit::StaticClass()))
		{
			++ExitCount;
		}
		else if (Actor->IsA(APlayerStart::StaticClass()))
		{
			++PlayerStartCount;
		}
	}
	TestEqual(TEXT("the reloaded combat map carries exactly one RoomTrigger"), TriggerCount, 1);
	TestEqual(TEXT("the reloaded combat map carries exactly one RoomExit"), ExitCount, 1);
	TestTrue(TEXT("the reloaded combat map still carries a PlayerStart"), PlayerStartCount >= 1);
	return true;
}

// 6. Capture companion: drives one real ARoomExit through its locked and
//    unlocked states in the running game world and requests one screenshot
//    per state (Artifacts/Tasks/M2-009/exit-locked.png, exit-unlocked.png).
//    Skips gracefully wherever no game viewport / player / rendering exists;
//    the -RenderOffscreen capture run on L_CombatRoom01 produces the images.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM2_009ExitStateScreenshots,
	"UEMMO.Tasks.M2_009.ExitStateScreenshots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM2_009ExitStateScreenshots::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	if (World == nullptr || GEngine == nullptr || GEngine->GameViewport == nullptr
		|| World->WorldType != EWorldType::Game)
	{
		AddInfo(TEXT("screenshot capture skipped: no running game world/viewport"));
		return true;
	}
	APrototypeCharacter* Player = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0));
	if (Player == nullptr)
	{
		AddInfo(TEXT("screenshot capture skipped: no player pawn in the game world"));
		return true;
	}
	URoomSessionSubsystem* Session = World->GetSubsystem<URoomSessionSubsystem>();
	if (Session == nullptr || Session->GetState() != ERoomSessionState::Idle)
	{
		AddInfo(TEXT("screenshot capture skipped: the game world session is not Idle"));
		return true;
	}

	// Spawn the exit cube in front of the player, inside the side-view
	// camera's frame (the capture run views the image afterwards).
	FActorSpawnParameters ExitParams;
	ARoomExit* Exit = World->SpawnActor<ARoomExit>(
		ARoomExit::StaticClass(), Player->GetActorLocation() + FVector(420.0f, 0.0f, 30.0f), FRotator::ZeroRotator, ExitParams);
	if (!TestNotNull(TEXT("the screenshot exit spawns"), Exit))
	{
		return true;
	}

	if (!TestTrue(TEXT("the capture run starts a run"), Session->StartRoom(M2_009_MakeRoom(FName(TEXT("room_combat_01"))))))
	{
		return true;
	}
	TestFalse(TEXT("the capture exit starts locked"), Exit->IsExitUnlocked());

	const FString Directory = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M2-009"));
	ADD_LATENT_AUTOMATION_COMMAND(FM2_009_ScreenshotLatentCommand(Directory / TEXT("exit-locked.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
	ADD_LATENT_AUTOMATION_COMMAND(FM2_009_MarkClearedLatentCommand(Session));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.3f));
	ADD_LATENT_AUTOMATION_COMMAND(FM2_009_ScreenshotLatentCommand(Directory / TEXT("exit-unlocked.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
	return true;
}

#endif
