// M2-006 green implementation: the room session state machine and unique run
// identity. Every rule implemented here is locked by RoomSessionTests.cpp:
// first terminal state wins, outcomes are never overwritten or re-broadcast,
// a repeated Start while Running is rejected without resetting the RunId,
// kill notifications outside Running are ignored, and world cleanup drops
// every binding so rebuilt worlds start fully independent.

#include "RoomSessionSubsystem.h"

#include "RoomDefinition.h"

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
}

void URoomSessionSubsystem::SetSessionClockSeconds(double NowSeconds)
{
	// Defensive by contract: non-finite injections are ignored (the clock
	// keeps its previous value), matching the injected-clock style of the
	// combat components.
	if (FMath::IsFinite(NowSeconds))
	{
		SessionClockSeconds = NowSeconds;
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
	++KilledCount;
	UE_LOG(LogTemp, Verbose,
		TEXT("UEMMO RoomSession: RunId %llu counted a kill of %s (total %d)."),
		CurrentRunId, *EnemyId.ToString(), KilledCount);
	return true;
}

FOnRoomSessionRunStarted& URoomSessionSubsystem::OnRunStarted()
{
	return RunStartedDelegate;
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
