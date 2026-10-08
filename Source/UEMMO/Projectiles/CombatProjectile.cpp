// M5-027: the projectile actor (owner 027, the Projectiles/* Actor face).
// The structural carrier: one collision body, one movement component, no
// motion tick. M5-028 set the body's world-facing block responses (pawns and
// world static) so the swept transport can stop on targets and walls while
// projectiles never collide with each other; the motion/hit behavior itself
// lives in the M5-028 policy faces (LinearProjectilePolicy.h), not here.

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
	// M5-028: the swept transport blocks the world faces it can fly into -
	// pawns (targets) and world static (walls). Everything else stays ignored
	// so projectiles never collide with each other or with unrelated channels.
	Body->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	Body->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);

	Movement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("M5_027_Movement"));
	Movement->SetUpdatedComponent(Body);
	Movement->bAutoActivate = false;
}
