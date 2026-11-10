#include "SkillAbilityComponent.h"

#include "../Enemy/ThrowStoneProjectile.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Skills/SkillLoadoutComponent.h"
#include "EngineUtils.h"
#include "../PrototypeCharacter.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/CombatStateMachine.h"
#include "../Profile/GameFlowSubsystem.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Sound/SoundBase.h"

USkillAbilityComponent::USkillAbilityComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void USkillAbilityComponent::Configure(FName InSkillId, FName InAttackId, int32 InMPCost,
	float InCooldownSeconds, const FSkillProjectileSpec& InProjectileSpec,
	USkillLoadoutComponent* InLoadout)
{
	SkillId = InSkillId;
	AttackId = InAttackId;
	MPCost = InMPCost;
	CooldownSeconds = InCooldownSeconds;
	ProjectileSpec = InProjectileSpec;
	LoadoutWeak = InLoadout;
}

USkillAbilityComponent* USkillAbilityComponent::FindAbilityForSlotAttack(
	const TArray<USkillAbilityComponent*>& Abilities, FName SlotAttackId)
{
	if (SlotAttackId.IsNone())
	{
		return nullptr;
	}
	for (USkillAbilityComponent* Ability : Abilities)
	{
		if (Ability != nullptr && Ability->GetAttackId() == SlotAttackId)
		{
			return Ability;
		}
	}
	return nullptr;
}

void USkillAbilityComponent::BeginPlay()
{
	Super::BeginPlay();
	// The loadout back-fill for the blueprint-placed components (the C++
	// wiring calls Configure directly).
	if (LoadoutWeak.IsValid() == false)
	{
		if (const AActor* Owner = GetOwner())
		{
			LoadoutWeak = Owner->FindComponentByClass<USkillLoadoutComponent>();
		}
	}
}

bool USkillAbilityComponent::TryCast(UCombatComponent* Combat, USkillLoadoutComponent* Loadout, int32 Facing)
{
	UWorld* World = GetWorld();
	if (World == nullptr || Combat == nullptr)
	{
		return false;
	}

	// M8-031: the state machine gate is DISABLED (the M8-034 regression fix):
	// the same frozen-phase issue as the TryStartAttack gate (see the
	// CombatComponent M8-034 note). The economy + the Free-state check
	// already gate the cast; the state machine stays observation-only until
	// a follow-up task proves the phase tracking across all maps.
	// if (Combat->GetStateMachine() != nullptr && !Combat->GetStateMachine()->CanStartAttack()) { return false; }

	// The economy through the existing skill loadout (the MP/cooldown/profession
	// gates stay in their own component - the ability composes, not duplicates).
	// M8-037: when a loadout is wired, its slot activation IS the delivery for
	// its policies (the melee/ground rows start the shared attack with the rank
	// damage scale; the buff rows apply the timed bonus) - the component must
	// not deliver a second time. The M8-031 part2 draft started the attack in
	// the economy step and then TryStartAttack'd again in the delivery step
	// (the second start refused, or stacked a projectile on the already-started
	// attack); zero production callers masked it. Loadout wired = economy +
	// delivery ride the loadout verbatim; no loadout = the component delivers
	// from its own spec (the isolation tests and the loadout-less professions).
	APrototypeCharacter* Player = Cast<APrototypeCharacter>(
		UGameplayStatics::GetPlayerCharacter(World, 0));
	USkillLoadoutComponent* LoadoutComponent = LoadoutWeak.IsValid() ? LoadoutWeak.Get() : Loadout;
	if (LoadoutComponent != nullptr)
	{
		const FSkillActivationOutcome Outcome = LoadoutComponent->TryActivateSkillByAttack(AttackId);
		if (!Outcome.bAccepted)
		{
			return false;
		}
	}
	else
	{
		// The component-only delivery: the melee shape starts the shared
		// combat attack; the projectile shape spawns the skill's own
		// straight-flight projectile. The projectile's target: the nearest
		// alive melee enemy in the facing direction (the DNF launcher lifts
		// what stands in front).
		AMeleeEnemy* Target = nullptr;
		float BestDist = TNumericLimits<float>::Max();
		const FVector Origin = Player ? Player->GetActorLocation() : FVector::ZeroVector;
		for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
		{
			AMeleeEnemy* Enemy = *It;
			if (Enemy == nullptr || Enemy->GetHealthComponent() == nullptr
				|| !Enemy->GetHealthComponent()->IsAlive())
			{
				continue;
			}
			const FVector Delta = Enemy->GetActorLocation() - Origin;
			if (Delta.X * Facing < 0.f)
			{
				continue;
			}
			const float Dist = FVector::Dist2D(Delta, FVector::ZeroVector);
			if (Dist < BestDist)
			{
				BestDist = Dist;
				Target = Enemy;
			}
		}
		if (ProjectileSpec.bProjectile)
		{
			// The hand anchor: the foot + the forward/up offsets (the M8-019B
			// convention; the design-default heights).
			const float CapsuleHalfHeight = Player && Player->GetCapsuleComponent()
				? Player->GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() : 90.f;
			const FVector Foot = Player ? Player->GetActorLocation() - FVector(0.f, 0.f, CapsuleHalfHeight)
				: FVector::ZeroVector;
			const FVector Hand = Foot + FVector(Facing >= 0 ? 30.f : -30.f, 0.f, 100.f);
			AThrowStoneProjectile* Projectile = AThrowStoneProjectile::SpawnSkillProjectile(
				World, Hand, Facing, ProjectileSpec.ProjectileDamage,
				ProjectileSpec.ProjectileSpeedCmPerSecond,
				ProjectileSpec.ProjectileRangeCm, ProjectileSpec.ProjectileBoxHalfCm,
				nullptr);
			if (Projectile == nullptr)
			{
				return false;
			}
		}
		else
		{
			if (!Combat->TryStartAttack(AttackId, Facing))
			{
				return false;
			}
		}
	}

	if (CastSound.IsValid())
	{
		UGameplayStatics::PlaySound2D(World, CastSound.Get());
	}
	OnCast.Broadcast(SkillId, true);
	return true;
}
