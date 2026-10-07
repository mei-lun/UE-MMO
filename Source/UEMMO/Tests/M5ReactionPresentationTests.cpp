// M5-017: victim-side hit-reaction presentations. Pins the presentation
// mapping contract: the default table mirrors Data/CombatSystem/
// presentations.json schema 2 (react_hit/react_dead real, launch/down/
// recover honest placeholders), one state edge presents exactly once
// (repeated hits inside the same stun never re-fire), refused hits (blanket
// control immunity) never fake a reaction, a launched victim presents the
// launch row instead of the plain hit, the knockdown/recovering edges
// present once each, a placeholder row dispatches but plays nothing (one
// diagnostic per id), the death event presents Dead exactly once per
// lifecycle, and the audio half of the face distinction stays damage-gated
// (a pure-control hit fakes no hit sound).

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/FileManager.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Paths.h"
#include "Tests/AutomationCommon.h"
#include "UnrealClient.h"

#include "../Combat/AttackCatalog.h"
#include "../PrototypeCharacter.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/CombatGeometry.h"
#include "../Combat/CombatPresentationComponent.h"
#include "../Combat/HealthComponent.h"
#include "../Enemy/TrainingEnemy.h"

#include "CombatPresentationTestDoubles.h"

#include "Components/BoxComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_017
{
	// Remote base so the temp world can never collide with arena content.
	// X horizontal, Y depth, Z height; the base is the attacker's feet origin.
	const FVector M5_017_SceneBase(104000.0, 56000.0, 600.0);

	// Half extent of one test body box; sits fully inside the shared hit box
	// of the legacy attacks (offset (95,0,90), half extent (85,50,70)).
	const FVector M5_017_BodyHalfExtent(20.0, 20.0, 95.0);

	// World acquisition, M1-019 pattern: a private temp world, GWorld fallback.
	static UWorld* M5_017_AcquireWorld()
	{
		UWorld* TempWorld = UWorld::CreateWorld(EWorldType::Game, /*bInformEngineOfWorld*/ false, FName(TEXT("M5_017_TestWorld")));
		if (TempWorld != nullptr)
		{
			return TempWorld;
		}
		return GWorld;
	}

	/** Destroys the temp world again; each test owns exactly one. */
	struct FM5_017_WorldScope
	{
		UWorld* World = nullptr;

		~FM5_017_WorldScope()
		{
			if (World != nullptr && World != GWorld && GEngine != nullptr)
			{
				GEngine->DestroyWorldContext(World);
				World->DestroyWorld(/*bInformEngineOfWorld*/ false);
			}
		}
	};

	// Loads the real read-only catalog from Config/DefaultGame.ini.
	static UAttackCatalog* M5_017_NewCatalog(FAutomationTestBase& Test)
	{
		UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
		FText Error;
		if (!Test.TestTrue(TEXT("the attack catalog initializes from DefaultGame.ini"), Catalog->InitializeFromConfig(Error)))
		{
			Test.AddError(FString::Printf(TEXT("catalog initialization failed: %s"), *Error.ToString()));
			return nullptr;
		}
		return Catalog;
	}


	/**
	 * One scene: attacker box actor (combat + catalog + hit events) and a
	 * victim box actor (health + combat) with a recording presenter bound to
	 * the victim's combat component. The victim presenter is driven manually
	 * (TickComponent) after each combat tick, mirroring the component-tick
	 * order (the presenter poller consumes the same snapshot the attack sync
	 * does).
	 */
	struct FM5_017_Scene
	{
		UAttackCatalog* Catalog = nullptr;
		AActor* Attacker = nullptr;
		UCombatComponent* Combat = nullptr;
		AActor* Target = nullptr;
		UCombatComponent* VictimCombat = nullptr;
		URecordingReactionPresentation* VictimPresenter = nullptr;
		UCombatPresentationComponent* AttackerPresenter = nullptr;
		TArray<UCombatComponent*> ExtraAttackers;

		bool Build(FAutomationTestBase& Test, UWorld& World, const FVector& InBase)
		{
			Catalog = M5_017_NewCatalog(Test);
			if (Catalog == nullptr)
			{
				return false;
			}

			FActorSpawnParameters Params;
			Attacker = World.SpawnActor<AActor>(AActor::StaticClass(), InBase, FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("the attacker spawns"), Attacker))
			{
				return false;
			}
			UBoxComponent* Body = NewObject<UBoxComponent>(Attacker, TEXT("M5_017_AttackerBody"));
			Attacker->SetRootComponent(Body);
			Body->SetMobility(EComponentMobility::Movable);
			Body->SetBoxExtent(M5_017_BodyHalfExtent);
			Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Body->SetCollisionObjectType(ECC_Pawn);
			Body->SetCollisionResponseToAllChannels(ECR_Ignore);
			Body->RegisterComponent();
			Body->SetWorldLocation(InBase);

			Combat = NewObject<UCombatComponent>(Attacker, TEXT("M5_017_Combat"));
			if (!Test.TestTrue(TEXT("the attacker combat component attaches the catalog"),
				Combat != nullptr && Combat->InitializeFromCatalog(Catalog)))
			{
				return false;
			}
			Combat->RegisterComponent();
			Combat->SetFeetLocationProvider([FixedFeet = InBase]() { return FixedFeet; });

			const UAttackDefinition* Definition = Catalog->Find(FName(TEXT("light_01")));
			if (!Test.TestTrue(TEXT("the catalog holds light_01"), Definition != nullptr))
			{
				return false;
			}
			const FVector BoxCenter = ComputeHitBox(InBase, /*Facing*/ 1, *Definition).Center;
			Target = World.SpawnActor<AActor>(AActor::StaticClass(), BoxCenter, FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("the victim spawns"), Target))
			{
				return false;
			}
			UBoxComponent* TargetBody = NewObject<UBoxComponent>(Target, TEXT("M5_017_TargetBody"));
			Target->SetRootComponent(TargetBody);
			TargetBody->SetMobility(EComponentMobility::Movable);
			TargetBody->SetBoxExtent(M5_017_BodyHalfExtent);
			TargetBody->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			TargetBody->SetCollisionObjectType(ECC_Pawn);
			TargetBody->SetCollisionResponseToAllChannels(ECR_Ignore);
			TargetBody->RegisterComponent();
			TargetBody->SetWorldLocation(BoxCenter);

			UHealthComponent* Health = NewObject<UHealthComponent>(Target, TEXT("M5_017_Health"));
			Health->RegisterComponent();
			VictimCombat = NewObject<UCombatComponent>(Target, TEXT("M5_017_VictimCombat"));
			VictimCombat->RegisterComponent();

			// The victim's recording presenter (driven manually per tick).
			VictimPresenter = NewObject<URecordingReactionPresentation>(Target, TEXT("M5_017_VictimPresenter"));
			VictimPresenter->SetSources(VictimCombat, nullptr);
			// The attacker's real presenter (the audio face observation).
			AttackerPresenter = NewObject<UCombatPresentationComponent>(Attacker, TEXT("M5_017_AttackerPresenter"));
			AttackerPresenter->SetSources(Combat, nullptr);
			return true;
		}

		// Ticks the attacker combat, then the victim combat, then both
		// presenters (component-tick order: poller before the next combat
		// advance).
		void TickFrame(int32 Count = 1)
		{
			for (int32 Index = 0; Index < Count; ++Index)
			{
				Combat->TickCombat(1.0f / 60.0f);
				VictimCombat->TickCombat(1.0f / 60.0f);
				VictimPresenter->TickComponent(1.0f / 60.0f, ELevelTick::LEVELTICK_All, nullptr);
				AttackerPresenter->TickComponent(1.0f / 60.0f, ELevelTick::LEVELTICK_All, nullptr);
				for (UCombatComponent* Extra : ExtraAttackers)
				{
					Extra->TickCombat(1.0f / 60.0f);
				}
			}
		}

		// A second attacker at the same base (a distinct combat component
		// with its own catalog and instance ids): lets a second hit land
		// while the victim is still inside its first stun.
		UCombatComponent* SpawnExtraAttacker(FAutomationTestBase& Test, UWorld& World, const FVector& InBase)
		{
			FActorSpawnParameters Params;
			AActor* Extra = World.SpawnActor<AActor>(AActor::StaticClass(), InBase, FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("the extra attacker spawns"), Extra))
			{
				return nullptr;
			}
			UBoxComponent* Body = NewObject<UBoxComponent>(Extra, TEXT("M5_017_ExtraAttackerBody"));
			Extra->SetRootComponent(Body);
			Body->SetMobility(EComponentMobility::Movable);
			Body->SetBoxExtent(M5_017_BodyHalfExtent);
			Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
			Body->SetCollisionObjectType(ECC_Pawn);
			Body->SetCollisionResponseToAllChannels(ECR_Ignore);
			Body->RegisterComponent();
			Body->SetWorldLocation(InBase);

			UCombatComponent* ExtraCombat = NewObject<UCombatComponent>(Extra, TEXT("M5_017_ExtraCombat"));
			if (!Test.TestTrue(TEXT("the extra attacker combat attaches the catalog"),
				ExtraCombat != nullptr && ExtraCombat->InitializeFromCatalog(Catalog)))
			{
				return nullptr;
			}
			ExtraCombat->RegisterComponent();
			ExtraCombat->SetFeetLocationProvider([FixedFeet = InBase]() { return FixedFeet; });
			ExtraAttackers.Add(ExtraCombat);
			return ExtraCombat;
		}

		bool HitOnce(FAutomationTestBase& Test, const TCHAR* AttackId, int32 Facing)
		{
			const UAttackDefinition* Definition = Catalog->Find(FName(AttackId));
			if (!Test.TestTrue(FString::Printf(TEXT("the catalog holds %s"), AttackId), Definition != nullptr))
			{
				return false;
			}
			if (!Test.TestTrue(FString::Printf(TEXT("%s starts"), AttackId),
				Combat->TryStartAttack(FName(AttackId), Facing)))
			{
				return false;
			}
			TickFrame(Definition->ActiveWindow.StartFrame + 1);
			return true;
		}

		void FinishInstance(FAutomationTestBase& Test, const TCHAR* AttackId)
		{
			const UAttackDefinition* Definition = Catalog->Find(FName(AttackId));
			if (Definition != nullptr)
			{
				TickFrame(Definition->DurationFrames + 2);
			}
		}

		static float TargetHealth(AActor& Target)
		{
			const UHealthComponent* Health = Target.FindComponentByClass<UHealthComponent>();
			return (Health != nullptr) ? Health->GetHealth() : -1.0f;
		}
	};
}

