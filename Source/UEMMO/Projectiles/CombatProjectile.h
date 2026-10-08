#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "../Combat/System/CombatEventTypes.h"

#include "CombatProjectile.generated.h"

class USphereComponent;
class UProjectileMovementComponent;

/**
 * M5-027: the projectile actor (interface contract section 1, owner 027 the
 * "Projectiles/*" first owner of the Actor face). One actor = one pellet of
 * one committed shot. The card's lifetime face only - the motion/hit policy
 * interfaces for 029..032 are defined and frozen by M5-028, so this actor
 * carries no policy hook beyond the structural readiness (one projectile
 * movement component as the single motion driver, one query collision body
 * for the later sweep work).
 *
 * Contract pinned here:
 * - Single motion source: the UProjectileMovementComponent is the only thing
 *   that moves the actor. The actor never ticks for motion
 *   (PrimaryActorTick disabled) and never carries a second movement
 *   component - "no second motion tick stacked".
 * - Lifetime does not read source JSON: the spawn context arrives as a value
 *   snapshot (the committed FShotContext provenance, the resolved
 *   FProjectileDefinition numbers and the config revision string). The actor
 *   includes no Data/ConfigParser/catalog header and never resolves ids.
 * - Lifetime on the Pause-frozen World clock: the timeout face is the
 *   engine's standard LifeSpan (a world timer, frozen while paused), set
 *   from the definition's LifetimeS at spawn preparation.
 * - Provenance value snapshot: epoch, source entity, shot id, pellet index,
 *   weapon item instance, projectile instance id and the config revision are
 *   stamped by the world service at commit; the actor never mints identity.
 *   Plain C++ value struct on purpose - no reflection, no raw pointers.
 */

/** The committed-shot provenance stamped onto every pellet actor (a value snapshot). */
struct FProjectileSpawnContext
{
	/** The fire registry generation the shot was minted in. */
	FCombatEpoch Epoch = InvalidCombatEpoch;

	/** The registered fire source entity (the wielder's fire-lineage identity). */
	FEntityId SourceEntityId = InvalidCombatEntityId;

	/** The common ActionSequence value of the shot. */
	FShotId ShotId = InvalidCombatShotId;

	/** The pellet slot inside the volley (0..N-1; single pellet = 0). */
	FPelletIndex PelletIndex = InvalidCombatPelletIndex;

	/** The weapon item instance that fired (the M5-019 item identity). */
	FGuid WeaponInstanceId;

	/** The projectile instance identity minted for this reservation (traceable, not a pointer). */
	FGuid ProjectileInstanceId;

	/** The config revision the shot's definitions came from (provenance only). */
	FString ConfigRevision;

	/** The resolved projectile definition id. */
	FName ProjectileId;
};

UCLASS()
class ACombatProjectile : public AActor
{
	GENERATED_BODY()

public:
	ACombatProjectile();

	/** The single query collision body (the later sweep/hit work rides on it). */
	UPROPERTY(VisibleAnywhere, Category = "UEMMO|Projectile")
	TObjectPtr<USphereComponent> Body;

	/** The single motion driver: no second movement component, no motion tick. */
	UPROPERTY(VisibleAnywhere, Category = "UEMMO|Projectile")
	TObjectPtr<UProjectileMovementComponent> Movement;

	/** The committed-shot provenance (stamped by the world service at commit). */
	FProjectileSpawnContext Context;

	/** The resolved launch numbers (a value snapshot; never re-read from JSON). */
	float LaunchSpeedCmS = 0.0f;

	/** Gravity scale derived from the definition motion (0 straight/homing, 1 parabolic). */
	float MotionGravityScale = 0.0f;
};
