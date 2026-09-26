#include "RoomDefinition.h"

namespace
{
	// Uniquely named (Room*) because other translation units in this module
	// keep same-purpose helpers in their own anonymous namespaces.

	/** Minimum 3D distance in cm between the player spawn and any enemy spawn point. */
	constexpr float MinRoomPlayerEnemySpawnDistanceCm = 300.0f;

	void AddRoomProblem(TArray<FString>& Problems, FString&& Message)
	{
		Problems.Add(MoveTemp(Message));
	}

	/** Appends one problem when any component of the cm vector is not finite. */
	bool CheckRoomFinite(TArray<FString>& Problems, const TCHAR* FieldName, const FVector& Value)
	{
		if (!FMath::IsFinite(Value.X) || !FMath::IsFinite(Value.Y) || !FMath::IsFinite(Value.Z))
		{
			AddRoomProblem(Problems, FString::Printf(TEXT("%s must be finite (got %s)"),
				FieldName, *Value.ToString()));
			return false;
		}
		return true;
	}

	/**
	 * Appends problems for a non-finite BoundsCenter or a non-positive /
	 * non-finite BoundsSize axis. Returns true only when the bounds box is
	 * usable, so the point-in-bounds checks below can be gated on it (one
	 * broken bounds field must not bury the report in cascading noise).
	 */
	bool CheckRoomBoundsUsable(TArray<FString>& Problems, const URoomDefinition& Room)
	{
		bool bUsable = true;
		if (!FMath::IsFinite(Room.BoundsCenter.X) || !FMath::IsFinite(Room.BoundsCenter.Y) || !FMath::IsFinite(Room.BoundsCenter.Z))
		{
			AddRoomProblem(Problems, FString::Printf(TEXT("BoundsCenter must be finite (got %s)"),
				*Room.BoundsCenter.ToString()));
			bUsable = false;
		}
		if (!FMath::IsFinite(Room.BoundsSize.X) || !FMath::IsFinite(Room.BoundsSize.Y) || !FMath::IsFinite(Room.BoundsSize.Z) ||
			Room.BoundsSize.X <= 0.0 || Room.BoundsSize.Y <= 0.0 || Room.BoundsSize.Z <= 0.0)
		{
			AddRoomProblem(Problems, FString::Printf(TEXT("BoundsSize must be finite and > 0 on every axis (got %s)"),
				*Room.BoundsSize.ToString()));
			bUsable = false;
		}
		return bUsable;
	}

	/** Component-wise inside test against the box Center +- HalfSize (cm). */
	bool IsRoomPointInsideBounds(const FVector& Point, const FVector& Center, const FVector& HalfSize)
	{
		const FVector Delta = (Point - Center).GetAbs();
		return Delta.X <= HalfSize.X && Delta.Y <= HalfSize.Y && Delta.Z <= HalfSize.Z;
	}
}

