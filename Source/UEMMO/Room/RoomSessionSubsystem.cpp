// M2-006 green implementation: the room session state machine and unique run
// identity. Every rule implemented here is locked by RoomSessionTests.cpp:
// first terminal state wins, outcomes are never overwritten or re-broadcast,
// a repeated Start while Running is rejected without resetting the RunId,
// kill notifications outside Running are ignored, and world cleanup drops
// every binding so rebuilt worlds start fully independent.
//
// M2-008: death-driven wave progression on top of the M2-006 state machine.
// The session orchestrates one fresh UWaveSpawner per wave (BeginWaves), the
// injected session clock drives births and the 1.0 s inter-wave wait, and an
// accepted kill of a current-run spawned enemy advances the progression.

#include "RoomSessionSubsystem.h"

#include "RoomDefinition.h"
#include "WaveSpawner.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/EnemyDefinition.h"
#include "../Enemy/MeleeEnemy.h"
#include "../PrototypeCharacter.h"

#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "../Logging/OperationLogSubsystem.h"

namespace
{
	// M3-023: logs one room-session row through the world's game instance log
	// subsystem; skipped silently without one (bare test worlds). bResponse
	// picks the Response category for run outcomes, State for transitions.
	void M3_023_LogRoom(const URoomSessionSubsystem& Session, bool bResponse, const FString& Message)
	{
		UOperationLogSubsystem* OpLog = UOperationLogSubsystem::FindForContext(&Session);
		if (OpLog == nullptr)
		{
			return;
		}
		if (bResponse)
		{
			OpLog->LogResponse(Message);
		}
		else
		{
			OpLog->LogState(Message);
		}
	}

	// Anonymous-namespace state; every symbol carries the M2_006 prefix so the
	// per-file translation unit can never collide with another module TU.

	/**
	 * Process-global settlement counter. Settlement ids must be unique for the
	 * whole process lifetime - across the runs of one session AND across the
	 * worlds of the process (M2-006 acceptance item: a new world / new run
	 * never receives a used id). A single monotonic counter is the fixed,
	 * randomness-free strategy; it is deliberately NOT reset by Deinitialize.
	 */
	uint64 GM2_006SettlementIdCounter = 0;

	/**
	 * Fixed seed-derivation constant: Knuth's multiplicative hash. The seed
	 * strategy of this card is "derive from RunId with a fixed formula" - no
	 * RNG is involved anywhere, so the same RunId always yields the same Seed
	 * and runs stay reproducible for the M2-007 wave/drop consumers.
	 */
	constexpr uint64 M2_006SeedMultiplier = 0x9E3779B97F4A7C15ULL;

	/** Folds a run id into the positive int32 range with the fixed multiplier. */
	int32 M2_006_DeriveSeed(uint64 RunId)
	{
		const uint64 Mixed = RunId * M2_006SeedMultiplier;
		return static_cast<int32>((Mixed >> 33) & 0x7FFFFFFFULL);
	}

	// M2_008: the card's inter-wave wait and its due-time tolerance (guards
	// the double boundary of accumulated injection values).
	constexpr double M2_008WaveGapSeconds = 1.0;
	constexpr double M2_008WaveGapEpsilon = 1e-9;
}

bool URoomSessionSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	if (World == nullptr)
	{
		return false;
	}
	// Room sessions only exist where a game runs (game and PIE worlds);
	// editor/inactive worlds never need the session state.
	return World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE;
}

void URoomSessionSubsystem::Deinitialize()
{
	// World cleanup / EndPlay path (the collection deinitializes its
	// subsystems inside UWorld::CleanupWorld): drop every binding this world
	// registered and wipe all state, so nothing dangles after the world dies
	// and a rebuilt world starts fresh. The RunId counter resets with the
	// instance (uniqueness is per session); the process-global settlement
	// counter deliberately survives.
	Super::Deinitialize();
	// M2-010: drop the player death binding first, so a pawn broadcasting
	// during teardown can never reach this half-destroyed session.
	if (APrototypeCharacter* Player = PlayerPtr.Get())
	{
		Player->PlayerDied.Remove(PlayerDiedHandle);
	}
	PlayerPtr = nullptr;
	PlayerDiedHandle.Reset();
	RunStartedDelegate.Clear();
	RunEndedDelegate.Clear();
	State = ERoomSessionState::Idle;
	RunIdCounter = 0;
	CurrentRunId = 0;
	CurrentSettlementId = 0;
	CurrentSeed = 0;
	CurrentRoomId = FName();
	KilledCount = 0;
	RunStartClockSeconds = 0.0;
	LastResult = FRoomResult();
	// M2-013: the result archive is world state too - a rebuilt world's fresh
	// session starts with an empty archive (the process-global settlement
	// counter deliberately survives, so ids are never handed out twice).
	FinishedRunResults.Empty();
	CurrentWaveIndex = -1;
	SpawnedEnemyCount = 0;
	ResetWaveOrchestration();
}

