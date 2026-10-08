#pragma once

#include "CoreMinimal.h"

#include "../Combat/System/CombatEventTypes.h"
#include "../Combat/System/HitLedger.h"
#include "../Combat/System/UnifiedHitApplier.h"
#include "../Weapons/ShotPattern.h"
#include "../Weapons/WeaponComponent.h"

#include <type_traits>

class AActor;
class UWorld;
class FCombatEntityRegistry;

/**
 * M5-026: the hitscan delivery executor (interface contract section 1,
 * owner 026..032 "Projectiles/*"). It turns one committed shot's geometry
 * into world queries and unified hit submissions:
 *
 *   FShotContext + origin + planned pellets (M5-025 FShotPellet values)
 *     -> per-pellet world line trace (RangeCm, world obstacles block)
 *     -> target filtering (self never, friendly per filter, unregistered
 *        actors are environment that blocks)
 *     -> FUnifiedHitRequest per hit -> ApplyUnifiedHit (M5-012)
 *
 * Contract pinned here:
 * - No fake projectile actors: the executor is a pure query-and-submit
 *   pipeline. It spawns nothing, so after an execution the World's actor
 *   population is exactly what it was before.
 * - The key stays complete: every hit submission carries the full five-tuple
 *   (Epoch, Source, ShotId, PelletIndex, TargetId) - the pellet index from
 *   the planned pellet value, never a trace-order guess.
 * - No direct health writes: the executor never touches UHealthComponent.
 *   Damage and control flow exclusively through ApplyUnifiedHit, so the
 *   ledger dedup (same Shot + same Target replays are refused), the faction
 *   filter and the death/get-up gates all stay in the one owned entry.
 * - The executor never edits public services: the registry, the ledger and
 *   the unified applier are consumed read-only (the ledger's dedup commit is
 *   its own owned state, written by its own entry).
 *
 * Pointer rules (interface contract 0.5): every request and outcome here is
 * a value snapshot; World/registry/ledger arrive as references per call and
 * the only actor reference handed downstream is the hit actor weakly held by
 * FUnifiedHitRequest::TargetActor.
 */

/**
 * Why ExecuteHitscan refused the whole request before any trace. Append only
 * - never renumber, so log lines and tests keep their meaning. 0 always means
 * "not refused by the executor". A hit-level refusal (a pellet blocked by a
 * wall or a friendly, a target refused by the unified entry) is not an
 * executor reject: those surface inside the per-hit records.
 */
enum class EHitscanReject : uint8
{
	/** The request was accepted and traced. */
	None = 0,

	/** A null World was passed: no queries are possible. */
	NullWorld = 1,

	/** The shot context does not form a usable identity (epoch/source/shot id). */
	InvalidShotContext = 2,

	/** The trace origin is not finite. */
	InvalidOrigin = 3,

	/** The shot's RangeCm is not finite or not positive. */
	InvalidRange = 4,

	/** The pellet list is empty or its size does not match Shot.PelletCount. */
	PelletCountMismatch = 5,

	/** Two pellets carry the same index, or an index is not inside 0..N-1. */
	DuplicatePelletIndex = 6,

	/** A pellet direction is not finite or is the zero vector. */
	InvalidDirection = 7,

	/** No entity registry was passed: self/friendly filtering is impossible. */
	MissingRegistry = 8,

	/** No hit ledger was passed: hits could not go through the unified entry. */
	MissingLedger = 9,

	/** No target-identity resolver was passed: hits could not name a TargetId. */
	MissingTargetIdentity = 10
};

/**
 * The executor's target filter (the card's "friendly fire per Filter").
 * The shooter itself is always ignored; every other trace candidate falls
 * into exactly one of: registered friendly, registered hostile or
 * unregistered environment.
 */
struct FHitscanFilter
{
	/**
	 * True: a registered same-faction actor blocks the shot where it stands
	 * (the pellet ends there, no damage - the unified entry would refuse the
	 * friendly hit anyway). False: friendlies are passed through.
	 */
	bool bFriendliesBlockShot = true;

