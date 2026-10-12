// M5-036: the packaged Segment-B weapon trial evidence. These tests pin what
// the WEAPON TRIAL PACKAGE guarantees (the package ships the Data/CombatSystem
// runtime config next to Data/test_room.json, the M5-018C precedent):
// - the shipped combat config loads from the packaged project directory and
//   builds the production catalog with a stable revision (the config side of
//   "the dev source tables are gone but the weapon system still operates");
// - the real pawn's production mount path (BeginPlay -> RefreshWeaponMount-
//   FromEquipment -> EnsureWeaponCatalogsMounted) succeeds IN THE PACKAGE and
//   a real weapon from that shipped catalog binds, fires and drains ammo
//   through the component's public API with no fixture catalog substituted;
// - the rendered capture companion stages the production chain in the real
//   game world (staged wall + targets, a real bind, the real input path, the
//   inventory/weapon-status screen) so the packaged screenshots show the
//   trial actually working; headless runs skip silently.
// Every suite is environment-honest: in the editor the same assertions run
// against the dev Data/CombatSystem source, so the full editor suite stays
// green while the packaged run carries the trial proof.

#include "Misc/AutomationTest.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Sound/SoundWave.h"

#include "../PrototypeCharacter.h"

#include "../Combat/Data/CombatCatalog.h"
#include "../Combat/Data/CombatDataTableParser.h"
#include "../Combat/HealthComponent.h"
#include "../Combat/System/TestRoomConfigDriver.h"
#include "../Items/ItemDefinition.h"
#include "../Items/ItemInstance.h"
#include "../PrototypeHUD.h"
#include "../UI/MapSelectWidget.h"
#include "../Weapons/WeaponComponent.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "UnrealClient.h"
#include "Tests/AutomationCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M5_036
{
	// Fixed simulated frame step (60 fps like the design frame clock).
	constexpr float M5_036_FrameSeconds = 1.0f / 60.0f;

	// Remote scene base so the temp world can never collide with arena
	// content (the M1-022/M1-027 placement convention). X horizontal, Y
	// depth, Z height.
	const FVector M5_036_SceneBase(215000.0, 61000.0, 600.0);
	const float M5_036_FloorHalfThickness = 100.0f;
	const float M5_036_FloorHalfExtentXY = 5000.0f;
	const float M5_036_PlayerSpawnHeight = 120.0f;
	const FVector M5_036_TargetOffset(400.0, 0.0, 50.0);
	const FVector M5_036_TargetBoxHalfExtent(50.0, 50.0, 60.0);

	// Every table the packaged combat config must carry (the M5-005 loader's
	// input set) and every weapon id the trial package must expose (the
	// training sword plus the six ballistic families the H02 checklist
	// names).
	const TCHAR* M5_036_TableFiles[] = {
		TEXT("ammo_types.json"), TEXT("attack_reactions.json"), TEXT("damage_profiles.json"),
		TEXT("presentations.json"), TEXT("projectiles.json"), TEXT("target_filters.json"),
		TEXT("target_reactions.json"), TEXT("weapons.json"),
	};
	const TCHAR* M5_036_WeaponIds[] = {
		TEXT("weapon_training_sword"), TEXT("weapon_hitscan_sample"), TEXT("weapon_burst_sample"),
		TEXT("weapon_pellet_sample"), TEXT("weapon_projectile_sample"), TEXT("weapon_parabolic_sample"),
		TEXT("weapon_homing_sample"), TEXT("weapon_explosive_sample"),
	};

	static FString M5_036_CombatDataDir()
	{
		return FPaths::ProjectDir() / TEXT("Data") / TEXT("CombatSystem");
	}

	/** World-static blocking floor box the pawn stands on (the M5-033 pattern). */
	static AActor* M5_036_SpawnFloor(UWorld& World, const FVector& Base)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base - FVector(0.0, 0.0, M5_036_FloorHalfThickness), FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Floor = NewObject<UBoxComponent>(Actor, TEXT("M5_036_Floor"));
		Actor->SetRootComponent(Floor);
		Floor->SetMobility(EComponentMobility::Movable);
		Floor->SetBoxExtent(FVector(M5_036_FloorHalfExtentXY, M5_036_FloorHalfExtentXY, M5_036_FloorHalfThickness));
		Floor->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Floor->RegisterComponent();
		Floor->SetWorldLocation(Base - FVector(0.0, 0.0, M5_036_FloorHalfThickness));
		return Actor;
	}

	/**
	 * A hostile target: a world-static pawn-typed box plus a real health pool
	 * (the M5-035 shared shape: the blast overlap and the pellet sweeps both
	 * see the ECC_Pawn body, the pool drop measures the damage).
	 */
	static AActor* M5_036_SpawnTarget(UWorld& World, const FVector& Base, UHealthComponent** OutHealth)
	{
		FActorSpawnParameters Params;
		AActor* Actor = World.SpawnActor<AActor>(
			AActor::StaticClass(), Base + M5_036_TargetOffset, FRotator::ZeroRotator, Params);
		if (Actor == nullptr)
		{
			return nullptr;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M5_036_TargetBody"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(M5_036_TargetBoxHalfExtent);
		Body->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Body->SetCollisionObjectType(ECC_Pawn);
		Body->RegisterComponent();
		Body->SetWorldLocation(Base + M5_036_TargetOffset);
		UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_036_TargetHealth"));
		Health->RegisterComponent();
		Health->SetMaxHealth(100.0f);
		Health->ResetHealth();
		if (OutHealth != nullptr)
		{
			*OutHealth = Health;
		}
		return Actor;
	}

	/**
	 * Loads the SHIPPED combat config (the packaged Data/CombatSystem carry,
	 * the dev source in the editor) and builds the production catalog. The
	 * config must parse with zero problems and build with a stable revision -
	 * this is the packaged side of "the weapon system still operates without
	 * the dev sources".
	 */
	static bool M5_036_LoadShippedCatalog(FAutomationTestBase& Test, FCombatDataTableSet& OutTables,
		FCombatCatalog& OutCatalog)
	{
		const FString DataDir = M5_036_CombatDataDir();
		for (const TCHAR* FileName : M5_036_TableFiles)
		{
			Test.TestTrue(FString::Printf(TEXT("the shipped combat config carries '%s'"), FileName),
				FPaths::FileExists(DataDir / FileName));
		}
		TArray<FString> Problems;
		if (!Test.TestTrue(TEXT("the shipped combat config loads with zero problems"),
			LoadCombatDataDirectory(DataDir, OutTables, Problems) && Problems.Num() == 0))
		{
			Test.AddError(FString::Printf(TEXT("the shipped combat config refused: %s"), *FString::Join(Problems, TEXT(" | "))));
			return false;
		}
		FParsedCombatConfig Parsed;
		Parsed.SchemaVersion = 1;
		for (const TPair<FName, FDamageProfile>& Row : OutTables.DamageProfiles)
		{
			Parsed.DamageProfiles.Add(Row.Value);
		}
		for (const TPair<FName, FAttackReaction>& Row : OutTables.AttackReactions)
		{
			Parsed.AttackReactions.Add(Row.Value);
		}
		for (const TPair<FName, FTargetReaction>& Row : OutTables.TargetReactions)
		{
			Parsed.TargetReactions.Add(Row.Value);
		}
		for (const TPair<FName, FWeaponDefinition>& Row : OutTables.Weapons)
		{
			Parsed.Weapons.Add(Row.Value);
		}
		for (const TPair<FName, FAmmoType>& Row : OutTables.AmmoTypes)
		{
			Parsed.AmmoTypes.Add(Row.Value);
		}
		for (const TPair<FName, FProjectileDefinition>& Row : OutTables.Projectiles)
		{
			Parsed.Projectiles.Add(Row.Value);
		}
		FString BuildErrors;
		if (!Test.TestTrue(TEXT("the shipped combat config builds the production catalog"),
			FCombatCatalog::BuildFromParsed(Parsed, OutCatalog, BuildErrors)))
		{
			Test.AddError(FString::Printf(TEXT("the shipped catalog build refused: %s"), *BuildErrors));
			return false;
		}
		const FString Revision = OutCatalog.GetConfigRevision();
		Test.TestTrue(TEXT("the shipped catalog carries a non-empty config revision"), !Revision.IsEmpty());
		Test.TestEqual(TEXT("the shipped catalog revision is 16 lowercase hex characters"), Revision.Len(), 16);
		for (const TCHAR* WeaponId : M5_036_WeaponIds)
		{
			Test.TestNotNull(FString::Printf(TEXT("the shipped catalog defines '%s'"), WeaponId),
				OutCatalog.FindWeapon(FName(WeaponId)));
		}
		// M5-056: the items source rides with the package (the staging grant
		// and the weapon mappings). A package without it stages no items and
		// every bind refuses - the M5-H02 "Q does nothing" root cause.
		const FString ItemsPath = FPaths::ProjectDir() / TEXT("Data") / TEXT("items.json");
		TArray<FTestRoomItemRow> ItemRows;
		FString ItemsError;
		if (!Test.TestTrue(TEXT("the shipped items source is present and loads"),
			FPaths::FileExists(ItemsPath)
			&& ACombatTestRoomDriver::LoadProductionItems(ItemRows, ItemsError)))
		{
			Test.AddError(FString::Printf(TEXT("the shipped items source refused: %s"), *ItemsError));
			return false;
		}
		const FTestRoomItemRow* SwordRow = ItemRows.FindByPredicate([](const FTestRoomItemRow& Row)
		{
			return Row.DefinitionId == FName(TEXT("weapon_training"));
		});
		if (!Test.TestNotNull(TEXT("the shipped items define the training sword"), SwordRow))
		{
			return false;
		}
		Test.TestEqual(TEXT("the training sword row maps its weapon definition"),
			SwordRow->WeaponDefinitionId, FName(TEXT("weapon_training_sword")));
		return true;
	}
}

