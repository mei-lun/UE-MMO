#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "SeatModel.h"
#include "VehicleTypes.h"
#include "CombatVehicle.h"

#include "VehicleInteractionComponent.generated.h"

class ACombatVehicle;
class UInputAction;

/**
 * Why the component itself refused an enter attempt - the layer ABOVE the
 * seat model's own EVehicleSeatRejectReason. Append only - never renumber, so
 * log lines and tests keep their meaning. 0 always means "accepted"; the
 * model's own refusals surface as ModelRefused with the model reason intact
 * (FVehicleEnterAttempt::ModelReject).
 */
enum class EVehicleInteractionReject : uint8
{
	/** The enter attempt was accepted. */
	None = 0,
	/** No FSeatOccupancyModel was bound (the wiring's injected owner card did not run). */
	NoSeatModel = 1,
	/** The owning pawn's health pool is dead (contract section 4: death outranks everything). */
	OwnerDead = 2,
	/** A menu/result screen captured the input (the M2-012 CapturedToUI precedent). */
	UICapturedInput = 3,
	/** No registered vehicle/seat candidate resolved at all. */
	NoVehicleInRange = 4,
	/** The seat model refused; the model's reason and detail carry the cause. */
	ModelRefused = 5
};

/** Stable human-readable text for one component reject reason (diagnostics). */
inline const TCHAR* GetVehicleInteractionRejectText(EVehicleInteractionReject Reject)
{
	switch (Reject)
	{
	case EVehicleInteractionReject::None:
		return TEXT("accepted");
	case EVehicleInteractionReject::NoSeatModel:
		return TEXT("no seat model bound: the wiring must inject the FSeatOccupancyModel before any enter");
	case EVehicleInteractionReject::OwnerDead:
		return TEXT("the owning pawn is dead: a dead driver cannot enter a seat");
	case EVehicleInteractionReject::UICapturedInput:
		return TEXT("a menu/result screen captured the input: the enter request is refused while the UI holds the focus");
	case EVehicleInteractionReject::NoVehicleInRange:
		return TEXT("no registered vehicle/seat candidate resolved: nothing legal is near enough to enter");
	case EVehicleInteractionReject::ModelRefused:
		return TEXT("the seat model refused the enter: see the model's own reject reason");
	default:
		return TEXT("unknown vehicle interaction reject");
	}
}

/**
 * One enter attempt's report (the FWeaponMountOutcome precedent: a caller-
 * facing report value, never a profile value). ModelReject is only meaningful
 * when Reject == ModelRefused.
 */
struct FVehicleEnterAttempt
{
	/** True only when the seat model committed the occupation. */
	bool bEntered = false;

	/** The component-layer outcome (0 = accepted). */
	EVehicleInteractionReject Reject = EVehicleInteractionReject::None;

	/** The seat model's own rejection when the model refused. */
	EVehicleSeatRejectReason ModelReject = EVehicleSeatRejectReason::None;

	/** Human-readable detail (the model's detail line or the component reason). */
	FString Detail;

	/** The entered vehicle id (valid only on success). */
	FName VehicleId = NAME_None;

	/** The entered seat id (valid only on success). */
	FName SeatId = NAME_None;
};

/**
 * One registered vehicle/seat pair of the component's local registry: the weak
 * hull, the seat definition VALUES (the M5-038 loader owns the source table;
 * the wiring hands the row in) and the hull's own validity gate (the M5-039
 * MakeVehicleValidityGate production face, resolved per request through the
 * model's VehicleValidityGate injection point).
 */
struct FVehicleSeatEntry
{
	/** The hull (weak: a destroyed hull drops out of every search). */
	TWeakObjectPtr<ACombatVehicle> Vehicle;

	/** The seat row values (seat/exit offsets in cm, relative to the vehicle origin). */
	FVehicleSeatDefinition Seat;

	/** The hull's own validity gate (SeatModel.h's named M5-039 wiring face). */
	FSeatOccupancyModel::FSeatEnterGate ValidityGate;
};

