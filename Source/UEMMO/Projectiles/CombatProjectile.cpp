// M5-027: the projectile actor (owner 027, the Projectiles/* Actor face).
// STUB: the class compiles with its structural defaults (single collision
// body, single movement component, no motion tick) but carries no spawn-time
// stamping of its own - the world service stub refuses everything. The real
// implementation replaces the construction setup and the service body.

#include "CombatProjectile.h"

#include "Components/SphereComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"

ACombatProjectile::ACombatProjectile()
{
	PrimaryActorTick.bCanEverTick = false;

	Body = CreateDefaultSubobject<USphereComponent>(TEXT("M5_027_Body"));
	RootComponent = Body;
	Body->InitSphereRadius(8.0f);
	Body->SetCollisionEnabled(ECollisionEnabled::QueryAndProbe);
	Body->SetCollisionObjectType(ECC_WorldDynamic);
	Body->SetCollisionResponseToAllChannels(ECR_Ignore);

	Movement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("M5_027_Movement"));
	Movement->SetUpdatedComponent(Body);
	Movement->bAutoActivate = false;
}