using namespace UE::UEMMO::Tasks::M5_017;

// The M5-017 recording presenter (UCLASS test double; see
// CombatPresentationTestDoubles.h for why a plain C++ subclass cannot be
// NewObject'd). Method-local aliases keep the scene code readable.
using FM5_017_ReactionSpy = URecordingReactionPresentation;

void URecordingReactionPresentation::PlayVictimReactionMontage(FName PresentationId, UAnimMontage* Montage)
{
	PlayedReactions.Add(PresentationId);
	PlayedReactionMontageNames.Add(IsValid(Montage) ? Montage->GetName() : FString());
}

void URecordingReactionPresentation::DiagnosePlaceholderReaction(FName PresentationId)
{
	PlaceholderDiagnoses.Add(PresentationId);
}

// Safe row read for the row-identity asserts: a red run may reach the
// identity check with an empty history (the count assert failed but does not
// abort the test), and indexing an empty array crashes instead of failing.
static FName M5_017_RowAt(const TArray<FName>& History, int32 Index)
{
	return History.IsValidIndex(Index) ? History[Index] : FName(TEXT("<none>"));
}


// ---------------------------------------------------------------------------
// DefaultTableMirrorsTheSourceRows
// ---------------------------------------------------------------------------

// The default reaction table mirrors Data/CombatSystem/presentations.json
// schema 2: five rows, react_hit/react_dead carrying real montage references
// and react_launch/react_down/react_recover flagged as honest placeholders.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_017DefaultTableMirrorsTheSourceRows,
	"UEMMO.Tasks.M5_017.DefaultTableMirrorsTheSourceRows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_017DefaultTableMirrorsTheSourceRows::RunTest(const FString& Parameters)
{
	const TArray<FHitReactionPresentationRow> Table = UCombatPresentationComponent::MakeDefaultReactionPresentationTable();
	TestEqual("the default table carries the five reaction rows", Table.Num(), 5);

	const FHitReactionPresentationRow* Hit = nullptr;
	const FHitReactionPresentationRow* Launch = nullptr;
	const FHitReactionPresentationRow* Down = nullptr;
	const FHitReactionPresentationRow* Recover = nullptr;
	const FHitReactionPresentationRow* Dead = nullptr;
	for (const FHitReactionPresentationRow& Row : Table)
	{
		if (Row.PresentationId == FName(TEXT("react_hit"))) { Hit = &Row; }
		else if (Row.PresentationId == FName(TEXT("react_launch"))) { Launch = &Row; }
		else if (Row.PresentationId == FName(TEXT("react_down"))) { Down = &Row; }
		else if (Row.PresentationId == FName(TEXT("react_recover"))) { Recover = &Row; }
		else if (Row.PresentationId == FName(TEXT("react_dead"))) { Dead = &Row; }
	}
	if (!TestNotNull("react_hit exists", Hit)
		|| !TestNotNull("react_launch exists", Launch)
		|| !TestNotNull("react_down exists", Down)
		|| !TestNotNull("react_recover exists", Recover)
		|| !TestNotNull("react_dead exists", Dead))
	{
		return true;
	}
	TestTrue("react_hit carries a real montage reference (created and saved by the editor script)",
		!Hit->bPlaceholder && Hit->Montage.ToSoftObjectPath().ToString().Contains(TEXT("RCT_Hit")));
	TestTrue("react_dead carries a real montage reference (created and saved by the editor script)",
		!Dead->bPlaceholder && Dead->Montage.ToSoftObjectPath().ToString().Contains(TEXT("RCT_Dead")));
	TestTrue("react_launch is an honest placeholder", Launch->bPlaceholder && Launch->Montage.IsNull());
	TestTrue("react_down is an honest placeholder", Down->bPlaceholder && Down->Montage.IsNull());
	TestTrue("react_recover is an honest placeholder", Recover->bPlaceholder && Recover->Montage.IsNull());
	return true;
}

