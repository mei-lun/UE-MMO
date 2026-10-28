#include "VehicleInteractionComponent.h"

#include "../PrototypeCharacter.h"
#include "../Combat/HealthComponent.h"
#include "../PrototypeHUD.h"
#include "../UI/RoomResultWidget.h"
#include "Components/CapsuleComponent.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"
#include "VehicleMovementComponent.h"
#include "VehicleWeaponMountComponent.h"

/**
 * M5-041: implementation of the enter/exit interaction. Rules live in the
 * header comment; the implementation notes:
 * - The component layer's refusal order is fixed (no model -> dead owner ->
 *   captured UI -> seated re-entry -> nearest candidate -> model), so reasons
 *   stay deterministic exactly like the model's own check order.
 * - The model stays the single occupancy authority: the component never
 *   writes an occupation by hand, every enter is TryEnterSeat's atomic
 *   commit and every release is LeaveSeat / ClearVehicle.
 * - The forced-restore paths (destroy/death/reset/EndPlay) share one
 *   idempotent helper, so contract section 8's cleanup faces cannot drift.
 */

DEFINE_LOG_CATEGORY_STATIC(LogUEMMOVehicleInteraction, Log, All);

namespace
{
	// The enter range of the distance gate: the pawn must reach the seat
	// socket (the FVehicleSeatDefinition SeatLocationCm transformed by the
	// hull) within this straight-line distance. A schema field would replace
	// the constant (hull dimensions are not in the M5-037 schema either).
	constexpr float M5_041_MaxEnterDistanceCm = 300.0f;
}

UVehicleInteractionComponent::UVehicleInteractionComponent()
{
	// No per-frame duty: every face is event/entry driven (the M5-039 hull
	// precedent - the model layer owns no ticking either).
	PrimaryComponentTick.bCanEverTick = false;
}

void UVehicleInteractionComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// The cleanup face (contract section 8: destroy/LeaveRoom/Retry/EndPlay):
	// the seat frees while the model is addressable and the destroy
	// subscription + vacate seam detach while the hull is addressable. The
	// order against the hull's own EndPlay is irrelevant - both sides are
	// idempotent (the hull's FinalizeDestruction accepts the later terminal).
	RestoreDriverAfterForcedCleanup(TEXT("component EndPlay"));
	Super::EndPlay(EndPlayReason);
}

void UVehicleInteractionComponent::BindSeatModel(FSeatOccupancyModel& InModel)
{
	BoundSeatModel = &InModel;
}

void UVehicleInteractionComponent::SetUICapturePredicate(TFunction<bool()> InPredicate)
{
	UICapturePredicate = MoveTemp(InPredicate);
}

