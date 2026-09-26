#include "MeleeEnemyController.h"

#include "../Combat/CombatComponent.h"
#include "../Enemy/EnemyDefinition.h"
#include "MeleeEnemy.h"

#include "EngineUtils.h"

AMeleeEnemyController::AMeleeEnemyController()
{
	// The Tick-driven state machine needs no tick configuration beyond the
	// AAIController default (its primary actor tick is already enabled).
	PrimaryActorTick.bCanEverTick = true;
}

void AMeleeEnemyController::SetTarget(AActor* InTarget)
{
	// Weak reference: the controller never keeps the target alive and never
	// dereferences it after it is destroyed or garbage collected.
	TargetWeak = InTarget;
}

void AMeleeEnemyController::SetCombatParameters(float InAttackRangeX, float InAlignYTolerance)
{
	AttackRangeX = InAttackRangeX;
	AlignYTolerance = InAlignYTolerance;
}

void AMeleeEnemyController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	AMeleeEnemy* Enemy = Cast<AMeleeEnemy>(GetPawn());
	if (Enemy == nullptr)
	{
		// Unpossessed or wrong pawn kind: nothing to drive. (No pawn means no
		// mesh to restore, so only the bookkeeping flag is cleared.)
		bTelegraphVisualActive = false;
		State = EMeleeEnemyState::Idle;
		LastMoveIntent = FVector::ZeroVector;
		return;
	}

	// Effective combat parameters: contract defaults, overridable per
	// controller, with the pawn's M2-001 definition winning when one was
	// applied (the definition is the data-driven source of truth). M2-003
	// adds the wind-up duration and the catalog attack id the attack fires
	// with; without a definition (or an empty id) the enemy never attacks.
	float EffectiveAttackRangeX = AttackRangeX;
	float EffectiveAlignYTolerance = AlignYTolerance;
	float EffectiveTelegraphSeconds = DefaultTelegraphSeconds;
	FName EffectiveAttackId = NAME_None;
	if (const UEnemyDefinition* Definition = Enemy->GetEnemyDefinition())
	{
		EffectiveAttackRangeX = Definition->AttackRangeX;
		EffectiveAlignYTolerance = Definition->AlignYTolerance;
		EffectiveTelegraphSeconds = Definition->TelegraphSeconds;
		EffectiveAttackId = Definition->MeleeAttackId;
	}

	// M2-003: explicit game-clock injection for the Telegraph/Recover
	// countdown, once per tick BEFORE the state machine reads it (the M1
	// per-frame injection pattern). The World GetTimeSeconds clock advances
	// only with real game-time ticks (a manually ticked test world advances
	// it by the injected delta), so no wall clock is ever read.
	if (UWorld* World = GetWorld())
	{
		InjectTelegraphClockSeconds(World->GetTimeSeconds());
	}
	const double NowSeconds = TelegraphClockSeconds;

	// Self combat state first: a dead enemy never keeps a wind-up running.
	UCombatComponent* SelfCombat = Enemy->GetCombatComponent();
	if (SelfCombat != nullptr && SelfCombat->IsDead())
	{
		CancelTimedState(*Enemy, NowSeconds);
		return;
	}

	// Target resolution: a destroyed or dead target drops the brain to Idle
	// (ResolveTarget never dereferences a stale weak reference).
	FVector TargetLocation = FVector::ZeroVector;
	if (!ResolveTarget(TargetLocation))
	{
		CancelTimedState(*Enemy, NowSeconds);
		return;
	}

	const ECombatActionState SelfAction = (SelfCombat != nullptr)
		? SelfCombat->GetSnapshot().ActionState
		: ECombatActionState::Free;
	const bool bSelfCombatBusy = SelfAction != ECombatActionState::Free
		&& SelfAction != ECombatActionState::Attacking;
	const bool bSelfFree = SelfAction == ECombatActionState::Free;

	// ---------------- M2-003 timed state machine ----------------
	// Telegraph / Attack / Recover hold the position and the locked facing:
	// no approach input, no separation input, no facing update while one of
	// them runs. The approach loop below only resumes from Free states.
	switch (State)
	{
	case EMeleeEnemyState::Telegraph:
		TickTelegraphState(*Enemy, NowSeconds, bSelfCombatBusy, EffectiveAttackId);
		return;
	case EMeleeEnemyState::Attack:
		TickAttackState(NowSeconds, bSelfCombatBusy, bSelfFree);
		return;
	case EMeleeEnemyState::Recover:
		TickRecoverState(NowSeconds, bSelfCombatBusy);
		return;
	case EMeleeEnemyState::Idle:
	case EMeleeEnemyState::Approach:
	default:
		break;
	}

	if (State != EMeleeEnemyState::Approach)
	{
		EnterState(EMeleeEnemyState::Approach, NowSeconds);
	}

	// While the enemy's own combat component is busy (M1-020 hit stun, M1-026
	// knockdown/recovery) the brain freezes in place: the component refuses
	// movement, so chasing or starting a wind-up now would fight the stun.
	if (bSelfCombatBusy)
	{
		LastMoveIntent = FVector::ZeroVector;
		return;
	}

	// The approach rule (depth first, then X), filtered by the configured
	// room bound, is the only chase movement; it goes through
	// AddMovementInput so the normal CharacterMovement handles acceleration,
	// braking and real collision.
	const FVector EnemyLocation = Enemy->GetActorLocation();
	FVector Intent = ComputeMeleeApproachIntent(EnemyLocation, TargetLocation, EffectiveAttackRangeX, EffectiveAlignYTolerance);
	Intent = FilterMeleeIntentByRoomBounds(EnemyLocation, Intent, RoomBounds);
	LastMoveIntent = Intent;

	if (!Intent.IsNearlyZero())
	{
		Enemy->AddMovementInput(Intent, 1.0f);
	}

	// Separation: keep at least MinSeparation from every other melee enemy
	// so up to three chasers never stack on one point. The correction is a
	// positional shortfall (cm); it feeds the same movement input scaled
	// against the minimum, and is bound-filtered like the approach intent so
	// the push never steers across the field edge.
	const FVector Separation = FilterMeleeIntentByRoomBounds(
		EnemyLocation,
		ComputeMeleeSeparationAdjustment(EnemyLocation, CollectOtherMeleeEnemyLocations(Enemy), MinSeparation),
		RoomBounds);
	if (!Separation.IsNearlyZero())
	{
		const float Scale = FMath::Clamp(Separation.Size() / FMath::Max(MinSeparation, 1.0f), 0.0f, 1.0f);
		Enemy->AddMovementInput(Separation.GetSafeNormal(), Scale);
	}

	// Facing: only the +/-X approach axis turns the enemy (the pure rule in
	// AMeleeEnemy::ApplyFacingIntent); a pure depth or zero intent keeps the
	// current yaw, so the enemy never faces the camera direction.
	Enemy->ApplyFacingIntent(Intent);

	// Attack condition met: lock the attack direction from the target's
	// current relative position, arm the wind-up deadline and enter the
	// Telegraph state (the attack fires only when the deadline elapses; a
	// target that walks away meanwhile is simply missed by the real 3D hit
	// query). Without a definition attack id nothing ever fires.
	if (EffectiveAttackId != NAME_None
		&& ComputeMeleeAttackReady(EnemyLocation, TargetLocation, EffectiveAttackRangeX, EffectiveAlignYTolerance))
	{
		LockedFacing = (TargetLocation.X >= EnemyLocation.X) ? 1 : -1;
		TelegraphDeadlineSeconds = NowSeconds + EffectiveTelegraphSeconds;
		SetTelegraphVisual(*Enemy, true);
		EnterState(EMeleeEnemyState::Telegraph, NowSeconds);
	}
}

