#include "GoblinThrowerEnemy.h"

#include "../Character/DNFMCharacterVisualComponent.h"
#include "../Character/PlanarMovement.h"
#include "../Character/SideViewCameraComponent.h"
#include "../Combat/CombatComponent.h"
#include "Components/CapsuleComponent.h"

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
		// M8-019A: use the FIGHTER's proven in-game material as the MID parent;
		// the component's ApplyCurrentFrame then binds PageTex to the GOBLIN
		// page texture from the DA (the same contract the player's fighter
		// page follows). M_GoblinthrowerVisual (the v2 from-scratch build)
		// rendered nothing as a MID parent despite identical node dumps -
		// root cause unconfirmed, its assets stay on disk for P04 investigation.
		if (UMaterialInterface* FighterMaterial = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Game/UEMMO/Imported/DNFM130_5_8_202/Characters/Fighter/Materials/M_DNFMFighterVisual.M_DNFMFighterVisual")))
		{
			GoblinVisual->SetMaterialInterface(FighterMaterial);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("UEMMO M8-019A: fighter material missing; picture may stay hidden."));
		}
		GoblinVisual->SetVisualSet(Set);
		// Calibration uses the stay frame 0: the pivot is the foot, so the
		// standing height in pixels is the pivot's y inside the canvas; the
		// target cm keeps the source scale (M8-010 s=1.7615 cm/px).
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
		GoblinVisual->SetAction(FName(TEXT("stay")));
		CurrentDisplayAction = FName(TEXT("stay"));
		bVisualActivated = true;
		// Mirror the player's ApplyDNFMVisualIfEnabled activation tail: the
		// picture advances and re-places through the component tick.
		GoblinVisual->SetComponentTickEnabled(true);
		// M8-019A: the melee-enemy capsule root carries bVisible=false (shape
		// components default hidden); an attached picture plane inherits the
		// attachment chain's visibility and stays unrendered. The capsule
		// draws nothing in game either way - the flag only gates children.
		GetCapsuleComponent()->SetVisibility(true, false);
		// The picture owns the body: the 3D template mesh stays hidden (the
		// M6-021 mutual-exclusion rule).
		GetMesh()->SetVisibility(false, true);
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

void AGoblinThrowerEnemy::ResolveDisplayAction()
{
	const UCombatComponent* CombatComp = GetCombatComponent();
	const bool bDead = CombatComp ? CombatComp->IsDead() : false;
	const ECombatActionState State = CombatComp ? CombatComp->GetSnapshot().ActionState : ECombatActionState::Free;
	const float Speed2D = GetVelocity().Size2D();

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
