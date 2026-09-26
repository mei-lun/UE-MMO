#include "CombatPresentationComponent.h"

#include "AttackDefinition.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimMontage.h"
#include "Components/SkeletalMeshComponent.h"

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
}

UCombatPresentationComponent::UCombatPresentationComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
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
		// future interrupt-to-free path (hit stun / knockdown wiring).
		CombatSource->OnStarted.AddUObject(this, &UCombatPresentationComponent::HandleStarted);
		CombatSource->OnFinished.AddUObject(this, &UCombatPresentationComponent::HandleFinished);
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
	const FString ObjectPath = FString(MontageFolder) + AttackId.ToString() + TEXT(".") + AttackId.ToString();
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

void UCombatPresentationComponent::UnbindDelegates()
{
	if (bDelegatesBound)
	{
		if (UCombatComponent* Combat = CombatSource.Get())
		{
			Combat->OnStarted.RemoveAll(this);
			Combat->OnFinished.RemoveAll(this);
		}
		bDelegatesBound = false;
	}
}
