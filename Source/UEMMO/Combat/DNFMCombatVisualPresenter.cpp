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
		 * counts/totals in the comments are the M6-019 db_queries facts
		 * (Artifacts/Tasks/M6-019 .../db_queries/state_skill_animations.json
		 * and the ue_ready ue_manifest.json files); the segment splits inside
		 * the startup/recovery regions are PLACEHOLDERS to calibrate against
		 * the M6-020 manifest after the M6-022 import ("以 manifest 导入后校准").
		 * Every bHitPose row covers its attack's active window exactly (the
		 * hard plan constraint the automation suite re-validates).
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
		Table.Add(MakeMapping(FName(TEXT("light_01")), FName(TEXT("attack1")),
			{
				Seg(0, 7, 0),          // 起手段: windup pose
				Seg(7, 11, 1, true),   // 命中段: hit pose == the active window
				Seg(11, 26, 2)         // 收招段: recovery pose
			}));

		// light_02: 32 logic frames, active [9,14) <- attack2.ani 4 frames
		// [80, 80, 80, 240] ms (480 ms total). (Placeholder splits; calibrate.)
		Table.Add(MakeMapping(FName(TEXT("light_02")), FName(TEXT("attack2")),
			{
				Seg(0, 9, 0),
				Seg(9, 14, 1, true),
				Seg(14, 19, 2),
				Seg(19, 32, 3)
			}));

		// launcher: 40 logic frames, active [12,17) <- liftupper.ani 6 frames
		// 740 ms. (Placeholder splits; calibrate.)
		Table.Add(MakeMapping(FName(TEXT("launcher")), FName(TEXT("liftupper")),
			{
				Seg(0, 5, 0),
				Seg(5, 9, 1),
				Seg(9, 12, 2),
				Seg(12, 17, 3, true),
				Seg(17, 29, 4),
				Seg(29, 40, 5)
			}));

		// aerial_01: 28 logic frames, active [6,10) <- jumpattack.ani 4 frames
		// [20, 120, 50, 550] ms (740 ms total). (Placeholder splits; calibrate.)
		Table.Add(MakeMapping(FName(TEXT("aerial_01")), FName(TEXT("jumpattack")),
			{
				Seg(0, 6, 0),
				Seg(6, 10, 1, true),
				Seg(10, 16, 2),
				Seg(16, 28, 3)
			}));

		// singlekick: 24 logic frames, active [11,15) <- singlekick.ani 7 frames
		// 530 ms. (Placeholder splits; calibrate.)
		Table.Add(MakeMapping(FName(TEXT("singlekick")), FName(TEXT("singlekick")),
			{
				Seg(0, 4, 0),
				Seg(4, 8, 1),
				Seg(8, 11, 2),
				Seg(11, 15, 3, true),
				Seg(15, 19, 4),
				Seg(19, 22, 5),
				Seg(22, 24, 6)
			}));

		// damagelowkick: 24 logic frames, active [11,15) <- damagelowkick.ani
		// 5 frames 400 ms. (Placeholder splits; calibrate.)
		Table.Add(MakeMapping(FName(TEXT("damagelowkick")), FName(TEXT("damagelowkick")),
			{
				Seg(0, 6, 0),
				Seg(6, 11, 1),
				Seg(11, 15, 2, true),
				Seg(15, 19, 3),
				Seg(19, 24, 4)
			}));

		// throwsand: 28 logic frames, active [13,17) <- throwsand_sp.ani 8
		// frames 800 ms (the plain throwsand.ani does not exist in the library;
		// M6-019 located the _sp variant). (Placeholder splits; calibrate.)
		Table.Add(MakeMapping(FName(TEXT("throwsand")), FName(TEXT("throwsand_sp")),
			{
				Seg(0, 4, 0),
				Seg(4, 7, 1),
				Seg(7, 10, 2),
				Seg(10, 13, 3),
				Seg(13, 17, 4, true),
				Seg(17, 21, 5),
				Seg(21, 25, 6),
				Seg(25, 28, 7)
			}));

		// earthbreak: 30 logic frames, active [11,14) <- earthbreak.ani 6
		// frames 680 ms. (Placeholder splits; calibrate.)
		Table.Add(MakeMapping(FName(TEXT("earthbreak")), FName(TEXT("earthbreak")),
			{
				Seg(0, 4, 0),
				Seg(4, 8, 1),
				Seg(8, 11, 2),
				Seg(11, 14, 3, true),
				Seg(14, 22, 4),
				Seg(22, 30, 5)
			}));

		// stomp: 22 logic frames, active [11,14) <- stomp.ani 4 frames 700 ms
		// (the follow-up stompfinish.ani belongs to a later content pass).
		// (Placeholder splits; calibrate.)
		Table.Add(MakeMapping(FName(TEXT("stomp")), FName(TEXT("stomp")),
			{
				Seg(0, 6, 0),
				Seg(6, 11, 1),
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
	// (a revival never resumes an interrupted attack's frame).
	if (bObservedDead && !Facts.bDead)
	{
		ForgetPresentedInstance();
		Visual->ResetVisual();
		bAttackTimeExternal = false;
		PushTimeOwnership();
	}
	bObservedDead = Facts.bDead;

	// Priority 1: death (its clip plays once and holds the end frame; no
	// restart while the state persists).
	if (Facts.bDead)
	{
		bAttackTimeExternal = false;
		PushTimeOwnership();
		ApplyStateAction(DeathActionId, {DownActionId, IdleActionId}, /*bOnce*/ true);
		return;
	}

	// Priority 2: down/getup/hit (the combat state reactions; each plays once
	// and holds until the state ends - the visual never ends the state early).
	switch (Facts.Combat.ActionState)
	{
	case ECombatActionState::HitStun:
		bAttackTimeExternal = false;
		PushTimeOwnership();
		ApplyStateAction(HitActionId, {HitAltActionId, IdleActionId}, true);
		return;
	case ECombatActionState::Knockdown:
		bAttackTimeExternal = false;
		PushTimeOwnership();
		ApplyStateAction(DownActionId, {IdleActionId}, true);
		return;
	case ECombatActionState::Recovering:
		bAttackTimeExternal = false;
		PushTimeOwnership();
		ApplyStateAction(RecoverActionId, {DownActionId, IdleActionId}, true);
		return;
	default:
		break;
	}

	// Priority 3: the current attack (sampled from AttackId+InstanceId+Frame).
	if (Facts.Combat.ActionState == ECombatActionState::Attacking && Facts.Combat.InstanceId != 0)
	{
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

	// Priority 4/5: jump phase, then ground move/idle (real-time clips).
	bAttackTimeExternal = false;
	PushTimeOwnership();
	if (Facts.bAirborne)
	{
		// Placeholder jump display: the whole clip plays once and holds until
		// landing (the takeoff/air/landing phase mapping calibrates against
		// the M6-020 manifest; the real capsule Z stays the movement's own).
		ApplyStateAction(JumpActionId, {IdleActionId}, true);
	}
	else if (Facts.bGroundMoving)
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
	// reactions). Both compose: an attack that ends during a freeze keeps the
	// freeze on the reaction/idle clip that replaces it.
	Visual->SetPlaybackFrozen(bHitStopFrozen || bAttackTimeExternal);
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
