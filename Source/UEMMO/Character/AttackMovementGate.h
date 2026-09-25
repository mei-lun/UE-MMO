#pragma once

#include "CoreMinimal.h"

#include "../Combat/CombatComponent.h"

// M1-013: input-side combat gating for planar movement and facing, evaluated
// at the single M1-029 movement entry point (ApplyPlanarMovement). The gate
// only scales or stops the character's own active input; the
// CharacterMovementComponent is never disabled, so external impulses
// (LaunchCharacter, knockback) keep working during an attack.

namespace UE::UEMMO::Tasks::M1_013
{
    /**
     * Allowed scale (0..1) on the active planar input speed for one combatant.
     * Free and alive keeps the original M1-029 per-axis speed (scale 1);
     * the whole attack refuses active X/Y walking (scale 0, first version);
     * death has the highest priority (scale 0). A missing component is not
     * gated (scale 1), preserving the M0 behavior when no combat source exists.
     */
    inline float ComputeAllowedMoveScale(const UCombatComponent* Combat)
    {
        // CanAcceptMovement already encodes the priority: only Free and alive
        // accepts active movement; an in-flight attack and death both scale
        // the active input to zero. A missing component is not gated (allow).
        return (Combat == nullptr || Combat->CanAcceptMovement()) ? 1.0f : 0.0f;
    }

    /**
     * Whether horizontal input may flip the facing right now. Facing locks for
     * the whole attack and while dead; a missing component is not gated.
     */
    inline bool CanFlipFacing(const UCombatComponent* Combat)
    {
        // Same rule as the movement gate (CanTurn): facing locks for the whole
        // attack and while dead. A missing component is not gated.
        return Combat == nullptr || Combat->CanTurn();
    }
}
