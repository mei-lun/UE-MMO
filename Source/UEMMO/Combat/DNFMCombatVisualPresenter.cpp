// M6-023: the DNFM display-target coordinator (plan section 6.1/6.2). This
// translation unit owns the display priority, the attack segment table and
// the event dedup; every input is an existing fact (Combat snapshot, Health,
// movement) and nothing here mutates gameplay state. The attack display is
// sampled from (AttackId, InstanceId, Frame) - the combat 60 Hz logic frame -
// never from a second World-time clock.
#include "DNFMCombatVisualPresenter.h"

#include "../Character/DNFMCharacterVisualComponent.h"

namespace UE::UEMMO::Tasks::M6_023
{
	const FName IdleActionId(TEXT("rest"));
	const FName MoveActionId(TEXT("move"));
	const FName JumpActionId(TEXT("jump"));
	const FName HitActionId(TEXT("damage1"));
	const FName HitAltActionId(TEXT("damage2"));
	const FName DownActionId(TEXT("down"));
	const FName RecoverActionId(TEXT("overturn"));
	const FName DeathActionId(TEXT("die"));
	// M6-024: the getup roll clips (the manifest's ftrecovery* action ids are
	// the source .ani base names, matching every other row's convention).
	const FName RecoverRollBackActionId(TEXT("ftrecoveryrollback_body"));
	const FName RecoverRollFrontActionId(TEXT("ftrecoveryrollfront_body"));

	namespace
	{
		// Segment-table shorthand (keeps the nine frozen tables readable).
		constexpr FDNFMAttackDisplaySegment Seg(int32 LogicStart, int32 LogicEnd, int32 DisplayFrame, bool bHitPose = false)
		{
			FDNFMAttackDisplaySegment Row;
			Row.LogicStartFrame = LogicStart;
			Row.LogicEndFrame = LogicEnd;
			Row.DisplayFrameIndex = DisplayFrame;
			Row.bHitPose = bHitPose;
			return Row;
		}

		/**
		 * Builds one attack mapping from explicit rows. The source frame
		 * counts AND per-frame durations in the comments are the frozen M6-020
		 * manifest facts (SourceAssets/Unpacked/DNFM130_5_8_202/Characters/
		 * Fighter/composite/manifest.json). Every row below is the M6-024
		 * calibration: the startup rows map logic frames [0, ActiveStart) and
		 * the recovery rows map [ActiveEnd, DurationFrames) proportionally
		 * through the source frames' stored durations (logic frame = 1000/60
		 * ms, midpoint sampling, each region clamped to its first/last source
		 * frame; source frames whose stored time falls inside the compressed
		 * region are skipped honestly). The hit-pose row covers the active
		 * window EXACTLY (the hard plan constraint the automation suite
		 * re-validates); the displayed hit-pose frame index per attack keeps
		 * the M6-023 choice (the extended attack pose before the recovery
		 * frames).
		 */
		FDNFMAttackDisplayMapping MakeMapping(FName AttackId, FName ActionId,
			const TArray<FDNFMAttackDisplaySegment>& Rows)
		{
			FDNFMAttackDisplayMapping Mapping;
			Mapping.AttackId = AttackId;
			Mapping.ActionId = ActionId;
			Mapping.Segments = Rows;
			return Mapping;
		}
	}

