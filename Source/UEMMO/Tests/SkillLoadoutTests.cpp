// M7-003: the skill chain - the loadout component's precheck/commit economy
// (Locked/WrongProfession/InsufficientMP/Cooldown/Busy/MissingAttack/
// MissingAsset/InvalidSlot), the SP purchase flow, the profession-driven
// MP/SP resolvers, the combat damage-scale hook, the launcher payment, the
// SelfBuff policy handler, the run-transient reset and the GroundArea data
// pins. Pure component tests: no world, no input events (the automated
// motion tests never cover real keyboard events - the manual play validation
// stays the user's).

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UObjectGlobals.h"

#include "../Combat/AttackCatalog.h"
#include "../Combat/AttackDefinition.h"
#include "../Combat/CombatComponent.h"
#include "../Combat/Data/CombatDataTableParser.h"
#include "../Profession/ProfessionCatalog.h"
#include "../Profession/ProfessionTypes.h"
#include "../Profile/ProfileSubsystem.h"
#include "../Skills/SkillLoadoutComponent.h"
#include "../Skills/SkillTypes.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M7_003
{
	// Deterministic fixture identities.
	static FGuid FixtureCharacterId()
	{
		return FGuid(0x07A30000u, 0x0001u, 0x0001u, 0x0001u);
	}

	/** The attack definitions the fixture catalog serves (the legacy ids plus
	 * the five wired skill attacks). */
	static TArray<UAttackDefinition*> MakeAttackDefinitions(UObject* Outer)
	{
		TArray<UAttackDefinition*> Definitions;
		struct FRow
		{
			const TCHAR* Id;
			int32 Duration;
			int32 ActiveStart;
			int32 ActiveEnd;
		};
		const FRow Rows[] = {
			{TEXT("light_01"), 26, 7, 11},
			{TEXT("launcher"), 40, 12, 17},
			{TEXT("singlekick"), 24, 11, 15},
			{TEXT("damagelowkick"), 24, 11, 15},
			{TEXT("throwsand"), 28, 13, 17},
			{TEXT("earthbreak"), 30, 11, 14},
			{TEXT("stomp"), 22, 11, 14},
		};
		for (const FRow& Row : Rows)
		{
			UAttackDefinition* Definition = NewObject<UAttackDefinition>(Outer);
			Definition->AttackId = FName(Row.Id);
			Definition->DurationFrames = Row.Duration;
			Definition->ActiveWindow.StartFrame = Row.ActiveStart;
			Definition->ActiveWindow.EndFrame = Row.ActiveEnd;
			Definition->CancelWindow.StartFrame = Row.ActiveEnd + 1;
			Definition->CancelWindow.EndFrame = Row.Duration - 2;
			Definition->BaseDamage = 10.f;
			Definition->AttackCoefficient = 1.f;
			Definition->ClipEndSeconds = 0.4f;
			Definitions.Add(Definition);
		}
		return Definitions;
	}

	/** The fixture profession table set: fighter (+ launcher_z, the hotbar
	 * five) plus the deliberately broken rows each refusal reason needs.
	 * Rank-2 rows cost 10 SP (the shortage fixture); rank-1 rows are free. */
	static FProfessionTableSet MakeTableSet()
	{
		FProfessionTableSet Tables;

		FProfessionLevelCurveRow Proto;
		Proto.CurveId = FName(TEXT("proto_curve"));
		Proto.Level = 1;
		Proto.XPToNext = 0;
		Tables.LevelCurves.FindOrAdd(Proto.CurveId).Add(Proto);

		FProfessionLevelCurveRow F1;
		F1.CurveId = FName(TEXT("fighter_curve"));
		F1.Level = 1;
		F1.XPToNext = 100;
		F1.BaseMaxHP = 100;
		F1.BaseMaxMP = 100;
		F1.SkillPoints = 3;
		Tables.LevelCurves.FindOrAdd(F1.CurveId).Add(F1);
		FProfessionLevelCurveRow F2 = F1;
		F2.Level = 2;
		F2.XPToNext = 0;
		F2.BaseMaxMP = 115;
		F2.SkillPoints = 0;
		Tables.LevelCurves.FindOrAdd(F2.CurveId).Add(F2);

		FProfessionRow ProtoProfession;
		ProtoProfession.ProfessionId = FName(TEXT("prototype"));
		ProtoProfession.CurveId = FName(TEXT("proto_curve"));
		Tables.Professions.Add(ProtoProfession.ProfessionId, ProtoProfession);

		FProfessionRow Monk;
		Monk.ProfessionId = FName(TEXT("monk"));
		Monk.CurveId = FName(TEXT("proto_curve"));
		Tables.Professions.Add(Monk.ProfessionId, Monk);

		FProfessionRow Fighter;
		Fighter.ProfessionId = FName(TEXT("fighter"));
		Fighter.DisplayName = TEXT("fighter");
		Fighter.CurveId = FName(TEXT("fighter_curve"));
		Fighter.StartingSkillRanks.Add(FName(TEXT("launcher_z")), 1);
		Tables.Professions.Add(Fighter.ProfessionId, Fighter);

		auto AddSkill = [&Tables](FName SkillId, FName ProfessionId, int32 MaxRank, FName Eligibility,
			EProfessionSkillEffectPolicy Policy, FName AttackId, bool bPending)
		{
			FSkillRow Row;
			Row.SkillId = SkillId;
			Row.ProfessionId = ProfessionId;
			Row.MaxRank = MaxRank;
			Row.SlotEligibility = Eligibility;
			Row.EffectPolicy = Policy;
			Row.AttackId = AttackId;
			Row.AttackPending = bPending;
			Tables.Skills.Add(SkillId, Row);
			for (int32 Rank = 1; Rank <= MaxRank; ++Rank)
			{
				FSkillLevelRow Level;
				Level.SkillId = SkillId;
				Level.Rank = Rank;
				Level.RequiredCharacterLevel = 1;
				Level.SPCost = Rank == 1 ? 0 : 10;
				Level.CooldownS = 1.5f;
				Level.MPCost = 10;
				Level.DamageCoefficient = 1.f;
				Tables.SkillLevels.FindOrAdd(SkillId).Add(Level);
			}
		};

		AddSkill(FName(TEXT("launcher_z")), FName(TEXT("fighter")), 2, FName(TEXT("launcher")),
			EProfessionSkillEffectPolicy::SingleMeleeHit, FName(TEXT("launcher")), false);
		AddSkill(FName(TEXT("singlekick_w")), FName(TEXT("fighter")), 2, FName(TEXT("hotbar")),
			EProfessionSkillEffectPolicy::SingleMeleeHit, FName(TEXT("singlekick")), false);
		// The refusal fixtures (never real content rows).
		AddSkill(FName(TEXT("badattack_s")), FName(TEXT("fighter")), 2, FName(TEXT("hotbar")),
			EProfessionSkillEffectPolicy::SingleMeleeHit, FName(TEXT("no_such_attack")), false);
		AddSkill(FName(TEXT("pending_s")), FName(TEXT("fighter")), 1, FName(TEXT("hotbar")),
			EProfessionSkillEffectPolicy::SingleMeleeHit, FName(TEXT("anything")), true);
		AddSkill(FName(TEXT("monk_s")), FName(TEXT("monk")), 1, FName(TEXT("hotbar")),
			EProfessionSkillEffectPolicy::SingleMeleeHit, FName(TEXT("singlekick")), false);
		AddSkill(FName(TEXT("selfbuff_t")), FName(TEXT("fighter")), 1, FName(TEXT("hotbar")),
			EProfessionSkillEffectPolicy::SelfBuff, FName(TEXT("selfbuff_stub")), false);
		{
			FSkillLevelRow& Buff = Tables.SkillLevels.FindOrAdd(FName(TEXT("selfbuff_t")))[0];
			Buff.EffectParameters.Add(FName(TEXT("attack_bonus")), 15.f);
			Buff.EffectParameters.Add(FName(TEXT("duration_s")), 3.f);
		}

		auto AddLoadout = [&Tables](FName ProfessionId, int32 Slot, FName SkillId)
		{
			FLoadoutRow Row;
			Row.ProfessionId = ProfessionId;
			Row.SlotIndex = Slot;
			Row.SkillId = SkillId;
			Tables.Loadouts.Add(Row);
		};
		AddLoadout(FName(TEXT("fighter")), 2, FName(TEXT("singlekick_w")));
		AddLoadout(FName(TEXT("fighter")), 3, FName(TEXT("monk_s")));
		AddLoadout(FName(TEXT("fighter")), 4, FName(TEXT("pending_s")));
		AddLoadout(FName(TEXT("fighter")), 5, FName(TEXT("selfbuff_t")));
		AddLoadout(FName(TEXT("fighter")), 6, FName(TEXT("badattack_s")));
		// Q (1), D (7), F (8) stay empty by design (Q is the weapon slot).
		return Tables;
	}

	/** A published fixture catalog (nullptr on a failed build). */
	static const FProfessionCatalog* MakeCatalog(FAutomationTestBase& Test,
		FProfessionCatalog& Storage)
	{
		TArray<FString> Problems;
		if (!Test.TestTrue(TEXT("fixture: the catalog builds"),
			Storage.Build(MakeTableSet(), Problems)))
		{
			for (const FString& Problem : Problems)
			{
				Test.AddError(FString::Printf(TEXT("fixture: %s"), *Problem));
			}
			return nullptr;
		}
		return &Storage;
	}

	/** A rooted GameInstance owner (a UGameInstanceSubsystem's ClassWithin). */
	static UGameInstance* NewSubsystemOwner()
	{
		UGameInstance* Owner = NewObject<UGameInstance>(GetTransientPackage());
		Owner->AddToRoot();
		return Owner;
	}

	/** The fixture profile: a FIGHTER character holding launcher_z rank 1
	 * (the migrated-legacy shape; the hotbar five are NOT learned yet) and
	 * 20 unspent SP. The curve fixture tops out at level 2, so the level-4
	 * character reads the clamped level-2 pool (115 MP). */
	static UProfileSubsystem* MakeFighterProfile(FAutomationTestBase& Test, UObject* Owner)
	{
		UProfileSubsystem* Profile = NewObject<UProfileSubsystem>(Owner);
		Profile->AddToRoot();
		FProfileSnapshot Snapshot;
		Snapshot.CharacterId = FixtureCharacterId();
		Snapshot.Level = 4;
		Snapshot.XP = 0;
		Snapshot.ProfessionId = FName(TEXT("fighter"));
		TArray<FSkillRankSaveEntry> Ranks;
		FSkillRankSaveEntry Launcher;
		Launcher.SkillId = FName(TEXT("launcher_z"));
		Launcher.Rank = 1;
		Ranks.Add(Launcher);
		Test.TestTrue(TEXT("fixture: the fighter profile restores"),
			Profile->RestoreFromSave(Snapshot, Profile->GetInventory(),
				{}, {}, {}, {}, {}, false, FString(),
				{}, FName(TEXT("fighter")), Ranks, 20, {}, FString(), 0));
		return Profile;
	}

	/** A live combat component bound to the fixture attack catalog. */
	static UCombatComponent* MakeCombat(UObject* Outer, UAttackCatalog* Catalog)
	{
		UCombatComponent* Combat = NewObject<UCombatComponent>(Outer);
		Combat->InitializeFromCatalog(Catalog);
		return Combat;
	}
}

