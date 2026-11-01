#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"

#include "SkillTypes.generated.h"

/**
 * M7-003: why a skill activation was refused (interface contract section 6:
 * TryActivateSkill returns Accepted or an EXPLICIT rejection reason). The
 * order of the checks is the precheck order of USkillLoadoutComponent.
 */
UENUM(BlueprintType)
enum class ESkillActivationError : uint8
{
	/** The skill started (or, for the buffered launcher, was paid on start). */
	None = 0,
	/** No loadout row maps this slot, or the slot key is out of 1..8. */
	InvalidSlot = 1,
	/** The active character never learned this skill (no rank row). */
	Locked = 2,
	/** The skill belongs to another profession than the active character. */
	WrongProfession = 3,
	/** The cooldown deadline has not been reached yet. */
	Cooldown = 4,
	/** Current MP is below the rank's mp_cost. */
	InsufficientMP = 5,
	/** The combat component cannot start an attack right now. */
	Busy = 6,
	/** The skill's attack id is not in the attack catalog. */
	MissingAttack = 7,
	/** The skill's attack animation asset is missing (a pending skill). */
	MissingAsset = 8,
	/** The caller had no profile/character/catalog/combat to drive. */
	InvalidContext = 9
};

/**
 * M7-003: one skill intent (interface contract section 6: FSkillIntent(slot,
 * active character)). The input layer builds it; the loadout component
 * resolves it against the profile/catalog. SlotIndex is 1..8 (Q..F).
 */
USTRUCT(BlueprintType)
struct FSkillIntent
{
	GENERATED_BODY()

	/** The hotbar slot that was pressed (1..8). */
	UPROPERTY(BlueprintReadWrite, Category = "Skills")
	int32 SlotIndex = 0;

	/** Business identity of the active character (from the profile). */
	UPROPERTY(BlueprintReadWrite, Category = "Skills")
	FGuid CharacterId;
};

/**
 * M7-003: the result of one skill activation attempt. bAccepted mirrors
 * Error == None; SkillId/AttackId name what would run; MPCost/CooldownS are
 * the rank's numbers the commit path applied; Message carries the
 * human-readable refusal.
 */
USTRUCT(BlueprintType)
struct FSkillActivationOutcome
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Skills")
	bool bAccepted = false;

	UPROPERTY(BlueprintReadOnly, Category = "Skills")
	ESkillActivationError Error = ESkillActivationError::None;

	/** The skill this activation resolved to (None on InvalidSlot/Context). */
	UPROPERTY(BlueprintReadOnly, Category = "Skills")
	FName SkillId = NAME_None;

	/** The combat attack the skill drives. */
	UPROPERTY(BlueprintReadOnly, Category = "Skills")
	FName AttackId = NAME_None;

	/**
	 * In-session instance id of the started attack (0 when refused). Kept as
	 * uint64 without Blueprint exposure: UHT does not support uint64 in
	 * Blueprint-visible properties (the FCombatSnapshot precedent).
	 */
	UPROPERTY(Transient)
	uint64 AttackInstanceId = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Skills")
	int32 MPCost = 0;

	/** The rank's damage coefficient (the projectile policy scales its profile). */
	UPROPERTY(BlueprintReadOnly, Category = "Skills")
	float DamageCoefficient = 1.f;

	UPROPERTY(BlueprintReadOnly, Category = "Skills")
	float CooldownSeconds = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Skills")
	FString Message;
};