	TArray<FDNFMAttackDisplayMapping> MakeDefaultAttackDisplayMappings()
	{
		TArray<FDNFMAttackDisplayMapping> Table;

		// light_01: 26 logic frames, active [7,11) <- attack1.ani 3 frames
		// [100, 80, 220] ms (400 ms total; the plan section 6.2 worked example).
		// Startup 7 frames = 117 ms sits inside frame 0's 100 ms + clamp;
		// recovery 15 frames = 250 ms sits inside frame 2's 220 ms + clamp.
		Table.Add(MakeMapping(FName(TEXT("light_01")), FName(TEXT("attack1")),
			{
				Seg(0, 7, 0),          // 起手段: windup pose
				Seg(7, 11, 1, true),   // 命中段: hit pose == the active window
				Seg(11, 26, 2)         // 收招段: recovery pose
			}));

		// light_02: 32 logic frames, active [9,14) <- attack2.ani 4 frames
		// [80, 80, 80, 240] ms (480 ms total). Recovery: frame 2's 80 ms covers
		// [14,19), frame 3's 240 ms covers [19,32).
		Table.Add(MakeMapping(FName(TEXT("light_02")), FName(TEXT("attack2")),
			{
				Seg(0, 9, 0),
				Seg(9, 14, 1, true),
				Seg(14, 19, 2),
				Seg(19, 32, 3)
			}));

		// launcher: 40 logic frames, active [12,17) <- liftupper.ani 6 frames
		// [250, 70, 100, 70, 180, 70] ms (740 ms total). Startup 12 frames =
		// 200 ms sits inside frame 0's 250 ms (frames 1-2 are skipped); the
		// recovery 180 ms frame 4 covers [17,28), frame 5 covers [28,40).
		Table.Add(MakeMapping(FName(TEXT("launcher")), FName(TEXT("liftupper")),
			{
				Seg(0, 12, 0),
				Seg(12, 17, 3, true),
				Seg(17, 28, 4),
				Seg(28, 40, 5)
			}));

		// aerial_01: 28 logic frames, active [6,10) <- jumpattack.ani 4 frames
		// [20, 120, 50, 550] ms (740 ms total). Startup 6 frames = 100 ms
		// clamps into frame 0 (20 ms); recovery frame 2 (50 ms) covers [10,13),
		// frame 3 (550 ms) covers [13,28).
		Table.Add(MakeMapping(FName(TEXT("aerial_01")), FName(TEXT("jumpattack")),
			{
				Seg(0, 6, 0),
				Seg(6, 10, 1, true),
				Seg(10, 13, 2),
				Seg(13, 28, 3)
			}));

		// singlekick: 24 logic frames, active [11,15) <- singlekick.ani 7
		// frames [50, 20, 100, 150, 70, 70, 70] ms (530 ms total). Startup:
		// frame 0 [0,3), frame 1 (20 ms) [3,4), frame 2 (100 ms) [4,11);
		// recovery: frame 4 [15,19), frame 5 [19,23), frame 6 [23,24).
		Table.Add(MakeMapping(FName(TEXT("singlekick")), FName(TEXT("singlekick")),
			{
				Seg(0, 3, 0),
				Seg(3, 4, 1),
				Seg(4, 11, 2),
				Seg(11, 15, 3, true),
				Seg(15, 19, 4),
				Seg(19, 23, 5),
				Seg(23, 24, 6)
			}));

		// damagelowkick: 24 logic frames, active [11,15) <- damagelowkick.ani
		// 5 frames [80, 80, 50, 110, 80] ms (400 ms total). Startup: frame 0
		// [0,5), frame 1 [5,11); recovery: frame 3 (110 ms) [15,22), frame 4
		// [22,24).
		Table.Add(MakeMapping(FName(TEXT("damagelowkick")), FName(TEXT("damagelowkick")),
			{
				Seg(0, 5, 0),
				Seg(5, 11, 1),
				Seg(11, 15, 2, true),
				Seg(15, 22, 3),
				Seg(22, 24, 4)
			}));

		// throwsand: 28 logic frames, active [13,17) <- throwsand_sp.ani 8
		// frames [80 x7, 240] ms (800 ms total; the plain throwsand.ani does
		// not exist in the library, M6-019 located the _sp variant). Startup:
		// frame 0 [0,5), frame 1 [5,10), frame 2 [10,13); recovery: frame 5
		// [17,22), frame 6 [22,27), frame 7 [27,28).
		Table.Add(MakeMapping(FName(TEXT("throwsand")), FName(TEXT("throwsand_sp")),
			{
				Seg(0, 5, 0),
				Seg(5, 10, 1),
				Seg(10, 13, 2),
				Seg(13, 17, 4, true),
				Seg(17, 22, 5),
				Seg(22, 27, 6),
				Seg(27, 28, 7)
			}));

		// earthbreak: 30 logic frames, active [11,14) <- earthbreak.ani 6
		// frames [80, 250, 210, 40, 100, 1] ms (681 ms total). Startup: frame
		// 0 [0,5), frame 1 (250 ms) [5,11) (frame 2's 210 ms is skipped by the
		// compression); recovery: frame 4 (100 ms) [14,20), frame 5 (the 1 ms
		// stored end frame) holds [20,30) - the combat clock ends the attack,
		// so the 1 ms frame can never stall the display.
		Table.Add(MakeMapping(FName(TEXT("earthbreak")), FName(TEXT("earthbreak")),
			{
				Seg(0, 5, 0),
				Seg(5, 11, 1),
				Seg(11, 14, 3, true),
				Seg(14, 20, 4),
				Seg(20, 30, 5)
			}));

		// stomp: 22 logic frames, active [11,14) <- stomp.ani 4 frames
		// [50, 50, 50, 550] ms (700 ms total). Startup: frame 0 [0,3), frame 1
		// [3,11); recovery: frame 3 (550 ms) covers [14,22). (The follow-up
		// stompfinish.ani has no combat attack stage - Data/combat-attacks.json
		// carries no second stomp attack id - so it is not wired; see the
		// M6-024 report.)
		Table.Add(MakeMapping(FName(TEXT("stomp")), FName(TEXT("stomp")),
			{
				Seg(0, 3, 0),
				Seg(3, 11, 1),
				Seg(11, 14, 2, true),
				Seg(14, 22, 3)
			}));

		return Table;
	}