void URoomSessionSubsystem::ResetWaveOrchestration()
{
	// M2-008 per-run progression bookkeeping: dropped with the rest of the
	// per-run state by StartRoom / ResetToIdle / Deinitialize. The spawner
	// instances of a finished run are released here on purpose - their death
	// bindings hold weak references and the old-run filter in UWaveSpawner
	// (plus the alive-id guard below) keeps late deaths of their enemies from
	// ever touching a new run.
	CurrentRunAliveEnemyIds.Empty();
	// M2-010: the run-scoped enemy actor list is per-run bookkeeping too - it
	// is dropped here WITHOUT destroying the actors (the session itself never
	// destroys actors; URoomRetryService destroys through
	// DestroyRunEnemyActors BEFORE ResetToIdle).
	CurrentRunEnemyActors.Empty();
	RunWaveSpawners.Empty();
	StartedWaveCount = 0;
	bWaveOrchestrationActive = false;
	bWaitingNextWave = false;
	NextWaveStartClockSeconds = 0.0;
	WaveRoomDefPtr = nullptr;
	WaveEnemyDefPtr = nullptr;
}

void URoomSessionSubsystem::SetSessionClockSeconds(double NowSeconds)
{
	// Defensive by contract: non-finite injections are ignored (the clock
	// keeps its previous value), matching the injected-clock style of the
	// combat components.
	if (FMath::IsFinite(NowSeconds))
	{
		SessionClockSeconds = NowSeconds;
		// M2-008: the injected clock also drives the wave progression (due
		// births and the inter-wave wait); a session without an active
		// progression makes the pump a no-op.
		PumpWaveProgression();
	}
}

double URoomSessionSubsystem::GetSessionClockSeconds() const
{
	return SessionClockSeconds;
}

bool URoomSessionSubsystem::StartRoom(const URoomDefinition* Definition)
{
	if (Definition == nullptr)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomSession: StartRoom rejected - the room definition is null."));
		return false;
	}
	if (State != ERoomSessionState::Idle && State != ERoomSessionState::Exiting)
	{
		// In particular a second Start while Running: refused WITHOUT touching
		// the current RunId / SettlementId / Seed / room id (interface
		// contract section 7: repeated events and stale callbacks stay
		// idempotent; the running run keeps its identity). Exiting is the one
		// extra accepted source state (M2-011): the exit procedure already
		// cleaned the previous run's bookkeeping, so a player re-entering the
		// room starts a fresh run with new RunId / SettlementId.
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomSession: StartRoom rejected - the session is %d (only Idle or a post-exit Exiting may start a run); current RunId %llu is kept."),
			static_cast<int32>(State), CurrentRunId);
		return false;
	}

	// Identities: RunId is monotonic inside this session, SettlementId comes
	// from the process-global counter, Seed is the fixed RunId derivation.
	++RunIdCounter;
	CurrentRunId = RunIdCounter;
	CurrentSettlementId = ++GM2_006SettlementIdCounter;
	CurrentSeed = M2_006_DeriveSeed(CurrentRunId);
	CurrentRoomId = Definition->RoomId;
	KilledCount = 0;
	RunStartClockSeconds = SessionClockSeconds;
	CurrentWaveIndex = -1;
	SpawnedEnemyCount = 0;
	ResetWaveOrchestration();

	State = ERoomSessionState::Running;
	// M3-023: the accepted run start (the subscription rows follow from the
	// broadcast below).
	M3_023_LogRoom(*this, /*bResponse*/ false, FString::Printf(
		TEXT("Room RunStarted: run=%llu room=%s seed=%d settlement=%llu"),
		CurrentRunId, *CurrentRoomId.ToString(), CurrentSeed, CurrentSettlementId));
	RunStartedDelegate.Broadcast();
	return true;
}

