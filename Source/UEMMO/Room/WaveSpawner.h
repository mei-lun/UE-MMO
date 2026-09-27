#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "WaveSpawner.generated.h"

class UEnemyDefinition;
class URoomDefinition;
class URoomSessionSubsystem;

/**
 * Lifecycle of the ONE wave a UWaveSpawner drives (M2-007 single-wave spawner).
 * Idle: no StartWave accepted yet. Spawning: the wave was accepted and future
 * births are still pending, driven by UpdateWave. Complete: every requested
 * enemy of the wave was born successfully (they may still be alive or dead -
 * this is a spawn-completion state, not a run outcome). Cancelled: CancelWave
 * stopped the remaining future births; already spawned enemies are untouched.
 * Failed: a birth failed (null actor, lost world/session or a registration
 * refusal); the wave aborted with the remaining pending spawns zeroed so a
 * failed wave can never masquerade as a finished one.
 */
enum class EWaveState : uint8
{
	Idle,
	Spawning,
	Complete,
	Cancelled,
	Failed
};

/**
 * Result of StartWave. Every value but Started is an explicit refusal or
 * failure: a failed start never queues spawns, never registers anything and
 * never silently pretends the wave passed (M2-007 acceptance item).
 */
enum class EWaveStartResult : uint8
{
	/** Wave accepted; drive it with UpdateWave using the injected clock. */
	Started,

	/**
	 * Refused request data: null room or enemy definition, a wave index out
	 * of range, a wave whose Count does not match its SpawnLocations, a
	 * non-finite spawn location, or an enemy definition whose EnemyId does
	 * not match the wave's EnemyId. The diagnostic log names the detail.
	 */
	RejectedInvalidRequest,

	/**
	 * Refused because no session was handed over: without a session there is
	 * neither a world to spawn into nor a run to register the births with.
	 */
	RejectedNoSession,

	/**
	 * Refused because the session is not Running (M2-007 integration rule: a
	 * wave may only start inside an accepted run).
	 */
	RejectedSessionNotRunning,

	/** Refused because this spawner already drives a wave (single-wave). */
	RejectedAlreadyStarted,

	/** Accepted bookkeeping was impossible: the session has no world. */
	FailedNoWorld,

	/** Accepted bookkeeping was impossible: the session refused the wave. */
	FailedRegistration
};

/**
 * M2-007: cancellable single-wave spawner. StartWave reads one
 * FRoomWaveDefinition (EnemyId/Count/SpawnLocations) out of the handed room
 * definition and births exactly that many AMeleeEnemy actors at the
 * configured world-space SpawnLocations, one every SpawnInterval = 0.3 s.
 *
 * Drive model - injected clock, NO FTimerHandle anywhere: the owner calls
 * UpdateWave(double NowSeconds) with its own monotonic clock (tests pass
 * explicit times, the later room flow passes its frame clock). The FIRST
 * UpdateWave call anchors the wave start time; enemy i becomes due at
 * anchor + i * 0.3 s and is born on the first UpdateWave at or after its due
 * time (several overdue enemies may be born in one call).
 *
 * Bookkeeping that keeps settlement honest (interface contract section 7):
 * PendingSpawns counts the not-yet-born enemies and AliveEnemyIds only counts
 * enemies that were born AND registered to the session - a later room flow
 * can therefore never clear a wave while enemies are still unborn or
 * unaccounted. Every born enemy gets a stable in-session id and a one-shot
 * death binding: its first death removes the id from AliveEnemyIds and
 * notifies Session->NotifyEnemyKilled exactly once (duplicate death
 * notifications of the same enemy are ignored).
 *
 * Failure policy: StartWave validates the request and the session state up
 * front and returns an explicit result enum on every refusal; a birth that
 * fails later (SpawnActor null, lost world, registration refusal) aborts the
 * wave into EWaveState::Failed with the remaining pending spawns zeroed.
 * CancelWave only stops FUTURE births (pending -> 0); born enemies and every
 * other actor of the scene are deliberately left untouched.
 *
 * Scope: this type spawns the enemies of one wave and tracks them; it does
 * not read Data/enemies.json (the UEnemyDefinition is a parameter - the JSON
 * to asset loading belongs to the later catalog tasks), does not decide run
 * outcomes (Cleared/Failed stay with URoomSessionSubsystem) and does not
 * possess or steer the enemies (AMeleeEnemyController is a later wiring).
 */
