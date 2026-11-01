#include "DungeonCoordinatorSubsystem.h"

#include "DungeonSequenceParser.h"

#include "../Profile/ProfileSubsystem.h"
#include "../Room/RoomDefinition.h"
#include "../Room/RoomResult.h"
#include "../Room/RoomSessionSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/DateTime.h"

void UDungeonCoordinatorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// The sequence table loads once per GameInstance (data-driven like every
	// other source-of-truth JSON); the per-world session binding arrives
	// through BindSession from each new room's pawn.
	ReloadSequenceTable();
}

void UDungeonCoordinatorSubsystem::ReloadSequenceTable()
{
	TArray<FString> Problems;
	FDungeonSequenceParser::LoadFromFile(
		FPaths::Combine(FPaths::ProjectDir(), TEXT("Data/dungeon_sequences.json")),
		Sequences, Problems);
	if (Problems.Num() > 0)
	{
		UE_LOG(LogTemp, Error, TEXT("UEMMO M7-004: dungeon_sequences.json problems (%d):"), Problems.Num());
		for (const FString& Problem : Problems)
		{
			UE_LOG(LogTemp, Error, TEXT("  %s"), *Problem);
		}
		Sequences.Reset();
	}
}

void UDungeonCoordinatorSubsystem::RegisterRoomDefinition(URoomDefinition* Definition)
{
	if (Definition != nullptr && !Definition->RoomId.IsNone())
	{
		RoomDefinitions.Add(Definition->RoomId, Definition);
	}
}

void UDungeonCoordinatorSubsystem::BindSession(URoomSessionSubsystem* Session)
{
	if (BoundSession.Get() == Session)
	{
		return;
	}
	if (URoomSessionSubsystem* Previous = BoundSession.Get())
	{
		Previous->OnRunEnded().Remove(RunEndedHandle);
		Previous->OnExitAccepted().Remove(ExitAcceptedHandle);
	}
	BoundSession = Session;
	if (Session != nullptr)
	{
		RunEndedHandle = Session->OnRunEnded().AddUObject(this, &UDungeonCoordinatorSubsystem::HandleSessionRunEnded);
		ExitAcceptedHandle = Session->OnExitAccepted().AddUObject(this, &UDungeonCoordinatorSubsystem::HandleSessionExitAccepted);
	}
}

void UDungeonCoordinatorSubsystem::Deinitialize()
{
	if (URoomSessionSubsystem* Session = BoundSession.Get())
	{
		Session->OnRunEnded().Remove(RunEndedHandle);
		Session->OnExitAccepted().Remove(ExitAcceptedHandle);
	}
	Super::Deinitialize();
}

bool UDungeonCoordinatorSubsystem::StartDungeonRun(FName DungeonId, const FGuid& CharacterId,
	const FString& ConfigRevision)
{
	// 进入时要求无其他正在进行的 run.
	if (bRunActive)
	{
		UE_LOG(LogTemp, Warning, TEXT("UEMMO M7-004: a dungeon run is already active; the new start refuses."));
		return false;
	}
	const FDungeonSequenceRow* Row = Sequences.FindByPredicate(
		[&DungeonId](const FDungeonSequenceRow& Candidate) { return Candidate.DungeonId == DungeonId; });
	if (Row == nullptr)
	{
		UE_LOG(LogTemp, Warning, TEXT("UEMMO M7-004: unknown dungeon '%s'."), *DungeonId.ToString());
		return false;
	}

	RunState = FDungeonRunState();
	RunState.DungeonId = DungeonId;
	RunState.CharacterId = CharacterId;
	RunState.RoomIndex = 0;
	static uint64 M7_004_NextGeneration = 1;
	RunState.RunGeneration = M7_004_NextGeneration++;
	// The settlement token: a process-unique identity (generation + a prime
	// shift keeps it distinct from the session's run ids).
	RunState.SettlementToken = (RunState.RunGeneration << 20) ^ 0x5D00D00000000000ULL ^ static_cast<uint64>(Row->XPReward);
	RunState.ConfigRevision = ConfigRevision;
	RunState.SavedHP = -1.f;
	bRunActive = true;

	UE_LOG(LogTemp, Display, TEXT("UEMMO M7-004: dungeon run started (dungeon=%s rooms=%d generation=%llu token=%llu)."),
		*DungeonId.ToString(), Row->RoomIds.Num(), RunState.RunGeneration, RunState.SettlementToken);
	return true;
}

URoomDefinition* UDungeonCoordinatorSubsystem::GetCurrentRoomDefinition() const
{
	if (!bRunActive)
	{
		return nullptr;
	}
	const FDungeonSequenceRow* Row = Sequences.FindByPredicate(
		[&RunState = RunState](const FDungeonSequenceRow& Candidate) { return Candidate.DungeonId == RunState.DungeonId; });
	if (Row == nullptr || !Row->RoomIds.IsValidIndex(RunState.RoomIndex))
	{
		return nullptr;
	}
	const TObjectPtr<URoomDefinition>* Found = RoomDefinitions.Find(Row->RoomIds[RunState.RoomIndex]);
	return Found ? Found->Get() : nullptr;
}

