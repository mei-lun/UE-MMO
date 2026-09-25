#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "TrainingEnemy.generated.h"

class UHealthComponent;

/**
 * M1-016: a passive, damageable training enemy for the training arena.
 * The only damage entry point is UHealthComponent::ApplyDamage (interface
 * contract 7: reuse Health/Combat, never a second direct health-removal
 * path). This card intentionally ships no AIController behavior: the enemy
 * stands at its spawn anchor until it is hit. ResetEnemy restores health,
 * transform and velocity so a room can be replayed.
 */
UCLASS()
class UEMMO_API ATrainingEnemy : public ACharacter
{
	GENERATED_BODY()

public:
	ATrainingEnemy();

	/** Full health, spawn anchor transform, zero velocity. */
	void ResetEnemy();

	/**
	 * Explicit spawn anchor used by ResetEnemy. BeginPlay captures the placed
	 * transform when this was never called, so map-placed enemies reset to
	 * where the level put them while tests can pin an anchor without a world.
	 */
	void SetSpawnAnchor(const FVector& Location, const FRotator& Rotation);

	/** Health pool of this enemy; damage must go through ApplyDamage. */
	UFUNCTION(BlueprintPure, Category = "Combat")
	UHealthComponent* GetHealthComponent() const { return Health; }

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Combat")
	TObjectPtr<UHealthComponent> Health;

	UPROPERTY(EditInstanceOnly, Category = "Combat")
	FVector SpawnAnchorLocation = FVector::ZeroVector;

	UPROPERTY(EditInstanceOnly, Category = "Combat")
	FRotator SpawnAnchorRotation = FRotator::ZeroRotator;

	/** True once the anchor was captured in BeginPlay or set explicitly. */
	bool bSpawnAnchorCaptured = false;
};
