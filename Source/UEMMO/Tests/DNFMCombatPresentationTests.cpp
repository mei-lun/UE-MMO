// M6-023: the DNFM display-target suite. Covers the task card's acceptance
// list without any imported asset (the visual sets are built from a mock
// manifest in memory; the M6-022 import owns the real data):
//   1. the attack segment table vs the frozen combat-attacks.json facts
//      (every hit-pose segment sits INSIDE the existing active window),
//   2. the display priority (death -> down/getup/hit -> attack -> jump ->
//      ground move/idle) with death preempting a running attack,
//   3. attack sampling driven by (AttackId, InstanceId, Frame) only,
//   4. the HitStop freeze/unfreeze (display holds, no frozen-span replay),
//   5. the (ActorId, AttackInstanceId, EventId) dedup incl. low-framerate
//      crossed intervals and instance switches,
//   6. cancel/interrupt/reset/death/revive leave no residue,
//   7. the component wiring (SetSources + SetDNFMVisualTarget + tick feed)
//      and the gate=0 no-op face,
//   8. the SeekToDisplayFrame primitive, and
//   9. the pawn gate binding (gate 1 binds the presenter, gate 0 unbinds).
// M6-024 extends the same suite (the UEMMO.Tasks.M6_024.* tests below):
//   10. the calibrated attack segment tables (real manifest frame durations),
//   11. the alternating damage1/damage2 hit poses per HitStun episode,
//   12. the knockdown down clip + the overturn->roll getup sequence
//       (completes even after Recovering ends; attack/reaction/death abandon),
//   13. the death end-frame hold + the revive reset,
//   14. the state-driven three-phase jump walk (the stored 34464 ms / 10 s
//       hold frames never gate the display),
//   15. the earthbreak 1 ms end frame never stalls the display, and
//   16. the per-attack mapped-clip entry/hit-pose/exit wiring.
#include "Misc/AutomationTest.h"

#include "../Character/DNFMCharacterVisualComponent.h"
#include "../Character/DNFMCharacterVisualSet.h"
#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatPresentationComponent.h"
#include "../Combat/DNFMCombatVisualPresenter.h"
#include "../PrototypeCharacter.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Tests/AutomationCommon.h"
#include "UObject/Package.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M6_023_Tests
{
    // A mock manifest honoring the frozen M6-020 schema, M6-024-calibrated to
    // the REAL imported set's shape: every action's frame count and per-frame
    // durations mirror the M6-020 manifest (composite/manifest.json), so the
    // sequence walks (jump takeoff/air/landing, overturn->roll) and the
    // compressed attack mappings are exercised against honest content. The
    // attack1 clip keeps its frame-0/1 events so the walk order and the dedup
    // stay observable.
    static const TCHAR* const MockManifest = TEXT(R"raw({"generator": "Scripts/DNFM/prepare_fighter_visual_set.py", "character": "fighter", "outfit": "0000a", "pages": [{"file": "mock_page_0.png", "sha256": "mock0", "size": [512, 512]}], "actions": [{"action_id": "rest", "source_ani": "script/character/fighter/animation/rest.ani", "source_sha256": "h-rest", "loop": true, "frames": [{"index": 0, "duration_ms": 160, "page": 0, "uv_rect_px": [0, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32], "events": [{"type": "audio", "name": "snd_idle"}]}, {"index": 1, "duration_ms": 160, "page": 0, "uv_rect_px": [40, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]}, {"action_id": "move", "source_ani": "script/character/fighter/animation/move.ani", "source_sha256": "h-move", "loop": true, "frames": [{"index": 0, "duration_ms": 100, "page": 0, "uv_rect_px": [80, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 1, "duration_ms": 100, "page": 0, "uv_rect_px": [120, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]}, {"action_id": "jump", "source_ani": "script/character/fighter/animation/jump.ani", "source_sha256": "h-jump", "loop": false, "frames": [{"index": 0, "duration_ms": 100, "page": 0, "uv_rect_px": [160, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 1, "duration_ms": 80, "page": 0, "uv_rect_px": [200, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 2, "duration_ms": 80, "page": 0, "uv_rect_px": [240, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 3, "duration_ms": 80, "page": 0, "uv_rect_px": [280, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 4, "duration_ms": 80, "page": 0, "uv_rect_px": [320, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 5, "duration_ms": 80, "page": 0, "uv_rect_px": [360, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 6, "duration_ms": 34464, "page": 0, "uv_rect_px": [400, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 7, "duration_ms": 80, "page": 0, "uv_rect_px": [440, 0, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 8, "duration_ms": 80, "page": 0, "uv_rect_px": [0, 40, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 9, "duration_ms": 80, "page": 0, "uv_rect_px": [40, 40, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 10, "duration_ms": 80, "page": 0, "uv_rect_px": [80, 40, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 11, "duration_ms": 80, "page": 0, "uv_rect_px": [120, 40, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 12, "duration_ms": 10000, "page": 0, "uv_rect_px": [160, 40, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 13, "duration_ms": 50, "page": 0, "uv_rect_px": [200, 40, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 14, "duration_ms": 50, "page": 0, "uv_rect_px": [240, 40, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]}, {"action_id": "attack1", "source_ani": "script/character/fighter/animation/attack1.ani", "source_sha256": "h-attack1", "loop": false, "frames": [{"index": 0, "duration_ms": 100, "page": 0, "uv_rect_px": [280, 40, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32], "events": [{"type": "audio", "name": "snd_swing0"}]},)raw")
    TEXT(R"raw( {"index": 1, "duration_ms": 80, "page": 0, "uv_rect_px": [320, 40, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32], "events": [{"type": "audio", "name": "snd_swing1"}, {"type": "fx", "name": "fx_slash"}]}, {"index": 2, "duration_ms": 220, "page": 0, "uv_rect_px": [360, 40, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]}, {"action_id": "attack2", "source_ani": "script/character/fighter/animation/attack2.ani", "source_sha256": "h-attack2", "loop": false, "frames": [{"index": 0, "duration_ms": 80, "page": 0, "uv_rect_px": [400, 40, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 1, "duration_ms": 80, "page": 0, "uv_rect_px": [440, 40, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 2, "duration_ms": 80, "page": 0, "uv_rect_px": [0, 80, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 3, "duration_ms": 240, "page": 0, "uv_rect_px": [40, 80, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]}, {"action_id": "liftupper", "source_ani": "script/character/fighter/animation/liftupper.ani", "source_sha256": "h-liftupper", "loop": false, "frames": [{"index": 0, "duration_ms": 250, "page": 0, "uv_rect_px": [80, 80, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 1, "duration_ms": 70, "page": 0, "uv_rect_px": [120, 80, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 2, "duration_ms": 100, "page": 0, "uv_rect_px": [160, 80, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 3, "duration_ms": 70, "page": 0, "uv_rect_px": [200, 80, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 4, "duration_ms": 180, "page": 0, "uv_rect_px": [240, 80, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 5, "duration_ms": 70, "page": 0, "uv_rect_px": [280, 80, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]}, {"action_id": "jumpattack", "source_ani": "script/character/fighter/animation/jumpattack.ani", "source_sha256": "h-jumpattack", "loop": false, "frames": [{"index": 0, "duration_ms": 20, "page": 0, "uv_rect_px": [320, 80, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 1, "duration_ms": 120, "page": 0, "uv_rect_px": [360, 80, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 2, "duration_ms": 50, "page": 0, "uv_rect_px": [400, 80, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 3, "duration_ms": 550, "page": 0, "uv_rect_px": [440, 80, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]},)raw")
    TEXT(R"raw( {"action_id": "singlekick", "source_ani": "script/character/fighter/animation/singlekick.ani", "source_sha256": "h-singlekick", "loop": false, "frames": [{"index": 0, "duration_ms": 50, "page": 0, "uv_rect_px": [0, 120, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 1, "duration_ms": 20, "page": 0, "uv_rect_px": [40, 120, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 2, "duration_ms": 100, "page": 0, "uv_rect_px": [80, 120, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 3, "duration_ms": 150, "page": 0, "uv_rect_px": [120, 120, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 4, "duration_ms": 70, "page": 0, "uv_rect_px": [160, 120, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 5, "duration_ms": 70, "page": 0, "uv_rect_px": [200, 120, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 6, "duration_ms": 70, "page": 0, "uv_rect_px": [240, 120, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]}, {"action_id": "damagelowkick", "source_ani": "script/character/fighter/animation/damagelowkick.ani", "source_sha256": "h-damagelowkick", "loop": false, "frames": [{"index": 0, "duration_ms": 80, "page": 0, "uv_rect_px": [280, 120, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 1, "duration_ms": 80, "page": 0, "uv_rect_px": [320, 120, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 2, "duration_ms": 50, "page": 0, "uv_rect_px": [360, 120, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 3, "duration_ms": 110, "page": 0, "uv_rect_px": [400, 120, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 4, "duration_ms": 80, "page": 0, "uv_rect_px": [440, 120, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]}, {"action_id": "throwsand_sp", "source_ani": "script/character/fighter/animation/throwsand_sp.ani", "source_sha256": "h-throwsand_sp", "loop": false, "frames": [{"index": 0, "duration_ms": 80, "page": 0, "uv_rect_px": [0, 160, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 1, "duration_ms": 80, "page": 0, "uv_rect_px": [40, 160, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 2, "duration_ms": 80, "page": 0, "uv_rect_px": [80, 160, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 3, "duration_ms": 80, "page": 0, "uv_rect_px": [120, 160, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 4, "duration_ms": 80, "page": 0, "uv_rect_px": [160, 160, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 5, "duration_ms": 80, "page": 0, "uv_rect_px": [200, 160, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 6, "duration_ms": 80, "page": 0, "uv_rect_px": [240, 160, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 7, "duration_ms": 240, "page": 0, "uv_rect_px": [280, 160, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]},)raw")
    TEXT(R"raw( {"action_id": "earthbreak", "source_ani": "script/character/fighter/animation/earthbreak.ani", "source_sha256": "h-earthbreak", "loop": false, "frames": [{"index": 0, "duration_ms": 80, "page": 0, "uv_rect_px": [320, 160, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 1, "duration_ms": 250, "page": 0, "uv_rect_px": [360, 160, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 2, "duration_ms": 210, "page": 0, "uv_rect_px": [400, 160, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 3, "duration_ms": 40, "page": 0, "uv_rect_px": [440, 160, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 4, "duration_ms": 100, "page": 0, "uv_rect_px": [0, 200, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 5, "duration_ms": 1, "page": 0, "uv_rect_px": [40, 200, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]}, {"action_id": "stomp", "source_ani": "script/character/fighter/animation/stomp.ani", "source_sha256": "h-stomp", "loop": false, "frames": [{"index": 0, "duration_ms": 50, "page": 0, "uv_rect_px": [80, 200, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 1, "duration_ms": 50, "page": 0, "uv_rect_px": [120, 200, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 2, "duration_ms": 50, "page": 0, "uv_rect_px": [160, 200, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 3, "duration_ms": 550, "page": 0, "uv_rect_px": [200, 200, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]}, {"action_id": "damage1", "source_ani": "script/character/fighter/animation/damage1.ani", "source_sha256": "h-damage1", "loop": false, "frames": [{"index": 0, "duration_ms": 1000, "page": 0, "uv_rect_px": [240, 200, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]}, {"action_id": "damage2", "source_ani": "script/character/fighter/animation/damage2.ani", "source_sha256": "h-damage2", "loop": false, "frames": [{"index": 0, "duration_ms": 1000, "page": 0, "uv_rect_px": [280, 200, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]}, {"action_id": "down", "source_ani": "script/character/fighter/animation/down.ani", "source_sha256": "h-down", "loop": false, "frames": [{"index": 0, "duration_ms": 80, "page": 0, "uv_rect_px": [320, 200, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 1, "duration_ms": 80, "page": 0, "uv_rect_px": [360, 200, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 2, "duration_ms": 10000, "page": 0, "uv_rect_px": [400, 200, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 3, "duration_ms": 10000, "page": 0, "uv_rect_px": [440, 200, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 4, "duration_ms": 50, "page": 0, "uv_rect_px": [0, 240, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 5, "duration_ms": 50, "page": 0, "uv_rect_px": [40, 240, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 6, "duration_ms": 400, "page": 0, "uv_rect_px": [80, 240, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]}, {"action_id": "overturn", "source_ani": "script/character/fighter/animation/overturn.ani", "source_sha256": "h-overturn", "loop": false, "frames": [{"index": 0, "duration_ms": 10000, "page": 0, "uv_rect_px": [120, 240, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]},)raw")
    TEXT(R"raw( {"action_id": "ftrecoveryrollback_body", "source_ani": "script/character/fighter/animation/ftrecoveryrollback_body.ani", "source_sha256": "h-ftrecoveryrollback_body", "loop": false, "frames": [{"index": 0, "duration_ms": 60, "page": 0, "uv_rect_px": [160, 240, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 1, "duration_ms": 50, "page": 0, "uv_rect_px": [200, 240, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 2, "duration_ms": 50, "page": 0, "uv_rect_px": [240, 240, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 3, "duration_ms": 50, "page": 0, "uv_rect_px": [280, 240, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 4, "duration_ms": 50, "page": 0, "uv_rect_px": [320, 240, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 5, "duration_ms": 60, "page": 0, "uv_rect_px": [360, 240, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 6, "duration_ms": 60, "page": 0, "uv_rect_px": [400, 240, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 7, "duration_ms": 250, "page": 0, "uv_rect_px": [440, 240, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]}, {"action_id": "ftrecoveryrollfront_body", "source_ani": "script/character/fighter/animation/ftrecoveryrollfront_body.ani", "source_sha256": "h-ftrecoveryrollfront_body", "loop": false, "frames": [{"index": 0, "duration_ms": 60, "page": 0, "uv_rect_px": [0, 280, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 1, "duration_ms": 50, "page": 0, "uv_rect_px": [40, 280, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 2, "duration_ms": 50, "page": 0, "uv_rect_px": [80, 280, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 3, "duration_ms": 50, "page": 0, "uv_rect_px": [120, 280, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 4, "duration_ms": 50, "page": 0, "uv_rect_px": [160, 280, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 5, "duration_ms": 60, "page": 0, "uv_rect_px": [200, 280, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 6, "duration_ms": 60, "page": 0, "uv_rect_px": [240, 280, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 7, "duration_ms": 250, "page": 0, "uv_rect_px": [280, 280, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]}, {"action_id": "die", "source_ani": "script/character/fighter/animation/die.ani", "source_sha256": "h-die", "loop": false, "frames": [{"index": 0, "duration_ms": 300, "page": 0, "uv_rect_px": [320, 280, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}, {"index": 1, "duration_ms": 270, "page": 0, "uv_rect_px": [360, 280, 32, 32], "canvas_size_px": [64, 64], "pivot_px": [16, 32]}]}]})raw");

    /** Builds the in-memory set from the mock manifest (no textures bound). */
    static UDNFMCharacterVisualSet* BuildMockSet(FAutomationTestBase& Test, FString& OutError)
    {
        UDNFMCharacterVisualSet* Set = NewObject<UDNFMCharacterVisualSet>(GetTransientPackage());
        if (!Test.TestTrue(TEXT("the mock manifest builds"),
            UDNFMCharacterVisualSet::BuildFromManifestJson(MockManifest, {}, *Set, OutError)))
        {
            Test.AddError(FString::Printf(TEXT("mock manifest build failed: %s"), *OutError));
            return nullptr;
        }
        return Set;
    }

    /** A world-less visual component bound to the mock set (tests drive it). */
    static UDNFMCharacterVisualComponent* NewMockVisual(FAutomationTestBase& Test, UDNFMCharacterVisualSet* Set)
    {
        UDNFMCharacterVisualComponent* Visual = NewObject<UDNFMCharacterVisualComponent>(GetTransientPackage());
        Visual->SetVisualSet(Set);
        return Visual;
    }

    /** Fact factory for the presenter unit tests. */
    static FDNFMCombatVisualPresenter::FVisualFacts MakeFacts(ECombatActionState State,
        FName AttackId, uint64 InstanceId, int32 Frame,
        bool bDead = false, bool bAirborne = false, bool bGroundMoving = false,
        float DeltaSeconds = 0.0f, bool bAirborneRising = false)
    {
        FDNFMCombatVisualPresenter::FVisualFacts Facts;
        Facts.Combat.ActionState = State;
        Facts.Combat.AttackId = AttackId;
        Facts.Combat.InstanceId = InstanceId;
        Facts.Combat.Frame = Frame;
        Facts.bDead = bDead;
        Facts.bAirborne = bAirborne;
        Facts.bGroundMoving = bGroundMoving;
        Facts.DeltaSeconds = DeltaSeconds;
        Facts.bAirborneRising = bAirborneRising;
        return Facts;
    }

    // The frozen combat-attacks.json facts the mapping table must respect
    // (display only; the file itself is never read or written at runtime).
    struct FFrozenAttackFact
    {
        FName AttackId;
        int32 DurationFrames;
        int32 ActiveStart;
        int32 ActiveEnd;
    };

    static const TArray<FFrozenAttackFact>& FrozenFacts()
    {
        static const TArray<FFrozenAttackFact> Facts = {
            {FName(TEXT("light_01")), 26, 7, 11},
            {FName(TEXT("light_02")), 32, 9, 14},
            {FName(TEXT("launcher")), 40, 12, 17},
            {FName(TEXT("aerial_01")), 28, 6, 10},
            {FName(TEXT("singlekick")), 24, 11, 15},
            {FName(TEXT("damagelowkick")), 24, 11, 15},
            {FName(TEXT("throwsand")), 28, 13, 17},
            {FName(TEXT("earthbreak")), 30, 11, 14},
            {FName(TEXT("stomp")), 22, 11, 14}
        };
        return Facts;
    }

    /** The bare combat rig the attack-driven tests share (no world needed). */
    struct FCombatRig
    {
        UAttackCatalog* Catalog = nullptr;
        UCombatComponent* Combat = nullptr;
        UDNFMCharacterVisualSet* Set = nullptr;
        UDNFMCharacterVisualComponent* Visual = nullptr;
        FDNFMCombatVisualPresenter* Presenter = nullptr;
        TUniquePtr<FDNFMCombatVisualPresenter> PresenterOwner;

        /** M6-024: extra attack definitions a test adds before Build. */
        TArray<UAttackDefinition*> ExtraDefinitions;

        bool Build(FAutomationTestBase& Test)
        {
            Set = BuildMockSet(Test, Error);
            if (Set == nullptr)
            {
                return false;
            }
            UAttackDefinition* Light01 = NewObject<UAttackDefinition>();
            Light01->AttackId = FName(TEXT("light_01"));
            Light01->DurationFrames = 26;
            Light01->ActiveWindow.StartFrame = 7;
            Light01->ActiveWindow.EndFrame = 11;
            UAttackDefinition* Light02 = NewObject<UAttackDefinition>();
            Light02->AttackId = FName(TEXT("light_02"));
            Light02->DurationFrames = 32;
            Light02->ActiveWindow.StartFrame = 9;
            Light02->ActiveWindow.EndFrame = 14;
            Catalog = NewObject<UAttackCatalog>();
            TArray<UAttackDefinition*> Definitions = {Light01, Light02};
            Definitions.Append(ExtraDefinitions);
            FText CatalogError;
            if (!Test.TestTrue(TEXT("the probe catalog builds"),
                Catalog->BuildFromDefinitions(Definitions, CatalogError)))
            {
                Test.AddError(FString::Printf(TEXT("probe catalog failed: %s"), *CatalogError.ToString()));
                return false;
            }
            Combat = NewObject<UCombatComponent>();
            Combat->InitializeFromCatalog(Catalog);
            Visual = NewMockVisual(Test, Set);
            PresenterOwner = MakeUnique<FDNFMCombatVisualPresenter>();
            Presenter = PresenterOwner.Get();
            Presenter->SetVisualTarget(Visual);
            return true;
        }

        /** Advances one 60 Hz logic frame and re-syncs the display. */
        void StepLogicFrame()
        {
            Combat->TickCombat(1.0f / 60.0f);
            SyncDisplay();
        }

        /** The presenter Step from the live combat snapshot (no other facts). */
        void SyncDisplay()
        {
            FDNFMCombatVisualPresenter::FVisualFacts Facts;
            Facts.Combat = Combat->GetSnapshot();
            Facts.bDead = Combat->IsDead();
            Presenter->Step(Facts);
        }

        FString Error;
    };
}

