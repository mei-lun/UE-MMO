#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Templates/Function.h"

#include "VehicleMovementComponent.generated.h"

class ACombatVehicle;
class UPrimitiveComponent;

/**
 * M5-042: the X/Y ground drive of the ACombatVehicle hull (interface contract
 * section 8 "Movement在042持有"). The hull body is a QueryOnly box root
 * (M5-039: a passive hittable target, nothing simulates), so the displacement
 * is produced HERE as a kinematic swept move - the component owns an
 * acceleration/brake planar velocity model and resolves every step through
 * the hull root's MoveComponent sweep, which makes walls and room borders
 * stop or slide the hull instead of letting it penetrate.
 *
 * Axes (contract section 0.6): X lateral, Y depth, Z height. The drive input
 * is a planar (X/Y) pair; there is deliberately NO jump/fly input - the first
 * version is a ground platform, and Z is decided by collision alone (gravity
 * integrates from the world's own gravity until the hull's swept down-move is
 * blocked by the floor; the hull then holds a small hover clearance so the
 * next planar sweep can never touch the floor it rests on). The Y axis is a
 * first-class drive axis (no plane lock) - depth walking is the project's
 * core movement feature.
 *
 * Speed/acceleration come from the hull's loaded FVehicleDefinition
 * (MaxSpeedCmS / AccelerationCmS2, the M5-037 validated value types) through
 * Configure. Diagonal input is normalized (the M1-029 precedent): a unit
 * input direction times the scalar acceleration, and the planar speed clamp
 * keeps the diagonal top speed equal to the straight one.
 *
 * Drive authority (contract section 8 "载具权限"): the component accepts
 * SetDriveInput only while a driver holds the seat AND the drive controls are
 * not suspended. The seat knowledge arrives INJECTED (the same pattern as the
 * M5-039 SetSeatVacateCallback seam) because the occupancy model is owned by
 * the World wiring, not by the vehicle: either a bool flag
 * (SetDriverPresent - what the M5-041 interaction wiring will set on
 * enter/exit) or a provider function reading the FSeatOccupancyModel
 * directly. A refused input ALSO clears any held throttle, so a driver lost
 * mid-drive (death, destroy, exit, suspension) decelerates to a stop instead
 * of driving on with stale input. A dead or destroyed hull never drives.
 *
 * Tick form: ACombatVehicle cannot tick (PrimaryActorTick.bCanEverTick=false,
 * and CombatVehicle.* is not this card's file), but a component tick function
 * is registered independently of the actor's own tick, so the component
 * enables its own PrimaryComponentTick and forwards into the public
 * TickMovement entry - the same entry tests and wiring call for
 * deterministic stepping.
 *
 * Persistence: every member is World/Session state (contract section 0.5) -
 * nothing here reaches the profile/save (the M5-040 SeatModel precedent).
 */
