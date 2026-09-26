#pragma once

#include "CoreMinimal.h"
#include "CombatHitTypes.generated.h"

class AActor;

/**
 * Deduplication key of one hit (interface contract section 5):
 * (InstigatorId, AttackInstanceId, HitGroupId, TargetId).
 *
 * The attacker's stable session id is part of the key so two characters with
 * colliding local instance ids can never share a key. TargetId is the target
 * actor's session-unique object id (a serial number, never a pointer value).
 * A key is recorded only after the target's health actually accepted damage;
 * a refused hit records nothing so a later instance can hit the same target.
 */
struct FCombatHitDedupKey
{
	/** Stable in-session id of the attacking combat component (minted once). */
	uint64 InstigatorId = 0;

	/** In-session instance id of the attack that landed the hit. */
	uint64 AttackInstanceId = 0;

	/** Independent hit group inside the attack instance (0 = the single default group). */
	int32 HitGroupId = 0;

	/** Session-unique object id of the damaged target actor. */
	uint64 TargetId = 0;

	bool operator==(const FCombatHitDedupKey& Other) const
	{
		return InstigatorId == Other.InstigatorId
			&& AttackInstanceId == Other.AttackInstanceId
			&& HitGroupId == Other.HitGroupId
			&& TargetId == Other.TargetId;
	}
};

inline uint32 GetTypeHash(const FCombatHitDedupKey& Key)
{
	uint32 Hash = ::GetTypeHash(Key.InstigatorId);
	Hash = HashCombine(Hash, ::GetTypeHash(Key.AttackInstanceId));
	Hash = HashCombine(Hash, ::GetTypeHash(Key.HitGroupId));
	Hash = HashCombine(Hash, ::GetTypeHash(Key.TargetId));
	return Hash;
}

/**
 * One accepted hit (interface contract section 5). Carried by
 * UCombatComponent::OnHitConfirmed exactly once per successful damage
 * application: the query only selects candidates and ApplyDamage returning
 * a positive value is what produces this event (sounds and damage numbers
 * must never trigger from the overlap alone). Instigator and Target are weak
 * references: consumers must check them before use, a destroyed actor never
 * crashes a handler, it only yields a null pointer.
 */
USTRUCT(BlueprintType)
struct FCombatHit
{
	GENERATED_BODY()

	/** Attacking actor; weak, may be stale by the time a handler reads it. */
	UPROPERTY(BlueprintReadOnly, Category = "Combat")
	TWeakObjectPtr<AActor> Instigator;

	/** Stable in-session id of the attacker (never a pointer value). */
	UPROPERTY(Transient)
	uint64 InstigatorId = 0;

	/** In-session instance id of the attack that landed this hit. */
	UPROPERTY(Transient)
	uint64 AttackInstanceId = 0;

	/**
	 * Independent hit group inside the attack instance; 0 for every attack
	 * while only single-hit attacks exist. Multi-hit skills later define one
	 * group per sub-hit so each group can hit the same target once.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Combat")
	int32 HitGroupId = 0;

	/** Damaged target actor; weak, may be stale by the time a handler reads it. */
	UPROPERTY(BlueprintReadOnly, Category = "Combat")
	TWeakObjectPtr<AActor> Target;

	/** Health the target actually lost (the ApplyDamage return value, > 0). */
	UPROPERTY(BlueprintReadOnly, Category = "Combat")
	float Damage = 0.0f;

	/**
	 * Impulse the hit applies to a surviving target: X = +-KnockbackSpeed
	 * (mirrored by the attacker's facing), Y = 0, Z = LaunchSpeed in cm/s.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Combat")
	FVector Impulse = FVector::ZeroVector;

	/** World-space location the hit is reported at (the active hit box center). */
	UPROPERTY(BlueprintReadOnly, Category = "Combat")
	FVector WorldHitLocation = FVector::ZeroVector;

	/** Stable id of the attack definition that landed this hit. */
	UPROPERTY(BlueprintReadOnly, Category = "Combat")
	FName AttackId = NAME_None;

	/**
	 * Hit stop duration this hit requests, copied from the attack definition
	 * (0.04 s for the current attacks). Executing the pause belongs to the
	 * hit-stop task (M1-033); this card only carries the value.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Combat")
	float HitStopSeconds = 0.0f;

	/**
	 * M1-020: hit stun duration this hit requests on the victim, copied from
	 * the attack definition's HitStunSeconds (0.22 s for light_01). The value
	 * rides on the hit so the victim's combat component can consume it through
	 * NotifyHitReceived without ever reading the attacker's catalog.
	 */
	UPROPERTY(BlueprintReadOnly, Category = "Combat")
	float StunSeconds = 0.0f;
};