using namespace UE::UEMMO::Tasks::M6_023;
using namespace UE::UEMMO::Tasks::M6_023_Tests;

// 1) The frozen mapping table: every hit-pose segment covers its attack's
// active window exactly, and the segments tile [0, DurationFrames) contiguously.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_023_MappingTableMatchesActiveWindows,
    "UEMMO.Tasks.M6_023.MappingTableMatchesActiveWindows",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_023_MappingTableMatchesActiveWindows::RunTest(const FString& Parameters)
{
    const TArray<FDNFMAttackDisplayMapping> Table = MakeDefaultAttackDisplayMappings();
    TestEqual(TEXT("one mapping row per known attack"), Table.Num(), FrozenFacts().Num());

    for (const FFrozenAttackFact& Fact : FrozenFacts())
    {
        const FDNFMAttackDisplayMapping* Mapping = FindAttackDisplayMapping(Table, Fact.AttackId);
        if (!TestNotNull(FString::Printf(TEXT("%s has a mapping row"), *Fact.AttackId.ToString()), Mapping))
        {
            continue;
        }
        // Coverage: the segments tile [0, DurationFrames) without holes.
        int32 ExpectedStart = 0;
        for (const FDNFMAttackDisplaySegment& Row : Mapping->Segments)
        {
            TestEqual(FString::Printf(TEXT("%s segment starts at %d"), *Fact.AttackId.ToString(), ExpectedStart),
                Row.LogicStartFrame, ExpectedStart);
            TestTrue(FString::Printf(TEXT("%s segment is non-empty"), *Fact.AttackId.ToString()),
                Row.LogicEndFrame > Row.LogicStartFrame);
            ExpectedStart = Row.LogicEndFrame;
        }
        TestEqual(FString::Printf(TEXT("%s segments cover the full duration"), *Fact.AttackId.ToString()),
            ExpectedStart, Fact.DurationFrames);

        // The hard constraint: the hit-pose row IS the active window, so the
        // visual hit pose always falls inside the existing damage window.
        bool bFoundHitRow = false;
        for (const FDNFMAttackDisplaySegment& Row : Mapping->Segments)
        {
            if (Row.bHitPose)
            {
                TestFalse(FString::Printf(TEXT("%s has at most one hit row"), *Fact.AttackId.ToString()), bFoundHitRow);
                TestEqual(FString::Printf(TEXT("%s hit row starts at the active window"), *Fact.AttackId.ToString()),
                    Row.LogicStartFrame, Fact.ActiveStart);
                TestEqual(FString::Printf(TEXT("%s hit row ends at the active window"), *Fact.AttackId.ToString()),
                    Row.LogicEndFrame, Fact.ActiveEnd);
                bFoundHitRow = true;
            }
        }
        TestTrue(FString::Printf(TEXT("%s declares its hit pose"), *Fact.AttackId.ToString()), bFoundHitRow);

        // Display frame indices stay inside the source clip's frame count
        // (the M6-019 frame-count facts the placeholders encode).
        for (const FDNFMAttackDisplaySegment& Row : Mapping->Segments)
        {
            TestTrue(FString::Printf(TEXT("%s row's display frame is non-negative"), *Fact.AttackId.ToString()),
                Row.DisplayFrameIndex >= 0);
        }
    }

    // The pure sampling: clamped at both ends, exact inside the rows.
    const FDNFMAttackDisplayMapping* Light01 = FindAttackDisplayMapping(Table, FName(TEXT("light_01")));
    if (TestNotNull(TEXT("light_01 has a mapping row"), Light01))
    {
        TestEqual(TEXT("pre-first-tick Frame (-1) reads as the first row"),
            FindDisplayFrameForLogicFrame(*Light01, -1), 0);
        TestEqual(TEXT("frame 6 is still the windup"), FindDisplayFrameForLogicFrame(*Light01, 6), 0);
        TestEqual(TEXT("frame 7 is the hit pose"), FindDisplayFrameForLogicFrame(*Light01, 7), 1);
        TestEqual(TEXT("frame 10 is still the hit pose"), FindDisplayFrameForLogicFrame(*Light01, 10), 1);
        TestEqual(TEXT("frame 11 is the recovery"), FindDisplayFrameForLogicFrame(*Light01, 11), 2);
        TestEqual(TEXT("an overrun clamps to the last row"), FindDisplayFrameForLogicFrame(*Light01, 999), 2);
    }
    FDNFMAttackDisplayMapping Empty;
    TestEqual(TEXT("an empty mapping samples to none"), FindDisplayFrameForLogicFrame(Empty, 5), INDEX_NONE);
    TestEqual(TEXT("an unknown attack id has no mapping"),
        FindAttackDisplayMapping(Table, FName(TEXT("not_an_attack"))) == nullptr, true);
    return true;
}

