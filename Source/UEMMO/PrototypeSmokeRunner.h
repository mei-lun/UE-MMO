#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "PrototypeSmokeRunner.generated.h"
class ACharacter;
/** Spawned only by the explicit -UEMMOSmoke command-line flag. */
UCLASS()
class UEMMO_API APrototypeSmokeRunner : public AActor
{
    GENERATED_BODY()
public:
    APrototypeSmokeRunner();
    virtual void Tick(float DeltaSeconds) override;
private:
    float Elapsed = 0;
    float MaximumZ = -BIG_NUMBER;
    bool bStarted = false;
    bool bJumped = false;
    bool bCaptured = false;
    bool bFinished = false;
    FVector Origin = FVector::ZeroVector;
};