bool UVehicleInteractionComponent::RegisterVehicle(ACombatVehicle& Vehicle, const FVehicleSeatDefinition& Seat, FString* OutError)
{
	const auto Refuse = [OutError](const FString& Why)
	{
		if (OutError != nullptr)
		{
			*OutError = Why;
		}
		return false;
	};

	// No model, no registration: an entry without the model could never be
	// entered or cleaned up (the component layer holds no occupancy itself).
	if (BoundSeatModel == nullptr)
	{
		return Refuse(TEXT("no seat model bound: BindSeatModel must run before any vehicle registration"));
	}
	if (!Vehicle.HasVehicleDefinition())
	{
		return Refuse(FString::Printf(TEXT("the hull has no vehicle definition loaded (id would be '%s')"), *Seat.VehicleId.ToString()));
	}
	if (Seat.VehicleId != Vehicle.GetVehicleDefinition().VehicleId)
	{
		return Refuse(FString::Printf(TEXT("the seat row names vehicle '%s' but the hull is '%s'"),
			*Seat.VehicleId.ToString(), *Vehicle.GetVehicleDefinition().VehicleId.ToString()));
	}
	FString SeatErrors;
	if (!ValidateVehicleSeatDefinition(Seat, SeatErrors))
	{
		return Refuse(FString::Printf(TEXT("the seat row is invalid: %s"), *SeatErrors));
	}

	EnsureOwnerWiring();
	InstallGatesIfNeeded();

	// The model registration is idempotent through the registered-pair check
	// (a re-registration would refuse at the model with DuplicateSeat; the
	// wiring treats the registered pair as the upsert below).
	if (!BoundSeatModel->IsSeatRegistered(Seat.VehicleId, Seat.SeatId))
	{
		EVehicleSeatRejectReason Reason = EVehicleSeatRejectReason::None;
		if (!BoundSeatModel->RegisterSeat(Seat.VehicleId, Seat.SeatId, Reason))
		{
			return Refuse(FString::Printf(TEXT("the seat model refused the registration (%s)"),
				GetVehicleSeatRejectReasonText(Reason)));
		}
	}

	// Upsert the entry: a re-registration (the 039 spawn/destroy cycle reuse
	// of the same business id) refreshes the seat values and the hull's own
	// validity gate (the new hull instance vouches for itself).
	if (FVehicleSeatEntry* Existing = FindEntryMutable(Seat.VehicleId, Seat.SeatId))
	{
		Existing->Vehicle = &Vehicle;
		Existing->Seat = Seat;
		Existing->ValidityGate = Vehicle.MakeVehicleValidityGate();
	}
	else
	{
		FVehicleSeatEntry Entry;
		Entry.Vehicle = &Vehicle;
		Entry.Seat = Seat;
		Entry.ValidityGate = Vehicle.MakeVehicleValidityGate();
		Entries.Add(MoveTemp(Entry));
	}

	UE_LOG(LogUEMMOVehicleInteraction, Log,
		TEXT("UEMMO M5-041: vehicle '%s' seat '%s' registered into the interaction component."),
		*Seat.VehicleId.ToString(), *Seat.SeatId.ToString());
	return true;
}

void UVehicleInteractionComponent::InstallGatesIfNeeded()
{
	if (bGatesInstalled || BoundSeatModel == nullptr)
	{
		return;
	}
	bGatesInstalled = true;

	// Weak capture: the injected model may outlive this component, and a dead
	// component's gates answer false (the refusal paths, never a dangle).
	const TWeakObjectPtr<UVehicleInteractionComponent> WeakThis(this);

	// Distance -> the nearest-seat range (the M5-038/041 production wiring
	// point SeatModel.h names): the pawn must reach the seat socket.
	BoundSeatModel->SetDistanceGate([WeakThis](const FVehicleSeatPolicyContext& Context)
	{
		const UVehicleInteractionComponent* Ctx = WeakThis.Get();
		if (Ctx == nullptr || Ctx->BoundSeatModel == nullptr)
		{
			return false;
		}
		const FVehicleSeatEntry* Entry = Ctx->FindEntry(Context.Request.VehicleId, Context.Request.SeatId);
		const ACombatVehicle* Hull = Entry != nullptr ? Entry->Vehicle.Get() : nullptr;
		const APawn* Pawn = Cast<APawn>(Ctx->GetOwner());
		if (Entry == nullptr || Hull == nullptr || Pawn == nullptr)
		{
			return false;
		}
		const FVector Socket = Hull->GetActorTransform().TransformPosition(Entry->Seat.SeatLocationCm);
		return FVector::Dist(Pawn->GetActorLocation(), Socket) <= M5_041_MaxEnterDistanceCm;
	});

	// Validity -> the hull's own M5-039 production face (MakeVehicleValidity-
	// Gate): a dead or destroyed hull refuses with VehicleInvalid. Fail-closed
	// when the gate is somehow unset.
	BoundSeatModel->SetVehicleValidityGate([WeakThis](const FVehicleSeatPolicyContext& Context)
	{
		const UVehicleInteractionComponent* Ctx = WeakThis.Get();
		if (Ctx == nullptr || Ctx->BoundSeatModel == nullptr)
		{
			return false;
		}
		const FVehicleSeatEntry* Entry = Ctx->FindEntry(Context.Request.VehicleId, Context.Request.SeatId);
		if (Entry == nullptr || !Entry->ValidityGate)
		{
			return false;
		}
		return Entry->ValidityGate(Context);
	});

	// Permission -> the Driver seat role grants drive+fire rights (the
	// SeatModel.h wiring point). The implemented vocabulary has exactly this
	// role; a future role refuses here until a card grants it.
	BoundSeatModel->SetPermissionGate([WeakThis](const FVehicleSeatPolicyContext& Context)
	{
		const UVehicleInteractionComponent* Ctx = WeakThis.Get();
		if (Ctx == nullptr || Ctx->BoundSeatModel == nullptr)
		{
			return false;
		}
		const FVehicleSeatEntry* Entry = Ctx->FindEntry(Context.Request.VehicleId, Context.Request.SeatId);
		return Entry != nullptr && Entry->Seat.SeatRole == EVehicleSeatRole::Driver;
	});
}