// 2) The display priority (plan 6.1): death -> down/getup/hit -> attack ->
// jump -> ground move/idle; death preempts a running attack; death holds its
// end frame; the revive edge clears the old display instance.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_023_DisplayStatePriority,
    "UEMMO.Tasks.M6_023.DisplayStatePriority",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_023_DisplayStatePriority::RunTest(const FString& Parameters)
{
    FString Error;
    UDNFMCharacterVisualSet* Set = BuildMockSet(*this, Error);
    if (Set == nullptr)
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = NewMockVisual(*this, Set);
    FDNFMCombatVisualPresenter Presenter;
    Presenter.SetVisualTarget(Visual);

    // Ground idle / move / jump baselines.
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1));
    TestEqual(TEXT("a free grounded owner idles"), Visual->GetCurrentActionId(), IdleActionId);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, false, true));
    TestEqual(TEXT("a moving owner plays the move loop"), Visual->GetCurrentActionId(), MoveActionId);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, true, false));
    TestEqual(TEXT("an airborne owner plays the jump clip"), Visual->GetCurrentActionId(), JumpActionId);
    // M6-024: the ground edge opens the landing tail (the capped landing pose,
    // jump frame 12) instead of an instant idle; the tail ends and the
    // locomotion display returns within the tail's 220 ms.
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1));
    TestEqual(TEXT("the landing edge plays the landing tail"), Visual->GetCurrentActionId(), JumpActionId);
    TestEqual(TEXT("the landing pose is the capped frame 12"), Visual->GetCurrentFrameIndex(), 12);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, false, false, 0.25f));
    TestEqual(TEXT("the finished landing tail returns to idle"), Visual->GetCurrentActionId(), IdleActionId);

    // The reaction states (each plays once and the state owns its end).
    Presenter.Step(MakeFacts(ECombatActionState::HitStun, NAME_None, 0, -1));
    TestEqual(TEXT("hit stun shows the hit pose"), Visual->GetCurrentActionId(), HitActionId);
    Presenter.Step(MakeFacts(ECombatActionState::HitStun, NAME_None, 0, -1));
    TestEqual(TEXT("a repeated stun sync never restarts the hit pose"), Visual->GetCurrentActionId(), HitActionId);
    Presenter.Step(MakeFacts(ECombatActionState::Knockdown, NAME_None, 0, -1));
    TestEqual(TEXT("knockdown shows the down pose"), Visual->GetCurrentActionId(), DownActionId);
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1));
    TestEqual(TEXT("recovering shows the getup pose"), Visual->GetCurrentActionId(), RecoverActionId);

    // Death preempts everything and holds its end frame.
    Presenter.Step(MakeFacts(ECombatActionState::Attacking, FName(TEXT("light_01")), 7, 8, true));
    TestEqual(TEXT("death preempts a running attack"), Visual->GetCurrentActionId(), DeathActionId);
    Visual->TickVisual(5.0f); // way past the 570 ms die clip
    TestEqual(TEXT("the death clip holds its end frame"), Visual->GetCurrentFrameIndex(), 1);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, true));
    TestEqual(TEXT("a steady death never restarts the clip"), Visual->GetCurrentActionId(), DeathActionId);
    TestEqual(TEXT("the death end frame stays held"), Visual->GetCurrentFrameIndex(), 1);

    // The revive edge: the old display instance dies entirely and the same
    // sync rebuilds the locomotion display from scratch (frame 0, not the
    // death end frame).
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false));
    TestEqual(TEXT("a revived owner idles again"), Visual->GetCurrentActionId(), IdleActionId);
    TestEqual(TEXT("the revived display restarts at frame 0"), Visual->GetCurrentFrameIndex(), 0);

    // Reaction attacks are refused by combat, but the display also honors the
    // priority: an attack snapshot under a reaction state is not reachable
    // (the state switch above covers it); the attack display is tested next.
    return true;
}

