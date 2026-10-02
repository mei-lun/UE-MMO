#include "CombatPresentationComponent.h"

#include "AttackDefinition.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Components/SkeletalMeshComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundWave.h"

namespace
{
	// Naming convention mandated by M1-032: MNT_<AttackId> under the montage
	// folder and DA_<AttackId> under the definition folder (the same asset
	// names the M1-009 generator produces and DefaultGame.ini pins for the
	// runtime catalog). Object path = <folder><AttackId>.<AttackId>.
	const TCHAR* const MontageFolder = TEXT("/Game/UEMMO/Animation/Montages/MNT_");
	const TCHAR* const DefinitionFolder = TEXT("/Game/UEMMO/Combat/Definitions/DA_");

	// Attack logic runs on the fixed 60 Hz combat clock (interface contract
	// section 3: DurationFrames are frames at logic_fps 60), so the play-rate
	// mapping compresses the montage into DurationFrames / 60 seconds.
	constexpr float LogicFramesPerSecond = 60.0f;

	// M1-034: the two manually picked Kenney Impact CC0 SoundWaves (loaded by
	// the M0 foundation pass, see Docs/05 section 3): punch medium for hits,
	// soft heavy for landings. Object path = <package>.<object>.
	const TCHAR* const DefaultHitSoundPath = TEXT("/Game/ThirdParty/Kenney/Impact/impactPunch_medium_000.impactPunch_medium_000");
	const TCHAR* const DefaultLandSoundPath = TEXT("/Game/ThirdParty/Kenney/Impact/impactSoft_heavy_000.impactSoft_heavy_000");

	// M1-034: dispatcher bookkeeping caps so a long session cannot grow the
	// diagnostics state without bound.
	constexpr int32 MaxTrackedAudioKeys = 256;
	constexpr int32 MaxAudioHistoryEntries = 64;
}

void FCombatAudioDispatcher::SetConfig(const FCombatAudioDispatchConfig& InConfig)
{
	Config = InConfig;
}

void FCombatAudioDispatcher::SetClockSeconds(double NowSecondsValue)
{
	NowSeconds = NowSecondsValue;
}

int32 FCombatAudioDispatcher::GetAcceptedCount() const
{
	return AcceptedEvents.Num();
}

const TArray<FCombatAudioEvent>& FCombatAudioDispatcher::GetAcceptedEvents() const
{
	return AcceptedEvents;
}

FCombatAudioDedupKey FCombatAudioDispatcher::MakeDedupKey(const FCombatAudioEvent& Event) const
{
	FCombatAudioDedupKey Key;
	Key.Type = Event.Type;
	const uint64 ActorKey = Event.TargetId != 0
		? Event.TargetId
		: (Event.TargetActor.IsValid() ? static_cast<uint64>(Event.TargetActor->GetUniqueID()) : 0ull);
	if (Event.Type == ECombatAudioEventType::Hit)
	{
		Key.A = Event.InstigatorId;
		Key.B = Event.AttackInstanceId;
		Key.C = static_cast<uint64>(Event.HitGroupId);
		Key.D = ActorKey;
	}
	else
	{
		Key.A = 0;
		Key.B = ActorKey;
		Key.C = Event.LandingEpoch;
		Key.D = 0;
	}
	return Key;
}

void FCombatAudioDispatcher::PruneExpired()
{
	// Requests expire (inclusive) when their concurrency window elapsed; the
	// expiry check runs before every Submit so the cap only counts voices that
	// could still be sounding.
	for (int32 Index = PendingRequests.Num() - 1; Index >= 0; --Index)
	{
		if (PendingRequests[Index].AcceptedAtSeconds + Config.ConcurrencyWindowSeconds <= NowSeconds)
		{
			PendingRequests.RemoveAtSwap(Index);
		}
	}
	// Hit keys are unique per instance, so the key map only ever grows; trim
	// stale entries once it exceeds the cap to keep long sessions bounded.
	if (LastAcceptedByKey.Num() > MaxTrackedAudioKeys)
	{
		for (TMap<FCombatAudioDedupKey, double>::TIterator It = LastAcceptedByKey.CreateIterator(); It; ++It)
		{
			if (NowSeconds - It.Value() >= Config.MinRepeatIntervalSeconds)
			{
				It.RemoveCurrent();
			}
		}
	}
}

