// M2-007 green implementation: the cancellable single-wave spawner. Every
// rule implemented here is locked by WaveSpawnerTests.cpp: exactly Count
// enemies born (one per 0.3 s slot, driven by the injected UpdateWave clock,
// never by a timer), a schedule that stays honest (PendingSpawns counts the
// unborn, AliveEnemyIds only the born-and-registered), explicit failure
// statuses instead of silent "pass" behavior, a once-per-enemy death binding
// whose duplicate notifications are idempotent, and a CancelWave that stops
// future births without touching any actor of the world.

#include "WaveSpawner.h"

#include "../Combat/HealthComponent.h"
#include "../Enemy/EnemyDefinition.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomSessionSubsystem.h"

#include "Engine/World.h"

namespace
{
	// Card contract: one enemy births every 0.3 s. Fixed on purpose (the
	// spawner exposes no configurability beyond the wave data).
	constexpr double M2_007SpawnIntervalSeconds = 0.3;

	// Due-time comparison tolerance: guards the float-to-double boundary of
	// callers that pass computed times (0.3 accumulated as a float can land
	// just under the exact due instant).
	constexpr double M2_007DueEpsilon = 1e-9;
}

EWaveStartResult UWaveSpawner::StartWave(const URoomDefinition* Definition, int32 InWaveIndex,
	UEnemyDefinition* EnemyDef, URoomSessionSubsystem* Session)
{
	if (State != EWaveState::Idle)
	{
		// Single-wave spawner: a second StartWave (while spawning, complete,
		// cancelled or failed) is refused without touching any bookkeeping.
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO WaveSpawner: StartWave rejected - this single-wave spawner already drives wave %d (state %d)."),
			WaveIndex, static_cast<int32>(State));
		return EWaveStartResult::RejectedAlreadyStarted;
	}
	if (Definition == nullptr || EnemyDef == nullptr)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO WaveSpawner: StartWave rejected - the room definition or the enemy definition is null (definition missing)."));
		return EWaveStartResult::RejectedInvalidRequest;
	}
	if (!Definition->Waves.IsValidIndex(InWaveIndex))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO WaveSpawner: StartWave rejected - wave index %d is out of range (the room carries %d waves)."),
			InWaveIndex, Definition->Waves.Num());
		return EWaveStartResult::RejectedInvalidRequest;
	}
	const FRoomWaveDefinition& Wave = Definition->Waves[InWaveIndex];
	if (Wave.Count <= 0 || Wave.SpawnLocations.Num() != Wave.Count)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO WaveSpawner: StartWave rejected - wave %d is illegal (Count %d vs %d spawn locations)."),
			InWaveIndex, Wave.Count, Wave.SpawnLocations.Num());
		return EWaveStartResult::RejectedInvalidRequest;
	}
	bool bLocationsFinite = true;
	for (const FVector& Location : Wave.SpawnLocations)
	{
		bLocationsFinite = bLocationsFinite
			&& FMath::IsFinite(Location.X) && FMath::IsFinite(Location.Y) && FMath::IsFinite(Location.Z);
	}
	if (!bLocationsFinite)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO WaveSpawner: StartWave rejected - wave %d carries a non-finite spawn location."), InWaveIndex);
		return EWaveStartResult::RejectedInvalidRequest;
	}
	if (Wave.EnemyId.IsNone() || Wave.EnemyId != EnemyDef->EnemyId)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO WaveSpawner: StartWave rejected - wave %d names enemy '%s' but the handed definition is '%s'."),
			InWaveIndex, *Wave.EnemyId.ToString(), *EnemyDef->EnemyId.ToString());
		return EWaveStartResult::RejectedInvalidRequest;
	}
	if (Session == nullptr)
	{
		// Without a session there is neither a world to spawn into nor a run
		// to register the births with (the "no world" failure injection).
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO WaveSpawner: StartWave rejected - no session was handed over, so there is no world and no run."));
		return EWaveStartResult::RejectedNoSession;
	}
	if (Session->GetState() != ERoomSessionState::Running)
	{
		// Integration rule: a wave may only start inside an accepted run.
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO WaveSpawner: StartWave rejected - the session is %d (only Running accepts waves)."),
			static_cast<int32>(Session->GetState()));
		return EWaveStartResult::RejectedSessionNotRunning;
	}
	if (Session->GetWorld() == nullptr)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO WaveSpawner: StartWave rejected - the session has no world to spawn into."));
		return EWaveStartResult::FailedNoWorld;
	}
	if (!Session->NotifyWaveStarted(InWaveIndex))
	{
		// Defensive: the state was checked above, so this refusal can only
		// come from a state change between the check and the notification.
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO WaveSpawner: StartWave failed - the session refused the wave %d registration."), InWaveIndex);
		return EWaveStartResult::FailedRegistration;
	}

	// All validation passed: record the wave data. The births themselves
	// happen on UpdateWave, never inside StartWave.
	SessionPtr = Session;
	EnemyDefPtr = EnemyDef;
	WaveIndex = InWaveIndex;
	SpawnLocations = Wave.SpawnLocations;
	AliveEnemyIds.Reset();
	SpawnedCount = 0;
	PendingSpawns = Wave.Count;
	bStartClockAnchored = false;
	StartClockSeconds = 0.0;
	LastFailure.Reset();
	// M2-008: the run this wave belongs to - its death binding compares the
	// session's current RunId against it to swallow stale old-run deaths.
	SpawnRunId = Session->GetRunId();
	State = EWaveState::Spawning;

	UE_LOG(LogTemp, Verbose,
		TEXT("UEMMO WaveSpawner: wave %d accepted (%d enemies, %.2f s interval); drive with UpdateWave."),
		WaveIndex, PendingSpawns, M2_007SpawnIntervalSeconds);
	return EWaveStartResult::Started;
}

