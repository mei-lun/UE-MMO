#pragma once

#include "CoreMinimal.h"
#include "Containers/UnrealString.h"
#include "Misc/DateTime.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/Function.h"
#include "UObject/NameTypes.h"

#include "../Combat/System/CombatEventTypes.h"

#include "OperationLogSubsystem.generated.h"

class APrototypeCharacter;
class UCombatComponent;
class URoomSessionSubsystem;
class FShotChainStore;
struct FShotContext;
struct FUnifiedHitOutcome;
struct FUnifiedHitRequest;

// ===========================================================================
// M5-053: the shot event-chain value types
//
// The unified hit entry (M5-012) and the weapon mount (M5-022) already emit
// the per-shot facts at their own link points; this face joins them into one
// traceable chain keyed by (Epoch, SourceEntityId, ShotId) - the pellet-free
// prefix of the FCombatEventKey - so one shot's admission, projectiles,
// hits/filters, damage, reactions, deaths and cleanup are answerable from a
// single id. Everything here is a plain value snapshot (the same rule as the
// frozen combat event types): ids are the native integer aliases, never a
// pointer or an actor address, and a name (driver/vehicle) is recorded as a
// string snapshot taken at commit time, never as a reference.
// ===========================================================================

/**
 * The stage of one chain event row. Append only - never renumber, so log
 * lines and tests keep their meaning.
 */
enum class EShotChainStage : uint8
{
	/** The weapon mount committed the shot (BeginShotChain). */
	ShotCommitted = 0,
	/** The unified entry admitted (Approved) or refused (named reason) the hit. */
	Admission = 1,
	/** One projectile lifecycle fact (spawn / per-frame checkpoint / end). */
	Projectile = 2,
	/** The hit connected on one pellet/target (the filter passed it through). */
	Hit = 3,
	/** The damage face of one applied hit (applied value / immune). */
	Damage = 4,
	/** The control face (stagger/launch/knockdown accepted by the policy). */
	Reaction = 5,
	/** The target died from this hit. */
	Death = 6,
	/** The chain closed (ReleaseFire / active-chain LRU eviction). */
	Cleanup = 7,
};

/**
 * Row severity, and with it the sampling/drop policy: Info rows of the
 * sample-eligible stages may be sampled down and are dropped first on
 * overflow; Refusal and Critical (error/death) rows are NEVER sampled, never
 * overflow-dropped and stay recordable even after the chain ended - the
 * card's "dedup/sampling must not delete error chains" rule.
 */
enum class EShotChainSeverity : uint8
{
	/** A normal discrete fact (subject to sampling/overflow policy). */
	Info = 0,
	/** A named refusal (never sampled, never dropped). */
	Refusal = 1,
	/** An error or a death (never sampled, never dropped). */
	Critical = 2,
};

/**
 * How a chain left its open state. Append only. A chain hard-dropped by the
 * tracked-chain window has no end reason - it is simply no longer findable.
 */
enum class EShotChainEndReason : uint8
{
	/** The chain is still open (accepting rows). */
	StillOpen = 0,
	/** EndShotChain: the shot's in-flight window ended (ReleaseFire). */
	Released = 1,
	/** The active-chain cap force-closed this stale open chain (LRU). */
	ActiveEvicted = 2,
};

/** One stage record submitted to the chain (the LogShotStage payload). */
struct FShotStageRecord
{
	/** Which link point produced the row. */
	EShotChainStage Stage = EShotChainStage::Admission;

	/** Sampling/drop class of the row (see EShotChainSeverity). */
	EShotChainSeverity Severity = EShotChainSeverity::Info;

	/** The pellet the row belongs to; the sentinel for shot-wide rows. */
	FPelletIndex PelletIndex = InvalidCombatPelletIndex;

	/** The target the row belongs to; 0 for shot-wide rows. */
	FTargetId TargetId = InvalidCombatTargetId;

	/** The stage verdict (Committed/Approved/Connected/Applied/Died/... or the refusal reason name). */
	FName Result;

