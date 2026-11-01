#pragma once

#include "CoreMinimal.h"
#include "Misc/Guid.h"

/**
 * M7-004: one dungeon_sequences.json row (02 文档 §7: Data/dungeon_sequences.json
 * 新增一个"连续模式"ID，按顺序引用现有两张房间图，独立于现有菜单自由选关).
 */
struct FDungeonSequenceRow
{
	/** Business identity of the continuous dungeon (e.g. "dungeon_lorien_double"). */
	FName DungeonId;

	/** The ordered room ids of one full run (the sequence IS the order). */
	TArray<FName> RoomIds;

	/** The ONE settlement's XP for a full clear (the per-room 50 flow stays
	 * for the free-select path; the dungeon XP comes from this row). */
	int32 XPReward = 0;
};

/**
 * M7-004: the GameInstance-lifecycle run state (02 文档 §7: DungeonId、选定
 * CharacterId、当前房索引、RunGeneration/SettlementToken、带状态的 HP/MP、按
 * SkillId 保存的冷却剩余值、源配置 revision). Only this run's transient state
 * - never a persistent save (大厅存档默认不保存战斗中冷却). HP/MP/cooldowns
 * snapshot at the exit door and restore at the next room's start; the times
 * are remainders, never old absolute timestamps (绝不直接跨 World 比较旧绝对
 * 时间戳).
 */
struct FDungeonRunState
{
	FName DungeonId = NAME_None;
	FGuid CharacterId;

	/** Index into the sequence's RoomIds of the room the run is in NOW. */
	int32 RoomIndex = 0;

	/** Monotonic per-session run counter (the stale-callback guard). */
	uint64 RunGeneration = 0;

	/** The ONE settlement's identity, minted at run start. */
	uint64 SettlementToken = 0;

	/** HP snapshot taken at the last exit door (from the health pool). */
	float SavedHP = -1.f;

	/** MP snapshot taken at the last exit door (-1 = no snapshot pending). */
	int32 SavedMP = -1;

	/** Per-skill cooldown remainders (seconds), snapshot at the exit door. */
	TMap<FName, float> CooldownRemainders;

	/** The profession config revision the run started under (provenance). */
	FString ConfigRevision;

	/** True once the final room's settlement was issued (exactly once). */
	bool bSettlementIssued = false;

	/** True once the current room cleared (the door-open fact; a second
	 * OnRunEnded for the same room never re-fires). */
	bool bCurrentRoomCleared = false;
};
