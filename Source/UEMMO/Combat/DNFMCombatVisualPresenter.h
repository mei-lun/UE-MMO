#pragma once

#include "CoreMinimal.h"
#include "CombatComponent.h"
#include "Delegates/Delegate.h"

class UDNFMCharacterVisualComponent;

/**
 * M6-023: one row of an attack's display segment table (plan section 6.2).
 * While the attack's logic frame sits in the half-open range
 * [LogicStartFrame, LogicEndFrame) the DNFM display shows the source clip's
 * display frame DisplayFrameIndex. The rows are the hand-tuned mapping of
 * "起手段/命中段/收招段" onto the frozen combat windows: bHitPose marks the
 * row that must sit INSIDE the attack's active window (the hard constraint;
 * the automation suite validates every row against the combat-attacks.json
 * facts). The display layer never mutates those windows.
 */
struct FDNFMAttackDisplaySegment
{
	/** First logic frame covered (inclusive). */
	int32 LogicStartFrame = 0;

	/** Frame after the last covered logic frame (exclusive). */
	int32 LogicEndFrame = 0;

	/** The source clip's display frame shown across the range. */
	int32 DisplayFrameIndex = 0;

	/** True on the visual hit-pose row (must sit inside the active window). */
	bool bHitPose = false;
};

/**
 * M6-023: the display mapping of one combat attack (Data/combat-attacks.json
 * attack_id) onto one M6-020 manifest action (the source .ani's base name).
 * Pure data: the table adjusts DISPLAY only - combat damage/timing/windows
 * stay untouched (plan section 6.2: "画面切换不扣血/不给无敌/不解锁移动").
 */
struct FDNFMAttackDisplayMapping
{
	/** The combat attack id (never mutated here). */
	FName AttackId;

	/** The M6-020 manifest action id that renders the attack. */
	FName ActionId;

	/** Segment rows covering [0, DurationFrames) in logic frame order. */
	TArray<FDNFMAttackDisplaySegment> Segments;
};

/**
 * M6-023: one deduplicated visual frame event row (the presenter's output).
 * The dedup key is (ActorId, AttackInstanceId, EventId) with
 * EventId = (ActionName, FrameIndex, EventTag, EventName) - the plan section
 * 6.2 tuple. AttackInstanceId is the combat instance the presenter was
 * presenting when the display fired the event (0 for the non-attack display
 * states).
 */
struct FDNFMRecordedVisualEvent
{
	uint64 ActorId = 0;
	uint64 AttackInstanceId = 0;
	FName ActionName;
	int32 FrameIndex = INDEX_NONE;
	FName EventTag;
	FName EventName;
};

/** Post-dedup visual event feed (M6-023 consumers; the tests observe it). */
DECLARE_MULTICAST_DELEGATE_SixParams(FDNFMDedupedVisualEventBroadcast,
	uint64 /*ActorId*/, uint64 /*AttackInstanceId*/, FName /*ActionName*/,
	int32 /*FrameIndex*/, FName /*EventTag*/, FName /*EventName*/);

namespace UE::UEMMO::Tasks::M6_023
{
	// M6-023: the deduplicated event history cap (diagnostics bookkeeping, the
	// FCombatAudioDispatcher precedent - long sessions stay bounded).
	constexpr int32 MaxDedupedEventHistory = 64;

	// The non-attack display action ids (the M6-020 manifest action ids are
	// the source .ani base names). The resource keys follow the M6-019
	// located facts (Docs/Tasks/Reports/M6-019.md): hit = damage1/damage2
	// (1-frame holds), knockdown = down.ani, getup = overturn.ani, death =
	// die.ani (2 frames 570 ms), idle = rest.ani, move = move.ani, jump =
	// jump.ani. Per-frame durations/phase mapping are calibrated against the
	// M6-020 manifest after the M6-022 import ("以 manifest 导入后校准").
	UEMMO_API extern const FName IdleActionId;      // rest.ani (loop)
	UEMMO_API extern const FName MoveActionId;      // move.ani (loop)
	UEMMO_API extern const FName JumpActionId;      // jump.ani (takeoff/air/landing phases; placeholder: once-clip hold)
	UEMMO_API extern const FName HitActionId;       // damage1.ani (1-frame hold)
	UEMMO_API extern const FName HitAltActionId;    // damage2.ani (fallback hit pose)
	UEMMO_API extern const FName DownActionId;      // down.ani (lying holds)
	UEMMO_API extern const FName RecoverActionId;   // overturn.ani (1-frame getup start)
	UEMMO_API extern const FName DeathActionId;     // die.ani (holds the end frame)

