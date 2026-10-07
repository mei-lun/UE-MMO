#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"

#include "TestRoomConfigDriver.generated.h"

class UEnemyDefinition;
class UWorld;

/**
 * M5-018A: one configured target row of the system test room (the design's
 * "目标布局/策略来自配置"). Pure value row: the enemy business id resolves a
 * /Game/UEMMO/Enemy/Definitions/DA_<EnemyId> definition, the health and the
 * target reaction policy id come from the test_room source table and the
 * spawn location is the configured room-local X/Y/Z.
 */
USTRUCT(BlueprintType)
struct FTestRoomTargetConfig
{
	GENERATED_BODY()

	/** Unique target id inside the room (a-z, 0-9, _). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TestRoom|Target")
	FName TargetId;

	/** Enemy definition business id (DA_<EnemyId> under Enemy/Definitions). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TestRoom|Target")
	FName EnemyId;

	/** The target's configured max health (finite, greater than 0). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TestRoom|Target")
	float MaxHealth = 100.0f;

	/** The configured target reaction policy id (DA_Target_<PolicyId>). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TestRoom|Target")
	FName PolicyId;

	/** Room-local spawn location (X lateral, Y depth, Z height, cm). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TestRoom|Target")
	FVector LocationCm = FVector::ZeroVector;
};

/**
 * M5-018A: the parsed test_room source row (the whole config of one system
 * test room). VehicleSpawnIds carries the future vehicle_spawns list: it is
 * legal and must stay empty until the vehicle segment (M5-037+) implements
 * the capability - a non-empty list is refused by the parser with an
 * explicit "not implemented" error, never silently ignored, and no stand-in
 * vehicle is ever fabricated.
 */
USTRUCT(BlueprintType)
struct FTestRoomConfig
{
	GENERATED_BODY()

	/** Room business id (a-z, 0-9, _). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TestRoom|Config")
	FName RoomId;

	/** Configured targets in spawn order; at least one. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TestRoom|Config")
	TArray<FTestRoomTargetConfig> Targets;

	/** Future vehicle spawns; legal only while empty (see the struct comment). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TestRoom|Config")
	TArray<FName> VehicleSpawnIds;

	/** The reward pool id the room's settlement uses (the M2 reward tables). */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "TestRoom|Config")
	FName RewardPoolId;
};

/**
 * M5-018A: the config-driven system test room driver. One driver actor loads
 * the test_room source configuration, spawns every configured target by id in
 * a loop and applies the configured health, reaction policy and location -
 * changing a value in the source table and reloading the map changes the room
 * without any class-code change, and an unknown id fails explicitly (never a
 * silent skip, never a stand-in actor).
 *
 * Frozen future seams (the card's "给后继 Weapon/Vehicle 模块预留冻结接口"):
 * the config carries the vehicle list and the driver exposes SpawnVehicles -
 * both refuse with an explicit "capability not implemented" until the vehicle
 * segment implements them. New content joins by adding a config row, never by
 * branching the driver on a concrete id.
 *
 * Config source: Data/test_room.json, parsed by
 * ParseTestRoomConfig (the M5-005 FCombatJsonValue reader). The runtime JSON
 * read is the development-facility path of this card; the production catalog
 * adaptation (assets, cook list, packaged builds) is owned by M5-020A per the
 * interface contract section 6.
 */
UCLASS(ClassGroup = (Combat))
class UEMMO_API ACombatTestRoomDriver : public AActor
{
	GENERATED_BODY()

public:
	ACombatTestRoomDriver();

	/**
	 * Parses one test_room source text. Strict: schema_version must be 1, the
	 * table must be "test_room", exactly one room row, at least one target
	 * with syntactically valid ids, a finite positive max health, a non-empty
	 * policy id - and vehicle_spawns must be empty (a non-empty list refuses
	 * with the explicit not-implemented error; the capability arrives with
	 * the vehicle segment). Every problem names its field.
	 */
	static bool ParseTestRoomConfig(const FString& JsonText, FTestRoomConfig& OutConfig, FString& OutError);

	/** The parsed config (valid only after a successful LoadConfig). */
	const FTestRoomConfig& GetConfig() const { return Config; }

	/**
	 * Loads the config from the source JSON file (ConfigJsonPath, project
	 * relative) and stores it. Returns false with OutError on a missing file
	 * or a refused parse.
	 */
	bool LoadConfig(FString& OutError);

	/**
	 * Spawns every configured target: the enemy definition resolves from
	 * /Game/UEMMO/Enemy/Definitions/DA_<EnemyId> (a missing id fails
	 * explicitly and names the target), the spawn applies the configured
	 * location, max health and reaction policy (DA_Target_<PolicyId> loaded
	 * from the M5-008 generated assets). Returns the number of spawned
	 * targets; OutMissingIds lists every refused target id.
	 */
	int32 SpawnTargets(UWorld& World, TArray<FName>& OutMissingIds);

	/**
	 * M5-037+ frozen seam: refuses with an explicit "not implemented" until
	 * the vehicle segment implements vehicle spawning. Never fabricates a
	 * stand-in.
	 */
	bool SpawnVehicles(UWorld& World, FString& OutError);

	/** The source JSON path (project relative) the driver loads. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "TestRoom|Config")
	FString ConfigJsonPath = TEXT("Data/test_room.json");

	/** M5-018A: test/diagnostic surface - mutable config access (a fixture parses into the driver without a file). */
	FTestRoomConfig& GetConfigMutable() { return Config; }

	/** M5-018A: test surface - installs a parsed config (the same state LoadConfig produces). */
	void SetConfigForTesting(const FTestRoomConfig& InConfig) { Config = InConfig; }

private:
	FTestRoomConfig Config;
};