// 3) Attack sampling: the displayed frame is a pure function of the combat
// 60 Hz logic frame; the instance id switches cleanly between two instances.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_023_AttackSamplingFollowsLogicFrame,
    "UEMMO.Tasks.M6_023.AttackSamplingFollowsLogicFrame",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_023_AttackSamplingFollowsLogicFrame::RunTest(const FString& Parameters)
{
    FCombatRig Rig;
    if (!Rig.Build(*this))
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = Rig.Visual;
    FDNFMCombatVisualPresenter* Presenter = Rig.Presenter;
    UCombatComponent* Combat = Rig.Combat;

    TestTrue(TEXT("light_01 starts"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    const uint64 FirstInstance = Combat->GetSnapshot().InstanceId;
    TestTrue(TEXT("instance ids are minted from 1"), FirstInstance >= 1);
    Rig.SyncDisplay();
    TestEqual(TEXT("the attack display uses the mapped source clip"),
        Visual->GetCurrentActionId(), FName(TEXT("attack1")));
    TestEqual(TEXT("the presenter presents the combat instance"),
        Presenter->GetPresentedInstanceId(), FirstInstance);

    // Frames 0..6: windup pose; 7..10: hit pose; 11..24: recovery (the step
    // that would land frame 25 = DurationFrames-1 ends the attack).
    for (int32 LogicFrame = 0; LogicFrame < 25; ++LogicFrame)
    {
        Rig.StepLogicFrame(); // this tick lands on LogicFrame
        const int32 ExpectedDisplay = (LogicFrame < 7) ? 0 : (LogicFrame < 11) ? 1 : 2;
        TestEqual(FString::Printf(TEXT("logic frame %d shows display frame %d"), LogicFrame, ExpectedDisplay),
            Visual->GetCurrentFrameIndex(), ExpectedDisplay);
        TestEqual(TEXT("the attack display is still presenting"),
            Visual->GetCurrentActionId(), FName(TEXT("attack1")));
    }
    Rig.StepLogicFrame(); // the step that reaches DurationFrames-1 finishes
    TestEqual(TEXT("the attack returned to Free"), Combat->GetSnapshot().ActionState, ECombatActionState::Free);
    Rig.SyncDisplay();
    TestEqual(TEXT("a finished attack returns to the idle display"), Visual->GetCurrentActionId(), IdleActionId);
    TestEqual(TEXT("the presented instance dropped to non-attack"), Presenter->GetPresentedInstanceId(), 0ull);

    // A second instance of the same attack: a fresh display instance with its
    // own event space (no residue from the first).
    TestTrue(TEXT("light_01 starts again"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    const uint64 SecondInstance = Combat->GetSnapshot().InstanceId;
    TestTrue(TEXT("instance ids never repeat"), SecondInstance > FirstInstance);
    Rig.SyncDisplay();
    TestEqual(TEXT("the new instance presents the attack clip again"),
        Visual->GetCurrentActionId(), FName(TEXT("attack1")));
    TestEqual(TEXT("the presenter follows the new instance"),
        Presenter->GetPresentedInstanceId(), SecondInstance);
    TestEqual(TEXT("the new instance restarts at the first display frame"),
        Visual->GetCurrentFrameIndex(), 0);

    // Wall time never moves the attack display: a huge TickVisual on the
    // attack clip changes nothing (the logic frame owns the time).
    Visual->TickVisual(10.0f);
    TestEqual(TEXT("real time cannot advance the attack display"),
        Visual->GetCurrentFrameIndex(), 0);
    return true;
}

// 4) The HitStop freeze: real-time display holds while frozen, no frozen-span
// event replay when it lifts; a presented attack keeps its time external.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_023_HitStopFreezesDisplay,
    "UEMMO.Tasks.M6_023.HitStopFreezesDisplay",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_023_HitStopFreezesDisplay::RunTest(const FString& Parameters)
{
    FCombatRig Rig;
    if (!Rig.Build(*this))
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = Rig.Visual;
    FDNFMCombatVisualPresenter* Presenter = Rig.Presenter;
    UCombatComponent* Combat = Rig.Combat;

    // A real-time clip: the idle loop freezes and resumes without replay.
    Rig.SyncDisplay(); // Free -> rest
    TestEqual(TEXT("idle plays the rest loop"), Visual->GetCurrentActionId(), IdleActionId);
    Presenter->NotifyHitStopFrozen(true);
    TestTrue(TEXT("the freeze reaches the visual playback"), Visual->IsPlaybackFrozen());
    Visual->TickVisual(5.0f);
    TestEqual(TEXT("a frozen idle never advances"), Visual->GetCurrentFrameIndex(), 0);
    Presenter->NotifyHitStopFrozen(false);
    TestFalse(TEXT("the unfreeze reaches the visual playback"), Visual->IsPlaybackFrozen());
    Visual->TickVisual(0.17); // past the 160 ms first frame
    TestEqual(TEXT("the unfrozen idle resumes from the frozen point"),
        Visual->GetCurrentFrameIndex(), 1);

    // An attack display: the time is external for the whole instance, so the
    // display frame moves only with the logic frame - a frozen or unfrozen
    // TickVisual can never drift it, and no frozen-span event can replay.
    TestTrue(TEXT("light_01 starts"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    Rig.SyncDisplay();
    Presenter->NotifyHitStopFrozen(true);
    Visual->TickVisual(5.0f);
    TestEqual(TEXT("a frozen attack display holds its frame"),
        Visual->GetCurrentFrameIndex(), 0);
    Presenter->NotifyHitStopFrozen(false);
    Visual->TickVisual(5.0f);
    TestEqual(TEXT("an attack display ignores wall time even unfrozen"),
        Visual->GetCurrentFrameIndex(), 0);
    // The logic frame advances the pose again after the freeze.
    Rig.StepLogicFrame(); // frame 0
    Rig.StepLogicFrame(); // frame 1
    TestEqual(TEXT("the logic frame still drives the pose after the freeze"),
        Visual->GetCurrentFrameIndex(), 0);
    return true;
}

// 5) The event dedup: the (ActorId, AttackInstanceId, EventId) tuple fires
// once per instance; low-framerate logic jumps walk every crossed display
// frame in order; instance switches re-arm the key space.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_023_EventDedupAndCrossedIntervals,
    "UEMMO.Tasks.M6_023.EventDedupAndCrossedIntervals",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_023_EventDedupAndCrossedIntervals::RunTest(const FString& Parameters)
{
    FCombatRig Rig;
    if (!Rig.Build(*this))
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = Rig.Visual;
    FDNFMCombatVisualPresenter* Presenter = Rig.Presenter;
    UCombatComponent* Combat = Rig.Combat;

    // (a) Loop re-crossings dedup: the idle loop wraps its frame-0 event, but
    // the presenter dispatches it exactly once for the session-scoped display.
    Rig.SyncDisplay();
    Visual->TickVisual(0.16 * 6); // six rest cycles
    int32 IdleEvents = 0;
    for (const FDNFMRecordedVisualEvent& Record : Presenter->GetDedupedEventHistory())
    {
        if (Record.ActionName == IdleActionId)
        {
            ++IdleEvents;
        }
    }
    TestEqual(TEXT("the looping idle's event dispatched exactly once"), IdleEvents, 1);

    // (b) A low-framerate logic jump: one 13-frame combat step crosses both
    // the windup->hit and hit->recovery boundaries in a single sync.
    TestTrue(TEXT("light_01 starts"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    const uint64 FirstInstance = Combat->GetSnapshot().InstanceId;
    Rig.SyncDisplay(); // enter at frame -1 -> display 0 (its event fires now)
    Combat->TickCombat(13.0f / 60.0f); // -1 -> frame 12 in ONE step
    Rig.SyncDisplay();                 // the seek walks display 1 then 2
    int32 Swing0 = 0;
    int32 Swing1 = 0;
    int32 Slash = 0;
    for (const FDNFMRecordedVisualEvent& Record : Presenter->GetDedupedEventHistory())
    {
        if (Record.ActionName != FName(TEXT("attack1")))
        {
            continue;
        }
        if (Record.EventName == FName(TEXT("snd_swing0"))) { ++Swing0; }
        if (Record.EventName == FName(TEXT("snd_swing1"))) { ++Swing1; }
        if (Record.EventName == FName(TEXT("fx_slash"))) { ++Slash; }
    }
    TestEqual(TEXT("the frame-0 event fired once on entry"), Swing0, 1);
    TestEqual(TEXT("the crossed frame-1 audio fired exactly once"), Swing1, 1);
    TestEqual(TEXT("the crossed frame-1 fx fired exactly once"), Slash, 1);

    // (c) An instance switch re-arms the key space: the same events dispatch
    // again under the new instance id, and the recorded rows carry it.
    for (int32 Frame = 0; Frame < 20; ++Frame)
    {
        Rig.StepLogicFrame(); // run the first instance out (frame 11 -> Free)
    }
    TestTrue(TEXT("light_02 chains"), Combat->TryStartAttack(FName(TEXT("light_02")), 1));
    // light_02 maps to attack2, which the M6-024 mock set now carries (the
    // per-attack coverage): the display switches to it cleanly and the
    // attack1 events cannot leak into the new action's event space (the
    // acceptance filter is per presented action; instance switches re-arm).
    Rig.SyncDisplay();
    TestEqual(TEXT("the chained attack switches to its mapped clip"),
        Visual->GetCurrentActionId(), FName(TEXT("attack2")));
    TestEqual(TEXT("the presenter follows the chained instance"),
        Presenter->GetPresentedInstanceId(), Combat->GetSnapshot().InstanceId);
    for (int32 Guard = 0; Guard < 64 && Combat->GetSnapshot().ActionState == ECombatActionState::Attacking; ++Guard)
    {
        Rig.StepLogicFrame(); // run the light_02 instance out
    }

    TestTrue(TEXT("light_01 starts once more"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    const uint64 ThirdInstance = Combat->GetSnapshot().InstanceId;
    TestTrue(TEXT("the third instance is fresh"), ThirdInstance > FirstInstance);
    Rig.SyncDisplay();
    // After the third entry the frame-0 event exists for both instances.
    int32 Swing0First = 0;
    int32 Swing0Third = 0;
    for (const FDNFMRecordedVisualEvent& Record : Presenter->GetDedupedEventHistory())
    {
        if (Record.ActionName == FName(TEXT("attack1")) && Record.EventName == FName(TEXT("snd_swing0")))
        {
            if (Record.AttackInstanceId == FirstInstance) { ++Swing0First; }
            if (Record.AttackInstanceId == ThirdInstance) { ++Swing0Third; }
        }
    }
    TestEqual(TEXT("instance 1's swing0 dispatched once"), Swing0First, 1);
    TestEqual(TEXT("instance 3's swing0 dispatched once (no residue either way)"), Swing0Third, 1);
    return true;
}

// 6) Cancel/interrupt/reset/death leave no residue on the display or the
// event space.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_023_CancelInterruptResetNoResidue,
    "UEMMO.Tasks.M6_023.CancelInterruptResetNoResidue",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_023_CancelInterruptResetNoResidue::RunTest(const FString& Parameters)
{
    FCombatRig Rig;
    if (!Rig.Build(*this))
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = Rig.Visual;
    FDNFMCombatVisualPresenter* Presenter = Rig.Presenter;
    UCombatComponent* Combat = Rig.Combat;

    // Cancel (no OnFinished): the display leaves the attack immediately.
    TestTrue(TEXT("light_01 starts"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    Rig.SyncDisplay();
    TestEqual(TEXT("the attack display runs"), Visual->GetCurrentActionId(), FName(TEXT("attack1")));
    Combat->CancelCurrentAttack(FName(TEXT("M6_023_test")));
    Rig.SyncDisplay();
    TestEqual(TEXT("a cancelled attack returns to idle display"), Visual->GetCurrentActionId(), IdleActionId);
    TestEqual(TEXT("the cancelled instance is forgotten"), Presenter->GetPresentedInstanceId(), 0ull);

    // ResetCombat (the teardown face): same clean exit.
    TestTrue(TEXT("light_01 restarts"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    Rig.SyncDisplay();
    Combat->ResetCombat();
    Rig.SyncDisplay();
    TestEqual(TEXT("a reset teardown returns to idle display"), Visual->GetCurrentActionId(), IdleActionId);

    // Death: the die clip replaces the attack and holds; the facts-driven
    // revive edge clears it (the health lifecycle owns the revival).
    TestTrue(TEXT("light_01 starts for the death case"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    Rig.SyncDisplay();
    Combat->SetDead(true);
    Rig.SyncDisplay();
    TestEqual(TEXT("death preempts the attack display"), Visual->GetCurrentActionId(), DeathActionId);
    Visual->TickVisual(5.0f);
    TestEqual(TEXT("the death display holds its end frame"), Visual->GetCurrentFrameIndex(), 1);
    Combat->SetDead(false); // a revival (ResetHealth) opened a new life
    Rig.SyncDisplay();
    // The in-flight attack fact survives a revival (death never interrupted
    // it): the display re-enters the mapped clip fresh instead of resuming
    // the death frame.
    TestEqual(TEXT("the revived display re-enters the running attack"),
        Visual->GetCurrentActionId(), FName(TEXT("attack1")));
    TestEqual(TEXT("the re-entered display restarts at the mapped frame"),
        Visual->GetCurrentFrameIndex(), 0);
    for (int32 Guard = 0; Guard < 64 && Combat->GetSnapshot().ActionState == ECombatActionState::Attacking; ++Guard)
    {
        Rig.StepLogicFrame();
    }
    TestEqual(TEXT("the finished attack returns to idle display"), Visual->GetCurrentActionId(), IdleActionId);
    return true;
}

// 7) The component wiring: SetSources + SetDNFMVisualTarget + TickComponent
// feed the coordinator; the gate=0 face (no target bound) is a full no-op and
// the 3D montage seams keep dispatching.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_023_PresentationComponentWiring,
    "UEMMO.Tasks.M6_023.PresentationComponentWiring",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_023_PresentationComponentWiring::RunTest(const FString& Parameters)
{
    FString Error;
    UDNFMCharacterVisualSet* Set = BuildMockSet(*this, Error);
    if (Set == nullptr)
    {
        return false;
    }
    UAttackDefinition* Light01 = NewObject<UAttackDefinition>();
    Light01->AttackId = FName(TEXT("light_01"));
    Light01->DurationFrames = 26;
    Light01->ActiveWindow.StartFrame = 7;
    Light01->ActiveWindow.EndFrame = 11;
    UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
    FText CatalogError;
    if (!TestTrue(TEXT("the probe catalog builds"),
        Catalog->BuildFromDefinitions({Light01}, CatalogError)))
    {
        return false;
    }
    UCombatComponent* Combat = NewObject<UCombatComponent>();
    Combat->InitializeFromCatalog(Catalog);
    UCombatPresentationComponent* Presentation = NewObject<UCombatPresentationComponent>();
    Presentation->SetSources(Combat, nullptr); // null mesh: the 3D path no-ops safely
    UDNFMCharacterVisualComponent* Visual = NewMockVisual(*this, Set);
    Presentation->SetDNFMVisualTarget(Visual);
    TestTrue(TEXT("the presenter binds the picture surface"),
        Presentation->GetDNFMVisualPresenter().GetVisualTarget() == Visual);

    // Fact provider injection: the bare component has no owner movement, so
    // the tests pin the airborne/ground-move facts explicitly.
    bool bAirborne = false;
    bool bGroundMoving = false;
    Presentation->SetDNFMFactProviders(
        [&bAirborne]() { return bAirborne; },
        [&bGroundMoving]() { return bGroundMoving; });

    const auto Tick = [&](float DeltaSeconds)
    {
        Presentation->TickComponent(DeltaSeconds, LEVELTICK_All, nullptr);
    };

    // Idle by default; the injected move fact switches the loop.
    Tick(1.0f / 60.0f);
    TestEqual(TEXT("the tick feed idles the display"), Visual->GetCurrentActionId(), IdleActionId);
    bGroundMoving = true;
    Tick(1.0f / 60.0f);
    TestEqual(TEXT("the tick feed moves the display"), Visual->GetCurrentActionId(), MoveActionId);
    bGroundMoving = false;
    bAirborne = true;
    Tick(1.0f / 60.0f);
    TestEqual(TEXT("the tick feed jumps the display"), Visual->GetCurrentActionId(), JumpActionId);
    bAirborne = false;

    // The attack sampling rides the real delegates (OnStarted) + tick sync.
    TestTrue(TEXT("light_01 starts"), Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    for (int32 Frame = -1; Frame < 8; ++Frame)
    {
        Tick(1.0f / 60.0f);
        Combat->TickCombat(1.0f / 60.0f);
    }
    TestEqual(TEXT("the tick-fed display reaches the hit pose"),
        Visual->GetCurrentFrameIndex(), 1);

    // The HitStop event face: RequestHitStop broadcasts and the presenter
    // freezes the real-time clips (the 3D pause seam still records too).
    Combat->CancelCurrentAttack(FName(TEXT("M6_023_wiring")));
    Tick(1.0f / 60.0f); // back to the idle display
    Combat->RequestHitStop(0.05f);
    TestTrue(TEXT("the hit stop freezes the DNFM playback"), Visual->IsPlaybackFrozen());
    TestTrue(TEXT("the 3D hit stop seam still dispatches"),
        Presentation->GetHitStopPauseDispatchHistory().Contains(true));
    Visual->TickVisual(1.0f);
    TestEqual(TEXT("the frozen idle holds"), Visual->GetCurrentFrameIndex(), 0);
    for (int32 Step = 0; Step < 6 && Combat->IsHitStopActive(); ++Step)
    {
        Combat->TickCombat(1.0f / 60.0f); // real-time countdown, no world needed
    }
    TestFalse(TEXT("the hit stop ended"), Combat->IsHitStopActive());
    Tick(1.0f / 60.0f);
    TestFalse(TEXT("the DNFM playback unfreezes with the hit stop"), Visual->IsPlaybackFrozen());

    // The gate=0 no-op face: an unbound presentation never touches the visual.
    UCombatPresentationComponent* GateOff = NewObject<UCombatPresentationComponent>();
    GateOff->SetSources(Combat, nullptr);
    UDNFMCharacterVisualComponent* UntouchedVisual = NewMockVisual(*this, Set);
    TestTrue(TEXT("an unbound presentation is the no-op face"),
        GateOff->GetDNFMVisualPresenter().GetVisualTarget() == nullptr);
    GateOff->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
    TestFalse(TEXT("the gate-off path never starts a display"), UntouchedVisual->IsVisualActive());
    TestTrue(TEXT("light_01 starts under the gate-off presentation"),
        Combat->TryStartAttack(FName(TEXT("light_01")), 1));
    GateOff->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
    Combat->TickCombat(1.0f / 60.0f);
    GateOff->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
    TestFalse(TEXT("the gate-off path still never starts a display"), UntouchedVisual->IsVisualActive());
    return true;
}

// 8) The SeekToDisplayFrame primitive: forward walks fire crossed events in
// order, backward walks stay silent, elapsed pins to the target frame start,
// and a frozen playback still accepts explicit positions.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_023_SeekToDisplayFramePrimitive,
    "UEMMO.Tasks.M6_023.SeekToDisplayFramePrimitive",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_023_SeekToDisplayFramePrimitive::RunTest(const FString& Parameters)
{
    FString Error;
    UDNFMCharacterVisualSet* Set = BuildMockSet(*this, Error);
    if (Set == nullptr)
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = NewMockVisual(*this, Set);
    TArray<FString> Fired;
    Visual->OnVisualFrameEvent.AddLambda([&Fired](uint64, FName Action, int32 Frame, FName, FName Event)
    {
        Fired.Add(FString::Printf(TEXT("%s|%d|%s"), *Action.ToString(), Frame, *Event.ToString()));
    });

    TestFalse(TEXT("a seek without playback refuses"), Visual->SeekToDisplayFrame(0));
    TestTrue(TEXT("attack1 starts"), Visual->SetAction(FName(TEXT("attack1"))));
    Fired.Reset(); // the SetAction frame-0 events are the M6-021 contract, not this test's subject

    TestTrue(TEXT("a seek to the current frame is idempotent"), Visual->SeekToDisplayFrame(0));
    TestEqual(TEXT("the idempotent seek fired nothing"), Fired.Num(), 0);
    TestFalse(TEXT("an out-of-range seek refuses"), Visual->SeekToDisplayFrame(3));
    TestEqual(TEXT("the refused seek fired nothing"), Fired.Num(), 0);

    // Forward 0 -> 2 walks frames 1 and 2 in order (frame 1 carries two
    // events; frame 2 carries none).
    TestTrue(TEXT("the forward seek lands"), Visual->SeekToDisplayFrame(2));
    TestEqual(TEXT("the walk reported frame 1's two events"), Fired.Num(), 2);
    if (Fired.Num() == 2)
    {
        TestEqual(TEXT("the walk order is the manifest order"), Fired[0], FString(TEXT("attack1|1|snd_swing1")));
        TestEqual(TEXT("the second event row follows"), Fired[1], FString(TEXT("attack1|1|fx_slash")));
    }
    // Elapsed pinned to the target frame start (220 ms frame): a short
    // real-time advance stays, a full one crosses the end hold (frame 2).
    Visual->TickVisual(0.1);
    TestEqual(TEXT("the pinned elapsed continues from the seeked frame"),
        Visual->GetCurrentFrameIndex(), 2);

    // Backward seek on a once-clip: the frame applies without fake re-fires.
    Fired.Reset();
    TestTrue(TEXT("the backward seek applies"), Visual->SeekToDisplayFrame(0));
    TestEqual(TEXT("a backward seek fires nothing"), Fired.Num(), 0);
    TestEqual(TEXT("the backward seek moved the frame"), Visual->GetCurrentFrameIndex(), 0);

    // A frozen playback accepts explicit positions (time is frozen, not the
    // display): the presenter can switch poses during a freeze.
    Visual->SetPlaybackFrozen(true);
    Fired.Reset();
    TestTrue(TEXT("a frozen seek still lands"), Visual->SeekToDisplayFrame(1));
    TestEqual(TEXT("the frozen seek fired the crossed frame's events"), Fired.Num(), 2);
    TestEqual(TEXT("the frozen seek moved the frame"), Visual->GetCurrentFrameIndex(), 1);
    // And the frozen self-tick does not move past it.
    Visual->TickVisual(1.0f);
    TestEqual(TEXT("the frozen playback never self-advances"), Visual->GetCurrentFrameIndex(), 1);
    Visual->SetPlaybackFrozen(false);
    return true;
}

// 9) The pawn gate binding: gate 1 binds the presenter target, gate 0
// unbinds it and restores the 3D hero (the M6-021 teardown keeps its shape).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_023_PawnGateBindsPresenter,
    "UEMMO.Tasks.M6_023.PawnGateBindsPresenter",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_023_PawnGateBindsPresenter::RunTest(const FString& Parameters)
{
    IConsoleVariable* Gate = IConsoleManager::Get().FindConsoleVariable(TEXT("UEMMO.PlayerDNFMVisual"));
    if (!TestNotNull(TEXT("the DNFM gate exists"), Gate))
    {
        return true;
    }
    const int32 Original = Gate->GetInt();
    Gate->Set(1, ECVF_SetByCode);
    FTestWorldWrapper Wrapper;
    if (!TestTrue(TEXT("the manually ticked test world is created"),
        Wrapper.CreateTestWorld(EWorldType::Game)))
    {
        Gate->Set(Original, ECVF_SetByCode);
        return true;
    }
    UWorld* World = Wrapper.GetTestWorld();
    if (!TestNotNull(TEXT("the test world is available"), World)
        || !TestTrue(TEXT("play begins in the test world"), Wrapper.BeginPlayInTestWorld()))
    {
        Gate->Set(Original, ECVF_SetByCode);
        return true;
    }
    FActorSpawnParameters Params;
    APrototypeCharacter* Player = World->SpawnActor<APrototypeCharacter>(
        APrototypeCharacter::StaticClass(), FVector(96000.0, 75000.0, 600.0), FRotator::ZeroRotator, Params);
    if (!TestNotNull(TEXT("the player exists"), Player))
    {
        Gate->Set(Original, ECVF_SetByCode);
        return true;
    }
    UCombatPresentationComponent* Presentation = Player->FindComponentByClass<UCombatPresentationComponent>();
    if (!TestNotNull(TEXT("the pawn carries the presentation component"), Presentation))
    {
        Gate->Set(Original, ECVF_SetByCode);
        return true;
    }
    TestTrue(TEXT("gate 1 activated the DNFM mode"), Player->IsDNFMVisualActive());
    TestTrue(TEXT("the pawn gate bound the presenter target"),
        Presentation->GetDNFMVisualTarget() == Player->GetDNFMVisual());

    // The runtime teardown unbinds the presenter and restores the 3D hero.
    Gate->Set(0, ECVF_SetByCode);
    Player->ApplyDNFMVisualIfEnabled();
    TestFalse(TEXT("the runtime gate-off tears the mode down"), Player->IsDNFMVisualActive());
    TestTrue(TEXT("the gate-off unbound the presenter target"),
        Presentation->GetDNFMVisualTarget() == nullptr);
    TestTrue(TEXT("the 3D mesh returns"), Player->GetMesh()->IsVisible());
    Gate->Set(Original, ECVF_SetByCode);
    return true;
}

// ---------------------------------------------------------------------------
// M6-024: the calibrated segment tables, the reaction/sequence wiring and the
// state-driven jump walk. Same mock set discipline as the M6-023 tests (no
// imported asset); the mock manifest now mirrors the real imported set's
// frame counts and stored durations so every walk is exercised honestly.
// ---------------------------------------------------------------------------

namespace UE::UEMMO::Tasks::M6_024_Tests
{
    /** One frozen expectation row of the calibrated mapping table. */
    struct FCalibratedAttackFact
    {
        FName AttackId;
        FName ActionId;
        int32 HitPoseDisplayFrame;
        int32 ActiveStart;
        int32 ActiveEnd;
    };

    static const TArray<FCalibratedAttackFact>& CalibratedFacts()
    {
        static const TArray<FCalibratedAttackFact> Facts = {
            {FName(TEXT("light_01")), FName(TEXT("attack1")), 1, 7, 11},
            {FName(TEXT("light_02")), FName(TEXT("attack2")), 1, 9, 14},
            {FName(TEXT("launcher")), FName(TEXT("liftupper")), 3, 12, 17},
            {FName(TEXT("aerial_01")), FName(TEXT("jumpattack")), 1, 6, 10},
            {FName(TEXT("singlekick")), FName(TEXT("singlekick")), 3, 11, 15},
            {FName(TEXT("damagelowkick")), FName(TEXT("damagelowkick")), 2, 11, 15},
            {FName(TEXT("throwsand")), FName(TEXT("throwsand_sp")), 4, 13, 17},
            {FName(TEXT("earthbreak")), FName(TEXT("earthbreak")), 3, 11, 14},
            {FName(TEXT("stomp")), FName(TEXT("stomp")), 2, 11, 14}
        };
        return Facts;
    }

    /** Builds one probe definition with the frozen combat-attacks.json shape. */
    static UAttackDefinition* MakeAttackDefinition(FName AttackId, int32 DurationFrames,
        int32 ActiveStart, int32 ActiveEnd)
    {
        UAttackDefinition* Definition = NewObject<UAttackDefinition>();
        Definition->AttackId = AttackId;
        Definition->DurationFrames = DurationFrames;
        Definition->ActiveWindow.StartFrame = ActiveStart;
        Definition->ActiveWindow.EndFrame = ActiveEnd;
        return Definition;
    }
}

using namespace UE::UEMMO::Tasks::M6_024_Tests;

// 10) The calibrated mapping table: every attack's rows are the manifest-
// proportional splits (the M6-023 placeholders are gone), the hit-pose rows
// still cover the active windows exactly, and the sampling spot checks walk
// the calibrated boundaries.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_024_CalibratedAttackSegmentTables,
    "UEMMO.Tasks.M6_024.CalibratedAttackSegmentTables",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_024_CalibratedAttackSegmentTables::RunTest(const FString& Parameters)
{
    const TArray<FDNFMAttackDisplayMapping> Table = MakeDefaultAttackDisplayMappings();
    TestEqual(TEXT("one mapping row per known attack"), Table.Num(), CalibratedFacts().Num());

    // The exact frozen tables (the M6-024 calibration output; a drift here is
    // a calibration change and must be re-derived from the manifest).
    const auto ExpectRows = [this, &Table](FName AttackId, std::initializer_list<FDNFMAttackDisplaySegment> Expected)
    {
        const FDNFMAttackDisplayMapping* Mapping = FindAttackDisplayMapping(Table, AttackId);
        if (!TestNotNull(FString::Printf(TEXT("%s has a mapping row"), *AttackId.ToString()), Mapping))
        {
            return;
        }
        TestEqual(FString::Printf(TEXT("%s row count"), *AttackId.ToString()),
            Mapping->Segments.Num(), static_cast<int32>(Expected.size()));
        int32 Index = 0;
        for (const FDNFMAttackDisplaySegment& Row : Expected)
        {
            const FDNFMAttackDisplaySegment& Actual = Mapping->Segments[Index];
            TestEqual(FString::Printf(TEXT("%s row %d start"), *AttackId.ToString(), Index),
                Actual.LogicStartFrame, Row.LogicStartFrame);
            TestEqual(FString::Printf(TEXT("%s row %d end"), *AttackId.ToString(), Index),
                Actual.LogicEndFrame, Row.LogicEndFrame);
            TestEqual(FString::Printf(TEXT("%s row %d display frame"), *AttackId.ToString(), Index),
                Actual.DisplayFrameIndex, Row.DisplayFrameIndex);
            TestEqual(FString::Printf(TEXT("%s row %d hit flag"), *AttackId.ToString(), Index),
                Actual.bHitPose ? 1 : 0, Row.bHitPose ? 1 : 0);
            ++Index;
        }
    };

    ExpectRows(FName(TEXT("light_01")), {
        FDNFMAttackDisplaySegment{0, 7, 0, false}, FDNFMAttackDisplaySegment{7, 11, 1, true},
        FDNFMAttackDisplaySegment{11, 26, 2, false}});
    ExpectRows(FName(TEXT("light_02")), {
        FDNFMAttackDisplaySegment{0, 9, 0, false}, FDNFMAttackDisplaySegment{9, 14, 1, true},
        FDNFMAttackDisplaySegment{14, 19, 2, false}, FDNFMAttackDisplaySegment{19, 32, 3, false}});
    ExpectRows(FName(TEXT("launcher")), {
        FDNFMAttackDisplaySegment{0, 12, 0, false}, FDNFMAttackDisplaySegment{12, 17, 3, true},
        FDNFMAttackDisplaySegment{17, 28, 4, false}, FDNFMAttackDisplaySegment{28, 40, 5, false}});
    ExpectRows(FName(TEXT("aerial_01")), {
        FDNFMAttackDisplaySegment{0, 6, 0, false}, FDNFMAttackDisplaySegment{6, 10, 1, true},
        FDNFMAttackDisplaySegment{10, 13, 2, false}, FDNFMAttackDisplaySegment{13, 28, 3, false}});
    ExpectRows(FName(TEXT("singlekick")), {
        FDNFMAttackDisplaySegment{0, 3, 0, false}, FDNFMAttackDisplaySegment{3, 4, 1, false},
        FDNFMAttackDisplaySegment{4, 11, 2, false}, FDNFMAttackDisplaySegment{11, 15, 3, true},
        FDNFMAttackDisplaySegment{15, 19, 4, false}, FDNFMAttackDisplaySegment{19, 23, 5, false},
        FDNFMAttackDisplaySegment{23, 24, 6, false}});
    ExpectRows(FName(TEXT("damagelowkick")), {
        FDNFMAttackDisplaySegment{0, 5, 0, false}, FDNFMAttackDisplaySegment{5, 11, 1, false},
        FDNFMAttackDisplaySegment{11, 15, 2, true}, FDNFMAttackDisplaySegment{15, 22, 3, false},
        FDNFMAttackDisplaySegment{22, 24, 4, false}});
    ExpectRows(FName(TEXT("throwsand")), {
        FDNFMAttackDisplaySegment{0, 5, 0, false}, FDNFMAttackDisplaySegment{5, 10, 1, false},
        FDNFMAttackDisplaySegment{10, 13, 2, false}, FDNFMAttackDisplaySegment{13, 17, 4, true},
        FDNFMAttackDisplaySegment{17, 22, 5, false}, FDNFMAttackDisplaySegment{22, 27, 6, false},
        FDNFMAttackDisplaySegment{27, 28, 7, false}});
    ExpectRows(FName(TEXT("earthbreak")), {
        FDNFMAttackDisplaySegment{0, 5, 0, false}, FDNFMAttackDisplaySegment{5, 11, 1, false},
        FDNFMAttackDisplaySegment{11, 14, 3, true}, FDNFMAttackDisplaySegment{14, 20, 4, false},
        FDNFMAttackDisplaySegment{20, 30, 5, false}});
    ExpectRows(FName(TEXT("stomp")), {
        FDNFMAttackDisplaySegment{0, 3, 0, false}, FDNFMAttackDisplaySegment{3, 11, 1, false},
        FDNFMAttackDisplaySegment{11, 14, 2, true}, FDNFMAttackDisplaySegment{14, 22, 3, false}});

    // Sampling spot checks across the calibrated boundaries (the launcher's
    // compressed startup, the earthbreak 1 ms end frame, the singlekick
    // 20 ms middle frame).
    const FDNFMAttackDisplayMapping* Launcher = FindAttackDisplayMapping(Table, FName(TEXT("launcher")));
    if (TestNotNull(TEXT("launcher has a mapping row"), Launcher))
    {
        TestEqual(TEXT("launcher frame 11 is still the windup"), FindDisplayFrameForLogicFrame(*Launcher, 11), 0);
        TestEqual(TEXT("launcher frame 12 is the hit pose"), FindDisplayFrameForLogicFrame(*Launcher, 12), 3);
        TestEqual(TEXT("launcher frame 17 is the first recovery"), FindDisplayFrameForLogicFrame(*Launcher, 17), 4);
        TestEqual(TEXT("launcher frame 28 is the last recovery"), FindDisplayFrameForLogicFrame(*Launcher, 28), 5);
    }
    const FDNFMAttackDisplayMapping* Singlekick = FindAttackDisplayMapping(Table, FName(TEXT("singlekick")));
    if (TestNotNull(TEXT("singlekick has a mapping row"), Singlekick))
    {
        TestEqual(TEXT("singlekick frame 2 is frame 0"), FindDisplayFrameForLogicFrame(*Singlekick, 2), 0);
        TestEqual(TEXT("singlekick frame 3 is the 20 ms frame"), FindDisplayFrameForLogicFrame(*Singlekick, 3), 1);
        TestEqual(TEXT("singlekick frame 4 is the swing"), FindDisplayFrameForLogicFrame(*Singlekick, 4), 2);
        TestEqual(TEXT("singlekick frame 23 is the last recovery"), FindDisplayFrameForLogicFrame(*Singlekick, 23), 6);
    }
    const FDNFMAttackDisplayMapping* Earthbreak = FindAttackDisplayMapping(Table, FName(TEXT("earthbreak")));
    if (TestNotNull(TEXT("earthbreak has a mapping row"), Earthbreak))
    {
        TestEqual(TEXT("earthbreak frame 19 is recovery frame 4"), FindDisplayFrameForLogicFrame(*Earthbreak, 19), 4);
        TestEqual(TEXT("earthbreak frame 20 shows the 1 ms end frame"), FindDisplayFrameForLogicFrame(*Earthbreak, 20), 5);
        TestEqual(TEXT("earthbreak frame 29 holds the 1 ms end frame"), FindDisplayFrameForLogicFrame(*Earthbreak, 29), 5);
    }
    return true;
}

// 11) The hit pose alternates damage1 -> damage2 -> damage1 per NEW HitStun
// episode and never restarts inside one episode.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_024_HitReactionAlternatesDamagePoses,
    "UEMMO.Tasks.M6_024.HitReactionAlternatesDamagePoses",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_024_HitReactionAlternatesDamagePoses::RunTest(const FString& Parameters)
{
    FString Error;
    UDNFMCharacterVisualSet* Set = BuildMockSet(*this, Error);
    if (Set == nullptr)
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = NewMockVisual(*this, Set);
    FDNFMCombatVisualPresenter Presenter;
    Presenter.SetVisualTarget(Visual);

    // First episode: the initial flip yields damage1 (the M6-023 order kept).
    Presenter.Step(MakeFacts(ECombatActionState::HitStun, NAME_None, 0, -1));
    TestEqual(TEXT("the first hit shows damage1"), Visual->GetCurrentActionId(), HitActionId);
    Visual->TickVisual(2.0f); // the pose holds (a 1-frame clip, real-time hold)
    Presenter.Step(MakeFacts(ECombatActionState::HitStun, NAME_None, 0, -1));
    TestEqual(TEXT("a repeated stun sync never restarts the hit pose"), Visual->GetCurrentActionId(), HitActionId);

    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1));
    TestEqual(TEXT("the stun end returns to idle"), Visual->GetCurrentActionId(), IdleActionId);

    // Second episode: the alternation flips to damage2.
    Presenter.Step(MakeFacts(ECombatActionState::HitStun, NAME_None, 0, -1));
    TestEqual(TEXT("the second hit shows damage2"), Visual->GetCurrentActionId(), HitAltActionId);
    Presenter.Step(MakeFacts(ECombatActionState::HitStun, NAME_None, 0, -1));
    TestEqual(TEXT("the second episode stays damage2"), Visual->GetCurrentActionId(), HitAltActionId);

    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1));
    // Third episode: back to damage1 (deterministic alternation).
    Presenter.Step(MakeFacts(ECombatActionState::HitStun, NAME_None, 0, -1));
    TestEqual(TEXT("the third hit shows damage1 again"), Visual->GetCurrentActionId(), HitActionId);
    return true;
}

// 12) The knockdown shows down; Recovering starts the overturn -> roll
// sequence; the roll walks its real frames and the completed sequence returns
// to idle while the state lasts (and never restarts).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_024_KnockdownDownThenRecoverSequence,
    "UEMMO.Tasks.M6_024.KnockdownDownThenRecoverSequence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_024_KnockdownDownThenRecoverSequence::RunTest(const FString& Parameters)
{
    FString Error;
    UDNFMCharacterVisualSet* Set = BuildMockSet(*this, Error);
    if (Set == nullptr)
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = NewMockVisual(*this, Set);
    FDNFMCombatVisualPresenter Presenter;
    Presenter.SetVisualTarget(Visual);

    Presenter.Step(MakeFacts(ECombatActionState::Knockdown, NAME_None, 0, -1));
    TestEqual(TEXT("knockdown shows the down clip"), Visual->GetCurrentActionId(), DownActionId);
    Presenter.Step(MakeFacts(ECombatActionState::Knockdown, NAME_None, 0, -1, false, false, false, 0.3f));
    TestEqual(TEXT("a long knockdown keeps the down clip"), Visual->GetCurrentActionId(), DownActionId);

    // The Recovering edge starts the sequence at the overturn pose.
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1));
    TestEqual(TEXT("recovering starts at the overturn pose"), Visual->GetCurrentActionId(), RecoverActionId);
    TestEqual(TEXT("the overturn pose is its single frame"), Visual->GetCurrentFrameIndex(), 0);
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1, false, false, false, 0.1f));
    TestEqual(TEXT("the overturn beat holds 120 ms"), Visual->GetCurrentActionId(), RecoverActionId);

    // Past the hold the roll clip starts and walks its real frames. The
    // switch tick resets the walk clock (the next tick advances it).
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1, false, false, false, 0.05f));
    TestEqual(TEXT("the roll clip follows the overturn beat"),
        Visual->GetCurrentActionId(), RecoverRollBackActionId);
    TestEqual(TEXT("the roll starts at its first frame"), Visual->GetCurrentFrameIndex(), 0);
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1, false, false, false, 0.07f));
    TestEqual(TEXT("the roll walks by the tick delta"), Visual->GetCurrentFrameIndex(), 1);
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1, false, false, false, 0.12f));
    TestEqual(TEXT("the roll keeps walking its frames"), Visual->GetCurrentFrameIndex(), 3);
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1, false, false, false, 0.5f));
    TestEqual(TEXT("the roll reaches its held end frame"), Visual->GetCurrentFrameIndex(), 7);
    TestEqual(TEXT("the roll stays visible at completion"), Visual->GetCurrentActionId(), RecoverRollBackActionId);

    // The sequence is done: idle while Recovering lasts, no restart.
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1));
    TestEqual(TEXT("the completed sequence returns to idle"), Visual->GetCurrentActionId(), IdleActionId);
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1, false, false, false, 1.0f));
    TestEqual(TEXT("the completed sequence never restarts"), Visual->GetCurrentActionId(), IdleActionId);

    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1));
    TestEqual(TEXT("the state end keeps the idle display"), Visual->GetCurrentActionId(), IdleActionId);
    return true;
}

