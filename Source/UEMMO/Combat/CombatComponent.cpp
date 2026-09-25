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

		// M1-014: inside the cancel window the attack can switch into the
		// buffered Light follow-up. A successful switch retires the old
		// instance and starts the next one; this tick then stops advancing
		// the old timeline (the new instance steps from its next TickCombat,
		// so a chain never skips frames inside a single tick).
		if (TryChainFromBuffer())
		{
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

void UCombatComponent::QueueInput(FBufferedCombatInput Input)
{
	// Push rejections (duplicate/regressing sequence, non-finite time) are
	// intentionally silent: QueueInput reports nothing (M1-012 contract).
	InputBuffer.Push(Input);
}

bool UCombatComponent::PeekInputBuffer(FBufferedCombatInput& Out, int32 Index) const
{
	return InputBuffer.PeekAt(Index, Out);
}

void UCombatComponent::SetInputClockSeconds(double NowSeconds)
{
	InputClockSeconds = NowSeconds;
}

double UCombatComponent::GetInputClockSeconds() const
{
	return InputClockSeconds;
}

bool UCombatComponent::TryChainFromBuffer()
{
	// The chaining only reads definitions of the running attack and never
	// hardcodes windows or id lists: the cancel window and the allowed
	// follow-ups both come from the injected catalog's definition.
	const UAttackDefinition* Definition = (Catalog != nullptr) ? Catalog->Find(ActiveAttackId) : nullptr;
	if (Definition == nullptr)
	{
		return false;
	}
	if (!Definition->CancelWindow.Contains(CurrentFrame))
	{
		// Outside the cancel window the buffer is deliberately untouched:
		// presses wait there until the window opens (or expire there).
		return false;
	}

	// Lifetime rule first (interface contract section 2): expired entries are
	// dropped before any consumption attempt, and an age of exactly 150 ms is
	// still valid (only a strictly greater age expires).
	InputBuffer.PruneExpired(InputClockSeconds, 0.150);

	// Find whether any buffered Light exists without consuming it: a Light
	// that cannot chain (the running attack does not allow the light
	// follow-up) must stay buffered, so the consume happens only after the
	// switch is confirmed. The ConsumeFirst below then removes exactly the
	// earliest such entry because nothing mutates the buffer in between.
	bool bFoundLight = false;
	const int32 BufferedCount = InputBuffer.Size();
	for (int32 Index = 0; Index < BufferedCount && !bFoundLight; ++Index)
	{
		FBufferedCombatInput Entry;
		if (InputBuffer.PeekAt(Index, Entry) && Entry.Action == ECombatInput::Light)
		{
			bFoundLight = true;
		}
	}
	if (!bFoundLight)
	{
		// Other buffered actions (Launcher, Jump) stay untouched: consuming
		// them belongs to later tasks.
		return false;
	}

	// The Light press chains into the next light attack (M1-014 scope: the
	// light_02 follow-up); the running attack's own definition decides whether
	// that follow-up is allowed, so light_02 (whose next list holds only the
	// launcher) can never loop into itself.
	static const FName LightChainAttackId(TEXT("light_02"));
	if (!Definition->AllowedNextAttacks.Contains(LightChainAttackId))
	{
		// Kept for later consumers; deliberately not consumed here.
		return false;
	}
	// Belt-and-braces before consuming: the follow-up must exist in the
	// catalog and the component must be able to start it (TryStartAttack
	// refuses for a dead component).
	if (bDead || Catalog->Find(LightChainAttackId) == nullptr)
	{
		return false;
	}

	FBufferedCombatInput Consumed;
	if (!InputBuffer.ConsumeFirst(ECombatInput::Light, Consumed))
	{
		return false;
	}

	// Switch semantics: the old instance ends (exactly one Finished broadcast,
	// handlers observe a Free snapshot in between, like a natural end) and the
	// follow-up starts immediately with a fresh InstanceId and the same
	// Facing. Only one switch per step: the caller stops advancing the old
	// timeline, so a frame never skips through the follow-up.
	const int32 ChainedFacing = Facing;
	FinishCurrentAttack();
	TryStartAttack(LightChainAttackId, ChainedFacing);
	return true;
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
