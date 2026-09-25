#include "CombatGeometry.h"

#include "AttackDefinition.h"

namespace
{
	// Facing is contractually +-1 (interface contract section 5). Any other
	// value reduces to its sign: positive counts as +1, negative as -1, and
	// 0 counts as +1 so a zeroed facing still produces the default
	// right-facing box instead of an arbitrary side.
	int32 M1_017_FacingSign(int32 Facing)
	{
		return Facing < 0 ? -1 : 1;
	}
}

FCombatHitBox ComputeHitBox(const FVector& FeetLocation, int32 Facing, const UAttackDefinition& Definition)
{
	FCombatHitBox HitBox;

	const FVector& Offset = Definition.HitOffsetFromFeet;

	// Feet-origin contract: the offset is measured from the feet location and
	// only its X component mirrors with facing; Y and Z stay identical for
	// both directions. The feet location is the Capsule center minus the
	// capsule half height, so no extra waist height is added here.
	const int32 Sign = M1_017_FacingSign(Facing);
	HitBox.Center = FeetLocation + FVector(Offset.X * static_cast<double>(Sign), Offset.Y, Offset.Z);

	// The half extent is direction independent, and the axes stay fixed in
	// world space: no model rotation compensation is applied.
	HitBox.Extent = Definition.HitHalfExtent;

	return HitBox;
}
