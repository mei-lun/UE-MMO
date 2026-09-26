#include "CombatComponent.h"

#include "AttackCatalog.h"
#include "AttackDefinition.h"
#include "CombatGeometry.h"
#include "CombatHitQuery.h"
#include "HealthComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Logging/LogMacros.h"

#include <atomic>

namespace
{
	// Session-wide monotonic source of stable attacker ids (interface contract
	// sections 4 and 5): minted once per component, never a raw pointer value,
	// never reused within the session. Starts at 1 so 0 stays "no id".
	std::atomic<uint64> M1_019_NextInstigatorId{1};

	// The design damage formula (Docs/01 section 8.2):
	// damage = max(1, round((baseDamage + AttackPower * coefficient)
	//                        * 100 / (100 + max(0, Defense))))
	// Attacker AttackPower and defender Defense have no growth source yet
	// (profile stats belong to M3), so both stay 0 here and the result is
	// max(1, round(BaseDamage)): light_01 deducts exactly 10 and stays
	// verifiable. The formula keeps the two attribute entry points named so
	// M3 can wire real stats without reshaping the call site.
	float M1_019_ComputeHitDamage(const UAttackDefinition& Definition)
	{
		constexpr float AttackPower = 0.0f;
		constexpr float Defense = 0.0f;
		const float RawDamage = (Definition.BaseDamage + AttackPower * Definition.AttackCoefficient)
			* 100.0f / (100.0f + FMath::Max(0.0f, Defense));
		return FMath::Max(1.0f, FMath::RoundToFloat(RawDamage));
	}
}

