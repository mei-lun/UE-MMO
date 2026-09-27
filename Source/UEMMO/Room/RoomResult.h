// M2-013: the result data of one finished room run, migrated VERBATIM from
// RoomSessionSubsystem.h (where M2-006 defined it) into its own header so the
// pure value type can be included without pulling in the whole session
// subsystem. Field names, defaults and semantics are UNCHANGED - M2-006's
// tests (RoomSessionTests.cpp) keep passing untouched.
//
// Settlement consumption contract (M3 preview, deliberately NOT implemented
// here): the future M3 reward service claims a settlement exactly once by
// SettlementId (FPendingReward / UProfileSaveGame, interface contract section
// 8). This card only guarantees the id exists, is stable and is queryable
// through URoomSessionSubsystem's run result archive; it stores no save game
// and grants no rewards. Only a Cleared result is ever reward eligible
// (IsRewardEligible) - a Failed result can never enter the reward path.

#pragma once

#include "CoreMinimal.h"

/**
 * Result data of one finished run (interface contract section 7).
 * Pure value type on purpose: the room session never stores an inventory or
 * any other cross-run state - it only produces this result (M2-006 card).
 * M2-013 hands finished results out as immutable VALUE copies
 * (URoomSessionSubsystem::GetRunResult), so a UI holding one can never dangle
 * when the world - and the session instance behind any reference - goes away.
 */
struct FRoomResult
{
	/** Monotonic run id inside the owning session; 0 = no run. */
	uint64 RunId = 0;

	/** Process-global unique settlement id; never repeats across runs or worlds. */
	uint64 SettlementId = 0;

	/** RoomId of the definition the run was started with. */
	FName RoomId;

	/** Deterministic seed derived from RunId (fixed formula, no randomness). */
	int32 Seed = 0;

	/** True when the run ended Cleared, false when it ended Failed. */
	bool bCleared = false;

	/** Enemies accepted through NotifyEnemyKilled while the run was Running. */
	int32 KilledCount = 0;

	/** Run duration in seconds, measured with the injected session clock. */
	double ElapsedSeconds = 0.0;
};