void AMeleeEnemyController::TickTelegraphState(AMeleeEnemy& Enemy, double NowSeconds, bool bSelfCombatBusy, FName EffectiveAttackId)
{
	// The wind-up holds the locked position and facing: no approach input,
	// no separation push, no facing update - the attack direction was fixed
	// at entry and the position judgment is deliberately frozen so a target
	// that steps away gets missed by the real hit query.
	LastMoveIntent = FVector::ZeroVector;

	if (bSelfCombatBusy)
	{
		// An accepted hit (M1-020) or a knockdown (M1-026) interrupted the
		// wind-up: cancel it immediately - the deadline timer is dropped, so
		// no delayed attack can ever fire from an interrupted telegraph.
		SetTelegraphVisual(Enemy, false);
		EnterState(EMeleeEnemyState::Approach, NowSeconds);
		return;
	}

	if (NowSeconds < TelegraphDeadlineSeconds)
	{
		return;
	}

	// Wind-up complete: fire the attack through the enemy's own combat
	// component (the shared M1 pipeline settles the damage; this controller
	// never touches a victim's health directly).
	SetTelegraphVisual(Enemy, false);
	UCombatComponent* Combat = Enemy.GetCombatComponent();
	if (Combat != nullptr && Combat->TryStartAttack(EffectiveAttackId, LockedFacing))
	{
		EnterState(EMeleeEnemyState::Attack, NowSeconds);
		return;
	}

	// Refused (a dead or stunned race): no attack, resume approaching.
	EnterState(EMeleeEnemyState::Approach, NowSeconds);
}

