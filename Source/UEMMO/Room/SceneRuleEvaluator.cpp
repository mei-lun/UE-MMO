// M5-072: the pure per-run scene-rule evaluator (see SceneRuleEvaluator.h
// for the full contract). Every rule here is a bookkeeping decision - the
// injected session adapter owns every real effect, the injected clock owns
// every real second, and the run scope owns nothing that survives a
// ResetForRun / CancelRun.

#include "SceneRuleEvaluator.h"

namespace
{
	// Due-time tolerance: guards the double boundary of accumulated injected
	// values (the M2-008 wave-gap precedent).
	constexpr double M5_072_DueEpsilon = 1e-9;

	FString M5_072_Describe(const FRoomRuleCommand& Command)
	{
		return FString::Printf(TEXT("run=%llu rule='%s' effect=%d wave=%d"),
			Command.RunId, *Command.RuleId.ToString(), static_cast<int32>(Command.Effect), Command.WaveIndex);
	}
}

void FSceneRuleEvaluator::ResetForRun(uint64 RunId, FName RoomId, const TArray<FRoomSceneRule>& InRules,
	const FCommandHandler& InHandler)
{
	// A fresh run scope: previous completions, pending delays and the seen-
	// sequence watermark must never leak into the new run. The clock itself
	// keeps its last injected value on purpose - it is the continuous session
	// clock, not per-run state.
	ActiveRunId = RunId;
	ActiveRoomId = RoomId;
	Rules = InRules;
	Handler = InHandler;
	CompletedRuleIds.Empty();
	PendingCommands.Empty();
	LastSeenSequence = 0;
}

void FSceneRuleEvaluator::CancelRun(uint64 RunId)
{
	if (RunId != ActiveRunId || ActiveRunId == 0)
	{
		// A foreign or already-canceled id cancels nothing (a stale CancelRun
		// of an old run must not wipe the active one).
		return;
	}
	ActiveRunId = 0;
	ActiveRoomId = FName();
	Rules.Empty();
	Handler = nullptr;
	CompletedRuleIds.Empty();
	PendingCommands.Empty();
	LastSeenSequence = 0;
}

bool FSceneRuleEvaluator::RuleMatchesEvent(const FRoomSceneRule& Rule, const FRoomRuleEvent& Event)
{
	if (Rule.Event != Event.Type)
	{
		return false;
	}
	switch (Rule.Event)
	{
	case ERoomRuleEvent::PlayerEnteredRegion:
		return Rule.RegionId == Event.RegionId;
	case ERoomRuleEvent::WaveCleared:
		return Rule.WaveIndex == Event.WaveIndex;
	case ERoomRuleEvent::PlayerDied:
		return true;
	default:
		return false;
	}
}

FRoomRuleDecision FSceneRuleEvaluator::DispatchCommand(const FRoomRuleCommand& Command, const FRoomRuleEvent* Cause)
{
	if (!Handler)
	{
		return { ERoomRuleDecisionType::Rejected, TEXT("no command handler is installed (") + M5_072_Describe(Command) + TEXT(")") };
	}
	// Reentrancy guard: a handler submitting events (or advancing the clock)
	// back into the evaluator during dispatch could recurse without bound;
	// the nested call is refused with a named reason instead.
	if (bDispatching)
	{
		return { ERoomRuleDecisionType::Rejected, TEXT("reentrant dispatch refused (") + M5_072_Describe(Command) + TEXT(")") };
	}
	bDispatching = true;
	const FRoomRuleDecision Decision = Handler(Command);
	bDispatching = false;
	if (Decision.Type == ERoomRuleDecisionType::Accepted
		|| Decision.Type == ERoomRuleDecisionType::AlreadyApplied)
	{
		CompletedRuleIds.Add(Command.RuleId);
	}
	// A Rejected command leaves the rule armed: "completed only after the
	// effect succeeded" - no fake completion, a later matching event (or a
	// re-armed delay after ResetForRun) retries honestly.
	FRoomRuleDecision Annotated = Decision;
	if (Cause != nullptr)
	{
		Annotated.Reason = FString::Printf(TEXT("%s (event sequence %llu)"),
			*Annotated.Reason, Cause->EventSequence);
	}
	return Annotated;
}