/**
 * M5-041: the enter/exit interaction between the owning pawn and the world's
 * vehicles (interface contract section 8 "载具权限"; task M5-040 owns the
 * occupancy model, M5-042 the movement, M5-043 the mount, M5-044 the vehicle
 * input routing). The component lives on the pawn and wires the pure
 * FSeatOccupancyModel into the live World:
 *
 * - The model is INJECTED (BindSeatModel) - the component never owns it; the
 *   model's lifetime belongs to the World wiring (a later card). With no
 *   model bound the component refuses every enter with NoSeatModel: the bare
 *   pawn degrades gracefully, never fakes an occupation.
 * - The vehicle registry (RegisterVehicle) feeds the seat definition values
 *   and registers the (vehicle, seat) pair in the model; it also installs the
 *   three production gates ONCE (Distance -> the nearest-seat range, Validity
 *   -> the hull's MakeVehicleValidityGate face, Permission -> the Driver
 *   role). The model stays the single authority: the enter runs through
 *   TryEnterSeat's atomic commit, never a fake occupation.
 * - Enter (the V toggle, routed through PrototypeCharacter's input setup):
 *   death and UI-capture refusals run component-side first (contract section
 *   4 order), then the nearest registered candidate feeds the model. On
 *   acceptance the driver's character-side control face pauses (see
 *   IsPawnControlPaused) and the operator faction is recorded (contract
 *   section 8 filters friendly fire on the OPERATOR faction; M5-044 reads it
 *   for its shot contexts). The pawn is not possessed into the vehicle and
 *   not teleported here - the physical seat placement is M5-042's face.
 * - Exit: the seated seat's ExitOffsetCm is validated by a capsule ground
 *   sweep (the hull being left is ignored); every candidate blocked refuses
 *   the exit and the driver stays seated (no wall clipping). On success the
 *   character drops at the exit spot, the control face restores and the
 *   LeaveSeat ends the occupation.
 * - Forced cleanup (contract section 8: destroy/LeaveRoom/Retry/EndPlay):
 *   the hull's one-shot destroy event, the owner's death, the unified reset
 *   and the component's own EndPlay all funnel into one idempotent restore -
 *   the seat frees, the control face restores, the controller/possession is
 *   never touched (a surviving driver keeps its PlayerController and input
 *   context).
 *
 * Idempotency: a duplicated enter lands on the model's AlreadyInThisSeat
 * refusal (the occupant-side check outranks every gate) and touches nothing;
 * a duplicated exit is the not-seated no-op; the destroy subscription and the
 * vacate seam are bound once per hull (the vacate callback slot on the hull
 * is an overwrite seam, never an accumulating one).
 *
 * Persistence: every member here is World/Session state (contract section
 * 0.5) - nothing reaches the profile/save (the M5-040 SeatModel precedent).
 */
UCLASS(ClassGroup = (Vehicles))
class UEMMO_API UVehicleInteractionComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UVehicleInteractionComponent();

	/** The cleanup face (contract section 8): seat free, subscription detached, control restored. */
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// -- Wiring (the World owner's injection points) ---------------------------

	/**
	 * Injects the seat occupancy model (M5-040). NOT owned: the wiring owns
	 * model and component together and must keep the model addressable for as
	 * long as vehicles can be entered. Without a binding every enter refuses
	 * with NoSeatModel (the documented bare-pawn degradation).
	 */
	void BindSeatModel(FSeatOccupancyModel& InModel);

	/**
	 * Registers one vehicle/seat pair: validates the seat row against the
	 * hull's definition, registers the pair in the bound model (idempotent
	 * through the model's own registration guard) and upserts the local entry
	 * with a fresh MakeVehicleValidityGate. Also installs the three production
	 * gates exactly once per component (first registration wins; the gates
	 * resolve ids through the local registry, so later registrations are
	 * covered). Returns false with OutError on any refusal - no partial state.
	 */
	bool RegisterVehicle(ACombatVehicle& Vehicle, const FVehicleSeatDefinition& Seat, FString* OutError = nullptr);

	/**
	 * Records the driver-side operator faction (contract section 8: friendly
	 * fire filters on the operator faction while seated). Unset = unaligned.
	 * Set by the pawn wiring with the same constant the weapon fire source
	 * registers with.
	 */
	void SetOperatorFaction(FName InFaction) { OperatorFaction = InFaction; }

	/**
	 * Injects the UI-capture gate (true = the enter request is refused with
	 * UICapturedInput). Unset, the default reads the local HUD's screen
	 * validity and the M2-012 CapturedToUI phase (the menu UIOnly acceptance
	 * face). An empty TFunction restores the default.
	 */
	void SetUICapturePredicate(TFunction<bool()> InPredicate);

	// -- Interaction ------------------------------------------------------------

	/**
	 * Enters the nearest registered seat: component refusals first (no model,
	 * dead owner, captured UI), then a seated re-entry rides the model's
	 * AlreadyInThisSeat path, then the nearest resolved candidate runs the
	 * model's atomic TryEnterSeat (gates included). On acceptance the driver
	 * control face pauses and the hull's vacate seam + destroy subscription
	 * are bound. The attempt report always names its layer.
	 */
	FVehicleEnterAttempt TryEnterNearestVehicle();

	/**
	 * Leaves the seat: the seated seat's ExitOffsetCm is validated by a
	 * capsule ground sweep (the hull ignored); every candidate blocked refuses
	 * (false + OutRefusal) and the driver stays seated. On success the
	 * character drops at the exit spot, the control face restores and the
	 * LeaveSeat ends the occupation. Idempotent: not seated = the false no-op.
	 */
	bool TryExitVehicle(FString* OutRefusal = nullptr);

	/**
	 * The V-key toggle: enter when walking, exit when seated. Both directions
	 * run through their own refusal paths, so a duplicated input binding
	 * (a re-setup) can never double-apply.
	 */
	void TryInteract();

	/**
	 * M5-041: the UFUNCTION entry the pawn's input setup binds the V key to.
	 * Binding the COMPONENT object (not a bare lambda) keeps the binding
	 * covered by the pawn's ClearBindingsForObject re-setup guard, so a
	 * repeated setup never stacks a second V trigger (the M1-012 pin).
	 */
	UFUNCTION()
	void HandleVehicleInteractTriggered();

	// -- Queries ----------------------------------------------------------------

	/**
	 * True while the driver's character-side control face is suspended
	 * (movement input, combat intents, character fire/reload). The pause flag
	 * of the PrototypeCharacter gating; 042/044 own the vehicle-side input
	 * that replaces it.
	 */
	bool IsPawnControlPaused() const { return bDriverControlsPaused; }

	/** True while the occupant holds a seat (bookkeeping, not the model's state). */
	bool IsSeated() const { return bSeated; }

	FName GetSeatedVehicleId() const { return SeatedVehicleId; }
	FName GetSeatedSeatId() const { return SeatedSeatId; }

	/** The seated hull (null when not seated or the hull is gone). */
	ACombatVehicle* GetSeatedVehicle() const { return SeatedVehicle.Get(); }

	/** The World-minted occupant identity (stable per component, valid GUID). */
	const FGuid& GetOccupantId() const { return OccupantId; }

	/** The recorded operator faction (contract section 8; unset = unaligned). */
	FName GetOperatorFaction() const { return OperatorFaction; }

	/**
	 * The V-key input action (created once, owned by the component so the
	 * shared PrototypeCharacter.h needs no member). The pawn's input setup
	 * maps and binds it.
	 */
	UInputAction* EnsureVehicleInteractAction();

	/**
	 * How many destroy events the component's subscription received. The
	 * once-only binding pin: a duplicated AddUObject would surface here.
	 */
	int32 GetVehicleDestroyedNotificationCount() const { return VehicleDestroyedNotifications; }

	/**
	 * The unified-reset (F2/retry) face: a seated driver leaves the seat and
	 * regains control. Idempotent; a no-op without a seat.
	 */
	void NotifyOwnerReset();

