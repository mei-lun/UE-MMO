#include "MeleeEnemy.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../PrototypeHUD.h"
#include "MeleeEnemyController.h"

#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "EnemyDefinition.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameters.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// M2-003: telegraph mesh swell factor while the wind-up runs (minimal
	// presentation, the card allows color tint or simple scale).
	constexpr float M2_003_TelegraphVisualScale = 1.15f;

	// M3-025: the death presentation prototype standard - a dead wired enemy
	// is removed this many seconds after its death (the corpse window).
	constexpr float M3_025_CorpseCleanupDelaySeconds = 2.0f;

	// M3-027: the vector parameters of the mannequin material drive the hit
	// flash - every inherited vector parameter of the mesh's slot-0 material
	// carries the tint while the overlay renders (the render probe proved
	// BasicShapeMaterial overlays never reach the skeletal mesh, while the
	// slot-0-based one renders the whole body red).
	const FLinearColor M3_027_HitFlashTint(1.0f, 0.12f, 0.10f, 1.0f);

	// M3-027: horizontal speed (cm/s) above which the single-node mesh runs
	// the jog clip instead of the idle loop.
	constexpr float M3_027_RunAnimationSpeedThreshold = 20.0f;
}

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
		IdleLocomotionAsset = IdleAsset.Object;
		CurrentLocomotionAsset = IdleAsset.Object;
	}
	// M3-027: the in-place jog clip the enemy runs with while chasing (same
	// template mannequin family as the idle above; no new art). A moving
	// enemy that glides in the idle loop reads as "not moving" - the user's
	// fourth-round feedback. MF_Unarmed_Jog_Fwd is an AnimSequence (asset
	// verified), so the single-node mode can play it directly.
	static ConstructorHelpers::FObjectFinder<UAnimationAsset> RunAsset(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Jog/MF_Unarmed_Jog_Fwd.MF_Unarmed_Jog_Fwd"));
	if (RunAsset.Succeeded())
	{
		RunLocomotionAsset = RunAsset.Object;
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

	// M3-025: the pawn's default brain is the M2-002 controller, so the
	// production wiring's SpawnDefaultController call attaches exactly the
	// AMeleeEnemyController state machine. Auto possession stays Disabled on
	// purpose: the engine's PostInitializeComponents auto-possess path would
	// otherwise wire controllers behind the spawner's back (spawned pawns on
	// a still-startup world count as "placed in world"), which the M2-014
	// bookkeeping scenarios' actor counts must never see. The wiring - and
	// only the wiring - attaches controllers.
	AIControllerClass = AMeleeEnemyController::StaticClass();
	AutoPossessAI = EAutoPossessAI::Disabled;

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

	// M3-027: the enemy health bar is production HUD and tracks this enemy
	// from spawn (the M3-026 hit-driven upsert stays as the data refresher;
	// the user read the hit-only appearance as "no health bar at all"). A
	// world without the prototype HUD (headless/temp automation worlds)
	// skips silently - registration is a presentation nicety, never a
	// combat dependency.
	if (UWorld* WorldPtr = GetWorld())
	{
		if (APlayerController* PlayerController = WorldPtr->GetFirstPlayerController())
		{
			if (APrototypeHUD* Hud = Cast<APrototypeHUD>(PlayerController->GetHUD()))
			{
				Hud->TrackEnemyBarActor(this);
			}
		}
	}

	// M3-027: build the flash overlay once (a dynamic instance of this mesh's
	// own slot-0 material; applied through the mesh's SetOverlayMaterial while
	// the hit stun holds and removed when it ends). The render probe settled
	// WHY the first overlay attempt stayed invisible: the engine
	// BasicShapeMaterial (the M2-009 exit-cube tint precedent) renders red on
	// static meshes but silently renders NOTHING as a skeletal-mesh overlay
	// (its probe screenshot is pixel-identical to the unmodified mesh - the
	// material lacks the skeletal usage), while the same mesh slot-0 material
	// as the overlay base with every inherited vector parameter written red
	// renders the whole body red (the probe's params-red screenshot). The
	// mannequin material exposes no body-tint parameter by design contract -
	// its vector parameter set (Paint Tint / LogoTint / Blend Offset, probed
	// at runtime) drives the render, so all of them carry the flash tint.
	if (UMaterialInterface* Slot0 = GetMesh()->GetMaterial(0))
	{
		FlashOverlayMaterial = UMaterialInstanceDynamic::Create(Slot0, this);
		if (FlashOverlayMaterial != nullptr)
		{
			TArray<FMaterialParameterInfo> VectorInfos;
			TArray<FGuid> VectorIds;
			FlashOverlayMaterial->GetAllParameterInfoOfType(
				EMaterialParameterType::Vector, VectorInfos, VectorIds);
			for (const FMaterialParameterInfo& Info : VectorInfos)
			{
				FlashOverlayMaterial->SetVectorParameterValue(Info.Name, M3_027_HitFlashTint);
			}
			if (VectorInfos.Num() > 0)
			{
				// Test seam anchor: the first inherited vector parameter the
				// flash wrote (PeekHitFlashTint reads this back).
				FlashTintParameterName = VectorInfos[0].Name;
			}
		}
	}

	// M2-003: this enemy's own attacks fire through its combat component's
	// TryStartAttack (the shared M1 pipeline; the controller never touches a
	// victim's health directly), which needs the same read-only M1 attack
	// catalog the player pawn attaches in its BeginPlay. Same loading path
	// (Config/DefaultGame.ini references), same failure policy: on failure
	// no catalog is attached and TryStartAttack rejects every id with its
	// own diagnostic - attacks stay disabled, nothing crashes.
	if (Combat != nullptr)
	{
		UAttackCatalog* Catalog = NewObject<UAttackCatalog>(this);
		FText CatalogError;
		if (Catalog->InitializeFromConfig(CatalogError))
		{
			Combat->InitializeFromCatalog(Catalog);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("UEMMO: melee enemy attack catalog unavailable (%s); enemy attacks stay disabled."), *CatalogError.ToString());
		}
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

		// M3-027 presentation (follows the shared snapshot, the M1-032
		// "presentation follows combat state" direction): the hit flash is
		// active exactly while the accepted hit holds this enemy in HitStun
		// (no reaction animation asset exists, so the material tint is the
		// card's minimal victim-side feedback), and the single-node
		// locomotion switches idle/jog with the horizontal speed.
		const bool bStunned = Combat->GetSnapshot().ActionState == ECombatActionState::HitStun;
		if (bStunned != bHitFlashActive)
		{
			ApplyHitFlash(bStunned);
		}
		RefreshLocomotionAnimation();
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

void AMeleeEnemy::ApplyTelegraphVisual(bool bActive)
{
	USkeletalMeshComponent* MeshComponent = GetMesh();
	if (MeshComponent == nullptr)
	{
		return;
	}
	// Minimal wind-up presentation: a uniform mesh swell while the telegraph
	// runs, the plain identity scale when it ends. Presentation only - the
	// mesh has no collision, so the combat pipeline's real 3D hit query
	// (component contract section 5) is unaffected either way.
	MeshComponent->SetRelativeScale3D(bActive
		? FVector(M2_003_TelegraphVisualScale)
		: FVector::OneVector);
}

void AMeleeEnemy::LaunchCharacter(FVector LaunchVelocity, bool bXYOverride, bool bZOverride)
{
	// M3-025: the combat launch path (UCombatComponent::ApplyHitImpulse) sends
	// every hit that carries a launch component through here - the M1-022
	// ATrainingEnemy override pattern copied for the wave enemies, so a Z
	// launcher hit lifts them through the normal falling physics. A vertical
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
		// M3-027: the M1-022 ATrainingEnemy launched-airborne marker - the
		// only difference between a knocked-down landing and a plain one.
		bLaunchedAirborne = true;
	}
}

void AMeleeEnemy::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);

	// M3-025: minimal ground-contact recording - the landing re-opens the
	// ground phase so the next vertical launch counts 1 again (the M1-022
	// semantics; this enemy deliberately carries no landing audio).
	bGroundedSinceLastLaunch = true;

	// M3-027: the M1-026 landing-recovery semantics reach the wave enemy - a
	// LAUNCHED landing runs the Knockdown 0.45 s -> Recovering 0.25 s ->
	// Free process, so the launcher arc visibly ends on a downed enemy that
	// stays down (and holds its chase) before resuming. The component
	// refuses an already-running process and a dead combatant (death has
	// priority); a refused request leaves the marker set only for a dead
	// victim, where it is irrelevant (the corpse cleanup removes the actor).
	if (bLaunchedAirborne && Combat != nullptr)
	{
		const UWorld* WorldPtr = GetWorld();
		if (Combat->BeginLandingRecovery(WorldPtr ? WorldPtr->GetTimeSeconds() : 0.0))
		{
			bLaunchedAirborne = false;
		}
	}
}

