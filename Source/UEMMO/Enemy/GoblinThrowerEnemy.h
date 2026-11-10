// M8-019A: the bamboo-thrower goblin. A melee-enemy skeleton (the shared
// Health/Combat chain, never a second health path) that displays through the
// DNFM picture pipeline with the monster visual set - the same interface the
// player fighter uses, no fighter-specific state table.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "MeleeEnemy.h"
#include "../Character/DNFMCharacterVisualSet.h"
#include "../Combat/CombatStateMachine.h"

#include "GoblinThrowerEnemy.generated.h"

class UDNFMCharacterVisualComponent;
class UMaterialInterface;
class USoundWave;

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

	/** M8-019A: re-applies the material+set+action sequence post-BeginPlay. */
	void RefreshGoblinMaterial()
	{
		if (RefreshVisual) RefreshVisual();
	}

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

public:
	/**
	 * M8-019B: the throw replaces the melee instance - the stone flies from
	 * the hand anchor and carries the damage through the unified health
	 * chain. The controller's bool contract is kept (true = started).
	 */
	virtual bool LaunchAttack(FName AttackId, int32 Facing) override;

	/**
	 * M8-019B: the telegraph presentation is the attack_0 clip itself (the
	 * source's startup frames are the warning) - not the 3D mesh swell.
	 */
	virtual void ApplyTelegraphVisual(bool bActive) override;

private:
	TFunction<void()> RefreshVisual;

	/**
	 * M8-038: the phase events drive the action switch (the plan 08 problem
	 * 1: the display subscribes to the state machine instead of re-deriving
	 * the phase from the snapshot each tick). The per-tick resolver keeps
	 * only the hold gate, the death-hold pin and the Idle velocity switch.
	 */
	void HandlePhaseChanged(ECombatPhase OldPhase, ECombatPhase NewPhase);

	/** M8-038: the cached-phase → display-action mapping (shared by the event
	 * handler and the per-tick idempotent re-apply; NAME_None = the phase is
	 * tick-resolver territory). */
	FName PhaseDesiredAction(bool& bOutHoldLastFrame) const;

	/** M8-038: applies one display action (idempotent); returns true on change. */
	bool ApplyDisplayAction(FName Desired, bool bHoldLastFrame = false);

	void ResolveDisplayAction();

	/** M8-038: the last phase the machine reported (the tick resolver reads
	 * this instead of re-deriving from the combat snapshot). */
	ECombatPhase CachedPhase = ECombatPhase::Idle;

	/** The hand anchor for the release: the foot + these offsets (design
	 * defaults - no monster bone data exists; the throw hand sits forward of
	 * the body at roughly two-thirds of the 155cm standing height). */
	static constexpr float HandForwardCm = 30.f;
	static constexpr float HandUpCm = 100.f;
	/** attack_0 runs 400ms; the ~300ms telegraph consumed most of it. */
	static constexpr float AttackClipRemainderSeconds = 0.1f;
	/** Fallback damage when no enemy definition was applied (enemies.json: 6). */
	static constexpr float DefaultThrowDamage = 6.f;

	bool bVisualActivated = false;
	FName CurrentDisplayAction = FName(NAME_None);
	int32 HitStunAlternator = 0;
	/** While now < this, ResolveDisplayAction keeps the attack_0 release pose. */
	double AttackClipHoldUntilSeconds = 0.0;
	/** The throwstone material loaded once (duplicate-fighter lineage). */
	TWeakObjectPtr<UMaterialInterface> StoneMaterialWeak;
};