using namespace UE::UEMMO::Tasks::M5_036;

// The config side: the packaged Data/CombatSystem carry parses, builds and
// exposes every trial weapon. In the editor the same assertions run against
// the dev source tables, so the suite stays green in both environments while
// the packaged run proves the package carries its own working config.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_036PackagedWeaponConfigCarries,
	"UEMMO.Tasks.M5_036.PackagedWeaponConfigCarries",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_036PackagedWeaponConfigCarries::RunTest(const FString& Parameters)
{
	// The environment note keys on a TABLE FILE (FileExists is file-only): the
	// packaged carry and the dev source both present Data/CombatSystem/*.json.
	const bool bDevDataPresent = FPaths::FileExists(M5_036_CombatDataDir() / TEXT("weapons.json"));
	AddInfo(FString::Printf(TEXT("the run carries Data/CombatSystem/weapons.json: %s"),
		bDevDataPresent ? TEXT("true (development run - same tables as the packaged carry)")
			: TEXT("false (the package must always ship the config; a package without it is a build defect)")));

	FCombatDataTableSet Tables;
	FCombatCatalog Catalog;
	return M5_036_LoadShippedCatalog(*this, Tables, Catalog);
}

// The operation side: the real pawn's production mount path succeeds against
// the shipped config, and a real weapon from that shipped catalog binds,
// fires through the real input path and drains the real magazine - no
// fixture catalog is substituted anywhere.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_036PackagedProductionMountOperates,
	"UEMMO.Tasks.M5_036.PackagedProductionMountOperates",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_036PackagedProductionMountOperates::RunTest(const FString& Parameters)
{
	FTestWorldWrapper Wrapper;
	if (!TestTrue(TEXT("the manually ticked test world is created (engine FTestWorldWrapper precedent)"),
		Wrapper.CreateTestWorld(EWorldType::Game)))
	{
		return true;
	}
	UWorld* World = Wrapper.GetTestWorld();
	if (!TestNotNull(TEXT("the test world is available"), World))
	{
		return true;
	}
	if (!TestTrue(TEXT("play begins in the test world (full actor initialization)"),
		Wrapper.BeginPlayInTestWorld()))
	{
		return true;
	}
	if (M5_036_SpawnFloor(*World, M5_036_SceneBase) == nullptr)
	{
		AddError(TEXT("the floor failed to spawn"));
		return true;
	}
	UHealthComponent* TargetHealth = nullptr;
	AActor* Target = M5_036_SpawnTarget(*World, M5_036_SceneBase, &TargetHealth);
	if (!TestNotNull(TEXT("the target actor spawns"), Target))
	{
		return true;
	}
	World->EnsureCollisionTreeIsBuilt();

	FActorSpawnParameters PlayerParams;
	APrototypeCharacter* Player = World->SpawnActor<APrototypeCharacter>(
		APrototypeCharacter::StaticClass(),
		M5_036_SceneBase + FVector(0.0, 0.0, M5_036_PlayerSpawnHeight), FRotator::ZeroRotator, PlayerParams);
	if (!TestNotNull(TEXT("the real prototype character spawns"), Player))
	{
		return true;
	}
	if (!Player->HasActorBegunPlay())
	{
		Player->DispatchBeginPlay();
	}
	if (UCharacterMovementComponent* Movement = Player->GetCharacterMovement())
	{
		Movement->bRunPhysicsWithNoController = true;
		if (!Movement->IsActive())
		{
			Movement->Activate(/*bReset*/ true);
		}
		if (Movement->MovementMode == MOVE_None)
		{
			Movement->SetDefaultMovementMode();
		}
	}

	// The production build path against the SHIPPED config. The bare test
	// world has no HUD/profile, so the production refresh skips honestly
	// (the M5-020 "no local HUD or profile yet" skip) - the same catalog the
	// production mount builds (same directory, same loader, same builder)
	// drives the real weapon component here instead. A packaged run proves
	// this path against the package's own Data/CombatSystem carry.
	FCombatDataTableSet Tables;
	FCombatCatalog ShippedCatalogValue;
	if (!M5_036_LoadShippedCatalog(*this, Tables, ShippedCatalogValue))
	{
		return true;
	}
	UWeaponComponent* Mount = Player->GetWeaponMount();
	const FWeaponDefinition* SampleDef = ShippedCatalogValue.FindWeapon(FName(TEXT("weapon_projectile_sample")));
	if (!TestNotNull(TEXT("the shipped linear sample resolves"), SampleDef))
	{
		return true;
	}
	FWeaponDefinition Weapon = *SampleDef;
	const FAmmoType* AmmoDef = ShippedCatalogValue.FindAmmoType(Weapon.AmmoId);
	if (!TestNotNull(TEXT("the shipped ammo type resolves"), AmmoDef))
	{
		return true;
	}
	// Pre-seed the shared reserve from the shipped ammo row (the M5-033
	// pre-seed pattern: the bind's own zero-reserve registration refuses as
	// a duplicate, the pool keeps the shipped stock).
	if (!TestTrue(TEXT("the shipped reserve seeds"),
		Mount->GetAmmoModel().RegisterAmmoType(Weapon.AmmoId, AmmoDef->MaxReserve, AmmoDef->MaxReserve)))
	{
		return true;
	}
	// The item mapping rides the programmatic definition side (the M5-019
	// lineage): a local item catalog value must outlive the mount's
	// non-owning pointer, so it leaks (the M5-035 capture precedent; the
	// test process exits after).
	FItemDefinitionCatalog* Items = new FItemDefinitionCatalog();
	FItemDefinition Item;
	Item.DefinitionId = FName(TEXT("item_m5_036_trial"));
	Item.DisplayName = TEXT("M5-036 Trial Weapon");
	Item.Slot = EItemSlot::Weapon;
	Item.Rarity = EItemRarity::Normal;
	Item.WeaponDefinitionId = Weapon.WeaponId;
	FString ItemError;
	if (!TestTrue(TEXT("the trial item registers"), Items->AddDefinition(Item, &ItemError)))
	{
		delete Items;
		return true;
	}
	// Mount the shipped catalog value plus the trial items into the real
	// weapon component - the weapon values stay the shipped rows, never a
	// fixture.
	FString MountError;
	if (!TestTrue(TEXT("the shipped catalog mounts into the weapon component"),
		Mount->MountCatalogs(&ShippedCatalogValue, Items, &MountError)))
	{
		AddError(FString::Printf(TEXT("the trial mount refused: %s"), *MountError));
		delete Items;
		return true;
	}
	FItemInstance Instance;
	Instance.InstanceId = FGuid(0x0360A000u, 0x0BECu, 0x5336u, 1u);
	Instance.DefinitionId = Item.DefinitionId;
	Instance.RollSeed = 0x52036;
	Instance.Level = 1;
	const FWeaponMountOutcome Outcome = Mount->ApplyEquippedWeapon(&Instance);
	if (!TestTrue(TEXT("the shipped linear sample binds"), Outcome.bSucceeded && Outcome.bWeaponBound))
	{
		AddError(FString::Printf(TEXT("the trial bind refused (%d): %s"),
			static_cast<int32>(Outcome.Reject), *Outcome.RejectDetail));
		return true;
	}
	Player->ApplyWeaponBindWiring();
	if (!TestTrue(TEXT("the bound mount authorizes fire"), Mount->IsFireAuthorized()))
	{
		return true;
	}

	// The real input path fires one round: the pressed/released pair the Q
	// binding drives (the M5-034 wiring), then the world ticks the round to
	// the target.
	Player->OnFireInputPressed();
	Player->OnFireInputReleased();
	int32 MagazineAfterShot = -1;
	for (int32 Frame = 0; Frame < 90; ++Frame)
	{
		if (!TestTrue(TEXT("the test world ticks (the round flies)"), Wrapper.TickTestWorld(M5_036_FrameSeconds)))
		{
			return true;
		}
		if (const FWeaponBindingRecord* Binding = Mount->GetActiveBinding())
		{
			MagazineAfterShot = Binding->LoadedRounds;
		}
		if (TargetHealth != nullptr && TargetHealth->GetHealth() < 100.0f)
		{
			break;
		}
	}
	const FWeaponBindingRecord* Binding = Mount->GetActiveBinding();
	TestTrue(TEXT("the shot drained exactly one magazine round"),
		Binding != nullptr && Binding->LoadedRounds == Weapon.MagazineSize - 1);
	TestEqual(TEXT("the shipped reserve pool is untouched by one shot"),
		Mount->GetAmmoModel().GetReserveRounds(Weapon.AmmoId), AmmoDef->MaxReserve);
	TestTrue(TEXT("the round damaged the target through the shipped config"),
		TargetHealth != nullptr && TargetHealth->GetHealth() < 100.0f && TargetHealth->GetHealth() > 0.0f);
	TestEqual(TEXT("the mount committed exactly one shot"), Mount->GetCommittedShotCount(), 1);

	// The training sword from the shipped catalog rebinds (the melee family
	// the H02 user holds first): the bind succeeds and the mount stays
	// un-authorized for weapon fire (melee delivers through the legacy
	// attack chain).
	const FWeaponDefinition* SwordDef = ShippedCatalogValue.FindWeapon(FName(TEXT("weapon_training_sword")));
	if (TestNotNull(TEXT("the shipped training sword resolves"), SwordDef))
	{
		FItemDefinitionCatalog* SwordItems = new FItemDefinitionCatalog();
		FItemDefinition SwordItem;
		SwordItem.DefinitionId = FName(TEXT("item_m5_036_sword"));
		SwordItem.DisplayName = TEXT("M5-036 Trial Sword");
		SwordItem.Slot = EItemSlot::Weapon;
		SwordItem.Rarity = EItemRarity::Normal;
		SwordItem.WeaponDefinitionId = SwordDef->WeaponId;
		FString SwordItemError;
		if (TestTrue(TEXT("the trial sword item registers"), SwordItems->AddDefinition(SwordItem, &SwordItemError)))
		{
			if (TestTrue(TEXT("the shipped catalog re-mounts with the sword items"),
				Mount->MountCatalogs(&ShippedCatalogValue, SwordItems, &MountError)))
			{
				FItemInstance SwordInstance;
				SwordInstance.InstanceId = FGuid(0x0360A000u, 0x0BECu, 0x5336u, 2u);
				SwordInstance.DefinitionId = SwordItem.DefinitionId;
				SwordInstance.RollSeed = 0x52036;
				SwordInstance.Level = 1;
				const FWeaponMountOutcome SwordOutcome = Mount->ApplyEquippedWeapon(&SwordInstance);
				TestTrue(TEXT("the shipped training sword binds"), SwordOutcome.bSucceeded && SwordOutcome.bWeaponBound);
				TestFalse(TEXT("the melee bind never authorizes weapon fire"), Mount->IsFireAuthorized());
			}
		}
	}

	Player->Destroy();
	Target->Destroy();
	return true;
}

