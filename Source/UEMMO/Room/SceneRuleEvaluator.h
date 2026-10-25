#pragma once

#include "CoreMinimal.h"
#include "RoomDefinition.h"

/**
 * M5-072: one finite effect request the evaluator hands to the injected
 * session adapter. The evaluator never applies effects itself - HP, rewards,
 * wave starts and exits belong to the existing owners (HealthComponent,
 * URoomSessionSubsystem, the M5-073 wiring).
 */
struct FRoomRuleCommand
{
	/** The rule whose effect is requested (diagnostics and once-tracking key). */
	FName RuleId;

	/** The whitelisted effect (StartWave / MarkCleared / FailRun). */
	ERoomRuleEffect Effect = ERoomRuleEffect::StartWave;

	/** The wave a StartWave command starts (-1 for the terminal effects). */
	int32 WaveIndex = -1;

	/** The run the command belongs to (mirrors the active run). */
	uint64 RunId = 0;
};

/** Outcome of one command as reported by the session adapter. */
enum class ERoomRuleDecisionType : uint8
{
	/** The session applied (or will apply) the effect. */
	Accepted,
	/** The session refused the effect (state, guard or first-terminal-wins); the reason names the cause. */
	Rejected,
	/** The effect was already in place (an idempotent repeat); treated as success. */
	AlreadyApplied
};

/** Decision + human-readable context (room/run/rule/event/effect), never a bare bool. */
struct FRoomRuleDecision
{
	ERoomRuleDecisionType Type = ERoomRuleDecisionType::Rejected;
	FString Reason;
};

/**
 * M5-072: one external scene event as a value snapshot (produced by the
 * world's trusted sources; M5-073 wires the production ones). EventSequence
 * is strictly increasing inside one run - the evaluator deduplicates by it.
 */
struct FRoomRuleEvent
{
	/** The run the event belongs to (must equal the evaluator's active run). */
	uint64 RunId = 0;

	/** The room the event belongs to (must equal the active room when set). */
	FName RoomId;

	/** Strictly increasing per-run sequence (1..; replays are idempotent no-ops). */
	uint64 EventSequence = 0;

	/** The whitelisted event type. */
	ERoomRuleEvent Type = ERoomRuleEvent::PlayerEnteredRegion;

	/** The region of a PlayerEnteredRegion event ("activation"). */
	FName RegionId;

	/** The wave of a WaveCleared event (-1 otherwise). */
	int32 WaveIndex = -1;

	/** The session clock at the event source (optional sync hint). */
	double SessionClockSeconds = 0.0;
};

/**
 * M5-072: the pure per-run scene-rule evaluator. It owns ONLY rule progress
 * and delayed-effect bookkeeping for exactly one run at a time:
 *
 *   - ResetForRun arms the (already validated, M5-071) rules of one run with
 *     an injected command handler (the session adapter); CancelRun drops the
 *     whole run scope so no stale delayed callback can ever fire.
 *   - SubmitEvent deduplicates by the per-run EventSequence (replays return
 *     AlreadyApplied and fire nothing), rejects foreign/old runs, foreign
 *     rooms, reentrant submits (a handler submitting back during dispatch)
 *     and sequence rewind; every rule matched by the event either dispatches
 *     its command immediately (delay 0) or arms a delayed command on the
 *     injected clock.
 *   - once rules complete ONLY when the session adapter reports the effect
 *     succeeded (Accepted / AlreadyApplied); a Rejected command leaves the
 *     rule armed - no fake completion. Non-once rules may fire on every
 *     matching event.
 *   - AdvanceClock drives the due delayed commands on the injected clock
 *     only (non-finite injections ignored; rewinds freeze rather than fire).
 *
 * The evaluator holds no world state: no HP, no kills, no spawns, no
 * settlement. First-terminal-wins stays in the session adapter - a rejected
 * terminal command is recorded, never forced.
 */
class FSceneRuleEvaluator
{
public:
	/** The injected session adapter: applies (or refuses) one finite command. */
	using FCommandHandler = TFunction<FRoomRuleDecision(const FRoomRuleCommand&)>;

	/** Arms the rules of one fresh run; drops any previous run scope entirely. */
	void ResetForRun(uint64 RunId, FName RoomId, const TArray<FRoomSceneRule>& InRules,
		const FCommandHandler& InHandler);

	/** Drops the run scope (pending delays included) when the id matches; foreign ids are ignored. */
	void CancelRun(uint64 RunId);

	/** Feeds one event; returns the aggregated decision of this submission. */
	FRoomRuleDecision SubmitEvent(const FRoomRuleEvent& Event);

	/** Advances the injected clock; returns the decisions of every due delayed command (in arm order). */
	TArray<FRoomRuleDecision> AdvanceClock(double NowSeconds);

	// -- Diagnostics (assertions read these) -----------------------------------

	uint64 GetRunId() const { return ActiveRunId; }
	FName GetRoomId() const { return ActiveRoomId; }
	double GetClockSeconds() const { return ClockSeconds; }
	int32 GetRuleCount() const { return Rules.Num(); }
	int32 GetCompletedRuleCount() const { return CompletedRuleIds.Num(); }
	int32 GetPendingDelayedCommandCount() const { return PendingCommands.Num(); }

	/** True when the once-rule completed through a successful effect. */
	bool IsRuleCompleted(FName RuleId) const { return CompletedRuleIds.Contains(RuleId); }

private:
	/** One armed delayed command (injected-clock due time). */
	struct FPendingCommand
	{
		FName RuleId;
		ERoomRuleEffect Effect = ERoomRuleEffect::StartWave;
		int32 WaveIndex = -1;
		double DueSeconds = 0.0;
	};

	/** Dispatches one command through the handler (reentrancy-guarded); completes the once-rule on success. */
	FRoomRuleDecision DispatchCommand(const FRoomRuleCommand& Command, const FRoomRuleEvent* Cause);

	/** True when the rule matches the event's type and parameters. */
	static bool RuleMatchesEvent(const FRoomSceneRule& Rule, const FRoomRuleEvent& Event);

	uint64 ActiveRunId = 0;
	FName ActiveRoomId;
	TArray<FRoomSceneRule> Rules;
	FCommandHandler Handler;
	TSet<FName> CompletedRuleIds;
	TArray<FPendingCommand> PendingCommands;
	uint64 LastSeenSequence = 0;
	double ClockSeconds = 0.0;
	bool bDispatching = false;
};