UCombatComponent::UCombatComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	CachedInstigatorId = M1_019_NextInstigatorId.fetch_add(1) + 1;
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
	// M1-020: a stun runs on the injected input clock (the same explicitly
	// injected "now" as the buffered input lifetimes). The owner keeps calling
	// TickCombat; on the first tick whose clock reached the stun end the state
	// returns to Free. The action-clock delta plays no role while stunned.
	if (ActionState == ECombatActionState::HitStun)
	{
		if (InputClockSeconds >= HitStunEndTimeSeconds)
		{
			EndHitStun();
		}
		return;
	}

	if (ActionState != ECombatActionState::Attacking)
	{
		return;
	}

	const int32 Steps = Clock.Advance(DeltaSeconds, bClockFrozen);
	for (int32 Step = 0; Step < Steps; ++Step)
	{
		++CurrentFrame;
		// M1-019: the current frame's active-window hits run before the finish
		// check, so a definition whose active window includes the final frame
		// still lands its hit exactly once. The stub applies nothing yet.
		TryApplyActiveWindowHits();
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

void UCombatComponent::NotifyHitReceived(const FCombatHit& Hit)
{
	// A hit that does not name this owner is not this owner's business
	// (defense in depth for the public victim-side entry).
	if (Hit.Target.Get() != GetOwner())
	{
		return;
	}
	// Death has the highest priority (interface contract section 4): a dead
	// combatant neither stuns nor re-runs any death handling.
	if (bDead)
	{
		return;
	}

	// A lethal hit (the attacker already applied the damage before this call,
	// so the owner's health pool is gone) marks the component dead instead of
	// stunning it: a dead combatant never comes back when a stun timer ends.
	const UHealthComponent* OwnerHealth = GetOwner() ? GetOwner()->FindComponentByClass<UHealthComponent>() : nullptr;
	if (OwnerHealth != nullptr && !OwnerHealth->IsAlive())
	{
		SetDead(true);
		CancelCurrentAttack(FName(TEXT("Death")));
		EndHitStun();
		return;
	}

	// Capture the remaining stun before the cancel (the cancel itself never
	// touches an existing stun, but ClearInstance resets the bookkeeping).
	const double RemainingSeconds = (ActionState == ECombatActionState::HitStun)
		? FMath::Max(0.0, HitStunEndTimeSeconds - InputClockSeconds)
		: 0.0;

	// Interrupt first: the running attack dies with the accepted hit and its
	// instance hit set (and any pending active-window damage with it) is
	// cleared, so the interrupted instance can never hit again.
	CancelCurrentAttack(FName(TEXT("HitStun")));

	// New stun = max(remaining, new): a hit during a stun refreshes to the
	// longer of the two and never stacks additively. Non-finite requests are
	// treated as no stun; a zero total leaves the component simply Free.
	const double RequestedSeconds = FMath::IsFinite(Hit.StunSeconds) ? static_cast<double>(Hit.StunSeconds) : 0.0;
	const double StunSeconds = FMath::Max(RemainingSeconds, RequestedSeconds);
	if (StunSeconds > 0.0)
	{
		ActionState = ECombatActionState::HitStun;
		HitStunEndTimeSeconds = InputClockSeconds + StunSeconds;
	}
}

void UCombatComponent::CancelCurrentAttack(FName Reason)
{
	// Idempotent: only a running attack can be cancelled; Free and HitStun
	// already have no instance to tear down.
	if (ActionState != ECombatActionState::Attacking)
	{
		return;
	}
	// A cancel is an interruption, not the attack reaching its final frame:
	// no OnFinished (that delegate's contract is the natural timeline end).
	// ClearInstance drops the timeline, the facing and the instance hit set,
	// so the interrupted instance can never land its pending damage.
	ClearInstance();
	UE_LOG(LogTemp, Verbose, TEXT("UEMMO UCombatComponent: attack cancelled (reason: %s)"), *Reason.ToString());
}

void UCombatComponent::EndHitStun()
{
	if (ActionState != ECombatActionState::HitStun)
	{
		return;
	}
	ActionState = ECombatActionState::Free;
	HitStunEndTimeSeconds = 0.0;
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
	// M1-020: torn-down instances also drop the stun deadline bookkeeping; a
	// real stun is (re)applied right after the cancel that lands here.
	HitStunEndTimeSeconds = 0.0;
	// M1-019: the instance's hit set dies with the instance (finish, chain
	// switch and reset all funnel through here), so the next instance can hit
	// the same target again.
	InstanceHitKeys.Reset();
}

uint64 UCombatComponent::GetInstigatorId() const
{
	return CachedInstigatorId;
}

void UCombatComponent::SetFeetLocationProvider(FCombatFeetLocationProvider InProvider)
{
	FeetLocationProvider = MoveTempIfPossible(InProvider);
}

void UCombatComponent::TryApplyActiveWindowHits()
{
	// Guards: no running instance, no catalog entry, no owner or no world
	// means no hits. The definition lookup repeats the one TryStartAttack
	// used, so a swapped catalog can never crash the tick path.
	if (ActionState != ECombatActionState::Attacking)
	{
		return;
	}
	const UAttackDefinition* Definition = (Catalog != nullptr) ? Catalog->Find(ActiveAttackId) : nullptr;
	if (Definition == nullptr)
	{
		return;
	}
	AActor* OwnerActor = GetOwner();
	if (OwnerActor == nullptr || OwnerActor->GetWorld() == nullptr)
	{
		return;
	}
	// Active window test through FCombatWindow::Contains (the definition's
	// window struct delegates there, one place owns the half-open rules).
	if (!Definition->ActiveWindow.Contains(CurrentFrame))
	{
		return;
	}

	// Feet origin first (interface contract section 5), then the pure
	// geometry step, then the read-only world query.
	const FVector FeetLocation = ResolveFeetLocation();
	const FCombatHitBox Box = ComputeHitBox(FeetLocation, Facing, *Definition);
	const TArray<TWeakObjectPtr<AActor>> Targets = QueryTargets(OwnerActor->GetWorld(), Box, OwnerActor, /*AttackerTeam*/ 0);

	// Single hit group per attack instance this card: every current attack is
	// one hit, so all keys share HitGroupId 0. Multi-hit skills later define
	// one group per sub-hit (interface contract section 5).
	constexpr int32 HitGroupId = 0;

	for (const TWeakObjectPtr<AActor>& WeakTarget : Targets)
	{
		// Stale weak references (an actor destroyed between query and use)
		// are skipped safely; the query filters destroyed actors upstream,
		// this guard is defense in depth.
		AActor* Target = WeakTarget.Get();
		if (Target == nullptr)
		{
			continue;
		}

		// Dedup key of the contract shape. Only an accepted damage adds the
		// key, so a refused hit (dead target, zero result) never blocks a
		// later instance.
		FCombatHitDedupKey Key;
		Key.InstigatorId = CachedInstigatorId;
		Key.AttackInstanceId = ActiveInstanceId;
		Key.HitGroupId = HitGroupId;
		Key.TargetId = static_cast<uint64>(Target->GetUniqueID());
		if (InstanceHitKeys.Contains(Key))
		{
			continue;
		}

		UHealthComponent* TargetHealth = Target->FindComponentByClass<UHealthComponent>();
		if (TargetHealth == nullptr)
		{
			continue;
		}

		// The design damage formula (Docs/01 section 8.2) with the current
		// zero growth attributes; light_01 then deducts exactly 10.
		const float Damage = M1_019_ComputeHitDamage(*Definition);
		const float Applied = TargetHealth->ApplyDamage(Damage);
		if (Applied <= 0.0f)
		{
			continue;
		}
		InstanceHitKeys.Add(Key);

		FCombatHit Hit;
		Hit.Instigator = OwnerActor;
		Hit.InstigatorId = CachedInstigatorId;
		Hit.AttackInstanceId = ActiveInstanceId;
		Hit.HitGroupId = HitGroupId;
		Hit.Target = Target;
		Hit.Damage = Applied;
		Hit.WorldHitLocation = Box.Center;
		Hit.AttackId = ActiveAttackId;
		Hit.HitStopSeconds = Definition->HitStopSeconds;
		Hit.StunSeconds = Definition->HitStunSeconds;
		Hit.Impulse = FVector(Facing * Definition->KnockbackSpeed, 0.0f, Definition->LaunchSpeed);

		// A target that died from this hit receives no impulse: death has
		// priority (interface contract section 4).
		if (TargetHealth->IsAlive())
		{
			ApplyHitImpulse(*Target, Hit.Impulse);
		}

		// M1-020: the accepted hit reaches the victim's combat component (when
		// it has one) before the attacker-side broadcast, so any observer of
		// OnHitConfirmed already sees the victim stunned or dead. The victim
		// entry owns the stun/death decision (death has priority there).
		if (UCombatComponent* VictimCombat = Target->FindComponentByClass<UCombatComponent>())
		{
			VictimCombat->NotifyHitReceived(Hit);
		}

		// Only now the hit exists for presentations and state tasks.
		OnHitConfirmed.Broadcast(Hit);
	}
}

FVector UCombatComponent::ResolveFeetLocation() const
{
	// The injected provider wins (tests pin an explicit feet origin).
	if (FeetLocationProvider)
	{
		return FeetLocationProvider();
	}
	const AActor* OwnerActor = GetOwner();
	if (OwnerActor == nullptr)
	{
		return FVector::ZeroVector;
	}
	// A character roots on its capsule whose center sits at the actor
	// location: the feet origin is the capsule center minus the half height
	// (interface contract section 5). This matches APrototypeCharacter, whose
	// root capsule carries the standard ACharacter alignment.
	if (const ACharacter* Character = Cast<ACharacter>(OwnerActor))
	{
		if (const UCapsuleComponent* Capsule = Character->GetCapsuleComponent())
		{
			return OwnerActor->GetActorLocation() - FVector(0.0f, 0.0f, Capsule->GetScaledCapsuleHalfHeight());
		}
	}
	// Any other owner (test actors, non-character prototypes): the actor
	// location itself is treated as the feet origin.
	return OwnerActor->GetActorLocation();
}

void UCombatComponent::ApplyHitImpulse(AActor& Target, const FVector& Impulse) const
{
	// Characters take the impulse through their movement component as a
	// velocity change (mass independent; applied on its next movement update).
	if (ACharacter* TargetCharacter = Cast<ACharacter>(&Target))
	{
		if (UCharacterMovementComponent* Movement = TargetCharacter->GetCharacterMovement())
		{
			Movement->AddImpulse(Impulse, /*bVelocityChange*/ true);
			return;
		}
	}
	// Simulating rigid bodies get a velocity-change impulse; query-only or
	// static bodies (the test actors of the temp worlds, walls) cannot move
	// and are skipped, which the task report records.
	if (UPrimitiveComponent* RootPrimitive = Cast<UPrimitiveComponent>(Target.GetRootComponent()))
	{
		if (RootPrimitive->IsAnySimulatingPhysics())
		{
			RootPrimitive->AddImpulse(Impulse, NAME_None, /*bVelChange*/ true);
		}
	}
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
