#include "DungeonSequenceParser.h"

#include "../Combat/Data/CombatDataTableParser.h"
#include "Misc/FileHelper.h"

namespace
{
	void AddProblem(TArray<FString>& Problems, const FString& Message)
	{
		if (Problems.Num() < 32)
		{
			Problems.Add(Message);
		}
	}
}

bool FDungeonSequenceParser::ParseFromText(const FString& Text, TArray<FDungeonSequenceRow>& OutRows,
	TArray<FString>& OutProblems)
{
	OutRows.Reset();
	OutProblems.Reset();

	TSharedPtr<FCombatJsonValue> Root;
	FString ParseError;
	if (!FCombatJsonParser::Parse(Text, Root, ParseError) || !Root.IsValid()
		|| Root->Kind != FCombatJsonValue::EKind::Object)
	{
		AddProblem(OutProblems, FString::Printf(TEXT("the table is not a JSON object (%s)"), *ParseError));
		return false;
	}
	const TSharedPtr<FCombatJsonValue>* Schema = Root->Object.Find(TEXT("schema_version"));
	if (Schema == nullptr || (*Schema)->Kind != FCombatJsonValue::EKind::Number
		|| (*Schema)->Number != 1.0)
	{
		AddProblem(OutProblems, TEXT("schema_version must be the number 1"));
		return false;
	}
	const TSharedPtr<FCombatJsonValue>* Rows = Root->Object.Find(TEXT("rows"));
	if (Rows == nullptr || (*Rows)->Kind != FCombatJsonValue::EKind::Array)
	{
		AddProblem(OutProblems, TEXT("the required top-level array 'rows' is missing"));
		return false;
	}

	TSet<FName> SeenIds;
	for (const TSharedPtr<FCombatJsonValue>& RowValue : (*Rows)->Array)
	{
		if (!RowValue.IsValid() || RowValue->Kind != FCombatJsonValue::EKind::Object)
		{
			AddProblem(OutProblems, TEXT("a rows[] entry is not a JSON object"));
			continue;
		}
		const TSharedPtr<FCombatJsonValue>* Id = RowValue->Object.Find(TEXT("dungeon_id"));
		if (Id == nullptr || (*Id)->Kind != FCombatJsonValue::EKind::String || (*Id)->String.IsEmpty())
		{
			AddProblem(OutProblems, TEXT("a rows[] entry is missing its string 'dungeon_id'"));
			continue;
		}
		const FName DungeonId(*(*Id)->String);
		if (SeenIds.Contains(DungeonId))
		{
			AddProblem(OutProblems, FString::Printf(TEXT("duplicate dungeon_id '%s'"), *(*Id)->String));
			continue;
		}
		const TSharedPtr<FCombatJsonValue>* RoomIds = RowValue->Object.Find(TEXT("room_ids"));
		if (RoomIds == nullptr || (*RoomIds)->Kind != FCombatJsonValue::EKind::Array
			|| (*RoomIds)->Array.Num() < 2)
		{
			AddProblem(OutProblems, FString::Printf(
				TEXT("dungeon '%s': room_ids must hold at least two rooms"), *(*Id)->String));
			continue;
		}
		const TSharedPtr<FCombatJsonValue>* XP = RowValue->Object.Find(TEXT("xp_reward"));
		if (XP == nullptr || (*XP)->Kind != FCombatJsonValue::EKind::Number
			|| (*XP)->Number <= 0.0)
		{
			AddProblem(OutProblems, FString::Printf(
				TEXT("dungeon '%s': xp_reward must be a positive number"), *(*Id)->String));
			continue;
		}

		FDungeonSequenceRow Row;
		Row.DungeonId = DungeonId;
		bool bRoomsValid = true;
		TSet<FName> SeenRooms;
		for (const TSharedPtr<FCombatJsonValue>& RoomValue : (*RoomIds)->Array)
		{
			if (!RoomValue.IsValid() || RoomValue->Kind != FCombatJsonValue::EKind::String
				|| RoomValue->String.IsEmpty())
			{
				AddProblem(OutProblems, FString::Printf(
					TEXT("dungeon '%s': a room_ids entry is not a non-empty string"), *(*Id)->String));
				bRoomsValid = false;
				break;
			}
			const FName RoomId(RoomValue->String);
			if (SeenRooms.Contains(RoomId))
			{
				AddProblem(OutProblems, FString::Printf(
					TEXT("dungeon '%s': room id '%s' appears twice in one sequence"), *(*Id)->String, *RoomValue->String));
				bRoomsValid = false;
				break;
			}
			SeenRooms.Add(RoomId);
			Row.RoomIds.Add(RoomId);
		}
		if (!bRoomsValid)
		{
			continue;
		}
		Row.XPReward = static_cast<int32>((*XP)->Number);
		SeenIds.Add(DungeonId);
		OutRows.Add(Row);
	}
	return OutProblems.Num() == 0 && OutRows.Num() > 0;
}

bool FDungeonSequenceParser::LoadFromFile(const FString& AbsolutePath, TArray<FDungeonSequenceRow>& OutRows,
	TArray<FString>& OutProblems)
{
	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *AbsolutePath))
	{
		AddProblem(OutProblems, FString::Printf(TEXT("the dungeon table is unreadable: %s"), *AbsolutePath));
		return false;
	}
	return ParseFromText(Raw, OutRows, OutProblems);
}
