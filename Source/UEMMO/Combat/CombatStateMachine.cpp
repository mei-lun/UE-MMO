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
		static const TArray<uint8> Active = {uint8(P::Recovery), uint8(P::HitStun), uint8(P::LaunchAir), uint8(P::Knockdown), uint8(P::Dead)};
		static const TArray<uint8> Recovery = {uint8(P::Idle), uint8(P::HitStun), uint8(P::LaunchAir), uint8(P::Knockdown), uint8(P::Dead)};
		static const TArray<uint8> CancelWindow = {uint8(P::Windup), uint8(P::Idle), uint8(P::HitStun), uint8(P::LaunchAir), uint8(P::Knockdown), uint8(P::Dead)};
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
		ApplyTransition(ECombatPhase::Idle);
		return;
	}

	if (Facts.Frame < Facts.ActiveStartFrame)
	{
		ApplyTransition(ECombatPhase::Windup);
		return;
	}
	if (Facts.Frame < Facts.ActiveEndFrame)
	{
		ApplyTransition(ECombatPhase::Active);
		return;
	}
	// The cancel window opens at cancel_start and stays through the end.
	bCancelWindowOpen = Facts.Frame >= Facts.CancelStartFrame && Facts.Frame < Facts.DurationFrames;
	if (bCancelWindowOpen)
	{
		ApplyTransition(ECombatPhase::CancelWindow);
		return;
	}
	ApplyTransition(ECombatPhase::Recovery);
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
