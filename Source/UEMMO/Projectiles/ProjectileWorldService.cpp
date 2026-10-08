// M5-027: the projectile world service (owner 027, the World-lifetime face).
// The one-shot spawn transaction: ReserveProjectiles validates the whole
// volley (shot identity, definition, pellet geometry, capacity) before any
// slot is granted; CommitSpawn spends a reservation all-or-nothing (a
// mid-volley spawn failure destroys everything it prepared); CancelReservation
// releases an uncommitted reservation. BeginNextWorldEpoch is the map
// unload / room exit / retry face: every live actor is destroyed and the
// population returns to baseline 0; reservations minted in an older epoch
// can never commit. Definitions arrive as validated value snapshots - this
// service includes no Data/ConfigParser/catalog header.

#include "ProjectileWorldService.h"

#include "Engine/World.h"

namespace UE::UEMMO::Tasks::M5_027
{
	/** True when the vector has no NaN/Inf component. */
	static bool IsFiniteVector(const FVector& Vector)
	{
		return FMath::IsFinite(Vector.X) && FMath::IsFinite(Vector.Y) && FMath::IsFinite(Vector.Z);
	}

	/** True when the shot context forms a usable identity. */
	static bool IsValidShotIdentity(const FShotContext& Shot)
	{
		return IsValidCombatEpoch(Shot.Epoch) && IsValidCombatEntityId(Shot.SourceEntityId)
			&& IsValidCombatShotId(Shot.ShotId);
	}
}

FProjectileWorldService::FProjectileWorldService(UWorld* InWorld)
	: World(InWorld)
	, CurrentEpoch(1)
{
	// A fresh service models a live epoch 1 (the registry's own start value);
	// the ownership ladder dies this instance with its World.
}

FCombatEpoch FProjectileWorldService::GetCurrentEpoch() const
{
	return CurrentEpoch;
}

FCombatEpoch FProjectileWorldService::BeginNextWorldEpoch()
{
	// World rebuild: destroy every live actor, drop the records, mint the
	// next generation. Old-epoch reservations die with the records here.
	for (const TWeakObjectPtr<ACombatProjectile>& Live : LiveProjectiles)
	{
		ACombatProjectile* Actor = Live.Get();
		if (Actor != nullptr && IsValid(Actor))
		{
			Actor->Destroy();
		}
	}
	LiveProjectiles.Reset();
	return ++CurrentEpoch;
}

FProjectileReserveOutcome FProjectileWorldService::ReserveProjectiles(const FShotContext& Shot,
	const FProjectileDefinition& Definition, const FVector& Origin,
	const TArray<FShotPellet>& Pellets, const FString& ConfigRevision,
	FProjectileSpawnReservation& OutReservation)
{
	using namespace UE::UEMMO::Tasks::M5_027;

	FProjectileReserveOutcome Outcome;

	// Everything validates BEFORE any slot is granted: a refused request
	// reserves nothing and leaves the caller's reservation untouched.
	if (World.IsValid() == false)
	{
		Outcome.Reject = EProjectileReserveReject::NullWorld;
		Outcome.RejectDetail = TEXT("World");
		return Outcome;
	}
	if (!IsValidShotIdentity(Shot))
	{
		Outcome.Reject = EProjectileReserveReject::InvalidShotContext;
		Outcome.RejectDetail = TEXT("Shot.Epoch/SourceEntityId/ShotId");
		return Outcome;
	}
	if (Shot.Epoch != CurrentEpoch)
	{
		// A shot naming an older generation is dead on arrival: its volley
		// died with that epoch (map unload / room exit / retry) and never
		// re-enters the current one.
		Outcome.Reject = EProjectileReserveReject::StaleEpoch;
		Outcome.RejectDetail = FString::Printf(TEXT("Shot.Epoch %llu != current %llu"),
			static_cast<unsigned long long>(Shot.Epoch), static_cast<unsigned long long>(CurrentEpoch));
		return Outcome;
	}
	if (Pellets.Num() == 0 || Pellets.Num() != Shot.PelletCount)
	{
		Outcome.Reject = EProjectileReserveReject::PelletCountMismatch;
		Outcome.RejectDetail = TEXT("Pellets.Num() != Shot.PelletCount");
		return Outcome;
	}
	for (const FShotPellet& Pellet : Pellets)
	{
		if (!IsValidCombatPelletIndex(Pellet.PelletIndex) || !IsFiniteVector(Pellet.Direction) || Pellet.Direction.IsZero())
		{
			Outcome.Reject = EProjectileReserveReject::InvalidPelletDirection;
			Outcome.RejectDetail = FString::Printf(TEXT("PelletIndex %u Direction"), static_cast<uint32>(Pellet.PelletIndex));
			return Outcome;
		}
	}
	if (!IsFiniteVector(Origin))
	{
		Outcome.Reject = EProjectileReserveReject::InvalidOrigin;
		Outcome.RejectDetail = TEXT("Origin");
		return Outcome;
	}
	FString DefinitionErrors;
	if (!ValidateProjectileDefinition(Definition, DefinitionErrors))
	{
		Outcome.Reject = EProjectileReserveReject::InvalidDefinition;
		Outcome.RejectDetail = DefinitionErrors;
		return Outcome;
	}

	// Hard capacity: the live population (valid records only) plus the whole
	// new volley must fit; a full service refuses explicitly, never silently
	// truncates a volley.
	LiveProjectiles.RemoveAll([](const TWeakObjectPtr<ACombatProjectile>& Live) { return !Live.IsValid(); });
	if (LiveProjectiles.Num() + Pellets.Num() > MaxLiveProjectiles)
	{
		Outcome.Reject = EProjectileReserveReject::CapacityFull;
		Outcome.RejectDetail = FString::Printf(TEXT("LiveProjectiles %d + Pellets %d > %d"),
			LiveProjectiles.Num(), Pellets.Num(), MaxLiveProjectiles);
		return Outcome;
	}

	// Grant: mint the reservation's own identities and snapshot everything.
	OutReservation = FProjectileSpawnReservation();
	OutReservation.Epoch = CurrentEpoch;
	OutReservation.ReservationId = FGuid::NewGuid();
	OutReservation.ProjectileInstanceId = FGuid::NewGuid();
	OutReservation.Shot = Shot;
	OutReservation.Definition = Definition;
	OutReservation.Origin = Origin;
	OutReservation.Pellets = Pellets;
	OutReservation.ConfigRevision = ConfigRevision;
	OutReservation.State = EProjectileReservationState::Reserved;
	OutReservation.RejectDetail.Reset();

	Outcome.bReserved = true;
	return Outcome;
}

