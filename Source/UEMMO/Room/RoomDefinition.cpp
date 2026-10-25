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

// -- M5-071: finite scene rules (schema 2) ------------------------------------

namespace
{
	/** The one defined region of this stage: the ARoomTrigger activation box (M5-073 wires it). */
	const FName M5_071_ActivationRegionId(TEXT("activation"));
}

bool ValidateRoomRules(const URoomDefinition& Room, FText& OutErrors)
{
	TArray<FString> Problems;
	const FString RoomPrefix = FString::Printf(TEXT("room '%s'"), *Room.RoomId.ToString());

	// Per-rule checks; every message carries the rule index, the rule id and
	// the offending field so the report reads room/rule/field.
	TSet<FName> SeenRuleIds;
	bool bHasStartWaveRule = false;
	bool bHasEntryRule = false;
	for (int32 RuleIndex = 0; RuleIndex < Room.Rules.Num(); ++RuleIndex)
	{
		const FRoomSceneRule& Rule = Room.Rules[RuleIndex];
		const FString Prefix = FString::Printf(TEXT("%s Rules[%d] '%s'"),
			*RoomPrefix, RuleIndex, *Rule.RuleId.ToString());

		if (Rule.RuleId.IsNone())
		{
			AddRoomProblem(Problems, Prefix + TEXT(".RuleId must be a non-empty identifier (got None)"));
		}
		else if (SeenRuleIds.Contains(Rule.RuleId))
		{
			AddRoomProblem(Problems, Prefix + FString::Printf(
				TEXT(".RuleId duplicates an earlier rule ('%s' used twice)"), *Rule.RuleId.ToString()));
		}
		else
		{
			SeenRuleIds.Add(Rule.RuleId);
		}

		// Event field pairing.
		if (Rule.Event == ERoomRuleEvent::PlayerEnteredRegion)
		{
			bHasEntryRule = true;
			if (Rule.RegionId != M5_071_ActivationRegionId)
			{
				AddRoomProblem(Problems, Prefix + FString::Printf(
					TEXT(".RegionId '%s' is not a defined region of this room (defined: activation)"),
					*Rule.RegionId.ToString()));
			}
			if (Rule.WaveIndex != -1)
			{
				AddRoomProblem(Problems, Prefix + TEXT(".WaveIndex must stay unset for PlayerEnteredRegion (got ") + FString::FromInt(Rule.WaveIndex) + TEXT(")"));
			}
		}
		else if (Rule.Event == ERoomRuleEvent::WaveCleared)
		{
			if (!Room.Waves.IsValidIndex(Rule.WaveIndex))
			{
				AddRoomProblem(Problems, FString::Printf(
					TEXT("%s.WaveIndex %d is out of range (the room carries %d waves)"),
					*Prefix, Rule.WaveIndex, Room.Waves.Num()));
			}
			if (!Rule.RegionId.IsNone())
			{
				AddRoomProblem(Problems, Prefix + TEXT(".RegionId must stay unset for WaveCleared"));
			}
		}
		else if (Rule.Event == ERoomRuleEvent::PlayerDied)
		{
			if (!Rule.RegionId.IsNone())
			{
				AddRoomProblem(Problems, Prefix + TEXT(".RegionId must stay unset for PlayerDied"));
			}
			if (Rule.WaveIndex != -1)
			{
				AddRoomProblem(Problems, Prefix + TEXT(".WaveIndex must stay unset for PlayerDied"));
			}
		}

		// Effect field pairing + delay bounds.
		if (Rule.Effect == ERoomRuleEffect::StartWave)
		{
			bHasStartWaveRule = true;
			if (!Room.Waves.IsValidIndex(Rule.NextWaveIndex))
			{
				AddRoomProblem(Problems, FString::Printf(
					TEXT("%s.NextWaveIndex %d is out of range (the room carries %d waves)"),
					*Prefix, Rule.NextWaveIndex, Room.Waves.Num()));
			}
		}
		else if (Rule.NextWaveIndex != -1)
		{
			AddRoomProblem(Problems, Prefix + TEXT(".NextWaveIndex is only valid for the StartWave effect"));
		}
		if (!FMath::IsFinite(Rule.DelaySeconds) || Rule.DelaySeconds < 0.0f)
		{
			AddRoomProblem(Problems, FString::Printf(
				TEXT("%s.DelaySeconds must be finite and >= 0 (got %f)"), *Prefix, Rule.DelaySeconds));
		}
	}

	// Graph shape: only meaningful when the room actually starts waves.
	if (bHasStartWaveRule && !bHasEntryRule)
	{
		AddRoomProblem(Problems, RoomPrefix
			+ TEXT(" starts waves through its rules but carries no PlayerEnteredRegion entry rule"));
	}

	// Walk every chain from the entry rules: started indices must strictly
	// increase (no cycles, no same-wave restart), every started wave needs its
	// WaveCleared continuation, and at least one chain must reach MarkCleared.
	if (bHasEntryRule)
	{
		bool bAnyTerminalReached = false;
		for (const FRoomSceneRule& Rule : Room.Rules)
		{
			if (Rule.Event != ERoomRuleEvent::PlayerEnteredRegion
				|| Rule.Effect != ERoomRuleEffect::StartWave
				|| !Room.Waves.IsValidIndex(Rule.NextWaveIndex))
			{
				continue;
			}
			int32 StartedWave = Rule.NextWaveIndex;
			TSet<int32> VisitedWaves;
			const FString EntryPrefix = FString::Printf(TEXT("%s Rules '%s'"),
				*RoomPrefix, *Rule.RuleId.ToString());
			for (;;)
			{
				if (VisitedWaves.Contains(StartedWave))
				{
					AddRoomProblem(Problems, FString::Printf(
						TEXT("%s chains into a rule cycle at wave %d"), *EntryPrefix, StartedWave));
					break;
				}
				VisitedWaves.Add(StartedWave);
				const FRoomSceneRule* Continuation = nullptr;
				bool bChainTerminal = false;
				for (const FRoomSceneRule& Candidate : Room.Rules)
				{
					if (Candidate.Event != ERoomRuleEvent::WaveCleared
						|| Candidate.WaveIndex != StartedWave
						|| !Room.Waves.IsValidIndex(Candidate.WaveIndex))
					{
						continue;
					}
					if (Candidate.Effect == ERoomRuleEffect::MarkCleared)
					{
						bChainTerminal = true;
						bAnyTerminalReached = true;
						continue;
					}
					if (Candidate.Effect == ERoomRuleEffect::StartWave)
					{
						if (Continuation != nullptr)
						{
							AddRoomProblem(Problems, FString::Printf(
								TEXT("%s: wave %d carries more than one StartWave continuation ('%s' and '%s')"),
								*EntryPrefix, StartedWave, *Continuation->RuleId.ToString(), *Candidate.RuleId.ToString()));
							continue;
						}
						Continuation = &Candidate;
					}
				}
				if (bChainTerminal || Continuation == nullptr)
				{
					if (!bChainTerminal)
					{
						AddRoomProblem(Problems, FString::Printf(
							TEXT("%s: started wave %d has no WaveCleared continuation (no terminal path)"),
							*EntryPrefix, StartedWave));
					}
					break;
				}
				if (Continuation->NextWaveIndex <= StartedWave)
				{
					AddRoomProblem(Problems, FString::Printf(
						TEXT("%s: rule '%s' restarts an earlier or same wave (%d -> %d); started waves must strictly increase"),
						*EntryPrefix, *Continuation->RuleId.ToString(), StartedWave, Continuation->NextWaveIndex));
					break;
				}
				StartedWave = Continuation->NextWaveIndex;
			}
		}
		if (!bAnyTerminalReached)
		{
			AddRoomProblem(Problems, RoomPrefix
				+ TEXT(" carries entry rules but no chain reaches a MarkCleared terminal"));
		}
	}

	OutErrors = FText::FromString(FString::Join(Problems, TEXT("; ")));
	return Problems.Num() == 0;
}