private:
	/** The one idempotent forced-restore (destroy/death/reset/EndPlay share it). */
	void RestoreDriverAfterForcedCleanup(const TCHAR* Why);

	/** The owner's health OnDied handler (contract section 8: death clears the seat). */
	void HandleOwnerDied();

	/** The hull's one-shot destroy handler (the forced restore of the driver). */
	void HandleVehicleDestroyed(const FVehicleDestroyedEvent& Event);

	/** Binds the owner-death cleanup exactly once (lazy: no BeginPlay ordering dependence). */
	void EnsureOwnerWiring();

	/** Installs the default HUD UI-capture predicate exactly once. */
	void EnsureUICapturePredicate();

	/** Installs the three production gates into the bound model exactly once. */
	void InstallGatesIfNeeded();

	/** Mints the occupant GUID once per component (the World-wiring rule). */
	FGuid& GetOrCreateOccupantId();

	const FVehicleSeatEntry* FindEntry(FName VehicleId, FName SeatId) const;
	FVehicleSeatEntry* FindEntryMutable(FName VehicleId, FName SeatId);

	/**
	 * The injected occupancy model (M5-040). NOT owned - the World wiring owns
	 * model and component together (the M5-039 SetSeatVacateCallback lifetime
	 * rule); raw pointer by contract, never a profile value.
	 */
	FSeatOccupancyModel* BoundSeatModel = nullptr;

	/** The local vehicle/seat registry (weak hulls + definition values). */
	TArray<FVehicleSeatEntry> Entries;

	/** The World-minted occupant identity, minted once per component. */
	FGuid OccupantId;

	/** The seated bookkeeping (kept in lockstep with the model's own state). */
	bool bSeated = false;
	FName SeatedVehicleId = NAME_None;
	FName SeatedSeatId = NAME_None;
	TWeakObjectPtr<ACombatVehicle> SeatedVehicle;

	/** The hull whose OnVehicleDestroyed carries this component's subscription. */
	TWeakObjectPtr<ACombatVehicle> BoundDestroySource;

	/** The driver-side operator faction (contract section 8; unset = unaligned). */
	FName OperatorFaction = NAME_None;

	/** The suspended character-side control face (the pawn gating reads this). */
	bool bDriverControlsPaused = false;

	/** The UI-capture gate (empty = not yet defaulted; see SetUICapturePredicate). */
	TFunction<bool()> UICapturePredicate;

	/** The gates-install once-guard (first RegisterVehicle wins). */
	bool bGatesInstalled = false;

	/** The owner-death binding once-guard. */
	bool bOwnerDeathBound = false;

	/** The V-key action, created once and owned here (no hot-header member). */
	UPROPERTY(Transient)
	TObjectPtr<UInputAction> VehicleInteractAction;

	/** The destroy-notification counter (the once-only subscription pin). */
	int32 VehicleDestroyedNotifications = 0;
};