void AMeleeEnemyController::TickAttackState(double NowSeconds, bool bSelfCombatBusy, bool bSelfFree)
{
	// The attack instance runs on the combat component: hold position and
	// the locked facing while it is in flight.
	LastMoveIntent = FVector::ZeroVector;

	if (bSelfCombatBusy)
	{
		// The instance was interrupted (an accepted hit cancels it without a
		// Finished broadcast): no recovery rest for an unfinished attack.
		EnterState(EMeleeEnemyState::Approach, NowSeconds);
		return;
	}

	if (bSelfFree)
	{
		// The instance finished (Finished broadcast / natural end): rest.
		RecoverEndTimeSeconds = NowSeconds + RecoverSeconds;
		EnterState(EMeleeEnemyState::Recover, NowSeconds);
	}
}

void AMeleeEnemyController::TickRecoverState(double NowSeconds, bool bSelfCombatBusy)
{
	// The rest holds the position (the post-attack recovery stance).
	LastMoveIntent = FVector::ZeroVector;

	if (bSelfCombatBusy)
	{
		// A hit during the rest interrupts it (death/loss of target are
		// handled before the state machine).
		EnterState(EMeleeEnemyState::Approach, NowSeconds);
		return;
	}

	if (NowSeconds >= RecoverEndTimeSeconds)
	{
		// Recovery over: back to the approach loop (Free). A still-satisfied
		// attack condition starts the next wind-up from here.
		EnterState(EMeleeEnemyState::Approach, NowSeconds);
	}
}

void AMeleeEnemyController::EnterState(EMeleeEnemyState NewState, double NowSeconds)
{
	State = NewState;
	StateEnteredSeconds = NowSeconds;
}

void AMeleeEnemyController::CancelTimedState(AMeleeEnemy& Enemy, double NowSeconds)
{
	// Any timed state (Telegraph wind-up, Attack wait, Recover rest)
	// collapses to Idle when the self is dead or the target is gone: the
	// wind-up timer is dropped (no delayed attack can ever fire from it) and
	// the telegraph presentation is restored.
	SetTelegraphVisual(Enemy, false);
	TelegraphDeadlineSeconds = 0.0;
	RecoverEndTimeSeconds = 0.0;
	EnterState(EMeleeEnemyState::Idle, NowSeconds);
	LastMoveIntent = FVector::ZeroVector;
}

void AMeleeEnemyController::SetTelegraphVisual(AMeleeEnemy& Enemy, bool bActive)
{
	// Deduped: repeated ticks in the same state never re-issue the request.
	if (bTelegraphVisualActive == bActive)
	{
		return;
	}
	bTelegraphVisualActive = bActive;
	Enemy.ApplyTelegraphVisual(bActive);
}

bool AMeleeEnemyController::ResolveTarget(FVector& OutTargetLocation) const
{
	// TWeakObjectPtr::Get() returns null once the object is stale (destroyed
	// or pending kill): the dead object is never dereferenced.
	AActor* Target = TargetWeak.Get();
	if (Target == nullptr)
	{
		return false;
	}
	// A dead target stops the chase (interface contract 7: death is read
	// from the shared M1 combat component; a target without one - a generic
	// actor - counts as alive).
	const UCombatComponent* Combat = Target->FindComponentByClass<UCombatComponent>();
	if (Combat != nullptr && Combat->IsDead())
	{
		return false;
	}
	OutTargetLocation = Target->GetActorLocation();
	return true;
}

TArray<FVector> AMeleeEnemyController::CollectOtherMeleeEnemyLocations(const AMeleeEnemy* Self) const
{
	TArray<FVector> Others;
	UWorld* World = Self ? Self->GetWorld() : nullptr;
	if (World == nullptr)
	{
		return Others;
	}
	// Every other AMeleeEnemy in the world contributes its location to the
	// separation rule (the pure rule itself decides per distance). With the
	// M2 stage sizes (a handful of enemies) this per-tick query is trivial.
	for (TActorIterator<AMeleeEnemy> It(World); It; ++It)
	{
		const AMeleeEnemy* Other = *It;
		if (Other == nullptr || Other == Self)
		{
			continue;
		}
		Others.Add(Other->GetActorLocation());
	}
	return Others;
}

// ---------------------------------------------------------------------------
// Pure rule functions (declared in the header; unit-tested without a world).
// ---------------------------------------------------------------------------