FVehicleEnterAttempt UVehicleInteractionComponent::TryEnterNearestVehicle()
{
	FVehicleEnterAttempt Attempt;
	const auto Refuse = [&Attempt](EVehicleInteractionReject Reject, EVehicleSeatRejectReason ModelReject, FString&& Detail)
	{
		Attempt.bEntered = false;
		Attempt.Reject = Reject;
		Attempt.ModelReject = ModelReject;
		Attempt.Detail = MoveTemp(Detail);
		UE_LOG(LogUEMMOVehicleInteraction, Verbose,
			TEXT("UEMMO M5-041: enter refused (%s): %s"), GetVehicleInteractionRejectText(Reject), *Attempt.Detail);
		return Attempt;
	};

	APrototypeCharacter* Pawn = Cast<APrototypeCharacter>(GetOwner());
	FSeatOccupancyModel* Model = BoundSeatModel;
	if (Model == nullptr || Pawn == nullptr)
	{
		return Refuse(EVehicleInteractionReject::NoSeatModel, EVehicleSeatRejectReason::None,
			TEXT("no seat model bound: the wiring must inject the FSeatOccupancyModel before any enter"));
	}
	EnsureOwnerWiring();
	EnsureUICapturePredicate();

	// 1. Death outranks everything (contract section 4): a dead driver never
	// enters, no matter what the model would say.
	if (Pawn->GetHealth() == nullptr || !Pawn->GetHealth()->IsAlive())
	{
		return Refuse(EVehicleInteractionReject::OwnerDead, EVehicleSeatRejectReason::None,
			TEXT("the owning pawn is dead: a dead driver cannot enter a seat"));
	}

	// 2. The UI-capture face (the menu UIOnly acceptance): while a screen
	// holds the input the enter request is refused before any model state.
	if (UICapturePredicate && UICapturePredicate())
	{
		return Refuse(EVehicleInteractionReject::UICapturedInput, EVehicleSeatRejectReason::None,
			TEXT("a menu/result screen captured the input: the enter request is refused while the UI holds the focus"));
	}

	// 3. A seated re-entry rides the model's own AlreadyInThisSeat path: the
	// occupant-side check outranks every gate, so the refusal is deterministic
	// (the duplicated enter changes nothing - the idempotency pin).
	if (bSeated)
	{
		FVehicleSeatEnterRequest Repeat;
		Repeat.VehicleId = SeatedVehicleId;
		Repeat.SeatId = SeatedSeatId;
		Repeat.OccupantId = GetOrCreateOccupantId();
		EVehicleSeatRejectReason ModelReason = EVehicleSeatRejectReason::None;
		FString ModelDetail;
		if (!Model->TryEnterSeat(Repeat, ModelReason, &ModelDetail))
		{
			return Refuse(EVehicleInteractionReject::ModelRefused, ModelReason, MoveTemp(ModelDetail));
		}
		// Unreachable by the model's own atomicity (an occupant re-sending its
		// held seat is refused above): kept as the named impossibility.
		return Refuse(EVehicleInteractionReject::ModelRefused, EVehicleSeatRejectReason::None,
			TEXT("the seated re-entry was accepted by the model - a state the one-seat-per-occupant invariant forbids"));
	}

	// 4. The nearest registered candidate: destroyed/collected hulls drop out;
	// dead-but-alive hulls STAY candidates so the model's validity gate names
	// the refusal (VehicleInvalid) instead of the search inventing one.
	const FVector PawnLocation = Pawn->GetActorLocation();
	const FVehicleSeatEntry* Best = nullptr;
	float BestDistanceSquared = TNumericLimits<float>::Max();
	for (const FVehicleSeatEntry& Entry : Entries)
	{
		const ACombatVehicle* Hull = Entry.Vehicle.Get();
		if (Hull == nullptr || Hull->IsPendingKillPending())
		{
			continue;
		}
		const FVector Socket = Hull->GetActorTransform().TransformPosition(Entry.Seat.SeatLocationCm);
		const float DistanceSquared = FVector::DistSquared(PawnLocation, Socket);
		if (DistanceSquared < BestDistanceSquared)
		{
			BestDistanceSquared = DistanceSquared;
			Best = &Entry;
		}
	}
	if (Best == nullptr)
	{
		return Refuse(EVehicleInteractionReject::NoVehicleInRange, EVehicleSeatRejectReason::None,
			TEXT("no registered vehicle/seat candidate resolved: nothing legal is near enough to enter"));
	}

	// 5. The model's atomic enter (gates Distance -> Validity -> Permission
	// included). A refusal passes through with the model's own reason.
	FVehicleSeatEnterRequest Request;
	Request.VehicleId = Best->Seat.VehicleId;
	Request.SeatId = Best->Seat.SeatId;
	Request.OccupantId = GetOrCreateOccupantId();
	EVehicleSeatRejectReason ModelReason = EVehicleSeatRejectReason::None;
	FString ModelDetail;
	if (!Model->TryEnterSeat(Request, ModelReason, &ModelDetail))
	{
		return Refuse(EVehicleInteractionReject::ModelRefused, ModelReason, MoveTemp(ModelDetail));
	}

	// 6. Accepted: the bookkeeping locks onto the hull (the model already
	// refused the duplicated enter, so this runs exactly once per occupation).
	// M5-H03 audit: the rider KEEPS walking control - no pause flag exists.
	ACombatVehicle* Hull = Best->Vehicle.Get();
	bSeated = true;
	SeatedVehicleId = Request.VehicleId;
	SeatedSeatId = Request.SeatId;
	SeatedVehicle = Hull;
	Attempt.bEntered = true;
	Attempt.VehicleId = Request.VehicleId;
	Attempt.SeatId = Request.SeatId;

	// M5-044: the production rigging (drive + turret from the real source
	// tables, operator seam wired) is ensured on the first enter of the hull
	// and refreshed in place afterwards - the unarmed/driveless degradation
	// is a named refusal logged at the rigging, never a silent one.
	FString RiggingError;
	if (!Hull->EnsureProductionRigging(BoundSeatModel, Request.SeatId, OperatorFaction, &RiggingError))
	{
		UE_LOG(LogUEMMOVehicleInteraction, Warning,
			TEXT("UEMMO M5-044: vehicle '%s' production rigging refused: %s"), *Request.VehicleId.ToString(), *RiggingError);
	}
	// M5-H03 audit: the HULL becomes a child of the rider - the plain engine
	// attachment, no control swap and no visibility toggling. Snapped to the
	// pawn origin, it rides every walk step through the transform
	// propagation; the drive component stays idle (movement = the rider's
	// own walking) and the exit detaches it right beside the rider.
	Hull->AttachToComponent(Pawn->GetRootComponent(), FAttachmentTransformRules::SnapToTargetNotIncludingScale);
	// The rider and the worn hull ignore each other's sweeps: the capsule
	// starts inside the hull box, and without the mutual ignore the walking
	// depenetration would shove the rider out (the M5-044 ride-shove lesson).
	if (UCapsuleComponent* RiderCapsule = Pawn->GetCapsuleComponent())
	{
		RiderCapsule->MoveIgnoreActors.Add(Hull);
	}
	if (UPrimitiveComponent* HullBody = Cast<UPrimitiveComponent>(Hull->GetRootComponent()))
	{
		HullBody->MoveIgnoreActors.Add(Pawn);
	}

	// The hull's one-shot vacate seam (M5-039's SetSeatVacateCallback wiring
	// point): the death sequence forwards to the model's ClearVehicle, whose
	// registrations survive so the vehicle can be occupied again. The slot is
	// an overwrite seam - exactly one callback exists per hull, never a stack.
	Hull->SetSeatVacateCallback([Model](FName DestroyedVehicleId)
	{
		if (Model != nullptr)
		{
			Model->ClearVehicle(DestroyedVehicleId);
		}
	});
	// The destroy subscription binds once per hull (a re-enter of the same
	// hull reuses it; switching hulls detaches the previous one first).
	if (BoundDestroySource.Get() != Hull)
	{
		if (ACombatVehicle* Previous = BoundDestroySource.Get())
		{
			Previous->OnVehicleDestroyed.RemoveAll(this);
		}
		Hull->OnVehicleDestroyed.AddUObject(this, &UVehicleInteractionComponent::HandleVehicleDestroyed);
		BoundDestroySource = Hull;
	}

	UE_LOG(LogUEMMOVehicleInteraction, Log,
		TEXT("UEMMO M5-041: the driver entered seat '%s' of vehicle '%s' (the hull rides the rider)."),
		*Request.SeatId.ToString(), *Request.VehicleId.ToString());
	return Attempt;
}

