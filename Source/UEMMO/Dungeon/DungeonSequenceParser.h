#pragma once

#include "CoreMinimal.h"

#include "DungeonTypes.h"

/**
 * M7-004: the strict dungeon_sequences.json loader (the combat/profession
 * parser conventions: schema_version + table + rows, duplicate ids refused,
 * non-positive XP refused, empty/cross-referenced room lists refused at the
 * CALLER - the parser stays room-table-free and validates shape only).
 */
namespace FDungeonSequenceParser
{
	/**
	 * Parses the table text. Returns false with OutProblems naming the first
	 * issues; on success OutRows carries every validated row in file order.
	 */
	bool ParseFromText(const FString& Text, TArray<FDungeonSequenceRow>& OutRows,
		TArray<FString>& OutProblems);

	/** File-level convenience (Data/dungeon_sequences.json). */
	bool LoadFromFile(const FString& AbsolutePath, TArray<FDungeonSequenceRow>& OutRows,
		TArray<FString>& OutProblems);
}