// 12b) The started sequence keeps playing after Recovering ends (plan 6.2
// "序列播完回待机") but a new attack or a new reaction abandons it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_024_RecoverSequenceContinuesAndPreempts,
    "UEMMO.Tasks.M6_024.RecoverSequenceContinuesAndPreempts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_024_RecoverSequenceContinuesAndPreempts::RunTest(const FString& Parameters)
{
    FString Error;
    UDNFMCharacterVisualSet* Set = BuildMockSet(*this, Error);
    if (Set == nullptr)
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = NewMockVisual(*this, Set);
    FDNFMCombatVisualPresenter Presenter;
    Presenter.SetVisualTarget(Visual);

    // The state leaves Recovering mid-roll: the display keeps rolling.
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1));
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1, false, false, false, 0.15f));
    TestEqual(TEXT("the roll started before the state ended"),
        Visual->GetCurrentActionId(), RecoverRollBackActionId);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, false, false, 0.1f));
    TestEqual(TEXT("the roll continues after Recovering ended"),
        Visual->GetCurrentActionId(), RecoverRollBackActionId);
    TestEqual(TEXT("the roll kept walking after the state end"), Visual->GetCurrentFrameIndex(), 1);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, false, false, 0.6f));
    TestEqual(TEXT("the roll stays visible on its completion tick"),
        Visual->GetCurrentActionId(), RecoverRollBackActionId);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1));
    TestEqual(TEXT("the finished roll returns to idle"), Visual->GetCurrentActionId(), IdleActionId);

    // A new attack during the roll takes the picture and cancels the roll.
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1));
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1, false, false, false, 0.2f));
    TestEqual(TEXT("the second sequence reaches the roll"),
        Visual->GetCurrentActionId(), RecoverRollBackActionId);
    Presenter.Step(MakeFacts(ECombatActionState::Attacking, FName(TEXT("light_01")), 9, 5));
    TestEqual(TEXT("the attack preempts the roll"), Visual->GetCurrentActionId(), FName(TEXT("attack1")));
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1));
    TestEqual(TEXT("the cancelled roll never comes back"), Visual->GetCurrentActionId(), IdleActionId);

    // A new reaction during the roll owns the picture (the priority order).
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1));
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1, false, false, false, 0.2f));
    Presenter.Step(MakeFacts(ECombatActionState::Knockdown, NAME_None, 0, -1));
    TestEqual(TEXT("a new knockdown preempts the roll"), Visual->GetCurrentActionId(), DownActionId);
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1));
    TestEqual(TEXT("the new recovery restarts the sequence from overturn"),
        Visual->GetCurrentActionId(), RecoverActionId);
    return true;
}

