#include "GoblinThrowerEnemy.h"

#include "../Character/DNFMCharacterVisualComponent.h"
#include "../Character/PlanarMovement.h"
#include "../Character/SideViewCameraComponent.h"
#include "../Combat/CombatComponent.h"
#include "EnemyDefinition.h"
#include "../PrototypeCharacter.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Components/CapsuleComponent.h"
#include "ThrowStoneProjectile.h"

AGoblinThrowerEnemy::AGoblinThrowerEnemy()
{
	GoblinVisual = CreateDefaultSubobject<UDNFMCharacterVisualComponent>(TEXT("GoblinDNFMVisual"));
	// M8-018 monster atlas (single-layer, per-frame anchor convention).
	MonsterVisualSet = TSoftObjectPtr<UDNFMCharacterVisualSet>(
		FSoftObjectPath(TEXT("/Game/UEMMO/DNFM/Monsters/Goblinthrower/DA_GoblinthrowerVisualSet.DA_GoblinthrowerVisualSet")));
}

void AGoblinThrowerEnemy::BeginPlay()
{
	Super::BeginPlay();

	// The goblin has no 3D alternative: the picture mode is its only display.
	if (UDNFMCharacterVisualSet* Set = MonsterVisualSet.LoadSynchronous())
	{
		// M8-019A: use the GOBLIN's own material (node default = the goblin
		// page) as the MID parent. The v2 from-scratch build rendered nothing
		// as a MID parent (rebuilt as a duplicate of the fighter material —
		// same node graph, proven shader lineage). The PageTex runtime
		// override to the goblin page is then a no-op (same texture), which
		// sidesteps the texture-override path entirely.
		UMaterialInterface* GoblinMaterial = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Game/UEMMO/DNFM/Monsters/Goblinthrower/M/M_GoblinthrowerVisual.M_GoblinthrowerVisual"));
		if (GoblinMaterial == nullptr)
		{
			UE_LOG(LogTemp, Warning, TEXT("UEMMO M8-019A: goblin material missing; picture may stay hidden."));
		}
		// M8-019A ROOT CAUSE: the goblin has no SideViewCameraComponent and no
		// rig; the component's camera-basis fallback produced a basis whose
		// quad rendered as a 1px-wide sliver (the thin-line artifact in the
		// first spawns). Pin the EXACT fixed side-view rig basis the player's
		// camera carries (yaw -90/pitch -18 => right=+X, up=(0,sin18,cos18)).
		GoblinVisual->SetCameraBasisProvider([](FVector& OutRight, FVector& OutUp)
		{
			OutRight = FVector(1.0f, 0.0f, 0.0f);
			const float PitchRad = FMath::DegreesToRadians(
				UE::UEMMO::Tasks::M1_030::SideViewCameraPitch);
			OutUp = FVector(0.0f, FMath::Sin(PitchRad), FMath::Cos(PitchRad));
		});
		// M8-019A: the full material+set+action sequence runs through this one
		// helper so it can be re-applied AFTER BeginPlay (the test calls
		// RefreshGoblinMaterial post-spawn; if the re-created MID renders
		// where the BeginPlay-time MID didn't, BeginPlay raced registration).
		auto ApplyVisual = [this, Set, GoblinMaterial]()
		{
			if (GoblinMaterial != nullptr)
			{
				GoblinVisual->SetMaterialInterface(GoblinMaterial);
			}
			GoblinVisual->SetVisualSet(Set);
			if (const FDNFMActionClip* Stay = Set->FindAction(FName(TEXT("stay"))))
			{
				if (Stay->Frames.Num() > 0)
				{
					const float StandingHeadTopPx = static_cast<float>(Stay->Frames[0].PivotPx.Y);
					if (StandingHeadTopPx > 0.f)
					{
						GoblinVisual->SetWorldScaleFromStandingHeight(StandingHeadTopPx, StandingHeadTopPx * CmPerPixel);
					}
				}
			}
			GoblinVisual->SetAction(FName(TEXT("stay")));
			CurrentDisplayAction = FName(TEXT("stay"));
		};
		ApplyVisual();
		bVisualActivated = true;
		GetMesh()->SetVisibility(false, true);
		// Mirror the player's ApplyDNFMVisualIfEnabled activation tail: the
		// picture advances and re-places through the component tick.
		GoblinVisual->SetComponentTickEnabled(true);
		// M8-019A: the melee-enemy capsule root carries bVisible=false (shape
		// components default hidden); an attached picture plane inherits the
		// attachment chain's visibility and stays unrendered. The capsule
		// draws nothing in game either way - the flag only gates children.
		GetCapsuleComponent()->SetVisibility(true, false);
	// M8-021: the goblin's own hit voice (the M8-009 located clip) through
	// the presenter's existing audio chain - the slice's audible victim
	// feedback for the monster side.
	SetHitFeedbackSound(TSoftObjectPtr<USoundWave>(FSoftObjectPath(
		TEXT("/Game/UEMMO/DNFM/Audio/SND_gbn_dmg_01.SND_gbn_dmg_01"))));
		// The picture owns the body: the 3D template mesh stays hidden (the
		// M6-021 mutual-exclusion rule).
		GetMesh()->SetVisibility(false, true);
		// M8-019A: expose the re-apply helper for the acceptance test.
		RefreshVisual = [ApplyVisual]() { ApplyVisual(); };
	}
	else
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO M8-019A: goblin visual set missing (%s); the 3D template stays visible."),
			*MonsterVisualSet.ToString());
	}
}

