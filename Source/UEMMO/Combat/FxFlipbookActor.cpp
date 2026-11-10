#include "FxFlipbookActor.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// The same side-view quad orientation the DNFM quads use (the goblin, the
	// player visual, the throwstone, the volcano objects).
	const FQuat SideViewQuadRotation = FQuat(FVector::RightVector, FMath::DegreesToRadians(108.f));
}

AFxFlipbookActor::AFxFlipbookActor()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	Plane = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("FxPlane"));
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
	Plane->SetRelativeRotation(SideViewQuadRotation);
	Plane->SetVisibility(false);
}

AFxFlipbookActor* AFxFlipbookActor::Spawn(UWorld* World, UMaterialInterface* ParentMaterial,
	const FVector& Location, int32 FrameCount, float FrameMs, float ScaleCm)
{
	if (World == nullptr || ParentMaterial == nullptr || FrameCount <= 0 || FrameMs <= 0.f || ScaleCm <= 0.f)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AFxFlipbookActor* Fx = World->SpawnActor<AFxFlipbookActor>(
		AFxFlipbookActor::StaticClass(), Location, FRotator::ZeroRotator, Params);
	if (Fx == nullptr)
	{
		return nullptr;
	}
	Fx->TotalFrames = FrameCount;
	Fx->FrameMs = FrameMs;
	Fx->BornSeconds = World->GetTimeSeconds();

	// The strip cell aspect comes from the parent material's PageTex; a
	// missing texture falls back to a square cell (the quad degrades, it
	// never crashes).
	float CellAspect = 1.f;
	UTexture* PageTex = nullptr;
	if (ParentMaterial->GetTextureParameterValue(FMaterialParameterInfo(TEXT("PageTex")), PageTex)
		&& PageTex != nullptr)
	{
		if (const UTexture2D* PageTex2D = Cast<UTexture2D>(PageTex))
		{
			const float PageHeight = static_cast<float>(PageTex2D->GetSizeY());
			if (PageHeight > 0.f)
			{
				const float PageWidth = static_cast<float>(PageTex2D->GetSizeX());
				const float CellWidth = PageWidth / static_cast<float>(FrameCount);
				CellAspect = CellWidth / PageHeight;
			}
		}
	}
	// The engine plane is 100x100cm; scale Z is the effect height in cm, X
	// follows the cell aspect so a non-square cell shows undistorted.
	Fx->Plane->SetWorldScale3D(FVector(ScaleCm * CellAspect / 100.f, ScaleCm / 100.f, 1.f));

	Fx->FlipbookMID = UMaterialInstanceDynamic::Create(ParentMaterial, Fx);
	if (Fx->FlipbookMID != nullptr)
	{
		Fx->FlipbookMID->SetVectorParameterValue(TEXT("UVRectMin"), FLinearColor(0.f, 0.f, 0.f, 0.f));
		Fx->FlipbookMID->SetVectorParameterValue(TEXT("UVRectSize"),
			FLinearColor(1.f / static_cast<float>(FrameCount), 1.f, 0.f, 0.f));
		Fx->Plane->SetMaterial(0, Fx->FlipbookMID);
	}
	Fx->Plane->SetVisibility(true);
	return Fx;
}

void AFxFlipbookActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	const float ElapsedMs = static_cast<float>((World->GetTimeSeconds() - BornSeconds) * 1000.0);
	const int32 Frame = FMath::FloorToInt32(ElapsedMs / FrameMs);
	if (Frame >= TotalFrames)
	{
		// Play-once: the burst ends when the last frame has shown.
		Destroy();
		return;
	}
	if (FlipbookMID != nullptr)
	{
		FlipbookMID->SetVectorParameterValue(TEXT("UVRectMin"),
			FLinearColor(static_cast<float>(Frame) / static_cast<float>(TotalFrames), 0.f, 0.f, 0.f));
	}
}