bool URoomSessionSubsystem::EndRun(bool bInCleared, const TCHAR* RequestName)
{
	if (State != ERoomSessionState::Running)
	{
		// First terminal state wins: after Cleared or Failed the outcome is
		// never overwritten and OnRunEnded never fires again (the repeated
		// request is refused with a diagnostic instead).
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomSession: %s rejected - the session is %d (only Running may end a run); the recorded outcome is kept."),
			RequestName, static_cast<int32>(State));
		return false;
	}

	LastResult = FRoomResult();
	LastResult.RunId = CurrentRunId;
	LastResult.SettlementId = CurrentSettlementId;
	LastResult.RoomId = CurrentRoomId;
	LastResult.Seed = CurrentSeed;
	LastResult.bCleared = bInCleared;
	LastResult.KilledCount = KilledCount;
	LastResult.ElapsedSeconds = SessionClockSeconds - RunStartClockSeconds;

	// M2-013: snapshot the settled result into the per-run archive, exactly
	// once (EndRun only ever reaches this line once per run - first terminal
	// state wins). The archive deliberately survives ResetToIdle (results are
	// queried by RunId after the session moved on, and M3 claims rewards by
	// SettlementId later); only Deinitialize drops it with the world.
	FinishedRunResults.Add(CurrentRunId, LastResult);

	State = bInCleared ? ERoomSessionState::Cleared : ERoomSessionState::Failed;
	// M3-023: the run outcome (the terminal transition result; the
	// subscription rows follow from the broadcast below).
	M3_023_LogRoom(*this, /*bResponse*/ true, FString::Printf(
		TEXT("Room RunEnded: run=%llu cleared=%d killed=%d elapsed=%.2f settlement=%llu"),
		LastResult.RunId, LastResult.bCleared ? 1 : 0, LastResult.KilledCount,
		LastResult.ElapsedSeconds, LastResult.SettlementId));
	RunEndedDelegate.Broadcast(LastResult);
	return true;
}

bool URoomSessionSubsystem::MarkCleared()
{
	return EndRun(true, TEXT("MarkCleared"));
}

bool URoomSessionSubsystem::FailRun()
{
	return EndRun(false, TEXT("FailRun"));
}

bool URoomSessionSubsystem::BeginExit()
{
	if (State == ERoomSessionState::Exiting)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomSession: BeginExit rejected - the session is already Exiting."));
		return false;
	}
	// The exit procedure itself belongs to M2-011; this card only guarantees
	// that the state exists and that an unfinished run's bookkeeping stays
	// untouched (its notifications stop being accepted while Exiting).
	State = ERoomSessionState::Exiting;
	// M3-023: the exit-flow transition row (unfinished runs keep their ids).
	M3_023_LogRoom(*this, /*bResponse*/ false, FString::Printf(
		TEXT("Room BeginExit: run=%llu"), CurrentRunId));
	return true;
}

bool URoomSessionSubsystem::ResetToIdle()
{
	if (State == ERoomSessionState::Idle || State == ERoomSessionState::Running)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomSession: ResetToIdle rejected - the session is %d (only Cleared/Failed/Exiting may reset to Idle)."),
			static_cast<int32>(State));
		return false;
	}
	// M3-023: capture the finished run id before the per-run bookkeeping clears.
	const uint64 M3_023_FinishedRunId = CurrentRunId;
	State = ERoomSessionState::Idle;
	CurrentRunId = 0;
	CurrentSettlementId = 0;
	CurrentSeed = 0;
	CurrentRoomId = FName();
	KilledCount = 0;
	RunStartClockSeconds = 0.0;
	CurrentWaveIndex = -1;
	SpawnedEnemyCount = 0;
	ResetWaveOrchestration();
	// M3-023: the back-to-Idle transition row.
	M3_023_LogRoom(*this, /*bResponse*/ false, FString::Printf(
		TEXT("Room ResetToIdle: last run=%llu"), M3_023_FinishedRunId));
	return true;
}

