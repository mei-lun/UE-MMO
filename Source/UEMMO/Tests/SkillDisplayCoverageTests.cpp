// M8-031 problem 5: the full-skill display segment table coverage check.
// Every legacy attack must have a display mapping that covers
// [0, DurationFrames) with no holes - the M6-023 plan section 6.2 contract.
#include "Misc/AutomationTest.h"
#include "../Combat/DNFMCombatVisualPresenter.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUEMMOM8_031DisplayCoverage,
    "UEMMO.Acceptance.M8_031.DisplayCoverage",
    EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOM8_031DisplayCoverage::RunTest(const FString& Parameters)
{
    using namespace UE::UEMMO::Tasks::M6_023;

    // The M6-023 presenter's default attack display mappings (the frozen
    // table the presenter uses in production).
    const TArray<FDNFMAttackDisplayMapping> Mappings = MakeDefaultAttackDisplayMappings();
    TestTrue(TEXT("display mappings exist"), Mappings.Num() > 0);

    // The M1 legacy four-attack set (combat-attacks.json). The durations are
    // the combat-attacks.json values (the frame tables).
    struct FKnownAttack { FName Id; int32 Duration; };
    const FKnownAttack KnownAttacks[] = {
        {FName(TEXT("light_01")), 26},
        {FName(TEXT("light_02")), 26},
        {FName(TEXT("launcher")), 25},
        {FName(TEXT("aerial_01")), 26},
    };

    for (const FKnownAttack& KnownAttack : KnownAttacks)
    {
        const FDNFMAttackDisplayMapping* Mapping =
            Mappings.FindByPredicate([&KnownAttack](const FDNFMAttackDisplayMapping& M)
            { return M.AttackId == KnownAttack.Id; });

        if (!TestNotNull(FString::Printf(TEXT("attack '%s' has a display mapping"),
            *KnownAttack.Id.ToString()), Mapping))
        {
            continue;
        }

        // The segment coverage: the rows must tile [0, Duration) with no holes.
        int32 Covered = 0;
        bool bHole = false;
        for (const FDNFMAttackDisplaySegment& Seg : Mapping->Segments)
        {
            if (Seg.LogicStartFrame != Covered)
            {
                AddError(FString::Printf(TEXT("attack '%s': gap at frame %d (row starts %d)"),
                    *KnownAttack.Id.ToString(), Covered, Seg.LogicStartFrame));
                bHole = true;
                break;
            }
            Covered = Seg.LogicEndFrame;
        }
        if (!bHole)
        {
            TestTrue(FString::Printf(TEXT("attack '%s': segments cover [0,%d) fully"),
                *KnownAttack.Id.ToString(), KnownAttack.Duration),
                Covered >= KnownAttack.Duration);
        }
    }
    return true;
}
#endif