	const FDNFMAttackDisplayMapping* FindAttackDisplayMapping(
		const TArray<FDNFMAttackDisplayMapping>& Table, FName AttackId)
	{
		if (AttackId.IsNone())
		{
			return nullptr;
		}
		for (const FDNFMAttackDisplayMapping& Mapping : Table)
		{
			if (Mapping.AttackId == AttackId)
			{
				return &Mapping;
			}
		}
		return nullptr;
	}

	int32 FindDisplayFrameForLogicFrame(const FDNFMAttackDisplayMapping& Mapping, int32 LogicFrame)
	{
		if (Mapping.Segments.Num() == 0)
		{
			return INDEX_NONE;
		}
		// Clamp both ends: a pre-first-tick Frame (-1) reads as the first row
		// and an overrun as the last row (the attack ends by the combat clock
		// before the rows run out; the clamps are defensive only).
		if (LogicFrame < Mapping.Segments[0].LogicStartFrame)
		{
			return Mapping.Segments[0].DisplayFrameIndex;
		}
		for (const FDNFMAttackDisplaySegment& Row : Mapping.Segments)
		{
			if (LogicFrame >= Row.LogicStartFrame && LogicFrame < Row.LogicEndFrame)
			{
				return Row.DisplayFrameIndex;
			}
		}
		return Mapping.Segments.Last().DisplayFrameIndex;
	}
}

using namespace UE::UEMMO::Tasks::M6_023;

FDNFMCombatVisualPresenter::FDNFMCombatVisualPresenter()
{
	AttackDisplayMappings = MakeDefaultAttackDisplayMappings();
}

FDNFMCombatVisualPresenter::~FDNFMCombatVisualPresenter()
{
	// Unbind from the visual even when the owner forgot Reset (EndPlay paths).
	if (UDNFMCharacterVisualComponent* Visual = VisualTarget.Get())
	{
		Visual->OnVisualFrameEvent.RemoveAll(this);
	}
}

void FDNFMCombatVisualPresenter::SetVisualTarget(UDNFMCharacterVisualComponent* InVisual)
{
	if (VisualTarget.Get() == InVisual)
	{
		return;
	}
	if (UDNFMCharacterVisualComponent* Previous = VisualTarget.Get())
	{
		Previous->OnVisualFrameEvent.RemoveAll(this);
	}
	Reset();
	VisualTarget = InVisual;
	if (UDNFMCharacterVisualComponent* Visual = VisualTarget.Get())
	{
		VisualEventHandle = Visual->OnVisualFrameEvent.AddRaw(this, &FDNFMCombatVisualPresenter::HandleVisualFrameEvent);
	}
}

void FDNFMCombatVisualPresenter::Reset()
{
	if (UDNFMCharacterVisualComponent* Visual = VisualTarget.Get())
	{
		Visual->OnVisualFrameEvent.RemoveAll(this);
	}
	VisualEventHandle.Reset();
	PresentedActionId = NAME_None;
	PresentedInstanceId = 0;
	bHitStopFrozen = false;
	bAttackTimeExternal = false;
	bObservedDead = false;
	JumpPhase = EDNFMJumpPhase::None;
	JumpPhaseElapsedMs = 0.0;
	ResetRecoverSequence();
	bNextHitUsesAltPose = true;
	bObservedAirborne = false;
	PrevObservedState = ECombatActionState::Free;
	InstanceEventKeys.Reset();
}

void FDNFMCombatVisualPresenter::Step(const FVisualFacts& Facts)
{
	UDNFMCharacterVisualComponent* Visual = VisualTarget.Get();
	if (Visual == nullptr)
	{
		return; // the gate=0 no-op face: no target, no DNFM display work at all
	}

	// Revive edge (plan 6.2): the death lifecycle closed - drop the old
	// display instance entirely and let the next sync rebuild from scratch
	// (a revival never resumes an interrupted attack's frame, a half-played
	// getup sequence or a jump phase).
	if (bObservedDead && !Facts.bDead)
	{
		ForgetPresentedInstance();
		Visual->ResetVisual();
		bAttackTimeExternal = false;
		JumpPhase = EDNFMJumpPhase::None;
		JumpPhaseElapsedMs = 0.0;
		ResetRecoverSequence();
		PushTimeOwnership();
	}
	bObservedDead = Facts.bDead;

	StepDisplay(Facts);

	// The trailing edge bookkeeping (every display path): the next Step reads
	// these as "the previous tick" for the takeoff/landing and the reaction
	// edges.
	PrevObservedState = Facts.Combat.ActionState;
	bObservedAirborne = Facts.bAirborne;
}