using namespace UE::UEMMO::Tasks::M7_003;

// -- 1: the activation matrix (Locked / purchase / Cooldown / InsufficientMP /
//       Busy / death / InvalidSlot) --------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_003ActivationMatrix,
	"UEMMO.Tasks.M7_003.ActivationMatrix",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_003ActivationMatrix::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	double Now = 100.0;
	UAttackCatalog* Attacks = NewObject<UAttackCatalog>();
	FText CatalogError;
	if (!Test.TestTrue(TEXT("fixture: the attack catalog builds"),
		Attacks->BuildFromDefinitions(MakeAttackDefinitions(GetTransientPackage()), CatalogError)))
	{
		return false;
	}
	FProfessionCatalog CatalogStorage;
	const FProfessionCatalog* Catalog = MakeCatalog(Test, CatalogStorage);
	if (Catalog == nullptr)
	{
		return false;
	}
	UProfileSubsystem* Profile = MakeFighterProfile(Test, NewSubsystemOwner());
	UCombatComponent* Combat = MakeCombat(GetTransientPackage(), Attacks);

	USkillLoadoutComponent* Loadout = NewObject<USkillLoadoutComponent>(GetTransientPackage());
	Loadout->Initialize(Combat, Profile, Catalog, TEXT("fixture-revision"));
	Loadout->SetAttackCatalog(Attacks);
	Loadout->SetClockProvider([&Now]() { return Now; });
	Loadout->RefreshMaxMPFromProfile();
	Loadout->RestoreFullMP();
	Test.TestEqual(TEXT("the fixture pool is the clamped level-2 MP"), Loadout->GetCurrentMP(), 115);

	// Locked: the hotbar five are not learned (only launcher_z rank 1).
	{
		const FSkillActivationOutcome Out = Loadout->TryActivateSkill(2);
		Test.TestEqual(TEXT("an unlearned skill refuses with Locked"), Out.Error, ESkillActivationError::Locked);
	}

	// Purchase + commit: rank 1 is free; the activation starts the attack,
	// pays the MP and writes the cooldown.
	{
		FString Problem;
		Test.TestTrue(TEXT("the free rank-1 purchase succeeds"), Profile->TryPurchaseSkillRank(
			FName(TEXT("singlekick_w")), 1, 2, 1, 0, Problem));
		const FSkillActivationOutcome Out = Loadout->TryActivateSkill(2);
		Test.TestEqual(TEXT("the learned skill activates"), Out.Error, ESkillActivationError::None);
		Test.TestTrue(TEXT("the outcome is accepted"), Out.bAccepted);
		Test.TestEqual(TEXT("the attack started"), Combat->GetSnapshot().AttackId, FName(TEXT("singlekick")));
		Test.TestTrue(TEXT("the attack instance is live"), Combat->GetSnapshot().InstanceId > 0);
		Test.TestEqual(TEXT("the MP was paid (10 of 115)"), Loadout->GetCurrentMP(), 105);
		Test.TestEqual(TEXT("the cooldown was written"),
			Loadout->IsOnCooldown(FName(TEXT("singlekick_w"))), true);
		Test.TestEqual(TEXT("the config revision rides the component"),
			Loadout->GetConfigRevision(), FString(TEXT("fixture-revision")));
	}

	// Cooldown: the immediate second press refuses; the expiry restores it.
	{
		Combat->ResetCombat();
		const FSkillActivationOutcome Second = Loadout->TryActivateSkill(2);
		Test.TestEqual(TEXT("the immediate re-activation refuses with Cooldown"),
			Second.Error, ESkillActivationError::Cooldown);
		Now += 2.0;
		const FSkillActivationOutcome AfterExpiry = Loadout->TryActivateSkill(2);
		Test.TestEqual(TEXT("the post-expiry activation is accepted"),
			AfterExpiry.Error, ESkillActivationError::None);
	}

	// InsufficientMP: a drained pool refuses; the refill restores.
	{
		Combat->ResetCombat();
		Now += 2.0;
		Loadout->SetCurrentMP(0);
		const FSkillActivationOutcome Broke = Loadout->TryActivateSkill(2);
		Test.TestEqual(TEXT("an empty MP pool refuses with InsufficientMP"),
			Broke.Error, ESkillActivationError::InsufficientMP);
		Loadout->RestoreFullMP();
		const FSkillActivationOutcome Refilled = Loadout->TryActivateSkill(2);
		Test.TestEqual(TEXT("the refilled activation is accepted"),
			Refilled.Error, ESkillActivationError::None);
	}

	// Busy: the combatant is mid-attack (the state the combat chain owns).
	{
		Combat->ResetCombat();
		Now += 2.0;
		Test.TestTrue(TEXT("setup: the legacy attack starts"),
			Combat->TryStartAttack(FName(TEXT("light_01")), 1));
		const FSkillActivationOutcome Busy = Loadout->TryActivateSkill(2);
		Test.TestEqual(TEXT("a busy combatant refuses with Busy"), Busy.Error, ESkillActivationError::Busy);
	}

	// Death: the dead combatant cannot start anything.
	{
		Combat->ResetCombat();
		Combat->SetDead(true);
		const FSkillActivationOutcome Dead = Loadout->TryActivateSkill(2);
		Test.TestEqual(TEXT("a dead combatant refuses with Busy"), Dead.Error, ESkillActivationError::Busy);
		Combat->SetDead(false);
	}

	// InvalidSlot: the empty D slot and the out-of-range press.
	{
		Combat->ResetCombat();
		const FSkillActivationOutcome EmptySlot = Loadout->TryActivateSkill(7);
		Test.TestEqual(TEXT("the empty D slot refuses with InvalidSlot"),
			EmptySlot.Error, ESkillActivationError::InvalidSlot);
		const FSkillActivationOutcome OutOfRange = Loadout->TryActivateSkill(9);
		Test.TestEqual(TEXT("an out-of-range slot refuses with InvalidSlot"),
			OutOfRange.Error, ESkillActivationError::InvalidSlot);
	}
	Profile->RemoveFromRoot();
	return true;
}