bool UVehicleInteractionComponent::TryExitVehicle(FString* OutRefusal)
{
	// Idempotent by design: the duplicated exit is the named no-op (no model
	// call, no restore, no second teleport).
	if (!bSeated)
	{
		if (OutRefusal != nullptr)
		{
			*OutRefusal = TEXT("not seated: the exit request is the idempotent no-op");
		}
		UE_LOG(LogUEMMOVehicleInteraction, Verbose,
			TEXT("UEMMO M5-041: exit refused: not seated (the idempotent no-op)"));
		return false;
	}
	// M5-H03 audit: the rider never stopped walking, so the exit simply
	// detaches the hull right beside them - no drop spot, nothing to be
	// blocked by.
	RestoreDriverAfterForcedCleanup(TEXT("active exit"));
	return true;
}

void UVehicleInteractionComponent::HandleVehicleInteractTriggered()
{
	TryInteract();
}

void UVehicleInteractionComponent::TryInteract()
{
	// The V toggle: enter when walking, exit when seated. A duplicated binding
	// (a re-setup lambda) lands on the seated re-entry refusal or the
	// not-seated no-op - never a second occupation or a second restore.
	if (bSeated)
	{
		FString Refusal;
		if (!TryExitVehicle(&Refusal))
		{
			UE_LOG(LogUEMMOVehicleInteraction, Verbose,
				TEXT("UEMMO M5-041: the V exit request was refused: %s"), *Refusal);
		}
		return;
	}
	const FVehicleEnterAttempt Attempt = TryEnterNearestVehicle();
	if (!Attempt.bEntered)
	{
		UE_LOG(LogUEMMOVehicleInteraction, Verbose,
			TEXT("UEMMO M5-041: the V enter request was refused (%s): %s"),
			GetVehicleInteractionRejectText(Attempt.Reject), *Attempt.Detail);
	}
}