void FDNFMCombatVisualPresenter::StepDisplay(const FVisualFacts& Facts)
{
	UDNFMCharacterVisualComponent* Visual = VisualTarget.Get();
	if (Visual == nullptr)
	{
		return;
	}
	const ECombatActionState State = Facts.Combat.ActionState;
	const bool bStateEdge = State != PrevObservedState;

	// Priority 1: death (its clip plays once and holds the end frame; no
	// restart while the state persists). A half-played getup sequence or jump
	// phase dies with the body.
	if (Facts.bDead)
	{
		bAttackTimeExternal = false;
		JumpPhase = EDNFMJumpPhase::None;
		JumpPhaseElapsedMs = 0.0;
		ResetRecoverSequence();
		PushTimeOwnership();
		ApplyStateAction(DeathActionId, {DownActionId, IdleActionId}, /*bOnce*/ true);
		return;
	}

	// Priority 2: down/getup/hit (the combat state reactions; each plays once
	// and holds until the state ends - the visual never ends the state early).
	switch (State)
	{
	case ECombatActionState::HitStun:
		// M6-024: each NEW stun episode flips the alternating hit pose
		// (damage1 -> damage2 -> damage1 ...). No per-hit direction fact
		// reaches the display layer (the victim snapshot and health component
		// carry none), so the deterministic alternation is the per-hit
		// variety; within one episode the pose holds (the M6-023 rule).
		if (bStateEdge)
		{
			ResetRecoverSequence();
			bNextHitUsesAltPose = !bNextHitUsesAltPose;
		}
		bAttackTimeExternal = false;
		PushTimeOwnership();
		ApplyStateAction(bNextHitUsesAltPose ? HitAltActionId : HitActionId,
			{bNextHitUsesAltPose ? HitActionId : HitAltActionId, IdleActionId}, true);
		return;
	case ECombatActionState::Knockdown:
		if (bStateEdge)
		{
			ResetRecoverSequence(); // a new knockdown abandons any rolling getup
		}
		bAttackTimeExternal = false;
		PushTimeOwnership();
		ApplyStateAction(DownActionId, {IdleActionId}, true);
		return;
	case ECombatActionState::Recovering:
		bAttackTimeExternal = false;
		if (bStateEdge)
		{
			// A fresh Recovering episode: the overturn pose first (a single
			// displayed frame - the clip's 10 s storage duration is a logic
			// hold placeholder, never a display duration), then the getup
			// roll. Direction: the back roll by default - the display layer
			// receives no per-hit direction fact, and every current knockdown
			// source pushes the victim away from the attacker, which the
			// DNF-style getup roll continues (see the header constants).
			ResetRecoverSequence();
			RecoverPhase = EDNFMRecoverPhase::Overturn;
			RecoverPhaseElapsedMs = 0.0;
			RecoverRollActionId = RecoverRollBackActionId;
		}
		if (bRecoverSequenceDone || RecoverPhase == EDNFMRecoverPhase::None)
		{
			// The sequence played to its end inside this Recovering episode:
			// the display returns to idle while the state lasts (plan 6.2
			// "序列播完回待机").
			PushTimeOwnership();
			ApplyStateAction(IdleActionId, {}, false);
			return;
		}
		PushTimeOwnership();
		ApplyRecoverSequence(Facts);
		return;
	default:
		break;
	}

	// Priority 3: the current attack (sampled from AttackId+InstanceId+Frame).
	if (State == ECombatActionState::Attacking && Facts.Combat.InstanceId != 0)
	{
		ResetRecoverSequence(); // the player's new action cancels a rolling getup
		if (const FDNFMAttackDisplayMapping* Mapping =
			FindAttackDisplayMapping(AttackDisplayMappings, Facts.Combat.AttackId))
		{
			ApplyAttackDisplay(*Mapping, Facts);
			return;
		}
		// No mapping for this attack id (a future/skill id): one diagnostic,
		// then the locomotion display keeps running (honest degradation - the
		// combat behavior itself is untouched).
		WarnMissingActionOnce(Facts.Combat.AttackId, TEXT("no DNFM display mapping"));
	}

	// Priority 3.5 (M6-024): a started getup sequence keeps playing to its end
	// even after Recovering ended (plan 6.2 "序列播完回待机"). It sits below
	// the reactions (a new hit preempts above) and below the attack (the
	// player acting wins) - and ResetRecoverSequence in those branches is what
	// abandons it.
	if (RecoverPhase != EDNFMRecoverPhase::None)
	{
		PushTimeOwnership();
		ApplyRecoverSequence(Facts);
		return;
	}

	// Priority 4/5: jump phase, then ground move/idle.
	bAttackTimeExternal = false;
	const bool bTakeoffEdge = Facts.bAirborne && !bObservedAirborne;
	const bool bLandingEdge = !Facts.bAirborne && bObservedAirborne;
	if (Facts.bAirborne)
	{
		ApplyJumpAirDisplay(Facts, bTakeoffEdge);
		// The push runs AFTER the phase entry so the pin covers the walked
		// display from its first tick (and clears when the clip was missing
		// and the phase fell back to the locomotion display).
		PushTimeOwnership();
		return;
	}
	if (JumpPhase != EDNFMJumpPhase::None)
	{
		if (JumpPhase == EDNFMJumpPhase::Landing)
		{
			if (bStateEdge)
			{
				// The tail was interrupted by a reaction/attack and the state
				// just changed back: the landing moment is long gone, a late
				// tail would read as a second recovery - abandon it.
				JumpPhase = EDNFMJumpPhase::None;
				JumpPhaseElapsedMs = 0.0;
			}
			else if (ApplyJumpLandingDisplay(Facts))
			{
				return; // the tail still owns the picture
			}
			// else: the tail finished this tick; fall through to locomotion.
		}
		else if (bLandingEdge)
		{
			// The ground edge seen by THIS branch: the landing moment (plan
			// 6.2 落地=回地瞬间播尾部帧). The tail pins the clip and walks
			// frame 12 (hold cap) -> 13 -> 14 by the tick delta.
			JumpPhase = EDNFMJumpPhase::Landing;
			JumpPhaseElapsedMs = 0.0;
			PushTimeOwnership();
			if (PresentedActionId != JumpActionId)
			{
				// The airborne display was stolen (an aerial attack ended
				// exactly on the landing tick): rebuild the jump display.
				ForgetPresentedInstance();
				PresentedActionId = JumpActionId;
				if (!Visual->SetAction(JumpActionId, EDNFMVisualLoopPolicy::ForceOnce))
				{
					WarnMissingActionOnce(JumpActionId, TEXT("jump clip missing from the visual set"));
					PresentedActionId = NAME_None;
					JumpPhase = EDNFMJumpPhase::None;
					ApplyStateAction(IdleActionId, {}, false);
					return;
				}
			}
			ApplyJumpLandingDisplay(Facts);
			return;
		}
		else
		{
			// A stale airborne phase (the ground edge happened under a higher
			// display): no tail, straight back to the locomotion display.
			JumpPhase = EDNFMJumpPhase::None;
			JumpPhaseElapsedMs = 0.0;
		}
	}
	PushTimeOwnership();
	if (Facts.bGroundMoving)
	{
		ApplyStateAction(MoveActionId, {IdleActionId}, false);
	}
	else
	{
		ApplyStateAction(IdleActionId, {}, false);
	}
}

