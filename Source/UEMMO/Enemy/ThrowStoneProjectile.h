// M8-019B: the goblin thrower's stone. A lightweight dedicated projectile
// (the M5-027 weapon pipeline carries weapon-shot provenance machinery -
// epochs, fire registries, item instances - that monsters do not have, so a
// monster throw is its own minimal actor; the report records this scoping).
// Contract from the card + the M8-007 proven values:
// - straight flight along the locked facing at the release depth
// - a 20x20x20cm hit box centered on the stone, active the whole flight
//   (source: one attack cube per frame on all 4 flight frames)
// - the hit enters the victim's unified health chain (UHealthComponent::
//   ApplyDamage - the same single removal entry the melee pipeline's
//   UnifiedHitApplier bottoms out at; never a second removal path)
// - the display is the 4f x 30ms looping ThrowStone page (68x17, 17px cells)
// - landing destroys the stone with a short linger (the impact feedback)
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "ThrowStoneProjectile.generated.h"

class UMaterialInterface;
class UStaticMeshComponent;
class UMaterialInstanceDynamic;

UCLASS()
class UEMMO_API AThrowStoneProjectile : public AActor
{
	GENERATED_BODY()

public:
	AThrowStoneProjectile();

	/**
	 * Spawns one stone at the thrower's hand anchor. Facing mirrors the X
	 * axis convention (+1 right / -1 left); the stone keeps the anchor's Y
	 * (the thrower was depth-aligned before the release). Returns null when
	 * the world/class is unusable.
	 */
	static AThrowStoneProjectile* Spawn(UWorld* World, const FVector& HandWorldLocation,
		int32 Facing, float Damage, AActor* InTarget,
		UMaterialInterface* StoneMaterial);

	/** Test seams: flight state, the locked data, and the hand anchor. */
	bool IsInFlight() const { return bInFlight; }
	bool HasLanded() const { return bLanded; }
	int32 GetFacing() const { return Facing; }
	float GetDamage() const { return Damage; }
	FVector GetHandAnchor() const { return HandAnchor; }
	FVector GetFlightVelocity() const { return FVector(Facing * FlightSpeedCmPerSecond, 0.f, 0.f); }
	float GetTravelledCm() const { return TravelledCm; }

protected:
	virtual void Tick(float DeltaSeconds) override;

	/** The picture quad (the engine plane, NoCollision - display only). */
	UPROPERTY(VisibleAnywhere, Category = "UEMMO|ThrowStone")
	TObjectPtr<UStaticMeshComponent> Plane;

	/** The stone's dynamic material instance (the fighter-lineage MID). */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> StoneMID;

	// ---------------- locked release data ----------------
	int32 Facing = 1;
	float Damage = 0.f;
	FVector HandAnchor = FVector::ZeroVector;
	TWeakObjectPtr<AActor> TargetWeak;

	// ---------------- flight state ----------------
	bool bInFlight = false;
	bool bLanded = false;
	double LandAtSeconds = 0.0;
	double BornSeconds = 0.0;
	float ElapsedMs = 0.f;
	float TravelledCm = 0.f;

	// ---------------- M8-007 proven values / project adaptations ----------------
	/** M8-007: 4 flight frames, 30ms each (120ms loop). */
	static constexpr int32 FrameCount = 4;
	static constexpr float FrameMs = 30.f;
	/** ThrowStone.img page: 68x17, four 17px cells. */
	static constexpr float CellPx = 17.f;
	static constexpr float PageWidthPx = 68.f;
	/** M6-022/M8-010 calibration: the stone quad is 17px * 1.7615 = ~30cm. */
	static constexpr float CmPerPixel = 1.7615f;
	/** M8-007: the flight hit box is a 20cm cube centered on the stone. */
	static constexpr float HitBoxHalfCm = 10.f;
	/**
	 * Design default (recorded in the report): the source .obj physics are
	 * not decoded; the daydungeon family sibling authors a 1400ms flight for
	 * the 500cm range (~357cm/s). 400cm/s keeps a comparable dodge window.
	 */
	static constexpr float FlightSpeedCmPerSecond = 400.f;
	/** Design default: hard flight time cap (safety; the range end lands first). */
	static constexpr float MaxFlightSeconds = 3.f;
	/**
	 * The landing distance: the M8-007/enemies.json throw range (500cm) - the
	 * AI only fires within it and the straight stone lands at that distance
	 * (the decoded source carries no gravity for the renewal flight).
	 */
	static constexpr float MaxFlightDistanceCm = 500.f;
	/** Design default: the landing linger (the impact feedback) before removal. */
	static constexpr float LandLingerSeconds = 0.15f;
};
