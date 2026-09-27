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
#include "../Enemy/EnemyDefinition.h"

namespace
{
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
	if (State != ERoomSessionState::Idle)
	{
		// In particular a second Start while Running: refused WITHOUT touching
		// the current RunId / SettlementId / Seed / room id (interface
		// contract section 7: repeated events and stale callbacks stay
		// idempotent; the running run keeps its identity).
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomSession: StartRoom rejected - the session is %d (only Idle may start a run); current RunId %llu is kept."),
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

	State = bInCleared ? ERoomSessionState::Cleared : ERoomSessionState::Failed;
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
