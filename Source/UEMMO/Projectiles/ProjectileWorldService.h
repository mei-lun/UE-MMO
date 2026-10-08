#pragma once

#include "CoreMinimal.h"
#include <type_traits>

#include "../Combat/System/CombatEventTypes.h"
#include "../Projectiles/ProjectileTypes.h"
#include "../Weapons/ShotPattern.h"
#include "../Weapons/WeaponComponent.h"

#include "CombatProjectile.h"

class UWorld;

/**
 * M5-027: the projectile world service (interface contract section 1, owner
 * 027 "Projectiles/*" - the World-lifetime face). A plain logic class - no
 * UObject, no subsystem - that owns the World's projectile population and the
 * one-shot spawn transaction:
 *
 *   ReserveProjectiles (all-or-nothing slot reservation, refuses with a named
 *     reason and mints the reservation's instance identity)
 *     -> CommitSpawn (every pellet actor spawns or NOTHING does; a failed
 *        commit destroys what it prepared and never leaves half a volley)
 *     -> CancelReservation (a reserved-but-uncommitted volley leaves nothing).
 *
 * Lifetime faces pinned here:
 * - Epoch (interface contract 0.5: World/Session lifetime): the service holds
 *   the current generation; BeginNextWorldEpoch destroys every live actor,
 *   drops the records and mints the next epoch (map unload / room exit /
 *   retry / EndPlay all land here). A shot naming an older epoch refuses at
 *   reserve (StaleEpoch) and a reservation held across the rebuild can never
 *   commit; the live count returns to baseline 0.
 * - Timeout: the engine LifeSpan (Pause-frozen world timer) set from the
 *   definition's LifetimeS at commit.
 * - No source JSON: definitions arrive as validated value snapshots; this
 *   service includes no Data/ConfigParser/catalog header.
 * - The reservation stamps every pellet actor with the committed shot's
 *   provenance (instance ids, source, config revision).
 */

/**
 * Why ReserveProjectiles refused the reservation. Append only - never
 * renumber (0 = accepted). CommitSpawn refusals (stale epoch, wrong
 * reservation state, spawn failure) surface as a false return with the
 * reservation's own reject detail.
 */
enum class EProjectileReserveReject : uint8
{
	/** The reservation was granted. */
	None = 0,

	/** No spawn world is available to the service. */
	NullWorld = 1,

	/** The shot context does not form a usable identity (epoch/source/shot id). */
	InvalidShotContext = 2,

	/** The pellet list is empty or its size does not match Shot.PelletCount. */
	PelletCountMismatch = 3,

	/** A pellet direction is not finite or is the zero vector. */
	InvalidPelletDirection = 4,

	/** The origin is not finite. */
	InvalidOrigin = 5,

	/** The definition failed its own validation (M5-004 rules). */
	InvalidDefinition = 6,

	/** The live population plus the new volley would exceed the hard cap. */
	CapacityFull = 7,

	/**
	 * The shot names an older world generation than the service's current
	 * one: a stale volley died with its epoch (map unload / room exit /
	 * retry) and never re-enters the new one. Refused before any reservation
	 * is minted.
	 */
	StaleEpoch = 8
};

/**
 * The state of one reservation. Reserved -> (Committed | Cancelled | Failed);
 * every other transition is refused or a no-op.
 */
enum class EProjectileReservationState : uint8
{
	/** The slots are held; CommitSpawn or CancelReservation may still act. */
	Reserved = 0,

	/** Every pellet actor spawned (the reservation is spent). */
	Committed = 1,

	/** The caller released the reservation; no actor was ever spawned. */
	Cancelled = 2,

	/** A commit attempt failed mid-way; every prepared actor was destroyed. */
	Failed = 3
};

/**
 * One all-or-nothing spawn transaction (a value snapshot plus the service's
 * own bookkeeping). ReserveProjectiles fills it; CommitSpawn spends it.
 */
struct FProjectileSpawnReservation
{
	/** The service generation this reservation was minted in. */
	FCombatEpoch Epoch = InvalidCombatEpoch;

	/** The reservation's own identity (minted by the service, traceable). */
	FGuid ReservationId;

	/** The projectile instance identity minted for this volley. */
	FGuid ProjectileInstanceId;

	/** The committed shot this volley belongs to. */
	FShotContext Shot;

	/** The resolved projectile definition (a validated value snapshot). */
	FProjectileDefinition Definition;

	/** The world-space origin every pellet launches from. */
	FVector Origin = FVector::ZeroVector;