UCLASS(ClassGroup = (Vehicles))
class UEMMO_API UVehicleMovementComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UVehicleMovementComponent();

	// The component carries the hull's per-frame drive (see the class comment
	// for why the tick lives here and not on the non-ticking vehicle actor).
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/**
	 * Loads the planar tuning from the owner hull's loaded FVehicleDefinition:
	 * MaxSpeedCmS and AccelerationCmS2 (the M5-037 validator already
	 * guarantees finite, positive, in-bounds values, so no second validation
	 * runs here), and defaults the braking deceleration to the acceleration
	 * (the definition carries no separate brake field). Refuses (false +
	 * OutError, no partial state) without a live hull or without a loaded
	 * definition; a re-Configure refreshes the values.
	 */
	bool Configure(FString* OutError = nullptr);

	/** True once Configure accepted a definition (planar drive is then live). */
	bool IsConfigured() const { return bConfigured; }

	/**
	 * Stores the planar drive input (-1..1 per axis; non-finite counts as 0,
	 * out-of-range clamps). Returns false and CLEARS the held input when the
	 * drive authority is off (no driver present, controls suspended, or a
	 * dead/destroyed hull) - a refused call can never leave stale throttle
	 * behind. The stored input is applied on the next TickMovement.
	 */
	bool SetDriveInput(float X, float Y);

	/** The sanitized stored input (0 when the last call was refused). */
	FVector2D GetDriveInput() const { return DriveInput; }

	// -- Drive authority (the injected gating seams) ---------------------------

	/**
	 * The manual in-seat flag (the seam the M5-041 interaction wiring will
	 * set on enter/exit; the automation tests set it directly). Ignored while
	 * a driver presence provider is injected.
	 */
	void SetDriverPresent(bool bInPresent) { bDriverPresent = bInPresent; }

	/**
	 * True when a driver holds the seat: the injected provider (a
	 * FSeatOccupancyModel query, the wiring's production form) when set,
	 * otherwise the manual flag.
	 */
	bool IsDriverPresent() const;

	/**
	 * The manual suspension flag (drive controls captured by a higher-priority
	 * face; default false). Ignored while a suspension provider is injected.
	 */
	void SetDriveControlsSuspended(bool bInSuspended) { bControlsSuspended = bInSuspended; }

	/** True when the drive controls are suspended (provider or manual flag). */
	bool IsDriveControlsSuspended() const;

	/** Injects the seat-occupancy query (overrides the manual flag when set). */
	void SetDriverPresenceProvider(TFunction<bool()> InProvider) { DriverPresenceProvider = MoveTemp(InProvider); }

	/** Injects the control-suspension query (overrides the manual flag). */
	void SetControlSuspensionProvider(TFunction<bool()> InProvider) { ControlSuspensionProvider = MoveTemp(InProvider); }

	/**
	 * The full drive authority gate: a live, non-destroyed hull with a driver
	 * present and unsuspended controls. SetDriveInput and TickMovement both
	 * read it (the tick re-read catches a driver lost mid-drive).
	 */
	bool IsDriveInputAccepted() const;

	// -- Simulation -------------------------------------------------------------

	/**
	 * Advances one step: the authority re-check, the vertical pass (gravity +
	 * swept ground contact) and the planar pass (accelerate/brake, then the
	 * swept slide move). Public so tests and wiring step the simulation
	 * deterministically; TickComponent forwards here. Non-finite or
	 * non-positive steps are treated as zero (the step is skipped).
	 */
	void TickMovement(float DeltaSeconds);

	// -- Tuning and state queries ----------------------------------------------

	/**
	 * Overrides the braking deceleration (cm/s^2, finite and > 0; the default
	 * is the configured acceleration). A re-Configure resets the override.
	 */
	bool SetBrakingDecelerationCmS2(float InBrakingCmS2);

	float GetBrakingDecelerationCmS2() const { return BrakingDecelerationCmS2; }

	/** The planar (X/Y) velocity the model integrates (cm/s; Z never enters). */
	const FVector2D& GetPlanarVelocity() const { return PlanarVelocity; }

	/** True while the hull rests on swept floor contact (the hover hold). */
	bool IsGrounded() const { return bGrounded; }

	/** The configured top speed (0 while unconfigured). */
	float GetConfiguredMaxSpeedCmS() const { return MaxSpeedCmS; }

	/** The configured acceleration (0 while unconfigured). */
	float GetConfiguredAccelerationCmS2() const { return AccelerationCmS2; }

	/** The owner hull (null when the owner is not an ACombatVehicle). */
	ACombatVehicle* GetVehicle() const;

private:
	/** Gravity + swept floor contact: Z is decided by collision, never by input. */
	void UpdateVertical(float DeltaSeconds, UPrimitiveComponent& Body);

	/** Accelerate/brake the planar velocity, then the swept slide move. */
	void UpdatePlanar(float DeltaSeconds, UPrimitiveComponent& Body);

	/** True when blocking support still exists right below the hover height. */
	bool ProbeGroundBelow(const UPrimitiveComponent& Body) const;

	/**
	 * The stored planar input (sanitized -1..1 per axis; zeroed on refusal).
	 * World-transient, deliberately not a UPROPERTY (nothing here may reach a
	 * save path).
	 */
	FVector2D DriveInput = FVector2D::ZeroVector;

	/** The integrated planar velocity (cm/s); the swept move consumes it. */
	FVector2D PlanarVelocity = FVector2D::ZeroVector;

	/** The vertical velocity (cm/s) gravity integrates while ungrounded. */
	float VerticalVelocityCmS = 0.0f;

	/** Configure-loaded tuning (the M5-037 definition values). */
	float MaxSpeedCmS = 0.0f;
	float AccelerationCmS2 = 0.0f;
	float BrakingDecelerationCmS2 = 0.0f;

	bool bConfigured = false;

	/** True while the hull holds swept floor contact (the hover hold). */
	bool bGrounded = false;

	/** The injected drive authority (see the setter comments). */
	bool bDriverPresent = false;
	bool bControlsSuspended = false;
	TFunction<bool()> DriverPresenceProvider;
	TFunction<bool()> ControlSuspensionProvider;
};