EWaveState UWaveSpawner::UpdateWave(double NowSeconds)
{
	// Non-finite injections are ignored (injected-clock contract) and a
	// spawner not in the Spawning state has nothing left to birth.
	if (!FMath::IsFinite(NowSeconds) || State != EWaveState::Spawning)
	{
		return State;
	}
	if (!bStartClockAnchored)
	{
		// StartWave carries no clock; the first UpdateWave call anchors the
		// wave start, so slot i becomes due at anchor + i * interval.
		StartClockSeconds = NowSeconds;
		bStartClockAnchored = true;
	}
	while (PendingSpawns > 0)
	{
		const double DueSeconds = StartClockSeconds
			+ static_cast<double>(SpawnedCount) * M2_007SpawnIntervalSeconds;
		if (NowSeconds + M2_007DueEpsilon < DueSeconds)
		{
			break;
		}
		if (!SpawnNextEnemy())
		{
			// The wave aborted (state Failed, pending zeroed); stop driving.
			break;
		}
	}
	return State;
}

void UWaveSpawner::CancelWave()
{
	if (State == EWaveState::Spawning)
	{
		State = EWaveState::Cancelled;
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO WaveSpawner: wave %d cancelled with %d of %d enemies still unborn."),
			WaveIndex, PendingSpawns, SpawnedCount + PendingSpawns);
	}
	// Future births stop in every state; already spawned enemies and every
	// other actor of the world are deliberately left untouched (no friendly
	// fire, the card's cancel rule).
	PendingSpawns = 0;
}

EWaveState UWaveSpawner::GetState() const
{
	return State;
}

int32 UWaveSpawner::GetPendingSpawnCount() const
{
	return PendingSpawns;
}

const TArray<FName>& UWaveSpawner::GetAliveEnemyIds() const
{
	return AliveEnemyIds;
}

int32 UWaveSpawner::GetWaveIndex() const
{
	return WaveIndex;
}

const FString& UWaveSpawner::GetLastFailure() const
{
	return LastFailure;
}

