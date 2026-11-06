// M8-013: minimal frame cycler for DNFM map props (bamboo fog). Two placed
// picture quads toggle visibility on the source frame delays; the import
// script fills the references and delays from fog_timing.json. Display-only:
// the actor owns no gameplay state.
#pragma once

#include "GameFramework/Actor.h"
#include "DNFMPropFrameCycler.generated.h"


class AStaticMeshActor;

UCLASS()
class ADNFMPropFrameCycler : public AActor
{
	GENERATED_BODY()

public:
	ADNFMPropFrameCycler();

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;

	/** First frame quad (shown for FrameADelaySeconds). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM|Prop")
	TWeakObjectPtr<AStaticMeshActor> FrameAActor;

	/** Second frame quad (shown for FrameBDelaySeconds). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM|Prop")
	TWeakObjectPtr<AStaticMeshActor> FrameBActor;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM|Prop")
	float FrameADelaySeconds = 50.f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM|Prop")
	float FrameBDelaySeconds = 0.1f;

private:
	void ApplyVisibility();

	float ElapsedSeconds = 0.f;
	bool bShowingFrameA = true;
};