	/** One entry per pellet (index + unit direction), size == Shot.PelletCount. */
	TArray<FShotPellet> Pellets;

	/** The config revision the definitions came from (provenance stamp). */
	FString ConfigRevision;

	/** The transaction state. */
	EProjectileReservationState State = EProjectileReservationState::Reserved;

	/** Human-readable reject/failed detail; empty when nothing went wrong. */
	FString RejectDetail;
};

/**
 * The spawn seam: the service commits pellet actors through this interface so
 * a test can fail the Nth spawn and pin the no-half-volley cleanup. The
 * production implementation spawns into the bound World.
 */
struct IProjectileSpawner
{
	virtual ~IProjectileSpawner() = default;

	/** Spawns one pellet actor for the pellet slot (null = refused/failed). */
	virtual ACombatProjectile* SpawnPellet(const FProjectileSpawnReservation& Reservation, const FShotPellet& Pellet) = 0;

	/** Destroys one previously spawned pellet actor (the cleanup face). */
	virtual void DestroyPellet(ACombatProjectile& PelletActor) = 0;
};

/** Outcome of one ReserveProjectiles request: explicit decision + reason. */
struct FProjectileReserveOutcome
{
	/** True only when the whole volley is reserved. */
	bool bReserved = false;

	/** None when bReserved; otherwise the named refusal cause. */
	EProjectileReserveReject Reject = EProjectileReserveReject::None;

	/** Human-readable detail naming the offending field; empty when granted. */
	FString RejectDetail;
};

/** Hard live-population cap: a full capacity refuses new volleys explicitly. */
constexpr int32 MaxLiveProjectiles = 256;

/**
 * The World-lifetime projectile service. One instance per World (ownership
 * ladder section 0.5); plain logic, never a UObject.
 */
class FProjectileWorldService
{
public:
	/** Binds the spawn world (may be null: the service then refuses to reserve). */
	explicit FProjectileWorldService(UWorld* InWorld);

	/** The current world generation. A fresh service starts in epoch 1. */
	FCombatEpoch GetCurrentEpoch() const;

	/**
	 * World rebuild: destroys every live projectile actor, drops the records
	 * and mints the next epoch (map unload / room exit / retry / EndPlay).
	 * Returns the new current epoch.
	 */
	FCombatEpoch BeginNextWorldEpoch();

	/**
	 * Reserves the whole volley of one committed shot all-or-nothing: the
	 * shot's identity and epoch currency, the definition, the pellet geometry
	 * and the capacity are validated BEFORE any slot is granted; a refusal
	 * carries its named reason in the outcome and reserves nothing. A shot
	 * naming an older generation refuses as StaleEpoch - a stale volley never
	 * even mints a reservation.
	 */
	FProjectileReserveOutcome ReserveProjectiles(const FShotContext& Shot, const FProjectileDefinition& Definition,
		const FVector& Origin, const TArray<FShotPellet>& Pellets, const FString& ConfigRevision,
		FProjectileSpawnReservation& OutReservation);

	/**
	 * Spends a Reserved reservation: every pellet actor spawns or the prepared
	 * half is destroyed and the reservation lands in Failed. A Committed or
	 * Cancelled reservation refuses (false, detail). A reservation minted in
	 * an older epoch refuses as stale.
	 */
	bool CommitSpawn(FProjectileSpawnReservation& Reservation, IProjectileSpawner& Spawner);

	/**
	 * Releases a Reserved reservation (no actor was ever spawned). On any
	 * other state this is a no-op: a committed volley's cleanup belongs to
	 * the epoch/destroy faces, never to a late cancel.
	 */
	void CancelReservation(FProjectileSpawnReservation& Reservation);

	/** The live projectile population (the "back to baseline" face). */
	int32 GetNumLiveProjectiles() const;

private:
	/** The bound spawn world. */
	TWeakObjectPtr<UWorld> World;

	/** The current world generation. */
	FCombatEpoch CurrentEpoch;

	/** Live committed pellet actors (weak references; the actors own nothing here). */
	TArray<TWeakObjectPtr<ACombatProjectile>> LiveProjectiles;
};

// Compile-time pins: the reservation stays a value snapshot (no actor
// pointers - the actors live in the service's weak list), and the spawn
// context carries no raw pointers (interface contract 0.5).
static_assert(!std::is_pointer_v<decltype(FProjectileSpawnReservation::Shot)>, "FProjectileSpawnReservation::Shot must stay a value type");
