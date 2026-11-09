// M8-031 acceptance: the explicit combat phase state machine.
// The plan 08 problem-1 acceptance conditions covered here:
// - the transition table (the legal pairs pass, the illegal pairs refuse)
// - the phase derivation from the frame counters and the victim timers
// - the attack-start gate (the HitStun/knockdown/dead refuse; the Idle and
//   the cancel window allow)
#include "Misc/AutomationTest.h"
#include "../Combat/CombatStateMachine.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUEMMOM8_031StateMachine,
    "UEMMO.Acceptance.M8_031.StateMachine",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOM8_031StateMachine::RunTest(const FString& Parameters)
{
    using P = ECombatPhase;

    // ---- the transition table (the doc's legal/illegal pairs) ----
    TestTrue(TEXT("Idle -> Windup allowed"), UCombatStateMachine::TransitionAllowed(P::Idle, P::Windup));
    TestTrue(TEXT("Windup -> Active allowed"), UCombatStateMachine::TransitionAllowed(P::Windup, P::Active));
    TestTrue(TEXT("Active -> Recovery allowed"), UCombatStateMachine::TransitionAllowed(P::Active, P::Recovery));
    TestTrue(TEXT("CancelWindow -> Windup allowed (the chain)"),
        UCombatStateMachine::TransitionAllowed(P::CancelWindow, P::Windup));
    TestTrue(TEXT("HitStun -> Idle allowed"), UCombatStateMachine::TransitionAllowed(P::HitStun, P::Idle));
    TestTrue(TEXT("Knockdown -> GetUp allowed"), UCombatStateMachine::TransitionAllowed(P::Knockdown, P::GetUp));
    TestTrue(TEXT("Dead -> Idle allowed (the lifecycle reset)"),
        UCombatStateMachine::TransitionAllowed(P::Dead, P::Idle));
    TestFalse(TEXT("Windup -> Idle refused (the attack must run its windows)"),
        UCombatStateMachine::TransitionAllowed(P::Windup, P::Idle));
    TestFalse(TEXT("HitStun -> Windup refused (the stun rejects attacks)"),
        UCombatStateMachine::TransitionAllowed(P::HitStun, P::Windup));
    TestFalse(TEXT("Knockdown -> Windup refused"),
        UCombatStateMachine::TransitionAllowed(P::Knockdown, P::Windup));
    TestFalse(TEXT("Active -> Windup refused (no same-attack rewind)"),
        UCombatStateMachine::TransitionAllowed(P::Active, P::Windup));
    TestFalse(TEXT("GetUp -> Windup refused until the combatant is up"),
        UCombatStateMachine::TransitionAllowed(P::GetUp, P::Windup));

    // ---- the phase derivation from the combat facts ----
    UCombatStateMachine* SM = NewObject<UCombatStateMachine>();
    if (!TestNotNull(TEXT("state machine created"), SM)) return false;

    // An attack running through its windows: light_01-shaped
    // (duration 26, active 7..11, cancel 12..24).
    FCombatPhaseFacts Facts;
    Facts.ActiveStartFrame = 7;
    Facts.ActiveEndFrame = 11;
    Facts.CancelStartFrame = 12;
    Facts.DurationFrames = 26;

    Facts.Frame = 0;
    SM->UpdateFromFacts(Facts);
    TestEqual(TEXT("frame 0 is Windup"), SM->GetPhase(), P::Windup);
    Facts.Frame = 6;
    SM->UpdateFromFacts(Facts);
    TestEqual(TEXT("frame 6 (the active start -1) is Windup"), SM->GetPhase(), P::Windup);
    Facts.Frame = 7;
    SM->UpdateFromFacts(Facts);
    TestEqual(TEXT("active start frame is Active"), SM->GetPhase(), P::Active);
    Facts.Frame = 10;
    SM->UpdateFromFacts(Facts);
    TestEqual(TEXT("inside the active window is Active"), SM->GetPhase(), P::Active);
    Facts.Frame = 12;
    SM->UpdateFromFacts(Facts);
    TestEqual(TEXT("cancel start frame is CancelWindow"), SM->GetPhase(), P::CancelWindow);
    TestTrue(TEXT("the cancel window is open"), SM->IsCancelWindowOpen());
    Facts.Frame = 24;
    SM->UpdateFromFacts(Facts);
    TestEqual(TEXT("after the cancel end is Recovery"), SM->GetPhase(), P::Recovery);
    TestFalse(TEXT("the cancel window closed"), SM->IsCancelWindowOpen());

    // The no-attack frames are Idle.
    Facts.Frame = -1;
    SM->UpdateFromFacts(Facts);
    TestEqual(TEXT("no attack is Idle"), SM->GetPhase(), P::Idle);

    // The victim timers override any attack phase (the M8-007 rule).
    Facts.Frame = 8; // the middle of the active window
    Facts.bHitStun = true;
    SM->UpdateFromFacts(Facts);
    TestEqual(TEXT("the hit stun interrupts the active window"), SM->GetPhase(), P::HitStun);
    TestFalse(TEXT("the stunned machine refuses the attack start"),
        SM->CanStartAttack());
    Facts.bHitStun = false;
    Facts.bKnockdown = true;
    SM->UpdateFromFacts(Facts);
    TestEqual(TEXT("the knockdown overrides"), SM->GetPhase(), P::Knockdown);
    TestFalse(TEXT("the knocked-down machine refuses the attack start"),
        SM->CanStartAttack());
    Facts.bKnockdown = false;
    Facts.bDead = true;
    SM->UpdateFromFacts(Facts);
    TestEqual(TEXT("death has absolute priority"), SM->GetPhase(), P::Dead);
    TestFalse(TEXT("the dead machine refuses the attack start"), SM->CanStartAttack());

    // The legal attack starts.
    Facts = FCombatPhaseFacts();
    SM->UpdateFromFacts(Facts);
    TestEqual(TEXT("the bare facts are Idle"), SM->GetPhase(), P::Idle);
    TestTrue(TEXT("Idle allows the attack start"), SM->CanStartAttack());
    Facts.Frame = 13; // inside the cancel window
    Facts.ActiveStartFrame = 7;
    Facts.ActiveEndFrame = 11;
    Facts.CancelStartFrame = 12;
    Facts.DurationFrames = 26;
    SM->UpdateFromFacts(Facts);
    TestTrue(TEXT("the cancel window allows the chain start"), SM->CanStartAttack());

    return true;
}
#endif