bool URoomSessionSubsystem::LeaveRoom()
{
	if (State == ERoomSessionState::Idle)
	{
		// Nothing was ever started: there is no room to leave and no state to
		// move (the card's transition table lists Running/Failed/Cleared).
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO RoomSession: LeaveRoom ignored - the session is Idle (there is no run to leave)."));
		return false;
	}
	const bool bWasAlreadyExiting = State == ERoomSessionState::Exiting;
	// Cancel every wave that still has future births (only the current one can
	// be Spawning): CancelWave zeroes its PendingSpawns and stops the births
	// for good, while born enemies and every other actor of the world stay
	// untouched (the no-friendly-fire cancel rule).
	int32 CancelledSpawners = 0;
	for (const TObjectPtr<UWaveSpawner>& SpawnerPtr : RunWaveSpawners)
	{
		UWaveSpawner* Spawner = SpawnerPtr.Get();
		if (Spawner != nullptr && Spawner->GetState() == EWaveState::Spawning)
		{
			Spawner->CancelWave();
			++CancelledSpawners;
		}
	}
	// Drop the run's player-death binding (SetPlayer(null) unbinds): a stale
	// death broadcast after the exit can never reach this session again.
	SetPlayer(nullptr);
	// Per-run weak references and wave bookkeeping: alive ids, registered
	// enemy actors, the per-wave spawners (with any remaining pending births),
	// the orchestration flags and the definition references. Weak only: the
	// actors stay in the world - the map unload owns their destruction, and a
	// later retry/destroy helper works only on its own run's registrations.
	ResetWaveOrchestration();
	State = ERoomSessionState::Exiting;
	if (bWasAlreadyExiting)
	{
		// Idempotency: the repeated leave re-ran the same cleanup over already
		// empty bookkeeping - no crash, no state change, and (below) no event.
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO RoomSession: repeated LeaveRoom while already Exiting - idempotent, nothing further changed."));
	}
	// The exit is NOT a settlement: OnRunEnded never fires here, whatever the
	// state the run is left in (a Running run simply never ends; a Cleared or
	// Failed run keeps its already-fired result). Late events of the exited
	// run are ignored by the out-of-Running notification guards and the
	// spawners' old-run filters. Deinitialize (M2-006) covers the world-destroy
	// path with the same idempotent cleanup, so a torn-down world never dangles.
	UE_LOG(LogTemp, Verbose,
		TEXT("UEMMO RoomSession: RunId %llu left the room (state -> Exiting, %d spawning wave(s) cancelled; exit is not a settlement)."),
		CurrentRunId, CancelledSpawners);
	// M3-023: the real exit procedure row (CurrentRunId survives the exit;
	// only ResetToIdle clears it).
	M3_023_LogRoom(*this, /*bResponse*/ false, FString::Printf(
		TEXT("Room LeaveRoom: run=%llu cancelledSpawners=%d"), CurrentRunId, CancelledSpawners));
	return true;
}

bool URoomSessionSubsystem::NotifyEnemyKilled(FName EnemyId)
{
	if (State != ERoomSessionState::Running)
	{
		// Stale callback of an already ended or exited run: ignored (never
		// counted, never crashes - the interface contract's idempotency rule).
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO RoomSession: kill notification of %s ignored - the session is %d (only Running counts kills)."),
			*EnemyId.ToString(), static_cast<int32>(State));
		return false;
	}
	// M2-008 idempotency guard: only a death of an enemy this run actually
	// spawned (NotifyEnemySpawned) may advance the wave progression. Unknown,
	// stale or duplicate notifications keep the M2-006 counting semantics but
	// never settle or skip a wave.
	const bool bRunSpawnedThisId = CurrentRunAliveEnemyIds.Contains(EnemyId);
	if (bRunSpawnedThisId)
	{
		// Each spawned id leaves the alive set exactly once, so a duplicate
		// death notification of the same enemy can never double-advance.
		CurrentRunAliveEnemyIds.Remove(EnemyId);
	}
	++KilledCount;
	UE_LOG(LogTemp, Verbose,
		TEXT("UEMMO RoomSession: RunId %llu counted a kill of %s (total %d)."),
		CurrentRunId, *EnemyId.ToString(), KilledCount);
	// M3-023: the accepted kill row (state with the running kill count).
	M3_023_LogRoom(*this, /*bResponse*/ false, FString::Printf(
		TEXT("Room EnemyKilled: run=%llu enemy=%s killed=%d"),
		CurrentRunId, *EnemyId.ToString(), KilledCount));
	if (bWaveOrchestrationActive && bRunSpawnedThisId)
	{
		CheckWaveProgressionAfterKill();
	}
	return true;
}

FOnRoomSessionRunStarted& URoomSessionSubsystem::OnRunStarted()
{
	return RunStartedDelegate;
}

bool URoomSessionSubsystem::NotifyWaveStarted(int32 WaveIndex)
{
	if (State != ERoomSessionState::Running)
	{
		// Stale request of an already ended or exited run: ignored (the
		// interface contract's idempotency rule, same as kill notifications).
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO RoomSession: wave notification of wave %d ignored - the session is %d (only Running records waves)."),
			WaveIndex, static_cast<int32>(State));
		return false;
	}
	CurrentWaveIndex = WaveIndex;
	UE_LOG(LogTemp, Verbose,
		TEXT("UEMMO RoomSession: RunId %llu records wave %d."),
		CurrentRunId, CurrentWaveIndex);
	return true;
}

int32 URoomSessionSubsystem::GetCurrentWaveIndex() const
{
	return CurrentWaveIndex;
}

