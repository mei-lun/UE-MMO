#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"
#include "Engine/DataAsset.h"
#include "UObject/SoftObjectPtr.h"

#include "RoomDefinition.generated.h"

/**
 * One wave of a room: which enemy kind to spawn, how many and where.
 * Pure data. Units are explicit: SpawnLocations are world-space positions in
 * centimeters (UE convention) and the JSON source spells them out in the field
 * name (spawn_locations_cm). Wave execution and spawning belong to the later
 * room session tasks, never to this definition.
 */
USTRUCT(BlueprintType)
struct FRoomWaveDefinition
{
	GENERATED_BODY()

	/** Enemy kind of this wave, an enemy_id of Data/enemies.json (e.g. "melee_grunt"). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Room|Wave")
	FName EnemyId;

	/** How many enemies this wave spawns; must be > 0 and match SpawnLocations.Num(). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Room|Wave")
	int32 Count = 0;

	/** Spawn positions in centimeters (world space); SpawnLocations.Num() must equal Count. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Room|Wave")
	TArray<FVector> SpawnLocations;
};

/**
 * Data-driven definition of one combat room (interface contract section 7).
 * Pure data plus single-entry validation only: wave execution, spawning,
 * run/settlement state and the URoomSessionSubsystem belong to later M2 tasks.
 *
 * Units are explicit (acceptance item of M2-005): every position and size is a
 * world-space value in centimeters (FVector, UE convention; the JSON source
 * spells the unit out in field names such as bounds_size_cm) and the player
 * yaw is in degrees (PlayerSpawnYawDegrees). MapPath is a soft object
 * reference to a UWorld map package so a room can be authored, validated and
 * transported without force-loading the map into memory.
 */
UCLASS(BlueprintType)
class URoomDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** Stable identifier of this room (e.g. "room_training_01"). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Room|Identity")
	FName RoomId;

	/** Soft reference to the room's map package (e.g. /Game/UEMMO/Maps/L_TrainingArena). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Room|Identity")
	TSoftObjectPtr<UWorld> MapPath;

	/** Player spawn position in centimeters (world space; mirrors the map's PlayerStart). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Room|Spawn")
	FVector PlayerSpawnLocation = FVector::ZeroVector;

	/** Player spawn yaw in degrees (0 faces +X, matching the un-rotated PlayerStart). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Room|Spawn")
	float PlayerSpawnYawDegrees = 0.0f;

	/** Center of the legal play volume in centimeters (world space). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Room|Bounds")
	FVector BoundsCenter = FVector::ZeroVector;

	/** Full extent of the legal play volume in centimeters (e.g. a 24m x 10m floor -> X=2400, Y=1000). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Room|Bounds")
	FVector BoundsSize = FVector::ZeroVector;

	/** Waves in fight order; at least one wave. Wave execution is a later task. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Room|Waves")
	TArray<FRoomWaveDefinition> Waves;

	/** Reward table id used by later settlement tasks (e.g. "starter"); string only in this card. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Room|Reward")
	FName RewardTableId;
};

/**
 * Validates a single room definition in isolation and returns true when legal.
 * KnownEnemyIds holds every enemy_id the current Data/enemies.json provides;
 * each wave's EnemyId must be a member of it (cross-reference validation is a
 * parameter on purpose: the room card owns the check while the enemy catalog
 * stays the single source of legal ids).
 *
 * Checked, with every problem naming its field (waves as "Waves[i].Field"):
 * RoomId non-empty, MapPath non-empty, RewardTableId non-empty, BoundsCenter
 * finite, BoundsSize finite and > 0 on every axis, at least one wave, per wave
 * EnemyId non-empty and known, Count > 0, SpawnLocations.Num() == Count, every
 * spawn location finite, inside BoundsCenter +- BoundsSize/2 and at least
 * 300 cm away from PlayerSpawnLocation (3D distance), and PlayerSpawnLocation
 * itself finite and inside bounds. On failure OutErrors joins every problem
 * with "; ". Point-in-bounds checks only run when the bounds box itself is
 * usable, so one broken field does not bury the report in cascading noise.
 * Pure check: it never loads the map, spawns actors or touches a clock.
 */
bool ValidateRoomDefinition(const URoomDefinition& Room, const TSet<FName>& KnownEnemyIds, FText& OutErrors);
