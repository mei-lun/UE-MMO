#pragma once

#include "CoreMinimal.h"
#include "UObject/NameTypes.h"
#include "Templates/TypeHash.h"

#include <type_traits>

/**
 * M5-040: the atomic single-seat occupancy/permission model (interface
 * contract section 1 owner 040, section 8 "载具权限"). A plain C++ class - no
 * UObject - that owns WHO sits WHERE as pure identity bookkeeping. It never
 * possesses a pawn, never moves one and never reads the World: Possess, input
 * handover (M5-041), movement (M5-042) and turret firing (M5-043) stay with
 * their own tasks. A later card owns the World-lifetime holder instance and
 * wires it to the live vehicles (the model itself has no World back pointer).
 *
 * Identity model - ids, never actors (acceptance: "模型不持有裸Actor，ID生命
 * 周期由World维护"):
 * - VehicleId / SeatId are FName: the VehicleTypes.h precedent (M5-037) uses
 *   FName for stable business identifiers that resolve against the source
 *   tables ("vehicle_wheeled_sample" / "seat_wheeled_driver"); NAME_None is
 *   the absent-id sentinel exactly like the M5-037 validators.
 * - OccupantId is FGuid: an occupant is a transient World-level instance, not
 *   a business kind - the contract's own split is FName for definition ids vs
 *   FGuid for instance ids (WeaponItemInstanceId / ProjectileInstanceId, M5
 *   interface contract section 1). The World wiring mints the occupant GUID
 *   (it may derive it from the M5-010 registry identity of the pawn); the
 *   model only stores and compares it and never resolves it to an object.
 * - No member of any struct here is or contains a pointer: consumers resolve
 *   ids through their own World-side maps (weak references per section 0.5).
 *   Static asserts at the bottom of this header pin that.
 *
 * Lifetime and persistence:
 * - Occupancy is World/Session state (contract section 0.5): it is NEVER
 *   serialized into the player profile/save (the Profile ladder lives at
 *   GameInstance level and must not reference World actors or transient
 *   combat state). A new world generation starts from an empty model; the
 *   wiring re-registers the seats of every vehicle it spawns (M5-038) and
 *   the save system has nothing to restore.
 * - Cleanup coverage for contract section 8 (death, destroy, LeaveRoom,
 *   Retry, World EndPlay): an occupant leaving/dying/being destroyed is one
 *   idempotent LeaveSeat call (safe to call unconditionally); a vehicle
 *   being destroyed/unloaded is ClearVehicle (occupancy drops, the seat
 *   registrations stay so the same vehicle id can be occupied again by a new
 *   occupant - "该车辆可再次被新占用者使用"); a world rebuild/unload is
 *   Reset (full factory baseline, seats included).
 *
 * Atomicity (acceptance: "双enter只占一次"):
 * - The model is single-threaded by contract (game thread only, like every
 *   other pure M5 model). "Atomic" means TryEnterSeat is all-or-nothing: it
 *   runs every check first and commits both maps in one tail mutation, so a
 *   refusal leaves the state byte-identical and a double-send of the same
 *   request succeeds exactly once (the second send observes the committed
 *   state and is refused with AlreadyInThisSeat - no partial state, no
 *   second grant).
 * - The injected gates are predicates by contract: they must not re-enter
 *   the model. They run before the commit while the state is still the
 *   pre-enter state.
 *
 * Permission checks - the three injectable gates (default: unset = allow):
 * - DistanceGate (拒绝原因 DistanceTooFar): "is the occupant close enough to
 *   the seat?". Production wiring (M5-038/041): compare the pawn location
 *   against the hull's seat socket world location (the FVehicleSeatDefinition
 *   SeatLocationCm transformed by the vehicle actor).
 * - VehicleValidityGate (拒绝原因 VehicleInvalid): "is the vehicle still
 *   enterable?". Production wiring (M5-039): the hull is alive (its
 *   HealthComponent health > 0) and its actor is not pending kill.
 * - PermissionGate (拒绝原因 DriveDenied): "may this occupant drive and fire
 *   from this seat?". Production wiring (M5-041): the Driver seat role grants
 *   drive+fire rights; faction/operator filtering follows contract section 8
 *   (friendly fire uses the operator faction, vehicle faction without one).
 * Gates receive the request ids as a value snapshot (FVehicleSeatPolicyContext)
 * and close over whatever World facts they need; the model stays World-free.
 *
 * Fixed refusal-check order (deterministic reasons, cheapest first - the
 * model's own state outranks the injected opinions):
 *   1 UnusableIds -> 2 UnknownSeat -> 3 AlreadyInThisSeat /
 *   OccupantAlreadySeated -> 4 SeatOccupied -> 5 DistanceTooFar ->
 *   6 VehicleInvalid -> 7 DriveDenied -> commit.
 */

/**
 * Why a seat request was refused. Append only - never renumber, so log lines
 * and tests keep their meaning. 0 always means "accepted".
 */