bool FProjectileWorldService::CommitSpawn(FProjectileSpawnReservation& Reservation, IProjectileSpawner& Spawner)
{
	// A committed reservation changes nothing when its world is gone.
	if (World.IsValid() == false)
	{
		Reservation.RejectDetail = TEXT("commit refused: World unavailable");
		return false;
	}
	if (Reservation.State != EProjectileReservationState::Reserved)
	{
		Reservation.RejectDetail = FString::Printf(TEXT("commit refused: reservation state %d"),
			static_cast<int32>(Reservation.State));
		return false;
	}
	if (Reservation.Epoch != CurrentEpoch)
	{
		// A reservation minted in an older generation can never commit: the
		// volley died with its epoch (map unload / room exit / retry).
		Reservation.RejectDetail = FString::Printf(TEXT("commit refused: stale epoch (reservation %llu, current %llu)"),
			static_cast<unsigned long long>(Reservation.Epoch), static_cast<unsigned long long>(CurrentEpoch));
		return false;
	}

	// All-or-nothing: every pellet spawns, or everything prepared so far is
	// destroyed and the reservation lands in Failed - never half a volley.
	TArray<ACombatProjectile*> Prepared;
	for (const FShotPellet& Pellet : Reservation.Pellets)
	{
		ACombatProjectile* Actor = Spawner.SpawnPellet(Reservation, Pellet);
		if (Actor == nullptr)
		{
			for (ACombatProjectile* PreparedActor : Prepared)
			{
				Spawner.DestroyPellet(*PreparedActor);
			}
			Reservation.State = EProjectileReservationState::Failed;
			Reservation.RejectDetail = FString::Printf(TEXT("spawn failed at PelletIndex %u"),
				static_cast<uint32>(Pellet.PelletIndex));
			return false;
		}
		Prepared.Add(Actor);
	}

	for (ACombatProjectile* Actor : Prepared)
	{
		LiveProjectiles.Add(Actor);
	}
	Reservation.State = EProjectileReservationState::Committed;
	Reservation.RejectDetail.Reset();
	return true;
}

void FProjectileWorldService::CancelReservation(FProjectileSpawnReservation& Reservation)
{
	// Only an uncommitted reservation releases; a committed volley's cleanup
	// belongs to the epoch/destroy faces, never to a late cancel.
	if (Reservation.State == EProjectileReservationState::Reserved)
	{
		Reservation.State = EProjectileReservationState::Cancelled;
	}
}

int32 FProjectileWorldService::GetNumLiveProjectiles() const
{
	int32 Count = 0;
	for (const TWeakObjectPtr<ACombatProjectile>& Live : LiveProjectiles)
	{
		if (Live.IsValid())
		{
			++Count;
		}
	}
	return Count;
}