	/**
	 * The default attack display mapping table (plan section 6.2/6.3). The
	 * source frame counts and totals are the M6-019 db_queries facts; every
	 * bHitPose row covers its attack's active window exactly, so the visual
	 * hit pose always falls inside the existing damage window. Segment splits
	 * inside the startup/recovery regions are placeholders calibrated after
	 * the M6-022 import ("以 manifest 导入后校准"); combat-attacks.json is
	 * never read or written here.
	 */
	UEMMO_API TArray<FDNFMAttackDisplayMapping> MakeDefaultAttackDisplayMappings();

	/** The mapping row for AttackId, or nullptr. */
	UEMMO_API const FDNFMAttackDisplayMapping* FindAttackDisplayMapping(
		const TArray<FDNFMAttackDisplayMapping>& Table, FName AttackId);

	/**
	 * The display frame current at LogicFrame: the segment row containing the
	 * frame, clamped at both ends (a pre-first-tick Frame of -1 reads as the
	 * first row, an overrun reads as the last row). INDEX_NONE for an empty
	 * table. Deterministic pure math (the sampling never touches World time).
	 */
	UEMMO_API int32 FindDisplayFrameForLogicFrame(const FDNFMAttackDisplayMapping& Mapping, int32 LogicFrame);
}

/**
 * M6-023: the event dedup key (the plan's (ActorId, AttackInstanceId,
 * EventId) tuple with the event identity as the EventId face).
 */
struct FDNFMVisualEventKey
{
	uint64 ActorId = 0;
	uint64 AttackInstanceId = 0;
	FName ActionName;
	int32 FrameIndex = INDEX_NONE;
	FName EventTag;
	FName EventName;

	bool operator==(const FDNFMVisualEventKey& Other) const
	{
		return ActorId == Other.ActorId
			&& AttackInstanceId == Other.AttackInstanceId
			&& ActionName == Other.ActionName
			&& FrameIndex == Other.FrameIndex
			&& EventTag == Other.EventTag
			&& EventName == Other.EventName;
	}
};

inline uint32 GetTypeHash(const FDNFMVisualEventKey& Key)
{
	uint32 Hash = ::GetTypeHash(Key.ActorId);
	Hash = HashCombine(Hash, ::GetTypeHash(Key.AttackInstanceId));
	Hash = HashCombine(Hash, GetTypeHash(Key.ActionName));
	Hash = HashCombine(Hash, ::GetTypeHash(Key.FrameIndex));
	Hash = HashCombine(Hash, GetTypeHash(Key.EventTag));
	Hash = HashCombine(Hash, GetTypeHash(Key.EventName));
	return Hash;
}

/**
 * M6-023: the DNFM display-target coordinator (plan section 6.1). The plan
 * keeps UCombatPresentationComponent as the presentation dispatch entry; this
 * plain C++ presenter (the FCombatAudioDispatcher precedent) owns the DNFM
 * branch so the 3D montage path stays byte-identical and the state machine,
 * segment sampling and event dedup stay world-less unit-testable. The owning
 * component feeds it the existing facts every tick; the presenter drives the
 * injected UDNFMCharacterVisualComponent through SetAction/SeekToDisplayFrame/
 * SetPlaybackFrozen - it builds no second clock and never mutates gameplay
 * state (no damage, no invulnerability, no movement unlock).
 *
 * Display priority (plan section 6.1, exact):
 *   death -> down/getup/hit -> current attack -> jump phase -> ground
 *   move/idle. Every input is an existing fact (combat snapshot, health,
 *   movement); interrupts stay with Combat/Health judgment.
 *
 * Timing contract: the attack display is sampled from
 * (AttackId, InstanceId, Frame) - the combat component's 60 Hz logic frame -
 * never from accumulated World time. Real-time clips (idle/move loops and the
 * once reaction clips) advance through the visual component's own playback,
 * which the presenter freezes for the HitStop span (OnHitStopChanged); a
 * presented attack keeps its time external for its whole life, so HitStop and
 * attack sampling compose (frozen logic frame -> frozen display frame, no
 * wall-clock drift, no frozen-span event replay).
 */
class FDNFMCombatVisualPresenter
{
public:
	/** The display facts the owning component gathers (all optional sources). */
	struct FVisualFacts
	{
		/** The combat snapshot (the Free default without a combat source). */
		FCombatSnapshot Combat;

		/** The owner's death fact (health OR combat dead). */
		bool bDead = false;

		/** The owner's airborne fact (the movement component's IsFalling). */
		bool bAirborne = false;

		/** The owner's ground-move fact (horizontal speed above threshold). */
		bool bGroundMoving = false;
	};

	FDNFMCombatVisualPresenter();
	~FDNFMCombatVisualPresenter();