bool URoomSessionSubsystem::NotifyEnemySpawned(FName EnemyId)
{
	if (State != ERoomSessionState::Running)
	{
		// Stale spawn callback of an already ended or exited run: ignored
		// (never counted, never crashes - the idempotency rule again).
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO RoomSession: spawn notification of %s ignored - the session is %d (only Running counts spawns)."),
			*EnemyId.ToString(), static_cast<int32>(State));
		return false;
	}
	// M2-008: record the alive id for the progression's idempotency guard -
	// only deaths of ids this run actually spawned may advance the waves.
	CurrentRunAliveEnemyIds.Add(EnemyId);
	++SpawnedEnemyCount;
	UE_LOG(LogTemp, Verbose,
		TEXT("UEMMO RoomSession: RunId %llu registered spawn %s (total %d)."),
		CurrentRunId, *EnemyId.ToString(), SpawnedEnemyCount);
	// M3-023: the accepted spawn row (state with the running spawn count).
	M3_023_LogRoom(*this, /*bResponse*/ false, FString::Printf(
		TEXT("Room EnemySpawned: run=%llu enemy=%s spawned=%d"),
		CurrentRunId, *EnemyId.ToString(), SpawnedEnemyCount));
	return true;
}

int32 URoomSessionSubsystem::GetSpawnedEnemyCount() const
{
	return SpawnedEnemyCount;
}

// -- M2-008: death-driven wave progression -----------------------------------

bool URoomSessionSubsystem::BeginWaves(const URoomDefinition* Definition, UEnemyDefinition* EnemyDef)
{
	if (State != ERoomSessionState::Running)
	{
		// The card's integration rule: the progression belongs to an accepted
		// run and is started exactly once for it.
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomSession: BeginWaves rejected - the session is %d (only Running accepts a progression)."),
			static_cast<int32>(State));
		return false;
	}
	if (Definition == nullptr || EnemyDef == nullptr)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomSession: BeginWaves rejected - the room definition or the enemy definition is null."));
		return false;
	}
	if (Definition->Waves.Num() == 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomSession: BeginWaves rejected - the room carries no waves."));
		return false;
	}
	if (bWaveOrchestrationActive)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomSession: BeginWaves rejected - this run already drives its waves (RunId %llu)."),
			CurrentRunId);
		return false;
	}

	// The progression owns the room/enemy definitions weakly and starts wave 0
	// immediately; every later wave starts 1.0 s (injected clock) after the
	// previous wave's last enemy died.
	bWaveOrchestrationActive = true;
	bWaitingNextWave = false;
	NextWaveStartClockSeconds = 0.0;
	WaveRoomDefPtr = Definition;
	WaveEnemyDefPtr = EnemyDef;
	if (!StartNextWave())
	{
		// StartNextWave applied the card's failure path (FailRun) already; the
		// request fails with the run.
		return false;
	}
	// M3-023: the wave-orchestration row (wave 0 started immediately, the
	// rest follow through StartNextWave's rows below).
	M3_023_LogRoom(*this, /*bResponse*/ false, FString::Printf(
		TEXT("Room BeginWaves: run=%llu waves=%d"), CurrentRunId, Definition->Waves.Num()));
	return true;
}

int32 URoomSessionSubsystem::GetStartedWaveCount() const
{
	return StartedWaveCount;
}

bool URoomSessionSubsystem::StartNextWave()
{
	const URoomDefinition* Room = WaveRoomDefPtr.Get();
	UEnemyDefinition* Enemy = WaveEnemyDefPtr.Get();
	if (Room == nullptr || Enemy == nullptr)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomSession: wave %d cannot start - the room or enemy definition was lost mid-run; failing the run."),
			StartedWaveCount);
		FailRun();
		return false;
	}
	if (!Room->Waves.IsValidIndex(StartedWaveCount))
	{
		// Unreachable through the progression itself (the last wave clears
		// instead of arming a wait); a direct misuse fails the run honestly.
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomSession: wave %d is out of range (the room carries %d waves); failing the run."),
			StartedWaveCount, Room->Waves.Num());
		FailRun();
		return false;
	}

	// One fresh spawner instance per wave: the M2-007 per-instance bookkeeping
	// (PendingSpawns / AliveEnemyIds) therefore always starts from zero, so
	// wave N+1 can never inherit wave N's counters.
	UWaveSpawner* Spawner = NewObject<UWaveSpawner>(this);
	const EWaveStartResult Result = Spawner->StartWave(Room, StartedWaveCount, Enemy, this);
	if (Result != EWaveStartResult::Started)
	{
		// The card's failure path: ANY refused wave start ends the run as
		// Failed (first terminal state wins; nothing was spawned or queued).
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomSession: wave %d was refused (result %d) - failing the run."),
			StartedWaveCount, static_cast<int32>(Result));
		FailRun();
		return false;
	}
	RunWaveSpawners.Add(Spawner);
	++StartedWaveCount;
	bWaitingNextWave = false;
	UE_LOG(LogTemp, Verbose,
		TEXT("UEMMO RoomSession: RunId %llu started wave %d of %d."),
		CurrentRunId, StartedWaveCount - 1, Room->Waves.Num());
	// M3-023: the accepted wave-start row (M2-008 progression).
	M3_023_LogRoom(*this, /*bResponse*/ false, FString::Printf(
		TEXT("Room WaveStarted: run=%llu wave=%d of %d"),
		CurrentRunId, StartedWaveCount - 1, Room->Waves.Num()));
	return true;
}

