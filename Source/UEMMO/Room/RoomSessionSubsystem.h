#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Subsystems/WorldSubsystem.h"
#include "RoomSessionSubsystem.generated.h"

class URoomDefinition;

/**
 * Lifecycle state of one room run (interface contract section 7).
 * Idle: no run. Running: StartRoom accepted a run that has not reached a
 * terminal state yet. Cleared / Failed: the two terminal states of a finished
 * run - they are mutually exclusive and the FIRST terminal transition wins;
 * any later request for the other (or the same) outcome is rejected and never
 * overwrites the result or re-broadcasts the end event. Exiting: the room
 * exit flow state; M2-011 owns the real exit procedure, this card only
 * implements that the state exists and which transitions are legal.
 */
enum class ERoomSessionState : uint8
{
	Idle,
	Running,
	Cleared,
	Failed,
	Exiting
};

/**
 * Result data of one finished run (interface contract section 7).
 * Pure value type on purpose: the room session never stores an inventory or
 * any other cross-run state - it only produces this result (M2-006 card).
 */
struct FRoomResult
{
	/** Monotonic run id inside the owning session; 0 = no run. */
	uint64 RunId = 0;

	/** Process-global unique settlement id; never repeats across runs or worlds. */
	uint64 SettlementId = 0;

	/** RoomId of the definition the run was started with. */
	FName RoomId;

	/** Deterministic seed derived from RunId (fixed formula, no randomness). */
	int32 Seed = 0;

	/** True when the run ended Cleared, false when it ended Failed. */
	bool bCleared = false;

	/** Enemies accepted through NotifyEnemyKilled while the run was Running. */
	int32 KilledCount = 0;

	/** Run duration in seconds, measured with the injected session clock. */
	double ElapsedSeconds = 0.0;
};

/** Fired exactly once per run, when StartRoom accepts a new run. */
DECLARE_MULTICAST_DELEGATE(FOnRoomSessionRunStarted);

/**
 * Fired exactly once per run, at its first terminal transition (MarkCleared
 * or FailRun). Late duplicates of the terminal request never fire it again.
 */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnRoomSessionRunEnded, const FRoomResult& /*Result*/);

/**
 * M2-006: per-world room session state and unique run identity (interface
 * contract section 7). One instance exists per UWorld (game and PIE worlds
 * only); it tracks one run at a time through Idle/Running/Cleared/Failed/
 * Exiting and hands out the three per-run identities - RunId (monotonic
 * inside this session), SettlementId (a process-global counter, so a new
 * world never reuses one) and Seed (a fixed, randomness-free derivation of
 * the RunId, kept reproducible for the M2-007 wave/drop consumers).
 *
 * Events fire once per run: OnRunStarted when a run starts, OnRunEnded at the
 * first terminal transition. Repeated Start requests while Running are
 * rejected WITHOUT resetting the RunId; repeated Clear/Fail requests after a
 * terminal state are rejected and never overwrite the recorded outcome;
 * kill notifications that arrive outside Running (stale callbacks from an
 * already ended or exited run) are ignored. Deinitialize (world cleanup /
 * EndPlay path) clears both delegates and all state, so nothing dangles
 * across worlds.
 *
 * Scope: this card implements state, identity and bookkeeping only. It does
 * not spawn waves (M2-007 owns WaveIndex/AliveIds/PendingSpawns and their
 * execution), does not persist inventory, and does not implement the exit
 * procedure itself (M2-011) - it only accepts the Exiting state transition.
 * Time comes exclusively from the injected session clock
 * (SetSessionClockSeconds, same style as the combat input clock); no wall
 * clock is ever read here.
 */