	FDNFMCombatVisualPresenter(const FDNFMCombatVisualPresenter&) = delete;
	FDNFMCombatVisualPresenter& operator=(const FDNFMCombatVisualPresenter&) = delete;

	/**
	 * Binds the picture surface (and its frame-event feed) and forgets the
	 * previous display state; null unbinds and leaves the visual untouched
	 * (the caller owns the gate teardown). Idempotent.
	 */
	void SetVisualTarget(UDNFMCharacterVisualComponent* InVisual);

	UDNFMCharacterVisualComponent* GetVisualTarget() const { return VisualTarget.Get(); }

	/**
	 * Overrides the attack display mapping table (config/tests); the default
	 * is MakeDefaultAttackDisplayMappings(). Pure display configuration -
	 * combat windows stay untouched either way.
	 */
	void SetAttackDisplayMappings(const TArray<FDNFMAttackDisplayMapping>& InTable) { AttackDisplayMappings = InTable; }

	/** The active attack display mapping table (read-only observation). */
	const TArray<FDNFMAttackDisplayMapping>& GetAttackDisplayMappings() const { return AttackDisplayMappings; }

	/**
	 * The one per-tick sync: computes the desired display target from the
	 * facts (priority above) and applies only the diffs. No-op without a
	 * bound visual (the gate=0 no-op face).
	 */
	void Step(const FVisualFacts& Facts);

	/**
	 * The OnHitStopChanged face: freezes/unfreezes the display time advance
	 * (the visual's playback). A presented attack stays time-external
	 * regardless; the freeze only matters for the real-time clips.
	 */
	void NotifyHitStopFrozen(bool bFrozen);

	bool IsHitStopFrozen() const { return bHitStopFrozen; }

	/**
	 * Full presenter teardown (gate off / EndPlay): unbinds the event feed and
	 * forgets every display state. Does not touch the visual's playback (the
	 * gate teardown calls ResetVisual on it).
	 */
	void Reset();

	/** Post-dedup visual events (in dispatch order). */
	FDNFMDedupedVisualEventBroadcast OnDedupedVisualFrameEvent;

	/** Deduplicated event history, oldest first (bounded; tests observe). */
	const TArray<FDNFMRecordedVisualEvent>& GetDedupedEventHistory() const { return DedupedEventHistory; }

	/** The action id the presenter last successfully presented (None when none). */
	FName GetPresentedActionId() const { return PresentedActionId; }

	/** The combat instance the presenter currently presents (0 = non-attack). */
	uint64 GetPresentedInstanceId() const { return PresentedInstanceId; }

private:
	/**
	 * One deduplicated event row: accepts only events of the action the
	 * presenter actually presented, dedups on the plan tuple, records and
	 * forwards the first occurrence.
	 */
	void HandleVisualFrameEvent(uint64 ActorId, FName ActionName, int32 FrameIndex,
		FName EventTag, FName EventName);

	/** Enters (or stays in) a state action with its fallback chain. */
	void ApplyStateAction(FName PrimaryAction, const TArray<FName>& Fallbacks, bool bOnce);

	/** Starts (new instance) or repositions (logic frame) the attack display. */
	void ApplyAttackDisplay(const FDNFMAttackDisplayMapping& Mapping, const FVisualFacts& Facts);

	/** Pushes the composed time-ownership state to the visual. */
	void PushTimeOwnership();

	/** Drops the presented instance + event space (cancel/switch/revive face). */
	void ForgetPresentedInstance();

	/** Once-per-id diagnostics keeping the log readable. */
	bool WarnMissingActionOnce(FName ActionId, const TCHAR* Reason);

	TWeakObjectPtr<UDNFMCharacterVisualComponent> VisualTarget;
	FDelegateHandle VisualEventHandle;

	/** The attack display mapping table (the default frozen table or an override). */
	TArray<FDNFMAttackDisplayMapping> AttackDisplayMappings;

	/** What the presenter last presented (the event-acceptance filter). */
	FName PresentedActionId = NAME_None;
	uint64 PresentedInstanceId = 0;

	/** The composed time ownership pushed to the visual. */
	bool bHitStopFrozen = false;
	bool bAttackTimeExternal = false;

	/** The dead edge (the revive cleanup reads true -> false). */
	bool bObservedDead = false;

	/** The dedup set of the CURRENT instance (cleared on instance change). */
	TSet<FDNFMVisualEventKey> InstanceEventKeys;

	/** Post-dedup history (bounded). */
	TArray<FDNFMRecordedVisualEvent> DedupedEventHistory;

	/** Action ids already diagnosed as missing (one log each). */
	TSet<FName> WarnedMissingActionIds;
};
