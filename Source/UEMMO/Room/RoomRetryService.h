#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"

#include "RoomRetryService.generated.h"

class APrototypeCharacter;
class UEnemyDefinition;
class URoomDefinition;

/**
 * M2-010: the single retry entry of a failed room run (the card's "retry
 * after the player death" flow; the M2-012 UI button is a later caller).
 *
 * RetryRoom restarts a FAILED run in one idempotent-by-construction call:
 * it destroys ONLY the enemies the failed run registered (the session's
 * run-scoped actor list - never a world scan over every Character), resets
 * the session to Idle (which drops the old run's spawner instances, their
 * death bindings and every per-run counter), revives the player through the
 * M2-004 reset entry (full HP = fresh death lifecycle, dead flag dropped,
 * back at the spawn point) and starts a fresh run with a new RunId /
 * SettlementId / Seed plus a restarted wave progression. No reward, counter
 * or bookkeeping of the old run survives; nothing is accumulated per retry,
 * so the Nth retry behaves exactly like the first.
 */
UCLASS()
class UEMMO_API URoomRetryService : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Restarts the failed run of the handed world. Returns true only when the
	 * fresh run was fully started (StartRoom + BeginWaves accepted). Refused
	 * with a diagnostic and without touching anything when: the world or the
	 * session is missing, the definitions are null, the session is not Failed
	 * (a Running/Idle/Cleared/Exiting run has no retry), or the player is
	 * null (a retry without a pawn to revive is a misuse).
	 */
	static bool RetryRoom(UWorld* World, const URoomDefinition* Definition,
		UEnemyDefinition* EnemyDef, APrototypeCharacter* Player);
};