// -- 2: WrongProfession / MissingAttack / MissingAsset -------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_003ConfigRefusals,
	"UEMMO.Tasks.M7_003.ConfigRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_003ConfigRefusals::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	UAttackCatalog* Attacks = NewObject<UAttackCatalog>();
	FText CatalogError;
	if (!Test.TestTrue(TEXT("fixture: the attack catalog builds"),
		Attacks->BuildFromDefinitions(MakeAttackDefinitions(GetTransientPackage()), CatalogError)))
	{
		return false;
	}
	FProfessionCatalog CatalogStorage;
	const FProfessionCatalog* Catalog = MakeCatalog(Test, CatalogStorage);
	if (Catalog == nullptr)
	{
		return false;
	}
	UProfileSubsystem* Profile = MakeFighterProfile(Test, NewSubsystemOwner());
	UCombatComponent* Combat = MakeCombat(GetTransientPackage(), Attacks);
	USkillLoadoutComponent* Loadout = NewObject<USkillLoadoutComponent>(GetTransientPackage());
	Loadout->Initialize(Combat, Profile, Catalog, TEXT("fixture-revision"));
	Loadout->SetAttackCatalog(Attacks);

	// WrongProfession: the fixture loadout maps a monk skill into the
	// fighter's slot 3 (the config error the check exists for).
	{
		const FSkillActivationOutcome Out = Loadout->TryActivateSkill(3);
		Test.TestEqual(TEXT("a foreign-profession skill refuses with WrongProfession"),
			Out.Error, ESkillActivationError::WrongProfession);
	}

	// MissingAttack: the skill drives an attack the combat catalog lacks.
	{
		FString Problem;
		Test.TestTrue(TEXT("fixture: the badattack rank is free"),
			Profile->TryPurchaseSkillRank(FName(TEXT("badattack_s")), 1, 2, 1, 0, Problem));
		const FSkillActivationOutcome Out = Loadout->TryActivateSkill(6);
		Test.TestEqual(TEXT("an unwired attack refuses with MissingAttack"),
			Out.Error, ESkillActivationError::MissingAttack);
	}

	// MissingAsset: the pending-attack skill refuses before any combat call.
	{
		FString Problem;
		Test.TestTrue(TEXT("fixture: the pending rank is free"),
			Profile->TryPurchaseSkillRank(FName(TEXT("pending_s")), 1, 1, 1, 0, Problem));
		const FSkillActivationOutcome Out = Loadout->TryActivateSkill(4);
		Test.TestEqual(TEXT("a pending-attack skill refuses with MissingAsset"),
			Out.Error, ESkillActivationError::MissingAsset);
	}
	Profile->RemoveFromRoot();
	return true;
}

