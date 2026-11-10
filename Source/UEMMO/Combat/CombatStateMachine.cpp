#include "CombatStateMachine.h"

#include "Engine/World.h"

namespace
{
	// M8-031: the transition table (the plan 08 problem-1 design: the data-
	// driven legal transitions). Mirrored by Data/CombatSystem/
	// state_transitions.json; the code table is the enforcement source.
	using P = ECombatPhase;
	const TArray<uint8>& TransitionsFrom(ECombatPhase From)
	{
		static const TArray<uint8> Idle = {uint8(P::Windup), uint8(P::HitStun), uint8(P::LaunchAir), uint8(P::Knockdown), uint8(P::Dead)};
		static const TArray<uint8> Windup = {uint8(P::Active), uint8(P::HitStun), uint8(P::LaunchAir), uint8(P::Knockdown), uint8(P::Dead)};
		// M8-037: the cancel window is an attack sub-phase the flow passes
		// through (the derivation enters it at cancel_start from Active, or
		// from Recovery when the cancel window opens late) - without these
		// two edges CancelWindow was unreachable and every derivation into
		// it was refused (the M8-031 acceptance caught it red).
		static const TArray<uint8> Active = {uint8(P::Recovery), uint8(P::CancelWindow), uint8(P::HitStun), uint8(P::LaunchAir), uint8(P::Knockdown), uint8(P::Dead)};
		static const TArray<uint8> Recovery = {uint8(P::Idle), uint8(P::CancelWindow), uint8(P::HitStun), uint8(P::LaunchAir), uint8(P::Knockdown), uint8(P::Dead)};
		// M8-037: the cancel window closes back into the post-cancel recovery
		// tail (the frame table's [cancel_end, duration) frames) - the natural
		// attack-phase walk alongside Windup/Active/CancelWindow.
		static const TArray<uint8> CancelWindow = {uint8(P::Windup), uint8(P::Recovery), uint8(P::Idle), uint8(P::HitStun), uint8(P::LaunchAir), uint8(P::Knockdown), uint8(P::Dead)};
		static const TArray<uint8> HitStun = {uint8(P::Idle), uint8(P::Knockdown), uint8(P::Dead), uint8(P::LaunchAir)};
		static const TArray<uint8> LaunchAir = {uint8(P::Knockdown), uint8(P::Idle), uint8(P::Dead)};
		static const TArray<uint8> Knockdown = {uint8(P::GetUp), uint8(P::Dead)};
		static const TArray<uint8> GetUp = {uint8(P::Idle), uint8(P::HitStun), uint8(P::Dead)};
		static const TArray<uint8> Dead = {uint8(P::Idle)};
		static const TArray<uint8> None = {};
		switch (From)
		{
		case P::Idle: return Idle;
		case P::Windup: return Windup;
		case P::Active: return Active;
		case P::Recovery: return Recovery;
		case P::CancelWindow: return CancelWindow;
		case P::HitStun: return HitStun;
		case P::LaunchAir: return LaunchAir;
		case P::Knockdown: return Knockdown;
		case P::GetUp: return GetUp;
		case P::Dead: return Dead;
		default: return None;
		}
	}
}

UCombatStateMachine::UCombatStateMachine()
{
	PrimaryComponentTick.bCanEverTick = false;
}

bool UCombatStateMachine::TransitionAllowed(ECombatPhase From, ECombatPhase To)
{
	if (From == To)
	{
		return true; // staying in a phase is never a "transition"
	}
	for (uint8 ToValue : TransitionsFrom(From))
	{
		if (ToValue == uint8(To))
		{
			return true;
		}
	}
	return false;
}

bool UCombatStateMachine::ApplyTransition(ECombatPhase NewPhase)
{
	if (NewPhase == Phase)
	{
		return false;
	}
	if (!TransitionAllowed(Phase, NewPhase))
	{
		// The table refuses: log once and keep the current phase (the plan's
		// "illegal transitions refused and logged" contract).
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO M8-031: transition refused by the table (from %d to %d)."),
			int32(Phase), int32(NewPhase));
		return false;
	}
	const ECombatPhase Old = Phase;
	Phase = NewPhase;
	OnPhaseChanged.Broadcast(Old, NewPhase);
	return true;
}