void FDNFMCombatVisualPresenter::NotifyHitStopFrozen(bool bFrozen)
{
	bHitStopFrozen = bFrozen;
	PushTimeOwnership();
}

void FDNFMCombatVisualPresenter::ApplyStateAction(FName PrimaryAction, const TArray<FName>& Fallbacks, bool bOnce)
{
	UDNFMCharacterVisualComponent* Visual = VisualTarget.Get();
	if (Visual == nullptr)
	{
		return;
	}
	if (PresentedActionId == PrimaryAction)
	{
		return; // steady state: a once-clip keeps playing/holding, a loop keeps looping
	}
	// Presenting something else (an attack just ended, a reaction interrupted):
	// the instance space of the old display dies here.
	ForgetPresentedInstance();

	// Walk the candidates in order; the first the set actually carries wins.
	// The acceptance filter is raised BEFORE each SetAction: the frame-0
	// events broadcast synchronously inside it and must be attributed to the
	// candidate being presented. A missing reaction/loop action falls to the
	// next candidate with one diagnostic (content gaps stay visible, the
	// display never freezes).
	FName Chosen = NAME_None;
	TArray<FName> Candidates;
	Candidates.Add(PrimaryAction);
	Candidates.Append(Fallbacks);
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		const FName& Candidate = Candidates[Index];
		PresentedActionId = Candidate;
		if (Visual->SetAction(Candidate, Index == 0 && bOnce
			? EDNFMVisualLoopPolicy::ForceOnce : EDNFMVisualLoopPolicy::UseClipDefault))
		{
			Chosen = Candidate;
			break;
		}
		WarnMissingActionOnce(Candidate, TEXT("action missing from the visual set"));
	}
	PresentedActionId = Chosen; // None when every candidate failed (events then drop)
	PresentedInstanceId = 0;
}