bool BuildLegacySchemaOneDefaultRules(const TArray<FRoomWaveDefinition>& Waves, TArray<FRoomSceneRule>& OutRules)
{
	OutRules.Reset();
	if (Waves.Num() <= 0)
	{
		return false;
	}
	FRoomSceneRule Enter;
	Enter.RuleId = TEXT("enter_start_wave_0");
	Enter.Event = ERoomRuleEvent::PlayerEnteredRegion;
	Enter.RegionId = TEXT("activation");
	Enter.Effect = ERoomRuleEffect::StartWave;
	Enter.NextWaveIndex = 0;
	Enter.DelaySeconds = 0.0f;
	Enter.bOnce = true;
	OutRules.Add(Enter);

	for (int32 WaveIndex = 0; WaveIndex + 1 < Waves.Num(); ++WaveIndex)
	{
		FRoomSceneRule Advance;
		Advance.RuleId = FName(*FString::Printf(TEXT("wave_%d_start_wave_%d"), WaveIndex, WaveIndex + 1));
		Advance.Event = ERoomRuleEvent::WaveCleared;
		Advance.WaveIndex = WaveIndex;
		Advance.Effect = ERoomRuleEffect::StartWave;
		Advance.NextWaveIndex = WaveIndex + 1;
		Advance.DelaySeconds = 1.0f;
		Advance.bOnce = true;
		OutRules.Add(Advance);
	}

	FRoomSceneRule Final;
	Final.RuleId = FName(*FString::Printf(TEXT("wave_%d_clear"), Waves.Num() - 1));
	Final.Event = ERoomRuleEvent::WaveCleared;
	Final.WaveIndex = Waves.Num() - 1;
	Final.Effect = ERoomRuleEffect::MarkCleared;
	Final.bOnce = true;
	OutRules.Add(Final);

	FRoomSceneRule Death;
	Death.RuleId = TEXT("player_dead_fail");
	Death.Event = ERoomRuleEvent::PlayerDied;
	Death.Effect = ERoomRuleEffect::FailRun;
	Death.bOnce = true;
	OutRules.Add(Death);
	return true;
}

bool ValidateRoomCatalog(const TArray<const URoomDefinition*>& Rooms, FText& OutErrors)
{
	// M5-069: the one cross-row rule of the catalog - every RoomId must be
	// unique. The first occurrence is remembered by index so the error names
	// the duplicated id AND both rows (a row the asset builder could not
	// address unambiguously otherwise).
	TArray<FString> Problems;
	TMap<FName, int32> FirstRowIndex;
	for (int32 RowIndex = 0; RowIndex < Rooms.Num(); ++RowIndex)
	{
		const URoomDefinition* Room = Rooms[RowIndex];
		if (Room == nullptr)
		{
			AddRoomProblem(Problems, FString::Printf(TEXT("rooms[%d] is null"), RowIndex));
			continue;
		}
		const int32* FirstIndex = FirstRowIndex.Find(Room->RoomId);
		if (FirstIndex != nullptr)
		{
			AddRoomProblem(Problems, FString::Printf(
				TEXT("rooms[%d].RoomId '%s' duplicates rooms[%d] - room ids must be unique"),
				RowIndex, *Room->RoomId.ToString(), *FirstIndex));
		}
		else
		{
			FirstRowIndex.Add(Room->RoomId, RowIndex);
		}
	}
	OutErrors = FText::FromString(FString::Join(Problems, TEXT("; ")));
	return Problems.Num() == 0;
}