void URoomSessionSubsystem::PumpWaveProgression()
{
	// The injected clock drives the whole progression: due births of the
	// current wave, the failure path of an aborted wave, and the start of the
	// next wave once the 1.0 s inter-wave wait has elapsed. A session without
	// an active progression (or outside Running) makes this a no-op.
	if (!bWaveOrchestrationActive || State != ERoomSessionState::Running)
	{
		return;
	}
	for (;;)
	{
		UWaveSpawner* Current = RunWaveSpawners.Num() > 0 ? RunWaveSpawners.Last().Get() : nullptr;
		if (Current != nullptr)
		{
			Current->UpdateWave(SessionClockSeconds);
			if (Current->GetState() == EWaveState::Failed)
			{
				// A mid-wave birth failure aborted the accepted wave (M2-007
				// zeroed its pending spawns, so it can never masquerade as a
				// finished one): the run can never honestly complete, so the
				// failure path ends it here.
				UE_LOG(LogTemp, Warning,
					TEXT("UEMMO RoomSession: wave %d aborted in Spawning - failing the run."),
					Current->GetWaveIndex());
				FailRun();
				return;
			}
		}
		if (!bWaitingNextWave
			|| SessionClockSeconds + M2_008WaveGapEpsilon < NextWaveStartClockSeconds)
		{
			// No wait armed, or the 1.0 s gap has not elapsed yet.
			return;
		}
		if (!StartNextWave())
		{
			// FailRun was applied inside; the progression is over.
			return;
		}
		// Fall through and drive the freshly started wave in this same
		// injection (its first slot is due immediately at its anchor).
	}
}

void URoomSessionSubsystem::CheckWaveProgressionAfterKill()
{
	// Death-driven progression step: runs only while the progression is active
	// and the run is still Running (inside the accepted-kill path).
	if (!bWaveOrchestrationActive || State != ERoomSessionState::Running)
	{
		return;
	}
	UWaveSpawner* Current = RunWaveSpawners.Num() > 0 ? RunWaveSpawners.Last().Get() : nullptr;
	if (Current == nullptr
		|| Current->GetState() == EWaveState::Failed
		|| Current->GetPendingSpawnCount() != 0
		|| Current->GetAliveEnemyIds().Num() != 0)
	{
		// The wave is not settled: an enemy is still unborn or still alive
		// (or the wave already aborted) - never advance on a partial state.
		return;
	}
	const URoomDefinition* Room = WaveRoomDefPtr.Get();
	if (Room != nullptr && StartedWaveCount >= Room->Waves.Num())
	{
		// The LAST wave is fully born and fully dead: the run clears exactly
		// here and exactly once (MarkCleared is first-terminal-wins, so any
		// later duplicate request can never re-fire the end event).
		bWaveOrchestrationActive = false;
		MarkCleared();
		return;
	}
	// More waves remain: wait 1.0 s on the injected clock before the next one.
	bWaitingNextWave = true;
	NextWaveStartClockSeconds = SessionClockSeconds + M2_008WaveGapSeconds;
	UE_LOG(LogTemp, Verbose,
		TEXT("UEMMO RoomSession: RunId %llu wave %d is fully dead - waiting 1.0 s before wave %d."),
		CurrentRunId, StartedWaveCount - 1, StartedWaveCount);
}

FOnRoomSessionRunEnded& URoomSessionSubsystem::OnRunEnded()
{
	return RunEndedDelegate;
}

// -- M2-010: player failure and run-scoped enemy bookkeeping ------------------

