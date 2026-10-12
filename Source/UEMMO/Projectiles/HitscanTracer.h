#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "HitscanTracer.generated.h"

class UStaticMeshComponent;

/**
 * M5-058: the hitscan tracer beam - the perceivable-feedback face of a
 * committed hitscan shot. The M5-026 contract pins ExecuteHitscan as a pure
 * query-and-submit pipeline that spawns nothing, so the caller's presentation
 * seam (PrototypeCharacter::HandleWeaponShotCommitted) owns this actor: one
 * beam per pellet, from the shot origin to the pellet's end (the unified hit
 * point, or the max-range endpoint when the pellet hit nothing).
 *
 * Contract pinned here:
 * - Presentation only: no collision of any kind, no tick, no damage path.
 * The beam never participates in combat; it is the visual echo of a shot that
 * already committed through the M5-022 transaction.
 * - Lifetime on the engine LifeSpan timer (world clock, pause-frozen): the
 * beam flashes for the M5-058 constant and the engine destroys it - no manual
 * bookkeeping, no leak path.
 * - Headless-safe construction: the engine basic cube load failing only
 * leaves the beam invisible (the same degradation the M5-035 projectile
 * visual carrier established for cooked/headless environments).
 */
UCLASS()
class AHitscanTracer : public AActor
{
    GENERATED_BODY()

public:
    AHitscanTracer();

    /**
     * Positions and scales the beam between two world points. Degenerate spans
     * (the endpoints closer than the minimum visible length) clamp to that
     * minimum along the direction, so a zero-length shot still flashes.
     */
    void InitializeBeam(const FVector& From, const FVector& To);

    /** The stretched cube carrier (pure presentation, NoCollision). */
    UPROPERTY(VisibleAnywhere, Category = "UEMMO|WeaponFeedback")
    TObjectPtr<UStaticMeshComponent> Beam;
};