void FDNFMCombatVisualPresenter::ApplyAttackDisplay(const FDNFMAttackDisplayMapping& Mapping, const FVisualFacts& Facts)
{
	UDNFMCharacterVisualComponent* Visual = VisualTarget.Get();
	if (Visual == nullptr)
	{
		return;
	}

	// New instance (or the mapping clip lost its slot to a fallback): start
	// the source clip from frame 0 and take over its time (the display frame
	// comes from the logic frame only). The presented identity is raised
	// BEFORE SetAction so its synchronous frame-0 events attribute to this
	// instance; the old instance's key space dies here, so a fresh instance
	// re-dispatches its own events.
	if (PresentedInstanceId != Facts.Combat.InstanceId || PresentedActionId != Mapping.ActionId)
	{
		ForgetPresentedInstance();
		PresentedActionId = Mapping.ActionId;
		PresentedInstanceId = Facts.Combat.InstanceId;
		bAttackTimeExternal = true;
		PushTimeOwnership();
		if (!Visual->SetAction(Mapping.ActionId, EDNFMVisualLoopPolicy::ForceOnce))
		{
			WarnMissingActionOnce(Mapping.ActionId, TEXT("attack clip missing from the visual set"));
			PresentedActionId = NAME_None;
			PresentedInstanceId = 0;
			bAttackTimeExternal = false;
			PushTimeOwnership();
			ApplyStateAction(IdleActionId, {}, false);
			return;
		}
	}

	// Deterministic sampling: the display frame is a pure function of the
	// logic frame (plan 6.2) - HitStop freezes the logic frame, so the display
	// freezes with it and no frozen-span events can exist.
	const int32 TargetFrame = FindDisplayFrameForLogicFrame(Mapping, Facts.Combat.Frame);
	if (TargetFrame != INDEX_NONE && Visual->GetCurrentFrameIndex() != TargetFrame)
	{
		// The seek walks every newly crossed display frame in time order (the
		// honest low-framerate interval), so the dedup sees each frame's
		// events exactly once per instance.
		Visual->SeekToDisplayFrame(TargetFrame);
	}
}

void FDNFMCombatVisualPresenter::PushTimeOwnership()
{
	UDNFMCharacterVisualComponent* Visual = VisualTarget.Get();
	if (Visual == nullptr)
	{
		return;
	}
	// A presented attack's time is external for its whole life (the logic
	// frame owns the pose); HitStop freezes the real-time clips (idle/move/
	// reactions); a jump phase or getup sequence pins the clip time entirely
	// (the presenter walks it with seeks - the stored >=10 s hold frames can
	// never self-advance the picture). All three compose.
	Visual->SetPlaybackFrozen(bHitStopFrozen || bAttackTimeExternal || HasPresenterPinnedDisplay());
}

// M6-024: the jump.ani phase walk, frozen from the M6-020 manifest
// (composite/manifest.json, jump.ani 15 frames):
//   frames 0..5  takeoff  [100, 80, 80, 80, 80, 80] ms (500 ms total)
//   frame  6     air hold (34464 ms storage hold = the apex pose)
//   frames 7..11 fall     80 ms each (400 ms)
//   frame 12     landing (10000 ms storage hold = the landing pose)
//   frames 13/14 landing recovery tail, 50 ms each
// The two storage holds NEVER gate the display (plan 6.2: 根据运动状态推进，
// 不能按10秒以上的存储等待值僵等) - the movement facts switch the phase and
// the tick delta walks the frames; the presenter caps the landing pose at
// JumpLandingPoseHoldMs so the 10 s storage value reads as a short squash.
namespace
{
	// The takeoff frames' durations, then the apex hold (frames 0..6).
	constexpr int32 JumpTakeoffDurationsMs[] = {100, 80, 80, 80, 80, 80};
	constexpr int32 JumpTakeoffLastFrame = 5;   // frame 6 is the apex hold
	constexpr double JumpTakeoffTotalMs = 500.0;
	constexpr int32 JumpApexFrame = 6;

	// The fall frames' durations (frames 7..11, held at 11 while airborne).
	constexpr int32 JumpFallDurationsMs[] = {80, 80, 80, 80, 80};
	constexpr int32 JumpFallFirstFrame = 7;
	constexpr int32 JumpFallLastFrame = 11;

	// The landing tail (frames 12..14); the pose hold is the presenter's cap,
	// the two recovery frames keep their stored 50 ms.
	constexpr int32 JumpLandingDurationsMs[] = {120, 50, 50};
	constexpr int32 JumpLandingFirstFrame = 12;
	constexpr double JumpLandingTailTotalMs = 220.0;

	// The overturn pose hold (the clip stores 10000 ms; the display shows the
	// single getup frame for this beat before the roll starts).
	constexpr double RecoverOverturnHoldMs = 120.0;

	// The getup roll clips (both directions, 8 frames 630 ms total).
	constexpr int32 RecoverRollDurationsMs[] = {60, 50, 50, 50, 50, 60, 60, 250};
	constexpr double RecoverRollTotalMs = 630.0;
}

int32 FDNFMCombatVisualPresenter::WalkFramesByElapsedMs(const int32* DurationsMs, int32 Count, double ElapsedMs)
{
	double Acc = 0.0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Acc += FMath::Max(DurationsMs[Index], 0);
		if (ElapsedMs < Acc)
		{
			return Index;
		}
	}
	return Count - 1; // past the total: hold the last walked frame
}