enum class EVehicleSeatRejectReason : uint8
{
	/** The request was accepted. */
	None = 0,
	/** NAME_None vehicle/seat id or an invalid (zero) occupant GUID. */
	UnusableIds = 1,
	/** The (vehicle, seat) pair was never registered in this model. */
	UnknownSeat = 2,
	/** Another occupant currently holds the seat. */
	SeatOccupied = 3,
	/** The occupant already holds a different seat (one seat per occupant). */
	OccupantAlreadySeated = 4,
	/** The exact re-entry of the seat the occupant already holds; state unchanged. */
	AlreadyInThisSeat = 5,
	/** The injected distance gate refused the entry. */
	DistanceTooFar = 6,
	/** The injected vehicle-validity gate refused the entry. */
	VehicleInvalid = 7,
	/** The injected drive/fire permission gate refused the entry. */
	DriveDenied = 8,
	/** RegisterSeat: the (vehicle, seat) pair is already registered. */
	DuplicateSeat = 9
};

/** Stable human-readable text for one reject reason (diagnostics and logs). */
inline const TCHAR* GetVehicleSeatRejectReasonText(EVehicleSeatRejectReason Reason)
{
	switch (Reason)
	{
	case EVehicleSeatRejectReason::None:
		return TEXT("accepted");
	case EVehicleSeatRejectReason::UnusableIds:
		return TEXT("unusable ids: VehicleId/SeatId must be non-empty and OccupantId must be a valid GUID");
	case EVehicleSeatRejectReason::UnknownSeat:
		return TEXT("unknown seat: the (vehicle, seat) pair is not registered in the model");
	case EVehicleSeatRejectReason::SeatOccupied:
		return TEXT("seat occupied: another occupant holds the seat");
	case EVehicleSeatRejectReason::OccupantAlreadySeated:
		return TEXT("occupant already seated: an occupant holds at most one seat");
	case EVehicleSeatRejectReason::AlreadyInThisSeat:
		return TEXT("already in this seat: the identical re-entry changes nothing");
	case EVehicleSeatRejectReason::DistanceTooFar:
		return TEXT("distance gate refused the entry");
	case EVehicleSeatRejectReason::VehicleInvalid:
		return TEXT("vehicle validity gate refused the entry");
	case EVehicleSeatRejectReason::DriveDenied:
		return TEXT("permission gate refused driving/firing from the seat");
	case EVehicleSeatRejectReason::DuplicateSeat:
		return TEXT("duplicate seat: the (vehicle, seat) pair is already registered");
	default:
		return TEXT("unknown seat reject reason");
	}
}

/**
 * One seat enter request as a value snapshot (pointer-free, trivially
 * copyable - safe to log, queue and replay). The World wiring builds it from
 * live actors and passes it in; the model never sees an actor.
 */
struct FVehicleSeatEnterRequest
{
	/** The vehicles-table id of the target vehicle (NAME_None is unusable). */
	FName VehicleId;

	/** The seats-table id of the target seat (NAME_None is unusable). */
	FName SeatId;

	/** The World-minted occupant instance identity (a zero GUID is unusable). */
	FGuid OccupantId;
};

/** The context one gate receives: the request ids, nothing else (pointer ban). */
struct FVehicleSeatPolicyContext
{
	/** The enter request under evaluation. */
	FVehicleSeatEnterRequest Request;
};

/** What a LeaveSeat that actually ended an occupation reports. */
struct FVehicleSeatLeaveResult
{
	/** Always true in a reported result (the struct is only written on success). */
	bool bLeft = false;

	/** The vehicle the occupant left. */
	FName VehicleId;

	/** The seat the occupant left. */
	FName SeatId;
};

/** The composite identity of one registered seat: SeatId is unique per vehicle. */
struct FVehicleSeatKey
{
	FName VehicleId;
	FName SeatId;

	bool operator==(const FVehicleSeatKey& Other) const
	{
		return VehicleId == Other.VehicleId
			&& SeatId == Other.SeatId;
	}
};

inline uint32 GetTypeHash(const FVehicleSeatKey& Key)
{
	// Unqualified calls on purpose: FName's hash is a friend inside NameTypes.h
	// and is only visible through argument-dependent lookup (the ::-qualified
	// form used by HitLedger works there because its members are integers).
	uint32 Hash = GetTypeHash(Key.VehicleId);
	Hash = HashCombine(Hash, GetTypeHash(Key.SeatId));
	return Hash;
}

/**
 * The seat occupancy model. State:
 * - SeatOccupants: every registered seat with its occupant (invalid FGuid =
 *   free seat). Registrations survive ClearVehicle; Reset drops them.
 * - OccupantSeats: the reverse index enforcing the one-seat-per-occupant
 *   invariant. The two maps are updated together (one commit in TryEnterSeat,
 *   one removal per release path) - neither map is an authority alone.
 */
class FSeatOccupancyModel
{
public:
	/**
	 * One injectable gate: true allows, false refuses with the gate's own
	 * reject reason. Unset (= default) means allow - the bare model is
	 * permissive and every production restriction arrives through a gate
	 * (see the header comment for the wiring points). Gates are predicates:
	 * they must not re-enter the model and must stay cheap/pure.
	 */
	using FSeatEnterGate = TFunction<bool(const FVehicleSeatPolicyContext&)>;

