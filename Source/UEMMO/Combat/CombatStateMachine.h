// M8-031 part1: the explicit combat phase state machine (the quality plan
// 08 problem 1). The frame-table numbers (combat-attacks.json active/cancel
// windows) and the victim timers stay the single source of truth - this
// component is their EXPLICIT WRAPPER: it derives one of ten named phases
// from the combat snapshot, enforces the data-driven transition table on
// every change, and broadcasts phase-entered events for the display layer
// (which no longer reverse-derives the phase from scattered checks).
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"

#include "CombatStateMachine.generated.h"

/**
 * M8-031: the ten named combat phases (the plan 08 problem-1 state set).
 * The attack phases (Windup/Active/Recovery/CancelWindow) are derived from
 * the running attack's frame counters against its definition windows; the
 * victim phases mirror the combat component's victim state. Append only -
 * never renumber.
 */
UENUM(BlueprintType)
enum class ECombatPhase : uint8
{
	/** No action in flight. */
	Idle = 0,
	/** An attack instance is running before its active window. */
	Windup = 1,
	/** The attack's active window (the hit queries are open). */
	Active = 2,
	/** After the active window, before the attack ends. */
	Recovery = 3,
	/** The cancel window (the next attack may chain). */
	CancelWindow = 4,
	/** An accepted hit stunned the combatant. */
	HitStun = 5,
	/** A launcher lifted the combatant. */
	LaunchAir = 6,
	/** A launched landing knocked the combatant down. */
	Knockdown = 7,
	/** The knockdown ended; getting back up. */
	GetUp = 8,
	/** Dead (until ResetHealth opens a new lifecycle). */
	Dead = 9
};

DECLARE_MULTICAST_DELEGATE_TwoParams(FOnCombatPhaseChanged, ECombatPhase /*Old*/, ECombatPhase /*New*/);

/** The per-tick combat facts the phase derivation consumes (plain values -
 * the state machine has no dependency on the combat component header). */
struct FCombatPhaseFacts
{
	bool bDead = false;
	bool bHitStun = false;
	bool bKnockdown = false;
	bool bRecovering = false;
	bool bLaunchedAir = false;
	/** The running attack's logic frame (-1 = no attack in flight). */
	int32 Frame = -1;
	/** The attack's definition windows (the active/cancel boundaries). */
	int32 ActiveStartFrame = 0;
	int32 ActiveEndFrame = 0;
	int32 CancelStartFrame = 0;
	int32 DurationFrames = 0;
};

UCLASS(ClassGroup = (Combat), meta = (BlueprintSpawnableComponent))
class UEMMO_API UCombatStateMachine : public UActorComponent
{
	GENERATED_BODY()

public:
	UCombatStateMachine();

	/**
	 * Derives the phase from the combat facts (the frame counters and the
	 * victim timers) and applies the transition table; broadcasts the
	 * change. Called by the combat component once per TickCombat.
	 */
	void UpdateFromFacts(const FCombatPhaseFacts& Facts);

	/**
	 * The attack-start gate: true when the current phase may legally enter
	 * Windup (the transition table). The combat component consults this
	 * before accepting an attack intent.
	 */
	bool CanStartAttack() const;

	/** Whether the cancel window is open (the next attack may chain). */
	bool IsCancelWindowOpen() const { return bCancelWindowOpen; }

	ECombatPhase GetPhase() const { return Phase; }
	static FName PhaseName(ECombatPhase Phase);

	/** Fired on every accepted phase change (old, new). */
	FOnCombatPhaseChanged OnPhaseChanged;

	/** The transition-table lookup (the pure function; unit-testable). */
	static bool TransitionAllowed(ECombatPhase From, ECombatPhase To);

private:
	/** Applies the transition if the table allows it; returns true on change. */
	bool ApplyTransition(ECombatPhase NewPhase);

	/**
	 * Sets the phase unconditionally (the table is not consulted) and
	 * broadcasts the change like any accepted transition. Only for the
	 * fact-driven resets the table cannot model - the instance teardown.
	 */
	void ForcePhase(ECombatPhase NewPhase);

	ECombatPhase Phase = ECombatPhase::Idle;
	bool bCancelWindowOpen = false;
};
