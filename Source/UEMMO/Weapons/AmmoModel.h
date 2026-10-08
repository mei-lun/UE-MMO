#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"

#include "AmmoTypes.h"
#include "WeaponTypes.h"

/**
 * M5-021: the magazine, shared reserve and reload model (M5 interface
 * contract section 1, owner 021 "AmmoModel"; section 7 "存档与实例"). A plain
 * value-type account book over the two ammo stores the contract freezes:
 *
 * - the per-instance magazine keyed by ItemInstanceId ("WeaponStateByItemId
 *   仅记录该实例弹匣"),
 * - the shared reserve pool keyed by ammo id ("ReserveAmmoByType 记录共享
 *   备弹").
 *
 * Reload is a two-phase transaction: BeginReload only opens the reload window
 * (the caller supplies the clock seconds - this type never reads a World
 * timer), and the rounds move exactly once, atomically, when the caller
 * reports completion: granted = min(capacity - loaded, reserve). Cancelling
 * (equipment switch, death) closes the window and moves nothing - "切换/死亡
 * 取消不得转移弹药". There is no trickle transfer and no completion side
 * effect: a repeated completion is a named refusal, never a second grant.
 *
 * Guarantees pinned by this card:
 * - Explicit refusal, never a fallback: every invalid input (unknown ammo id,
 *   negative rounds, beyond-capacity loads, unknown instances, a full
 *   magazine, an empty reserve pool) is refused with a named error and the
 *   model is left completely unchanged - no half-written state.
 * - Shared reserve: rounds drain from (and only from) the pool of the
 *   instance's AmmoId; two ammo ids never touch each other's pool.
 * - Session scope only: no World/Actor/UObject/timer dependency and no
 *   serialization (the v2 save belongs to M5-046). This value-type surface is
 *   what the later WeaponComponent (020/022) reads and drives; nothing here
 *   reaches into the character or the save service.
 */

/** Why an ammo operation was refused; only grows, never renumbers (0 = success). */
enum class EAmmoModelError : uint8
{
	None = 0,
	/** The instance identity is unusable: the all-zero FGuid. */
	InvalidInstance = 1,
	/** The ammo id text is not 1-64 characters of a-z0-9_. */
	InvalidAmmoId = 2,
	/** The ammo id is not registered as a reserve pool kind. */
	UnknownAmmoType = 3,
	/** The magazine capacity is outside 1..MaxAmmoMagazineSize. */
	InvalidCapacity = 4,
	/** A rounds value is negative (initial loaded, reserve or max reserve). */
	NegativeRounds = 5,
	/** The initial loaded rounds exceed the magazine capacity. */
	BeyondCapacity = 6,
	/** A reserve value exceeds its limit (MaxReserve or MaxAmmoReserve). */
	BeyondReserveLimit = 7,
	/** This ammo id already holds a registered pool. */
	DuplicateRegistration = 8,
	/** This item instance already holds an initialized magazine. */
	DuplicateInstance = 9,
	/** The reload window inputs are unusable: non-finite clock or a negative duration. */
	InvalidDuration = 10,
	/** An active reload window is already open for this instance. */
	DuplicateReload = 11,
	/** The magazine is already full - there is nothing to refill. */
	MagazineFull = 12,
	/** The shared reserve pool of this ammo kind has no rounds left. */
	NoReserveAmmo = 13,
	/** No reload window is open for this instance (double completion / stray cancel). */
	NoActiveReload = 14,
	/** The instance holds no initialized magazine (unregistered with InitializeInstance). */
	UnknownInstance = 15,
};

/**
 * Outcome of one reload-window operation (BeginReload / CompleteReload):
 * explicit decision + reason, never a bare bool. BeginReload reports the
 * untouched counts with RoundsTransferred = 0; CompleteReload reports the
 * post-transfer counts.
 */
struct FAmmoReloadOutcome
{
	bool bSuccess = false;

	/** None when bSuccess; otherwise the named refusal cause. */
	EAmmoModelError Error = EAmmoModelError::None;

	/** Human-readable detail naming the offending id/field; empty on success. */
	FString ErrorDetail;

	/** Rounds moved by this call; only CompleteReload ever moves rounds. */
	int32 RoundsTransferred = 0;

	/** Loaded rounds of the instance after the operation. */
	int32 LoadedRounds = 0;

	/** Shared reserve rounds of the instance's ammo kind after the operation. */
	int32 ReserveRounds = 0;
};

/** The per-instance magazine slot (keyed by ItemInstanceId). */
struct FMagazineState
{
	/** The owning item instance identity. */
	FGuid InstanceId;

	/** The shared reserve kind this magazine refills from. */
	FName AmmoId;

	/** Maximum rounds the magazine holds; at least 1. */
	int32 Capacity = 0;