	/** Human-readable detail (why refused, which controls, ...). */
	FString Detail;

	/** Optional numeric payload (the applied damage or the refuse-reason enum value). */
	double NumericValue = 0.0;
	bool bHasNumericValue = false;
};

/** One recorded chain event row (the GetShotChainEvents element). */
struct FShotChainEvent
{
	// -- The chain identity (repeated on every row so a flat dump stays self-contained) --
	FCombatEpoch Epoch = InvalidCombatEpoch;
	FEntityId SourceEntityId = InvalidCombatEntityId;
	FShotId ShotId = InvalidCombatShotId;

	/** The pellet the row belongs to (the sentinel for shot-wide rows). */
	FPelletIndex PelletIndex = InvalidCombatPelletIndex;

	/** The target the row belongs to (0 for shot-wide rows). */
	FTargetId TargetId = InvalidCombatTargetId;

	EShotChainStage Stage = EShotChainStage::ShotCommitted;
	EShotChainSeverity Severity = EShotChainSeverity::Info;
	FName Result;
	FString Detail;

	/** Optional numeric payload (damage value / refuse-reason enum). */
	double NumericValue = 0.0;
	bool bHasNumericValue = false;

	/** The chain's config revision snapshot (empty on auto-opened chains). */
	FString ConfigRevision;

	/** The injectable now provider's timestamp at record time (the time domain). */
	FDateTime RecordedAt;

	/** How many identical (stage, pellet, target, verdict) calls merged into this row. */
	uint32 RepeatCount = 1;
};

/** Read-only chain state for diagnostics and tests. */
struct FShotChainSummary
{
	FCombatEpoch Epoch = InvalidCombatEpoch;
	FEntityId SourceEntityId = InvalidCombatEntityId;
	FShotId ShotId = InvalidCombatShotId;

	/** Opening metadata (empty for auto-opened chains). */
	FName WeaponDefinitionId;
	FString ConfigRevision;

	/**
	 * Best-effort operator/source-actor name snapshots from the committing
	 * caller (empty when the caller cannot see them; the pure log face never
	 * invents them). Pawn-held mounts pass the wielder; vehicle mounts may
	 * pass the hull and its seat occupant.
	 */
	FString DriverName;
	FString VehicleName;

	bool bEnded = false;
	EShotChainEndReason EndReason = EShotChainEndReason::StillOpen;

	/** Recorded rows (dedup merges do not add rows). */
	int32 EventCount = 0;

	/** Info rows dropped by sampling/overflow/post-end policy - the sampling budget audit face. */
	int32 DroppedInfoCount = 0;