bool ValidateRoomDefinition(const URoomDefinition& Room, const TSet<FName>& KnownEnemyIds, FText& OutErrors)
{
	TArray<FString> Problems;

	// Identity: a room without a stable id cannot be referenced by the later
	// room session subsystem or by settlement records.
	if (Room.RoomId.IsNone())
	{
		AddRoomProblem(Problems, TEXT("RoomId must be a non-empty identifier (got None)"));
	}

	// Map: the soft reference must at least name a package. Whether the map
	// package itself loads is a load-time question and stays out of this pure
	// check (the M2-005 tests pin the actual L_TrainingArena reference).
	if (Room.MapPath.IsNull())
	{
		AddRoomProblem(Problems, TEXT("MapPath must name a map package (got None)"));
	}

	// Reward: later settlement resolves this id, so it must be non-empty.
	if (Room.RewardTableId.IsNone())
	{
		AddRoomProblem(Problems, TEXT("RewardTableId must be a non-empty identifier (got None)"));
	}

	// Bounds: a finite, positive size is the precondition for every
	// inside/outside question below.
	const bool bBoundsUsable = CheckRoomBoundsUsable(Problems, Room);
	const FVector HalfSize = Room.BoundsSize * 0.5;

	// Player spawn: finite and inside the legal play volume.
	const bool bPlayerSpawnUsable = CheckRoomFinite(Problems, TEXT("PlayerSpawnLocation"), Room.PlayerSpawnLocation);
	if (bBoundsUsable && bPlayerSpawnUsable &&
		!IsRoomPointInsideBounds(Room.PlayerSpawnLocation, Room.BoundsCenter, HalfSize))
	{
		AddRoomProblem(Problems, FString::Printf(
			TEXT("PlayerSpawnLocation must be inside BoundsCenter +- BoundsSize/2 (got %s)"),
			*Room.PlayerSpawnLocation.ToString()));
	}

	// Waves: at least one wave, each with a known enemy, a positive count and
	// legal, far-enough spawn points.
	if (Room.Waves.Num() <= 0)
	{
		AddRoomProblem(Problems, FString::Printf(TEXT("Waves must hold at least one wave (got %d)"), Room.Waves.Num()));
	}
	for (int32 WaveIndex = 0; WaveIndex < Room.Waves.Num(); ++WaveIndex)
	{
		const FRoomWaveDefinition& Wave = Room.Waves[WaveIndex];
		const FString Prefix = FString::Printf(TEXT("Waves[%d]."), WaveIndex);

		// Enemy reference: the id must be one the current enemy catalog
		// (Data/enemies.json) actually defines.
		if (Wave.EnemyId.IsNone())
		{
			AddRoomProblem(Problems, Prefix + TEXT("EnemyId must be a non-empty identifier (got None)"));
		}
		else if (!KnownEnemyIds.Contains(Wave.EnemyId))
		{
			AddRoomProblem(Problems, FString::Printf(
				TEXT("%sEnemyId '%s' is not a known enemy_id of Data/enemies.json"),
				*Prefix, *Wave.EnemyId.ToString()));
		}

		// Count: a wave must spawn something, and the location list must agree
		// with the count so later tasks never guess which entries to use.
		if (Wave.Count <= 0)
		{
			AddRoomProblem(Problems, FString::Printf(TEXT("%sCount must be > 0 (got %d)"), *Prefix, Wave.Count));
		}
		else if (Wave.SpawnLocations.Num() != Wave.Count)
		{
			AddRoomProblem(Problems, FString::Printf(
				TEXT("%sSpawnLocations must hold exactly Count entries (Count=%d, got %d)"),
				*Prefix, Wave.Count, Wave.SpawnLocations.Num()));
		}

		// Spawn points: finite, inside the room volume, and never on top of
		// the player spawn (3D distance in cm).
		for (int32 LocationIndex = 0; LocationIndex < Wave.SpawnLocations.Num(); ++LocationIndex)
		{
			const FVector Location = Wave.SpawnLocations[LocationIndex];
			const FString LocationField = FString::Printf(TEXT("%sSpawnLocations[%d]"), *Prefix, LocationIndex);
			if (!CheckRoomFinite(Problems, *LocationField, Location))
			{
				continue;
			}
			if (bBoundsUsable && !IsRoomPointInsideBounds(Location, Room.BoundsCenter, HalfSize))
			{
				AddRoomProblem(Problems, FString::Printf(
					TEXT("%s must be inside BoundsCenter +- BoundsSize/2 (got %s)"),
					*LocationField, *Location.ToString()));
			}
			if (bPlayerSpawnUsable)
			{
				const double DistanceCm = FVector::Dist(Room.PlayerSpawnLocation, Location);
				if (DistanceCm < MinRoomPlayerEnemySpawnDistanceCm)
				{
					AddRoomProblem(Problems, FString::Printf(
						TEXT("%s must be at least %.0f cm from PlayerSpawnLocation (got %.1f cm)"),
						*LocationField, MinRoomPlayerEnemySpawnDistanceCm, DistanceCm));
				}
			}
		}
	}

	OutErrors = FText::FromString(FString::Join(Problems, TEXT("; ")));
	return Problems.Num() == 0;
}