// -- 3: the launcher economy (precheck, payment on the started attack) --------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_003LauncherEconomy,
	"UEMMO.Tasks.M7_003.LauncherEconomy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_003LauncherEconomy::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	UAttackCatalog* Attacks = NewObject<UAttackCatalog>();
	FText CatalogError;
	if (!Test.TestTrue(TEXT("fixture: the attack catalog builds"),
		Attacks->BuildFromDefinitions(MakeAttackDefinitions(GetTransientPackage()), CatalogError)))
	{
		return false;
	}
	FProfessionCatalog CatalogStorage;
	const FProfessionCatalog* Catalog = MakeCatalog(Test, CatalogStorage);
	if (Catalog == nullptr)
	{
		return false;
	}
	UProfileSubsystem* Profile = MakeFighterProfile(Test, NewSubsystemOwner());
	UCombatComponent* Combat = MakeCombat(GetTransientPackage(), Attacks);
	USkillLoadoutComponent* Loadout = NewObject<USkillLoadoutComponent>(GetTransientPackage());
	Loadout->Initialize(Combat, Profile, Catalog, TEXT("fixture-revision"));
	Loadout->SetAttackCatalog(Attacks);
	Loadout->RefreshMaxMPFromProfile();
	Loadout->RestoreFullMP();

	// The Z precheck accepts (launcher_z rank 1 learned via starting ranks).
	const FSkillActivationOutcome Pre = Loadout->PrecheckLauncherSkill();
	Test.TestEqual(TEXT("the launcher precheck accepts"), Pre.Error, ESkillActivationError::None);
	Test.TestEqual(TEXT("the precheck names the launcher skill"),
		Pre.SkillId, FName(TEXT("launcher_z")));

	// Payment on the started launcher attack (MP + cooldown).
	Loadout->CommitLauncherPayment(FName(TEXT("launcher")));
	Test.TestEqual(TEXT("the launcher payment charged MP"), Loadout->GetCurrentMP(), 105);
	Test.TestEqual(TEXT("the launcher cooldown is active"),
		Loadout->IsOnCooldown(FName(TEXT("launcher_z"))), true);

	// A legacy light attack start pays NOTHING (the pre-M7 chain stays free).
	Loadout->CommitLauncherPayment(FName(TEXT("light_01")));
	Test.TestEqual(TEXT("a non-launcher attack start pays nothing"), Loadout->GetCurrentMP(), 105);

	// The cooldown gates the NEXT Z press (the input layer's refusal path).
	const FSkillActivationOutcome After = Loadout->PrecheckLauncherSkill();
	Test.TestEqual(TEXT("the cooldown gate refuses the next press"),
		After.Error, ESkillActivationError::Cooldown);

	// An empty pool floors the payment at 0 (the chain never breaks).
	Loadout->SetCurrentMP(0);
	Loadout->CommitLauncherPayment(FName(TEXT("launcher")));
	Test.TestEqual(TEXT("the payment floors at zero"), Loadout->GetCurrentMP(), 0);
	Profile->RemoveFromRoot();
	return true;
}

