#pragma once

#include "CoreMinimal.h"

class AActor;
struct FCombatHitBox;

/**
 * M1-018: world-space 3D target query for one attack hit box.
 *
 * Queries the real World for actors whose collision shapes overlap the
 * world-axis-aligned box (X horizontal, Y depth, Z up; the box rotation is
 * the unit quaternion, so FCombatHitBox::Extent is used as the box half
 * extent as-is). The query is read-only: it never applies damage (damage
 * application belongs to M1-019).
 *
 * Returned actors are deduplicated by identity: an actor with several
 * overlapping collision components is reported exactly once, even when every
 * one of its components overlaps the box. The result is filtered:
 * - the Attacker itself is never returned,
 * - actors without a UHealthComponent are never targets (walls and static
 *   meshes are not enemies),
 * - dead actors (UHealthComponent::IsAlive() == false) are never targets.
 *
 * AttackerTeam is a placeholder for future same-team filtering (interface
 * contract section 5 leaves team semantics to a later task): this card does
 * not yet exclude same-team actors because no team source exists on actors.
 * The parameter is accepted now so the call sites do not change later.
 *
 * Returns an empty array for a null World or a degenerate box (non-finite or
 * non-positive extent component); it never reports targets from a partially
 * valid query.
 */
TArray<TWeakObjectPtr<AActor>> QueryTargets(UWorld* World, const FCombatHitBox& Box, const AActor* Attacker, int32 AttackerTeam);
