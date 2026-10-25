#include "SeatModel.h"

/**
 * M5-040 implementation notes (rules live in the header comment):
 * - Single-threaded game-thread model; atomicity = check everything first,
 *   commit both maps in one tail mutation, refuse without touching state.
 * - The refusal-check order is fixed so reasons are deterministic:
 *   UnusableIds -> UnknownSeat -> AlreadyInThisSeat/OccupantAlreadySeated ->
 *   SeatOccupied -> DistanceTooFar -> VehicleInvalid -> DriveDenied.
 * - The TFunction gates run while the state is still the pre-enter state and
 *   must not re-enter the model (predicate contract, header comment).
 */

bool FSeatOccupancyModel::RegisterSeat(FName VehicleId, FName SeatId, EVehicleSeatRejectReason& OutReason)
{
	OutReason = EVehicleSeatRejectReason::None;

	// Identity: an absent vehicle or seat id can never be requested or
	// resolved by the World wiring ("None" only looks legal when printed).
	if (VehicleId == NAME_None || SeatId == NAME_None)
	{
		OutReason = EVehicleSeatRejectReason::UnusableIds;
		return false;
	}

	const FVehicleSeatKey Key{VehicleId, SeatId};

	// One registration per (vehicle, seat) pair: a duplicate would silently
	// reset the seat's occupancy view, so it is refused instead. Note the key
	// is the pair - the same SeatId under two vehicles stays distinct.
	if (SeatOccupants.Contains(Key))
	{
		OutReason = EVehicleSeatRejectReason::DuplicateSeat;
		return false;
	}

	SeatOccupants.Add(Key, FGuid()); // registered and free
	return true;
}

bool FSeatOccupancyModel::TryEnterSeat(const FVehicleSeatEnterRequest& Request, EVehicleSeatRejectReason& OutReason, FString* OutDetail)
{
	OutReason = EVehicleSeatRejectReason::None;
	if (OutDetail)
	{
		OutDetail->Reset();
	}

	// The one refusal path: nothing before it has touched the model, so every
	// refused request leaves the state byte-identical (all-or-nothing).
	const auto Refuse = [&OutReason, &OutDetail](EVehicleSeatRejectReason Reason, FString&& Detail)
	{
		OutReason = Reason;
		if (OutDetail)
		{
			*OutDetail = MoveTemp(Detail);
		}
		return false;
	};

	// 1. Identity: unusable ids never reach the maps at all (the HitLedger
	// "half-empty key" precedent - a request that cannot be named is refused
	// before anything is looked up).
	if (Request.VehicleId == NAME_None || Request.SeatId == NAME_None || !Request.OccupantId.IsValid())
	{
		return Refuse(EVehicleSeatRejectReason::UnusableIds,
			FString::Printf(TEXT("enter refused: VehicleId/SeatId must be non-empty ids and OccupantId must be a valid GUID (got vehicle '%s', seat '%s', occupant %s)"),
				*Request.VehicleId.ToString(), *Request.SeatId.ToString(), *Request.OccupantId.ToString()));
	}

	const FVehicleSeatKey Key{Request.VehicleId, Request.SeatId};

	// 2. Structure: the seat must be registered (an enter on a typo'd or
	// never-registered seat is refused instead of creating an implicit seat).
	const FGuid* SeatOccupant = SeatOccupants.Find(Key);
	if (!SeatOccupant)
	{
		return Refuse(EVehicleSeatRejectReason::UnknownSeat,
			FString::Printf(TEXT("enter refused: seat '%s' of vehicle '%s' is not registered in the model"),
				*Request.SeatId.ToString(), *Request.VehicleId.ToString()));
	}

	// 3. Occupant side: an occupant holds at most one seat (contract section
	// 8). The exact re-entry of the held seat is called out separately from
	// "you already sit somewhere else" - the atomic double-send lands here.
	if (const FVehicleSeatKey* HeldSeat = OccupantSeats.Find(Request.OccupantId))
	{
		if (*HeldSeat == Key)
		{
			return Refuse(EVehicleSeatRejectReason::AlreadyInThisSeat,
				FString::Printf(TEXT("enter refused: the occupant already sits in seat '%s' of vehicle '%s' (the identical re-entry changes nothing)"),
					*Request.SeatId.ToString(), *Request.VehicleId.ToString()));
		}
		return Refuse(EVehicleSeatRejectReason::OccupantAlreadySeated,
			FString::Printf(TEXT("enter refused: the occupant already sits in seat '%s' of vehicle '%s' (an occupant holds at most one seat)"),
				*HeldSeat->SeatId.ToString(), *HeldSeat->VehicleId.ToString()));
	}

	// 4. Seat side: one seat, one occupant. A refused second occupant never
	// evicts or observes the holder - the holder's id stays untouched.
	if (SeatOccupant->IsValid())
	{
		return Refuse(EVehicleSeatRejectReason::SeatOccupied,
			FString::Printf(TEXT("enter refused: seat '%s' of vehicle '%s' is already held by another occupant"),
				*Request.SeatId.ToString(), *Request.VehicleId.ToString()));
	}

	// 5..7. The injected gates, in the fixed Distance -> Validity -> Permission
	// order. All unset by default = allow; each refusal names its gate. The
	// gates run before the commit (predicate contract - no re-entry).
	FVehicleSeatPolicyContext Context;
	Context.Request = Request;

	if (DistanceGate && !DistanceGate(Context))
	{
		return Refuse(EVehicleSeatRejectReason::DistanceTooFar,
			FString::Printf(TEXT("enter refused: the distance gate refused seat '%s' of vehicle '%s'"),
				*Request.SeatId.ToString(), *Request.VehicleId.ToString()));
	}
	if (VehicleValidityGate && !VehicleValidityGate(Context))
	{
		return Refuse(EVehicleSeatRejectReason::VehicleInvalid,
			FString::Printf(TEXT("enter refused: the vehicle-validity gate refused vehicle '%s'"),
				*Request.VehicleId.ToString()));
	}
	if (PermissionGate && !PermissionGate(Context))
	{
		return Refuse(EVehicleSeatRejectReason::DriveDenied,
			FString::Printf(TEXT("enter refused: the permission gate refused driving/firing from seat '%s' of vehicle '%s'"),
				*Request.SeatId.ToString(), *Request.VehicleId.ToString()));
	}

	// 8. Commit: both maps flip in one tail mutation - no observable partial
	// state exists (single game thread). Add overwrites the free sentinel.
	SeatOccupants.Add(Key, Request.OccupantId);
	OccupantSeats.Add(Request.OccupantId, Key);

	OutReason = EVehicleSeatRejectReason::None;
	if (OutDetail)
	{
		OutDetail->Reset();
	}
	return true;
}

