// M8-019A: the bamboo-thrower goblin. A melee-enemy skeleton (the shared
// Health/Combat chain, never a second health path) that displays through the
// DNFM picture pipeline with the monster visual set - the same interface the
// player fighter uses, no fighter-specific state table.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "MeleeEnemy.h"
#include "../Character/DNFMCharacterVisualSet.h"

#include "GoblinThrowerEnemy.generated.h"

class UDNFMCharacterVisualComponent;

/**
 * Display actions resolved from the combat/movement state each frame; the
 * component is only re-entered when the resolved action id changes.
 */
UCLASS()
class UEMMO_API AGoblinThrowerEnemy : public AMeleeEnemy
{
	GENERATED_BODY()

public:
	AGoblinThrowerEnemy();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	/** The picture component (test/inspection access). */
	UFUNCTION(BlueprintPure, Category = "DNFM")
	UDNFMCharacterVisualComponent* GetGoblinVisual() const { return GoblinVisual; }

protected:
	/** Monster atlas set (M8-018): 8 actions, 21 drawn frames. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM")
	TSoftObjectPtr<UDNFMCharacterVisualSet> MonsterVisualSet;

	/** Source scale carried into the picture (M8-010/M8-018: 1.7615 cm/px). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "DNFM")
	float CmPerPixel = 1.7615f;

protected:
	/** The picture display (M8-018 monster atlas; M6-021 component). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "DNFM")
	TObjectPtr<UDNFMCharacterVisualComponent> GoblinVisual;

private:
	void ResolveDisplayAction();

	bool bVisualActivated = false;
	FName CurrentDisplayAction = FName(NAME_None);
	int32 HitStunAlternator = 0;
};