UCLASS()
class UEMMO_API URoomSessionSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	// -- USubsystem lifecycle ------------------------------------------------

	/** Creates the subsystem only for worlds that run room sessions (game/PIE). */
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	/**
	 * World cleanup / EndPlay path: drops every delegate binding registered by
	 * this world's session and resets all state, so a destroyed world never
	 * dangles handlers and a later world starts fresh and independent.
	 */
	virtual void Deinitialize() override;

	// -- Injected session clock ----------------------------------------------

	/**
	 * Injects the current session time in seconds (same style as the combat
	 * input clock). ElapsedSeconds of a finished run is the difference between
	 * the injections at the terminal transition and at StartRoom. Non-finite
	 * values are ignored (the clock keeps its previous value).
	 */
	void SetSessionClockSeconds(double NowSeconds);

	/** Last injected session time in seconds (0 before the first injection). */
	double GetSessionClockSeconds() const;

	// -- State machine -------------------------------------------------------

	/**
	 * Starts one run from Idle: assigns a fresh RunId / SettlementId / Seed
	 * (each unique for the lifetime they promise), stores the room id, zeroes
	 * the kill counter, captures the run start time and fires OnRunStarted
	 * exactly once. Returns false (with a diagnostic log) and changes nothing
	 * when: the definition is null, or the session is not Idle - in
	 * particular a second Start while Running is rejected and the current
	 * RunId is never reset by it.
	 */
	bool StartRoom(const URoomDefinition* Definition);

	/**
	 * First-terminal-wins: only a Running run can end Cleared. Finalizes the
	 * run result (Cleared=true, kill count, elapsed time), fires OnRunEnded
	 * exactly once and returns true. In every other state - including after a
	 * previous Cleared or Failed - the request is rejected and the recorded
	 * outcome is never overwritten.
	 */
	bool MarkCleared();

	/**
	 * First-terminal-wins: only a Running run can end Failed. Symmetric with
	 * MarkCleared; a Cleared outcome can never be replaced by a failure.
	 */
	bool FailRun();

	/**
	 * Moves the session into Exiting from any state but Exiting (the exit
	 * procedure itself is M2-011's scope; an unfinished run keeps its
	 * bookkeeping untouched here and its notifications stop being accepted).
	 * Returns false when already Exiting.
	 */
	bool BeginExit();

	/**
	 * Returns the session to Idle from a terminal state (Cleared/Failed) or
	 * from Exiting, clearing all per-run bookkeeping. Rejected while a run is
	 * still Running (it must end or exit first) and when already Idle.
	 */
	bool ResetToIdle();

	// -- Kill bookkeeping ----------------------------------------------------

	/**
	 * Counts one enemy killed for the current run. Accepted only while
	 * Running; notifications from an already ended/exited run (stale
	 * callbacks, the interface contract's idempotency rule) return false and
	 * change nothing.
	 */
	bool NotifyEnemyKilled(FName EnemyId);

	// -- Events ----------------------------------------------------------------

	/** Fires exactly once per accepted StartRoom. */
	FOnRoomSessionRunStarted& OnRunStarted();

	/** Fires exactly once per run, at its first terminal transition. */
	FOnRoomSessionRunEnded& OnRunEnded();

	// -- Introspection ---------------------------------------------------------

	ERoomSessionState GetState() const;

	/** Run id of the current (or last non-reset) run; 0 when none. */
	uint64 GetRunId() const;

	/** Settlement id of the current (or last non-reset) run; 0 when none. */
	uint64 GetSettlementId() const;

	/** Seed of the current (or last non-reset) run; 0 when none. */
	int32 GetSeed() const;

	/** RoomId of the current (or last non-reset) run; none when empty. */
	FName GetRoomId() const;

	/** Enemies accepted as killed so far in the current run. */
	int32 GetKilledCount() const;

	/** Result of the last finished run; default value before the first one. */
	const FRoomResult& GetLastResult() const;

private:
	/**
	 * Shared terminal transition body: guards the Running state, finalizes the
	 * result (bCleared decides the outcome) and broadcasts OnRunEnded once.
	 * RequestName only feeds the rejection diagnostic.
	 */
	bool EndRun(bool bInCleared, const TCHAR* RequestName);

	/** Session time comes from injections only; never read from a wall clock. */
	double SessionClockSeconds = 0.0;

	/** Lifecycle state; starts Idle. */
	ERoomSessionState State = ERoomSessionState::Idle;

	/**
	 * Total runs ever started by this session; RunId is assigned from it and
	 * it never rewinds (a repeated Start while Running must not reset ids).
	 */
	uint64 RunIdCounter = 0;

	/** Per-run bookkeeping; everything below is cleared by ResetToIdle. */
	uint64 CurrentRunId = 0;
	uint64 CurrentSettlementId = 0;
	int32 CurrentSeed = 0;
	FName CurrentRoomId;
	int32 KilledCount = 0;
	double RunStartClockSeconds = 0.0;

	/** Result of the last finished run (default before the first terminal). */
	FRoomResult LastResult;

	/** Events; Deinitialize clears them so world teardown never dangles. */
	FOnRoomSessionRunStarted RunStartedDelegate;
	FOnRoomSessionRunEnded RunEndedDelegate;
};