bool FCombatAudioDispatcher::Submit(const FCombatAudioEvent& Event)
{
	// Gate order: expire first (the cap only counts still-unexpired voices),
	// then the per-key minimum repeat interval, then the concurrency cap.
	PruneExpired();

	const FCombatAudioDedupKey Key = MakeDedupKey(Event);
	if (const double* LastAccepted = LastAcceptedByKey.Find(Key))
	{
		// Same key inside the interval (strictly less than): rejected, so one
		// hit/landing round can only produce one accepted request. Exactly at
		// the interval boundary the request is accepted again.
		if (NowSeconds - *LastAccepted < Config.MinRepeatIntervalSeconds)
		{
			return false;
		}
	}
	if (PendingRequests.Num() >= Config.MaxConcurrentRequests)
	{
		// Voice budget exhausted inside the concurrency window: rejected.
		return false;
	}

	LastAcceptedByKey.Add(Key, NowSeconds);
	PendingRequests.Add({Key, NowSeconds});
	AcceptedEvents.Add(Event);
	if (AcceptedEvents.Num() > MaxAudioHistoryEntries)
	{
		// Diagnostics history only; drop the oldest entry to stay bounded.
		AcceptedEvents.RemoveAt(0);
	}
	return true;
}

UCombatPresentationComponent::UCombatPresentationComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;

	// M1-034: default Kenney Impact CC0 sounds (manually picked, see the
	// anonymous-namespace constants above); both stay configurable per
	// instance so later tasks can re-route them without code changes.
	HitSound = TSoftObjectPtr<USoundWave>(FSoftObjectPath(DefaultHitSoundPath));
	LandSound = TSoftObjectPtr<USoundWave>(FSoftObjectPath(DefaultLandSoundPath));
}

float UCombatPresentationComponent::ComputeMontagePlayRate(const UAttackDefinition& Definition, float MontageLengthSeconds)
{
	const int32 DurationFrames = Definition.DurationFrames;
	if (DurationFrames <= 0 || !FMath::IsFinite(MontageLengthSeconds) || MontageLengthSeconds <= 0.0f)
	{
		// Once-per-process diagnostic: a broken definition or a zero-length
		// montage degrades to real-time playback instead of dividing by zero.
		static bool bDiagnosedOnce = false;
		if (!bDiagnosedOnce)
		{
			bDiagnosedOnce = true;
			UE_LOG(LogTemp, Warning,
				TEXT("UEMMO UCombatPresentationComponent: ComputeMontagePlayRate fell back to rate 1.0 (DurationFrames=%d, MontageLength=%f s); attack definition and montage length disagree."),
				DurationFrames, MontageLengthSeconds);
		}
		return 1.0f;
	}
	return MontageLengthSeconds / (static_cast<float>(DurationFrames) / LogicFramesPerSecond);
}

void UCombatPresentationComponent::SetSources(UCombatComponent* InCombatSource, USkeletalMeshComponent* InMeshTarget)
{
	UnbindDelegates();
	CombatSource = InCombatSource;
	MeshTarget = InMeshTarget;
	// Forget what the previous source was presenting so the next sync re-reads
	// the (new) source snapshot from scratch.
	PresentingInstanceId = 0;
	PresentingAttackId = NAME_None;
	if (CombatSource.IsValid())
	{
		// Ownership wiring (card): Started/Finished drive playback immediately;
		// the Tick fallback covers event-less teardowns (ResetCombat) and any
		// future interrupt-to-free path (hit stun / knockdown wiring). M1-034
		// adds the accepted-hit audio dispatch on the same source events.
		// M1-033 adds the local hit stop pause dispatch.
		CombatSource->OnStarted.AddUObject(this, &UCombatPresentationComponent::HandleStarted);
		CombatSource->OnFinished.AddUObject(this, &UCombatPresentationComponent::HandleFinished);
		CombatSource->OnHitConfirmed.AddUObject(this, &UCombatPresentationComponent::HandleHitConfirmed);
		CombatSource->OnHitStopChanged.AddUObject(this, &UCombatPresentationComponent::HandleHitStopChanged);
		bDelegatesBound = true;
	}
}

void UCombatPresentationComponent::BeginPlay()
{
	Super::BeginPlay();
	// First sync. In the normal character wiring SetSources runs later (the
	// owner's BeginPlay), so this usually observes no source yet and is a
	// no-op; it only matters for components whose sources are set earlier.
	if (CombatSource.IsValid())
	{
		ApplySnapshot(CombatSource->GetSnapshot());
	}
}

void UCombatPresentationComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnbindDelegates();
	CombatSource = nullptr;
	MeshTarget = nullptr;
	Super::EndPlay(EndPlayReason);
}

void UCombatPresentationComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	// Snapshot-driven fallback sync. Component ticks run before the owning
	// actor's Tick (where TickCombat advances the clock), so same-tick starts
	// and finishes arrive through the Started/Finished delegates and this
	// fallback catches event-less state changes (ResetCombat; future hit-stun
	// interrupts) one component tick later at worst.
	if (CombatSource.IsValid())
	{
		ApplySnapshot(CombatSource->GetSnapshot());
	}
	// The sync itself is registration-independent (tests drive this component
	// directly without a world); Super's tick bookkeeping only applies to a
	// registered component (it checks bRegistered).
	if (IsRegistered())
	{
		Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	}
}

void UCombatPresentationComponent::PlayAttackMontage(FName AttackId, UAnimMontage* Montage, float PlayRate)
{
	USkeletalMeshComponent* Mesh = MeshTarget.Get();
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
	if (Montage == nullptr || AnimInstance == nullptr)
	{
		return;
	}
	// No stacking (card rule): an already playing instance of the same montage
	// is stopped first, so a retrigger cleanly restarts the action.
	if (AnimInstance->Montage_IsPlaying(Montage))
	{
		AnimInstance->Montage_Stop(Montage->GetDefaultBlendOutTime(), Montage);
	}
	AnimInstance->Montage_Play(Montage, PlayRate);
}

void UCombatPresentationComponent::StopAttackMontage(FName AttackId, UAnimMontage* Montage)
{
	USkeletalMeshComponent* Mesh = MeshTarget.Get();
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
	if (Montage == nullptr || AnimInstance == nullptr)
	{
		return;
	}
	if (AnimInstance->Montage_IsPlaying(Montage))
	{
		AnimInstance->Montage_Stop(Montage->GetDefaultBlendOutTime(), Montage);
	}
}

UAnimMontage* UCombatPresentationComponent::FindMontageForAttack(FName AttackId)
{
	if (AttackId.IsNone())
	{
		return nullptr;
	}
	if (const TSoftObjectPtr<UAnimMontage>* Cached = MontageCache.Find(AttackId))
	{
		return Cached->LoadSynchronous();
	}
	// M3-024: the montage package AND its inner object both carry the MNT_
	// prefix (the M1-032 factory output; the MNT_<AttackId>.MNT_<AttackId>
	// object paths are pinned in CombatPresentationTests). The pre-fix path
	// built <MNT_<AttackId> package>.<AttackId object>, which never resolved:
	// the M3-024 red in-loop test logged "montage '...MNT_light_01.light_01'
	// not found" on every attack, so attacks ran without animation playback.
	const FString MontageObjectName = FString(TEXT("MNT_")) + AttackId.ToString();
	const FString ObjectPath = FString(MontageFolder) + AttackId.ToString() + TEXT(".") + MontageObjectName;
	const TSoftObjectPtr<UAnimMontage> Reference{FSoftObjectPath(ObjectPath)};
	UAnimMontage* Montage = Reference.LoadSynchronous();
	if (Montage == nullptr)
	{
		// Missing montage: skip playback gracefully (no crash), one diagnostic
		// per attack id. Hit logic stays with the combat component and is not
		// affected by any animation gap (card: Notify/animation is presentation
		// only and never decides damage).
		bool bAlreadyLogged = false;
		LoggedMissingMontageIds.Add(AttackId, &bAlreadyLogged);
		if (!bAlreadyLogged)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("UEMMO UCombatPresentationComponent: montage '%s' not found for attack '%s'; attack plays without animation playback."),
				*ObjectPath, *AttackId.ToString());
		}
	}
	MontageCache.Add(AttackId, Reference);
	return Montage;
}

const UAttackDefinition* UCombatPresentationComponent::FindDefinitionForAttack(FName AttackId)
{
	if (AttackId.IsNone())
	{
		return nullptr;
	}
	if (const TSoftObjectPtr<UAttackDefinition>* Cached = DefinitionCache.Find(AttackId))
	{
		return Cached->Get();
	}
	const FString ObjectPath = FString(DefinitionFolder) + AttackId.ToString() + TEXT(".") + AttackId.ToString();
	const TSoftObjectPtr<UAttackDefinition> Reference{FSoftObjectPath(ObjectPath)};
	UAttackDefinition* Definition = Reference.LoadSynchronous();
	DefinitionCache.Add(AttackId, Reference);
	return Definition;
}