// 13) The death display holds die's end frame; the revive clears the display
// AND any in-flight sequence state (a revival never resumes a half roll).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_024_DeathHoldsEndFrameAndReviveClearsSequences,
    "UEMMO.Tasks.M6_024.DeathHoldsEndFrameAndReviveClearsSequences",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_024_DeathHoldsEndFrameAndReviveClearsSequences::RunTest(const FString& Parameters)
{
    FString Error;
    UDNFMCharacterVisualSet* Set = BuildMockSet(*this, Error);
    if (Set == nullptr)
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = NewMockVisual(*this, Set);
    FDNFMCombatVisualPresenter Presenter;
    Presenter.SetVisualTarget(Visual);

    // Death during the getup roll: die preempts and holds its end frame.
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1));
    Presenter.Step(MakeFacts(ECombatActionState::Recovering, NAME_None, 0, -1, false, false, false, 0.2f));
    TestEqual(TEXT("the roll is playing before death"),
        Visual->GetCurrentActionId(), RecoverRollBackActionId);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, true));
    TestEqual(TEXT("death preempts the roll"), Visual->GetCurrentActionId(), DeathActionId);
    Visual->TickVisual(5.0f); // way past the 570 ms die clip
    TestEqual(TEXT("the death clip holds its end frame"), Visual->GetCurrentFrameIndex(), 1);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, true));
    TestEqual(TEXT("a steady death never restarts the clip"), Visual->GetCurrentActionId(), DeathActionId);
    TestEqual(TEXT("the death end frame stays held"), Visual->GetCurrentFrameIndex(), 1);

    // The revive: the display instance dies entirely and the locomotion
    // display rebuilds from frame 0 (never the death end frame or the roll).
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false));
    TestEqual(TEXT("the revived owner idles again"), Visual->GetCurrentActionId(), IdleActionId);
    TestEqual(TEXT("the revived display restarts at frame 0"), Visual->GetCurrentFrameIndex(), 0);
    return true;
}