bool UDungeonCoordinatorSubsystem::IsDungeonRunRoom(FName RoomId, uint64 RunId) const
{
	// Freshness = the CURRENT room id only (the sequence's room ids are
	// unique and the session's run id is per-room monotonic - a run-id
	// equality can never hold across rooms). The RunId param stays on the
	// signature for the log rows.
	return bRunActive && !RunState.bSettlementIssued && RunState.CharacterId.IsValid()
		&& GetCurrentRoomDefinition() != nullptr
		&& GetCurrentRoomDefinition()->RoomId == RoomId;
}

bool UDungeonCoordinatorSubsystem::IsDungeonCurrentRoom(FName RoomId) const
{
	return bRunActive && GetCurrentRoomDefinition() != nullptr
		&& GetCurrentRoomDefinition()->RoomId == RoomId;
}

bool UDungeonCoordinatorSubsystem::IsDungeonFinalClear(FName RoomId, uint64 RunId) const
{
	if (!IsDungeonRunRoom(RoomId, RunId))
	{
		return false;
	}
	const FDungeonSequenceRow* Row = Sequences.FindByPredicate(
		[&RunState = RunState](const FDungeonSequenceRow& Candidate) { return Candidate.DungeonId == RunState.DungeonId; });
	return Row != nullptr && RunState.RoomIndex == Row->RoomIds.Num() - 1;
}

void UDungeonCoordinatorSubsystem::SetSnapshotProviders(TFunction<float()> InHPProvider,
	TFunction<int32()> InMPProvider,
	TFunction<TMap<FName, float>()> InCooldownProvider)
{
	HPProvider = MoveTemp(InHPProvider);
	MPProvider = MoveTemp(InMPProvider);
	CooldownProvider = MoveTemp(InCooldownProvider);
}

void UDungeonCoordinatorSubsystem::SetTravelProvider(TFunction<bool(FName RoomId)> InTravelProvider)
{
	TravelProvider = MoveTemp(InTravelProvider);
}

void UDungeonCoordinatorSubsystem::SetRewardProvider(TFunction<bool(const FRoomResult& Result)> InRewardProvider)
{
	RewardProvider = MoveTemp(InRewardProvider);
}

void UDungeonCoordinatorSubsystem::HandleSessionRunEnded(const FRoomResult& Result)
{
	// Free-select rooms (no active dungeon run) are untouched - the per-room
	// settlement flow stays verbatim.
	if (!bRunActive || !Result.bCleared)
	{
		return;
	}
	// Only the CURRENT room at the CURRENT generation counts (an old world's
	// late session event is stale by definition).
	if (!IsDungeonRunRoom(Result.RoomId, Result.RunId))
	{
		UE_LOG(LogTemp, Verbose, TEXT("UEMMO M7-004: ignoring a stale/foreign room clear (%s run %llu)."),
			*Result.RoomId.ToString(), Result.RunId);
		return;
	}
	if (RunState.bCurrentRoomCleared)
	{
		// First-terminal-wins: a duplicate clear of the same room is a no-op.
		return;
	}
	RunState.bCurrentRoomCleared = true;
	UE_LOG(LogTemp, Display, TEXT("UEMMO M7-004: room %d ('%s') cleared inside dungeon '%s'."),
		RunState.RoomIndex, *Result.RoomId.ToString(), *RunState.DungeonId.ToString());

	// The FINAL room's clear is the one settlement of the whole dungeon.
	if (IsDungeonFinalClear(Result.RoomId, Result.RunId))
	{
		IssueFinalSettlement();
	}
	// A non-final clear only marks the room (the door chain opens it); the
	// per-room settlement UI is suppressed by the HUD's IsDungeonRunRoom ask.
}

void UDungeonCoordinatorSubsystem::IssueFinalSettlement()
{
	if (RunState.bSettlementIssued)
	{
		// 失败、退出、重复消息均不得结算或重复计数.
		return;
	}
	RunState.bSettlementIssued = true;
	DungeonDisplayResult.bSettlementIdValid = false;
	const FDungeonSequenceRow* Row = Sequences.FindByPredicate(
		[&RunState = RunState](const FDungeonSequenceRow& Candidate) { return Candidate.DungeonId == RunState.DungeonId; });
	if (Row == nullptr)
	{
		return;
	}
	// The synthesized result records the original room AND the dungeon
	// identity (the audit rule); the XP comes from the dungeon row, never the
	// per-room table.
	FRoomResult FinalResult;
	FinalResult.RunId = RunState.RunGeneration;
	FinalResult.SettlementId = RunState.SettlementToken;
	FinalResult.RoomId = Row->RoomIds[RunState.RoomIndex];
	FinalResult.Seed = static_cast<int32>(RunState.SettlementToken & 0x7FFFFFFFu);
	FinalResult.bCleared = true;
	// The XP rides the dungeon row (the synthesized result IS the audit row).
	FinalResult.KilledCount = 0;
	DungeonDisplayResult.Result = FinalResult;
	DungeonDisplayResult.bSettlementIdValid = true;

	if (RewardProvider)
	{
		const bool bIssued = RewardProvider(FinalResult);
		UE_LOG(LogTemp, Display, TEXT("UEMMO M7-004: the dungeon settlement %s (token %llu, xp %d)."),
			bIssued ? TEXT("issued") : TEXT("REFUSED"), RunState.SettlementToken, Row->XPReward);
	}
	// DungeonClears: the independent value-type counter (schema v4; the A/B
	// digest already covers it).
	if (UProfileSubsystem* Profile = GetGameInstance() ? GetGameInstance()->GetSubsystem<UProfileSubsystem>() : nullptr)
	{
		Profile->RecordDungeonCleared();
		Profile->RecordRoomCleared(FinalResult.RoomId, static_cast<int64>(FDateTime::UtcNow().ToUnixTimestamp()));
	}
}

