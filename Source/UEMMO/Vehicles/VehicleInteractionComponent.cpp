#include "VehicleInteractionComponent.h"

#include "../PrototypeCharacter.h"
#include "../Combat/HealthComponent.h"
#include "../PrototypeHUD.h"
#include "../UI/RoomResultWidget.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Components/CapsuleComponent.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "InputAction.h"

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

	// The exit probe: the capsule sweep starts this far above the candidate
	// (a start already inside geometry is the blocked case) and searches this
	// deep below it for the resting floor.
	constexpr float M5_041_ExitProbeHeadroomCm = 8.0f;
	constexpr float M5_041_ExitProbeDepthCm = 300.0f;

	/**
	 * The exit drop spot: sweeps the pawn's capsule down the exit candidate
	 * column (the seat's ExitOffsetCm transformed by the hull; the hull being
	 * left and the pawn are ignored) and returns the capsule CENTER location
	 * resting on the first blocking ground. False when the capsule does not
	 * fit (a start penetration - the "all candidates blocked" case) or no
	 * ground resolved: the caller refuses the exit and the driver stays
	 * seated, so the character can never clip into blocking geometry.
	 */
	bool M5_041_FindExitTeleportSpot(const APrototypeCharacter& Pawn, const ACombatVehicle& Hull,
		const FVector& ExitOffsetCm, FVector& OutCenterLocation)
	{
		const UWorld* World = Pawn.GetWorld();
		const UCapsuleComponent* Capsule = Pawn.GetCapsuleComponent();
		if (World == nullptr || Capsule == nullptr)
		{
			return false;
		}
		const float HalfHeight = Capsule->GetUnscaledCapsuleHalfHeight();
		const FVector Candidate = Hull.GetActorTransform().TransformPosition(ExitOffsetCm);
		const FVector SweepStart = Candidate + FVector(0.0f, 0.0f, HalfHeight + M5_041_ExitProbeHeadroomCm);
		const FVector SweepEnd = Candidate - FVector(0.0f, 0.0f, M5_041_ExitProbeDepthCm);

		FCollisionShape CapsuleShape = FCollisionShape::MakeCapsule(Capsule->GetUnscaledCapsuleRadius(), HalfHeight);
		FCollisionQueryParams Params(SCENE_QUERY_STAT(M5_041VehicleExit), /*bTraceComplex*/ false);
		Params.AddIgnoredActor(&Pawn);
		Params.AddIgnoredActor(&Hull);

		FHitResult Hit;
		if (!World->SweepSingleByChannel(Hit, SweepStart, SweepEnd, FQuat::Identity, ECC_Pawn, CapsuleShape, Params))
		{
			return false;
		}
		if (Hit.bStartPenetrating || !Hit.bBlockingHit)
		{
			// A start inside geometry means the candidate column has no room
			// for the capsule (a wall/ceiling over the exit); a non-blocking
			// touch is no floor either. Both refuse - no teleport, no clip.
			return false;
		}
		OutCenterLocation = FVector(Candidate.X, Candidate.Y, Hit.ImpactPoint.Z + HalfHeight);
		return true;
	}
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

	// 6. Accepted: the bookkeeping locks onto the hull and the control face
	// pauses (an idempotent flag set - the model already refused the
	// duplicated enter, so this runs exactly once per occupation).
	ACombatVehicle* Hull = Best->Vehicle.Get();
	bSeated = true;
	SeatedVehicleId = Request.VehicleId;
	SeatedSeatId = Request.SeatId;
	SeatedVehicle = Hull;
	bDriverControlsPaused = true;
	Attempt.bEntered = true;
	Attempt.VehicleId = Request.VehicleId;
	Attempt.SeatId = Request.SeatId;

	// The pause face starts clean: a trigger held across the enter moment is
	// dropped (the M5-033 release entry early-outs when nothing is held). The
	// vehicle's own fire routing is M5-044's input face.
	Pawn->OnFireInputReleased();

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
		TEXT("UEMMO M5-041: the driver entered seat '%s' of vehicle '%s' (character control suspended)."),
		*Request.SeatId.ToString(), *Request.VehicleId.ToString());
	return Attempt;
}

bool UVehicleInteractionComponent::TryExitVehicle(FString* OutRefusal)
{
	const auto Refuse = [OutRefusal](const FString& Why)
	{
		if (OutRefusal != nullptr)
		{
			*OutRefusal = Why;
		}
		UE_LOG(LogUEMMOVehicleInteraction, Verbose, TEXT("UEMMO M5-041: exit refused: %s"), *Why);
		return false;
	};

	// Idempotent by design: the duplicated exit is the named no-op (no model
	// call, no restore, no second teleport).
	if (!bSeated)
	{
		return Refuse(TEXT("not seated: the exit request is the idempotent no-op"));
	}
	APrototypeCharacter* Pawn = Cast<APrototypeCharacter>(GetOwner());
	ACombatVehicle* Hull = SeatedVehicle.Get();
	if (Pawn == nullptr || Hull == nullptr)
	{
		// The hull is gone without its destroy event reaching this component
		// (wiring misuse): the safe face is the forced restore, never a
		// half-state where the model holds a seat the pawn cannot leave.
		RestoreDriverAfterForcedCleanup(TEXT("exit with a gone vehicle"));
		return true;
	}

	// The exit spot: the seated seat definition's ExitOffsetCm (the M5-037
	// row the registry handed in), validated by the capsule ground sweep.
	const FVehicleSeatEntry* Entry = FindEntry(SeatedVehicleId, SeatedSeatId);
	// A missing entry (the seat row unregistered mid-occupation) falls back to
	// the hull origin - the entry array is never pruned in normal operation.
	const FVector ExitOffset = Entry != nullptr ? Entry->Seat.ExitOffsetCm : FVector::ZeroVector;
	FVector CenterLocation;
	if (!M5_041_FindExitTeleportSpot(*Pawn, *Hull, ExitOffset, CenterLocation))
	{
		return Refuse(FString::Printf(TEXT("every exit candidate of seat '%s' is blocked: the driver stays seated (no wall clipping)"),
			*SeatedSeatId.ToString()));
	}

	// The drop: the validated spot (no swept move needed - the sweep above
	// proved the capsule fits there), then the shared restore ends the
	// occupation and reopens the control face.
	Pawn->SetActorLocation(CenterLocation, /*bSweep*/ false, nullptr, ETeleportType::TeleportPhysics);
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
	// 2. The bookkeeping and the control face (both idempotent flags): the
	// character-side control face reopens. No possession change, no teleport -
	// the surviving driver keeps its PlayerController and input context (the
	// physical placement is M5-042's face; the pawn stands where it entered).
	bSeated = false;
	SeatedVehicleId = NAME_None;
	SeatedSeatId = NAME_None;
	SeatedVehicle = nullptr;
	bDriverControlsPaused = false;
	// 3. The subscription and the vacate seam detach while the hull is still
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
