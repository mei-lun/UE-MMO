#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"
#include "Engine/DataAsset.h"
#include "UObject/SoftObjectPtr.h"
#include "Animation/AnimSequence.h"

#include "CombatWindow.h"

#include "AttackDefinition.generated.h"

/**
 * Editor-serializable half-open frame window [StartFrame, EndFrame) for attack
 * data assets. FCombatWindow (M1-005) is a plain C++ struct without UPROPERTY
 * support, so this USTRUCT mirrors the same semantics by delegating to a
 * transient FCombatWindow; the half-open window rules stay defined in one
 * place inside FCombatWindow and are not copied here.
 */
USTRUCT(BlueprintType)
struct FAttackFrameWindow
{
	GENERATED_BODY()

	/** First frame covered by the window (inclusive). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat")
	int32 StartFrame = 0;

	/** Frame after the last covered frame (exclusive). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat")
	int32 EndFrame = 0;

	/** Same contract as FCombatWindow::IsValid: non-empty and inside [0, Duration]. */
	bool IsValid(int32 Duration) const
	{
		return ToCombatWindow().IsValid(Duration);
	}

	/** Same contract as FCombatWindow::Contains: StartFrame <= Frame < EndFrame. */
	bool Contains(int32 Frame) const
	{
		return ToCombatWindow().Contains(Frame);
	}

	/** Same contract as FCombatWindow::Crosses over the advanced frames (Previous, Current]. */
	bool Crosses(int32 PreviousFrame, int32 CurrentFrame) const
	{
		return ToCombatWindow().Crosses(PreviousFrame, CurrentFrame);
	}

	/** Builds the runtime window type on the fly (no duplicated state in this struct). */
	FCombatWindow ToCombatWindow() const
	{
		FCombatWindow Window;
		Window.StartFrame = StartFrame;
		Window.EndFrame = EndFrame;
		return Window;
	}
};

/**
 * Data-driven definition of one attack (interface contract section 3).
 * Pure data: units are part of the field names (Frames / Seconds / cm speeds
 * and offsets), seconds and frames are never mixed. Validate a single entry
 * with ValidateAttackDefinition; cross-entry checks such as AllowedNextAttacks
 * referencing existing attack IDs belong to the catalog tasks (M1-009/M1-010).
 */
UCLASS(BlueprintType)
class UAttackDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Stable identifier used by inputs, combos and the catalog. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Identity")
	FName AttackId;

	/** Total attack length in frames; must be a positive integer. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Timing")
	int32 DurationFrames = 0;

	/** Half-open [StartFrame, EndFrame) frames in which the attack can hit. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Timing")
	FAttackFrameWindow ActiveWindow;

	/** Half-open [StartFrame, EndFrame) frames after which the attack can be canceled. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Timing")
	FAttackFrameWindow CancelWindow;

	/** Stable IDs of attacks allowed to follow this one; existence is checked by the catalog, not per entry. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Timing")
	TArray<FName> AllowedNextAttacks;

	/** Base damage before the attack coefficient; finite and >= 0. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Damage")
	float BaseDamage = 0.0f;

	/** Multiplier applied on top of BaseDamage; finite and > 0. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Damage")
	float AttackCoefficient = 1.0f;

	/** Hit box center offset in cm measured from the character's feet location. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Hit")
	FVector HitOffsetFromFeet = FVector(95.0f, 0.0f, 90.0f);

	/** Hit box half extent in cm; every component must be finite and > 0. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Hit")
	FVector HitHalfExtent = FVector(85.0f, 50.0f, 70.0f);

	/** Knockback impulse as an X speed in cm/s applied to the target; finite and >= 0. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Hit")
	float KnockbackSpeed = 0.0f;

	/** Launch impulse as a Z speed in cm/s applied to the target; finite and >= 0. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Hit")
	float LaunchSpeed = 0.0f;

	/** Hit stun duration applied to the target in seconds; finite and >= 0. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Hit")
	float HitStunSeconds = 0.0f;

	/** Hit stop duration in seconds; finite and >= 0. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Hit")
	float HitStopSeconds = 0.04f;

	/** Animation asset played for this attack; soft reference keeps the asset optional. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Animation")
	TSoftObjectPtr<UAnimSequence> Animation;

	/** Clip start in seconds inside the source animation; finite and >= 0. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Animation")
	float ClipStartSeconds = 0.0f;

	/** Clip end in seconds inside the source animation; finite and >= 0. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Animation")
	float ClipEndSeconds = 0.0f;

	/** True while the attack plays a placeholder animation; clip ordering is only checked for real animations. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Animation")
	bool bPlaceholderAnimation = true;
};

/**
 * Validates a single attack definition in isolation and returns true when legal.
 * On failure OutErrors joins every problem with "; " and each problem names its
 * field (e.g. "DurationFrames", "ActiveWindow", "BaseDamage"). Pure check: it
 * never creates or mutates assets. Cross-entry validation (AllowedNextAttacks
 * referring to existing attack IDs) is intentionally out of scope here; the
 * catalog tasks (M1-009/M1-010) can call this per entry in a loop and add the
 * set-based checks on top of it.
 */
bool ValidateAttackDefinition(const UAttackDefinition& Attack, FText& OutErrors);