void AGoblinThrowerEnemy::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bVisualActivated)
	{
		ResolveDisplayAction();
	}
}

bool AGoblinThrowerEnemy::LaunchAttack(FName AttackId, int32 Facing)
{
	// M8-019B: the throw replaces the melee instance entirely - the stone
	// carries the damage through the unified health chain when its 20cm
	// flight box reaches the target. The chase target IS the player pawn
	// (the same target the spawner injects into the controller), so the
	// release resolves it through the player pawn directly.
	UWorld* World = GetWorld();
	APrototypeCharacter* Target = World
		? Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0))
		: nullptr;
	if (World == nullptr || Target == nullptr)
	{
		return false;
	}

	// The hand anchor: the foot position + the forward/up offsets (design
	// defaults - no monster bone data; recorded in the report).
	const float CapsuleHalfHeight = GetCapsuleComponent()
		? GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() : 90.f;
	const FVector Foot = GetActorLocation() - FVector(0.f, 0.f, CapsuleHalfHeight);
	const FVector HandAnchor = Foot + FVector(Facing >= 0 ? HandForwardCm : -HandForwardCm, 0.f, HandUpCm);

	if (!StoneMaterialWeak.IsValid())
	{
		StoneMaterialWeak = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Game/UEMMO/DNFM/Monsters/Goblinthrower/M/M_ThrowStoneVisual.M_ThrowStoneVisual"));
	}
	const float Damage = GetEnemyDefinition() ? GetEnemyDefinition()->AttackPower : DefaultThrowDamage;
	AThrowStoneProjectile* Stone = AThrowStoneProjectile::Spawn(
		World, HandAnchor, Facing, Damage, Target, StoneMaterialWeak.Get());
	if (Stone == nullptr)
	{
		return false;
	}

	// The release pose holds for the attack_0 clip remainder before the
	// state machine resolves stay/move again.
	AttackClipHoldUntilSeconds = World->GetTimeSeconds() + AttackClipRemainderSeconds;
	return true;
}

void AGoblinThrowerEnemy::ApplyTelegraphVisual(bool bActive)
{
	if (!bVisualActivated || GoblinVisual == nullptr)
	{
		// No picture yet (or none ever): the base's mesh swell is inert on
		// the hidden template mesh but keeps its contract.
		Super::ApplyTelegraphVisual(bActive);
		return;
	}
	if (bActive)
	{
		// The source warning: attack_0's startup frames ARE the telegraph.
		AttackClipHoldUntilSeconds = 0.0;
		if (GoblinVisual->SetAction(FName(TEXT("attack_0"))))
		{
			CurrentDisplayAction = FName(TEXT("attack_0"));
		}
	}
	// bActive=false (fire or cancel): the hold was set inside LaunchAttack on
	// a real release; a cancel falls through to the state machine's own
	// damage/down/stay resolution on the next tick.
}

void AGoblinThrowerEnemy::ResolveDisplayAction()
{
	const UCombatComponent* CombatComp = GetCombatComponent();
	const bool bDead = CombatComp ? CombatComp->IsDead() : false;
	const ECombatActionState State = CombatComp ? CombatComp->GetSnapshot().ActionState : ECombatActionState::Free;
	const float Speed2D = GetVelocity().Size2D();

	// M8-019B: hold the attack_0 release pose for the clip remainder after
	// the stone is away (every interrupt - death/stun/knockdown - overrides
	// the hold and resolves its own display action).
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	const bool bInterrupted = bDead
		|| State == ECombatActionState::HitStun
		|| State == ECombatActionState::Knockdown
		|| State == ECombatActionState::Recovering;
	if (!bInterrupted && Now < AttackClipHoldUntilSeconds)
	{
		return;
	}

	FName Desired;
	bool bHoldLastFrame = false;
	if (bDead)
	{
		// M8-007 death chain: the down sequence plays out and holds its last
		// frame (the M6-024 death rule); overturn stays for M8-019B's
		// interruption composition.
		Desired = FName(TEXT("down"));
		bHoldLastFrame = true;
	}
	else if (State == ECombatActionState::Knockdown || State == ECombatActionState::Recovering)
	{
		Desired = FName(TEXT("down"));
	}
	else if (State == ECombatActionState::HitStun)
	{
		// Alternate the two single-frame damage holds (M8-007: damage_0/1).
		Desired = FName((HitStunAlternator % 2 == 0) ? TEXT("damage_0") : TEXT("damage_1"));
	}
	else if (Speed2D > 20.f)
	{
		Desired = FName(TEXT("move"));
	}
	else
	{
		Desired = FName(TEXT("stay"));
	}

	if (Desired != CurrentDisplayAction)
	{
		if (GoblinVisual->SetAction(Desired))
		{
			CurrentDisplayAction = Desired;
			if (State == ECombatActionState::HitStun)
			{
				++HitStunAlternator;
			}
		}
	}
	else if (bHoldLastFrame && Desired == FName(TEXT("down")))
	{
		// Death hold: pin the display to the final drawn frame (the M6-024
		// rule); the clip itself is non-looping in the manifest.
		const UDNFMCharacterVisualSet* Set = GoblinVisual->GetVisualSet();
		if (Set != nullptr)
		{
			const int32 ActionIndex = Set->GetActionIndex(Desired);
			if (Set->Actions.IsValidIndex(ActionIndex)
				&& Set->Actions[ActionIndex].Frames.Num() > 0)
			{
				GoblinVisual->SeekToDisplayFrame(Set->Actions[ActionIndex].Frames.Num() - 1);
			}
		}
	}
}
