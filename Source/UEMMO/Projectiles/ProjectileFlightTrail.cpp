#include "ProjectileFlightTrail.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

AProjectileFlightTrail::AProjectileFlightTrail()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("TrailRoot"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Mesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Material(TEXT("/Game/UEMMO/Materials/M_BulletTrail.M_BulletTrail"));
	for (int32 Index = 0; Index < MaxSegments; ++Index)
	{
		UStaticMeshComponent* Segment = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("Trail_%d"), Index));
		Segment->SetupAttachment(RootComponent);
		Segment->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Segment->SetGenerateOverlapEvents(false);
		Segment->SetCastShadow(false);
		Segment->SetVisibility(false);
		if (Mesh.Succeeded()) { Segment->SetStaticMesh(Mesh.Object); }
		if (Material.Succeeded()) { Segment->SetMaterial(0, Material.Object); }
		Segments.Add(Segment);
	}
}

void AProjectileFlightTrail::BeginPlay()
{
	Super::BeginPlay();
	for (UStaticMeshComponent* Segment : Segments) { Materials.Add(Segment->CreateDynamicMaterialInstance(0)); }
}

void AProjectileFlightTrail::RecordPosition(const FVector& Position)
{
	if (bFinished || Position.ContainsNaN() || !GetWorld()) { return; }
	if (Points.Num() > 0 && Points.Last().Position.Equals(Position, 0.01)) { return; }
	Points.Add({Position, GetWorld()->GetTimeSeconds()});
	// Truncate the oldest edge to the exact length budget, not a line to the target.
	float Remaining = MaxLengthCm;
	for (int32 Index = Points.Num() - 1; Index > 0; --Index)
	{
		const float Length = FVector::Dist(Points[Index].Position, Points[Index - 1].Position);
		if (Length > Remaining)
		{
			const double Fraction = Remaining / Length;
			Points[Index - 1].Position = FMath::Lerp(Points[Index].Position, Points[Index - 1].Position, Fraction);
			Points[Index - 1].Time = FMath::Lerp(Points[Index].Time, Points[Index - 1].Time, Fraction);
			Points.RemoveAt(0, Index - 1);
			break;
		}
		Remaining -= Length;
	}
	if (Points.Num() > MaxSegments + 1) { Points.RemoveAt(0, Points.Num() - MaxSegments - 1); }
	Refresh();
}

void AProjectileFlightTrail::Refresh()
{
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0;
	while (Points.Num() > 1 && Now - Points[1].Time >= FadeSeconds) { Points.RemoveAt(0); }
	VisibleSegments = 0;
	for (int32 Index = 0; Index < Segments.Num(); ++Index)
	{
		UStaticMeshComponent* Segment = Segments[Index];
		if (Index + 1 >= Points.Num()) { Segment->SetVisibility(false); continue; }
		const FVector A = Points[Index].Position, B = Points[Index + 1].Position;
		const float Length = FVector::Dist(A, B);
		const float Alpha = FMath::Clamp(1.0f - static_cast<float>((Now - Points[Index + 1].Time) / FadeSeconds), 0.0f, 1.0f);
		const float Taper = FMath::Lerp(0.35f, 1.0f, static_cast<float>(Index + 1) / FMath::Max(1, Points.Num() - 1));
		const bool bVisible = Length > 0.01f && Alpha > 0.01f;
		Segment->SetVisibility(bVisible);
		if (!bVisible) { continue; }
		Segment->SetWorldLocationAndRotation((A + B) * 0.5, (B - A).Rotation());
		Segment->SetWorldScale3D(FVector(Length / 100.0f, WidthCm * Taper / 100.0f, WidthCm * Taper / 100.0f));
		if (Materials.IsValidIndex(Index) && Materials[Index]) { Materials[Index]->SetScalarParameterValue(TEXT("TrailOpacity"), Alpha * Taper * 0.85f); }
		++VisibleSegments;
	}
}

void AProjectileFlightTrail::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Refresh();
}

void AProjectileFlightTrail::Finish()
{
	if (bFinished) { return; }
	bFinished = true;
	SetLifeSpan(FadeSeconds + 0.02f);
}

float AProjectileFlightTrail::GetRecordedLength() const
{
	float Length = 0;
	for (int32 Index = 1; Index < Points.Num(); ++Index) { Length += FVector::Dist(Points[Index - 1].Position, Points[Index].Position); }
	return Length;
}

FVector AProjectileFlightTrail::GetLastRecordedPosition() const
{
	return Points.Num() ? Points.Last().Position : GetActorLocation();
}