	/**
	 * True: an actor the target identity cannot name (environment, an
	 * unregistered body, a wall) blocks the shot like world geometry. False:
	 * unregistered bodies are passed through.
	 */
	bool bUnregisteredActorsBlockShot = true;
};

/**
 * One committed shot's hitscan delivery request. Value snapshot: the shot's
 * public context (M5-022), the resolved world origin, the planned pellets
 * (M5-025 FShotPellet values - index + unit direction) and the caller's
 * per-hit damage profile (resolved from the catalog by the caller; the
 * executor never reads config services).
 */
struct FHitscanRequest
{
	/** The committed shot this delivery delivers. */
	FShotContext Shot;

	/** The world-space trace origin (the resolved muzzle/feet origin). */
	FVector Origin = FVector::ZeroVector;

	/** One entry per pellet of the shot (size must equal Shot.PelletCount). */
	TArray<FShotPellet> Pellets;

	/** The attack profile every hit applies (already resolved by the caller). */
	FDamageProfile AttackProfile;
};

/** One pellet's hit submission through the unified entry. */
struct FHitscanHitRecord
{
	/** The pellet this record belongs to. */
	FPelletIndex PelletIndex = InvalidCombatPelletIndex;

	/** The full five-tuple key the hit was submitted under. */
	FCombatEventKey Key;

	/** The target the hit was submitted against. */
	FEntityId TargetEntityId = InvalidCombatTargetId;

	/** The world-space impact point the trace reported. */
	FVector HitLocation = FVector::ZeroVector;

	/** The unified entry's complete verdict (applied or refused, with reasons). */
	FUnifiedHitOutcome Outcome;
};

/**
 * The complete result of one ExecuteHitscan call. Per-pellet hit records are
 * reported for every pellet that reached the unified entry (a blocked or
 * passed-through pellet has no record); the counters summarize the rest.
 */
struct FHitscanExecutionOutcome
{
	/** True when the request passed every executor check and was traced. */
	bool bExecuted = false;

	/** None when bExecuted; otherwise the named pre-trace refusal cause. */
	EHitscanReject Reject = EHitscanReject::None;

	/** Human-readable detail naming the offending field; empty when executed. */
	FString RejectDetail;

	/** One entry per pellet that submitted a hit to the unified entry. */
	TArray<FHitscanHitRecord> Hits;

	/** How many pellets actually ran a trace (all of them when executed). */
	int32 PelletsTraced = 0;

	/** How many hit submissions the unified entry accepted (WasApplied). */
	int32 HitsApplied = 0;

	/** How many pellets were blocked by the world/a friendly/environment. */
	int32 PelletsBlocked = 0;
};

/**
 * The hit-to-entity seam: the executor must name a TargetEntityId for a hit
 * actor and the registry has no actor->id face (M5-010 contract); the caller
 * therefore injects this resolver. A plain logic interface, never stateful.
 */
struct IHitscanTargetIdentity
{
	virtual ~IHitscanTargetIdentity() = default;

	/** The registered entity id of the hit actor (InvalidCombatTargetId when unknown). */
	virtual FEntityId ResolveTargetEntityId(const AActor& HitActor) const = 0;
};

/**
 * Executes one committed shot's hitscan delivery. Game thread only.
 *
 * Refusal order (named, all-or-nothing): null world -> invalid shot context
 * -> invalid origin -> invalid range -> pellet count mismatch -> duplicate
 * pellet index -> invalid direction -> missing registry -> missing ledger ->
 * missing target identity. A refused request traces nothing and submits
 * nothing. An executed request never spawns actors and never writes health
 * directly: every hit goes through ApplyUnifiedHit with the complete key.
 */
FHitscanExecutionOutcome ExecuteHitscan(UWorld* World, const FHitscanRequest& Request,
	FCombatEntityRegistry* Registry, FHitLedger* Ledger,
	const IHitscanTargetIdentity* TargetIdentity, const FHitscanFilter& Filter = FHitscanFilter());

// Compile-time pins: value snapshots, no raw actor pointers (interface
// contract 0.5).
static_assert(!std::is_pointer_v<decltype(FHitscanHitRecord::TargetEntityId)>, "FHitscanHitRecord::TargetEntityId must stay a value type");
