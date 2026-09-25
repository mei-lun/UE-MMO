#pragma once

#include "CoreMinimal.h"

class UAttackDefinition;

/**
 * Debug data for one world-space attack hit box: the AABB center and its
 * half extent, both expressed in fixed world axes (X horizontal, Y depth,
 * Z up). No character-model rotation is folded in; the stored values are the
 * final world-space box. Pure geometry only: overlap queries against actors
 * belong to the target query task (M1-018), not to this struct.
 */
struct FCombatHitBox
{
	/** World-space center of the hit box, measured from the feet origin. */
	FVector Center = FVector::ZeroVector;

	/** Half extent in cm, copied from the attack definition (never mirrored). */
	FVector Extent = FVector::ZeroVector;
};

/**
 * Computes the world-space hit box of one attack from the character's feet
 * location (interface contract section 5).
 *
 * Feet origin: Center = FeetLocation + HitOffsetFromFeet with the offset's X
 * component mirrored by Facing; Y and Z are identical for both directions.
 * The feet location is the Capsule center minus the capsule half height, so
 * callers must not add a waist height on top. Extent = HitHalfExtent as-is
 * (direction independent). Query axes stay fixed in world X/Y/Z, so the
 * character's -90 degree mesh calibration does not affect the box.
 *
 * Facing is contractually +-1; any other value reduces to its sign
 * (positive -> +1, zero or negative -> the sign below, 0 counts as +1) so a
 * zeroed facing still produces the default right-facing box.
 *
 * Pure function: it never reads the World and never runs collision queries.
 */
FCombatHitBox ComputeHitBox(const FVector& FeetLocation, int32 Facing, const UAttackDefinition& Definition);