void AMeleeEnemy::ArmDeathCleanup()
{
	// One-shot arming: the handler binds exactly once, and only on an enemy
	// with a health pool (the death event source).
	if (bDeathCleanupArmed || Health == nullptr)
	{
		return;
	}
	bDeathCleanupArmed = true;

	// The handler lives on the health component, which is a subobject of this
	// actor - the raw this capture cannot outlive the delegate's owner.
	Health->OnDied.AddLambda([this]()
	{
		// M3-025 death presentation, the prototype standard: stop the AI,
		// freeze the movement, drop every collision and remove the corpse
		// after the 2 s delay. The delay rides the world timer manager, so
		// it expires regardless of the frozen movement. The bookkeeping side
		// (AliveIds removal, the session kill count) stays with the spawner's
		// own death binding - this handler only stages the corpse.
		if (AController* EnemyController = GetController())
		{
			EnemyController->StopMovement();
			// The controller expires with the corpse: an emptied AI controller
			// must not linger in the world after its pawn is gone.
			EnemyController->SetLifeSpan(M3_025_CorpseCleanupDelaySeconds);
		}
		if (UCharacterMovementComponent* Movement = GetCharacterMovement())
		{
			// With the collision disabled the walking/falling physics would
			// otherwise sink the corpse through the floor during the corpse
			// window - freezing the mode keeps it where it died.
			Movement->StopMovementImmediately();
			Movement->DisableMovement();
		}
		SetActorEnableCollision(false);
		SetLifeSpan(M3_025_CorpseCleanupDelaySeconds);
	});
}

