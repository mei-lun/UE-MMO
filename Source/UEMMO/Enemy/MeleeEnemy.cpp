#include "MeleeEnemy.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "MeleeEnemyController.h"

#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EnemyDefinition.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "UObject/ConstructorHelpers.h"

AMeleeEnemy::AMeleeEnemy()
{
	// The actor must tick so its own Tick can drive the combat component
	// every game frame (the ATrainingEnemy M1-043 driver dependency).
	PrimaryActorTick.bCanEverTick = true;

	// Same mannequin capsule as ATrainingEnemy (Quinn is roughly 180 cm tall:
	// 34 cm radius, 88 cm half height so feet meet the floor plane).
	GetCapsuleComponent()->InitCapsuleSize(34.f, 88.f);

	// Same mesh-relative setup as APrototypeCharacter/ATrainingEnemy: the
	// mannequin asset is authored facing +Y in component space, so yaw -90
	// maps it to actor +X (actor yaw 0 = facing +X, the M2-002 facing rule).
	GetMesh()->SetRelativeLocation(FVector(0.f, 0.f, -88.f));
	GetMesh()->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
	GetMesh()->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// Same Quinn template asset as the training dummy (no new art; the card
	// allows reusing the TrainingEnemy visual until a dedicated M2 mesh).
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
		// Single-node looping idle mirrors the training dummy (no AnimBP).
		GetMesh()->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		GetMesh()->AnimationData.AnimToPlay = IdleAsset.Object;
		GetMesh()->AnimationData.bSavedLooping = true;
		GetMesh()->AnimationData.bSavedPlaying = true;
	}

	// Contract default approach speed (UEnemyDefinition::MoveSpeed default);
	// SetEnemyDefinition overrides it from the applied definition data.
	GetCharacterMovement()->MaxWalkSpeed = 220.0f;

	// Pre-possession physics: without a controller the movement physics would
	// never run (mode stays MOVE_None) - the same rationale as ATrainingEnemy.
	// Once an AMeleeEnemyController possesses the pawn the flag is irrelevant
	// (controller-driven physics takes over), so it only covers the settle
	// window between spawn and possession.
	GetCharacterMovement()->bRunPhysicsWithNoController = true;

	Health = CreateDefaultSubobject<UHealthComponent>(TEXT("MeleeEnemyHealth"));

	// The shared combat component (interface contract 7): an accepted hit can
	// interrupt and stun this enemy through the victim-side entry.
	Combat = CreateDefaultSubobject<UCombatComponent>(TEXT("MeleeEnemyCombat"));
}

void AMeleeEnemy::BeginPlay()
{
	Super::BeginPlay();

	// Death has priority over hit stun (M1-020): when the health pool dies the
	// combat component is marked dead. Health and Combat live and die with
	// this actor, so the captured lambda cannot dangle.
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

void AMeleeEnemy::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	// M1-043 driver, copied from ATrainingEnemy::Tick: inject the input game
	// clock once per game frame BEFORE TickCombat from the World
	// GetTimeSeconds clock, so the victim-side hit stop, hit stun and landing
	// recovery advance on their deadlines. A world-less pawn keeps the
	// pre-M1-043 semantics (no injection).
	if (Combat != nullptr)
	{
		if (UWorld* World = GetWorld())
		{
			Combat->SetInputClockSeconds(World->GetTimeSeconds());
		}
		Combat->TickCombat(DeltaSeconds);
	}
}

void AMeleeEnemy::SetEnemyDefinition(const UEnemyDefinition* InDefinition)
{
	EnemyDefinition = InDefinition;
	if (InDefinition != nullptr && GetCharacterMovement() != nullptr)
	{
		GetCharacterMovement()->MaxWalkSpeed = InDefinition->MoveSpeed;
	}
}

void AMeleeEnemy::ApplyFacingIntent(const FVector& MoveIntent)
{
	const float CurrentYaw = GetActorRotation().Yaw;
	const float NewYaw = ComputeMeleeFacingYaw(MoveIntent, CurrentYaw);
	SetActorRotation(FRotator(0.0f, NewYaw, 0.0f));
}
