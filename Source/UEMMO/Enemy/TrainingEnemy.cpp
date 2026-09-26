#include "TrainingEnemy.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"

#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundWave.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// M1-034: manually picked landing sound from the Kenney Impact CC0 set
	// (the punch medium wave is the presentation component's hit default).
	const TCHAR* const DefaultLandSoundPath = TEXT("/Game/ThirdParty/Kenney/Impact/impactSoft_heavy_000.impactSoft_heavy_000");
}

ATrainingEnemy::ATrainingEnemy()
{
	// Quinn is roughly 180 cm tall: keep the standard mannequin capsule
	// (34 cm radius, 88 cm half height) so feet meet the floor plane.
	GetCapsuleComponent()->InitCapsuleSize(34.f, 88.f);

	// Same mesh-relative setup as APrototypeCharacter: the mannequin asset is
	// authored facing +Y in component space, so yaw -90 maps it to actor +X.
	GetMesh()->SetRelativeLocation(FVector(0.f, 0.f, -88.f));
	GetMesh()->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
	GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// Quinn appearance via the exact template assets the M0 preview actor
	// uses. The editor script verifies this and re-applies as a fallback.
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> MeshAsset(
		TEXT("/Game/Characters/Mannequins/Meshes/SKM_Quinn_Simple.SKM_Quinn_Simple"));
	if (MeshAsset.Succeeded())
	{
		GetMesh()->SetSkeletalMeshAsset(MeshAsset.Object);
	}
	static ConstructorHelpers::FObjectFinder<UAnimationAsset> IdleAsset(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/MM_Idle.MM_Idle"));
	if (IdleAsset.Succeeded())
	{
		// Single-node looping idle mirrors ResourcePreview_Quinn (no AnimBP).
		GetMesh()->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		GetMesh()->AnimationData.AnimToPlay = IdleAsset.Object;
		GetMesh()->AnimationData.bSavedLooping = true;
		GetMesh()->AnimationData.bSavedPlaying = true;
	}

	Health = CreateDefaultSubobject<UHealthComponent>(TEXT("EnemyHealth"));

	// M1-020: the enemy carries the shared combat component so an accepted hit
	// can interrupt and stun it through the victim-side entry (the attacker's
	// damage application calls NotifyHitReceived on the target's component).
	Combat = CreateDefaultSubobject<UCombatComponent>(TEXT("EnemyCombat"));

	// M1-034: default landing sound (Kenney Impact CC0 soft heavy impact).
	LandSound = TSoftObjectPtr<USoundWave>(FSoftObjectPath(DefaultLandSoundPath));
}

void ATrainingEnemy::BeginPlay()
{
	Super::BeginPlay();
	if (!bSpawnAnchorCaptured)
	{
		SpawnAnchorLocation = GetActorLocation();
		SpawnAnchorRotation = GetActorRotation();
		bSpawnAnchorCaptured = true;
	}
	// M1-020: death has priority over hit stun. When the health pool dies the
	// combat component is marked dead, so a lethal hit never stuns and the
	// enemy never comes back when a stun timer would end. Health and Combat
	// live and die with this actor, so the captured lambda cannot dangle.
	if (Health != nullptr && Combat != nullptr)
	{
		Health->OnDied.AddLambda([this]()
		{
			if (Combat != nullptr)
			{
				Combat->SetDead(true);
			}
		});
	}
}

void ATrainingEnemy::SetSpawnAnchor(const FVector& Location, const FRotator& Rotation)
{
	SpawnAnchorLocation = Location;
	SpawnAnchorRotation = Rotation;
	bSpawnAnchorCaptured = true;
}

void ATrainingEnemy::ResetEnemy()
{
	if (Health)
	{
		// ResetHealth restores full HP and opens a new death lifecycle.
		Health->ResetHealth();
	}
	if (Combat)
	{
		// M1-020: the combat component must not carry a dead flag, a stun or
		// stale buffered input into the next life, so the documented
		// "replayable room" contract keeps holding. The unified reset
		// semantics stay with M1-027; this only covers the added component.
		Combat->SetDead(false);
		Combat->ResetCombat();
	}
	SetActorLocation(SpawnAnchorLocation, false, nullptr, ETeleportType::TeleportPhysics);
	SetActorRotation(SpawnAnchorRotation);
	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->StopMovementImmediately();
		Movement->Velocity = FVector::ZeroVector;
	}
}

void ATrainingEnemy::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);

	// M1-034: one landing round = one land request. The epoch advances only
	// after the round window since the previous Landed notify, so bounce-style
	// duplicate notifies reuse the epoch and the dispatcher's dedup key
	// (actor id, epoch) rejects them; a later round starts a fresh epoch.
	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	if (LastLandedNotifySeconds < 0.0 || Now - LastLandedNotifySeconds > LandRoundWindowSeconds)
	{
		++LandingEpoch;
	}
	LastLandedNotifySeconds = Now;
	SubmitLandingAudio(LandingEpoch, Now);
}

bool ATrainingEnemy::SubmitLandingAudio(uint64 InLandingEpoch, double NowSeconds)
{
	// The dispatcher is the once-per-round evidence surface: an accepted
	// request resolves the soft sound reference and plays it; a rejected one
	// (same round inside the interval or voice budget exhausted) stays silent.
	LandAudioDispatcher.SetClockSeconds(NowSeconds);

	FCombatAudioEvent Event;
	Event.Type = ECombatAudioEventType::Land;
	Event.WorldLocation = GetActorLocation();
	Event.TargetActor = this;
	Event.TargetId = static_cast<uint64>(GetUniqueID());
	Event.LandingEpoch = InLandingEpoch;
	if (!LandAudioDispatcher.Submit(Event))
	{
		return false;
	}

	USoundWave* Sound = LandSound.LoadSynchronous();
	if (Sound == nullptr)
	{
		// Missing/empty soft reference: one diagnostic, then silent skip.
		// Gameplay never depends on audio being present (no crash).
		if (!bLoggedMissingLandSound)
		{
			bLoggedMissingLandSound = true;
			UE_LOG(LogTemp, Warning,
				TEXT("UEMMO ATrainingEnemy: landing sound asset missing (empty or unloadable soft reference); landing continues without sound."));
		}
		return true;
	}
	PlayLandSound(Sound, Event.WorldLocation);
	return true;
}

void ATrainingEnemy::PlayLandSound(USoundWave* Sound, const FVector& Location)
{
	if (Sound == nullptr)
	{
		return;
	}
	++DispatchedLandingAudioCount;
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		// Tests and torn-down actors: the play request is dispatched (counted)
		// but nothing can sound; never a crash, never an audibility claim.
		return;
	}
	UGameplayStatics::PlaySoundAtLocation(this, Sound, Location, LandingAudioVolumeMultiplier);
}
