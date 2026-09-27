#pragma once

#include "CoreMinimal.h"
#include "Containers/Set.h"
#include "Delegates/Delegate.h"
#include "Subsystems/WorldSubsystem.h"
#include "RoomSessionSubsystem.generated.h"

class AActor;
class APrototypeCharacter;
class UEnemyDefinition;
class URoomDefinition;
class UWaveSpawner;

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
 * not execute waves (M2-007 owns their execution in UWaveSpawner; this session
 * only records the current wave index and the spawned-enemy count through the
 * M2-007 minimal Notify* additions below), does not persist inventory, and
 * does not implement the exit procedure itself (M2-011) - it only accepts the
 * Exiting state transition.
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
	 * Starts one run from Idle - or from Exiting after LeaveRoom (M2-011: the
	 * re-entered room starts a fresh run; the exit procedure already cleaned
	 * the previous run's bookkeeping) - assigning a fresh RunId /
	 * SettlementId / Seed (each unique for the lifetime they promise), storing
	 * the room id, zeroing the kill counter, capturing the run start time and
	 * firing OnRunStarted exactly once. Returns false (with a diagnostic log)
	 * and changes nothing when: the definition is null, or the session is in
	 * any other state - in particular a second Start while Running is
	 * rejected and the current RunId is never reset by it.
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

	/**
	 * M2-011: the real room exit procedure behind the M2-006 Exiting state.
	 * From Running/Failed/Cleared it moves the session into Exiting and cleans
	 * the whole run scope: every still-spawning wave is cancelled (CancelWave:
	 * future births only, no friendly fire), the run's player-death binding is
	 * dropped and every per-run weak reference is cleared (alive ids,
	 * registered enemy actors, the per-wave spawners with their pending
	 * births). An exit is NOT a settlement: OnRunEnded never fires for it, so
	 * repeated LeaveRoom calls are idempotent (no crash, no second
	 * settlement); from Idle there is nothing to leave. A late enemy death of
	 * the exited run is ignored like every other out-of-Running notification.
	 * Afterwards StartRoom is accepted again from Exiting, so the re-entered
	 * room starts a fresh run with new RunId / SettlementId.
	 */
	bool LeaveRoom();

	// -- Kill bookkeeping ----------------------------------------------------

	/**
	 * Counts one enemy killed for the current run. Accepted only while
	 * Running; notifications from an already ended/exited run (stale
	 * callbacks, the interface contract's idempotency rule) return false and
	 * change nothing.
	 */
	bool NotifyEnemyKilled(FName EnemyId);

	// -- Wave bookkeeping (M2-007 minimal additions) --------------------------

	/**
	 * M2-007: records the wave index the running run is currently executing.
	 * Accepted only while Running; rejected elsewhere (stale requests of an
	 * ended run change nothing). Bookkeeping only - the spawning itself lives
	 * in UWaveSpawner, which calls this once per accepted StartWave.
	 */
	bool NotifyWaveStarted(int32 WaveIndex);

	/** M2-007: wave index of the last accepted NotifyWaveStarted; -1 when none. */
	int32 GetCurrentWaveIndex() const;

	/**
	 * M2-007: registers one enemy successfully spawned for the running run.
	 * Accepted only while Running; false outside Running (a spawn callback of
	 * an already ended or exited run is ignored, same idempotency rule as
	 * kill notifications). The caller owns the alive/dead tracking; the
	 * session only counts registrations.
	 */
	bool NotifyEnemySpawned(FName EnemyId);

	/** M2-007: enemies accepted by NotifyEnemySpawned in the current run. */
	int32 GetSpawnedEnemyCount() const;

	// -- Wave progression (M2-008 minimal additions) ---------------------------

	/**
	 * M2-008: starts the death-driven wave progression of the running run from
	 * wave 0 of the handed room definition, spawning every wave's enemies
	 * through one fresh UWaveSpawner instance per wave (the M2-007 per-instance
	 * bookkeeping therefore always starts from zero, so a later wave can never
	 * inherit an earlier wave's counters). Accepted only while Running and
	 * while this run has no progression yet; the first wave starts immediately,
	 * every later wave starts 1.0 s (injected session clock) after the previous
	 * wave's last enemy died, and the run clears (MarkCleared, exactly once)
	 * when the LAST wave is fully born and fully dead. Any StartWave refusal or
	 * an aborted (Failed) wave ends the run through FailRun (first terminal
	 * wins). Returns false and changes nothing for a rejected request; a wave
	 * failure also returns false after the run was already failed.
	 */
	bool BeginWaves(const URoomDefinition* Definition, UEnemyDefinition* EnemyDef);

	/** M2-008: waves the current run has started so far (diagnostic). */
	int32 GetStartedWaveCount() const;

	// -- Player failure and run-scoped enemy bookkeeping (M2-010 minimal additions) ---

	/**
	 * M2-010: registers the player pawn whose death fails the running run.
	 * While a run is Running, the pawn's PlayerDied broadcast (one per death
	 * lifecycle, M2-004) cancels the current wave's future births, stops the
	 * enemies this run registered (stop, never kill) and ends the run through
	 * FailRun. Deaths outside Running (Idle/Cleared/Failed/Exiting) are
	 * ignored - a stale lifecycle broadcast can never fail or settle anything.
	 * Calling SetPlayer again with the SAME pawn is the retry-loop rebind: the
	 * guard keeps exactly one death handler bound no matter how often a retry
	 * re-registers the pawn; a different pawn unbinds the previous one first.
	 * Passing null drops the binding.
	 */
	void SetPlayer(APrototypeCharacter* Player);

	/**
	 * M2-010: records the world actor of one enemy spawned for the running run
	 * (UWaveSpawner calls this next to NotifyEnemySpawned). Accepted only while
	 * Running; the list is per-run bookkeeping cleared with the rest by
	 * StartRoom / ResetToIdle / Deinitialize. Bookkeeping only - the session
	 * never steers these actors from here.
	 */
	bool NotifyEnemyActorSpawned(FName EnemyId, AActor* Enemy);

	/**
	 * M2-010: unborn enemies still queued by the current run's wave spawners
	 * (sum of the per-wave PendingSpawns; 0 when no run or no pending births).
	 * Diagnostic surface of the interface contract's PendingSpawns state.
	 */
	int32 GetRunPendingSpawnCount() const;

	/**
	 * M2-010: destroys every still-valid actor the current (usually already
	 * failed) run registered through NotifyEnemyActorSpawned and returns how
	 * many were destroyed. This is the retry cleanup's exact destruction scope:
	 * run-registered enemies only, never a world scan over every Character.
	 * Refused while a run is Running (destroying an active run's enemies from
	 * outside is not this card's flow); the list is emptied either way.
	 */
	int32 DestroyRunEnemyActors();

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

	/** M2-008: drops the per-run wave progression bookkeeping. */
	void ResetWaveOrchestration();

	/** M2-008: starts wave StartedWaveCount of the orchestrated room; on any StartWave refusal the run is failed and false returned. */
	bool StartNextWave();

	/**
	 * M2-008: injected-clock pump, called from every accepted clock injection.
	 * Drives the current wave's births (UpdateWave), fails the run when the
	 * current wave aborted, and starts the next wave once the 1.0 s inter-wave
	 * wait has elapsed. No-op while no progression is active or the run is not
	 * Running.
	 */
	void PumpWaveProgression();

	/**
	 * M2-008: death-driven progression step, called from NotifyEnemyKilled for
	 * a kill of an id the current run actually spawned. Advances only when the
	 * current wave is fully born AND fully dead (PendingSpawns=0 and
	 * AliveIds=0): the last wave clears the run exactly once, an earlier wave
	 * arms the 1.0 s wait before the next one.
	 */
	void CheckWaveProgressionAfterKill();

	/**
	 * M2-010: PlayerDied handler bound by SetPlayer. Only a Running run fails
	 * from the player death: the current wave's future births are cancelled
	 * (CancelWave), the run's registered enemies are stopped (never killed)
	 * and FailRun applies first-terminal-wins. Any other state ignores the
	 * broadcast (a stale lifecycle death changes nothing).
	 */
	void HandlePlayerDied();

	/**
	 * M2-010: stops (never kills) every valid enemy actor the running run
	 * registered: stops any in-flight attack instance, halts the AI controller
	 * movement when one is wired, and zeroes the movement velocity. Alive ids
	 * stay alive - stopping is not destroying.
	 */
	void StopRunEnemies();

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

	/**
	 * M2-007 per-run wave bookkeeping; reset by StartRoom / ResetToIdle /
	 * Deinitialize like the rest of the per-run fields.
	 */
	int32 CurrentWaveIndex = -1;
	int32 SpawnedEnemyCount = 0;

	/**
	 * M2-008 per-run wave progression; reset by StartRoom / ResetToIdle /
	 * Deinitialize like the rest of the per-run fields. AliveEnemyIdSet is the
	 * idempotency guard of the progression: only a kill of an id this run
	 * actually spawned (NotifyEnemySpawned) may advance the waves, while the
	 * M2-006 kill-counting semantics stay untouched. RunWaveSpawners holds one
	 * fresh UWaveSpawner per started wave, so every wave's pending/alive
	 * bookkeeping starts from zero (per-instance isolation).
	 */
	TSet<FName> CurrentRunAliveEnemyIds;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UWaveSpawner>> RunWaveSpawners;
	int32 StartedWaveCount = 0;
	bool bWaveOrchestrationActive = false;
	bool bWaitingNextWave = false;
	double NextWaveStartClockSeconds = 0.0;
	TWeakObjectPtr<const URoomDefinition> WaveRoomDefPtr;
	TWeakObjectPtr<UEnemyDefinition> WaveEnemyDefPtr;

	/**
	 * M2-010 per-run enemy actor bookkeeping (weak references; parallel to the
	 * spawned ids, but corpses are kept too so a retry cleanup can also remove
	 * the failed run's bodies). Cleared by StartRoom / ResetToIdle /
	 * Deinitialize like the rest of the per-run fields; only
	 * DestroyRunEnemyActors ever destroys through it.
	 */
	TArray<TWeakObjectPtr<AActor>> CurrentRunEnemyActors;

	/**
	 * M2-010: the registered player pawn (weak) and its PlayerDied binding
	 * handle. The handle is the duplicate-binding guard: re-registering the
	 * same pawn never stacks a second handler.
	 */
	TWeakObjectPtr<APrototypeCharacter> PlayerPtr;
	FDelegateHandle PlayerDiedHandle;

	/** Result of the last finished run (default before the first terminal). */
	FRoomResult LastResult;

	/** Events; Deinitialize clears them so world teardown never dangles. */
	FOnRoomSessionRunStarted RunStartedDelegate;
	FOnRoomSessionRunEnded RunEndedDelegate;
};
