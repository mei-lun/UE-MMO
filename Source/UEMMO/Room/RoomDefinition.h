#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"
#include "Engine/DataAsset.h"
#include "UObject/SoftObjectPtr.h"

#include "RoomDefinition.generated.h"

/**
 * One wave of a room: which enemy kind to spawn, how many and where.
 * Pure data. Units are explicit: SpawnLocations are world-space positions in
 * centimeters (UE convention) and the JSON source spells them out in the
 * field name (spawn_locations_cm). Wave execution and spawning belong to the later
 * room session tasks, never to this definition.
 */
USTRUCT(BlueprintType)
struct FRoomWaveDefinition
{
	GENERATED_BODY()

	/** Enemy kind of this wave, an enemy_id of Data/enemies.json (e.g. "melee_grunt"). */
	// EditAnywhere (M5-070): the wave structs are authored as instances inside
	// the room data assets by the editor generation script; DefaultsOnly made
	// the Python property writes reject struct instances.
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room|Wave")
	FName EnemyId;

	/** How many enemies this wave spawns; must be > 0 and match SpawnLocations.Num(). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room|Wave")
	int32 Count = 0;

	/** Spawn positions in centimeters (world space); SpawnLocations.Num() must equal Count. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room|Wave")
	TArray<FVector> SpawnLocations;
};

/**
 * M5-071: the finite scene-rule event whitelist. PlayerEnteredRegion fires
 * when the player enters the room's activation region (the ARoomTrigger box,
 * M5-073); WaveCleared fires when a wave is fully born and fully dead (the
 * session's progression facts); PlayerDied fires from the confirmed player
 * death lifecycle. Nothing else may become a scene event (no per-hit events).
 */
UENUM(BlueprintType)
enum class ERoomRuleEvent : uint8
{
	PlayerEnteredRegion,
	WaveCleared,
	PlayerDied
};

/**
 * M5-071: the finite scene-rule effect whitelist. StartWave requests one
 * wave start (NextWaveIndex) after DelaySeconds on the injected session
 * clock; MarkCleared / FailRun request the terminal transitions (the session
 * applies first-terminal-wins, never the rule layer). No spawning, damage,
 * reward or exit effects exist on purpose.
 */
UENUM(BlueprintType)
enum class ERoomRuleEffect : uint8
{
	StartWave,
	MarkCleared,
	FailRun
};

/**
 * M5-071: one finite scene rule of a room (schema 2 rows of Data/rooms.json).
 * Pure data - the M5-072 evaluator owns execution, this struct owns nothing.
 * RegionId is only meaningful for PlayerEnteredRegion (the one defined
 * region of this stage is "activation", the trigger volume); WaveIndex is
 * only meaningful for WaveCleared; NextWaveIndex is only meaningful for
 * StartWave. Unused members stay at their none/0 defaults; -1 is the "no
 * index" sentinel of both index fields. DelaySeconds rides the injected
 * session clock (finite, >= 0; the 1.0 s inter-wave gap of the default
 * battle); bOnce marks one-shot rules (the dedup belongs to the evaluator).
 */
USTRUCT(BlueprintType)
struct FRoomSceneRule
{
	GENERATED_BODY()

	/** Stable identifier of this rule inside its room (unique, e.g. "enter_start_wave_0"). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room|Rule")
	FName RuleId;

	/** The whitelisted event that arms the rule. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room|Rule")
	ERoomRuleEvent Event = ERoomRuleEvent::PlayerEnteredRegion;

	/** Region reference of a PlayerEnteredRegion rule ("activation"; NAME_None elsewhere). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room|Rule")
	FName RegionId;

	/** Wave reference of a WaveCleared rule (-1 = not set). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room|Rule")
	int32 WaveIndex = -1;

	/** The whitelisted effect the rule requests. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room|Rule")
	ERoomRuleEffect Effect = ERoomRuleEffect::StartWave;

	/** The wave a StartWave rule starts ([0, Waves.Num()-1]; -1 = not set). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room|Rule")
	int32 NextWaveIndex = -1;

	/** Delay before the effect, on the injected session clock (finite, >= 0). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room|Rule")
	float DelaySeconds = 0.0f;

	/** True when the rule may fire at most once per run (evaluator-side dedup). */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room|Rule")
	bool bOnce = true;
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

	/**
	 * M6-001: the menu display name (the rooms.json optional display_name row
	 * field; ASCII English at this prototype stage - real localization is a
	 * later UI task). Empty means the UI falls back to the RoomId.
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Room|Identity")
	FString DisplayName;

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

	/**
	 * M5-071: the room's finite scene rules (schema 2). Empty means the room
	 * runs no rule-driven flow (the training room keeps its non-combat
	 * semantics on purpose). Rule execution belongs to the M5-072 evaluator.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Room|Rules")
	TArray<FRoomSceneRule> Rules;

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

/**
 * M5-071: validates the room's finite scene rules (schema 2). Every problem
 * names the room-independent rule path "Rules[i].Field" plus the rule id, so
 * the report reads room/rule/field. Checked: unique non-empty RuleId, the
 * event/effect field pairing (PlayerEnteredRegion needs RegionId
 * "activation" - the one defined region of this stage - and no wave index;
 * WaveCleared needs an in-range WaveIndex and no region; PlayerDied carries
 * neither; StartWave needs an in-range NextWaveIndex while MarkCleared /
 * FailRun must not carry one), finite non-negative DelaySeconds, and the
 * rule-graph shape: a room that starts waves needs a PlayerEnteredRegion
 * entry, every started wave needs its WaveCleared continuation, the started
 * indices strictly increase along every chain (no cycles, no same-wave
 * restart), and at least one chain from the entry reaches MarkCleared. An
 * empty rule list is legal (the training room). Pure check.
 */
bool ValidateRoomRules(const URoomDefinition& Room, FText& OutErrors);

/**
 * M5-071: the EXPLICIT migration of a legacy schema 1 room (waves only, no
 * rules) to the schema 2 default battle rules: enter_start_wave_0
 * (PlayerEnteredRegion/activation -> StartWave 0 after 0 s), wave i ->
 * StartWave i+1 after 1.0 s for every adjacent pair, wave_last -> MarkCleared
 * and player_dead_fail (PlayerDied -> FailRun), all once. Deterministic -
 * the same waves always migrate to the same rules; fails (OutRules empty,
 * false) for an empty wave list. This is the documented schema 1 -> 2
 * mapping, never a silent fallback for data that claims a newer schema.
 */
bool BuildLegacySchemaOneDefaultRules(const TArray<FRoomWaveDefinition>& Waves, TArray<FRoomSceneRule>& OutRules);

/**
 * M5-069: catalog-level validation across every room row of one source (the
 * later asset builder hands exactly this list in). RoomIds must be unique:
 * a duplicated id names the id and both row indices in OutErrors, because a
 * repeated row would make the room unaddressable for the run/session and the
 * asset generation ambiguous. Per-row field validation (map path, reward id,
 * enemy references, geometry) stays in ValidateRoomDefinition - this check is
 * only the cross-row rule. Null entries are reported by index. Pure check:
 * no loads, no spawns, no clocks.
 */
bool ValidateRoomCatalog(const TArray<const URoomDefinition*>& Rooms, FText& OutErrors);