FRoomRuleDecision FSceneRuleEvaluator::SubmitEvent(const FRoomRuleEvent& Event)
{
	if (ActiveRunId == 0)
	{
		return { ERoomRuleDecisionType::Rejected, TEXT("no active run: the event is a stale or pre-reset callback") };
	}
	if (Event.RunId != ActiveRunId)
	{
		return { ERoomRuleDecisionType::Rejected, FString::Printf(
			TEXT("foreign run: event run %llu is not the active run %llu"), Event.RunId, ActiveRunId) };
	}
	if (!Event.RoomId.IsNone() && Event.RoomId != ActiveRoomId)
	{
		return { ERoomRuleDecisionType::Rejected, FString::Printf(
			TEXT("foreign room: event room '%s' is not the active room '%s'"),
			*Event.RoomId.ToString(), *ActiveRoomId.ToString()) };
	}
	if (Event.EventSequence == 0)
	{
		return { ERoomRuleDecisionType::Rejected, TEXT("event sequence must start at 1 (got 0)") };
	}
	if (bDispatching)
	{
		return { ERoomRuleDecisionType::Rejected, TEXT("reentrant submit refused: a handler submitted during dispatch") };
	}
	if (Event.EventSequence <= LastSeenSequence)
	{
		// Replay of an already-seen sequence: idempotent no-op (never a second
		// command, never a watermark change).
		return { ERoomRuleDecisionType::AlreadyApplied, FString::Printf(
			TEXT("replay of sequence %llu (last seen %llu) fired nothing"), Event.EventSequence, LastSeenSequence) };
	}
	LastSeenSequence = Event.EventSequence;

	// The event's clock stamp only ever moves the evaluator clock forward.
	if (FMath::IsFinite(Event.SessionClockSeconds) && Event.SessionClockSeconds > ClockSeconds)
	{
		ClockSeconds = Event.SessionClockSeconds;
	}

	FRoomRuleDecision Aggregated{ ERoomRuleDecisionType::Rejected,
		FString::Printf(TEXT("no armed rule matched sequence %llu"), Event.EventSequence) };
	int32 MatchedCount = 0;
	// Index loop over a rule-count snapshot with a per-rule COPY: a dispatched
	// command may reenter synchronously (a terminal settlement cancels the
	// whole scope), which must never mutate an array under a ranged-for.
	const int32 RuleCountAtEntry = Rules.Num();
	for (int32 RuleIndex = 0; RuleIndex < RuleCountAtEntry; ++RuleIndex)
	{
		if (ActiveRunId == 0 || RuleIndex >= Rules.Num())
		{
			// The scope was canceled or reset mid-dispatch; the matching loop
			// ends honestly instead of touching stale storage.
			break;
		}
		const FRoomSceneRule Rule = Rules[RuleIndex];
		if (!RuleMatchesEvent(Rule, Event))
		{
			continue;
		}
		if (Rule.bOnce && CompletedRuleIds.Contains(Rule.RuleId))
		{
			continue;
		}
		++MatchedCount;
		if (Rule.DelaySeconds > 0.0f && FMath::IsFinite(Rule.DelaySeconds))
		{
			// Delayed effect: arm on the injected clock (skip when an equal
			// request of the same rule is already armed - a re-event must not
			// stack duplicates).
			const double DueSeconds = ClockSeconds + static_cast<double>(Rule.DelaySeconds);
			const bool bAlreadyArmed = PendingCommands.ContainsByPredicate(
				[&Rule](const FPendingCommand& Pending)
				{
					return Pending.RuleId == Rule.RuleId;
				});
			if (!bAlreadyArmed)
			{
				FPendingCommand Pending;
				Pending.RuleId = Rule.RuleId;
				Pending.Effect = Rule.Effect;
				Pending.WaveIndex = Rule.NextWaveIndex;
				Pending.DueSeconds = DueSeconds;
				PendingCommands.Add(Pending);
			}
			Aggregated = { ERoomRuleDecisionType::Accepted, FString::Printf(
				TEXT("armed '%s' for %.3f s on the injected clock"), *Rule.RuleId.ToString(), Rule.DelaySeconds) };
			continue;
		}
		FRoomRuleCommand Command;
		Command.RuleId = Rule.RuleId;
		Command.Effect = Rule.Effect;
		Command.WaveIndex = Rule.NextWaveIndex;
		Command.RunId = ActiveRunId;
		const FRoomRuleDecision Decision = DispatchCommand(Command, &Event);
		if (MatchedCount == 1 || Decision.Type == ERoomRuleDecisionType::Rejected)
		{
			Aggregated = Decision;
		}
	}
	if (MatchedCount == 0 && Rules.Num() == 0)
	{
		Aggregated.Reason = TEXT("the active run carries no rules");
	}
	return Aggregated;
}

TArray<FRoomRuleDecision> FSceneRuleEvaluator::AdvanceClock(double NowSeconds)
{
	TArray<FRoomRuleDecision> Decisions;
	if (!FMath::IsFinite(NowSeconds))
	{
		// Injected-clock contract: non-finite injections are ignored, the
		// clock keeps its previous value.
		return Decisions;
	}
	if (bDispatching)
	{
		// A handler advancing the clock during dispatch is the same reentrancy
		// hazard as a nested submit: refused, nothing fires.
		return Decisions;
	}
	// Monotonic by construction: a rewind keeps the older (larger) value, so
	// a rewound clock freezes rather than re-firing dues.
	ClockSeconds = FMath::Max(ClockSeconds, NowSeconds);
	if (ActiveRunId == 0)
	{
		return Decisions;
	}

	// Collect the due commands first (arm order), then dispatch after the
	// pending list is updated - a re-armed rule during dispatch can never
	// alias with the entry being executed.
	TArray<FPendingCommand> Due;
	for (int32 Index = PendingCommands.Num() - 1; Index >= 0; --Index)
	{
		if (ClockSeconds + M5_072_DueEpsilon >= PendingCommands[Index].DueSeconds)
		{
			Due.Insert(PendingCommands[Index], 0);
			PendingCommands.RemoveAt(Index);
		}
	}
	for (const FPendingCommand& Pending : Due)
	{
		FRoomRuleCommand Command;
		Command.RuleId = Pending.RuleId;
		Command.Effect = Pending.Effect;
		Command.WaveIndex = Pending.WaveIndex;
		Command.RunId = ActiveRunId;
		Decisions.Add(DispatchCommand(Command, nullptr));
	}
	return Decisions;
}