// -- 4: the damage-scale hook (per-rank coefficient, consumed at start) --------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_003DamageScaleHook,
	"UEMMO.Tasks.M7_003.DamageScaleHook",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_003DamageScaleHook::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	UAttackCatalog* Attacks = NewObject<UAttackCatalog>();
	FText CatalogError;
	if (!Test.TestTrue(TEXT("fixture: the attack catalog builds"),
		Attacks->BuildFromDefinitions(MakeAttackDefinitions(GetTransientPackage()), CatalogError)))
	{
		return false;
	}
	UCombatComponent* Combat = MakeCombat(GetTransientPackage(), Attacks);

	// Legacy default: scale 1.0 (every pre-M7 hit byte-identical).
	Test.TestEqual(TEXT("the default scale is 1"), Combat->GetActiveDamageScale(), 1.0f);

	// The pending scale lands on the NEXT instance and is consumed.
	Combat->SetNextInstanceDamageScale(2.5f);
	Test.TestTrue(TEXT("the scaled attack starts"), Combat->TryStartAttack(FName(TEXT("singlekick")), 1));
	Test.TestEqual(TEXT("the instance carries the rank scale"), Combat->GetActiveDamageScale(), 2.5f);

	// A running-instance override (the launcher's OnStarted payment path).
	Combat->SetActiveInstanceDamageScale(1.5f);
	Test.TestEqual(TEXT("the running override applies"), Combat->GetActiveDamageScale(), 1.5f);
	Combat->SetActiveInstanceDamageScale(0.f); // invalid: ignored
	Test.TestEqual(TEXT("a non-positive override is ignored"), Combat->GetActiveDamageScale(), 1.5f);

	// The next legacy start is never rescaled.
	Combat->ResetCombat();
	Test.TestEqual(TEXT("the teardown resets the scale"), Combat->GetActiveDamageScale(), 1.0f);
	Combat->TryStartAttack(FName(TEXT("light_01")), 1);
	Test.TestEqual(TEXT("the legacy start keeps scale 1"), Combat->GetActiveDamageScale(), 1.0f);
	return true;
}