void UCombatPresentationComponent::ApplySnapshot(const FCombatSnapshot& Snapshot)
{
	if (Snapshot.ActionState != ECombatActionState::Attacking || Snapshot.InstanceId == 0)
	{
		// Free (natural end already handled by Finished, ResetCombat teardown,
		// and every future interrupt-to-free path): no attack montage belongs
		// on the mesh; the locomotion AnimBP regains control.
		StopPresentedMontage();
		return;
	}
	if (Snapshot.InstanceId == PresentingInstanceId)
	{
		// Already presenting exactly this instance; the sync is idempotent and
		// never restarts playback from a steady-state snapshot.
		return;
	}
	// A changed instance id switches playback. This is the start path when it
	// arrives through the Started delegate and the recovery path when the
	// tick fallback observes a new instance without the delegate (combo chain
	// switches broadcast Finished+Started, so both paths converge here).
	UAnimMontage* Montage = FindMontageForAttack(Snapshot.AttackId);
	// Advance the presented state before playback: a missing montage (null,
	// already diagnosed once) must not retry on every tick, and the natural
	// finish/teardown path must be able to stop whatever was presented.
	PresentingInstanceId = Snapshot.InstanceId;
	PresentingAttackId = Snapshot.AttackId;
	if (Montage == nullptr)
	{
		return;
	}
	float PlayRate = 1.0f;
	if (const UAttackDefinition* Definition = FindDefinitionForAttack(Snapshot.AttackId))
	{
		PlayRate = ComputeMontagePlayRate(*Definition, Montage->GetPlayLength());
	}
	PlayAttackMontage(Snapshot.AttackId, Montage, PlayRate);
}

void UCombatPresentationComponent::StopPresentedMontage()
{
	if (PresentingInstanceId == 0 && PresentingAttackId.IsNone())
	{
		// Idle steady state: nothing was presented, dispatch nothing.
		return;
	}
	UAnimMontage* Montage = FindMontageForAttack(PresentingAttackId);
	StopAttackMontage(PresentingAttackId, Montage);
	PresentingInstanceId = 0;
	PresentingAttackId = NAME_None;
}

void UCombatPresentationComponent::HandleStarted(FName AttackId, uint64 InstanceId)
{
	// The Started broadcast happens while the snapshot is already Attacking;
	// re-reading it keeps one authoritative code path for starts and switches.
	if (CombatSource.IsValid())
	{
		ApplySnapshot(CombatSource->GetSnapshot());
	}
}

void UCombatPresentationComponent::HandleFinished(FName AttackId, uint64 InstanceId)
{
	// Finished fires after the combat state is already Free (the component
	// clears first, then broadcasts), so the stop is driven by the presented
	// instance record rather than a snapshot re-read.
	StopPresentedMontage();
}

void UCombatPresentationComponent::SetAudioClockSeconds(double NowSeconds)
{
	// Negative restores the world-time default (production never pins it).
	ExplicitAudioClockSeconds = NowSeconds;
}

void UCombatPresentationComponent::SetAudioSounds(TSoftObjectPtr<USoundWave> InHitSound, TSoftObjectPtr<USoundWave> InLandSound)
{
	HitSound = InHitSound;
	LandSound = InLandSound;
}

double UCombatPresentationComponent::ResolveAudioNowSeconds() const
{
	if (ExplicitAudioClockSeconds >= 0.0)
	{
		return ExplicitAudioClockSeconds;
	}
	const UWorld* World = GetWorld();
	return World ? World->GetTimeSeconds() : 0.0;
}

FCombatAudioEvent UCombatPresentationComponent::BuildHitAudioEvent(const FCombatHit& Hit) const
{
	FCombatAudioEvent Event;
	Event.Type = ECombatAudioEventType::Hit;
	Event.WorldLocation = Hit.WorldHitLocation;
	Event.TargetActor = Hit.Target;
	// Same session-unique target id derivation as the combat hit dedup key
	// (CombatComponent uses Target->GetUniqueID()); a stale target yields 0.
	Event.TargetId = Hit.Target.IsValid() ? static_cast<uint64>(Hit.Target->GetUniqueID()) : 0ull;
	Event.InstigatorId = Hit.InstigatorId;
	Event.AttackInstanceId = Hit.AttackInstanceId;
	Event.HitGroupId = Hit.HitGroupId;
	return Event;
}

USoundWave* UCombatPresentationComponent::ResolveSoundForEvent(const FCombatAudioEvent& Event)
{
	const TSoftObjectPtr<USoundWave>& Reference =
		(Event.Type == ECombatAudioEventType::Hit) ? HitSound : LandSound;
	return Reference.LoadSynchronous();
}

void UCombatPresentationComponent::PlayCombatSound(USoundWave* Sound, const FVector& Location)
{
	if (Sound == nullptr)
	{
		return;
	}
	++DispatchedAudioPlayCount;
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		// Tests and torn-down actors: the play request is dispatched (counted)
		// but nothing can sound; never a crash, never an audibility claim.
		return;
	}
	UGameplayStatics::PlaySoundAtLocation(this, Sound, Location, AudioVolumeMultiplier);
}