FVector ComputeMeleeApproachIntent(const FVector& EnemyLocation, const FVector& TargetLocation, float AttackRangeX, float AlignYTolerance)
{
	const float DeltaX = TargetLocation.X - EnemyLocation.X;
	const float DeltaY = TargetLocation.Y - EnemyLocation.Y;

	// Depth first: while the depth offset exceeds the tolerance the enemy
	// moves ONLY along Y toward the target - no matter how large the X
	// offset is, the intent is never a diagonal (this is what keeps a side
	// camera view honest: the chaser walks the depth axis first).
	if (FMath::Abs(DeltaY) > AlignYTolerance)
	{
		return FVector(0.0f, DeltaY > 0.0f ? 1.0f : -1.0f, 0.0f);
	}

	// Depth aligned: close along X until inside the attack range band.
	if (FMath::Abs(DeltaX) > AttackRangeX)
	{
		return FVector(DeltaX > 0.0f ? 1.0f : -1.0f, 0.0f, 0.0f);
	}

	// Aligned and in range: hold position (the attack itself is M2-003).
	return FVector::ZeroVector;
}

bool ComputeMeleeAttackReady(const FVector& EnemyLocation, const FVector& TargetLocation, float AttackRangeX, float AlignYTolerance)
{
	// The attack window equals the approach hold band: boundaries count as
	// satisfied (the same geometry that stops the chase starts the wind-up).
	const float DeltaX = FMath::Abs(TargetLocation.X - EnemyLocation.X);
	const float DeltaY = FMath::Abs(TargetLocation.Y - EnemyLocation.Y);
	return DeltaX <= AttackRangeX && DeltaY <= AlignYTolerance;
}

FVector ComputeMeleeSeparationAdjustment(const FVector& SelfLocation, const TArray<FVector>& OtherEnemyLocations, float MinSeparation)
{
	FVector Total = FVector::ZeroVector;
	for (const FVector& Other : OtherEnemyLocations)
	{
		// Horizontal (XY) rule: melee enemies share the ground plane, so the
		// vertical offset never contributes to the separation.
		FVector Delta(SelfLocation.X - Other.X, SelfLocation.Y - Other.Y, 0.0f);
		const float Distance = Delta.Size();
		if (Distance >= MinSeparation)
		{
			continue;
		}
		if (Distance <= KINDA_SMALL_NUMBER)
		{
			// Exact overlap: deterministic fallback push along +X of the
			// full minimum (documented limitation: two enemies spawned at
			// the very same spot receive the same fallback and cannot
			// separate by position alone; spawn points differ in practice).
			Total += FVector(MinSeparation, 0.0f, 0.0f);
			continue;
		}
		// Push away from the neighbor by exactly the shortfall: after the
		// correction the pair sits at the minimum distance.
		Total += Delta * ((MinSeparation - Distance) / Distance);
	}
	return Total;
}

FVector FilterMeleeIntentByRoomBounds(const FVector& EnemyLocation, const FVector& MoveIntent, const FBox& RoomBounds)
{
	// An unset (invalid) bound disables the rule entirely. (UE 5.8 FBox
	// exposes validity as the public uint8 field "IsValid", not a method.)
	if (!RoomBounds.IsValid)
	{
		return MoveIntent;
	}
	// Beyond the field boundary: stop entirely (the "outside" rule).
	if (!RoomBounds.IsInsideXY(EnemyLocation))
	{
		return FVector::ZeroVector;
	}
	// Inside: zero any component that would cross a bound (1 cm lookahead on
	// the unit intent), keeping the other component so the enemy can still
	// slide along the wall line.
	FVector Filtered = MoveIntent;
	if (Filtered.X > 0.0f && EnemyLocation.X + Filtered.X > RoomBounds.Max.X)
	{
		Filtered.X = 0.0f;
	}
	if (Filtered.X < 0.0f && EnemyLocation.X + Filtered.X < RoomBounds.Min.X)
	{
		Filtered.X = 0.0f;
	}
	if (Filtered.Y > 0.0f && EnemyLocation.Y + Filtered.Y > RoomBounds.Max.Y)
	{
		Filtered.Y = 0.0f;
	}
	if (Filtered.Y < 0.0f && EnemyLocation.Y + Filtered.Y < RoomBounds.Min.Y)
	{
		Filtered.Y = 0.0f;
	}
	return Filtered;
}

float ComputeMeleeFacingYaw(const FVector& MoveIntent, float CurrentYaw)
{
	// Only an X intent component turns the enemy: yaw 0 faces +X, yaw 180
	// faces -X. A pure depth (Y) or zero intent never turns the enemy, so a
	// side-camera chaser never ends up facing the lens or the raw diagonal.
	if (MoveIntent.X > KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}
	if (MoveIntent.X < -KINDA_SMALL_NUMBER)
	{
		return 180.0f;
	}
	return CurrentYaw;
}