void UVehicleInteractionComponent::NotifyOwnerReset()
{
	// The unified-reset face: the reset restores the pawn's spawn state, so a
	// seated driver leaves the seat here (the F2/service path shares this
	// entry through ApplyTrainingRoomReset). Idempotent - a no-op without a
	// seat or with everything already restored.
	RestoreDriverAfterForcedCleanup(TEXT("owner reset"));
}

void UVehicleInteractionComponent::RestoreDriverAfterForcedCleanup(const TCHAR* Why)
{
	// 1. The seat first (contract section 8 order): idempotent LeaveSeat - the
	// vacate seam or a previous cleanup may have freed it already.
	if (FSeatOccupancyModel* Model = BoundSeatModel)
	{
		Model->LeaveSeat(OccupantId);
	}
	// 2. M5-044: the vehicle-side faces of the driver loss - the held drive
	// input drops (the drive authority re-checks the presence anyway), the
	// in-flight turret shot releases and the camera hands back to the
	// character anchor - all weak-safe (a dying hull still answers; a
	// collected one is skipped).
	if (ACombatVehicle* Hull = SeatedVehicle.Get())
	{
		if (UVehicleMovementComponent* Drive = Hull->FindComponentByClass<UVehicleMovementComponent>())
		{
			Drive->SetDriveInput(0.0f, 0.0f);
			Drive->SetDriverPresent(false);
		}
		if (UVehicleWeaponMountComponent* Mount = Hull->FindComponentByClass<UVehicleWeaponMountComponent>())
		{
			Mount->ReleaseMountShot();
			Mount->NotifyOperatorChanged();
		}
	}
	// 2b. M5-H03 audit: detach the RIDING HULL first (it was the pawn's child
	// while seated; the active exit and every forced cleanup leave it in
	// world space right beside the rider). The pawn never moved. The mutual
	// move-ignore drops with it (the rider can sweep the detached hull
	// normally again).
	if (ACombatVehicle* Hull = SeatedVehicle.Get())
	{
		Hull->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		if (APrototypeCharacter* Rider = Cast<APrototypeCharacter>(GetOwner()))
		{
			if (UCapsuleComponent* RiderCapsule = Rider->GetCapsuleComponent())
			{
				RiderCapsule->MoveIgnoreActors.Remove(Hull);
			}
			if (UPrimitiveComponent* HullBody = Cast<UPrimitiveComponent>(Hull->GetRootComponent()))
			{
				HullBody->MoveIgnoreActors.Remove(Rider);
			}
		}
	}
	// 3. The bookkeeping and the control face (both idempotent flags): the
	// character-side control face reopens. No possession change, no teleport -
	// the surviving driver keeps its PlayerController and input context (the
	// rider never lost walking control; the hull detaches beside them).
	bSeated = false;
	SeatedVehicleId = NAME_None;
	SeatedSeatId = NAME_None;
	SeatedVehicle = nullptr;
	// 4. The subscription and the vacate seam detach while the hull is still
	// addressable (weak + pending-kill-safe: after a GC the weak reference is
	// null and the delegate object is gone with the hull anyway).
	if (ACombatVehicle* Bound = BoundDestroySource.Get())
	{
		Bound->OnVehicleDestroyed.RemoveAll(this);
		Bound->SetSeatVacateCallback(TFunction<void(FName)>());
	}
	BoundDestroySource = nullptr;

	UE_LOG(LogUEMMOVehicleInteraction, Log,
		TEXT("UEMMO M5-041: the driver restored (%s): seat freed, character control reopened."), Why);
}

