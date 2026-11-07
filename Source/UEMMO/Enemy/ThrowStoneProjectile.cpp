#include "ThrowStoneProjectile.h"

#include "../Combat/HealthComponent.h"
#include "../PrototypeCharacter.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// The side-view quad rotation shared with the character picture planes
	// (the same fixed rotation the goblin/player planes carry: the roll-90
	// vertical-quad rule composed with the -18deg side-view pitch).
	const FQuat SideViewQuadRotation = FQuat(FVector::RightVector, FMath::DegreesToRadians(108.f));
}

AThrowStoneProjectile::AThrowStoneProjectile()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	Plane = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("StonePlane"));
	Plane->SetupAttachment(RootComponent);
	Plane->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Plane->SetCollisionProfileName(TEXT("NoCollision"));
	Plane->SetCastShadow(false);
	Plane->SetGenerateOverlapEvents(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneMesh(TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (PlaneMesh.Succeeded())
	{
		Plane->SetStaticMesh(PlaneMesh.Object);
	}
	// The engine plane is 100x100cm; the stone cell is 17px * 1.7615 = ~30cm.
	const float QuadSizeCm = CellPx * CmPerPixel;
	Plane->SetWorldScale3D(FVector(QuadSizeCm / 100.f, QuadSizeCm / 100.f, 1.f));
	Plane->SetRelativeRotation(SideViewQuadRotation);
	Plane->SetVisibility(false);
}

AThrowStoneProjectile* AThrowStoneProjectile::Spawn(UWorld* World, const FVector& HandWorldLocation,
	int32 InFacing, float InDamage, AActor* InTarget, UMaterialInterface* StoneMaterial)
{
	if (World == nullptr || InTarget == nullptr)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AThrowStoneProjectile* Stone = World->SpawnActor<AThrowStoneProjectile>(
		AThrowStoneProjectile::StaticClass(), HandWorldLocation, FRotator::ZeroRotator, Params);
	if (Stone == nullptr)
	{
		return nullptr;
	}
	Stone->Facing = InFacing >= 0 ? 1 : -1;
	Stone->Damage = InDamage;
	Stone->HandAnchor = HandWorldLocation;
	Stone->TargetWeak = InTarget;
	Stone->BornSeconds = World->GetTimeSeconds();
	if (StoneMaterial != nullptr)
	{
		Stone->StoneMID = UMaterialInstanceDynamic::Create(StoneMaterial, Stone);
		if (Stone->StoneMID != nullptr)
		{
			// Frame 0 of the 4-cell strip; Tick keeps cycling the rect.
			Stone->StoneMID->SetVectorParameterValue(TEXT("UVRectMin"),
				FLinearColor(0.f, 0.f, 0.f, 0.f));
			Stone->StoneMID->SetVectorParameterValue(TEXT("UVRectSize"),
				FLinearColor(CellPx / PageWidthPx, 1.f, 0.f, 0.f));
			Stone->Plane->SetMaterial(0, Stone->StoneMID);
		}
	}
	Stone->Plane->SetVisibility(true);
	Stone->bInFlight = true;
	return Stone;
}

void AThrowStoneProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// The landing linger: freeze at the impact point, then remove (the
	// card's landing destroy + impact-point feedback).
	if (bLanded)
	{
		if (World->GetTimeSeconds() - LandAtSeconds >= LandLingerSeconds)
		{
			Destroy();
		}
		return;
	}
	if (!bInFlight)
	{
		return;
	}
	const double Now = World->GetTimeSeconds();
	if (Now - BornSeconds >= MaxFlightSeconds)
	{
		Destroy();
		return;
	}

	// Straight flight along the locked facing; Y is fixed at the release
	// depth (the thrower was depth-aligned before entering the wind-up).
	FVector Location = GetActorLocation();
	Location.X += Facing * FlightSpeedCmPerSecond * DeltaSeconds;
	SetActorLocation(Location, false);
	TravelledCm += FlightSpeedCmPerSecond * DeltaSeconds;

	// The 4f x 30ms looping flipbook drives the UVRect on the shared
	// fighter-lineage material contract.
	ElapsedMs += DeltaSeconds * 1000.f;
	const int32 Frame = FMath::FloorToInt32(FMath::Fmod(ElapsedMs / FrameMs, static_cast<float>(FrameCount)));
	if (StoneMID != nullptr)
	{
		StoneMID->SetVectorParameterValue(TEXT("UVRectMin"),
			FLinearColor(Frame * CellPx / PageWidthPx, 0.f, 0.f, 0.f));
	}

	// The whole-flight 20cm cube hit box against the target's actor bounds
	// (the M8-007 source rule: one attack cube per frame, active during the
	// entire looped flight).
	AActor* Target = TargetWeak.Get();
	if (Target != nullptr)
	{
		const FBox StoneBox(Location - FVector(HitBoxHalfCm), Location + FVector(HitBoxHalfCm));
		if (StoneBox.Intersect(Target->GetComponentsBoundingBox()))
		{
			// The unified health chain: the same single removal entry the
			// melee pipeline's UnifiedHitApplier bottoms out at.
			if (APrototypeCharacter* PlayerTarget = Cast<APrototypeCharacter>(Target))
			{
				UHealthComponent* VictimHealth = PlayerTarget->GetHealth();
				if (VictimHealth != nullptr && VictimHealth->IsAlive())
				{
					VictimHealth->ApplyDamage(Damage);
				}
			}
			Destroy();
			return;
		}
	}

	// Landing: the card's straight flight carries no decoded source gravity,
	// so the stone's ground contact is the range end - the AI only fires
	// within the throw range and the stone lands at that distance (the
	// impact point lingers briefly before removal).
	if (TravelledCm >= MaxFlightDistanceCm)
	{
		bLanded = true;
		bInFlight = false;
		LandAtSeconds = Now;
	}
}