void FDNFMCombatVisualPresenter::ApplyJumpAirDisplay(const FVisualFacts& Facts, bool bTakeoffEdge)
{
	UDNFMCharacterVisualComponent* Visual = VisualTarget.Get();
	if (Visual == nullptr)
	{
		return;
	}

	// (Re)enter the takeoff: a fresh display instance with a fresh event space
	// (each jump replays its own frame events). The mid-air entry (bTakeoffEdge
	// false, phase None) covers an airborne display that just lost a higher
	// priority - e.g. an aerial attack ended while still rising.
	if (bTakeoffEdge || JumpPhase == EDNFMJumpPhase::None)
	{
		ForgetPresentedInstance();
		PresentedActionId = JumpActionId;
		JumpPhase = EDNFMJumpPhase::Takeoff;
		JumpPhaseElapsedMs = 0.0;
		if (!Visual->SetAction(JumpActionId, EDNFMVisualLoopPolicy::ForceOnce))
		{
			WarnMissingActionOnce(JumpActionId, TEXT("jump clip missing from the visual set"));
			PresentedActionId = NAME_None;
			JumpPhase = EDNFMJumpPhase::None;
			ApplyStateAction(IdleActionId, {}, false);
			return;
		}
	}
	else if (PresentedActionId != JumpActionId)
	{
		// The display was stolen (attack/reaction) while airborne: rebuild the
		// phase display from its current elapsed with a fresh event space.
		ForgetPresentedInstance();
		PresentedActionId = JumpActionId;
		if (!Visual->SetAction(JumpActionId, EDNFMVisualLoopPolicy::ForceOnce))
		{
			WarnMissingActionOnce(JumpActionId, TEXT("jump clip missing from the visual set"));
			PresentedActionId = NAME_None;
			JumpPhase = EDNFMJumpPhase::None;
			ApplyStateAction(IdleActionId, {}, false);
			return;
		}
	}

	// The movement facts own the phase switch: rising walks the takeoff
	// frames, the first non-rising airborne tick starts the fall walk.
	if (JumpPhase == EDNFMJumpPhase::Takeoff && !Facts.bAirborneRising)
	{
		JumpPhase = EDNFMJumpPhase::Falling;
		JumpPhaseElapsedMs = 0.0;
	}
	JumpPhaseElapsedMs += FMath::Max(Facts.DeltaSeconds, 0.0f) * 1000.0;

	int32 Target;
	if (JumpPhase == EDNFMJumpPhase::Takeoff)
	{
		Target = JumpPhaseElapsedMs >= JumpTakeoffTotalMs
			? JumpApexFrame // the stored 34464 ms apex hold, entered by state
			: WalkFramesByElapsedMs(JumpTakeoffDurationsMs, JumpTakeoffLastFrame + 1, JumpPhaseElapsedMs);
	}
	else
	{
		Target = JumpFallFirstFrame
			+ WalkFramesByElapsedMs(JumpFallDurationsMs, JumpFallLastFrame - JumpFallFirstFrame + 1, JumpPhaseElapsedMs);
	}
	if (Visual->GetCurrentFrameIndex() != Target)
	{
		Visual->SeekToDisplayFrame(Target);
	}
}

bool FDNFMCombatVisualPresenter::ApplyJumpLandingDisplay(const FVisualFacts& Facts)
{
	UDNFMCharacterVisualComponent* Visual = VisualTarget.Get();
	if (Visual == nullptr)
	{
		return false;
	}
	JumpPhaseElapsedMs += FMath::Max(Facts.DeltaSeconds, 0.0f) * 1000.0;
	if (JumpPhaseElapsedMs >= JumpLandingTailTotalMs)
	{
		// The tail is over: back to the locomotion display (unpinned).
		JumpPhase = EDNFMJumpPhase::None;
		JumpPhaseElapsedMs = 0.0;
		return false;
	}
	if (PresentedActionId != JumpActionId)
	{
		ForgetPresentedInstance();
		PresentedActionId = JumpActionId;
		if (!Visual->SetAction(JumpActionId, EDNFMVisualLoopPolicy::ForceOnce))
		{
			WarnMissingActionOnce(JumpActionId, TEXT("jump clip missing from the visual set"));
			PresentedActionId = NAME_None;
			JumpPhase = EDNFMJumpPhase::None;
			return false;
		}
	}
	const int32 Target = JumpLandingFirstFrame
		+ WalkFramesByElapsedMs(JumpLandingDurationsMs, 3, JumpPhaseElapsedMs);
	if (Visual->GetCurrentFrameIndex() != Target)
	{
		Visual->SeekToDisplayFrame(Target);
	}
	return true;
}