bool FSeatOccupancyModel::LeaveSeat(FGuid OccupantId, FVehicleSeatLeaveResult* OutResult)
{
	// Idempotent by design: a second leave (or a leave for a never-seated or
	// already-destroyed occupant) finds nothing and returns false without
	// touching any map or OutResult - safe to call unconditionally from the
	// wiring's cleanup paths (death/destroy/destruct).
	const FVehicleSeatKey* HeldSeat = OccupantSeats.Find(OccupantId);
	if (!HeldSeat)
	{
		return false;
	}

	const FVehicleSeatKey Key = *HeldSeat;
	OccupantSeats.Remove(OccupantId);
	SeatOccupants.Add(Key, FGuid()); // back to the free sentinel, registration stays

	if (OutResult)
	{
		OutResult->bLeft = true;
		OutResult->VehicleId = Key.VehicleId;
		OutResult->SeatId = Key.SeatId;
	}
	return true;
}

int32 FSeatOccupancyModel::ClearVehicle(FName VehicleId)
{
	// Collect first, mutate second: removing from a TMap while iterating it is
	// not safe. Only occupied seats count as cleared; free registrations stay
	// free registrations (and the vehicle id stays usable for new occupants).
	TArray<TPair<FVehicleSeatKey, FGuid>> HeldSeats;
	for (const TPair<FVehicleSeatKey, FGuid>& Pair : SeatOccupants)
	{
		if (Pair.Key.VehicleId == VehicleId && Pair.Value.IsValid())
		{
			HeldSeats.Add(Pair);
		}
	}

	for (const TPair<FVehicleSeatKey, FGuid>& Held : HeldSeats)
	{
		OccupantSeats.Remove(Held.Value);
		SeatOccupants.Add(Held.Key, FGuid()); // free again, registration stays
	}
	return HeldSeats.Num();
}

void FSeatOccupancyModel::Reset()
{
	// World rebuild/unload: the full empty baseline. Registrations go too -
	// the world wiring re-registers every seat it spawns (M5-038). The gates
	// are configuration and survive (HitLedger keeps its bound registry).
	SeatOccupants.Empty();
	OccupantSeats.Empty();
}

bool FSeatOccupancyModel::IsSeatRegistered(FName VehicleId, FName SeatId) const
{
	return SeatOccupants.Contains(FVehicleSeatKey{VehicleId, SeatId});
}

bool FSeatOccupancyModel::FindSeatOccupant(FName VehicleId, FName SeatId, FGuid& OutOccupantId) const
{
	const FGuid* SeatOccupant = SeatOccupants.Find(FVehicleSeatKey{VehicleId, SeatId});
	if (SeatOccupant && SeatOccupant->IsValid())
	{
		OutOccupantId = *SeatOccupant;
		return true;
	}
	return false;
}

bool FSeatOccupancyModel::FindOccupantSeat(FGuid OccupantId, FVehicleSeatKey& OutSeatKey) const
{
	const FVehicleSeatKey* HeldSeat = OccupantSeats.Find(OccupantId);
	if (HeldSeat)
	{
		OutSeatKey = *HeldSeat;
		return true;
	}
	return false;
}