void URoomSessionSubsystem::SetPlayer(APrototypeCharacter* Player)
{
	if (PlayerPtr.Get() == Player)
	{
		// Already bound to this pawn: the retry flow re-registers the same
		// pawn on every retry, and this guard keeps exactly one PlayerDied
		// handler bound no matter how many retries run (one death broadcast
		// must stay exactly one failure request, never a stacked fan-out).
		return;
	}
	// A different pawn (or null): unbind the previous pawn's handler first so
	// the old binding never dangles. A destroyed previous pawn needs no
	// unbind - its delegate object died with it.
	if (APrototypeCharacter* Previous = PlayerPtr.Get())
	{
		Previous->PlayerDied.Remove(PlayerDiedHandle);
	}
	PlayerPtr = Player;
	PlayerDiedHandle.Reset();
	if (Player != nullptr)
	{
		// The lambda captures the subsystem only; the delegate lives on the
		// pawn, so world teardown cannot dangle either side (Deinitialize
		// additionally removes this binding explicitly).
		PlayerDiedHandle = Player->PlayerDied.AddLambda([this]()
		{
			HandlePlayerDied();
		});
	}
}

bool URoomSessionSubsystem::NotifyEnemyActorSpawned(FName EnemyId, AActor* Enemy)
{
	if (State != ERoomSessionState::Running)
	{
		// Stale registration of an already ended or exited run: ignored (the
		// interface contract's idempotency rule, same as the other Notify*).
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO RoomSession: actor registration of %s ignored - the session is %d (only Running registers run enemies)."),
			*EnemyId.ToString(), static_cast<int32>(State));
		return false;
	}
	if (Enemy == nullptr)
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO RoomSession: actor registration of %s ignored - the actor is null."),
			*EnemyId.ToString());
		return false;
	}
	// The spawner registers every born enemy exactly once next to its id
	// registration, so no dedupe is needed here. Weak references only: the
	// session never keeps an enemy alive.
	CurrentRunEnemyActors.Add(Enemy);
	UE_LOG(LogTemp, Verbose,
		TEXT("UEMMO RoomSession: RunId %llu registered run enemy actor %s (total %d)."),
		CurrentRunId, *EnemyId.ToString(), CurrentRunEnemyActors.Num());
	return true;
}

int32 URoomSessionSubsystem::GetRunPendingSpawnCount() const
{
	// Sum of the per-wave pending births (interface contract section 7 lists
	// PendingSpawns as session-visible state; the spawners are per-run).
	int32 Pending = 0;
	for (const TObjectPtr<UWaveSpawner>& SpawnerPtr : RunWaveSpawners)
	{
		const UWaveSpawner* Spawner = SpawnerPtr.Get();
		if (Spawner != nullptr)
		{
			Pending += Spawner->GetPendingSpawnCount();
		}
	}
	return Pending;
}

int32 URoomSessionSubsystem::DestroyRunEnemyActors()
{
	if (State == ERoomSessionState::Running)
	{
		// Destroying the active run's enemies from outside is not any flow of
		// this card: the retry cleanup only ever runs against an already
		// failed (or otherwise non-Running) run.
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomSession: DestroyRunEnemyActors refused - the session is Running (RunId %llu); destroy nothing."),
			CurrentRunId);
		return 0;
	}
	int32 DestroyedCount = 0;
	for (const TWeakObjectPtr<AActor>& EnemyPtr : CurrentRunEnemyActors)
	{
		AActor* Enemy = EnemyPtr.Get();
		if (Enemy != nullptr && IsValid(Enemy))
		{
			// Destroy() fires no health death event, so no kill bookkeeping,
			// wave progression or death binding can ever run from a cleanup.
			Enemy->Destroy();
			++DestroyedCount;
		}
	}
	UE_LOG(LogTemp, Verbose,
		TEXT("UEMMO RoomSession: destroyed %d of %d registered run enemies (cleanup scope: this run only)."),
		DestroyedCount, CurrentRunEnemyActors.Num());
	CurrentRunEnemyActors.Empty();
	return DestroyedCount;
}

void URoomSessionSubsystem::HandlePlayerDied()
{
	// Only a Running run fails from the player death (the M2-010 card rule):
	// a death broadcast while Idle/Cleared/Failed/Exiting is a stale or
	// out-of-run lifecycle event and must not fail, settle or re-settle
	// anything (the interface contract's idempotency rule).
	if (State != ERoomSessionState::Running)
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO RoomSession: player death ignored - the session is %d (only Running fails from the player death)."),
			static_cast<int32>(State));
		return;
	}
	// The card's failure sequence, with the end event LAST so every observer
	// reads the post-failure state: stop future births of the current wave,
	// stop (never kill) the run's registered enemies, then FailRun applies
	// first-terminal-wins and broadcasts OnRunEnded exactly once.
	int32 CancelledSpawners = 0;
	for (const TObjectPtr<UWaveSpawner>& SpawnerPtr : RunWaveSpawners)
	{
		UWaveSpawner* Spawner = SpawnerPtr.Get();
		if (Spawner != nullptr && Spawner->GetState() == EWaveState::Spawning)
		{
			Spawner->CancelWave();
			++CancelledSpawners;
		}
	}
	UE_LOG(LogTemp, Verbose,
		TEXT("UEMMO RoomSession: RunId %llu cancelled %d spawning wave(s) and stops its enemies for the player death."),
		CurrentRunId, CancelledSpawners);
	StopRunEnemies();
	FailRun();
}