// -- 5: the SelfBuff policy (apply, expiry, no attack) --------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_003SelfBuffPolicy,
	"UEMMO.Tasks.M7_003.SelfBuffPolicy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_003SelfBuffPolicy::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	double Now = 500.0;
	UAttackCatalog* Attacks = NewObject<UAttackCatalog>();
	FText CatalogError;
	if (!Test.TestTrue(TEXT("fixture: the attack catalog builds"),
		Attacks->BuildFromDefinitions(MakeAttackDefinitions(GetTransientPackage()), CatalogError)))
	{
		return false;
	}
	FProfessionCatalog CatalogStorage;
	const FProfessionCatalog* Catalog = MakeCatalog(Test, CatalogStorage);
	if (Catalog == nullptr)
	{
		return false;
	}
	UProfileSubsystem* Profile = MakeFighterProfile(Test, NewSubsystemOwner());
	UCombatComponent* Combat = MakeCombat(GetTransientPackage(), Attacks);
	Combat->SetCombatStats(50.f, 10.f);
	USkillLoadoutComponent* Loadout = NewObject<USkillLoadoutComponent>(GetTransientPackage());
	Loadout->Initialize(Combat, Profile, Catalog, TEXT("fixture-revision"));
	Loadout->SetClockProvider([&Now]() { return Now; });
	Loadout->RefreshMaxMPFromProfile();
	Loadout->RestoreFullMP();

	FString Problem;
	Test.TestTrue(TEXT("the buff skill is purchasable"),
		Profile->TryPurchaseSkillRank(FName(TEXT("selfbuff_t")), 1, 1, 1, 0, Problem));

	// The buff never starts an attack; the combat stats rise by attack_bonus.
	const FSkillActivationOutcome Out = Loadout->TryActivateSkill(5);
	Test.TestEqual(TEXT("the buff activates"), Out.Error, ESkillActivationError::None);
	Test.TestEqual(TEXT("no attack started"), Combat->GetSnapshot().AttackId, FName(NAME_None));
	Test.TestEqual(TEXT("the attack power carries the bonus"), Combat->GetAttackPower(), 65.0f);
	Test.TestTrue(TEXT("the buff is active"), Loadout->HasActiveBuff());
	Test.TestEqual(TEXT("the buff paid MP"), Loadout->GetCurrentMP(), 105);

	// The expiry restores the pre-buff base.
	Now += 4.0;
	Loadout->TickSkillBuffs();
	Test.TestFalse(TEXT("the buff expired"), Loadout->HasActiveBuff());
	Test.TestEqual(TEXT("the base attack is restored"), Combat->GetAttackPower(), 50.0f);
	Profile->RemoveFromRoot();
	return true;
}

// -- 6: the purchase flow + the SP/MP profession resolvers ----------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_003PurchaseAndResolvers,
	"UEMMO.Tasks.M7_003.PurchaseAndResolvers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_003PurchaseAndResolvers::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	UProfileSubsystem* Profile = MakeFighterProfile(Test, NewSubsystemOwner());

	// The purchase happy path: rank 1 free (SP 20), rank 2 costs 10 (SP 10).
	FString Problem;
	Test.TestTrue(TEXT("rank 1 purchases"), Profile->TryPurchaseSkillRank(
		FName(TEXT("singlekick_w")), 1, 2, 1, 0, Problem));
	Test.TestTrue(TEXT("rank 2 purchases"), Profile->TryPurchaseSkillRank(
		FName(TEXT("singlekick_w")), 2, 2, 1, 10, Problem));
	Test.TestEqual(TEXT("the SP dropped by the rank-2 cost"), Profile->GetUnspentSP(), 10);
	Test.TestEqual(TEXT("the rank row holds rank 2"), Profile->GetSkillRank(FName(TEXT("singlekick_w"))), 2);

	// No re-purchase / no skipping / over-max.
	FString ReBuy;
	Test.TestFalse(TEXT("a re-purchase refuses"), Profile->TryPurchaseSkillRank(
		FName(TEXT("singlekick_w")), 2, 2, 1, 10, ReBuy));
	FString Skip;
	Test.TestFalse(TEXT("a rank skip refuses"), Profile->TryPurchaseSkillRank(
		FName(TEXT("singlekick_w")), 4, 2, 1, 0, Skip));

	// Spend down to 0 (badattack rank 2), then the SP shortage refuses.
	Test.TestTrue(TEXT("fixture spend: badattack rank 1"), Profile->TryPurchaseSkillRank(
		FName(TEXT("badattack_s")), 1, 2, 1, 0, Problem));
	Test.TestTrue(TEXT("fixture spend: badattack rank 2"), Profile->TryPurchaseSkillRank(
		FName(TEXT("badattack_s")), 2, 2, 1, 10, Problem));
	FString Broke;
	Test.TestFalse(TEXT("an SP shortage refuses"), Profile->TryPurchaseSkillRank(
		FName(TEXT("launcher_z")), 2, 2, 1, 10, Broke));
	Test.TestEqual(TEXT("the failed transaction changed nothing"), Profile->GetUnspentSP(), 0);

	// The resolvers: the fighter curve's MP pool and SP gains.
	UProfileSubsystem::SetProfessionProgressionResolvers(
		[](FName ProfessionId, int32 Level)
		{
			return ProfessionId == FName(TEXT("fighter"))
				? (Level == 1 ? 100 : 115) : 0;
		},
		[](FName ProfessionId, int32 LevelReached)
		{
			return (ProfessionId == FName(TEXT("fighter")) && LevelReached >= 2)
				? 3 : 0;
		});
	{
		// A fresh fighter profile at level 1 reads the level-1 pool.
		UProfileSubsystem* Fresh = NewObject<UProfileSubsystem>(NewSubsystemOwner());
		Fresh->AddToRoot();
		FProfileSnapshot FreshSnapshot;
		FreshSnapshot.CharacterId = FGuid(0x07A30000u, 2u, 2u, 2u);
		FreshSnapshot.Level = 1;
		FreshSnapshot.ProfessionId = FName(TEXT("fighter"));
		TArray<FSkillRankSaveEntry> Ranks;
		FSkillRankSaveEntry Launcher;
		Launcher.SkillId = FName(TEXT("launcher_z"));
		Launcher.Rank = 1;
		Ranks.Add(Launcher);
		Test.TestTrue(TEXT("fixture: the fresh profile restores"),
			Fresh->RestoreFromSave(FreshSnapshot, Fresh->GetInventory(), {}, {}, {}, {}, {}, false,
				FString(), {}, FName(TEXT("fighter")), Ranks, 0, {}, FString(), 0));
		Test.TestEqual(TEXT("the level-1 fighter pool is 100"),
			Fresh->GetProfileSnapshot().MaxMP, 100);

		// The SP accrual: one level-up (1 -> 2) grants the row-1 skill_points.
		Fresh->AddXP(100);
		Test.TestEqual(TEXT("the character leveled to 2"), Fresh->GetLevel(), 2);
		Test.TestEqual(TEXT("the level-up granted the row SP"), Fresh->GetUnspentSP(), 3);
		Fresh->RemoveFromRoot();
	}
	// The legacy prototype reads 0 MP (every pre-M7 number verbatim).
	{
		UProfileSubsystem* Legacy = NewObject<UProfileSubsystem>(NewSubsystemOwner());
		Legacy->AddToRoot();
		Legacy->NewProfile();
		Test.TestEqual(TEXT("the prototype pool is 0"), Legacy->GetProfileSnapshot().MaxMP, 0);
		Legacy->RemoveFromRoot();
	}
	Profile->RemoveFromRoot();
	return true;
}

