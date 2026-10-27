#include "VehicleCameraBridge.h"

#include "../Character/SideViewCameraComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

using namespace UE::UEMMO::Tasks::M1_030;

/**
 * M5-042: implementation of the SpringArm re-parent camera bridge. Rules live
 * in the header comment; the implementation notes:
 * - The bridge performs runtime component wiring only (attach, framing,
 *   tick enablement): no camera/character source file is touched, and every
 *   mutation is captured first so the restore is the exact inverse.
 * - The restore snaps the pivot to the character anchor using the rig's own
 *   public M1-030 pure math plus one ground trace - without the snap the
 *   resumed rig tick would glide the camera back from the vehicle's last
 *   follow position (the anchor X is exponentially smoothed by design).
 */

DEFINE_LOG_CATEGORY_STATIC(LogUEMMOVehicleCameraBridge, Log, All);

UVehicleCameraBridge::UVehicleCameraBridge()
{
	// No per-frame duty: the follow is the engine's transform propagation and
	// every restore face is event/entry driven (the M5-039/041 precedent).
	PrimaryComponentTick.bCanEverTick = false;
}

void UVehicleCameraBridge::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// A bridge torn down with its pawn (or by the wiring) hands the camera
	// back first - the idempotent no-op when nothing is bridged.
	RestoreCharacterAnchor();
	Super::EndPlay(EndPlayReason);
}

bool UVehicleCameraBridge::ResolveRig(USideViewCameraComponent*& OutRig, FString* OutError) const
{
	const AActor* Owner = GetOwner();
	if (Owner == nullptr)
	{
		if (OutError != nullptr)
		{
			*OutError = TEXT("the camera bridge has no owner actor");
		}
		return false;
	}
	USideViewCameraComponent* Rig = Owner->FindComponentByClass<USideViewCameraComponent>();
	if (Rig == nullptr)
	{
		if (OutError != nullptr)
		{
			*OutError = TEXT("the owning pawn has no USideViewCameraComponent: there is no side-view rig to bridge");
		}
		return false;
	}
	OutRig = Rig;
	return true;
}