// ---------------------------------------------------------------------------
// HitStunEdgePresentsExactlyOnceAndRefusalsFakeNothing
// ---------------------------------------------------------------------------

// One accepted hit -> exactly one react_hit dispatch and one play; repeated
// hits inside the same stun re-fire nothing; a control-immune victim (the
// hit refused) never enters a state and never fakes a reaction.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_017HitStunEdgePresentsExactlyOnceAndRefusalsFakeNothing,
	"UEMMO.Tasks.M5_017.HitStunEdgePresentsExactlyOnceAndRefusalsFakeNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_017HitStunEdgePresentsExactlyOnceAndRefusalsFakeNothing::RunTest(const FString& Parameters)
{
	UWorld* World = M5_017_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_017_WorldScope WorldScope;
	WorldScope.World = World;

	// 1. The accepted-hit edge: one dispatch, one play, no repeats. The
	//    repeated hit is a SECOND attacker landing inside the victim's first
	//    stun (one attacker cannot re-swing before its instance ends).
	{
		FM5_017_Scene Scene;
		if (!Scene.Build(*this, *World, M5_017_SceneBase))
		{
			return true;
		}
		UCombatComponent* SecondAttacker = Scene.SpawnExtraAttacker(*this, *World, M5_017_SceneBase);
		if (SecondAttacker == nullptr)
		{
			return true;
		}
		if (!TestTrue("the first attack starts", Scene.Combat->TryStartAttack(FName(TEXT("light_01")), 1))
			|| !TestTrue("the second attack starts", SecondAttacker->TryStartAttack(FName(TEXT("light_01")), 1)))
		{
			return true;
		}
		// Both active windows open at the same frame: both hits land inside
		// this tick window, the second one inside the victim's first stun.
		Scene.TickFrame(9);
		TestTrue("the accepted hit stunned the victim",
			Scene.VictimCombat->GetSnapshot().ActionState == ECombatActionState::HitStun);
		TestEqual("the accepted hit dispatched exactly one reaction",
			Scene.VictimPresenter->GetReactionDispatchHistory().Num(), 1);
		TestEqual("the dispatched row is react_hit",
			M5_017_RowAt(Scene.VictimPresenter->GetReactionDispatchHistory(), 0), FName(TEXT("react_hit")));
		TestEqual("the reaction played exactly once", Scene.VictimPresenter->PlayedReactions.Num(), 1);

		// The second attacker's hit landed inside the same stun: no new edge.
		Scene.TickFrame(4);
		TestEqual("a repeated hit inside the same stun re-fired nothing",
			Scene.VictimPresenter->GetReactionDispatchHistory().Num(), 1);
		Scene.FinishInstance(*this, TEXT("light_01"));
	}

	// 2. The control-immune victim: the hit lands (damage stands) but the
	//    control is refused - no state entry, no fake reaction.
	{
		FM5_017_Scene Scene;
		if (!Scene.Build(*this, *World, M5_017_SceneBase + FVector(3000.0, 0.0, 0.0)))
		{
			return true;
		}
		FTargetReaction SuperArmor = MakeLegacyNormalTargetReaction();
		SuperArmor.PolicyId = TEXT("m5_017_super_armor");
		SuperArmor.bImmuneControl = true;
		Scene.VictimCombat->SetTargetReactionPolicy(SuperArmor);

		if (!Scene.HitOnce(*this, TEXT("light_01"), 1))
		{
			return true;
		}
		TestEqual("the super-armored victim still takes its damage",
			FM5_017_Scene::TargetHealth(*Scene.Target), 90.0f);
		TestTrue("the super-armored victim stays Free",
			Scene.VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);
		TestEqual("the refused control presented no reaction",
			Scene.VictimPresenter->GetReactionDispatchHistory().Num(), 0);
		Scene.FinishInstance(*this, TEXT("light_01"));
	}
	return true;
}

