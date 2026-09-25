#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include <limits>

#include "../Combat/HealthComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_015
{
	static constexpr float FullHealth = 100.0f;

	static UHealthComponent* MakeHealthComponent()
	{
		return NewObject<UHealthComponent>();
	}
}

using namespace UE::UEMMO::Tasks::M1_015;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_015DamageAppliesAndReturnsActual,
	"UEMMO.Tasks.M1_015.DamageAppliesAndReturnsActual",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_015DamageAppliesAndReturnsActual::RunTest(const FString& Parameters)
{
	UHealthComponent* Health = MakeHealthComponent();
	int32 DeathCount = 0;
	Health->OnDied.AddLambda([&DeathCount]() { ++DeathCount; });

	const float Applied = Health->ApplyDamage(10.0f);
	TestEqual(TEXT("ApplyDamage(10) on full HP returns 10"), Applied, 10.0f);
	TestEqual(TEXT("HP after taking 10 from 100 is 90"), Health->GetHealth(), 90.0f);
	TestTrue(TEXT("component is still alive after non-lethal damage"), Health->IsAlive());
	TestEqual(TEXT("non-lethal damage never broadcasts OnDied"), DeathCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_015LethalDamageClampsAndDiesOnce,
	"UEMMO.Tasks.M1_015.LethalDamageClampsAndDiesOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_015LethalDamageClampsAndDiesOnce::RunTest(const FString& Parameters)
{
	UHealthComponent* Health = MakeHealthComponent();
	int32 DeathCount = 0;
	Health->OnDied.AddLambda([&DeathCount]() { ++DeathCount; });

	// Acceptance scenario: HP 100, take 10 (HP 90), then an overkill hit of 200.
	TestEqual(TEXT("first hit of 10 applies 10"), Health->ApplyDamage(10.0f), 10.0f);
	const float Applied = Health->ApplyDamage(200.0f);
	TestEqual(TEXT("overkill ApplyDamage(200) with 90 HP remaining applies only 90"), Applied, 90.0f);
	TestEqual(TEXT("HP clamps to 0 after lethal damage"), Health->GetHealth(), 0.0f);
	TestFalse(TEXT("component is dead after lethal damage"), Health->IsAlive());
	TestEqual(TEXT("OnDied is broadcast exactly once for the first death"), DeathCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_015DeadRefusesAllFurtherDamage,
	"UEMMO.Tasks.M1_015.DeadRefusesAllFurtherDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_015DeadRefusesAllFurtherDamage::RunTest(const FString& Parameters)
{
	UHealthComponent* Health = MakeHealthComponent();
	int32 DeathCount = 0;
	Health->OnDied.AddLambda([&DeathCount]() { ++DeathCount; });

	Health->ApplyDamage(100.0f);
	TestEqual(TEXT("precondition: first lethal hit broadcast once"), DeathCount, 1);

	const float PositiveAfterDeath = Health->ApplyDamage(30.0f);
	const float NegativeAfterDeath = Health->ApplyDamage(-30.0f);
	const float NanAfterDeath = Health->ApplyDamage(std::numeric_limits<float>::quiet_NaN());
	TestEqual(TEXT("positive damage on a dead component returns 0"), PositiveAfterDeath, 0.0f);
	TestEqual(TEXT("negative damage on a dead component returns 0"), NegativeAfterDeath, 0.0f);
	TestEqual(TEXT("NaN damage on a dead component returns 0"), NanAfterDeath, 0.0f);
	TestEqual(TEXT("HP stays 0 after refused damage"), Health->GetHealth(), 0.0f);
	TestEqual(TEXT("a dead component never broadcasts OnDied again"), DeathCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_015RejectsNegativeAndNonFiniteDamage,
	"UEMMO.Tasks.M1_015.RejectsNegativeAndNonFiniteDamage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_015RejectsNegativeAndNonFiniteDamage::RunTest(const FString& Parameters)
{
	UHealthComponent* Health = MakeHealthComponent();
	int32 DeathCount = 0;
	Health->OnDied.AddLambda([&DeathCount]() { ++DeathCount; });

	const float Negative = Health->ApplyDamage(-10.0f);
	const float NegativeZero = Health->ApplyDamage(-0.0f);
	const float Nan = Health->ApplyDamage(std::numeric_limits<float>::quiet_NaN());
	const float PositiveInfinity = Health->ApplyDamage(std::numeric_limits<float>::infinity());
	const float NegativeInfinity = Health->ApplyDamage(-std::numeric_limits<float>::infinity());
	TestEqual(TEXT("negative damage is rejected with 0"), Negative, 0.0f);
	TestEqual(TEXT("negative zero damage is rejected with 0"), NegativeZero, 0.0f);
	TestEqual(TEXT("NaN damage is rejected with 0"), Nan, 0.0f);
	TestEqual(TEXT("+Inf damage is rejected with 0"), PositiveInfinity, 0.0f);
	TestEqual(TEXT("-Inf damage is rejected with 0"), NegativeInfinity, 0.0f);
	TestEqual(TEXT("HP is unchanged after rejected damage"), Health->GetHealth(), FullHealth);
	TestTrue(TEXT("component stays alive after rejected damage"), Health->IsAlive());
	TestEqual(TEXT("rejected damage never broadcasts OnDied"), DeathCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_015ZeroDamageCausesNoEvents,
	"UEMMO.Tasks.M1_015.ZeroDamageCausesNoEvents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_015ZeroDamageCausesNoEvents::RunTest(const FString& Parameters)
{
	UHealthComponent* Health = MakeHealthComponent();
	int32 DeathCount = 0;
	Health->OnDied.AddLambda([&DeathCount]() { ++DeathCount; });

	const float Applied = Health->ApplyDamage(0.0f);
	TestEqual(TEXT("zero damage returns 0"), Applied, 0.0f);
	TestEqual(TEXT("HP is unchanged after zero damage"), Health->GetHealth(), FullHealth);
	TestTrue(TEXT("component stays alive after zero damage"), Health->IsAlive());
	TestEqual(TEXT("zero damage fakes no OnDied broadcast"), DeathCount, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_015ResetOpensNewDeathLifecycle,
	"UEMMO.Tasks.M1_015.ResetOpensNewDeathLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_015ResetOpensNewDeathLifecycle::RunTest(const FString& Parameters)
{
	UHealthComponent* Health = MakeHealthComponent();
	int32 DeathCount = 0;
	Health->OnDied.AddLambda([&DeathCount]() { ++DeathCount; });

	Health->ApplyDamage(150.0f);
	TestEqual(TEXT("first lifecycle ends with exactly one broadcast"), DeathCount, 1);

	Health->ResetHealth();
	TestTrue(TEXT("ResetHealth restores Alive"), Health->IsAlive());
	TestEqual(TEXT("ResetHealth restores HP to MaxHP"), Health->GetHealth(), Health->GetMaxHealth());

	const float AppliedAfterReset = Health->ApplyDamage(100.0f);
	TestEqual(TEXT("second lifecycle: ApplyDamage(100) on full HP returns 100"), AppliedAfterReset, 100.0f);
	TestEqual(TEXT("second lifecycle: HP reaches 0 again"), Health->GetHealth(), 0.0f);
	TestFalse(TEXT("second lifecycle: component is dead again"), Health->IsAlive());
	TestEqual(TEXT("each lifecycle broadcasts OnDied at most once: total count is now 2"), DeathCount, 2);

	TestEqual(TEXT("damage after the second death returns 0"), Health->ApplyDamage(50.0f), 0.0f);
	TestEqual(TEXT("the counter stays at 2 after refused damage"), DeathCount, 2);
	return true;
}

#endif