	/** Rounds currently in the magazine. */
	int32 LoadedRounds = 0;
};

/** One open reload window; the clock lives with the caller, only the deadline is stored. */
struct FAmmoReloadState
{
	/** Caller-clock seconds at which the window is due (BeginReload Now + Duration). */
	double EndTimeSeconds = 0.0;
};

/** The shared reserve pool of one ammo kind (keyed by AmmoId). */
struct FAmmoReservePool
{
	/** Maximum shared stock of this ammo kind. */
	int32 MaxReserve = 0;

	/** Rounds currently in the shared pool. */
	int32 Rounds = 0;
};

/**
 * The session-scoped ammo model. One model per session (the WeaponComponent
 * layer mounts it); it holds the per-instance magazines, the open reload
 * windows and the shared reserve pools. Plain value container: no World, no
 * Actor, no timers, no serialization (M5-046 owns the v2 save).
 */
class FAmmoModel
{
public:
	/**
	 * Registers the shared reserve pool of one ammo kind with its cap and the
	 * initial stock. Refuses an invalid id text, a negative or over-limit
	 * MaxReserve (0..MaxAmmoReserve), a negative or over-cap InitialReserve
	 * (0..MaxReserve) and a duplicate registration; a refusal leaves the model
	 * unchanged.
	 */
	bool RegisterAmmoType(FName AmmoId, int32 MaxReserve, int32 InitialReserve, FString* OutError = nullptr);

	/**
	 * Establishes the magazine slot of one item instance. Refuses the all-zero
	 * instance id, an unregistered ammo id, a capacity outside
	 * 1..MaxAmmoMagazineSize, initial loaded rounds outside 0..Capacity and a
	 * duplicate initialization. The initial load is explicit - the model never
	 * invents a "full magazine" policy of its own (the caller passes
	 * MagazineCapacity to start full).
	 */
	bool InitializeInstance(const FGuid& InstanceId, FName AmmoId, int32 MagazineCapacity, int32 InitialLoaded, FString* OutError = nullptr);

	/**
	 * Opens the reload window of one instance at caller-clock NowSeconds for
	 * DurationSeconds. Refusal order: unknown instance, invalid window inputs
	 * (non-finite clock, negative duration), duplicate open window, full
	 * magazine ("满弹匣不换弹"), empty reserve pool ("无备弹不换弹"). Success
	 * moves nothing: loaded and reserve stay untouched until completion.
	 */
	FAmmoReloadOutcome BeginReload(const FGuid& InstanceId, double NowSeconds, double DurationSeconds);

	/**
	 * Closes the open reload window and atomically transfers
	 * granted = min(capacity - loaded, reserve) rounds from the shared pool
	 * into the magazine - the only operation that ever moves rounds. Refuses
	 * an unknown instance and a missing open window, so a repeated completion
	 * is a named refusal, never a second grant ("重复完成回调不赠弹").
	 */
	FAmmoReloadOutcome CompleteReload(const FGuid& InstanceId);

	/**
	 * Closes the open reload window and moves nothing - the equipment-switch /
	 * death cancellation path. Returns false (model unchanged) for an unknown
	 * instance or a missing open window; OutError names the reason.
	 */
	bool CancelReload(const FGuid& InstanceId, FString* OutError = nullptr);

	/** The magazine slot of this instance, or nullptr when not initialized. */
	const FMagazineState* FindMagazine(const FGuid& InstanceId) const;

	/**
	 * The shared reserve rounds of this ammo kind; -1 when the kind is not
	 * registered (distinguishing "no pool" from "an empty pool").
	 */
	int32 GetReserveRounds(FName AmmoId) const;

	/**
	 * Remaining seconds of the open reload window at caller-clock NowSeconds,
	 * clamped at 0 once the deadline passed (the completion callback remains
	 * the caller's duty). 0.0 when no window is open or the instance is
	 * unknown - "装填剩余时间" is a query, never a scheduler.
	 */
	double GetRemainingReloadSeconds(const FGuid& InstanceId, double NowSeconds) const;

	/** True when an open reload window exists for this instance. */
	bool IsReloading(const FGuid& InstanceId) const;

	/** Number of initialized magazine slots. */
	int32 NumMagazines() const;

	/** Number of registered reserve pool kinds. */
	int32 NumAmmoTypes() const;

	/** Drops every magazine, window and pool (session teardown / test isolation). */
	void Reset();

private:
	/** Per-instance magazine slots keyed by the item instance identity. */
	TMap<FGuid, FMagazineState> Magazines;

	/** Open reload windows keyed by the same identity. */
	TMap<FGuid, FAmmoReloadState> ActiveReloads;

	/** Shared reserve pools keyed by ammo id. */
	TMap<FName, FAmmoReservePool> ReservePools;
};