void UVehicleInteractionComponent::HandleOwnerDied()
{
	// Death inside the seat frees it (contract section 8: cleanup covers
	// death). A dead driver's control face is already stopped by the death
	// chain itself; the restore keeps the state coherent for the revive.
	if (bSeated)
	{
		RestoreDriverAfterForcedCleanup(TEXT("owner died in a seat"));
	}
}

void UVehicleInteractionComponent::HandleVehicleDestroyed(const FVehicleDestroyedEvent& Event)
{
	// The once-only subscription pin: the binding guards make a duplicate
	// AddUObject impossible; this counter would expose one if that ever
	// drifted (the 039 scene's event-counting precedent).
	++VehicleDestroyedNotifications;
	// M5-044: the destroyed hull's turret mount finalizes here - the mount
	// itself subscribes nothing (the one-shot destroy sequence's mount face is
	// the wiring's duty). A mount exists only on a rigged (entered) hull, and
	// an enter binds this subscription, so every mount sees its hull's
	// destroy exactly once.
	if (const ACombatVehicle* DestroyedHull = Event.Vehicle.Get())
	{
		// The event hands a read-only snapshot; the mount teardown mutates the
		// mount component only (the hull itself stays read-only here).
		ACombatVehicle* MutableHull = const_cast<ACombatVehicle*>(DestroyedHull);
		if (UVehicleWeaponMountComponent* Mount = MutableHull->FindComponentByClass<UVehicleWeaponMountComponent>())
		{
			Mount->Finalize();
		}
	}
	if (!bSeated || Event.VehicleId != SeatedVehicleId)
	{
		return;
	}
	// Contract section 8: the destroyed vehicle's driver is restored without
	// losing the controller or the input context. The model-side clear already
	// ran through the vacate seam (ClearVehicle before the broadcast), so the
	// LeaveSeat inside is the safe second no-op.
	RestoreDriverAfterForcedCleanup(TEXT("vehicle destroyed"));
}

void UVehicleInteractionComponent::EnsureOwnerWiring()
{
	if (bOwnerDeathBound)
	{
		return;
	}
	const APrototypeCharacter* Pawn = Cast<APrototypeCharacter>(GetOwner());
	if (Pawn == nullptr || Pawn->GetHealth() == nullptr)
	{
		return;
	}
	// The death binding rides the one health owner's at-most-once signal (the
	// pawn owns Health as a subobject, so the bound this cannot dangle).
	Pawn->GetHealth()->OnDied.AddUObject(this, &UVehicleInteractionComponent::HandleOwnerDied);
	bOwnerDeathBound = true;
}