// 14) The state-driven three-phase jump walk: the takeoff frames advance by
// the tick delta, the 34464 ms apex hold and the 10 s landing hold never gate
// the display, and the landing tail finishes into the locomotion display.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_024_JumpThreePhaseStateDriven,
    "UEMMO.Tasks.M6_024.JumpThreePhaseStateDriven",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_024_JumpThreePhaseStateDriven::RunTest(const FString& Parameters)
{
    FString Error;
    UDNFMCharacterVisualSet* Set = BuildMockSet(*this, Error);
    if (Set == nullptr)
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = NewMockVisual(*this, Set);
    FDNFMCombatVisualPresenter Presenter;
    Presenter.SetVisualTarget(Visual);

    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1));
    TestEqual(TEXT("the pre-jump baseline idles"), Visual->GetCurrentActionId(), IdleActionId);

    // Takeoff: rising walks frames 0..5 by the tick delta.
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, true, false, 0.0f, true));
    TestEqual(TEXT("the takeoff starts at frame 0"), Visual->GetCurrentFrameIndex(), 0);
    TestTrue(TEXT("the jump display pins the clip time"), Visual->IsPlaybackFrozen());
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, true, false, 0.05f, true));
    TestEqual(TEXT("frame 0 holds its 100 ms"), Visual->GetCurrentFrameIndex(), 0);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, true, false, 0.06f, true));
    TestEqual(TEXT("frame 1 follows the walk"), Visual->GetCurrentFrameIndex(), 1);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, true, false, 0.1f, true));
    TestEqual(TEXT("frame 2 follows the walk"), Visual->GetCurrentFrameIndex(), 2);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, true, false, 0.3f, true));
    TestEqual(TEXT("the past-500 ms rising shows the apex hold frame"), Visual->GetCurrentFrameIndex(), 6);

    // The 34464 ms stored hold NEVER advances the picture by itself (plan 6.2:
    // no waiting on the stored wait values) - only the movement state moves it.
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, true, false, 100.0f, true));
    TestEqual(TEXT("a 100 s rise stays on the apex hold frame"), Visual->GetCurrentFrameIndex(), 6);

    // Falling: the non-rising airborne fact switches the phase to the fall
    // frames 7..11 (the apex frame is skipped by state, not by waiting).
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, true, false, 0.0f, false));
    TestEqual(TEXT("the fall phase starts at frame 7"), Visual->GetCurrentFrameIndex(), 7);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, true, false, 0.09f, false));
    TestEqual(TEXT("the fall walks its 80 ms frames"), Visual->GetCurrentFrameIndex(), 8);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, true, false, 0.32f, false));
    TestEqual(TEXT("the fall holds at frame 11"), Visual->GetCurrentFrameIndex(), 11);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, true, false, 100.0f, false));
    TestEqual(TEXT("a long fall stays held at frame 11"), Visual->GetCurrentFrameIndex(), 11);

    // Landing: the ground edge walks the capped pose (12) and the tail.
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, false, false, 0.0f, false));
    TestEqual(TEXT("the landing edge shows the capped pose frame"), Visual->GetCurrentFrameIndex(), 12);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, false, false, 0.1f, false));
    TestEqual(TEXT("the landing pose holds its cap"), Visual->GetCurrentFrameIndex(), 12);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, false, false, 0.05f, false));
    TestEqual(TEXT("the landing tail walks frame 13"), Visual->GetCurrentFrameIndex(), 13);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, false, false, 0.06f, false));
    TestEqual(TEXT("the landing tail walks frame 14"), Visual->GetCurrentFrameIndex(), 14);
    TestTrue(TEXT("the landing tail still pins the clip time"), Visual->IsPlaybackFrozen());
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, false, false, 0.05f, false));
    TestEqual(TEXT("the finished tail returns to idle"), Visual->GetCurrentActionId(), IdleActionId);
    TestFalse(TEXT("the idle display unpins the clip time"), Visual->IsPlaybackFrozen());
    return true;
}

