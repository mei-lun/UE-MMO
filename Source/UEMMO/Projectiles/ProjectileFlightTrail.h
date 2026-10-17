#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ProjectileFlightTrail.generated.h"

class UStaticMeshComponent;
class UMaterialInstanceDynamic;

/** Presentation only. Records past positions; never predicts or applies hits. */
UCLASS()
class AProjectileFlightTrail : public AActor
{
	GENERATED_BODY()
public:
	AProjectileFlightTrail();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	void RecordPosition(const FVector& Position);
	void Finish();
	int32 GetVisibleSegmentCount() const { return VisibleSegments; }
	float GetRecordedLength() const;
	FVector GetLastRecordedPosition() const;
	static constexpr int32 MaxSegments = 12;
	static constexpr float MaxLengthCm = 180.0f;
	static constexpr float FadeSeconds = 0.14f;
	static constexpr float WidthCm = 4.0f;
private:
	struct FPoint { FVector Position; double Time; };
	TArray<FPoint> Points;
	void Refresh();
	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> Segments;
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;
	bool bFinished = false;
	int32 VisibleSegments = 0;
};
