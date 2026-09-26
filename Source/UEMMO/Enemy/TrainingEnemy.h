#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"

#include "../Combat/CombatPresentationComponent.h"

#include "TrainingEnemy.generated.h"

class UCombatComponent;
class UHealthComponent;
class USoundWave;

/**
 * M1-022: airborne phase of one combatant, read from its actual vertical
 * velocity (interface contract section 4: Grounded/Rising/Falling stay
 * separate from the Alive/Dead and the action states). Grounded while the
 * movement component walks on ground; Rising while the vertical speed is
 * positive; Falling otherwise (an airborne apex already falls).
 */
enum class ECombatAirState : uint8
{
	Grounded = 0,
	Rising = 1,
	Falling = 2
};

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

	/**
	 * M1-022: launcher launches accepted since the last ground contact (the
	 * first launch from ground contact counts 1, every further launcher hit
	 * before the next ground contact increments). Recording only: the launch
	 * count cap and the Z-factor decay stay with M1-025.
	 */
	int32 GetAirComboCount() const { return AirComboCount; }

	/**
	 * M1-025: launcher launches applied to this enemy in its current float
	 * cycle - the per-target count source of the air-combo policy (interface
	 * contract section 6: at most two launcher launches per cycle, the third
	 * is refused). The attacker's combat component records one launch only
	 * when a launcher hit actually applied the launch impulse; ground contact
	 * (RecordGroundContact), death and ResetEnemy clear the cycle. Deliberately
	 * independent of the M1-022 AirComboCount recording above, which counts
	 * every vertical launch including aerial follow-ups: the policy counts
	 * launchers only.
	 */
	int32 GetLauncherCycleCount() const { return LauncherCycleCount; }

	/** M1-025: records one applied launcher launch into the current float cycle. */
	void RecordLauncherLaunch();

	/**
	 * M1-022: injected clock value of the last recorded ground contact (the
	 * real Landed notify feeds it; 0.0 until the first record).
	 */
	double GetLastGroundedTimeSeconds() const { return LastGroundedTimeSeconds; }

	/** M1-022: airborne phase read from the actual velocity (see the enum). */
	ECombatAirState GetAirState() const;

	/**
	 * M1-022: records one ground contact at the given injected clock value
	 * and closes the running launcher combo. The ACharacter::Landed override
	 * calls this with the world time; tests call it directly with explicit
	 * times (interface contract section 2: early tests use explicit times).
	 */
	void RecordGroundContact(double NowSeconds);

	/**
	 * M1-026: testable/diagnostic landing dispatch entry carrying the whole
	 * landing handling the ACharacter::Landed override performs (the override
	 * resolves the world time and delegates here; tests pass explicit times,
	 * interface contract section 2). One call is one landing event: it
	 * records the ground contact (M1-022), dispatches the landing audio
	 * (M1-034) and - only when this enemy was previously hit airborne - opens
	 * the single Knockdown -> Recovering -> Free recovery process on its
	 * combat component. A plain landing (never launched) never knocks down.
	 */
	void NotifyLanded(double NowSeconds);

	/**
	 * M1-026: reopens the policy float cycle (LauncherCycleCount = 0). This
	 * is the fourth clear point of the air-combo policy: the cycle reopens
	 * only when the landing recovery completed (the component's Recovering ->
	 * Free transition calls this), so the next launcher after the recovery
	 * rises at the full definition speed again. Death and ResetEnemy keep
	 * their own M1-025 clear points.
	 */
	void ClearLauncherCycle() { LauncherCycleCount = 0; }

protected:
	virtual void BeginPlay() override;

	/**
	 * M1-022: launches are recorded for the launcher combo count before the
	 * base implementation defers the velocity application (ACharacter::
	 * LaunchCharacter is virtual in UE 5.8; the combat launch path in
	 * UCombatComponent::ApplyHitImpulse is the only game caller).
	 */
	virtual void LaunchCharacter(FVector LaunchVelocity, bool bXYOverride, bool bZOverride) override;

	/**
	 * M1-034: landing sound source. Advances the landing round epoch once per
	 * round (bounce-style duplicate Landed calls inside the round window reuse
	 * the epoch) and forwards to SubmitLandingAudio; M1-026: the whole landing
	 * handling (ground record, audio, launched-landing recovery) lives in
	 * NotifyLanded, which this override calls with the world time.
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

	/** M1-022: launcher launches accepted since the last ground contact. */
	int32 AirComboCount = 0;

	/** M1-025: launcher launches applied in the current float cycle (policy count source). */
	int32 LauncherCycleCount = 0;

	/** M1-022: true while no launcher launch happened since the last ground contact. */
	bool bGroundedSinceLastLaunch = true;

	/**
	 * M1-026: true while this enemy was hit airborne (a launcher or aerial
	 * launch actually applied a vertical launch velocity) and has not landed
	 * from that float yet. Only a landing with this flag set builds the
	 * Knockdown -> Recovering -> Free recovery process; a plain jump or
	 * gravity landing (flag false) never knocks down. Consumed by the
	 * landing that opens the process, cleared by ResetEnemy.
	 */
	bool bWasLaunchedAirborne = false;

	/** M1-022: clock value of the last recorded ground contact; 0.0 = none yet. */
	double LastGroundedTimeSeconds = 0.0;
};
