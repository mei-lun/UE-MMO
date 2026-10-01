#pragma once

#include "CoreMinimal.h"
#include "Containers/UnrealString.h"
#include "Misc/DateTime.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/Function.h"
#include "UObject/NameTypes.h"

#include "OperationLogSubsystem.generated.h"

class APrototypeCharacter;
class UCombatComponent;
class URoomSessionSubsystem;

/**
 * M3-023: the operation log (user-requested 2026-09-30). One UGameInstanceSubsystem
 * records four kinds of discrete facts into one text file under
 * ProjectSavedDir/OperationLogs/OperationLog.log:
 * 1. player operations (attack start/end, launcher, jump cancel, equip, claim,
 *    room enter/leave, reset) and keyboard input (every discrete press and
 *    release; movement axes only on DIRECTION changes, never per frame),
 * 2. feature state (combat ActionState, room state, level/XP, save results),
 * 3. keyboard input rows (category "Input"),
 * 4. program responses (damage, level-up, drops, save results, failure
 *    reasons) (category "Response").
 *
 * Line format: "[HH:MM:SS.mmm][Category] Message" - the timestamp comes from
 * the injectable now provider (FDateTime::Now() by default; tests pin fake
 * dates), and every line is APPENDED to the file with its own write call
 * (FFileHelper + FILEWRITE_Append), so a crash can only ever lose the line
 * being written, never the file.
 *
 * DAILY ROLLOVER (the user's explicit requirement): the subsystem keeps the
 * ActiveLogDate (the date part of the last write, local time). Every Log call
 * first compares the provider's current date against it - on a change the log
 * file is DELETED, ActiveLogDate is updated and a fresh header line
 * "=== Operation log started YYYY-MM-DD (build) ===" is written, so each day
 * starts an empty log. Local date semantics match the user's intuition (the
 * log empties when the user's calendar day changes).
 *
 * RegisterPlayer(APrototypeCharacter*) auto-subscribes the log to the player's
 * CombatComponent (Started/Finished/HitConfirmed) and to the owning world's
 * RoomSessionSubsystem (OnRunStarted/OnRunEnded). All bindings are weak-lambda
 * style (a captured TWeakObjectPtr to this subsystem is re-checked on every
 * broadcast) and the handles are remembered so UnregisterPlayer can remove
 * them; a destroyed player or world simply takes its delegates with it, so a
 * stale binding can never dangle. Every Log entry point is safe without a
 * registered player, without a world and without a combat component: the call
 * is skipped, nothing crashes.
 *
 * Volume rule (task card): only discrete events are logged. The continuous
 * movement axis values are NEVER logged per frame - only direction changes
 * (any nonzero<->zero transition or sign flip) produce one input row.
 */
UCLASS()
class UEMMO_API UOperationLogSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// -- Lifecycle -------------------------------------------------------------

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Resolves the log subsystem for any context object: the outer chain is
	 * searched for the owning UGameInstance first (game instance subsystems and
	 * everything created below them), then the context's world is asked for its
	 * game instance (actors, components, world subsystems). Returns null when
	 * no game instance can be reached - callers skip the log silently.
	 */
	static UOperationLogSubsystem* FindForContext(const UObject* Context);

	// -- Injectable wall clock ---------------------------------------------------

	/**
	 * Replaces the now source (tests pin fixed dates to drive the daily
	 * rollover deterministically). An empty function restores the default
	 * FDateTime::Now.
	 */
	void SetNowProvider(TFunction<FDateTime()> InProvider);

	/** Restores the default FDateTime::Now provider (same as an empty SetNowProvider). */
	void ResetNowProvider();

	// -- Unified logging API -------------------------------------------------------

	/** Appends one line with the given category after the daily-rollover check. */
	void Log(FName Category, const FString& Message);

	/** Category-fixed convenience wrappers (the three card categories). */
	void LogInput(const FString& Message);
	void LogState(const FString& Message);
	void LogResponse(const FString& Message);

	// -- Player auto-subscription --------------------------------------------------

	/**
	 * Binds the log to one player's combat events (CombatComponent
	 * Started/Finished/HitConfirmed) and to the player's world's room session
	 * events (OnRunStarted/OnRunEnded). A second call (a re-register, e.g. a
	 * re-possessed pawn or the test's explicit call after BeginPlay's automatic
	 * one) unbinds the previous set first, so exactly one binding set exists.
	 * Null is refused (returns false, nothing changes).
	 */
	bool RegisterPlayer(APrototypeCharacter* Player);

	/** Removes every binding RegisterPlayer created (safe to call unregistered). */
	void UnregisterPlayer();

	/** True while a registered player binding set exists. */
	bool IsPlayerRegistered() const;

	// -- Introspection (tests and diagnostics) ---------------------------------------

	/** The log file path (ProjectSavedDir/OperationLogs/OperationLog.log). */
	const FString& GetLogFilePath() const;

	/** The date part of the currently open log day (invalid before the first write). */
	const FDateTime& GetActiveLogDate() const;

private:
	/**
	 * The daily-rollover gate of every write: when the provider's date differs
	 * from ActiveLogDate (or no day is open yet) the file is deleted, the new
	 * day is opened and the fresh header line is written. Always returns true
	 * today (kept bool so a future write-refusal path fits the same shape).
	 */
	bool M3_023_EnsureDailyFile(const FDateTime& Now);

	/** Writes the "=== Operation log started YYYY-MM-DD (build) ===" line. */
	void M3_023_WriteRolloverHeader(const FDateTime& Now);

	/** Appends one line (with line terminator) to the file in one write call. */
	void M3_023_AppendLine(const FString& Line);

	/** Removes the combat/room delegate bindings (weak references judged null). */
	void M3_023_UnbindAll();

	/** The OnRunStarted handler body: reads the ids off the bound session. */
	void M3_023_LogRoomRunStarted();

	/** Log file path, computed once in Initialize. */
	FString LogFilePath;

	/** The date part of the open log day; invalid until the first write. */
	FDateTime ActiveLogDate;

	/** Injectable now source; empty = FDateTime::Now(). */
	TFunction<FDateTime()> NowProvider;

	/** The registered player and its combat component (weak; lifetime anchors). */
	TWeakObjectPtr<APrototypeCharacter> RegisteredPlayerPtr;
	TWeakObjectPtr<UCombatComponent> RegisteredCombatPtr;

	/** The room session the run events were bound on (weak; world lifetime). */
	TWeakObjectPtr<URoomSessionSubsystem> RoomSessionPtr;

	/** Binding handles of RegisterPlayer (removed by UnregisterPlayer). */
	FDelegateHandle CombatStartedHandle;
	FDelegateHandle CombatFinishedHandle;
	FDelegateHandle CombatHitHandle;
	FDelegateHandle RoomStartedHandle;
	FDelegateHandle RoomEndedHandle;
};