bool UWaveSpawner::SpawnNextEnemy()
{
	URoomSessionSubsystem* Session = SessionPtr.Get();
	UWorld* World = (Session != nullptr) ? Session->GetWorld() : nullptr;
	UEnemyDefinition* Definition = EnemyDefPtr.Get();
	if (Session == nullptr || World == nullptr || Definition == nullptr)
	{
		return FailWave(TEXT("the session, its world or the enemy definition was lost before a due birth"));
	}
	if (!SpawnLocations.IsValidIndex(SpawnedCount))
	{
		return FailWave(TEXT("the wave data lost the spawn location of a due birth"));
	}

	const FVector Location = SpawnLocations[SpawnedCount];
	FActorSpawnParameters Params;
	// Room definitions hand out validated, separated locations; the birth is
	// pinned to the exact configured position instead of being nudged or
	// refused by collision handling.
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AMeleeEnemy* Enemy = World->SpawnActor<AMeleeEnemy>(
		AMeleeEnemy::StaticClass(), Location, FRotator::ZeroRotator, Params);
	if (Enemy == nullptr)
	{
		return FailWave(FString::Printf(
			TEXT("SpawnActor returned null for the enemy at slot %d (location %s)"),
			SpawnedCount, *Location.ToString()));
	}
	Enemy->SetEnemyDefinition(Definition);

	// Stable in-session id: enemy kind + wave + birth slot.
	const FName EnemyId(*FString::Printf(TEXT("%s_w%d_s%d"),
		*Definition->EnemyId.ToString(), WaveIndex, SpawnedCount));
	if (!Session->NotifyEnemySpawned(EnemyId))
	{
		// The run ended (or exited) between the births: the just-born enemy
		// must never survive as a half-registered leftover of the wave.
		Enemy->Destroy();
		return FailWave(FString::Printf(
			TEXT("the session refused the registration of %s (run no longer Running)"), *EnemyId.ToString()));
	}
	// M2-010: hand the born actor to the session's run-scoped enemy list - the
	// retry cleanup (DestroyRunEnemyActors) destroys exactly these actors and
	// never a world scan of Characters. Bookkeeping only.
	Session->NotifyEnemyActorSpawned(EnemyId, Enemy);

	// One death binding per born enemy: the first death removes the alive id
	// and counts one session kill. The lambda holds weak references only, so
	// it can never outlive the spawner or the enemy.
	UHealthComponent* Health = Enemy->GetHealthComponent();
	if (Health == nullptr)
	{
		return FailWave(FString::Printf(
			TEXT("the enemy %s spawned without a health component"), *EnemyId.ToString()));
	}
	const TWeakObjectPtr<UWaveSpawner> WeakSelf(this);
	Health->OnDied.AddLambda([WeakSelf, EnemyId]()
	{
		if (UWaveSpawner* Self = WeakSelf.Get())
		{
			Self->HandleEnemyDied(EnemyId);
		}
	});

	AliveEnemyIds.Add(EnemyId);
	++SpawnedCount;
	--PendingSpawns;
	if (PendingSpawns == 0)
	{
		State = EWaveState::Complete;
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO WaveSpawner: wave %d complete - all %d enemies born and registered."),
			WaveIndex, SpawnedCount);
	}
	else
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO WaveSpawner: wave %d born slot %d as %s (%d alive, %d pending)."),
			WaveIndex, SpawnedCount - 1, *EnemyId.ToString(), AliveEnemyIds.Num(), PendingSpawns);
	}
	return true;
}

bool UWaveSpawner::FailWave(const FString& Reason)
{
	// A failed wave must never masquerade as a finished one: the state says
	// Failed and the unborn are zeroed so no settlement logic waits on them.
	State = EWaveState::Failed;
	PendingSpawns = 0;
	LastFailure = Reason;
	UE_LOG(LogTemp, Warning,
		TEXT("UEMMO WaveSpawner: wave %d failed - %s (%d enemies born before the failure)."),
		WaveIndex, *Reason, SpawnedCount);
	return false;
}

void UWaveSpawner::HandleEnemyDied(FName EnemyId)
{
	// M2-008 old-run filter: a death of this wave's enemy after its run already
	// ended (the session moved on to a new RunId, or the session/world is gone)
	// is swallowed here and never reaches the new run's kill bookkeeping - an
	// old run's leftovers must not count, advance or settle a new run.
	URoomSessionSubsystem* Session = SessionPtr.Get();
	if (Session == nullptr || Session->GetRunId() != SpawnRunId)
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO WaveSpawner: death of %s ignored - its run %llu is not the session's current run."),
			*EnemyId.ToString(), SpawnRunId);
		return;
	}
	// Once-per-enemy guard: AliveEnemyIds membership is authoritative, so a
	// duplicate death notification of the same enemy (e.g. a second death
	// lifecycle after a health reset) is ignored and never double-counts.
	if (AliveEnemyIds.RemoveSingle(EnemyId) == 0)
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO WaveSpawner: duplicate death notification of %s ignored (already removed)."),
			*EnemyId.ToString());
		return;
	}
	Session->NotifyEnemyKilled(EnemyId);
}