void UVehicleInteractionComponent::EnsureUICapturePredicate()
{
	if (UICapturePredicate)
	{
		return;
	}
	// The default gate: the local HUD's screen validity IS the capture signal
	// (the M5-033 menu-open fire gate precedent) plus the M2-012 CapturedToUI
	// phase of the room result screen. Without a HUD (bare test worlds) the
	// predicate answers false - no capture, no refusal.
	UICapturePredicate = [WeakOwner = TWeakObjectPtr<const AActor>(GetOwner())]()
	{
		const AActor* OwnerActor = WeakOwner.Get();
		const APawn* Pawn = Cast<APawn>(OwnerActor);
		const APlayerController* Controller = Pawn != nullptr ? Cast<APlayerController>(Pawn->GetController()) : nullptr;
		const APrototypeHUD* Hud = Controller != nullptr ? Cast<APrototypeHUD>(Controller->GetHUD()) : nullptr;
		if (Hud == nullptr)
		{
			return false;
		}
		return Hud->HasInventoryScreen() || Hud->HasMenuScreen()
			|| Hud->HasRoomResultScreen()
			|| Hud->PeekRoomResultInputFocus().GetPhase() == ERoomResultInputPhase::CapturedToUI;
	};
}

const FVehicleSeatEntry* UVehicleInteractionComponent::FindEntry(FName VehicleId, FName SeatId) const
{
	for (const FVehicleSeatEntry& Entry : Entries)
	{
		if (Entry.Seat.VehicleId == VehicleId && Entry.Seat.SeatId == SeatId)
		{
			return &Entry;
		}
	}
	return nullptr;
}

FVehicleSeatEntry* UVehicleInteractionComponent::FindEntryMutable(FName VehicleId, FName SeatId)
{
	for (FVehicleSeatEntry& Entry : Entries)
	{
		if (Entry.Seat.VehicleId == VehicleId && Entry.Seat.SeatId == SeatId)
		{
			return &Entry;
		}
	}
	return nullptr;
}

UInputAction* UVehicleInteractionComponent::EnsureVehicleInteractAction()
{
	if (VehicleInteractAction != nullptr)
	{
		return VehicleInteractAction.Get();
	}
	// The V action lives on the component (owned, GC-reachable) so the shared
	// PrototypeCharacter.h needs no member; the M1-040 runtime-action pattern
	// (created in code, key mapped by the pawn's setup).
	VehicleInteractAction = NewObject<UInputAction>(this, TEXT("M5_041_VehicleInteract"));
	return VehicleInteractAction.Get();
}

UInputAction* UVehicleInteractionComponent::EnsureVehicleStagingAction()
{
	if (VehicleStagingAction != nullptr)
	{
		return VehicleStagingAction.Get();
	}
	// The F8 staging action (the C-segment no-console trial entry): the same
	// runtime-action pattern as the V key.
	VehicleStagingAction = NewObject<UInputAction>(this, TEXT("M5_051_VehicleStaging"));
	return VehicleStagingAction.Get();
}

void UVehicleInteractionComponent::HandleVehicleStagingTriggered()
{
	// The F8 entry: the local HUD stages one trial vehicle (UEMMODebugVehicle
	// 1). A HUD-less world (bare test worlds) logs and does nothing - never a
	// fake car.
	const APrototypeCharacter* OwnerPawn = Cast<APrototypeCharacter>(GetOwner());
	const APlayerController* Controller = OwnerPawn != nullptr ? Cast<APlayerController>(OwnerPawn->GetController()) : nullptr;
	APrototypeHUD* Hud = Controller != nullptr ? Cast<APrototypeHUD>(Controller->GetHUD()) : nullptr;
	if (Hud == nullptr)
	{
		UE_LOG(LogUEMMOVehicleInteraction, Verbose,
			TEXT("UEMMO M5-051: the F8 staging needs the local HUD (no HUD in this world)."));
		return;
	}
	Hud->UEMMODebugVehicle(1);
}