void UCombatStateMachine::UpdateFromFacts(const FCombatPhaseFacts& Facts)
{
	// Death has absolute priority (the interface contract: death first).
	if (Facts.bDead)
	{
		ApplyTransition(ECombatPhase::Dead);
		return;
	}

	// The victim timers override any attack phase (an accepted hit
	// interrupts the windup/active/recovery - the M8-007 rule).
	if (Facts.bHitStun)
	{
		ApplyTransition(ECombatPhase::HitStun);
		return;
	}
	if (Facts.bKnockdown)
	{
		ApplyTransition(ECombatPhase::Knockdown);
		return;
	}
	if (Facts.bRecovering)
	{
		ApplyTransition(ECombatPhase::GetUp);
		return;
	}
	if (Facts.bLaunchedAir)
	{
		ApplyTransition(ECombatPhase::LaunchAir);
		return;
	}

	// The attack phase derivation from the running instance's frame
	// counters (the frame table stays the single source of truth).
	if (Facts.Frame < 0)
	{
		// M8-031 regression fix: a teardown is a FACT, not a table
		// transition. The real combat flow legitimately tears the running
		// instance down mid-Active (the jump cancel, the accepted-hit
		// interrupt and ResetCombat all ClearInstance before this derivation
		// can observe the attack's own cancel window), so the table's
		// Active->Idle refusal stuck the phase forever and every later gate
		// read then refused the attack (the user's "attacks completely
		// failed" package regression). The reset bypasses the table and
		// broadcasts like any accepted change; the stale cancel flag clears.
		ForcePhase(ECombatPhase::Idle);
		bCancelWindowOpen = false;
		return;
	}

	if (Facts.Frame < Facts.ActiveStartFrame)
	{
		// A fresh instance starts: the previous attack's cancel flag never
		// leaks into the new windup (IsCancelWindowOpen stays honest).
		bCancelWindowOpen = false;
		ApplyTransition(ECombatPhase::Windup);
		return;
	}
	if (Facts.Frame < Facts.ActiveEndFrame)
	{
		ApplyTransition(ECombatPhase::Active);
		return;
	}
	// The cancel window mirrors the frame table's half-open
	// [cancel_start, cancel_end); the [cancel_end, duration) tail derives
	// Recovery (the M8-037 fix - the DurationFrames-only bound swallowed the
	// tail into the cancel window and diverged from the live chain check,
	// which reads the same definition window).
	bCancelWindowOpen = Facts.Frame >= Facts.CancelStartFrame && Facts.Frame < Facts.CancelEndFrame;
	if (bCancelWindowOpen)
	{
		ApplyTransition(ECombatPhase::CancelWindow);
		return;
	}
	ApplyTransition(ECombatPhase::Recovery);
}

void UCombatStateMachine::ForcePhase(ECombatPhase NewPhase)
{
	if (NewPhase == Phase)
	{
		return;
	}
	const ECombatPhase Old = Phase;
	Phase = NewPhase;
	OnPhaseChanged.Broadcast(Old, NewPhase);
}

bool UCombatStateMachine::CanStartAttack() const
{
	// The table: Idle -> Windup and CancelWindow -> Windup are the two legal
	// attack-start entries (the recovery must complete; the stun/knockdown/
	// dead refuse by their own tables).
	return TransitionAllowed(Phase, ECombatPhase::Windup);
}

FName UCombatStateMachine::PhaseName(ECombatPhase Phase)
{
	switch (Phase)
	{
	case ECombatPhase::Idle: return FName(TEXT("Idle"));
	case ECombatPhase::Windup: return FName(TEXT("Windup"));
	case ECombatPhase::Active: return FName(TEXT("Active"));
	case ECombatPhase::Recovery: return FName(TEXT("Recovery"));
	case ECombatPhase::CancelWindow: return FName(TEXT("CancelWindow"));
	case ECombatPhase::HitStun: return FName(TEXT("HitStun"));
	case ECombatPhase::LaunchAir: return FName(TEXT("LaunchAir"));
	case ECombatPhase::Knockdown: return FName(TEXT("Knockdown"));
	case ECombatPhase::GetUp: return FName(TEXT("GetUp"));
	case ECombatPhase::Dead: return FName(TEXT("Dead"));
	default: return FName(TEXT("Unknown"));
	}
}
