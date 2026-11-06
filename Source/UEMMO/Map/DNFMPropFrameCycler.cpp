#include "DNFMPropFrameCycler.h"

#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"

ADNFMPropFrameCycler::ADNFMPropFrameCycler()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
}

void ADNFMPropFrameCycler::BeginPlay()
{
	Super::BeginPlay();
	ElapsedSeconds = 0.f;
	bShowingFrameA = true;
	ApplyVisibility();
}

void ADNFMPropFrameCycler::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	ElapsedSeconds += DeltaSeconds;
	const float Threshold = bShowingFrameA ? FrameADelaySeconds : FrameBDelaySeconds;
	if (Threshold <= 0.f)
	{
		return; // degenerate timing: keep the current frame
	}
	if (ElapsedSeconds >= Threshold)
	{
		ElapsedSeconds = FMath::Fmod(ElapsedSeconds, Threshold);
		bShowingFrameA = !bShowingFrameA;
		ApplyVisibility();
	}
}

void ADNFMPropFrameCycler::ApplyVisibility()
{
	if (AStaticMeshActor* FrameA = FrameAActor.Get())
	{
		FrameA->SetActorHiddenInGame(!bShowingFrameA);
	}
	if (AStaticMeshActor* FrameB = FrameBActor.Get())
	{
		FrameB->SetActorHiddenInGame(bShowingFrameA);
	}
}
