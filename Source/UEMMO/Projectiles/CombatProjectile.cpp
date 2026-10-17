// M5-027: the projectile actor (owner 027, the Projectiles/* Actor face).
// The structural carrier: one collision body, one movement component, no
// motion tick. M5-028 set the body's world-facing block responses (pawns and
// world static) so the swept transport can stop on targets and walls while
// projectiles never collide with each other; the motion/hit behavior itself
// lives in the M5-028 policy faces (LinearProjectilePolicy.h), not here.

#include "CombatProjectile.h"

#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

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

	// M5-061: an 8cm x 2cm rounded bullet. Keep visual size independent of
	// the conservative sweep radius. Hard references participate in cook.
	Visual = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("M5_035_Visual"));
	Visual->SetupAttachment(Body);
	Visual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> BulletMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (BulletMesh.Succeeded())
	{
		Visual->SetStaticMesh(BulletMesh.Object);
		Visual->SetRelativeScale3D(FVector(0.08f, 0.02f, 0.02f));
	}
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> BulletMaterial(TEXT("/Game/UEMMO/Materials/M_Bullet.M_Bullet"));
	if (BulletMaterial.Succeeded()) { Visual->SetMaterial(0, BulletMaterial.Object); }
	Visual->SetCastShadow(false);
}

void ACombatProjectile::FaceFlightDirection(const FVector& Direction)
{
	if (Visual && !Direction.IsNearlyZero()) { Visual->SetWorldRotation(Direction.Rotation()); }
}
