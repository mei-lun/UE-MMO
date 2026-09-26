#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "UObject/SoftObjectPtr.h"

#include "CombatComponent.h"

#include "CombatPresentationComponent.generated.h"

class UAttackDefinition;
class UAnimInstance;
class UAnimMontage;
class USkeletalMeshComponent;
class USoundWave;

/**
 * M1-034: kind of one combat audio request routed through the dispatcher.
 * Hit requests originate from UCombatComponent::OnHitConfirmed (one accepted
 * damage application); Land requests originate from ATrainingEnemy::Landed.
 */
enum class ECombatAudioEventType : uint8
{
	Hit = 0,
	Land = 1
};

/**
 * M1-034: one presentation audio request. Plain struct (no UHT): the audio
 * path never serializes these values. TargetId is the session-unique actor
 * id (GetUniqueID pattern of the combat hit dedup); when zero the dispatcher
 * derives it from TargetActor. LandingEpoch is the landing round instance
 * (a per-character counter advanced once per landing round), the land-side
 * half of the dedup key.
 */
struct FCombatAudioEvent
{
	ECombatAudioEventType Type = ECombatAudioEventType::Hit;

	/** World-space play location (hit box center / landing feet location). */
	FVector WorldLocation = FVector::ZeroVector;

	/** Actor the event belongs to (hit target / landing character); weak. */
	TWeakObjectPtr<AActor> TargetActor;

	/** Session-unique target id; 0 = derive from TargetActor when valid. */
	uint64 TargetId = 0;

	/** Hit dedup: attacker's stable in-session id (interface contract 5). */
	uint64 InstigatorId = 0;

	/** Hit dedup: in-session instance id of the attack that landed the hit. */
	uint64 AttackInstanceId = 0;

	/** Hit dedup: independent hit group inside the attack instance. */
	int32 HitGroupId = 0;

	/** Land dedup: landing round instance (counter, advanced once per round). */
	uint64 LandingEpoch = 0;
};

/**
 * M1-034: deduplication key of one audio request. Hit requests use the full
 * combat hit dedup tuple (InstigatorId, AttackInstanceId, HitGroupId,
 * TargetId); Land requests use (ActorKey, LandingEpoch). The event type is
 * part of the key so a hit and a land can never collide numerically.
 */
struct FCombatAudioDedupKey
{
	ECombatAudioEventType Type = ECombatAudioEventType::Hit;
	uint64 A = 0;
	uint64 B = 0;
	uint64 C = 0;
	uint64 D = 0;

	bool operator==(const FCombatAudioDedupKey& Other) const
	{
		return Type == Other.Type && A == Other.A && B == Other.B && C == Other.C && D == Other.D;
	}
};

inline uint32 GetTypeHash(const FCombatAudioDedupKey& Key)
{
	uint32 Hash = ::GetTypeHash(static_cast<uint8>(Key.Type));
	Hash = HashCombine(Hash, ::GetTypeHash(Key.A));
	Hash = HashCombine(Hash, ::GetTypeHash(Key.B));
	Hash = HashCombine(Hash, ::GetTypeHash(Key.C));
	Hash = HashCombine(Hash, ::GetTypeHash(Key.D));
	return Hash;
}

/**
 * M1-034: dispatcher tuning (card values). MaxConcurrentRequests bounds the
 * number of unexpired accepted requests inside ConcurrencyWindowSeconds so a
 * crowd of monsters cannot stack unlimited voices; MinRepeatIntervalSeconds
 * is the per-key minimum repeat interval (one hit round sounds once).
 */
struct FCombatAudioDispatchConfig
{
	int32 MaxConcurrentRequests = 8;
	double ConcurrencyWindowSeconds = 0.25;
	double MinRepeatIntervalSeconds = 0.05;
};

/**
 * M1-034: the presentation audio gate. Plain C++ class, no world needed:
 * callers inject the clock explicitly (SetClockSeconds) before Submit, so the
 * dedup/concurrency windows are testable with fixed times. Submit returns
 * true exactly when a play request was recorded (the caller then resolves the
 * sound and plays or diagnoses); false means the request was rejected by the
 * dedup key interval or the concurrency cap and must not sound.
 */
class FCombatAudioDispatcher
{
public:
	void SetConfig(const FCombatAudioDispatchConfig& InConfig);

	/** Injects "now" for the next Submit (explicit clock, interface contract 2 style). */
	void SetClockSeconds(double NowSeconds);

	/** Gates one request: true = accepted and recorded; false = silently dropped. */
	bool Submit(const FCombatAudioEvent& Event);

	/** Total number of accepted requests since construction (history, capped). */
	int32 GetAcceptedCount() const;

	/** Accepted request history (diagnostics/tests, oldest first, capped). */
	const TArray<FCombatAudioEvent>& GetAcceptedEvents() const;

private:
	struct FPendingAudioRequest
	{
		FCombatAudioDedupKey Key;
		double AcceptedAtSeconds = 0.0;
	};

	/** Drops requests whose concurrency window elapsed and trims key tracking. */
	void PruneExpired();

	FCombatAudioDedupKey MakeDedupKey(const FCombatAudioEvent& Event) const;