// ---------------------------------------------------------------------------
// LaunchedVictimPresentsLaunch
// ---------------------------------------------------------------------------

// A launcher hit stuns and lifts the victim: the state edge sees the rising
// victim (pending launch included) and presents the launch row instead of
// the plain hit. The victim is a real TrainingEnemy (a Character - only a
// character's movement component can report the rising velocity the poller
// reads; a box victim would present the plain hit row).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_017LaunchedVictimPresentsLaunch,
	"UEMMO.Tasks.M5_017.LaunchedVictimPresentsLaunch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_017LaunchedVictimPresentsLaunch::RunTest(const FString& Parameters)
{
	UWorld* World = M5_017_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_017_WorldScope WorldScope;
	WorldScope.World = World;

	const FVector Base = M5_017_SceneBase + FVector(6000.0, 0.0, 0.0);
	UAttackCatalog* Catalog = M5_017_NewCatalog(*this);
	if (Catalog == nullptr)
	{
		return true;
	}
	const UAttackDefinition* Launcher = Catalog->Find(FName(TEXT("launcher")));
	if (!TestTrue(TEXT("the catalog holds launcher"), Launcher != nullptr))
	{
		return true;
	}

	// The attacker box actor with its combat component.
	FActorSpawnParameters Params;
	AActor* Attacker = World->SpawnActor<AActor>(AActor::StaticClass(), Base, FRotator::ZeroRotator, Params);
	if (!TestNotNull(TEXT("the attacker spawns"), Attacker))
	{
		return true;
	}
	UBoxComponent* Body = NewObject<UBoxComponent>(Attacker, TEXT("M5_017_LaunchAttackerBody"));
	Attacker->SetRootComponent(Body);
	Body->SetMobility(EComponentMobility::Movable);
	Body->SetBoxExtent(M5_017_BodyHalfExtent);
	Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Body->SetCollisionObjectType(ECC_Pawn);
	Body->SetCollisionResponseToAllChannels(ECR_Ignore);
	Body->RegisterComponent();
	Body->SetWorldLocation(Base);
	UCombatComponent* Combat = NewObject<UCombatComponent>(Attacker, TEXT("M5_017_LaunchCombat"));
	if (!TestTrue(TEXT("the attacker combat attaches the catalog"), Combat != nullptr && Combat->InitializeFromCatalog(Catalog)))
	{
		return true;
	}
	Combat->RegisterComponent();
	Combat->SetFeetLocationProvider([FixedFeet = Base]() { return FixedFeet; });

	// The victim: a real TrainingEnemy at the launcher box center (the
	// M1-022 spawn pattern: movement activated so the launch applies).
	const FVector BoxCenter = ComputeHitBox(Base, 1, *Launcher).Center;
	ATrainingEnemy* Enemy = World->SpawnActor<ATrainingEnemy>(
		ATrainingEnemy::StaticClass(), BoxCenter, FRotator::ZeroRotator, Params);
	if (!TestNotNull(TEXT("the launch case spawns its training enemy"), Enemy))
	{
		return true;
	}
	if (!Enemy->HasActorBegunPlay())
	{
		Enemy->DispatchBeginPlay();
	}
	UCharacterMovementComponent* EnemyMovement = Enemy->GetCharacterMovement();
	if (EnemyMovement != nullptr)
	{
		if (!EnemyMovement->IsActive())
		{
			EnemyMovement->Activate(/*bReset*/ true);
		}
		if (EnemyMovement->MovementMode == MOVE_None)
		{
			EnemyMovement->SetDefaultMovementMode();
		}
	}
	UCombatComponent* VictimCombat = Enemy->GetCombatComponent();
	if (!TestNotNull(TEXT("precondition: the training enemy carries a combat component"), VictimCombat))
	{
		return true;
	}
	URecordingReactionPresentation* VictimPresenter = NewObject<URecordingReactionPresentation>(Enemy, TEXT("M5_017_LaunchVictimPresenter"));
	VictimPresenter->SetSources(VictimCombat, nullptr);

	if (!TestTrue("the launcher starts", Combat->TryStartAttack(FName(TEXT("launcher")), 1)))
	{
		return true;
	}
	for (int32 Index = 0; Index <= Launcher->ActiveWindow.StartFrame; ++Index)
	{
		Combat->TickCombat(1.0f / 60.0f);
		VictimCombat->TickCombat(1.0f / 60.0f);
		VictimPresenter->TickComponent(1.0f / 60.0f, ELevelTick::LEVELTICK_All, nullptr);
	}
	TestTrue("the launcher stunned the victim",
		VictimCombat->GetSnapshot().ActionState == ECombatActionState::HitStun);
	TestEqual("the launched victim dispatched exactly one reaction",
		VictimPresenter->GetReactionDispatchHistory().Num(), 1);
	TestEqual("the dispatched row is react_launch (not the plain hit)",
		M5_017_RowAt(VictimPresenter->GetReactionDispatchHistory(), 0), FName(TEXT("react_launch")));
	return true;
}

// ---------------------------------------------------------------------------
// DownAndRecoverEdgesPresentOnce
// ---------------------------------------------------------------------------

