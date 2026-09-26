#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UObject/SoftObjectPtr.h"

#include "CombatComponent.h"

#include "CombatPresentationComponent.generated.h"

class UAttackDefinition;
class UAnimMontage;
class USkeletalMeshComponent;

/**
 * M1-032: the single owner of attack playback presentation. When the combat
 * lifecycle (UCombatComponent) starts an attack instance, this component
 * plays the matching /Game/UEMMO/Animation/Montages/MNT_<AttackId> montage on
 * the injected mesh's AnimInstance, and stops it again when the instance ends
 * (OnFinished) or is torn down (ResetCombat and every future interrupt-to-free
 * path such as hit stun wiring: the Tick fallback syncs from GetSnapshot, so a
 * Free snapshot always returns the mesh to the locomotion AnimBP).
 *
 * Ownership direction (card rule): presentation only follows the combat state;
 * it never decides damage, never adds notifies, and never keeps its own attack
 * timeline (the combat component's 60 Hz clock stays the only timing source).
 * A missing montage skips playback with one diagnostic per attack id and never
 * crashes, so hit logic (which lives in the combat component) is unaffected by
 * any animation gap.
 */
UCLASS(ClassGroup = (Combat), meta = (BlueprintSpawnableComponent))
class UEMMO_API UCombatPresentationComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCombatPresentationComponent();

	/**
	 * Pure playback-rate mapping (M1-032 step 1): compresses the montage's raw
	 * play length into the attack's logic duration DurationFrames / 60 s, so
	 * the montage finishes exactly when the combat clock ends the attack:
	 * total logic duration = MontageLength / PlayRate = DurationFrames / 60.
	 * Returns 1.0 (once-per-process diagnostic) when DurationFrames <= 0 or
	 * MontageLengthSeconds <= 0 or non-finite, so a broken definition degrades
	 * to real-time playback instead of dividing by zero.
	 */
	static float ComputeMontagePlayRate(const UAttackDefinition& Definition, float MontageLengthSeconds);

	/**
	 * Injects the combat lifecycle source and the mesh that plays the montages
	 * (called from APrototypeCharacter::BeginPlay after the combat catalog is
	 * attached). Idempotent: previous delegate bindings are removed first, and
	 * the presenter forgets what it was showing so the next tick re-syncs from
	 * the source snapshot. Null arguments are tolerated (presentation stays
	 * idle); at least one null mesh is exercised by an automation test.
	 */
	void SetSources(UCombatComponent* InCombatSource, USkeletalMeshComponent* InMeshTarget);

	/**
	 * Snapshot-driven fallback sync (runs every tick; also reachable directly
	 * from tests that drive the component without a world). Reconciles the
	 * mesh playback with CombatSource's current snapshot: a Free snapshot
	 * (natural end, ResetCombat, future hit-stun interrupt) stops the montage
	 * and returns to locomotion; a new running instance id switches playback.
	 * Component ticks run before the actor's Tick, so same-tick starts and
	 * finishes arrive via the Started/Finished delegates while this fallback
	 * catches event-less paths (Reset) on the next tick.
	 */
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/**
	 * Playback seam: starts (or restarts) InMontage for AttackId. The default
	 * implementation stops an already playing instance of the same montage
	 * first (no stacking, card rule) and plays on the mesh's AnimInstance with
	 * InPlayRate. Tests override this to record dispatch calls instead of real
	 * playback.
	 */
	virtual void PlayAttackMontage(FName AttackId, UAnimMontage* Montage, float PlayRate);

	/**
	 * Playback seam: stops InMontage if it is still playing. Tests override
	 * this to record dispatch calls.
	 */
	virtual void StopAttackMontage(FName AttackId, UAnimMontage* Montage);

	/**
	 * Montage lookup seam (naming convention MNT_<AttackId> under
	 * /Game/UEMMO/Animation/Montages, LoadSynchronous behind a small cache).
	 * Returns nullptr for a missing asset; the caller then skips playback with
	 * a once-per-id diagnostic. Tests override this to inject a token montage.
	 */
	virtual UAnimMontage* FindMontageForAttack(FName AttackId);

	/**
	 * Definition lookup seam used only for the play-rate mapping. The default
	 * resolves DA_<AttackId> from /Game/UEMMO/Combat/Definitions/ (the same
	 * asset names the M1-009 generator and DefaultGame.ini catalog use);
	 * nullptr falls back to rate 1.0. Tests override this to inject synthetic
	 * definitions without authoring assets.
	 */
	virtual const UAttackDefinition* FindDefinitionForAttack(FName AttackId);

private:
	/**
	 * Single snapshot-driven sync shared by the Started/Finished delegates and
	 * the Tick fallback. Idempotent: an Attacking snapshot for the instance
	 * already being presented is a no-op, a new instance id switches playback,
	 * and a Free snapshot (natural end, ResetCombat, future hit-stun interrupt)
	 * stops the montage and returns the mesh to the locomotion AnimBP.
	 */
	void ApplySnapshot(const FCombatSnapshot& Snapshot);

	/** Stops and forgets the currently presented instance (if any). */
	void StopPresentedMontage();

	void HandleStarted(FName AttackId, uint64 InstanceId);
	void HandleFinished(FName AttackId, uint64 InstanceId);
	void UnbindDelegates();

	/** Injected sources; weak so neither side keeps the other alive. */
	TWeakObjectPtr<UCombatComponent> CombatSource;
	TWeakObjectPtr<USkeletalMeshComponent> MeshTarget;

	/** AttackId -> resolved montage cache (LoadSynchronous filled, no expiry). */
	UPROPERTY(Transient)
	TMap<FName, TSoftObjectPtr<UAnimMontage>> MontageCache;

	/** Definition cache for FindDefinitionForAttack's DA_<AttackId> lookups. */
	UPROPERTY(Transient)
	TMap<FName, TSoftObjectPtr<UAttackDefinition>> DefinitionCache;

	/** Instance currently presented on the mesh; 0 = idle (locomotion). */
	uint64 PresentingInstanceId = 0;
	FName PresentingAttackId = NAME_None;

	/** Attack ids already diagnosed for a missing montage (one log per id). */
	TSet<FName> LoggedMissingMontageIds;

	/** True while HandleStarted/HandleFinished are bound to the source. */
	bool bDelegatesBound = false;
};
