#include "CombatComponent.h"

#include "AttackCatalog.h"
#include "AttackDefinition.h"
#include "Logging/LogMacros.h"

UCombatComponent::UCombatComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

bool UCombatComponent::InitializeFromCatalog(UAttackCatalog* InCatalog)
{
	if (InCatalog == nullptr)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO UCombatComponent: InitializeFromCatalog rejected a null catalog; the previously attached catalog (if any) stays in place."));
		return false;
	}
	Catalog = InCatalog;
	LoggedMissingAttackIds.Reset();
	return true;
}

bool UCombatComponent::TryStartAttack(FName AttackId, int32 NewFacing)
{
	// Death has the highest priority (interface contract section 4).
	if (bDead)
	{
		return false;
	}
	// One action at a time: an in-flight attack rejects unconditional reentry.
	if (ActionState != ECombatActionState::Free)
	{
		return false;
	}

	const UAttackDefinition* Definition = (Catalog != nullptr) ? Catalog->Find(AttackId) : nullptr;
	if (Definition == nullptr)
	{
		// Missing definition (or no catalog attached): reject with a once-per-id
		// diagnostic so mashing the same unknown input cannot spam the log.
		bool bAlreadyLogged = false;
		LoggedMissingAttackIds.Add(AttackId, &bAlreadyLogged);
		if (!bAlreadyLogged)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("UEMMO UCombatComponent: no attack definition for '%s' in the attached catalog; start rejected."),
				*AttackId.ToString());
		}
		return false;
	}

	// Every successful start mints a fresh stable in-session instance id
	// (monotonic from 1, never a raw pointer value, never reused).
	ActiveInstanceId = NextInstanceId++;
	ActiveAttackId = AttackId;
	ActiveDurationFrames = Definition->DurationFrames;
	Facing = NewFacing;
	CurrentFrame = -1;
	Clock.Reset();
	ActionState = ECombatActionState::Attacking;
	OnStarted.Broadcast(ActiveAttackId, ActiveInstanceId);
	return true;
}

void UCombatComponent::TickCombat(float DeltaSeconds)
{
	if (ActionState != ECombatActionState::Attacking)
	{
		return;
	}

	const int32 Steps = Clock.Advance(DeltaSeconds, bClockFrozen);
	for (int32 Step = 0; Step < Steps; ++Step)
	{
		++CurrentFrame;
		// Half-open timeline: DurationFrames - 1 is the last covered frame;
		// the step that lands on it ends the attack (26 steps for light_01).
		if (CurrentFrame >= ActiveDurationFrames - 1)
		{
			FinishCurrentAttack();
			return;
		}
	}
}

void UCombatComponent::ResetCombat()
{
	// Reset is a teardown, not an attack ending: no OnFinished. The instance
	// id counter and the dead flag are intentionally left alone (the unified
	// reset semantics belong to M1-027).
	ClearInstance();
	InputBuffer.Reset();
}

void UCombatComponent::SetDead(bool bNewDead)
{
	bDead = bNewDead;
}

bool UCombatComponent::IsDead() const
{
	return bDead;
}

FCombatSnapshot UCombatComponent::GetSnapshot() const
{
	FCombatSnapshot Snapshot;
	Snapshot.AttackId = ActiveAttackId;
	Snapshot.InstanceId = ActiveInstanceId;
	Snapshot.Frame = CurrentFrame;
	Snapshot.Facing = Facing;
	Snapshot.ActionState = ActionState;
	Snapshot.BufferSize = InputBuffer.Size();
	return Snapshot;
}

bool UCombatComponent::CanAcceptMovement() const
{
	return !bDead && ActionState == ECombatActionState::Free;
}

bool UCombatComponent::CanTurn() const
{
	return !bDead && ActionState == ECombatActionState::Free;
}

void UCombatComponent::SetClockFrozen(bool bNewFrozen)
{
	bClockFrozen = bNewFrozen;
}

bool UCombatComponent::IsClockFrozen() const
{
	return bClockFrozen;
}

void UCombatComponent::ClearInstance()
{
	ActionState = ECombatActionState::Free;
	ActiveAttackId = NAME_None;
	ActiveInstanceId = 0;
	ActiveDurationFrames = 0;
	CurrentFrame = -1;
	Facing = 0;
	Clock.Reset();
}

void UCombatComponent::FinishCurrentAttack()
{
	const FName FinishedAttackId = ActiveAttackId;
	const uint64 FinishedInstanceId = ActiveInstanceId;
	// Clear first: a Finished handler observes a Free snapshot and can already
	// chain the next attack from inside the callback (combo wiring belongs to
	// a later task). Exactly one broadcast per completed instance.
	ClearInstance();
	OnFinished.Broadcast(FinishedAttackId, FinishedInstanceId);
}