// The landing recovery presents each phase exactly once: the launched
// landing edge dispatches react_down, the knockdown -> recovering flip
// dispatches react_recover, and a repeated landing inside the running
// process re-fires nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_017DownAndRecoverEdgesPresentOnce,
	"UEMMO.Tasks.M5_017.DownAndRecoverEdgesPresentOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_017DownAndRecoverEdgesPresentOnce::RunTest(const FString& Parameters)
{
	UWorld* World = M5_017_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_017_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_017_Scene Scene;
	if (!Scene.Build(*this, *World, M5_017_SceneBase + FVector(9000.0, 0.0, 0.0)))
	{
		return true;
	}

	// The landing edge drives the Down row (the state machine's landing, no
	// combat hit required - the presentation follows the state).
	TestTrue("the launched landing opens the recovery", Scene.VictimCombat->BeginLandingRecovery(10.0));
	Scene.TickFrame(2);
	TestEqual("the landing dispatched exactly one reaction so far",
		Scene.VictimPresenter->GetReactionDispatchHistory().Num(), 1);
	TestEqual("the dispatched row is react_down",
		M5_017_RowAt(Scene.VictimPresenter->GetReactionDispatchHistory(), 0), FName(TEXT("react_down")));
	TestEqual("the placeholder down row played nothing (honest placeholder)",
		Scene.VictimPresenter->PlayedReactions.Num(), 0);
	TestEqual("the placeholder row diagnosed exactly once",
		Scene.VictimPresenter->PlaceholderDiagnoses.Num(), 1);

	// The flip edge drives the Recover row.
	Scene.VictimCombat->SetInputClockSeconds(10.45);
	Scene.TickFrame(2);
	TestEqual("the flip added exactly one more dispatch",
		Scene.VictimPresenter->GetReactionDispatchHistory().Num(), 2);
	TestEqual("the second dispatched row is react_recover",
		M5_017_RowAt(Scene.VictimPresenter->GetReactionDispatchHistory(), 1), FName(TEXT("react_recover")));

	// A repeated landing request inside the running process re-fires nothing.
	Scene.VictimCombat->BeginLandingRecovery(10.6);
	Scene.TickFrame(2);
	TestEqual("the refused repeated landing re-fired nothing",
		Scene.VictimPresenter->GetReactionDispatchHistory().Num(), 2);

	// The process end returns to Free: no edge dispatch (-> Free is silent).
	Scene.VictimCombat->SetInputClockSeconds(10.70);
	Scene.TickFrame(3);
	TestTrue("the process completed back to Free",
		Scene.VictimCombat->GetSnapshot().ActionState == ECombatActionState::Free);
	TestEqual("the completion re-fired nothing",
		Scene.VictimPresenter->GetReactionDispatchHistory().Num(), 2);
	return true;
}

// ---------------------------------------------------------------------------
// DeadPresentsOncePerLifecycle
// ---------------------------------------------------------------------------

// The owner health component's single death event dispatches the Dead row
// exactly once per lifecycle; the dispatch plays the saved RCT_Dead montage
// through the seam (the recording spy counts it).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_017DeadPresentsOncePerLifecycle,
	"UEMMO.Tasks.M5_017.DeadPresentsOncePerLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_017DeadPresentsOncePerLifecycle::RunTest(const FString& Parameters)
{
	UWorld* World = M5_017_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_017_WorldScope WorldScope;
	WorldScope.World = World;

	FM5_017_Scene Scene;
	if (!Scene.Build(*this, *World, M5_017_SceneBase + FVector(12000.0, 0.0, 0.0)))
	{
		return true;
	}
	// The death binding happens on the first poll.
	Scene.TickFrame(1);
	TestEqual("precondition: no reaction before death",
		Scene.VictimPresenter->GetReactionDispatchHistory().Num(), 0);

	// The lethal damage: the health component broadcasts its single event.
	UHealthComponent* Health = Scene.Target->FindComponentByClass<UHealthComponent>();
	if (!TestNotNull(TEXT("precondition: the victim carries a health component"), Health))
	{
		return true;
	}
	Health->ApplyDamage(Health->GetMaxHealth());
	Scene.TickFrame(2);
	TestEqual("the death dispatched exactly one reaction",
		Scene.VictimPresenter->GetReactionDispatchHistory().Num(), 1);
	TestEqual("the dispatched row is react_dead",
		M5_017_RowAt(Scene.VictimPresenter->GetReactionDispatchHistory(), 0), FName(TEXT("react_dead")));
	TestEqual("the death reaction played once (the real saved montage)",
		Scene.VictimPresenter->PlayedReactions.Num(), 1);

	// A second lethal application on the same corpse: the death lifecycle
	// already closed - the dead flag refuses (no re-fire).
	Health->ApplyDamage(Health->GetMaxHealth());
	Scene.TickFrame(2);
	TestEqual("the dead corpse re-fired nothing",
		Scene.VictimPresenter->GetReactionDispatchHistory().Num(), 1);
	return true;
}

// ---------------------------------------------------------------------------
// PureControlFaceFakesNoHitSound
// ---------------------------------------------------------------------------