// -- 7: the GroundArea data pins + the run-transient reset ----------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM7_003ResetAndDataPins,
	"UEMMO.Tasks.M7_003.ResetAndDataPins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM7_003ResetAndDataPins::RunTest(const FString& Parameters)
{
	FAutomationTestBase& Test = *this;
	UAttackCatalog* Attacks = NewObject<UAttackCatalog>();
	FText CatalogError;
	if (!Test.TestTrue(TEXT("fixture: the attack catalog builds"),
		Attacks->BuildFromDefinitions(MakeAttackDefinitions(GetTransientPackage()), CatalogError)))
	{
		return false;
	}
	FProfessionCatalog CatalogStorage;
	const FProfessionCatalog* Catalog = MakeCatalog(Test, CatalogStorage);
	if (Catalog == nullptr)
	{
		return false;
	}
	UProfileSubsystem* Profile = MakeFighterProfile(Test, NewSubsystemOwner());
	UCombatComponent* Combat = MakeCombat(GetTransientPackage(), Attacks);
	USkillLoadoutComponent* Loadout = NewObject<USkillLoadoutComponent>(GetTransientPackage());
	Loadout->Initialize(Combat, Profile, Catalog, TEXT("fixture-revision"));
	Loadout->SetAttackCatalog(Attacks);
	Loadout->RefreshMaxMPFromProfile();

	// The GroundArea data pins (the live box query is the M1-019-covered
	// path; these pin the DEPTH/HEIGHT semantics of the two area rows and
	// the low-kick band against the shipped combat-attacks.json).
	const FString AttacksPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Data/combat-attacks.json"));
	FString Raw;
	if (Test.TestTrue(TEXT("combat-attacks.json is readable"), FFileHelper::LoadFileToString(Raw, *AttacksPath)))
	{
		TSharedPtr<FCombatJsonValue> Root;
		FString ParseError;
		if (Test.TestTrue(TEXT("combat-attacks.json parses"),
			FCombatJsonParser::Parse(Raw, Root, ParseError) && Root.IsValid()))
		{
			const TSharedPtr<FCombatJsonValue>* Rows = Root->Object.Find(TEXT("attacks"));
			if (Test.TestTrue(TEXT("the attacks array exists"), Rows != nullptr)
				&& (*Rows)->Kind == FCombatJsonValue::EKind::Array)
			{
				auto FindRow = [Rows](const TCHAR* Id) -> TSharedPtr<FCombatJsonValue>
				{
					for (const TSharedPtr<FCombatJsonValue>& Row : (*Rows)->Array)
					{
						if (Row.IsValid() && Row->Kind == FCombatJsonValue::EKind::Object)
						{
							const TSharedPtr<FCombatJsonValue>* IdValue = Row->Object.Find(TEXT("attack_id"));
							if (IdValue != nullptr && (*IdValue)->Kind == FCombatJsonValue::EKind::String
								&& (*IdValue)->String == Id)
							{
								return Row;
							}
						}
					}
					return nullptr;
				};
				auto GetVec = [](const TSharedPtr<FCombatJsonValue>& Row, const TCHAR* Field) -> FVector
				{
					const TSharedPtr<FCombatJsonValue>* Vec = Row->Object.Find(Field);
					if (Vec == nullptr || (*Vec)->Kind != FCombatJsonValue::EKind::Array
						|| (*Vec)->Array.Num() != 3)
					{
						return FVector::ZeroVector;
					}
					return FVector((*Vec)->Array[0]->Number, (*Vec)->Array[1]->Number, (*Vec)->Array[2]->Number);
				};
				const TSharedPtr<FCombatJsonValue> Earthbreak = FindRow(TEXT("earthbreak"));
				const TSharedPtr<FCombatJsonValue> Stomp = FindRow(TEXT("stomp"));
				const TSharedPtr<FCombatJsonValue> LowKick = FindRow(TEXT("damagelowkick"));
				const TSharedPtr<FCombatJsonValue> Kick = FindRow(TEXT("singlekick"));
				if (Test.TestNotNull(TEXT("the earthbreak row exists"), Earthbreak.Get())
					&& Test.TestNotNull(TEXT("the stomp row exists"), Stomp.Get())
					&& Test.TestNotNull(TEXT("the damagelowkick row exists"), LowKick.Get())
					&& Test.TestNotNull(TEXT("the singlekick row exists"), Kick.Get()))
				{
					// The GroundArea strikes reach the Y depth band (>= 90 cm
					// half extent) - the depth walk is a real target area.
					Test.TestTrue(TEXT("earthbreak spans the depth band"),
						GetVec(Earthbreak, TEXT("hit_half_extent_cm")).Y >= 90.f);
					Test.TestTrue(TEXT("stomp spans the depth band"),
						GetVec(Stomp, TEXT("hit_half_extent_cm")).Y >= 90.f);
					// The low kick lives in a LOW band (the standing-miss box).
					Test.TestTrue(TEXT("the low kick is a low box"),
						GetVec(LowKick, TEXT("hit_offset_cm")).Z <= 40.f
						&& GetVec(LowKick, TEXT("hit_half_extent_cm")).Z <= 45.f);
					// The plain kick keeps the standing band (the contrast case).
					Test.TestTrue(TEXT("the plain kick is a standing box"),
						GetVec(Kick, TEXT("hit_offset_cm")).Z >= 60.f);
				}
			}
		}
	}

	// The skills table carries NO pending rows anymore (the five are wired).
	{
		const FString SkillsPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Data/Professions/skills.json"));
		FString SkillsRaw;
		if (Test.TestTrue(TEXT("skills.json is readable"), FFileHelper::LoadFileToString(SkillsRaw, *SkillsPath)))
		{
			TSharedPtr<FCombatJsonValue> SkillsRoot;
			FString SkillsError;
			if (Test.TestTrue(TEXT("skills.json parses"),
				FCombatJsonParser::Parse(SkillsRaw, SkillsRoot, SkillsError) && SkillsRoot.IsValid()))
			{
				const TSharedPtr<FCombatJsonValue>* Rows = SkillsRoot->Object.Find(TEXT("rows"));
				if (Test.TestTrue(TEXT("the skills rows array exists"), Rows != nullptr))
				{
					int32 Pending = 0;
					int32 FighterRows = 0;
					for (const TSharedPtr<FCombatJsonValue>& Row : (*Rows)->Array)
					{
						if (!Row.IsValid() || Row->Kind != FCombatJsonValue::EKind::Object)
						{
							continue;
						}
						const TSharedPtr<FCombatJsonValue>* PendingValue = Row->Object.Find(TEXT("attack_pending"));
						if (PendingValue != nullptr && (*PendingValue)->Kind == FCombatJsonValue::EKind::Boolean
							&& (*PendingValue)->Boolean)
						{
							++Pending;
						}
						const TSharedPtr<FCombatJsonValue>* Prof = Row->Object.Find(TEXT("profession_id"));
						if (Prof != nullptr && (*Prof)->Kind == FCombatJsonValue::EKind::String
							&& (*Prof)->String == TEXT("fighter"))
						{
							++FighterRows;
						}
					}
					Test.TestEqual(TEXT("no pending attack rows remain"), Pending, 0);
					Test.TestEqual(TEXT("the fighter declares exactly six skills"), FighterRows, 6);
				}
			}
		}
	}

	// The run-transient reset: cooldowns clear, MP refills (the room-start
	// /load semantics; the level-transition carrying is M7-004's run state).
	{
		Loadout->SetCurrentMP(7);
		Loadout->CommitLauncherPayment(FName(TEXT("launcher")));
		Test.TestTrue(TEXT("setup: a cooldown is active"),
			Loadout->IsOnCooldown(FName(TEXT("launcher_z"))));
		Loadout->ResetTransientCombatState();
		Test.TestFalse(TEXT("the reset clears cooldowns"),
			Loadout->IsOnCooldown(FName(TEXT("launcher_z"))));
		Test.TestEqual(TEXT("the reset refills the MP pool (115)"), Loadout->GetCurrentMP(), 115);
	}
	Profile->RemoveFromRoot();
	return true;
}
#endif