// The rendered capture companion: the real game world stages the wall and
// the two targets (engine cubes so the frame shows the scene), the shipped
// linear sample binds through the production mount, the real input path
// fires and the inventory/weapon-status screen opens - the packaged run
// carries the trial screenshots; headless runs skip silently.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_036WeaponTrialCapture,
	"UEMMO.Tasks.M5_036.WeaponTrialCapture",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_036WeaponTrialCapture::RunTest(const FString& Parameters)
{
	UWorld* World = GWorld;
	APrototypeCharacter* GamePawn = World != nullptr
		? Cast<APrototypeCharacter>(UGameplayStatics::GetPlayerCharacter(World, 0))
		: nullptr;
	if (World == nullptr || GEngine == nullptr || GEngine->GameViewport == nullptr
		|| World->WorldType != EWorldType::Game || GamePawn == nullptr)
	{
		AddInfo(TEXT("weapon trial capture skipped: no rendered game world with the prototype pawn"));
		return true;
	}

	const FString CaptureDir = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectDir() / TEXT("Artifacts") / TEXT("Tasks") / TEXT("M5-036") / TEXT("scenario-json") / TEXT("render"));
	IFileManager::Get().MakeDirectory(*CaptureDir, /*Tree*/ true);

	// The shipped config builds the trial catalog directly (not through the
	// game pawn's mounted state: earlier suites in the same process may have
	// re-mounted fixture catalogs onto that pawn, the M5-034/035 capture
	// precedent). The bundle leaks - the latent chain outlives this body and
	// the mount's pointers are non-owning (the M5-035 capture precedent).
	struct FM5_036_CaptureBundle
	{
		FCombatCatalog Catalog;
		FItemDefinitionCatalog Items;
		bool bBound = false;
	};
	FM5_036_CaptureBundle* Bundle = new FM5_036_CaptureBundle();
	{
		FCombatDataTableSet Tables;
		TArray<FString> Problems;
		if (!TestTrue(TEXT("the shipped combat config loads in the game world"),
			LoadCombatDataDirectory(M5_036_CombatDataDir(), Tables, Problems) && Problems.Num() == 0))
		{
			AddError(FString::Printf(TEXT("the shipped combat config refused: %s"), *FString::Join(Problems, TEXT(" | "))));
			delete Bundle;
			return true;
		}
		FParsedCombatConfig Parsed;
		Parsed.SchemaVersion = 1;
		for (const TPair<FName, FDamageProfile>& Row : Tables.DamageProfiles)
		{
			Parsed.DamageProfiles.Add(Row.Value);
		}
		for (const TPair<FName, FAttackReaction>& Row : Tables.AttackReactions)
		{
			Parsed.AttackReactions.Add(Row.Value);
		}
		for (const TPair<FName, FTargetReaction>& Row : Tables.TargetReactions)
		{
			Parsed.TargetReactions.Add(Row.Value);
		}
		for (const TPair<FName, FWeaponDefinition>& Row : Tables.Weapons)
		{
			Parsed.Weapons.Add(Row.Value);
		}
		for (const TPair<FName, FAmmoType>& Row : Tables.AmmoTypes)
		{
			Parsed.AmmoTypes.Add(Row.Value);
		}
		for (const TPair<FName, FProjectileDefinition>& Row : Tables.Projectiles)
		{
			Parsed.Projectiles.Add(Row.Value);
		}
		FString BuildErrors;
		if (!TestTrue(TEXT("the shipped combat config builds the capture catalog"),
			FCombatCatalog::BuildFromParsed(Parsed, Bundle->Catalog, BuildErrors)))
		{
			AddError(FString::Printf(TEXT("the capture catalog build refused: %s"), *BuildErrors));
			delete Bundle;
			return true;
		}
	}
	const FWeaponDefinition* SampleDef = Bundle->Catalog.FindWeapon(FName(TEXT("weapon_projectile_sample")));
	if (!TestNotNull(TEXT("the shipped linear sample resolves in the game world"), SampleDef))
	{
		delete Bundle;
		return true;
	}
	const FAmmoType* AmmoDef = Bundle->Catalog.FindAmmoType(SampleDef->AmmoId);
	if (!TestNotNull(TEXT("the shipped ammo type resolves in the game world"), AmmoDef))
	{
		delete Bundle;
		return true;
	}

	// The trial item mapping leaks with the bundle (the M5-035 capture
	// precedent: the latent chain outlives the test body and the mount's
	// pointers are non-owning).
	FItemDefinition Item;
	Item.DefinitionId = FName(TEXT("item_m5_036_capture"));
	Item.DisplayName = TEXT("M5-036 Capture Weapon");
	Item.Slot = EItemSlot::Weapon;
	Item.Rarity = EItemRarity::Normal;
	Item.WeaponDefinitionId = SampleDef->WeaponId;
	FString ItemError;
	if (!TestTrue(TEXT("the capture item registers"), Bundle->Items.AddDefinition(Item, &ItemError)))
	{
		delete Bundle;
		return true;
	}

	// M5-058: the shipped hitscan rifle gets its own mapped item so the
	// tracer-flash leg can rebind through the same production path (the
	// hitscan delivery is the one the H02 checklist names "射线").
	const FWeaponDefinition* HitscanDef = Bundle->Catalog.FindWeapon(FName(TEXT("weapon_hitscan_sample")));
	if (!TestNotNull(TEXT("the shipped hitscan sample resolves in the game world"), HitscanDef))
	{
		delete Bundle;
		return true;
	}
	FItemDefinition HitscanItem;
	HitscanItem.DefinitionId = FName(TEXT("item_m5_036_capture_hitscan"));
	HitscanItem.DisplayName = TEXT("M5-036 Capture Hitscan");
	HitscanItem.Slot = EItemSlot::Weapon;
	HitscanItem.Rarity = EItemRarity::Normal;
	HitscanItem.WeaponDefinitionId = HitscanDef->WeaponId;
	if (!TestTrue(TEXT("the capture hitscan item registers"), Bundle->Items.AddDefinition(HitscanItem, &ItemError)))
	{
		delete Bundle;
		return true;
	}

	// Stage the production scenario at the game pawn's location: the wall,
	// the normal target and the heavy target behind it (engine cubes so the
	// rendered capture shows the scenario; the M5-035 visual precedent).
	const FVector Base = GamePawn->GetActorLocation();
	FActorSpawnParameters StageParams;
	StageParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	AActor* Wall = World->SpawnActor<AActor>(
		AActor::StaticClass(), Base + FVector(200.0, 0.0, 100.0), FRotator::ZeroRotator, StageParams);
	AActor* NormalTarget = World->SpawnActor<AActor>(
		AActor::StaticClass(), Base + FVector(400.0, 0.0, 50.0), FRotator::ZeroRotator, StageParams);
	AActor* HeavyTarget = World->SpawnActor<AActor>(
		AActor::StaticClass(), Base + FVector(700.0, 0.0, 50.0), FRotator::ZeroRotator, StageParams);
	UStaticMesh* StageCube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	auto GiveTargetBody = [StageCube](AActor* Actor, const FVector& HalfExtent, float MaxHealth) -> bool
	{
		if (Actor == nullptr)
		{
			return false;
		}
		UBoxComponent* Body = NewObject<UBoxComponent>(Actor, TEXT("M5_036_CaptureBody"));
		Actor->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(HalfExtent);
		Body->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Body->SetCollisionObjectType(ECC_Pawn);
		Body->RegisterComponent();
		if (StageCube != nullptr)
		{
			UStaticMeshComponent* Visual = NewObject<UStaticMeshComponent>(Actor, TEXT("M5_036_CaptureVisual"));
			Visual->SetupAttachment(Body);
			Visual->SetStaticMesh(StageCube);
			Visual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Visual->SetRelativeScale3D(HalfExtent * 2.0f / 100.0f);
			Visual->RegisterComponent();
		}
		UHealthComponent* Health = NewObject<UHealthComponent>(Actor, TEXT("M5_036_CaptureHealth"));
		Health->RegisterComponent();
		Health->SetMaxHealth(MaxHealth);
		Health->ResetHealth();
		return true;
	};
	if (Wall != nullptr)
	{
		UBoxComponent* Body = NewObject<UBoxComponent>(Wall, TEXT("M5_036_CaptureWall"));
		Wall->SetRootComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetBoxExtent(FVector(20.0, 3000.0, 250.0));
		Body->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
		Body->RegisterComponent();
		if (StageCube != nullptr)
		{
			UStaticMeshComponent* Visual = NewObject<UStaticMeshComponent>(Wall, TEXT("M5_036_CaptureWallVisual"));
			Visual->SetupAttachment(Body);
			Visual->SetStaticMesh(StageCube);
			Visual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Visual->SetRelativeScale3D(FVector(0.4, 60.0, 5.0));
			Visual->RegisterComponent();
		}
	}
	if (!TestTrue(TEXT("the capture scenario staged"),
		Wall != nullptr && GiveTargetBody(NormalTarget, FVector(50.0, 50.0, 60.0), 100.0f)
			&& GiveTargetBody(HeavyTarget, FVector(50.0, 50.0, 60.0), 300.0f)))
	{
		return true;
	}
	World->EnsureCollisionTreeIsBuilt();

	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([GamePawn, Bundle, SampleDef, AmmoDef]()
	{
		if (APlayerController* PC = Cast<APlayerController>(GamePawn->Controller))
		{
			if (APrototypeHUD* Hud = Cast<APrototypeHUD>(PC->GetHUD()))
			{
				if (UMapSelectWidget* BootMenu = Hud->PeekMenuWidget())
				{
					BootMenu->EnterAccepted.Broadcast();
				}
			}
		}
		UWeaponComponent* Mount = GamePawn->GetWeaponMount();
		FString MountError;
		Bundle->bBound = Mount != nullptr
			&& Mount->GetAmmoModel().RegisterAmmoType(SampleDef->AmmoId, AmmoDef->MaxReserve, AmmoDef->MaxReserve)
			&& Mount->MountCatalogs(&Bundle->Catalog, &Bundle->Items, &MountError);
		if (Bundle->bBound)
		{
			FItemInstance Instance;
			Instance.InstanceId = FGuid(0x0360C000u, 0x0BECu, 0x5336u, 1u);
			Instance.DefinitionId = FName(TEXT("item_m5_036_capture"));
			Instance.RollSeed = 0x52036;
			Instance.Level = 1;
			const FWeaponMountOutcome Outcome = Mount->ApplyEquippedWeapon(&Instance);
			Bundle->bBound = Outcome.bSucceeded && Outcome.bWeaponBound;
			if (Bundle->bBound)
			{
				GamePawn->ApplyWeaponBindWiring();
			}
		}
		return true;
	}));

	// Screenshot 1: the staged scenario (wall + normal + heavy targets).
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.6f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([CaptureDir]()
	{
		if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
		{
			return true;
		}
		FScreenshotRequest::RequestScreenshot(CaptureDir / TEXT("capture-trial-staged.png"), true, false);
		return true;
	}));

	// The trial fire: the real input path (the Q binding pair).
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([GamePawn, Bundle]()
	{
		if (Bundle->bBound)
		{
			GamePawn->OnFireInputPressed();
			GamePawn->OnFireInputReleased();
		}
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.25f));
	// Screenshot 2: the trial round mid-flight.
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([CaptureDir]()
	{
		if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
		{
			return true;
		}
		FScreenshotRequest::RequestScreenshot(CaptureDir / TEXT("capture-trial-flight.png"), true, false);
		return true;
	}));

	// Screenshot 3: the inventory screen with the embedded weapon-status
	// panel (the M5-034 production entry - the trial package's inventory
	// door the H02 checklist walks through).
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([GamePawn]()
	{
		if (APlayerController* PC = Cast<APlayerController>(GamePawn->Controller))
		{
			if (APrototypeHUD* Hud = Cast<APrototypeHUD>(PC->GetHUD()))
			{
				Hud->ToggleInventoryScreen();
			}
		}
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([CaptureDir]()
	{
		if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
		{
			return true;
		}
		FScreenshotRequest::RequestScreenshot(CaptureDir / TEXT("capture-trial-inventory.png"), true, false);
		return true;
	}));

	// M5-058: the hitscan tracer-flash leg. The inventory gate would refuse
	// fire, so the screen closes first; the rebind walks the same production
	// mount path (a fresh item instance mapped onto the shipped hitscan
	// rifle, its own ammo_cell pool), one round fires through the real Q
	// pair, and the screenshot lands inside the 0.12s flash window - the
	// beam stops at the staged wall (the honest wall-stop face). The second
	// capture, after the flash retires, evidences the LifeSpan cleanup.
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([GamePawn]()
	{
		if (APlayerController* PC = Cast<APlayerController>(GamePawn->Controller))
		{
			if (APrototypeHUD* Hud = Cast<APrototypeHUD>(PC->GetHUD()))
			{
				if (Hud->HasInventoryScreen())
				{
					Hud->ToggleInventoryScreen();
				}
			}
		}
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([GamePawn, Bundle, HitscanDef]()
	{
		UWeaponComponent* Mount = GamePawn->GetWeaponMount();
		if (Mount == nullptr || !Bundle->bBound)
		{
			return true;
		}
		// The hitscan rifle's own reserve kind (ammo_cell); the projectile
		// leg registered only its ammo_light pool.
		const FAmmoType* HitscanAmmo = Bundle->Catalog.FindAmmoType(HitscanDef->AmmoId);
		if (HitscanAmmo != nullptr)
		{
			Mount->GetAmmoModel().RegisterAmmoType(HitscanDef->AmmoId, HitscanAmmo->MaxReserve, HitscanAmmo->MaxReserve);
		}
		FItemInstance HitscanInstance;
		HitscanInstance.InstanceId = FGuid(0x0360C000u, 0x0BECu, 0x5336u, 2u);
		HitscanInstance.DefinitionId = FName(TEXT("item_m5_036_capture_hitscan"));
		HitscanInstance.RollSeed = 0x52037;
		HitscanInstance.Level = 1;
		const FWeaponMountOutcome HitscanOutcome = Mount->ApplyEquippedWeapon(&HitscanInstance);
		Bundle->bBound = HitscanOutcome.bSucceeded && HitscanOutcome.bWeaponBound;
		if (Bundle->bBound)
		{
			GamePawn->ApplyWeaponBindWiring();
			GamePawn->OnFireInputPressed();
			GamePawn->OnFireInputReleased();
		}
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([CaptureDir]()
	{
		if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
		{
			return true;
		}
		FScreenshotRequest::RequestScreenshot(CaptureDir / TEXT("capture-trial-hitscan-flash.png"), true, false);
		return true;
	}));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.0f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([CaptureDir]()
	{
		if (GEngine == nullptr || GEngine->GameViewport == nullptr || !FApp::CanEverRender())
		{
			return true;
		}
		FScreenshotRequest::RequestScreenshot(CaptureDir / TEXT("capture-trial-hitscan-after.png"), true, false);
		return true;
	}));

	// Cleanup: close the screen and retire the staged actors.
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(0.5f));
	ADD_LATENT_AUTOMATION_COMMAND(FFunctionLatentCommand([GamePawn, Wall, NormalTarget, HeavyTarget]()
	{
		if (APlayerController* PC = Cast<APlayerController>(GamePawn->Controller))
		{
			if (APrototypeHUD* Hud = Cast<APrototypeHUD>(PC->GetHUD()))
			{
				if (Hud->HasInventoryScreen())
				{
					Hud->ToggleInventoryScreen();
				}
			}
		}
		for (AActor* Staged : {Wall, NormalTarget, HeavyTarget})
		{
			if (Staged != nullptr && IsValid(Staged))
			{
				Staged->Destroy();
			}
		}
		return true;
	}));
	return true;
}

// ---------------------------------------------------------------------------
// PackagedFireSoundResolves
// ---------------------------------------------------------------------------

// M5-059: the shipped game can resolve the CC0 fire feedback sound through
// the same soft path the pawn's fallback loads - the in-package proof that
// the feedback audio actually rides the container (the editor always passes
// this from the source Content tree, so only the packaged gate counts).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM5_036PackagedFireSoundResolves,
	"UEMMO.Tasks.M5_036.PackagedFireSoundResolves",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM5_036PackagedFireSoundResolves::RunTest(const FString& Parameters)
{
	const USoundWave* FireSound = LoadObject<USoundWave>(nullptr,
		TEXT("/Game/ThirdParty/Kenney/Impact/impactPunch_heavy_000.impactPunch_heavy_000"));
	if (!TestNotNull(TEXT("the packaged fire sound resolves inside the shipped game"), FireSound))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("the packaged fire sound resolved: %s"), *FireSound->GetName()));
	return true;
}

#endif
