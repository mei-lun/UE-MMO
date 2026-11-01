#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/Function.h"

#include "../Room/RoomResult.h"

#include "DungeonTypes.h"

#include "DungeonCoordinatorSubsystem.generated.h"

class URoomDefinition;
class URoomSessionSubsystem;
class UProfileSubsystem;


/** Fired when the coordinator advanced the run into the next room. */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnDungeonRoomAdvanced, int32 /*NewRoomIndex*/);

/** Fired when the run ended (final settlement issued, failure or abort). */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnDungeonRunEnded, bool /*bSuccess*/);

/**
 * M7-004: the GameInstance-lifecycle owner of the continuous dungeon run
 * (02 文档 §7). The coordinator:
 * - holds the ONE active FDungeonRunState (进入时要求无其他正在进行的 run);
 * - consumes the room session's OnRunEnded (marking the cleared room; the
 *   FINAL room's clear synthesizes the single settlement through the
 *   injected reward seam and bumps the profile's DungeonClears exactly once)
 *   and OnExitAccepted (the player reached the door of the current cleared
 *   room: snapshot HP/MP/cooldown remainders, advance the index and travel
 *   through the injected seam; the final door ends the run);
 * - only accepts callbacks for the CURRENT room id at the CURRENT run
 *   generation - stale callbacks (an old world's late session event) drop.
 *
 * Travel and reward are INJECTED seams (TFunction): production binds
 * UGameFlowSubsystem::EnterRoom and URewardService::BeginReward; tests bind
 * counting lambdas. The coordinator never opens levels or touches the
 * reward catalog itself.
 */
UCLASS()
class UEMMO_API UDungeonCoordinatorSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// USubsystem
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Starts one continuous run. Refuses while a run is active or the
	 * dungeon id is unknown to the loaded sequence table. Mints a fresh
	 * RunGeneration/SettlementToken; RoomIndex 0 is the run's first room.
	 */
	bool StartDungeonRun(FName DungeonId, const FGuid& CharacterId, const FString& ConfigRevision);

	/** True while a run is in flight (one run at a time). */
	bool IsRunActive() const { return bRunActive; }

	/** The active run state (valid only while IsRunActive). */
	const FDungeonRunState& GetRunState() const { return RunState; }

	/** The loaded sequence rows (Data/dungeon_sequences.json). */
	const TArray<FDungeonSequenceRow>& GetSequences() const { return Sequences; }

	/** The room definition of the run's CURRENT room (null when inactive). */
	URoomDefinition* GetCurrentRoomDefinition() const;

	/**
	 * True when (RoomId, RunId) names the CURRENT room of an active dungeon
	 * run that has NOT settled yet - the HUD's suppression/labeling hook for
	 * the per-room victory flow.
	 */
	bool IsDungeonRunRoom(FName RoomId, uint64 RunId) const;

	/**
	 * True when RoomId names the CURRENT room of an active run REGARDLESS of
	 * the settlement flag (the exit-door guard: the final room's settlement
	 * is already issued when its door fires - the door still ends the run).
	 */
	bool IsDungeonCurrentRoom(FName RoomId) const;

	/** True after the FINAL room of an active run cleared (the one-settlement label). */
	bool IsDungeonFinalClear(FName RoomId, uint64 RunId) const;

	/**
	 * The synthesized FINAL-room result (the dungeon identity + xp_reward +
	 * the settlement token) for the HUD's reward binding; null when the last
	 * clear was not a dungeon final. One value, overwritten per final clear.
	 */
	const FRoomResult* GetDungeonDisplayResult() const
	{
		return DungeonDisplayResult.bSettlementIdValid ? &DungeonDisplayResult.Result : nullptr;
	}

	/**
	 * True while a carried snapshot (HP/MP/cooldowns taken at the previous
	 * door) is waiting to apply to the CURRENT room's pawn; the consumer
	 * clears it. The restore happens exactly once per room advance.
	 */
	bool ConsumePendingRoomSnapshot(float& OutHP, int32& OutMP, TMap<FName, float>& OutCooldowns);

	/**
	 * M7-004: binds the CURRENT world's room session (a UWorldSubsystem - a
	 * new room's world carries a NEW session, so the pawn's BeginPlay rebinds
	 * per world; the previous binding drops first). Idempotent per instance.
	 */
	void BindSession(URoomSessionSubsystem* Session);

	/**
	 * Reloads Data/dungeon_sequences.json (Initialize calls it once; tests
	 * call it on a bare NewObject'd subsystem where Initialize never ran).
	 */
	void ReloadSequenceTable();

	/**
	 * Registers one room definition the run may travel through (the game
	 * registers the selectable rooms at startup; tests register fixtures).
	 * A duplicate RoomId overwrites.
	 */
	void RegisterRoomDefinition(URoomDefinition* Definition);

	/**
	 * The injected transient-state providers (HP/MP/cooldowns snapshot at the
	 * exit door) and the travel/reward seams. The character and the game flow
	 * bind these once at startup; tests bind counting lambdas. An unset
	 * provider reads a neutral default (HP -1 skips the restore, MP 0).
	 */
	void SetSnapshotProviders(TFunction<float()> InHPProvider,
		TFunction<int32()> InMPProvider,
		TFunction<TMap<FName, float>()> InCooldownProvider);
	void SetTravelProvider(TFunction<bool(FName RoomId)> InTravelProvider);
	void SetRewardProvider(TFunction<bool(const FRoomResult& Result)> InRewardProvider);

	/** Aborts the active run WITHOUT settling (death/退出: 记失败并回菜单). */
	void AbortRun(bool bTravelToMenu);

	/** The room-advanced / run-ended notifications (the HUD and the pawn bind). */
	FOnDungeonRoomAdvanced OnRoomAdvanced;
	FOnDungeonRunEnded OnRunEnded;

private:
	/** The session callbacks (bound once in Initialize). */
	void HandleSessionRunEnded(const FRoomResult& Result);
	void HandleSessionExitAccepted(FName RoomId, uint64 RunId);

	/** Issues the ONE final settlement (exactly-once guarded). */
	void IssueFinalSettlement();

	/** Ends the run bookkeeping (no travel; the caller owns the destination). */
	void EndRunLocal(bool bSuccess);

	/** The loaded dungeon_sequences.json rows (empty = no dungeon selectable). */
	TArray<FDungeonSequenceRow> Sequences;

	/** The synthesized final result for the HUD (validity flag + the value). */
	struct
	{
		bool bSettlementIdValid = false;
		FRoomResult Result;
	} DungeonDisplayResult;

	/** The carried snapshot waiting for the next room's pawn. */
	bool bPendingRoomSnapshot = false;
	float PendingHP = -1.f;
	int32 PendingMP = -1;
	TMap<FName, float> PendingCooldowns;

	FDungeonRunState RunState;
	bool bRunActive = false;

	/** The room definitions the run may travel through (injected at start). */
	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<URoomDefinition>> RoomDefinitions;

	/** The session bindings (removed on Deinitialize). */
	TWeakObjectPtr<URoomSessionSubsystem> BoundSession;
	FDelegateHandle RunEndedHandle;
	FDelegateHandle ExitAcceptedHandle;

	// The injected seams (empty = neutral defaults).
	TFunction<float()> HPProvider;
	TFunction<int32()> MPProvider;
	TFunction<TMap<FName, float>()> CooldownProvider;
	TFunction<bool(FName RoomId)> TravelProvider;
	TFunction<bool(const FRoomResult& Result)> RewardProvider;
};
