#pragma once

#include "CoreMinimal.h"

#include "../Combat/System/CombatEventTypes.h"

/**
 * M5-025: the multi-pellet shot pattern - a pure value planner that turns one
 * committed shot into its per-pellet directions and world origin (the task
 * card "用配置产生同Shot的多弹丸方向和世界发射点，所有方向可复核"). No Actor,
 * no World, no damage service (contract section 0.5): the caller supplies the
 * feet world location and, optionally, the already-resolved muzzle socket
 * world location; the planner only does deterministic math and validation.
 *
 * Frozen semantics pinned by this card:
 * - Deterministic spread: the directions come from a local FRandomStream
 *   seeded by the shot-proprietary seed ("散布使用Shot专有seed"); the planner
 *   never touches any shared or global stream - two calls with the same seed
 *   and inputs produce byte-identical plans, so a shared stream would leak.
 * - Facing mirrors X only: the baseline direction is (Facing, 0, 0) and the
 *   spread offsets live in the Y (纵深) / Z (高度) plane, which the facing
 *   never mirrors ("Facing只镜像X，不锁Y"); the feet offset mirrors X the
 *   same way while Y depth and Z height pass through unchanged.
 * - Origin: feet-relative offset by default; an optional muzzle socket name
 *   prefers the caller-resolved socket location. A missing socket either
 *   falls back to the offset (named in the result detail, so the problem is
 *   locatable) or, when the config marks the socket required, refuses the
 *   whole plan ("缺必需socket可定位").
 * - One trigger, one round: a full volley of N pellets consumes exactly one
 *   round ("单次霰弹耗一发"), every pellet carries its own unique FPelletIndex
 *   (0..N-1; the single-pellet shot is always 0) and no two pellets share an
 *   index ("各PelletIndex唯一").
 * - Atomic planning: the plan is either complete (all N pellets + origin +
 *   rounds) or refused with a named reason and zero output - an insufficient
 *   pellet capacity never yields a half volley or a partial deduction
 *   ("全体容量不足无半串/半扣弹"); an out-of-range pellet count is refused the
 *   same way ("pellet_count越界拒绝").
 */

/** Why a shot-pattern plan was refused; only grows, never renumbers (0 = success). */
enum class EShotPatternReject : uint8
{
	None = 0,
	/** The pellet count is outside 1..MaxWeaponPelletCount. */
	InvalidPelletCount = 1,
	/** The spread is not finite or outside 0..MaxWeaponSpreadDegrees. */
	InvalidSpread = 2,
	/** The caller's pre-claimed pellet capacity is smaller than the volley. */
	CapacityInsufficient = 3,
	/** The config requires the muzzle socket and the caller could not resolve it. */
	RequiredSocketMissing = 4,
};

/**
 * The world origin configuration of one shot (task card: "脚底偏移/socket可选
 * 引用"). FeetOffsetCm is feet-relative: X is the forward offset (mirrored by
 * the facing), Y is the depth offset (never mirrored) and Z is the height
 * above the feet. An empty MuzzleSocketName uses the offset; a named socket
 * prefers the caller-resolved socket world location.
 */
struct FShotOriginConfig
{
	/** Feet-relative offset in centimeters: (forward * Facing, depth, height). */
	FVector FeetOffsetCm = FVector::ZeroVector;

	/** Optional muzzle socket name; empty = the feet offset owns the origin. */
	FName MuzzleSocketName;

	/**
	 * True when the named socket is mandatory: an unresolved socket refuses
	 * the whole plan instead of falling back to the offset.
	 */
	bool bRequireSocket = false;
};

/** The resolved world origin of one shot (the origin half of the plan). */
struct FShotOrigin
{
	/** The world location the pellets spawn from. */
	FVector WorldLocation = FVector::ZeroVector;

	/** True when the origin came from the caller-resolved muzzle socket. */
	bool bUsedSocket = false;

	/**
	 * True when a socket was named but unresolved and the offset fallback
	 * produced the origin (the detail names the socket - the problem is
	 * locatable). Never true when the socket resolved or was not named.
	 */
	bool bSocketFellBack = false;

	/** The socket name involved in a fallback, for locatable logs. */
	FName FallbackSocketName;
};

/** One pellet of a planned volley: its unique index and its unit direction. */
struct FShotPellet
{
	/** The unique pellet slot inside the volley (0..N-1; single pellet = 0). */
	FPelletIndex PelletIndex = InvalidCombatPelletIndex;

	/** The unit firing direction; the facing is already applied to X. */
	FVector Direction = FVector::ZeroVector;
};

/** Everything the planner needs for one shot; plain values and one optional pointer. */
struct FShotPatternRequest
{
	/** Pellets per shot; 1..MaxWeaponPelletCount. */
	int32 PelletCount = 1;

	/** Maximum angular deviation of one pellet in degrees; 0..MaxWeaponSpreadDegrees. */
	float SpreadDegrees = 0.0f;

	/** The shot-proprietary spread seed; the same seed replays the same volley. */
	uint32 Seed = 0;

	/**
	 * The pellet capacity pre-claimed for this shot by the caller (the 022/027
	 * transaction semantics: the whole volley must fit or nothing plans).
	 */
	int32 AvailablePelletSlots = 0;

	/** The origin configuration (feet offset / optional socket). */
	FShotOriginConfig Origin;

	/** The wielder's feet world location (the offset basis). */
	FVector FeetWorldLocation = FVector::ZeroVector;

	/** The facing, +1 or -1; mirrors the baseline direction and the offset X only. */
	float Facing = 1.0f;

	/**
	 * The caller-resolved muzzle socket world location; null when the socket
	 * could not be resolved (or no socket was named). The planner never
	 * resolves sockets itself - it holds no World.
	 */
	const FVector* MuzzleSocketWorldLocation = nullptr;
};

/**
 * The complete plan of one shot: origin + every pellet + the round cost, or a
 * named refusal with zero output. All-or-nothing by construction - there is
 * no partial plan to clean up.
 */
struct FShotPatternPlan
{
	/** True only when the full volley planned (origin + all pellets present). */
	bool bPlanned = false;

	/** Rounds this volley consumes: exactly 1 when planned, 0 when refused. */
	int32 RoundsConsumed = 0;

	/** The resolved world origin; valid only when bPlanned. */
	FShotOrigin Origin;

	/** The pellets in index order 0..N-1; empty when refused. */
	TArray<FShotPellet> Pellets;

	/** None when bPlanned; otherwise the named refusal cause. */
	EShotPatternReject Reject = EShotPatternReject::None;

	/** Human-readable detail naming the offending field/socket; empty when planned. */
	FString RejectDetail;
};

/**
 * Plans one shot deterministically. Refusal order: pellet count bounds, then
 * spread bounds, then the capacity transaction (the whole volley must fit in
 * AvailablePelletSlots), then the mandatory-socket check - every refusal
 * leaves the plan empty with RoundsConsumed = 0 (no half volley, no partial
 * deduction). A planned volley carries the origin, the 0..N-1 pellets in
 * index order and RoundsConsumed = 1.
 */
FShotPatternPlan PlanShotPattern(const FShotPatternRequest& Request);