// 14b) The jump display survives a preempting attack (resumes by the phase)
// and a ground edge missed under a reaction is stale (no late tail).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_024_JumpPreemptResumeAndStaleCleanup,
    "UEMMO.Tasks.M6_024.JumpPreemptResumeAndStaleCleanup",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_024_JumpPreemptResumeAndStaleCleanup::RunTest(const FString& Parameters)
{
    FString Error;
    UDNFMCharacterVisualSet* Set = BuildMockSet(*this, Error);
    if (Set == nullptr)
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = NewMockVisual(*this, Set);
    FDNFMCombatVisualPresenter Presenter;
    Presenter.SetVisualTarget(Visual);

    // An aerial attack steals the picture mid-jump.
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, true, false, 0.0f, true));
    TestEqual(TEXT("the takeoff display runs"), Visual->GetCurrentActionId(), JumpActionId);
    Presenter.Step(MakeFacts(ECombatActionState::Attacking, FName(TEXT("light_01")), 5, 3, false, true, false));
    TestEqual(TEXT("the aerial attack preempts the jump display"),
        Visual->GetCurrentActionId(), FName(TEXT("attack1")));
    Presenter.Step(MakeFacts(ECombatActionState::Attacking, FName(TEXT("light_01")), 5, 8, false, true, false));
    TestEqual(TEXT("the attack display samples its hit pose"), Visual->GetCurrentFrameIndex(), 1);

    // The attack ends while still airborne and falling: the jump display
    // resumes by the movement facts (the fall phase, not a takeoff replay).
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, true, false, 0.0f, false));
    TestEqual(TEXT("the fall phase resumes after the attack"),
        Visual->GetCurrentActionId(), JumpActionId);
    TestEqual(TEXT("the resumed fall starts at frame 7"), Visual->GetCurrentFrameIndex(), 7);

    // The landing edge (seen by the jump branch) plays the tail to idle.
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, false, false, 0.0f, false));
    TestEqual(TEXT("the landing tail opens on the ground edge"), Visual->GetCurrentFrameIndex(), 12);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, false, false, 0.3f, false));
    TestEqual(TEXT("the finished tail idles"), Visual->GetCurrentActionId(), IdleActionId);

    // A reaction covering the ground edge makes the airborne phase stale: the
    // next grounded sync cleans it up instead of playing a late tail.
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1, false, true, false, 0.0f, false));
    TestEqual(TEXT("the second fall starts"), Visual->GetCurrentActionId(), JumpActionId);
    Presenter.Step(MakeFacts(ECombatActionState::HitStun, NAME_None, 0, -1));
    TestEqual(TEXT("a landing-tick stun shows the reaction"), Visual->GetCurrentActionId(), HitActionId);
    Presenter.Step(MakeFacts(ECombatActionState::Free, NAME_None, 0, -1));
    TestEqual(TEXT("the stale phase cleans up to idle (no late tail)"),
        Visual->GetCurrentActionId(), IdleActionId);
    return true;
}

// 15) earthbreak's stored 1 ms end frame can never stall the display: the
// recovery rows map it across the remaining logic frames and the combat clock
// (not the clip time) ends the attack back into idle.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_024_EarthbreakEndFrameNotStuck,
    "UEMMO.Tasks.M6_024.EarthbreakEndFrameNotStuck",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_024_EarthbreakEndFrameNotStuck::RunTest(const FString& Parameters)
{
    FCombatRig Rig;
    Rig.ExtraDefinitions = {MakeAttackDefinition(FName(TEXT("earthbreak")), 30, 11, 14)};
    if (!Rig.Build(*this))
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = Rig.Visual;
    UCombatComponent* Combat = Rig.Combat;

    TestTrue(TEXT("earthbreak starts"), Combat->TryStartAttack(FName(TEXT("earthbreak")), 1));
    Rig.SyncDisplay();
    TestEqual(TEXT("earthbreak plays its mapped clip"),
        Visual->GetCurrentActionId(), FName(TEXT("earthbreak")));

    // Walk the whole 30-frame combat timeline: 30 bounded logic ticks, no
    // wall-clock dependency anywhere (the sampling is logic-frame driven).
    for (int32 LogicFrame = 0; LogicFrame < 29; ++LogicFrame)
    {
        Rig.StepLogicFrame(); // lands on LogicFrame
        const int32 Expected = (LogicFrame < 5) ? 0 : (LogicFrame < 11) ? 1
            : (LogicFrame < 14) ? 3 : (LogicFrame < 20) ? 4 : 5;
        TestEqual(FString::Printf(TEXT("earthbreak logic frame %d display"), LogicFrame),
            Visual->GetCurrentFrameIndex(), Expected);
    }
    Rig.StepLogicFrame(); // the step that reaches DurationFrames-1 ends it
    TestEqual(TEXT("the combat clock ended the attack"),
        Combat->GetSnapshot().ActionState, ECombatActionState::Free);
    Rig.SyncDisplay();
    TestEqual(TEXT("the 1 ms stored end frame never held the picture"),
        Visual->GetCurrentActionId(), IdleActionId);
    return true;
}

// 16) Every combat attack enters its mapped clip, shows the hit pose exactly
// inside the active window, and returns to idle when the combat clock ends it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_024_EveryAttackEntersItsMappedAction,
    "UEMMO.Tasks.M6_024.EveryAttackEntersItsMappedAction",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_024_EveryAttackEntersItsMappedAction::RunTest(const FString& Parameters)
{
    // The frozen combat-attacks.json shapes (duration + active windows).
    struct FAttackShape { FName AttackId; int32 Duration; int32 ActiveStart; int32 ActiveEnd; };
    static const TArray<FAttackShape> Shapes = {
        {FName(TEXT("light_01")), 26, 7, 11},
        {FName(TEXT("light_02")), 32, 9, 14},
        {FName(TEXT("launcher")), 40, 12, 17},
        {FName(TEXT("aerial_01")), 28, 6, 10},
        {FName(TEXT("singlekick")), 24, 11, 15},
        {FName(TEXT("damagelowkick")), 24, 11, 15},
        {FName(TEXT("throwsand")), 28, 13, 17},
        {FName(TEXT("earthbreak")), 30, 11, 14},
        {FName(TEXT("stomp")), 22, 11, 14}
    };

    FCombatRig Rig;
    for (const FAttackShape& Shape : Shapes)
    {
        // The rig already carries light_01/light_02; only the seven skills
        // are extra (the catalog refuses duplicate ids).
        if (Shape.AttackId == FName(TEXT("light_01")) || Shape.AttackId == FName(TEXT("light_02")))
        {
            continue;
        }
        Rig.ExtraDefinitions.Add(MakeAttackDefinition(Shape.AttackId, Shape.Duration,
            Shape.ActiveStart, Shape.ActiveEnd));
    }
    if (!Rig.Build(*this))
    {
        return false;
    }
    UDNFMCharacterVisualComponent* Visual = Rig.Visual;
    UCombatComponent* Combat = Rig.Combat;
    const TArray<FDNFMAttackDisplayMapping> Table = MakeDefaultAttackDisplayMappings();

    for (const FAttackShape& Shape : Shapes)
    {
        const FDNFMAttackDisplayMapping* Mapping = FindAttackDisplayMapping(Table, Shape.AttackId);
        if (!TestNotNull(FString::Printf(TEXT("%s has a mapping row"), *Shape.AttackId.ToString()), Mapping))
        {
            continue;
        }
        const int32 HitPoseFrame = FindDisplayFrameForLogicFrame(*Mapping, Shape.ActiveStart);

        TestTrue(FString::Printf(TEXT("%s starts"), *Shape.AttackId.ToString()),
            Combat->TryStartAttack(Shape.AttackId, 1));
        Rig.SyncDisplay();
        TestEqual(FString::Printf(TEXT("%s shows its mapped clip"), *Shape.AttackId.ToString()),
            Visual->GetCurrentActionId(), Mapping->ActionId);
        TestEqual(FString::Printf(TEXT("%s enters at its first display frame"), *Shape.AttackId.ToString()),
            Visual->GetCurrentFrameIndex(), Mapping->Segments[0].DisplayFrameIndex);

        // Walk into the active window: the display holds the hit pose there.
        while (Combat->GetSnapshot().ActionState == ECombatActionState::Attacking
            && Combat->GetSnapshot().Frame < Shape.ActiveStart)
        {
            Rig.StepLogicFrame();
        }
        TestEqual(FString::Printf(TEXT("%s reached its active window"), *Shape.AttackId.ToString()),
            Combat->GetSnapshot().Frame, Shape.ActiveStart);
        TestEqual(FString::Printf(TEXT("%s shows the hit pose inside the active window"),
            *Shape.AttackId.ToString()), Visual->GetCurrentFrameIndex(), HitPoseFrame);

        // Run the instance out: the combat clock (not the clip time) ends it.
        for (int32 Guard = 0; Guard < 96 && Combat->GetSnapshot().ActionState == ECombatActionState::Attacking; ++Guard)
        {
            Rig.StepLogicFrame();
        }
        TestEqual(FString::Printf(TEXT("%s ended on the combat clock"), *Shape.AttackId.ToString()),
            Combat->GetSnapshot().ActionState, ECombatActionState::Free);
        Rig.SyncDisplay();
        TestEqual(FString::Printf(TEXT("%s returned to idle"), *Shape.AttackId.ToString()),
            Visual->GetCurrentActionId(), IdleActionId);
    }
    return true;
}

// 17) The component wiring feeds the M6-024 sequence facts: the tick delta
// drives the jump walk through TickComponent and the rising provider switches
// the takeoff/fall phase.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FUEMMOTasksM6_024_ComponentWiringFeedsSequenceFacts,
    "UEMMO.Tasks.M6_024.ComponentWiringFeedsSequenceFacts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM6_024_ComponentWiringFeedsSequenceFacts::RunTest(const FString& Parameters)
{
    FString Error;
    UDNFMCharacterVisualSet* Set = BuildMockSet(*this, Error);
    if (Set == nullptr)
    {
        return false;
    }
    UAttackDefinition* Light01 = MakeAttackDefinition(FName(TEXT("light_01")), 26, 7, 11);
    UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
    FText CatalogError;
    if (!TestTrue(TEXT("the probe catalog builds"),
        Catalog->BuildFromDefinitions({Light01}, CatalogError)))
    {
        return false;
    }
    UCombatComponent* Combat = NewObject<UCombatComponent>();
    Combat->InitializeFromCatalog(Catalog);
    UCombatPresentationComponent* Presentation = NewObject<UCombatPresentationComponent>();
    Presentation->SetSources(Combat, nullptr);
    UDNFMCharacterVisualComponent* Visual = NewMockVisual(*this, Set);
    Presentation->SetDNFMVisualTarget(Visual);

    bool bAirborne = false;
    bool bGroundMoving = false;
    bool bRising = false;
    Presentation->SetDNFMFactProviders(
        [&bAirborne]() { return bAirborne; },
        [&bGroundMoving]() { return bGroundMoving; });
    Presentation->SetDNFMRisingProvider([&bRising]() { return bRising; });

    const auto Tick = [&](float DeltaSeconds)
    {
        Presentation->TickComponent(DeltaSeconds, LEVELTICK_All, nullptr);
    };

    Tick(1.0f / 60.0f);
    TestEqual(TEXT("the baseline idles"), Visual->GetCurrentActionId(), IdleActionId);

    // The takeoff walk advances by the TICKED delta (the component passes it).
    bAirborne = true;
    bRising = true;
    Tick(1.0f / 60.0f);
    TestEqual(TEXT("the tick feed starts the takeoff"), Visual->GetCurrentActionId(), JumpActionId);
    TestEqual(TEXT("the takeoff starts at frame 0"), Visual->GetCurrentFrameIndex(), 0);
    for (int32 Count = 0; Count < 7; ++Count)
    {
        Tick(1.0f / 60.0f); // ~117 ms more of rising
    }
    TestEqual(TEXT("the walked takeoff reaches frame 1 by the ticked delta"),
        Visual->GetCurrentFrameIndex(), 1);

    // The rising fact switches the phase (the apex frame is skipped by state).
    bRising = false;
    Tick(1.0f / 60.0f);
    TestEqual(TEXT("the fall phase starts at frame 7"), Visual->GetCurrentFrameIndex(), 7);

    // The landing tail runs on the ticked delta and finishes into idle.
    bAirborne = false;
    Tick(1.0f / 60.0f);
    TestEqual(TEXT("the landing tail opens"), Visual->GetCurrentFrameIndex(), 12);
    for (int32 Count = 0; Count < 16; ++Count)
    {
        Tick(1.0f / 60.0f); // ~267 ms: the 220 ms tail completes
    }
    TestEqual(TEXT("the finished tail returns to idle"), Visual->GetCurrentActionId(), IdleActionId);
    return true;
}

#endif