UCLASS()
class UEMMO_API UWaveSpawner : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Starts the single wave this spawner drives, reading Waves[WaveIndex] of
	 * the definition and using EnemyDef for every enemy it will birth.
	 * Accepted only when the request is legal, the session is handed over and
	 * Running, and this spawner is still Idle; the accepted wave is also
	 * recorded in the session (NotifyWaveStarted). Returns Started only then;
	 * every refusal/failure comes back as its own explicit result enum with a
	 * diagnostic log and changes no bookkeeping (nothing queued, nothing
	 * registered). The actual births happen on UpdateWave, never inside
	 * StartWave.
	 */
	EWaveStartResult StartWave(const URoomDefinition* Definition, int32 InWaveIndex,
		UEnemyDefinition* EnemyDef, URoomSessionSubsystem* Session);

	/**
	 * Injected-clock driver: births every enemy whose scheduled time is due at
	 * NowSeconds (the first call anchors the wave start). Returns the wave
	 * state after the update, so a mid-wave birth failure is directly visible
	 * to the caller (EWaveState::Failed). Non-finite times are ignored and a
	 * spawner not in the Spawning state does nothing.
	 */
	EWaveState UpdateWave(double NowSeconds);

	/**
	 * Stops all FUTURE births of the wave (PendingSpawns -> 0, state becomes
	 * Cancelled when the wave was still spawning). Deliberately does NOT
	 * destroy or otherwise touch already spawned enemies or any other actor
	 * of the world (the card's no-friendly-fire rule).
	 */
	void CancelWave();

	/** Current wave lifecycle state. */
	EWaveState GetState() const;

	/** Enemies requested by the wave that have not been born (yet). */
	int32 GetPendingSpawnCount() const;

	/**
	 * Stable in-session ids of the enemies born AND registered to the session
	 * by this wave (the AliveIds bookkeeping; deaths remove their entry).
	 */
	const TArray<FName>& GetAliveEnemyIds() const;

	/** Wave index this spawner was started with; INDEX_NONE while Idle. */
	int32 GetWaveIndex() const;

	/**
	 * Diagnostic of the last wave failure (empty when none); request-level
	 * refusals only log and return their result enum, this message covers
	 * failures that aborted an already accepted wave.
	 */
	const FString& GetLastFailure() const;

private:
	/** Births the next pending enemy; false (with a Failed state) on any birth failure. */
	bool SpawnNextEnemy();

	/** Aborts an accepted wave: Failed state, pending zeroed, diagnostic kept. */
	bool FailWave(const FString& Reason);

	/**
	 * Death callback of one born enemy: removes its id once and notifies the
	 * session's kill bookkeeping once. AliveEnemyIds membership is the
	 * authoritative once-per-enemy guard, so duplicate death notifications of
	 * the same enemy are ignored.
	 */
	void HandleEnemyDied(FName EnemyId);

	/** Session of the run this wave belongs to (weak: never owns it). */
	TWeakObjectPtr<URoomSessionSubsystem> SessionPtr;

	/** Definition applied to every enemy of this wave (weak, caller-owned data). */
	TWeakObjectPtr<UEnemyDefinition> EnemyDefPtr;

	/** Wave index within the room definition; INDEX_NONE while Idle. */
	int32 WaveIndex = INDEX_NONE;

	/** World-space birth locations, one per scheduled enemy slot. */
	TArray<FVector> SpawnLocations;

	/** Injected clock value that anchored the wave start (first UpdateWave). */
	double StartClockSeconds = 0.0;

	/** True once the first UpdateWave anchored StartClockSeconds. */
	bool bStartClockAnchored = false;

	/** Enemy slots already born (== AliveEnemyIds.Num() while healthy). */
	int32 SpawnedCount = 0;

	/** Enemies requested but not yet born; zeroed by CancelWave and failures. */
	int32 PendingSpawns = 0;

	/** Lifecycle state; starts Idle. */
	EWaveState State = EWaveState::Idle;

	/** Ids of the born and registered enemies; deaths remove their entry. */
	TArray<FName> AliveEnemyIds;

	/** Diagnostic of the last failure that aborted an accepted wave. */
	FString LastFailure;
};
