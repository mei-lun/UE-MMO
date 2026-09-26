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

	// M1-022: the dummy owns no AIController (M1-016), so without this flag
	// its movement physics would never initialize or run: the mode would stay
	// MOVE_None (which silently drops every AddImpulse and Launch) and
	// PhysWalking would zero any velocity it received. Running physics without
	// a controller gives the dummy a real physical presence: it settles onto
	// the floor under gravity, a launcher hit flies it through the normal
	// falling physics and every landing arrives through ACharacter::Landed
	// (which also feeds the M1-034 landing audio and the M1-022 ground record).
	GetCharacterMovement()->bRunPhysicsWithNoController = true;

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
			// M1-025: death clears the float cycle - a later reset/revive
			// starts a fresh cycle whose first launcher rises at full speed.
			LauncherCycleCount = 0;
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
		// M1-022: a room reset also drops any launch/impulse still pending on
		// the movement component and puts the dummy back on the ground, so the
		// replayable room never carries airborne state (or a resurrecting
		// pending launch) into the next run.
		Movement->ClearAccumulatedForces();
		Movement->SetMovementMode(MOVE_Walking);
	}
	// M1-022: the reset opens a fresh ground phase - no launcher combo, the
	// next launcher hit counts 1 again. The last-grounded time is a record of
	// real landing events and is left untouched (a reset is not a landing).
	AirComboCount = 0;
	bGroundedSinceLastLaunch = true;
	// M1-025: a room reset also reopens the policy float cycle, so the next
	// launcher rises at full launch speed again.
	LauncherCycleCount = 0;
}

ECombatAirState ATrainingEnemy::GetAirState() const
{
	// Grounded while the movement component walks on ground; the actual
	// vertical speed decides the airborne phase (interface contract section
	// 4): Z > 0 Rising, otherwise Falling - an airborne apex at Z == 0 already
	// falls on the next update. Computed on demand, so walking ground input
	// can never stale it and a launched state can never be masked.
	const UCharacterMovementComponent* Movement = GetCharacterMovement();
	if (Movement == nullptr || Movement->IsMovingOnGround())
	{
		return ECombatAirState::Grounded;
	}
	return Movement->Velocity.Z > 0.0f ? ECombatAirState::Rising : ECombatAirState::Falling;
}

void ATrainingEnemy::RecordLauncherLaunch()
{
	// M1-025: one applied launcher launch extends the current float cycle's
	// policy count. The cycle reset points (ground contact, death,
	// ResetEnemy) zero it; the attacker's combat component records the launch
	// only when the launcher impulse was actually applied (a refused launcher
	// and a lethal hit leave the count untouched).
	++LauncherCycleCount;
}

void ATrainingEnemy::RecordGroundContact(double NowSeconds)
{
	// M1-022: the landing time is the ground-contact record the later
	// floating-state tasks read; the real Landed notify feeds this with the
	// world time. A ground contact closes the running launcher combo: the
	// next launcher hit opens a fresh one at 1 (the count cap and the
	// Z-factor decay stay with M1-025; recording only here).
	LastGroundedTimeSeconds = NowSeconds;
	bGroundedSinceLastLaunch = true;
	// M1-025: landing recovers the float cycle - the policy count reopens at
	// zero so the next launcher rises at full launch speed again.
	LauncherCycleCount = 0;
}

void ATrainingEnemy::LaunchCharacter(FVector LaunchVelocity, bool bXYOverride, bool bZOverride)
{
	// M1-022: the combat launch path (UCombatComponent::ApplyHitImpulse) sends
	// every hit that carries a launch component through here. A vertical
	// launch opens or continues the pre-landing launcher combo: the first
	// launch from ground contact counts 1, every further launcher hit before
	// the next ground contact increments. The flag is the hit-time ground
	// state (not the movement mode, which the deferred launch only flips on
	// the next applied movement update), so the classification never races
	// the launch itself.
	Super::LaunchCharacter(LaunchVelocity, bXYOverride, bZOverride);
	if (LaunchVelocity.Z > 0.0f)
	{
		AirComboCount = bGroundedSinceLastLaunch ? 1 : AirComboCount + 1;
		bGroundedSinceLastLaunch = false;
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
	// M1-022: every landing is a ground contact (recorded time, launcher
	// combo bookkeeping) before the audio dispatch below.
	RecordGroundContact(Now);
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