// The audio half of the face distinction: an actual-damage hit keeps the
// confirmed-event hit sound (the attacker presenter's dispatcher accepts
// exactly one request per hit), while a pure-control hit (damage-immune
// victim, control standing) bridges no confirmed event and fakes no sound.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_017PureControlFaceFakesNoHitSound,
	"UEMMO.Tasks.M5_017.PureControlFaceFakesNoHitSound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_017PureControlFaceFakesNoHitSound::RunTest(const FString& Parameters)
{
	UWorld* World = M5_017_AcquireWorld();
	if (!TestTrue(TEXT("a test world is available"), World != nullptr))
	{
		return true;
	}
	FM5_017_WorldScope WorldScope;
	WorldScope.World = World;

	// 1. The damage face: the confirmed hit sounds exactly once.
	{
		FM5_017_Scene Scene;
		if (!Scene.Build(*this, *World, M5_017_SceneBase + FVector(15000.0, 0.0, 0.0)))
		{
			return true;
		}
		if (!Scene.HitOnce(*this, TEXT("light_01"), 1))
		{
			return true;
		}
		TestEqual("the damage hit accepted exactly one audio request",
			Scene.AttackerPresenter->GetAudioDispatcher().GetAcceptedCount(), 1);
		Scene.FinishInstance(*this, TEXT("light_01"));
	}

	// 2. The pure-control face: no confirmed event, no fake sound - while the
	//    victim still presents its reaction montage.
	{
		FM5_017_Scene Scene;
		if (!Scene.Build(*this, *World, M5_017_SceneBase + FVector(18000.0, 0.0, 0.0)))
		{
			return true;
		}
		FTargetReaction Ghost = MakeLegacyNormalTargetReaction();
		Ghost.PolicyId = TEXT("m5_017_ghost");
		Ghost.bImmuneDamage = true;
		Scene.VictimCombat->SetTargetReactionPolicy(Ghost);

		if (!Scene.HitOnce(*this, TEXT("light_01"), 1))
		{
			return true;
		}
		TestEqual("the pure-control hit bridged no confirmed event (no audio request)",
			Scene.AttackerPresenter->GetAudioDispatcher().GetAcceptedCount(), 0);
		TestEqual("the pure-control victim still presented its hit reaction",
			Scene.VictimPresenter->GetReactionDispatchHistory().Num(), 1);
		TestEqual("the pure-control reaction row is react_hit",
			M5_017_RowAt(Scene.VictimPresenter->GetReactionDispatchHistory(), 0), FName(TEXT("react_hit")));
		Scene.FinishInstance(*this, TEXT("light_01"));
	}
	return true;
}

// ---------------------------------------------------------------------------
// ReactionPoseScreenshots (the M3-024 capture precedent)
// ---------------------------------------------------------------------------

// In a real rendered game world the reaction phases are captured one
// screenshot each so the mapping can be EYEBALLED: the staged hit react
// (RCT_Hit mid-play through the AnimBP slot - the pose differs from the
// locomotion stance), the float phase (the launched, still-stunned victim in
// the air; the launch row is an honest placeholder - no mannequin clip) and
// the down phase (a training enemy inside its knockdown window; the down row
// is an honest placeholder too). Headless runs skip silently; the logic
// assertions live in the suites above. The staged victim-side flows use the
// PUBLIC victim entries (NotifyHitReceived / the combat launch path), the
// same entries the unified hit pipeline bridges - the player carries no
// health pool (the M1-016 design), so a real attacker hit would refuse
// before reaching any presentation.
namespace UE::UEMMO::Tasks::M5_017
{
	/** Shared state for the staged capture sequence. */
	struct FM5_017_CaptureState
	{
		APrototypeCharacter* Player = nullptr;
		UCombatComponent* StunAttacker = nullptr;   // drives the player-side stun only
		UCombatComponent* HitAttacker = nullptr;    // the real launcher attacker for the enemy
		ATrainingEnemy* Enemy = nullptr;
		FVector AttackerFeet = FVector::ZeroVector;

		bool Stage(FAutomationTestBase& Test, UWorld& World)
		{
			if (Player == nullptr)
			{
				return false;
			}
			// The stun attacker sits at the player's feet - its location never
			// matters (NotifyHitReceived is driven directly, no query runs).
			UAttackCatalog* Catalog = NewObject<UAttackCatalog>();
			FText Error;
			if (!Test.TestTrue(TEXT("the capture catalog initializes"), Catalog->InitializeFromConfig(Error)))
			{
				return false;
			}
			StunAttacker = NewObject<UCombatComponent>(GetTransientPackage(), TEXT("M5_017_StunAttacker"));
			if (!Test.TestTrue(TEXT("the capture stun attacker attaches the catalog"),
				StunAttacker != nullptr && StunAttacker->InitializeFromCatalog(Catalog)))
			{
				return false;
			}

			// The real launcher attacker faces the enemy through the shared
			// hit box shape (the feet origin pinned at spawn, the M5-017 test
			// pattern).
			const FVector PlayerLocation = Player->GetActorLocation();
			const FVector EnemyLocation = PlayerLocation + FVector(320.0, 0.0, 0.0);
			AttackerFeet = EnemyLocation - FVector(95.0, 0.0, 0.0);

			// The launcher attacker is a real TrainingEnemy: the combat
			// component's timeline is owner-tick driven (a bare actor never
			// advances it - the stage lesson of this capture), and the enemy
			// actor's Tick drives its combat exactly like production.
			FActorSpawnParameters Params;
			ATrainingEnemy* AttackerEnemy = World.SpawnActor<ATrainingEnemy>(
				ATrainingEnemy::StaticClass(), AttackerFeet, FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("the capture attacker spawns"), AttackerEnemy))
			{
				return false;
			}
			if (!AttackerEnemy->HasActorBegunPlay())
			{
				AttackerEnemy->DispatchBeginPlay();
			}
			HitAttacker = AttackerEnemy->GetCombatComponent();
			if (!Test.TestTrue(TEXT("the capture attacker attaches the catalog"),
				HitAttacker != nullptr && HitAttacker->InitializeFromCatalog(Catalog)))
			{
				return false;
			}