	FCombatAudioDispatchConfig Config;
	double NowSeconds = 0.0;
	TArray<FPendingAudioRequest> PendingRequests;
	TMap<FCombatAudioDedupKey, double> LastAcceptedByKey;
	TArray<FCombatAudioEvent> AcceptedEvents;
};

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

	/**
	 * M1-034: pins the audio dispatch clock ("now" for the dedup/concurrency
	 * windows). Tests inject fixed times; production never calls this and the
	 * dispatcher follows the world time instead (0.0 without a world).
	 */
	void SetAudioClockSeconds(double NowSeconds);

	/**
	 * M1-034: overrides both sound soft references (config/tests). An empty
	 * reference makes the corresponding event type skip playback with one
	 * diagnostic while the request stays accepted.
	 */
	void SetAudioSounds(TSoftObjectPtr<USoundWave> InHitSound, TSoftObjectPtr<USoundWave> InLandSound);

	/** M1-034: read-only dispatcher observation (accepted request history). */
	const FCombatAudioDispatcher& GetAudioDispatcher() const { return AudioDispatcher; }

	/**
	 * M1-033: shared UE 5.8 pause surface for one anim instance: pauses or
	 * resumes every active montage (Montage_Pause/Montage_Resume with a null
	 * montage reference) plus a single-node animation
	 * (UAnimSingleNodeInstance::SetPlaying, the training enemy's idle).
	 * Null-safe. UAnimInstance::SetPaused does not exist in UE 5.8, so this
	 * static is the one pause implementation for the presenter seam and
	 * UCombatComponent's presenter-less owner fallback alike.
	 */
	static void SetAnimInstancePausedForHitStop(UAnimInstance* AnimInstance, bool bPaused);

	/**
	 * M1-033: every SetHitStopPaused dispatch, oldest first (true = pause,
	 * false = resume). The same handoff-counting observation pattern as the
	 * audio play seam: the record exists even when the mesh is null and
	 * nothing can really pause.
	 */
	const TArray<bool>& GetHitStopPauseDispatchHistory() const { return HitStopPauseDispatchHistory; }

	/**
	 * M1-034: how many accepted requests reached the play seam with a resolved
	 * sound. Without a world the actual audible playback is suppressed (tests,
	 * NullRHI automation) — dispatched counts the play-seam handoffs, never
	 * audibility.
	 */
	int32 GetDispatchedAudioPlayCount() const { return DispatchedAudioPlayCount; }

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

	/**
	 * M1-034: OnHitConfirmed handler (bound in SetSources): one accepted hit
	 * becomes exactly one Hit audio request through the dispatcher gate.
	 */
	void HandleHitConfirmed(const FCombatHit& Hit);

	/**
	 * M1-033: playback seam for the bound combat source's local hit stop: the
	 * default pauses/resumes the mesh's animation through
	 * SetAnimInstancePausedForHitStop and records the dispatch. Tests observe
	 * the recorded history instead of real playback.
	 */
	virtual void SetHitStopPaused(bool bPaused);

	/**
	 * M1-034: sound resolution seam behind the soft references. Returns the
	 * loaded sound or nullptr (missing/empty reference) so a caller can skip
	 * playback with one diagnostic; tests can rely on the null path instead
	 * of real audio assets.
	 */
	virtual USoundWave* ResolveSoundForEvent(const FCombatAudioEvent& Event);

	/**
	 * M1-034: playback seam. The default implementation plays at Location
	 * through UGameplayStatics::PlaySoundAtLocation with the configured
	 * volume multiplier and is suppressed without a world (tests, torn-down
	 * actors): no crash, nothing audible.
	 */
	virtual void PlayCombatSound(USoundWave* Sound, const FVector& Location);

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

	/** M1-033: OnHitStopChanged handler (bound in SetSources). */
	void HandleHitStopChanged(bool bFrozen);

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

	/** M1-034: builds the Hit audio event from one accepted hit (dedup fields). */
	FCombatAudioEvent BuildHitAudioEvent(const FCombatHit& Hit) const;

	/**
	 * M1-034: submits one audio event to the dispatcher; on acceptance resolves
	 * the sound and plays it, or skips with one diagnostic when the reference
	 * is missing. Returns whether the request was accepted.
	 */
	bool SubmitAudioEvent(const FCombatAudioEvent& Event);

	/** M1-034: current dispatch clock (explicit override, else world time). */
	double ResolveAudioNowSeconds() const;

	/** M1-034: pinned audio clock; negative = follow the world time. */
	double ExplicitAudioClockSeconds = -1.0;

	/**
	 * M1-034: hit/landing sound soft references. Defaults pick the Kenney
	 * Impact CC0 set (punch medium for hits, soft heavy for landings); both
	 * are configurable per instance.
	 */
	UPROPERTY(EditAnywhere, Category = "Combat Audio")
	TSoftObjectPtr<USoundWave> HitSound;

	UPROPERTY(EditAnywhere, Category = "Combat Audio")
	TSoftObjectPtr<USoundWave> LandSound;

	/**
	 * M1-034: playback volume. The card's Master/SFX 0.7 target is simplified
	 * to this PlaySoundAtLocation volume multiplier (no SoundClass/mix asset
	 * exists yet); 0.7 keeps the initial value.
	 */
	UPROPERTY(EditAnywhere, Category = "Combat Audio")
	float AudioVolumeMultiplier = 0.7f;

	/** M1-034: dispatch state (dedup/concurrency gate + diagnostics). */
	FCombatAudioDispatcher AudioDispatcher;
	bool bLoggedMissingHitSound = false;
	bool bLoggedMissingLandSound = false;
	int32 DispatchedAudioPlayCount = 0;

	/** M1-033: SetHitStopPaused dispatch history (true = pause, false = resume). */
	TArray<bool> HitStopPauseDispatchHistory;
};
