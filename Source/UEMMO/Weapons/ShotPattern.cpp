#include "ShotPattern.h"

#include "Math/RandomStream.h"

#include "WeaponTypes.h"

FShotPatternPlan PlanShotPattern(const FShotPatternRequest& Request)
{
	FShotPatternPlan Plan;

	// The pellet count bounds mirror the weapon-definition schema (M5-004:
	// 1..MaxWeaponPelletCount); anything outside is refused before anything
	// else is examined ("pellet_count越界拒绝").
	if (Request.PelletCount < 1 || Request.PelletCount > MaxWeaponPelletCount)
	{
		Plan.Reject = EShotPatternReject::InvalidPelletCount;
		Plan.RejectDetail = FString::Printf(
			TEXT("PelletCount must be a value in 1..%d (got %d)"), MaxWeaponPelletCount, Request.PelletCount);
		return Plan;
	}

	// The spread shares the weapon-definition bound; a non-finite value would
	// poison every direction below.
	if (!FMath::IsFinite(Request.SpreadDegrees) || Request.SpreadDegrees < 0.0f
		|| Request.SpreadDegrees > MaxWeaponSpreadDegrees)
	{
		Plan.Reject = EShotPatternReject::InvalidSpread;
		Plan.RejectDetail = FString::Printf(
			TEXT("SpreadDegrees must be a finite value in 0..%f (got %s)"),
			MaxWeaponSpreadDegrees, *FString::SanitizeFloat(Request.SpreadDegrees));
		return Plan;
	}

	// The capacity transaction: the WHOLE volley must fit into the
	// pre-claimed pellet slots, or nothing plans and nothing is deducted
	// ("全体容量不足无半串/半扣弹").
	if (Request.AvailablePelletSlots < Request.PelletCount)
	{
		Plan.Reject = EShotPatternReject::CapacityInsufficient;
		Plan.RejectDetail = FString::Printf(
			TEXT("the volley needs %d pellet slots but only %d were pre-claimed"),
			Request.PelletCount, Request.AvailablePelletSlots);
		return Plan;
	}

	// The world origin: a named socket prefers the caller-resolved location
	// (the planner holds no World and never resolves sockets itself). A
	// missing socket either falls back to the feet offset - flagged and
	// naming the socket, so the problem is locatable - or, when the config
	// marks the socket required, refuses the whole plan.
	FShotOrigin Origin;
	const bool bSocketNamed = !Request.Origin.MuzzleSocketName.IsNone();
	if (bSocketNamed && Request.MuzzleSocketWorldLocation != nullptr)
	{
		Origin.bUsedSocket = true;
		Origin.WorldLocation = *Request.MuzzleSocketWorldLocation;
	}
	else if (bSocketNamed && Request.Origin.bRequireSocket)
	{
		Plan.Reject = EShotPatternReject::RequiredSocketMissing;
		Plan.RejectDetail = FString::Printf(
			TEXT("the required muzzle socket '%s' could not be resolved"),
			*Request.Origin.MuzzleSocketName.ToString());
		return Plan;
	}
	else
	{
		// Feet-relative offset: X is forward (mirrored by the facing), Y is
		// depth and Z is height above the feet - the Y depth offset passes
		// through unchanged at both facings ("不锁Y").
		Origin.WorldLocation = Request.FeetWorldLocation + FVector(
			static_cast<double>(Request.Facing) * Request.Origin.FeetOffsetCm.X,
			Request.Origin.FeetOffsetCm.Y,
			Request.Origin.FeetOffsetCm.Z);
		if (bSocketNamed)
		{
			Origin.bSocketFellBack = true;
			Origin.FallbackSocketName = Request.Origin.MuzzleSocketName;
		}
	}
	Plan.Origin = Origin;

	// The deterministic volley: a local stream seeded by the shot-proprietary
	// seed - the planner never touches any shared stream, so back-to-back
	// replays of the same seed are byte-identical ("散布使用Shot专有seed").
	FRandomStream Stream(Request.Seed);
	Plan.Pellets.Reserve(Request.PelletCount);
	for (int32 Index = 0; Index < Request.PelletCount; ++Index)
	{
		FShotPellet Pellet;
		Pellet.PelletIndex = static_cast<FPelletIndex>(Index);

		if (Request.SpreadDegrees <= 0.0f)
		{
			// Zero spread is the exact straight line along the facing - no
			// depth, no height, and the stream is left untouched ("散布0为直线").
			Pellet.Direction = FVector(static_cast<double>(Request.Facing), 0.0, 0.0);
		}
		else
		{
			// A uniform point on the spread disc: the radius is the angular
			// deviation from the baseline (dense toward the center), the
			// angle walks the Y (depth) / Z (height) plane. The facing lives
			// ONLY in the baseline vector, so mirroring flips X and never
			// touches the Y/Z components ("Facing只镜像X，不锁Y").
			const double RadiusRad = FMath::DegreesToRadians(
				static_cast<double>(Request.SpreadDegrees) * FMath::Sqrt(Stream.GetFraction()));
			const double AngleRad = 2.0 * PI * Stream.GetFraction();
			const FVector Baseline(static_cast<double>(Request.Facing), 0.0, 0.0);
			const FVector OffsetDir(0.0, FMath::Cos(AngleRad), FMath::Sin(AngleRad));
			Pellet.Direction = (Baseline * FMath::Cos(RadiusRad) + OffsetDir * FMath::Sin(RadiusRad)).GetSafeNormal();
		}
		Plan.Pellets.Add(Pellet);
	}

	// One trigger, one round: the whole volley is the unit of consumption
	// ("单次霰弹耗一发") - the deduction belongs to the caller's transaction,
	// the plan only states it.
	Plan.bPlanned = true;
	Plan.RoundsConsumed = 1;
	return Plan;
}