	/** Registers one seat of one vehicle; both ids must be non-NAME_None. */
	bool RegisterSeat(FName VehicleId, FName SeatId, EVehicleSeatRejectReason& OutReason);

	/**
	 * Atomically occupies the seat: all checks run first (identity, seat
	 * existence, occupant-side and seat-side occupancy, then the three gates
	 * in Distance -> Validity -> Permission order), and both maps commit in
	 * one tail mutation. A double-send of the same request succeeds exactly
	 * once (the re-send is refused with AlreadyInThisSeat). On refusal the
	 * state is unchanged, OutReason names the cause and OutDetail (when
	 * provided) carries a human-readable line naming the ids; OutDetail is
	 * emptied on acceptance. OutReason is always written.
	 */
	bool TryEnterSeat(const FVehicleSeatEnterRequest& Request, EVehicleSeatRejectReason& OutReason, FString* OutDetail = nullptr);

	/**
	 * Releases whatever seat the occupant holds. Doubles as the occupant-side
	 * cleanup path (death/destroy/destruct): idempotent, so the wiring can
	 * call it unconditionally. Returns true only when an occupation actually
	 * ended (OutResult filled with the vacated vehicle/seat); a false return
	 * is the safe "was not seated" no-op and leaves OutResult untouched.
	 */
	bool LeaveSeat(FGuid OccupantId, FVehicleSeatLeaveResult* OutResult = nullptr);

	/**
	 * Vehicle destroy/unload cleanup: frees every occupied seat of the
	 * vehicle while keeping the seat registrations (the same vehicle id can
	 * be occupied again by a new occupant, e.g. after a respawn). Returns
	 * the number of seats freed (0 for an unknown or already-empty vehicle -
	 * the call is idempotent).
	 */
	int32 ClearVehicle(FName VehicleId);

	/**
	 * World rebuild/unload cleanup: drops every seat registration and every
	 * occupancy - the full empty baseline (HitLedger::Reset precedent; the
	 * injected gates are configuration and survive, like BindRegistry does).
	 */
	void Reset();

	// -- Queries (diagnostics and wiring read these) ---------------------------

	/** True when the (vehicle, seat) pair is registered (occupied or free). */
	bool IsSeatRegistered(FName VehicleId, FName SeatId) const;

	/** The seat's occupant; false when the seat is unknown or free. */
	bool FindSeatOccupant(FName VehicleId, FName SeatId, FGuid& OutOccupantId) const;

	/** The seat an occupant holds; false when the occupant is seatless. */
	bool FindOccupantSeat(FGuid OccupantId, FVehicleSeatKey& OutSeatKey) const;

	/** Registered seats (free + occupied); survives ClearVehicle, dropped by Reset. */
	int32 GetNumRegisteredSeats() const { return SeatOccupants.Num(); }

	/** Seats currently held by an occupant. */
	int32 GetNumOccupiedSeats() const { return OccupantSeats.Num(); }

	// -- Gate injection (unset = allow; see header comment for wiring) ---------

	void SetDistanceGate(FSeatEnterGate InGate) { DistanceGate = MoveTemp(InGate); }
	void SetVehicleValidityGate(FSeatEnterGate InGate) { VehicleValidityGate = MoveTemp(InGate); }
	void SetPermissionGate(FSeatEnterGate InGate) { PermissionGate = MoveTemp(InGate); }

private:
	/** Every registered seat -> its occupant (invalid FGuid = free). */
	TMap<FVehicleSeatKey, FGuid> SeatOccupants;

	/** Every seated occupant -> the seat key (the one-seat-per-occupant index). */
	TMap<FGuid, FVehicleSeatKey> OccupantSeats;

	/** The three injectable gates; unset means allow. */
	FSeatEnterGate DistanceGate;
	FSeatEnterGate VehicleValidityGate;
	FSeatEnterGate PermissionGate;
};

// Compile-time pins: the model's value snapshots stay pointer-free and
// trivially copyable (contract section 0.5 pointer ban; ids only - the World
// owns the object lifetimes behind them).
static_assert(std::is_trivially_copyable_v<FVehicleSeatEnterRequest>, "FVehicleSeatEnterRequest must stay a trivially copyable value snapshot");
static_assert(std::is_trivially_copyable_v<FVehicleSeatPolicyContext>, "FVehicleSeatPolicyContext must stay a trivially copyable value snapshot");
static_assert(std::is_trivially_copyable_v<FVehicleSeatLeaveResult>, "FVehicleSeatLeaveResult must stay a trivially copyable value snapshot");
static_assert(std::is_trivially_copyable_v<FVehicleSeatKey>, "FVehicleSeatKey must stay a trivially copyable value snapshot");
static_assert(!std::is_pointer_v<decltype(FVehicleSeatEnterRequest::OccupantId)>, "FVehicleSeatEnterRequest must stay pointer-free");
static_assert(!std::is_pointer_v<decltype(FVehicleSeatKey::VehicleId)>, "FVehicleSeatKey must stay pointer-free");
static_assert(std::is_same_v<std::underlying_type_t<EVehicleSeatRejectReason>, uint8>, "seat reject reasons stay an append-only uint8 enum");
