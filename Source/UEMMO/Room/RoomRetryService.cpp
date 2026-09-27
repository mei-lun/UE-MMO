// M2-010: the retry entry of a failed room run. One call cleans the failed
// run's own objects, revives the player and starts a fresh run - see the
// class comment in RoomRetryService.h for the full contract.

#include "RoomRetryService.h"

#include "RoomSessionSubsystem.h"
#include "../PrototypeCharacter.h"
#include "../Enemy/EnemyDefinition.h"
#include "RoomDefinition.h"

#include "Engine/World.h"

bool URoomRetryService::RetryRoom(UWorld* World, const URoomDefinition* Definition,
	UEnemyDefinition* EnemyDef, APrototypeCharacter* Player)
{
	if (World == nullptr || Definition == nullptr || EnemyDef == nullptr || Player == nullptr)
	{
		// A retry without a world/session, definitions or a pawn to revive is
		// a misuse: refuse without touching anything.
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomRetryService: RetryRoom refused - world, definition, enemy definition or player is null."));
		return false;
	}
	URoomSessionSubsystem* Session = World->GetSubsystem<URoomSessionSubsystem>();
	if (Session == nullptr)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomRetryService: RetryRoom refused - the world carries no room session subsystem."));
		return false;
	}
	if (Session->GetState() != ERoomSessionState::Failed)
	{
		// The retry entry belongs to a FAILED run (the player-death outcome).
		// A Running/Idle/Cleared/Exiting session has no retry: refusing keeps
		// an active run's enemies, bookkeeping and outcome untouched.
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomRetryService: RetryRoom refused - the session is %d (only a Failed run can be retried)."),
			static_cast<int32>(Session->GetState()));
		return false;
	}

	// 1. Clean up exactly the failed run's own objects: destroy ONLY the
	//    enemies this run registered (the session's run-scoped actor list -
	//    corpses included) and never a world scan over every Character. The
	//    destruction fires no health death event, so no kill bookkeeping or
	//    wave progression can run from the cleanup.
	const int32 DestroyedCount = Session->DestroyRunEnemyActors();

	// 2. Reset the session to Idle: this drops the old run's spawner instances
	//    (their enemy death bindings hold weak references and the spawner's
	//    RunId filter swallows stale old-run deaths anyway), the per-wave
	//    pending bookkeeping (no FTimerHandle exists anywhere - the
	//    progression is injected-clock driven) and every per-run counter.
	if (!Session->ResetToIdle())
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomRetryService: RetryRoom aborted - the session refused the reset to Idle."));
		return false;
	}

	// 3. Revive the player BEFORE the fresh run starts: full HP (a fresh death
	//    lifecycle), dead flag dropped, body back at the spawn point - the
	//    M2-004 reset entry, the same one the unified F2 reset uses. The reset
	//    never broadcasts PlayerDied, so no failure request can fire from it.
	//    Re-registering the pawn is the retry-loop rebind: the SetPlayer guard
	//    keeps exactly one death handler bound no matter how many retries run.
	Player->ApplyTrainingRoomReset();
	Session->SetPlayer(Player);

	// 4. Start the fresh run (new RunId / SettlementId / Seed; no reward, kill
	//    count or wave bookkeeping of the old run survives) and restart the
	//    wave progression from wave 0.
	if (!Session->StartRoom(Definition))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomRetryService: RetryRoom aborted - the session refused the fresh StartRoom."));
		return false;
	}
	if (!Session->BeginWaves(Definition, EnemyDef))
	{
		// StartWaves applied its own failure path if it refused (the run would
		// be Failed again); the retry reports honestly either way.
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomRetryService: RetryRoom aborted - the session refused BeginWaves for the fresh run."));
		return false;
	}

	UE_LOG(LogTemp, Display,
		TEXT("UEMMO RoomRetryService: retry complete - destroyed %d old run enemy actor(s), new RunId %llu (SettlementId %llu), player revived."),
		DestroyedCount, Session->GetRunId(), Session->GetSettlementId());
	return true;
}