void UDungeonCoordinatorSubsystem::HandleSessionExitAccepted(FName RoomId, uint64 RunId)
{
	if (!bRunActive || !RunState.bCurrentRoomCleared)
	{
		// No active run, or the door of an UNCLEARED room (the exit chain's
		// own semantics already refused that case; the guard stays defensive).
		return;
	}
	if (!IsDungeonCurrentRoom(RoomId))
	{
		// A stale door callback (an old world's room) never advances the run;
		// the settlement flag deliberately does NOT gate the door (the final
		// room's door fires AFTER its settlement issued).
		return;
	}

	const FDungeonSequenceRow* Row = Sequences.FindByPredicate(
		[&RunState = RunState](const FDungeonSequenceRow& Candidate) { return Candidate.DungeonId == RunState.DungeonId; });
	if (Row == nullptr)
	{
		return;
	}

	if (RunState.RoomIndex >= Row->RoomIds.Num() - 1)
	{
		// The final door: the run ends (成功) and the flow returns to the menu.
		UE_LOG(LogTemp, Display, TEXT("UEMMO M7-004: the final door closed the dungeon run '%s'."), *RunState.DungeonId.ToString());
		EndRunLocal(true);
		if (TravelProvider)
		{
			TravelProvider(FName(NAME_None)); // NAME_None = the menu destination.
		}
		return;
	}

	// Snapshot the run-transient state at the door (remainders, not absolute
	// timestamps - the next world re-bases them on its own clock). The
	// values park in the pending slot the next room's pawn consumes.
	RunState.SavedHP = HPProvider ? HPProvider() : -1.f;
	RunState.CooldownRemainders = CooldownProvider ? CooldownProvider() : TMap<FName, float>();
	bPendingRoomSnapshot = true;
	PendingHP = RunState.SavedHP;
	PendingMP = -1;
	PendingCooldowns = RunState.CooldownRemainders;
	UE_LOG(LogTemp, Display, TEXT("UEMMO M7-004: snapshot taken at the door (hp %.1f, %d cooldown(s)); advancing to room %d."),
		RunState.SavedHP, RunState.CooldownRemainders.Num(), RunState.RoomIndex + 1);
	RunState.SavedMP = MPProvider ? MPProvider() : -1;
	PendingMP = RunState.SavedMP;

	RunState.RoomIndex++;
	RunState.bCurrentRoomCleared = false;
	OnRoomAdvanced.Broadcast(RunState.RoomIndex);

	// The next leg travels through the injected seam (the production binding
	// is the GameFlow's OpenLevel path; the new world's session starts a NEW
	// run id - the generation keeps the coordinator's identity stable).
	const FName NextRoomId = Row->RoomIds[RunState.RoomIndex];
	if (TravelProvider)
	{
		const bool bTraveled = TravelProvider(NextRoomId);
		UE_LOG(LogTemp, Display, TEXT("UEMMO M7-004: travel to '%s' %s."),
			*NextRoomId.ToString(), bTraveled ? TEXT("accepted") : TEXT("REFUSED"));
		if (!bTraveled)
		{
			// 加载失败恢复到安全菜单、原存档不动、错误可见.
			AbortRun(true);
		}
	}
}

bool UDungeonCoordinatorSubsystem::ConsumePendingRoomSnapshot(float& OutHP, int32& OutMP,
	TMap<FName, float>& OutCooldowns)
{
	if (!bPendingRoomSnapshot)
	{
		return false;
	}
	bPendingRoomSnapshot = false;
	OutHP = PendingHP;
	OutMP = PendingMP;
	OutCooldowns = MoveTemp(PendingCooldowns);
	PendingCooldowns.Reset();
	return true;
}

void UDungeonCoordinatorSubsystem::AbortRun(bool bTravelToMenu)
{
	if (!bRunActive)
	{
		return;
	}
	UE_LOG(LogTemp, Display, TEXT("UEMMO M7-004: the dungeon run '%s' aborted (no settlement)."), *RunState.DungeonId.ToString());
	EndRunLocal(false);
	if (bTravelToMenu && TravelProvider)
	{
		TravelProvider(FName(NAME_None));
	}
}

void UDungeonCoordinatorSubsystem::EndRunLocal(bool bSuccess)
{
	bRunActive = false;
	OnRunEnded.Broadcast(bSuccess);
}
