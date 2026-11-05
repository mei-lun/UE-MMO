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
	// jump.ani. M6-024 calibrated every phase walk against the frozen M6-020
	// manifest (composite/manifest.json, real per-frame durations).
	UEMMO_API extern const FName IdleActionId;      // rest.ani (loop)
	UEMMO_API extern const FName MoveActionId;      // move.ani (loop)
	UEMMO_API extern const FName JumpActionId;      // jump.ani (takeoff/air/landing phases; M6-024 state-driven walk)
	UEMMO_API extern const FName HitActionId;       // damage1.ani (1-frame hold)
	UEMMO_API extern const FName HitAltActionId;    // damage2.ani (alternating hit pose)
	UEMMO_API extern const FName DownActionId;      // down.ani (lying holds)
	UEMMO_API extern const FName RecoverActionId;   // overturn.ani (1-frame getup start)
	UEMMO_API extern const FName DeathActionId;     // die.ani (holds the end frame)

	// M6-024: the two getup roll clips of the Recovering sequence (the M6-020
	// manifest carries both directions, 8 frames 630 ms each). The presenter's
	// direction choice is documented at the Recovering branch: the display
	// layer receives no per-hit direction fact (the victim-side snapshot and
	// health component carry none), so the deterministic default is the back
	// roll - every current knockdown source pushes the victim away from the
	// attacker, and the DNF-style getup roll continues in that knockback
	// direction. The front roll stays wired for a future direction fact.
	UEMMO_API extern const FName RecoverRollBackActionId;   // ftrecoveryrollback_body.ani
	UEMMO_API extern const FName RecoverRollFrontActionId;  // ftrecoveryrollfront_body.ani

	/**
	 * The default attack display mapping table (plan section 6.2/6.3). The
	 * source frame counts AND per-frame durations are the frozen M6-020
	 * manifest facts (composite/manifest.json); every bHitPose row covers its
	 * attack's active window exactly, so the visual hit pose always falls
	 * inside the existing damage window. The M6-024 calibration walks each
	 * startup/recovery logic region proportionally through the source frames'
	 * stored durations (logic frame = 1/60 s, midpoint sampling, both regions
	 * clamped), which REPLACED the M6-023 placeholder splits. combat-attacks.
	 * json is never read or written here.
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
 *   movement); interrupts stay with Combat/Health judgment. M6-024 refines
 *   two slots without moving the priority lines:
 *   - hit = damage1/damage2 alternating per HitStun episode (no per-hit
 *     direction fact reaches the display; alternation is the deterministic
 *     per-hit variety), and the getup is the SEQUENCE overturn -> roll clip
 *     (the roll keeps playing to its end even after Recovering ends - plan
 *     6.2 "序列播完回待机" - unless a new reaction, an attack or death takes
 *     the picture first);
 *   - the jump display is the state-driven three-phase walk takeoff (rising)
 *     -> air/fall frames -> landing tail, advanced ONLY by the tick delta -
 *     the jump.ani 34464 ms air-hold and 10000 ms landing-hold storage frames
 *     never gate the display (plan 6.2: 不能按存储等待值僵等).
 *
 * Timing contract: the attack display is sampled from
 * (AttackId, InstanceId, Frame) - the combat component's 60 Hz logic frame -
 * never from accumulated World time. Real-time clips (idle/move loops and the
 * once reaction clips) advance through the visual component's own playback,
 * which the presenter freezes for the HitStop span (OnHitStopChanged); a
 * presented attack keeps its time external for its whole life, so HitStop and
 * attack sampling compose (frozen logic frame -> frozen display frame, no
 * wall-clock drift, no frozen-span event replay). The M6-024 sequence walks
 * (jump phases, getup overturn+roll) pin the clip time entirely: the visual
 * playback is frozen and the presenter seeks the walked frame, so the
 * pathological stored hold frames cannot stall the picture.
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

		/**
		 * M6-024: the delta seconds of the tick that gathered these facts.
		 * The jump phase walk and the getup sequence advance ONLY by this
		 * amount (plan section 6.2: the display is pushed by the movement
		 * state, never by the stored >=10 s wait frames). Unit tests that
		 * call Step directly without a clock leave it 0, which keeps every
		 * phase walk exactly where it is.
		 */
		float DeltaSeconds = 0.0f;

		/**
		 * M6-024: true while the airborne owner still rises (the movement
		 * component's upward velocity). The jump display's takeoff -> fall
		 * phase switch reads it; it is meaningless while grounded.
		 */
		bool bAirborneRising = false;
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
	 * M6-024: the jump display's phase (plan section 6.2's takeoff/air/landing
	 * split). Takeoff walks the rising frames, Falling walks the fall frames,
	 * Landing walks the ground-edge tail; None means no jump display. The
	 * phase survives a higher-priority display (an aerial attack) and resumes
	 * when the priority returns, but a ground edge missed under a higher
	 * display is stale and clears instead of playing a late tail.
	 */
	enum class EDNFMJumpPhase : uint8
	{
		None = 0,
		Takeoff = 1,
		Falling = 2,
		Landing = 3
	};

	/**
	 * M6-024: the Recovering sequence phase (overturn pose hold, then the getup
	 * roll clip). The sequence starts on the Recovering edge and keeps playing
	 * to its end even after the combat state left Recovering (plan 6.2
	 * "序列播完回待机"); a new reaction/attack/death abandons it.
	 */
	enum class EDNFMRecoverPhase : uint8
	{
		None = 0,
		Overturn = 1,
		Roll = 2
	};

	/** The whole priority walk of one Step (Step does the edge bookkeeping). */
	void StepDisplay(const FVisualFacts& Facts);

	/**
	 * M6-024: the airborne jump display (Takeoff/Falling walk). bTakeoffEdge
	 * starts a fresh jump display instance; a stolen display rebuilds from the
	 * phase's current elapsed.
	 */
	void ApplyJumpAirDisplay(const FVisualFacts& Facts, bool bTakeoffEdge);

	/**
	 * M6-024: the grounded landing tail (frame 12 hold cap -> 13 -> 14).
	 * Returns true while the tail still owns the picture.
	 */
	bool ApplyJumpLandingDisplay(const FVisualFacts& Facts);

	/** M6-024: advances the overturn -> roll sequence by the tick delta. */
	void ApplyRecoverSequence(const FVisualFacts& Facts);

	/** M6-024: abandons the getup sequence (new reaction/attack/death/revive). */
	void ResetRecoverSequence();

	/**
	 * M6-024: the walked frame of DurationsMs at ElapsedMs (the frame covering
	 * the accumulated time; past the total holds the last walked frame).
	 */
	static int32 WalkFramesByElapsedMs(const int32* DurationsMs, int32 Count, double ElapsedMs);

	/** True while a jump phase or getup sequence currently pins the clip time. */
	bool HasPresenterPinnedDisplay() const
	{
		return JumpPhase != EDNFMJumpPhase::None || RecoverPhase != EDNFMRecoverPhase::None;
	}

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

	/** M6-024: the jump display phase and its walked elapsed time. */
	EDNFMJumpPhase JumpPhase = EDNFMJumpPhase::None;
	double JumpPhaseElapsedMs = 0.0;

	/** M6-024: the getup sequence phase, its elapsed time and chosen roll clip. */
	EDNFMRecoverPhase RecoverPhase = EDNFMRecoverPhase::None;
	double RecoverPhaseElapsedMs = 0.0;
	FName RecoverRollActionId = NAME_None;
	bool bRecoverSequenceDone = false;

	/** M6-024: the alternating hit pose flip (the first flip yields damage1). */
	bool bNextHitUsesAltPose = true;

	/** M6-024: the previous tick's facts (the takeoff/landing edges and the reaction edges). */
	bool bObservedAirborne = false;
	ECombatActionState PrevObservedState = ECombatActionState::Free;

	/** The dedup set of the CURRENT instance (cleared on instance change). */
	TSet<FDNFMVisualEventKey> InstanceEventKeys;

	/** Post-dedup history (bounded). */
	TArray<FDNFMRecordedVisualEvent> DedupedEventHistory;

	/** Action ids already diagnosed as missing (one log each). */
	TSet<FName> WarnedMissingActionIds;
};
