// M8-036: one play-once flipbook FX quad (the skill cast/hit minimal VFX
// set). The same UVRect contract as the M8-019B throwstone flipbook
// (pageUV = UVRectMin + vec(u, 1-v) * UVRectSize against a horizontal strip
// page), driven by a per-instance MID; the quad destroys itself when the
// last frame has shown.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "FxFlipbookActor.generated.h"

class UMaterialInterface;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;

UCLASS()
class UEMMO_API AFxFlipbookActor : public AActor
{
	GENERATED_BODY()

public:
	AFxFlipbookActor();

	/**
	 * Spawns a play-once flipbook quad; returns null without a world or a
	 * parent material (callers pre-resolve the material through the attack
	 * catalog, so null here is the no-FX shape, never an error). ScaleCm is
	 * the effect's on-screen height in cm; the width follows the strip cell
	 * aspect read from the parent material's PageTex texture.
	 */
	static AFxFlipbookActor* Spawn(UWorld* World, UMaterialInterface* ParentMaterial,
		const FVector& Location, int32 FrameCount, float FrameMs, float ScaleCm);

	virtual void Tick(float DeltaSeconds) override;

private:
	UPROPERTY()
	UStaticMeshComponent* Plane = nullptr;

	UPROPERTY()
	UMaterialInstanceDynamic* FlipbookMID = nullptr;

	int32 TotalFrames = 0;
	float FrameMs = 40.f;
	double BornSeconds = 0.0;
};
