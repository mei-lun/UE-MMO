#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"

#include "../Combat/CombatPresentationComponent.h"

#include "TrainingEnemy.generated.h"

class UCombatComponent;
class UHealthComponent;
class USoundWave;

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

	/**
	 * M1-020: combat state of this enemy (hit stun and death priority). The
	 * attacker's damage application notifies this component about accepted
	 * hits; the enemy itself has no AI use for it yet.
	 */
	UFUNCTION(BlueprintPure, Category = "Combat")
	UCombatComponent* GetCombatComponent() const { return Combat; }

	/**
	 * M1-034: testable/diagnostic landing dispatch entry: submits one landing
	 * round (epoch = the landing round instance, NowSeconds = the injected
	 * clock) to the enemy's audio dispatcher and, on acceptance, resolves the
	 * landing sound and plays it. Returns false when the dispatcher rejected
	 * the request (same round inside the dedup window or concurrency cap), so
	 * one landing round can only produce one accepted request. The real
	 * ACharacter::Landed override calls this with the world time.
	 */
	bool SubmitLandingAudio(uint64 InLandingEpoch, double NowSeconds);

	/** M1-034: accepted landing audio requests so far (tests/diagnostics). */
	int32 GetAcceptedLandingAudioCount() const { return LandAudioDispatcher.GetAcceptedCount(); }

	/** M1-034: accepted landing requests that reached the play seam with a resolved sound. */
	int32 GetDispatchedLandingAudioCount() const { return DispatchedLandingAudioCount; }

	/** M1-034: overrides the landing sound soft reference (tests/config). */
	void SetLandSound(TSoftObjectPtr<USoundWave> InLandSound) { LandSound = InLandSound; }

protected:
	virtual void BeginPlay() override;

	/**
	 * M1-034: landing sound source. Advances the landing round epoch once per
	 * round (bounce-style duplicate Landed calls inside the round window reuse
	 * the epoch) and forwards to SubmitLandingAudio; floating-then-landing
	 * victims arrive through later tasks, this card only wires the source.
	 */
	virtual void Landed(const FHitResult& Hit) override;

	/**
	 * M1-034: playback seam. The default implementation plays the landing
	 * sound at the feet location with the configured volume multiplier and is
	 * suppressed without a world (tests): no crash, nothing audible.
	 */
	virtual void PlayLandSound(USoundWave* Sound, const FVector& Location);

private:
	UPROPERTY(VisibleAnywhere, Category = "Combat")
	TObjectPtr<UHealthComponent> Health;

	/** M1-020: hit stun / death priority state for this enemy. */
	UPROPERTY(VisibleAnywhere, Category = "Combat")
	TObjectPtr<UCombatComponent> Combat;

	UPROPERTY(EditInstanceOnly, Category = "Combat")
	FVector SpawnAnchorLocation = FVector::ZeroVector;

	UPROPERTY(EditInstanceOnly, Category = "Combat")
	FRotator SpawnAnchorRotation = FRotator::ZeroRotator;

	/** True once the anchor was captured in BeginPlay or set explicitly. */
	bool bSpawnAnchorCaptured = false;

	/**
	 * M1-034: landing sound soft reference (defaults to the Kenney Impact
	 * CC0 soft heavy impact) and volume. The card's Master/SFX 0.7 target is
	 * simplified to this PlaySoundAtLocation volume multiplier (no SoundClass
	 * asset exists yet); 0.7 keeps the initial value.
	 */
	UPROPERTY(EditAnywhere, Category = "Combat Audio")
	TSoftObjectPtr<USoundWave> LandSound;

	UPROPERTY(EditAnywhere, Category = "Combat Audio")
	float LandingAudioVolumeMultiplier = 0.7f;

	/** M1-034: Landed calls inside this window reuse the landing round epoch. */
	double LandRoundWindowSeconds = 0.15;

	/** M1-034: landing round instance; advanced once per new landing round. */
	uint64 LandingEpoch = 0;

	/** M1-034: clock value of the previous Landed notify; < 0 = none yet. */
	double LastLandedNotifySeconds = -1.0;

	/** M1-034: per-character landing audio gate (dedup key + concurrency). */
	FCombatAudioDispatcher LandAudioDispatcher;

	bool bLoggedMissingLandSound = false;
	int32 DispatchedLandingAudioCount = 0;
};
