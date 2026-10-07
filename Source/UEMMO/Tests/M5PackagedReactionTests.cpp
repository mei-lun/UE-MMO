// M5-018B: the packaged Segment-A reaction evidence. These tests run inside
// the packaged executable (Development builds ship the automation code) and
// pin what the PACKAGE guarantees:
// - the reward source fallback holds without the dev Data/ directory (the
//   loader refuses, the mirrored starter table still rolls a real reward);
// - the reaction chain flows the real entry against a real spawned enemy
//   (the cooked DA_ definitions, the M5-012 unified entry, the victim's own
//   health);
// - the reaction presentation dispatch resolves the COOKED RCT_Hit montage
//   (the M5-017 mapping works against cooked assets, not just the editor).
// The fallback test is packaged-environment-conditional: in a development run
// (the dev Data/ directory present) it degrades to an informational skip, so
// the full editor suite stays green while the packaged run carries the proof.

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatGeometry.h"
#include "../Combat/CombatPresentationComponent.h"
#include "../Tests/CombatPresentationTestDoubles.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/TestRoomConfigDriver.h"
#include "../Enemy/MeleeEnemy.h"
#include "../Items/DropGenerator.h"
#include "../Items/DropTable.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../PrototypeCharacter.h"

#include "Components/BoxComponent.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_018B
{
	static bool M5_018B_DevDataDirectoryPresent()
	{
		return FPaths::DirectoryExists(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Data")));
	}

}

using namespace UE::UEMMO::Tasks::M5_018B;

// The M5-017 recording presenter double (UCLASS; see CombatPresentationTestDoubles.h).
using UM5_018B_RecordingPresenter = URecordingReactionPresentation;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_018BPackagedReactionEvidence,
	"UEMMO.Tasks.M5_018B.PackagedReactionEvidence",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_018BPackagedReactionEvidence::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	if (!TestTrue(TEXT("a world is available"), World != nullptr))
	{
		return true;
	}
	const bool bDevDataPresent = M5_018B_DevDataDirectoryPresent();
	AddInfo(FString::Printf(TEXT("the run carries the dev Data/ directory: %s"),
		bDevDataPresent ? TEXT("true (development run - the fallback assertion degrades to a skip)") : TEXT("false (packaged run - the fallback assertion carries the proof)")));

	// 1. The reward source fallback (packaged-only): the loader refuses, the
	//    mirrored starter table still rolls a real reward through a local
	//    catalog - the reward chain never dies with the dev sources gone.
	{
		FTestRoomDropPool Pool;
		FString PoolError;
		const bool bLoaded = ACombatTestRoomDriver::LoadProductionDropPool(Pool, PoolError);
		if (bDevDataPresent)
		{
			AddInfo(TEXT("the drops source load succeeded in development; the packaged refusal is asserted in the package"));
		}
		else
		{
			TestFalse("the packaged run refuses the drops source (no Data directory)", bLoaded);
			FDropTable Table = MakeStarterDropTable();
			FItemDefinitionCatalog Catalog;
			FItemDefinition Sword;
			Sword.DefinitionId = FName(TEXT("weapon_training"));
			Sword.DisplayName = TEXT("Training Sword");
			Sword.Slot = EItemSlot::Weapon;
			Sword.BaseStats.Attack = 5.0f;
			Sword.Rarity = EItemRarity::Normal;
			TestTrue("the fallback catalog accepts the mirrored sword", Catalog.AddDefinition(Sword));
			FItemDefinition Armor;
			Armor.DefinitionId = FName(TEXT("armor_training"));
			Armor.DisplayName = TEXT("Training Armor");
			Armor.Slot = EItemSlot::Armor;
			Armor.BaseStats.Defense = 3.0f;
			Armor.Rarity = EItemRarity::Normal;
			TestTrue("the fallback catalog accepts the mirrored armor", Catalog.AddDefinition(Armor));
			FItemDefinition Charm;
			Charm.DefinitionId = FName(TEXT("charm_training"));
			Charm.DisplayName = TEXT("Training Charm");
			Charm.Slot = EItemSlot::Accessory;
			Charm.BaseStats.MaxHP = 20.0f;
			Charm.Rarity = EItemRarity::Normal;
			TestTrue("the fallback catalog accepts the mirrored charm", Catalog.AddDefinition(Charm));
			const FDropRewardResult Drop = FDropGenerator::GenerateReward(
				/*RewardSeed*/ 9001, /*SettlementId*/ 1, Table, Catalog);
			TestTrue("the fallback table still rolls a real reward", Drop.bSuccess);
			TestEqual("the fallback reward is a mirrored starter id",
				Drop.Instance.DefinitionId == FName(TEXT("weapon_training"))
				|| Drop.Instance.DefinitionId == FName(TEXT("armor_training"))
				|| Drop.Instance.DefinitionId == FName(TEXT("charm_training")), true);
		}
	}

	// 2. The real reaction chain against a real spawned enemy (the cooked
	//    definition, the unified entry, the victim's own health).
	{
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
		const FVector Base = FVector(96000.0, 96000.0, 100.0);
		APrototypeCharacter* Host = World->SpawnActor<APrototypeCharacter>(
			APrototypeCharacter::StaticClass(), Base, FRotator::ZeroRotator, Params);
		if (!TestNotNull(TEXT("the packaged host spawns"), Host))
		{
			return true;
		}
		if (!Host->HasActorBegunPlay())
		{
			Host->DispatchBeginPlay();
		}
		UCombatComponent* HostCombat = Host->GetCombat();
		UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
		FText CatalogError;
		if (!TestTrue(TEXT("the host attaches the shipped attack catalog"),
			HostCombat != nullptr && Catalog->InitializeFromConfig(CatalogError)
			&& HostCombat->InitializeFromCatalog(Catalog)))
		{
			return true;
		}
		HostCombat->SetCombatStats(0.0f, HostCombat->GetDefense());

		AMeleeEnemy* Enemy = World->SpawnActor<AMeleeEnemy>(
			AMeleeEnemy::StaticClass(), Base + FVector(95.0, 0.0, 2.0), FRotator::ZeroRotator, Params);
		if (!TestNotNull(TEXT("the packaged target spawns (the cooked definition)"), Enemy))
		{
			return true;
		}
		if (!Enemy->HasActorBegunPlay())
		{
			Enemy->DispatchBeginPlay();
		}

		if (!TestTrue(TEXT("the real light_01 starts"),
			HostCombat->TryStartAttack(FName(TEXT("light_01")), 1)))
		{
			return true;
		}
		const UAttackDefinition* Definition = Catalog->Find(FName(TEXT("light_01")));
		const int32 Frames = Definition ? Definition->ActiveWindow.StartFrame + 1 : 8;
		for (int32 Index = 0; Index < Frames; ++Index)
		{
			HostCombat->TickCombat(1.0f / 60.0f);
			if (UCombatComponent* VictimCombat = Enemy->GetCombatComponent())
			{
				VictimCombat->TickCombat(1.0f / 60.0f);
			}
		}
		TestEqual("the real hit removed exactly the legacy 10", Enemy->GetHealthComponent()->GetHealth(), 90.0f);
		TestTrue("the real hit stunned the victim through the unified entry",
			Enemy->GetCombatComponent()->GetSnapshot().ActionState == ECombatActionState::HitStun);
		Enemy->Destroy();
		Host->Destroy();
	}

	// 3. The cooked reaction presentation: the dispatch resolves and plays
	//    the cooked RCT_Hit montage (the M5-017 mapping against cooked assets).
	{
		FActorSpawnParameters Params;
		AActor* Victim = World->SpawnActor<AActor>(AActor::StaticClass(),
			FVector(97000.0, 96000.0, 100.0), FRotator::ZeroRotator, Params);
		if (!TestNotNull(TEXT("the presentation victim spawns"), Victim))
		{
			return true;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Victim, TEXT("M5_018B_Body"));
		Victim->SetRootComponent(Body);
		Body->SetBoxExtent(FVector(20.0f, 20.0f, 95.0f));
		Body->RegisterComponent();
		UHealthComponent* Health = NewObject<UHealthComponent>(Victim, TEXT("M5_018B_Health"));
		Health->RegisterComponent();
		UCombatComponent* VictimCombat = NewObject<UCombatComponent>(Victim, TEXT("M5_018B_VictimCombat"));
		VictimCombat->RegisterComponent();
		UM5_018B_RecordingPresenter* Presenter = NewObject<UM5_018B_RecordingPresenter>(Victim, TEXT("M5_018B_Presenter"));
		Presenter->SetSources(VictimCombat, nullptr);

		FCombatHit Hit;
		Hit.Target = Victim;
		Hit.Damage = 10.0f;
		Hit.StunSeconds = 0.9f;
		VictimCombat->NotifyHitReceived(Hit);
		Presenter->PollVictimReactions();
		TestTrue("the packaged reaction dispatched the hit row",
			Presenter->GetReactionDispatchHistory().Contains(FName(TEXT("react_hit"))));
		TestTrue("the cooked RCT_Hit montage resolved and played",
			Presenter->PlayedReactions.Num() == 1
			&& Presenter->PlayedReactionMontageNames.Num() == 1
			&& Presenter->PlayedReactionMontageNames[0].Contains(TEXT("RCT_Hit")));
		Victim->Destroy();
	}
	return true;
}

#endif
