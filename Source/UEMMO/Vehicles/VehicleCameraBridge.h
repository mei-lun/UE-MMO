#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "CombatVehicle.h"

#include "VehicleCameraBridge.generated.h"

class USideViewCameraComponent;
class USceneComponent;

/**
 * M5-042: the driver-seat camera bridge (interface contract section 8). While
 * the driver holds the seat, the pawn's fixed side-view camera rig
 * (USideViewCameraComponent, the M1-030 componentized boom on the pawn root)
 * is re-anchored onto the vehicle's ground anchor; on exit, on the hull's
 * one-shot destroy broadcast and on the bridge's own EndPlay the character
 * anchor is restored exactly as it was found.
 *
 * Bridge form - SpringArm re-parent, not per-frame follow: the rig is
 * ATTACHED to the hull root with the M5-037 follow-camera values from the
 * hull's definition (pivot = vehicle origin + CameraHeightCm, boom =
 * CameraDistanceCm, pitch = CameraPitchDeg; the M0 fixed side-view yaw
 * stays). The engine's transform propagation then follows the vehicle for
 * free - zero per-frame bridge work and the follow survives even a suspended
 * bridge tick. The rig's OWN per-frame anchor logic computes from the PAWN
 * (it would fight the vehicle follow), so its tick is suspended while
 * bridged; the restore re-enables it. This is runtime component wiring only:
 * no existing camera/character/pawn source file is touched, and the restore
 * re-attaches the rig to its saved parent with its saved framing.
 *
 * Seat-state ownership: the bridge is a COMMAND face, not an occupancy
 * authority - the World wiring (the M5-041 integration card owns the enter/
 * exit flow) calls BridgeToVehicle when the driver accepts a seat and
 * RestoreCharacterAnchor on the accepted exit and on the forced-restore
 * paths (death, reset, EndPlay). The hull's destroy broadcast additionally
 * self-restores through the bridge's own subscription (contract section 8:
 * the destroyed vehicle's driver is restored without losing the camera), so
 * a wiring gap cannot strand the camera on a dead hull. A refused command
 * never touches the rig (no half-bridged state).
 *
 * Persistence: World/Session state only (contract section 0.5) - the saved
 * framing lives in weak references and values on this component and never
 * reaches the profile/save (the M5-040 SeatModel precedent).
 */
UCLASS(ClassGroup = (Vehicles))
class UEMMO_API UVehicleCameraBridge : public UActorComponent
{
	GENERATED_BODY()

public:
	UVehicleCameraBridge();

	// The cleanup face (contract section 8): a bridge torn down with its pawn
	// restores the rig first, so the camera never stays parented to a dying
	// vehicle.
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/**
	 * Bridges the owner pawn's side-view rig onto the hull's ground anchor
	 * with the hull definition's follow-camera values. Refuses (false +
	 * OutError, rig untouched) without a USideViewCameraComponent on the
	 * owner or without a loaded vehicle definition on the hull. Idempotent:
	 * re-bridging the same hull is the accepted no-op; bridging a different
	 * hull restores the previous anchor first. Subscribes the hull's
	 * one-shot destroy broadcast for the self-restore face.
	 */
	bool BridgeToVehicle(ACombatVehicle& Vehicle, FString* OutError = nullptr);

	/**
	 * Restores the character anchor: the rig re-attaches to its saved parent
	 * (the pawn root) with its saved framing, snaps to the character anchor
	 * computed with the rig's own M1-030 pure math, and the rig's per-frame
	 * anchor tick resumes. Idempotent: not bridged = the false no-op with no
	 * side effects.
	 */
	bool RestoreCharacterAnchor();

	/** True while the rig is bridged to a live hull. */
	bool IsBridged() const { return BridgedRig.IsValid(); }

	/** The bridged hull (null when not bridged or the hull is gone). */
	ACombatVehicle* GetBridgedVehicle() const { return BridgedVehicle.Get(); }

private:
	/** The hull's one-shot destroy handler: the self-restore face. */
	void HandleVehicleDestroyed(const FVehicleDestroyedEvent& Event);

	/** Resolves the owner pawn's rig (the refuse-with-reason helper). */
	bool ResolveRig(USideViewCameraComponent*& OutRig, FString* OutError) const;

	/** The character anchor snap used by the restore (the rig's own math). */
	FVector ComputeCharacterAnchor(const USideViewCameraComponent& Rig) const;

	/** The bridged hull + rig (weak: a dead side never dangles the restore). */
	TWeakObjectPtr<ACombatVehicle> BridgedVehicle;
	TWeakObjectPtr<USideViewCameraComponent> BridgedRig;

	/** The character-side framing captured at bridge time for the restore. */
	TWeakObjectPtr<USceneComponent> SavedAttachParent;
	FTransform SavedWorldTransform = FTransform::Identity;
	float SavedArmLengthCm = 0.0f;
	FRotator SavedWorldRotation = FRotator::ZeroRotator;
	bool bRigTickWasEnabled = false;
};