bool UVehicleInteractionComponent::RouteVehicleFirePressed(FString* OutRefusal)
{
	const auto Refuse = [OutRefusal](const FString& Why)
	{
		if (OutRefusal != nullptr)
		{
			*OutRefusal = Why;
		}
		return false;
	};

	if (!bSeated)
	{
		return Refuse(TEXT("not seated: the vehicle fire route is the no-op"));
	}
	ACombatVehicle* Hull = SeatedVehicle.Get();
	UVehicleWeaponMountComponent* Mount = Hull != nullptr ? Hull->FindComponentByClass<UVehicleWeaponMountComponent>() : nullptr;
	if (Mount == nullptr)
	{
		return Refuse(TEXT("the seated hull carries no turret mount: the fire route is refused (an unarmed hull never fakes a shot)"));
	}
	// The lazy reload completion: the turret has no tick of its own, so a
	// window whose duration elapsed closes right here (CompleteReload is the
	// named no-op refusal without an open window - zero side effects).
	if (UWeaponComponent* TurretWeapon = Mount->GetTurretWeapon())
	{
		TurretWeapon->CompleteReload();
	}
	// The aim rides the vehicle body (yaw 0 = hull facing, pitch 0 - inside
	// every configured clamp; the M5-037 schema has no yaw field).
	const FVehicleMountFireOutcome Outcome = Mount->TryFire(FVehicleMountAim());
	if (!Outcome.bFired)
	{
		if (OutRefusal != nullptr)
		{
			*OutRefusal = FString::Printf(TEXT("the turret refused (%d): %s"),
				static_cast<int32>(Outcome.Reject), *Outcome.RejectDetail);
		}
		return false;
	}
	return true;
}

void UVehicleInteractionComponent::RouteVehicleFireReleased()
{
	if (ACombatVehicle* Hull = SeatedVehicle.Get())
	{
		if (UVehicleWeaponMountComponent* Mount = Hull->FindComponentByClass<UVehicleWeaponMountComponent>())
		{
			Mount->ReleaseMountShot();
		}
	}
}

bool UVehicleInteractionComponent::RouteVehicleReloadPressed(FString* OutRefusal)
{
	const auto Refuse = [OutRefusal](const FString& Why)
	{
		if (OutRefusal != nullptr)
		{
			*OutRefusal = Why;
		}
		return false;
	};

	if (!bSeated)
	{
		return Refuse(TEXT("not seated: the vehicle reload route is the no-op"));
	}
	ACombatVehicle* Hull = SeatedVehicle.Get();
	UVehicleWeaponMountComponent* Mount = Hull != nullptr ? Hull->FindComponentByClass<UVehicleWeaponMountComponent>() : nullptr;
	UWeaponComponent* TurretWeapon = Mount != nullptr ? Mount->GetTurretWeapon() : nullptr;
	if (TurretWeapon == nullptr)
	{
		return Refuse(TEXT("the seated hull carries no turret weapon: the reload route is refused"));
	}
	const double Now = Hull->GetWorld() != nullptr ? Hull->GetWorld()->GetTimeSeconds() : 0.0;
	// The M5-033 T semantics on the vehicle: a pending window is closed first
	// (the lazy completion), then the press opens a fresh one when the mount
	// is idle. The duration is the input-layer constant (no schema field).
	if (TurretWeapon->GetRemainingReloadSeconds(Now) > 0.0)
	{
		return Refuse(TEXT("the turret reload window is already open"));
	}
	const FAmmoReloadOutcome Outcome = TurretWeapon->BeginReload(Now, 2.0);
	if (!Outcome.bSuccess)
	{
		if (OutRefusal != nullptr)
		{
			*OutRefusal = FString::Printf(TEXT("the turret reload refused (%d): %s"),
				static_cast<int32>(Outcome.Error), *Outcome.ErrorDetail);
		}
		return false;
	}
	return true;
}

FGuid& UVehicleInteractionComponent::GetOrCreateOccupantId()
{
	if (!OccupantId.IsValid())
	{
		// The World wiring mints the occupant GUID (the M5-040 header's wiring
		// rule); minted once per component so enter/exit cycles and the
		// model's one-seat-per-occupant index stay stable.
		OccupantId = FGuid::NewGuid();
	}
	return OccupantId;
}
