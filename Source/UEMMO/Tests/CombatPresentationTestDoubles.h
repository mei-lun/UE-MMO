#pragma once

#include "CoreMinimal.h"
#include "../Combat/CombatPresentationComponent.h"

#include "CombatPresentationTestDoubles.generated.h"

class UAnimMontage;
class UAttackDefinition;

/**
 * M1-032 test double: records playback dispatch calls instead of really
 * playing, and resolves the synthetic probe attack id ("probe_strike") to
 * injected stand-ins, so the Started/Finished/Reset/instance-switch dispatch
 * semantics of UCombatPresentationComponent are verified without real
 * playback, real montages or real definition assets. The UCLASS form (instead
 * of a plain C++ subclass) is required because NewObject needs StaticClass.
 */
UCLASS()
class URecordingPresentation : public UCombatPresentationComponent
{
	GENERATED_BODY()

public:
	/** Stand-in montage returned for the probe id; never really played. */
	UPROPERTY(Transient)
	TObjectPtr<UAnimMontage> TokenMontage;

	/** Synthetic definition behind the probe id (sets the rate mapping). */
	UPROPERTY(Transient)
	TObjectPtr<UAttackDefinition> ProbeDefinition;

	/** Dispatch records: attack ids (and rates) handed to the play seam. */
	TArray<FName> PlayedIds;
	TArray<float> PlayedRates;

	/** Dispatch records: attack ids handed to the stop seam. */
	TArray<FName> StoppedIds;

	virtual void PlayAttackMontage(FName AttackId, UAnimMontage* Montage, float PlayRate) override;
	virtual void StopAttackMontage(FName AttackId, UAnimMontage* Montage) override;
	virtual UAnimMontage* FindMontageForAttack(FName AttackId) override;
	virtual const UAttackDefinition* FindDefinitionForAttack(FName AttackId) override;
};

/**
 * M5-017 test double: records victim-reaction dispatches instead of really
 * playing. The play/placeholder split is the observable placeholder-vs-real
 * contract; the base class's dispatch history records every dispatch before
 * the seams run.
 */
UCLASS()
class URecordingReactionPresentation : public UCombatPresentationComponent
{
	GENERATED_BODY()

public:
	/** Dispatch records: presentation ids handed to the play seam. */
	TArray<FName> PlayedReactions;

	/** Dispatch records: presentation ids handed to the placeholder seam. */
	TArray<FName> PlaceholderDiagnoses;

	virtual void PlayVictimReactionMontage(FName PresentationId, UAnimMontage* Montage) override;
	virtual void DiagnosePlaceholderReaction(FName PresentationId) override;
};
