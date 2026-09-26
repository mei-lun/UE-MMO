#include "TrainingEnemy.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"

#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "UObject/ConstructorHelpers.h"

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
