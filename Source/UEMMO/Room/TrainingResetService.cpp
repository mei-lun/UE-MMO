#include "TrainingResetService.h"

#include "../Enemy/TrainingEnemy.h"
#include "../PrototypeCharacter.h"
#include "../Combat/CombatComponent.h"

void UTrainingResetService::RegisterPlayer(APrototypeCharacter* InPlayer)
{
	Player = InPlayer;
}

void UTrainingResetService::RegisterEnemy(ATrainingEnemy* InEnemy)
{
	if (InEnemy == nullptr)
	{
		return;
	}
	PruneStaleEnemies();
	// Idempotent registration: the same actor never enters the set twice, so
	// repeated room setup cannot duplicate a participant (M1-027 acceptance:
	// repeated resets never add enemies).
	if (!Enemies.Contains(TWeakObjectPtr<ATrainingEnemy>(InEnemy)))
	{
		Enemies.Add(InEnemy);
	}
}

int32 UTrainingResetService::GetRegisteredEnemyCount() const
{
	PruneStaleEnemies();
	return Enemies.Num();
}

bool UTrainingResetService::HasRegisteredPlayer() const
{
	return Player.IsValid();
}

void UTrainingResetService::ResetTrainingSession()
{
	// ---- Phase 1: stop actions and timers everywhere first. ----
	// UCombatComponent::ResetCombat is the documented teardown entry: it
	// cancels the running attack (no Finished event - a reset is not an
	// attack ending), leaves HitStun / Knockdown / Recovering, drops the
	// instance hit set, clears the buffered combat input and zeroes every
	// combat deadline (stun end, knockdown end, recovering end) plus the
	// action clock and the attacker-side aerial follow-up records. It
	// deliberately leaves the dead flag and the instance id counter alone -
	// the dead flag is phase 2 enemy work, the id counter must never rewind
	// (M1-011: ids stay unique per session).
	if (APrototypeCharacter* PlayerActor = Player.Get())
	{
		if (UCombatComponent* Combat = PlayerActor->GetCombat())
		{
			Combat->ResetCombat();
		}
	}
	PruneStaleEnemies();
	for (const TWeakObjectPtr<ATrainingEnemy>& Entry : Enemies)
	{
		if (ATrainingEnemy* Enemy = Entry.Get())
		{
			if (UCombatComponent* Combat = Enemy->GetCombatComponent())
			{
				Combat->ResetCombat();
			}
		}
	}

	// ---- Phase 2: restore physics and values. ----
	// Player: spawn position, zero velocity, spawn facing, re-grounded
	// movement (the physics half is the pawn's own ApplyTrainingRoomReset, so
	// the service path and the M0 fallback share one implementation). The
	// player carries no UHealthComponent (M1-016 ships the health pool for
	// enemies only), so the HP step applies to the registered enemies.
	if (APrototypeCharacter* PlayerActor = Player.Get())
	{
		PlayerActor->ApplyTrainingRoomReset();
	}
	for (const TWeakObjectPtr<ATrainingEnemy>& Entry : Enemies)
	{
		if (ATrainingEnemy* Enemy = Entry.Get())
		{
			// ResetEnemy is the enemy-side value entry: full HP (opens a new
			// death lifecycle), dead flag dropped, combat torn down again
			// (idempotent second pass - phase 1 already stopped the actions),
			// anchor transform, zero velocity with pending launch/impulse
			// forces cleared, and the airborne bookkeeping reset (air combo
			// count, launcher float cycle, launched-airborne marker).
			Enemy->ResetEnemy();
		}
	}

	++ResetCount;
}

void UTrainingResetService::PruneStaleEnemies() const
{
	for (int32 Index = Enemies.Num() - 1; Index >= 0; --Index)
	{
		if (!Enemies[Index].IsValid())
		{
			Enemies.RemoveAt(Index);
		}
	}
}