void FDNFMCombatVisualPresenter::ApplyRecoverSequence(const FVisualFacts& Facts)
{
	UDNFMCharacterVisualComponent* Visual = VisualTarget.Get();
	if (Visual == nullptr)
	{
		return;
	}
	RecoverPhaseElapsedMs += FMath::Max(Facts.DeltaSeconds, 0.0f) * 1000.0;

	if (RecoverPhase == EDNFMRecoverPhase::Overturn)
	{
		if (PresentedActionId != RecoverActionId)
		{
			ForgetPresentedInstance();
			PresentedActionId = RecoverActionId;
			if (!Visual->SetAction(RecoverActionId, EDNFMVisualLoopPolicy::ForceOnce))
			{
				// No overturn clip: skip straight to the roll (the sequence
				// stays honest about what the set actually carries).
				WarnMissingActionOnce(RecoverActionId, TEXT("overturn clip missing from the visual set"));
				PresentedActionId = NAME_None;
				RecoverPhase = EDNFMRecoverPhase::Roll;
				RecoverPhaseElapsedMs = 0.0;
			}
		}
		if (RecoverPhase == EDNFMRecoverPhase::Overturn)
		{
			if (RecoverPhaseElapsedMs < RecoverOverturnHoldMs)
			{
				return; // the getup flick holds its beat
			}
			RecoverPhase = EDNFMRecoverPhase::Roll;
			RecoverPhaseElapsedMs = 0.0;
			ForgetPresentedInstance(); // the roll gets its own event space
		}
	}

	if (RecoverPhase == EDNFMRecoverPhase::Roll)
	{
		if (PresentedActionId != RecoverRollActionId)
		{
			PresentedActionId = RecoverRollActionId;
			if (!Visual->SetAction(RecoverRollActionId, EDNFMVisualLoopPolicy::ForceOnce))
			{
				// Neither roll direction exists: the sequence ends honestly
				// and the display returns to idle (one diagnostic).
				WarnMissingActionOnce(RecoverRollActionId, TEXT("getup roll clip missing from the visual set"));
				PresentedActionId = NAME_None;
				RecoverPhase = EDNFMRecoverPhase::None;
				bRecoverSequenceDone = true;
				return;
			}
		}
		const int32 Target = WalkFramesByElapsedMs(RecoverRollDurationsMs, 8, RecoverPhaseElapsedMs);
		if (Visual->GetCurrentFrameIndex() != Target)
		{
			Visual->SeekToDisplayFrame(Target);
		}
		if (RecoverPhaseElapsedMs >= RecoverRollTotalMs)
		{
			// The sequence is complete: the roll's end frame stays visible and
			// the next sync returns the locomotion display (plan 6.2).
			RecoverPhase = EDNFMRecoverPhase::None;
			bRecoverSequenceDone = true;
		}
	}
}

void FDNFMCombatVisualPresenter::ResetRecoverSequence()
{
	RecoverPhase = EDNFMRecoverPhase::None;
	RecoverPhaseElapsedMs = 0.0;
	bRecoverSequenceDone = false;
}

void FDNFMCombatVisualPresenter::ForgetPresentedInstance()
{
	PresentedActionId = NAME_None;
	PresentedInstanceId = 0;
	InstanceEventKeys.Reset(); // the cancelled instance never dispatches again
}

bool FDNFMCombatVisualPresenter::WarnMissingActionOnce(FName ActionId, const TCHAR* Reason)
{
	bool bAlreadyLogged = false;
	WarnedMissingActionIds.Add(ActionId, &bAlreadyLogged);
	if (!bAlreadyLogged)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO M6-023: DNFM display has no '%s' (%s); the display falls back and combat stays untouched. Calibrate after the M6-022 import."),
			*ActionId.ToString(), Reason);
	}
	return !bAlreadyLogged;
}

void FDNFMCombatVisualPresenter::HandleVisualFrameEvent(uint64 ActorId, FName ActionName, int32 FrameIndex,
	FName EventTag, FName EventName)
{
	// Only the action the presenter actually presented may dispatch (a stale
	// playback of a cancelled/switched-away clip drops here - the plan's
	// "暂停、中断、死亡、切关会取消旧实例").
	if (ActionName != PresentedActionId)
	{
		return;
	}
	FDNFMVisualEventKey Key;
	Key.ActorId = ActorId;
	Key.AttackInstanceId = PresentedInstanceId;
	Key.ActionName = ActionName;
	Key.FrameIndex = FrameIndex;
	Key.EventTag = EventTag;
	Key.EventName = EventName;
	bool bAlreadySeen = false;
	InstanceEventKeys.Add(Key, &bAlreadySeen);
	if (bAlreadySeen)
	{
		return; // the (ActorId, AttackInstanceId, EventId) dedup
	}

	FDNFMRecordedVisualEvent Record;
	Record.ActorId = ActorId;
	Record.AttackInstanceId = Key.AttackInstanceId;
	Record.ActionName = ActionName;
	Record.FrameIndex = FrameIndex;
	Record.EventTag = EventTag;
	Record.EventName = EventName;
	DedupedEventHistory.Add(Record);
	if (DedupedEventHistory.Num() > MaxDedupedEventHistory)
	{
		// Diagnostics history only; drop the oldest entry to stay bounded.
		DedupedEventHistory.RemoveAt(0);
	}
	// Audio never routes from here (the existing dispatcher owns it - plan
	// 6.2: no double playback); this feed is for the later effect consumers.
	OnDedupedVisualFrameEvent.Broadcast(ActorId, Key.AttackInstanceId, ActionName, FrameIndex, EventTag, EventName);
}
