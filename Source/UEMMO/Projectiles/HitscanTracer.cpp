// M5-058: the hitscan tracer beam (owner 058, presentation face of the
// committed-shot callback). The engine basic cube stretched along the shot
// line - the same cooked-in carrier precedent as the M5-035 projectile visual.

#include "HitscanTracer.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"

namespace
{
    // The beam flash window: long enough to read at gameplay framerate, short
    // enough that consecutive taps read as separate shots.
    constexpr float M5_058_TracerLifeSpanSeconds = 0.12f;

    // The visible thickness (cm) of the beam cross-section. The projectile
    // bodies render at a 16cm cube; the tracer reads as a thinner streak.
    constexpr float M5_058_TracerThicknessCm = 10.0f;

    // The cube's unit edge is 100cm, so the scale divides by 100.
    constexpr float M5_058_CubeUnitEdgeCm = 100.0f;

    // A degenerate span still flashes at this minimum visible length (cm).
    constexpr float M5_058_MinimumBeamLengthCm = 25.0f;
}

AHitscanTracer::AHitscanTracer()
{
    PrimaryActorTick.bCanEverTick = false;
    InitialLifeSpan = M5_058_TracerLifeSpanSeconds;

    Beam = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("M5_058_Beam"));
    RootComponent = Beam;
    Beam->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    if (UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")))
    {
        Beam->SetStaticMesh(Cube);
    }
}

void AHitscanTracer::InitializeBeam(const FVector& From, const FVector& To)
{
    FVector Direction = To - From;
    float LengthCm = static_cast<float>(Direction.Size());
    if (LengthCm < M5_058_MinimumBeamLengthCm)
    {
        // Degenerate (a point-blank hit): flash a minimal stub along +X.
        Direction = FVector(M5_058_MinimumBeamLengthCm, 0.0f, 0.0f);
        LengthCm = M5_058_MinimumBeamLengthCm;
    }
    else
    {
        Direction /= LengthCm;
    }
    const FVector Midpoint = From + Direction * (LengthCm * 0.5f);
    SetActorLocationAndRotation(Midpoint, FRotationMatrix::MakeFromX(Direction).Rotator());
    Beam->SetWorldScale3D(FVector(
        LengthCm / M5_058_CubeUnitEdgeCm,
        M5_058_TracerThicknessCm / M5_058_CubeUnitEdgeCm,
        M5_058_TracerThicknessCm / M5_058_CubeUnitEdgeCm));
}