FLinearColor AMeleeEnemy::PeekHitFlashTint() const
{
	// Reads back the first inherited vector parameter the flash wrote (the
	// real MID value, not the constant) while the flash overlay renders.
	if (bHitFlashActive && FlashOverlayMaterial != nullptr && !FlashTintParameterName.IsNone())
	{
		return FlashOverlayMaterial->K2_GetVectorParameterValue(FlashTintParameterName);
	}
	return FLinearColor::White;
}

void AMeleeEnemy::ApplyHitFlash(bool bActive)
{
	bHitFlashActive = bActive;
	if (USkeletalMeshComponent* MeshComponent = GetMesh())
	{
		// The overlay pass renders the tint material across the whole mesh
		// while the accepted hit holds the victim and is removed on the stun
		// end - no per-slot material bookkeeping, nothing left behind.
		MeshComponent->SetOverlayMaterial(bActive ? FlashOverlayMaterial.Get() : nullptr);
	}
}

void AMeleeEnemy::RefreshLocomotionAnimation()
{
	USkeletalMeshComponent* MeshComponent = GetMesh();
	UAnimationAsset* RunAsset = RunLocomotionAsset.Get();
	UAnimationAsset* IdleAsset = IdleLocomotionAsset.Get();
	if (MeshComponent == nullptr || RunAsset == nullptr || IdleAsset == nullptr)
	{
		return;
	}
	const UCharacterMovementComponent* Movement = GetCharacterMovement();
	const bool bRunning = Movement != nullptr
		&& Movement->Velocity.Size2D() > M3_027_RunAnimationSpeedThreshold;
	UAnimationAsset* Desired = bRunning ? RunAsset : IdleAsset;
	if (Desired == CurrentLocomotionAsset.Get())
	{
		return;
	}
	CurrentLocomotionAsset = Desired;
	// Single-node switch: SetAnimation swaps the asset, Play restarts it as a
	// loop (the constructor's AnimationData looping flags cover the boot, the
	// switches cover the rest of the enemy's life).
	MeshComponent->SetAnimation(Desired);
	MeshComponent->Play(true);
}
