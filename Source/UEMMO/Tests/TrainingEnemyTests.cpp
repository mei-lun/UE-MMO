#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/HealthComponent.h"
#include "../Enemy/TrainingEnemy.h"

#include "GameFramework/CharacterMovementComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_016
{
	// NewObject keeps the tests hermetic: no level load, no BeginPlay, no
	// world. Default subobjects (capsule, movement, health) still exist, so
	// composition and reset semantics are exercised exactly as shipped.
	static ATrainingEnemy* MakeTrainingEnemy()
	{
		return NewObject<ATrainingEnemy>();
	}
}

using namespace UE::UEMMO::Tasks::M1_016;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_016TrainingEnemyHasHealthComponent,
	"UEMMO.Tasks.M1_016.TrainingEnemyHasHealthComponent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_016TrainingEnemyHasHealthComponent::RunTest(const FString& Parameters)
{
	ATrainingEnemy* Enemy = MakeTrainingEnemy();
	UHealthComponent* Health = Enemy->GetHealthComponent();
	TestNotNull(TEXT("training enemy owns a UHealthComponent by default"), Health);
	if (Health)
	{
		TestEqual(TEXT("default MaxHP is 100"), Health->GetMaxHealth(), 100.0f);
		TestEqual(TEXT("default HP starts full"), Health->GetHealth(), 100.0f);
		TestTrue(TEXT("training enemy starts alive"), Health->IsAlive());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_016ApplyDamageChangesHPAndResetRestores,
	"UEMMO.Tasks.M1_016.ApplyDamageChangesHPAndResetRestores",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_016ApplyDamageChangesHPAndResetRestores::RunTest(const FString& Parameters)
{
	ATrainingEnemy* Enemy = MakeTrainingEnemy();
	UHealthComponent* Health = Enemy->GetHealthComponent();
	TestNotNull(TEXT("precondition: the enemy owns its health component"), Health);
	if (!Health)
	{
		return true;
	}

	// The damage entry point is HealthComponent::ApplyDamage; the enemy adds
	// no second path to subtract health.
	const float Applied = Health->ApplyDamage(10.0f);
	TestEqual(TEXT("ApplyDamage(10) on full HP applies 10"), Applied, 10.0f);
	TestEqual(TEXT("HP is 90 after taking 10"), Health->GetHealth(), 90.0f);
	TestTrue(TEXT("enemy survives a non-lethal hit"), Health->IsAlive());

	Enemy->ResetEnemy();
	TestEqual(TEXT("ResetEnemy restores HP to MaxHP"), Health->GetHealth(), Health->GetMaxHealth());
	TestTrue(TEXT("ResetEnemy keeps the enemy alive"), Health->IsAlive());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_016ResetEnemyRestoresTransformAndVelocity,
	"UEMMO.Tasks.M1_016.ResetEnemyRestoresTransformAndVelocity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_016ResetEnemyRestoresTransformAndVelocity::RunTest(const FString& Parameters)
{
	ATrainingEnemy* Enemy = MakeTrainingEnemy();
	const FVector AnchorLocation(100.0f, 200.0f, 300.0f);
	const FRotator AnchorRotation(0.0f, 180.0f, 0.0f);
	Enemy->SetSpawnAnchor(AnchorLocation, AnchorRotation);

	// Move the enemy away from the anchor first, then Reset and compare.
	const FVector MovedLocation(1234.0f, 567.0f, 890.0f);
	Enemy->SetActorLocation(MovedLocation);
	Enemy->SetActorRotation(FRotator(0.0f, 0.0f, 0.0f));
	Enemy->GetCharacterMovement()->Velocity = FVector(400.0f, 0.0f, 250.0f);
	TestEqual(TEXT("precondition: the enemy actually moved away"), Enemy->GetActorLocation(), MovedLocation);

	Enemy->ResetEnemy();

	TestEqual(TEXT("ResetEnemy returns the enemy to the anchor location"), Enemy->GetActorLocation(), AnchorLocation);
	TestEqual(TEXT("ResetEnemy restores the anchor rotation"), Enemy->GetActorRotation(), AnchorRotation);
	TestTrue(TEXT("ResetEnemy zeroes the movement velocity"), Enemy->GetCharacterMovement()->Velocity.IsZero());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_016ResetEnemyOpensNewDeathLifecycle,
	"UEMMO.Tasks.M1_016.ResetEnemyOpensNewDeathLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_016ResetEnemyOpensNewDeathLifecycle::RunTest(const FString& Parameters)
{
	ATrainingEnemy* Enemy = MakeTrainingEnemy();
	UHealthComponent* Health = Enemy->GetHealthComponent();
	TestNotNull(TEXT("precondition: the enemy owns its health component"), Health);
	if (!Health)
	{
		return true;
	}
	int32 DeathCount = 0;
	Health->OnDied.AddLambda([&DeathCount]() { ++DeathCount; });

	TestEqual(TEXT("precondition: lethal hit applies only the remaining HP"), Health->ApplyDamage(150.0f), 100.0f);
	TestFalse(TEXT("precondition: the enemy is dead"), Health->IsAlive());
	TestEqual(TEXT("precondition: exactly one OnDied broadcast"), DeathCount, 1);

	Enemy->ResetEnemy();
	TestTrue(TEXT("ResetEnemy revives the enemy"), Health->IsAlive());

	// A reset enemy can die again: ResetHealth opened a new death lifecycle.
	TestEqual(TEXT("second lifecycle: lethal damage applies again"), Health->ApplyDamage(100.0f), 100.0f);
	TestFalse(TEXT("second lifecycle: the enemy is dead again"), Health->IsAlive());
	TestEqual(TEXT("each lifecycle broadcasts OnDied at most once (total 2)"), DeathCount, 2);
	return true;
}

#endif