	FDateTime FirstRecordedAt;
	FDateTime LastRecordedAt;
};

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
 *
 * M5-053 ADDITION - the shot event-chain face: the same subsystem also keeps
 * one traceable in-memory event chain per committed shot, keyed by the
 * pellet-free (Epoch, SourceEntityId, ShotId) prefix of the FCombatEventKey.
 * The weapon mount's commit opens the chain (BeginShotChain), the unified hit
 * entry's outcome feeds the admission/hit/damage/reaction/death rows
 * (LogUnifiedHitOutcome, pure observation) and the shot release closes it
 * (EndShotChain). Rows dedup by (stage, pellet, target, verdict), high-
 * frequency Info rows of the Projectile stage may be sampled down, and
 * refusal/error/death rows are never sampled, never dropped and stay
 * recordable past the chain's end. The chain store is bounded (active-chain
 * LRU cap, retention window, per-chain row cap) and every bound drop is
 * counted on the chain summary, so the sampling budget stays auditable. The
 * text file face above receives one summary line per chain closure.
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

	// -- M5-053: the shot event-chain face --------------------------------------
	//
	// One traceable chain per (Epoch, SourceEntityId, ShotId). The face is
	// PULLED by call sites (the weapon mount's commit, the unified hit entry's
	// outcome) - the subsystem holds no per-chain delegate subscriptions, so
	// there is nothing to unbind per chain; a chain's lifecycle only moves it
	// between open and ended (ended chains stay queryable inside the retention
	// window). Game thread only, like every other face of this subsystem.

	/**
	 * Opens (or re-touches) the chain of one committed shot and records its
	 * ShotCommitted row. ConfigRevision snapshots the mounted weapon catalog
	 * revision; DriverName/VehicleName are best-effort name snapshots the
	 * caller may leave empty (a pawn-held mount passes the wielder, a vehicle
	 * mount may pass hull + occupant). A re-begin of a live chain merges into
	 * the existing one (dedup) and never wipes it. Returns false only for an
	 * unusable (Epoch, Source, Shot) identity.
	 */
	bool BeginShotChain(const FShotContext& Shot, const FString& ConfigRevision,
		const FString& DriverName = FString(), const FString& VehicleName = FString());

	/**
	 * Records one stage row on the chain; an unknown chain is auto-opened
	 * (empty metadata) so a hit pipeline that runs without the weapon-mount
	 * commit still lands on one chain. Repeated identical
	 * (stage, pellet, target, verdict) rows merge into the first row's
	 * RepeatCount. Returns true when the row was recorded or merged, false
	 * when the identity is unusable or the row was dropped by the sampling/
	 * overflow policy (refusal/critical rows are never dropped).
	 */
	bool LogShotStage(FCombatEpoch Epoch, FEntityId Source, FShotId Shot, const FShotStageRecord& Record);

	/**
	 * The unified hit entry's observation facade: turns one FUnifiedHitOutcome
	 * into the chain's admission row (Approved, or Refused with the named
	 * applier/ledger/resolver reasons and the applier reason as the numeric
	 * payload), plus the hit, damage (applied value), reaction and death rows
	 * for an admitted hit. Pure observation - it never touches the outcome.
	 * A request without a usable event key has no chain identity and is
	 * skipped (the entry's UnusableKey refusal stays at the call site only).
	 */
	void LogUnifiedHitOutcome(const FUnifiedHitRequest& Request, const FUnifiedHitOutcome& Outcome);

	/**
	 * Closes an open chain with its Cleanup row (Released). Completed chains
	 * stay queryable inside the retention window; late refusal/critical rows
	 * still land on an ended chain, Info rows no longer do (counted as
	 * dropped). False for an unknown or already ended chain.
	 */
	bool EndShotChain(FCombatEpoch Epoch, FEntityId Source, FShotId Shot);

	/** The event rows of one chain in record order (empty when unknown/evicted). */
	TArray<FShotChainEvent> GetShotChainEvents(FCombatEpoch Epoch, FEntityId Source, FShotId Shot) const;

	/** The chain's summary state; false when the chain is unknown (never opened or dropped). */
	bool FindShotChainSummary(FCombatEpoch Epoch, FEntityId Source, FShotId Shot, FShotChainSummary& OutSummary) const;

	/**
	 * Sampling stride for Info rows of sample-eligible stages (the per-frame
	 * Projectile lifecycle): 1 records every row (default), N records the
	 * first of every N distinct occurrences. Refusal/critical rows ignore it.
	 */
	void SetInfoSampleStride(int32 Stride);
	int32 GetInfoSampleStride() const;

	/** Number of tracked chains (open + ended, inside the retention window). */
	int32 GetNumTrackedShotChains() const;

	/** Number of open chains (the LRU-capped population). */
	int32 GetNumActiveShotChains() const;

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

	/**
	 * The M5-053 chain store (opaque, defined in the .cpp): the bounded LRU
	 * map of chains with the dedup/sampling/retention policy. A raw owning
	 * pointer (NOT TUniquePtr: UHT generates an inline constructor in the
	 * gen.cpp, which would need the complete type to destroy a unique-ptr
	 * member). The destructor is declared here and defined in the .cpp, which
	 * deletes the store.
	 */
	FShotChainStore* M5_053_ChainStore = nullptr;

public:
	virtual ~UOperationLogSubsystem() override;

private:
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