bool UVehicleCameraBridge::BridgeToVehicle(ACombatVehicle& Vehicle, FString* OutError)
{
	const auto Refuse = [OutError](const FString& Why)
	{
		if (OutError != nullptr)
		{
			*OutError = Why;
		}
		return false;
	};

	// The duplicated command is the accepted no-op (the enter flow can race a
	// re-setup without ever double-applying the framing).
	if (IsBridged() && BridgedVehicle.Get() == &Vehicle)
	{
		return true;
	}

	USideViewCameraComponent* Rig = nullptr;
	if (!ResolveRig(Rig, OutError))
	{
		return false;
	}
	if (!Vehicle.HasVehicleDefinition())
	{
		// The M5-037 follow-camera fields ARE the bridged framing source; a
		// hull without a definition would fake a camera.
		return Refuse(TEXT("the hull has no vehicle definition loaded: the follow-camera values are the bridge's framing source"));
	}

	// Switching hulls restores the previous anchor first, so the saved
	// character-side framing below is always captured from the un-bridged rig.
	if (IsBridged())
	{
		RestoreCharacterAnchor();
	}

	// Capture the character-side framing for the exact-inverse restore.
	SavedAttachParent = Rig->GetAttachParent();
	SavedWorldTransform = Rig->GetComponentTransform();
	SavedArmLengthCm = Rig->TargetArmLength;
	SavedWorldRotation = Rig->GetComponentRotation();
	bRigTickWasEnabled = Rig->IsComponentTickEnabled();

	const FVehicleDefinition& Definition = Vehicle.GetVehicleDefinition();

	// The re-parent: the engine's transform propagation follows the vehicle
	// for free (see the header for why this beats a per-frame follow). The
	// pivot is the vehicle's ground anchor (origin + the definition's height),
	// the fixed side-view yaw stays (the M0 framing), pitch and boom come from
	// the definition.
	Rig->AttachToComponent(Vehicle.GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
	const FVector Pivot = Vehicle.GetActorLocation() + FVector(0.0f, 0.0f, Definition.CameraHeightCm);
	Rig->SetWorldLocation(Pivot);
	Rig->SetWorldRotation(FRotator(Definition.CameraPitchDeg, SideViewCameraYaw, 0.0f));
	Rig->TargetArmLength = Definition.CameraDistanceCm;
	// The rig's own per-frame anchor logic computes from the PAWN and would
	// fight the vehicle follow; it resumes on the restore.
	Rig->SetComponentTickEnabled(false);

	BridgedVehicle = &Vehicle;
	BridgedRig = Rig;

	// The destroy face: the hull's one-shot broadcast restores the surviving
	// driver's camera even if the wiring never calls RestoreCharacterAnchor
	// (contract section 8). RestoreCharacterAnchor detaches the subscription.
	Vehicle.OnVehicleDestroyed.AddUObject(this, &UVehicleCameraBridge::HandleVehicleDestroyed);

	UE_LOG(LogUEMMOVehicleCameraBridge, Log,
		TEXT("UEMMO M5-042: the driver camera bridged onto vehicle '%s' (pivot height %f cm, boom %f cm, pitch %f deg)."),
		*Definition.VehicleId.ToString(), Definition.CameraHeightCm, Definition.CameraDistanceCm, Definition.CameraPitchDeg);
	return true;
}

FVector UVehicleCameraBridge::ComputeCharacterAnchor(const USideViewCameraComponent& Rig) const
{
	// The rig's own anchor math (M1-030) with one ground trace of our own (the
	// trace face on the rig is private). The pawn's depth stands in for the
	// rig's private ReferenceY: a pawn that never changed depth since the rig
	// started (the constant case) reproduces the rig's value exactly, and in
	// every other case the rig's resumed tick corrects Y on the same frame (Y
	// is not smoothed by the rig - only X is).
	const AActor* Owner = GetOwner();
	const FVector PawnLocation = Owner != nullptr ? Owner->GetActorLocation() : Rig.GetComponentLocation();
	float GroundZ = PawnLocation.Z;
	if (UWorld* World = GetWorld())
	{
		FVector Start = PawnLocation;
		Start.Z += GroundTraceStartClearance;
		const FVector End(Start.X, Start.Y, Start.Z - Rig.Config.GroundTraceDistance);
		FHitResult Hit;
		// The engine's own ignore-actor constructor form (the rig's
		// TraceGroundZ precedent): the pawn never grounds the restore trace.
		FCollisionQueryParams Params(TEXT("M5_042CameraAnchorRestore"), /*bTraceComplex*/ false, Owner);
		if (World->LineTraceSingleByChannel(Hit, Start, End, ECC_Visibility, Params) && FMath::IsFinite(Hit.ImpactPoint.Z))
		{
			GroundZ = Hit.ImpactPoint.Z;
		}
	}
	return ComputeAnchorLocation(PawnLocation, GroundZ, PawnLocation.Y, Rig.Config);
}

bool UVehicleCameraBridge::RestoreCharacterAnchor()
{
	USideViewCameraComponent* Rig = BridgedRig.Get();
	if (Rig == nullptr)
	{
		// Not bridged (or the rig died with its pawn): the idempotent no-op.
		return false;
	}
	ACombatVehicle* Hull = BridgedVehicle.Get();

	// The destroy subscription detaches while the hull is still addressable
	// (weak + pending-kill-safe: after a GC the weak reference is null and the
	// delegate object is gone with the hull anyway).
	if (Hull != nullptr)
	{
		Hull->OnVehicleDestroyed.RemoveAll(this);
	}

	// 1. The boom back onto its saved parent (the pawn root) - or detached
	// world-space when the saved parent is gone, never a dangling attach.
	USceneComponent* SavedParent = SavedAttachParent.Get();
	if (SavedParent != nullptr && SavedParent->GetOwner() != nullptr && !SavedParent->GetOwner()->IsPendingKillPending())
	{
		Rig->AttachToComponent(SavedParent, FAttachmentTransformRules::KeepWorldTransform);
	}
	else
	{
		Rig->DetachFromComponent(FDetachmentTransformRules::KeepWorldTransform);
	}

	// 2. The saved framing back (the M0 side-view values the rig applies
	// itself on BeginPlay; the bridged values never leak into the walk state).
	Rig->TargetArmLength = SavedArmLengthCm;
	Rig->SetWorldRotation(SavedWorldRotation);

	// 3. The character anchor snap (see ComputeCharacterAnchor): the resumed
	// rig tick continues from here instead of gliding from the vehicle.
	Rig->SetWorldLocation(ComputeCharacterAnchor(*Rig));

	// 4. The rig's per-frame anchor logic resumes exactly when it was enabled
	// before the bridge (a rig the wiring kept disabled stays disabled).
	Rig->SetComponentTickEnabled(bRigTickWasEnabled);

	BridgedVehicle = nullptr;
	BridgedRig = nullptr;
	SavedAttachParent = nullptr;

	UE_LOG(LogUEMMOVehicleCameraBridge, Log,
		TEXT("UEMMO M5-042: the driver camera restored to the character anchor (saved boom %f cm)."),
		SavedArmLengthCm);
	return true;
}

void UVehicleCameraBridge::HandleVehicleDestroyed(const FVehicleDestroyedEvent& Event)
{
	// The once-only subscription pin (the 041 counting precedent): a
	// duplicated AddUObject is impossible through the guarded bridge path,
	// and a destroy of a hull this bridge does not hold is ignored.
	if (!IsBridged() || BridgedVehicle.Get() == nullptr)
	{
		return;
	}
	if (Event.Vehicle.Get() != BridgedVehicle.Get())
	{
		return;
	}
	// The destroyed vehicle's driver camera returns to the character anchor
	// (contract section 8) - without touching the controller or possession.
	RestoreCharacterAnchor();
}