void URoomSessionSubsystem::StopRunEnemies()
{
	// The card's stop semantics: stop the chase and the attack, NEVER kill.
	// The current spawner flow possesses no AI controller, so the minimal
	// honest path is: halt the controller movement when one is wired (native
	// AController::StopMovement), cancel any in-flight attack instance on the
	// enemy's own combat component, and zero the movement velocity. Registered
	// enemies stay alive and in the world - stopping is not destroying.
	int32 StoppedCount = 0;
	for (const TWeakObjectPtr<AActor>& EnemyPtr : CurrentRunEnemyActors)
	{
		AActor* Enemy = EnemyPtr.Get();
		AMeleeEnemy* MeleeEnemyActor = Cast<AMeleeEnemy>(Enemy);
		if (MeleeEnemyActor == nullptr)
		{
			continue;
		}
		if (AController* Controller = MeleeEnemyActor->GetController())
		{
			Controller->StopMovement();
		}
		if (UCombatComponent* Combat = MeleeEnemyActor->GetCombatComponent())
		{
			// An interrupted wind-up/instance is a cancel, not a Finished
			// event (the M2-004 death-cancel precedent).
			Combat->CancelCurrentAttack(FName(TEXT("RoomRunFailed")));
		}
		if (UCharacterMovementComponent* Movement = MeleeEnemyActor->GetCharacterMovement())
		{
			Movement->StopMovementImmediately();
		}
		++StoppedCount;
	}
	UE_LOG(LogTemp, Verbose,
		TEXT("UEMMO RoomSession: RunId %llu stopped %d registered run enemy actor(s)."),
		CurrentRunId, StoppedCount);
}

ERoomSessionState URoomSessionSubsystem::GetState() const
{
	return State;
}

uint64 URoomSessionSubsystem::GetRunId() const
{
	return CurrentRunId;
}

uint64 URoomSessionSubsystem::GetSettlementId() const
{
	return CurrentSettlementId;
}

int32 URoomSessionSubsystem::GetSeed() const
{
	return CurrentSeed;
}

FName URoomSessionSubsystem::GetRoomId() const
{
	return CurrentRoomId;
}

int32 URoomSessionSubsystem::GetKilledCount() const
{
	return KilledCount;
}

const FRoomResult& URoomSessionSubsystem::GetLastResult() const
{
	return LastResult;
}

// -- M2-013: run result archive and one-time settlement identity --------------
// Every query reads only the archive that EndRun filled once per run; nothing
// here ever re-generates an id, re-randomizes a seed or mutates the snapshot.

bool URoomSessionSubsystem::GetRunResult(uint64 RunId, FRoomResult& OutResult) const
{
	if (const FRoomResult* Found = FinishedRunResults.Find(RunId))
	{
		// Immutable value hand-out: the caller owns a copy, so a UI can keep
		// it after leaving the world and scribbling on it cannot touch the
		// session's internal state.
		OutResult = *Found;
		return true;
	}
	// Non-terminal (still Running or exited mid-run), unknown or 0 ids: the
	// out value is default-constructed and the request refused.
	OutResult = FRoomResult();
	return false;
}

uint64 URoomSessionSubsystem::GetSettlementId(uint64 RunId) const
{
	// The id was minted once at StartRoom and snapshotted with the result at
	// the terminal transition, so every query of the same run returns the
	// SAME id for the whole world lifetime; 0 only for runs that never
	// settled (or unknown ones).
	const FRoomResult* Found = FinishedRunResults.Find(RunId);
	return Found != nullptr ? Found->SettlementId : 0;
}

bool URoomSessionSubsystem::IsRewardEligible(uint64 RunId) const
{
	// The ONLY gate into the future reward service: a Cleared result. A Failed
	// result can never enter the reward path, and neither can unknown /
	// non-terminal runs. M3 will claim the reward once by SettlementId
	// (FPendingReward); this card stores no save game and grants no rewards.
	const FRoomResult* Found = FinishedRunResults.Find(RunId);
	return Found != nullptr && Found->bCleared;
}