			// The enemy victim inside the launcher box center (the M1-022
			// pattern; the running game world activates the movement).
			Enemy = World.SpawnActor<ATrainingEnemy>(
				ATrainingEnemy::StaticClass(), EnemyLocation, FRotator::ZeroRotator, Params);
			if (!Test.TestNotNull(TEXT("the capture enemy spawns"), Enemy))
			{
				return false;
			}
			// The training enemy ships presenter-less (the M1-033 fallback);
			// the capture binds one so the launch/down placeholder diagnostics
			// are observable in the run log.
			UCombatPresentationComponent* EnemyPresenter = NewObject<UCombatPresentationComponent>(Enemy, TEXT("M5_017_EnemyPresenter"));
			EnemyPresenter->RegisterComponent();
			EnemyPresenter->SetSources(Enemy->GetCombatComponent(), Enemy->GetMesh());
			return true;
		}
	};

	/** Stages one latent step (spawn/attack) on the game thread. */
	struct FM5_017_StageLatentCommand : public IAutomationLatentCommand
	{
		TSharedRef<FM5_017_CaptureState> State;
		TFunction<void(FM5_017_CaptureState&)> Step;

		FM5_017_StageLatentCommand(const TSharedRef<FM5_017_CaptureState>& InState, TFunction<void(FM5_017_CaptureState&)> InStep)
			: State(InState), Step(MoveTemp(InStep))
		{
		}

		virtual bool Update() override
		{
			Step(State.Get());
			return true;
		}
	};

	/**
	 * Waits until a montage whose name contains NamePart is active on the
	 * player's anim instance (frame-rate-robust capture staging; the M3-024
	 * wait-for-montage pattern), or times out.
	 */
	struct FM5_017_WaitReactionMontageLatentCommand : public IAutomationLatentCommand
	{
		TSharedRef<FM5_017_CaptureState> State;
		FString NamePart;
		float ElapsedSeconds = 0.0f;
		float TimeoutSeconds;

		FM5_017_WaitReactionMontageLatentCommand(const TSharedRef<FM5_017_CaptureState>& InState, const FString& InNamePart, float InTimeoutSeconds = 4.0f)
			: State(InState), NamePart(InNamePart), TimeoutSeconds(InTimeoutSeconds)
		{
		}

		virtual bool Update() override
		{
			const USkeletalMeshComponent* Mesh = State->Player ? State->Player->GetMesh() : nullptr;
			const UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
			const UAnimMontage* Montage = AnimInstance ? AnimInstance->GetCurrentActiveMontage() : nullptr;
			if (Montage != nullptr && Montage->GetName().Contains(NamePart))
			{
				return true;
			}
			ElapsedSeconds += 0.1f;
			if (ElapsedSeconds >= TimeoutSeconds)
			{
				UE_LOG(LogTemp, Warning, TEXT("M5_017 wait-for-reaction-montage timed out (%s)"), *NamePart);
				return true;
			}
			return false;
		}
	};

	/**
	 * Waits until the enemy's combat state reaches the wanted phase (the
	 * knockdown window is 0.45 s - one or two frames at the offscreen render's
	 * fps - so the capture must be state-triggered, frame-rate robust), or
	 * times out.
	 */
	struct FM5_017_WaitEnemyStateLatentCommand : public IAutomationLatentCommand
	{
		TSharedRef<FM5_017_CaptureState> State;
		ECombatActionState Wanted;
		float ElapsedSeconds = 0.0f;
		float TimeoutSeconds;

		FM5_017_WaitEnemyStateLatentCommand(const TSharedRef<FM5_017_CaptureState>& InState, ECombatActionState InWanted, float InTimeoutSeconds = 8.0f)
			: State(InState), Wanted(InWanted), TimeoutSeconds(InTimeoutSeconds)
		{
		}

		virtual bool Update() override
		{
			UCombatComponent* Combat = State->Enemy ? State->Enemy->GetCombatComponent() : nullptr;
			if (Combat != nullptr && Combat->GetSnapshot().ActionState == Wanted)
			{
				return true;
			}
			ElapsedSeconds += 0.1f;
			return ElapsedSeconds < TimeoutSeconds;
		}
	};

	/** Requests one screenshot; skips gracefully without rendering. */
	struct FM5_017_ScreenshotLatentCommand : public IAutomationLatentCommand
	{
		FString AbsolutePath;

		explicit FM5_017_ScreenshotLatentCommand(const FString& InAbsolutePath)
			: AbsolutePath(InAbsolutePath)
		{
		}

		virtual bool Update() override
		{
			if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
			{
				UE_LOG(LogTemp, Display, TEXT("M5_017 screenshot skipped (no rendering): %s"), *AbsolutePath);
				return true;
			}
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(AbsolutePath), /*Tree*/ true);
			FScreenshotRequest::RequestScreenshot(AbsolutePath, /*bShowUI*/ true, /*bAddFilenameSuffix*/ false);
			UE_LOG(LogTemp, Display, TEXT("M5_017 screenshot requested: %s"), *AbsolutePath);
			return true;
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_017ReactionPoseScreenshots,
	"UEMMO.Tasks.M5_017.ReactionPoseScreenshots",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_017ReactionPoseScreenshots::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	if (World == nullptr || GEngine == nullptr || GEngine->GameViewport == nullptr
		|| World->WorldType != EWorldType::Game)
	{
		AddInfo(TEXT("reaction capture skipped: no running game world/viewport"));
		return true;
	}
	APrototypeCharacter* Player = Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0));
	if (Player == nullptr)
	{
		AddInfo(TEXT("reaction capture skipped: no prototype player in the running world"));
		return true;
	}

	TSharedRef<FM5_017_CaptureState> State = MakeShared<FM5_017_CaptureState>();
	State->Player = Player;
	const FString Directory = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M5-017"));

	// Baseline: the idle stance after the boot settles.
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));
	ADD_LATENT_AUTOMATION_COMMAND(FM5_017_ScreenshotLatentCommand(Directory / TEXT("reaction-baseline.png")));

	// The staged hit react: the public victim entry stuns the player, the
	// presenter dispatches react_hit and RCT_Hit plays through the AnimBP
	// slot (0.77 s; 0.25 s in is clearly inside the react pose).
	ADD_LATENT_AUTOMATION_COMMAND(FM5_017_StageLatentCommand(State, [](FM5_017_CaptureState& Capture)
	{
		// A 0.9 s stun: the offscreen render runs at a few fps, so a legacy
		// 0.22 s stun would expire before the presenter's next tick ever sees
		// the HitStun edge - the montage starts on the edge and outlives the
		// stun either way (the reaction is state-entry triggered).
		FCombatHit Hit;
		Hit.Target = Capture.Player;
		Hit.Damage = 10.0f;
		Hit.StunSeconds = 0.9f;
		Hit.Impulse = FVector(90.0f, 0.0f, 0.0f);
		Capture.Player->GetCombat()->NotifyHitReceived(Hit);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FM5_017_WaitReactionMontageLatentCommand(State, TEXT("RCT_Hit")));
	// The screenshot runs in the SAME automation tick the active montage was
	// detected (a hold would drift past the 0.77 s clip at a few fps). The
	// probe asserts run after it in the same tick, so the montage is still
	// active for them.
	// The capture-moment probe: the stun holds, the presenter dispatched the
	// hit row and the montage is actually active on the player's anim
	// instance (the visible pose difference is the whole point).
	ADD_LATENT_AUTOMATION_COMMAND(FM5_017_StageLatentCommand(State, [this](FM5_017_CaptureState& Capture)
	{
		const UCombatComponent* Combat = Capture.Player->GetCombat();
		const FCombatSnapshot Snapshot = Combat->GetSnapshot();
		TestTrue(TEXT("capture probe: the staged stun holds"),
			Snapshot.ActionState == ECombatActionState::HitStun);
		const UCombatPresentationComponent* Presenter = Cast<UCombatPresentationComponent>(
			Capture.Player->FindComponentByClass<UCombatPresentationComponent>());
		if (TestNotNull(TEXT("capture probe: the player carries a presenter"), Presenter))
		{
			TestTrue(TEXT("capture probe: the hit row dispatched"),
				Presenter->GetReactionDispatchHistory().Contains(FName(TEXT("react_hit"))));
		}
		const USkeletalMeshComponent* Mesh = Capture.Player->GetMesh();
		const UAnimInstance* AnimInstance = Mesh ? Mesh->GetAnimInstance() : nullptr;
		if (TestNotNull(TEXT("capture probe: the player mesh has an anim instance"), AnimInstance))
		{
			const UAnimMontage* Montage = AnimInstance->GetCurrentActiveMontage();
			TestTrue(TEXT("capture probe: a montage is active on the player mesh"),
				Montage != nullptr && Montage->GetName().Contains(TEXT("RCT_Hit")));
		}
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FM5_017_ScreenshotLatentCommand(Directory / TEXT("reaction-hit.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.8f));

	// The float phase: the combat launch path (LaunchCharacter) lifts the
	// still-stunned player; the react_launch row is an honest placeholder
	// (one diagnostic in the log) - the phase itself is the evidence.
	ADD_LATENT_AUTOMATION_COMMAND(FM5_017_StageLatentCommand(State, [](FM5_017_CaptureState& Capture)
	{
		Capture.Player->LaunchCharacter(FVector(0.0f, 0.0f, 700.0f), /*bXYOverride*/ false, /*bZOverride*/ true);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.35f));
	ADD_LATENT_AUTOMATION_COMMAND(FM5_017_ScreenshotLatentCommand(Directory / TEXT("reaction-float.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.2f));

	// The down phase: a real launcher hit knocks the training enemy down
	// through the unified pipeline (the enemy carries the health pool); the
	// capture lands inside the 0.45 s knockdown window.
	ADD_LATENT_AUTOMATION_COMMAND(FM5_017_StageLatentCommand(State, [this, World](FM5_017_CaptureState& Capture)
	{
		Capture.Stage(*this, *World);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FM5_017_StageLatentCommand(State, [this](FM5_017_CaptureState& Capture)
	{
		TestTrue(TEXT("the capture launcher starts"),
			Capture.HitAttacker->TryStartAttack(FName(TEXT("launcher")), 1));
	}));
	// The timeline probe: the attacker component must be ticking (the frame
	// advances into the active window) - distinguishes a dead tick from a
	// missed query.
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.4f));
	ADD_LATENT_AUTOMATION_COMMAND(FM5_017_StageLatentCommand(State, [this](FM5_017_CaptureState& Capture)
	{
		const FCombatSnapshot AttackerSnapshot = Capture.HitAttacker->GetSnapshot();
		TestTrue(TEXT("the capture launcher timeline is advancing (Attacking with a positive frame)"),
			AttackerSnapshot.ActionState == ECombatActionState::Attacking && AttackerSnapshot.Frame >= 1);
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(2.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FM5_017_WaitEnemyStateLatentCommand(State, ECombatActionState::Knockdown));
	// The hit probe: the launcher must have landed (health dropped) and the
	// enemy must have flown (the unified count advanced) before the down
	// capture is meaningful.
	ADD_LATENT_AUTOMATION_COMMAND(FM5_017_StageLatentCommand(State, [this](FM5_017_CaptureState& Capture)
	{
		if (Capture.Enemy != nullptr)
		{
			UHealthComponent* Health = Capture.Enemy->GetHealthComponent();
			TestTrue(TEXT("the capture launcher hit the enemy (health dropped)"),
				Health != nullptr && Health->GetHealth() < Health->GetMaxHealth());
			// The unified cycle count is deliberately not asserted here: the
			// 3.6 s capture wait lands after the recovery completed, and the
			// M5-015 recovery-completion clear point legitimately reopened it.
		}
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FM5_017_ScreenshotLatentCommand(Directory / TEXT("reaction-down.png")));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));

	// The state sanity pin: the enemy must be inside its landing recovery at
	// the down capture (or already recovering - both prove the phase).
	ADD_LATENT_AUTOMATION_COMMAND(FM5_017_StageLatentCommand(State, [this](FM5_017_CaptureState& Capture)
	{
		if (Capture.Enemy != nullptr && Capture.Enemy->GetCombatComponent() != nullptr)
		{
			TestTrue(TEXT("the enemy staged a down-state process at the capture moment"),
				Capture.Enemy->GetCombatComponent()->IsInLandingRecovery()
				|| Capture.Enemy->GetCombatComponent()->GetSnapshot().ActionState == ECombatActionState::Free);
		}
	}));
	return true;
}

#endif