bool UCombatPresentationComponent::SubmitAudioEvent(const FCombatAudioEvent& Event)
{
	AudioDispatcher.SetClockSeconds(ResolveAudioNowSeconds());
	if (!AudioDispatcher.Submit(Event))
	{
		// Rejected by the dedup interval or the concurrency cap: stays silent.
		return false;
	}
	USoundWave* Sound = ResolveSoundForEvent(Event);
	if (Sound == nullptr)
	{
		// Missing/empty soft reference: one diagnostic per event type, then
		// silent skip. Combat never depends on audio being present.
		const bool bHit = (Event.Type == ECombatAudioEventType::Hit);
		bool& bLoggedFlag = bHit ? bLoggedMissingHitSound : bLoggedMissingLandSound;
		if (!bLoggedFlag)
		{
			bLoggedFlag = true;
			UE_LOG(LogTemp, Warning,
				TEXT("UEMMO UCombatPresentationComponent: %s sound asset missing (empty or unloadable soft reference); combat continues without this sound."),
				bHit ? TEXT("hit") : TEXT("landing"));
		}
		return true;
	}
	PlayCombatSound(Sound, Event.WorldLocation);
	return true;
}

void UCombatPresentationComponent::HandleHitConfirmed(const FCombatHit& Hit)
{
	// M1-033: the confirmed hit executes the local hit stop on both parties -
	// the attacking component (this source) and the hit target's own combat
	// component. The components own the freeze mechanics (clock pinning,
	// movement save/restore, max reentry, reset/death cleanup); the presenter
	// is the trigger because it is the one place that already observes the
	// accepted hit event for presentation purposes. Bare combat components
	// without a bound presenter keep the pre-M1-033 behavior verbatim (the
	// same inertness pattern as the M1-021 input-driven start).
	if (UCombatComponent* Source = CombatSource.Get())
	{
		Source->RequestHitStop(Hit.HitStopSeconds);
	}
	if (const AActor* TargetActor = Hit.Target.Get())
	{
		if (UCombatComponent* TargetCombat = TargetActor->FindComponentByClass<UCombatComponent>())
		{
			TargetCombat->RequestHitStop(Hit.HitStopSeconds);
		}
	}

	// One accepted damage application = exactly one hit sound request; the
	// dispatcher's dedup key (InstigatorId, AttackInstanceId, HitGroupId,
	// TargetId) makes the once-per-hit guarantee local to this component too.
	SubmitAudioEvent(BuildHitAudioEvent(Hit));
}

void UCombatPresentationComponent::SetAnimInstancePausedForHitStop(UAnimInstance* AnimInstance, bool bPaused)
{
	if (AnimInstance == nullptr)
	{
		return;
	}
	if (bPaused)
	{
		// Montage playback (the attack montage on the player, any future
		// montage on a target): pause every active instance.
		AnimInstance->Montage_Pause(nullptr);
		// Single-node playback (the training enemy's looping idle) is not a
		// montage; stop its advance the same way the node tree would.
		if (UAnimSingleNodeInstance* SingleNode = Cast<UAnimSingleNodeInstance>(AnimInstance))
		{
			SingleNode->SetPlaying(false);
		}
	}
	else
	{
		// Resume exactly what the pause covered.
		AnimInstance->Montage_Resume(nullptr);
		if (UAnimSingleNodeInstance* SingleNode = Cast<UAnimSingleNodeInstance>(AnimInstance))
		{
			SingleNode->SetPlaying(true);
		}
	}
}

void UCombatPresentationComponent::SetHitStopPaused(bool bPaused)
{
	// Dispatch record first (tests observe the seam even with a null mesh, the
	// same handoff-counting pattern as the audio play seam), then the real
	// pause on the injected mesh's animation.
	HitStopPauseDispatchHistory.Add(bPaused);
	USkeletalMeshComponent* Mesh = MeshTarget.Get();
	UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
	SetAnimInstancePausedForHitStop(AnimInstance, bPaused);
}

void UCombatPresentationComponent::HandleHitStopChanged(bool bFrozen)
{
	SetHitStopPaused(bFrozen);
}

void UCombatPresentationComponent::UnbindDelegates()
{
	if (bDelegatesBound)
	{
		if (UCombatComponent* Combat = CombatSource.Get())
		{
			Combat->OnStarted.RemoveAll(this);
			Combat->OnFinished.RemoveAll(this);
			Combat->OnHitConfirmed.RemoveAll(this);
			Combat->OnHitStopChanged.RemoveAll(this);
		}
		bDelegatesBound = false;
	}
}
