#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/SoftObjectPath.h"

#include "../Combat/AttackDefinition.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UE::UEMMO::Tasks::M1_009
{
	// Expected values mirror Data/combat-attacks.json (schema_version 1) from
	// M1-008. The game runtime never reads that developer-machine JSON, so the
	// committed contract values below are what the four generated
	// /Game/UEMMO/Combat/Definitions/DA_* assets must reproduce field by field.
	struct FExpectedAttack
	{
		const TCHAR* ObjectPath;
		const TCHAR* AttackId;
		int32 DurationFrames;
		int32 ActiveStartFrame;
		int32 ActiveEndFrame;
		int32 CancelStartFrame;
		int32 CancelEndFrame;
		const TCHAR* const* NextAttackIds;
		int32 NextAttackIdCount;
		float BaseDamage;
		float AttackCoefficient;
		FVector HitOffsetFromFeet;
		FVector HitHalfExtent;
		float KnockbackSpeed;
		float LaunchSpeed;
		float HitStunSeconds;
		float HitStopSeconds;
		const TCHAR* AnimationPath;
		float ClipStartSeconds;
		float ClipEndSeconds;
		bool bPlaceholderAnimation;
	};

	// next_attack_ids keep JSON order; function-local static const arrays
	// avoid FNames being built before the name pool exists.
	static FExpectedAttack MakeLight01()
	{
		static const TCHAR* const Next[] = { TEXT("light_02"), TEXT("launcher"), nullptr };
		FExpectedAttack Expected;
		Expected.ObjectPath = TEXT("/Game/UEMMO/Combat/Definitions/DA_light_01.DA_light_01");
		Expected.AttackId = TEXT("light_01");
		Expected.DurationFrames = 26;
		Expected.ActiveStartFrame = 7;
		Expected.ActiveEndFrame = 11;
		Expected.CancelStartFrame = 12;
		Expected.CancelEndFrame = 24;
		Expected.NextAttackIds = Next;
		Expected.NextAttackIdCount = 2;
		Expected.BaseDamage = 10.0f;
		Expected.AttackCoefficient = 1.0f;
		Expected.HitOffsetFromFeet = FVector(95.0f, 0.0f, 90.0f);
		Expected.HitHalfExtent = FVector(85.0f, 50.0f, 70.0f);
		Expected.KnockbackSpeed = 90.0f;
		Expected.LaunchSpeed = 0.0f;
		Expected.HitStunSeconds = 0.22f;
		Expected.HitStopSeconds = 0.04f;
		Expected.AnimationPath = TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Attack/MM_Attack_01");
		Expected.ClipStartSeconds = 0.0f;
		Expected.ClipEndSeconds = 1.0f;
		Expected.bPlaceholderAnimation = false;
		return Expected;
	}

	static FExpectedAttack MakeLight02()
	{
		static const TCHAR* const Next[] = { TEXT("launcher"), nullptr };
		FExpectedAttack Expected;
		Expected.ObjectPath = TEXT("/Game/UEMMO/Combat/Definitions/DA_light_02.DA_light_02");
		Expected.AttackId = TEXT("light_02");
		Expected.DurationFrames = 32;
		Expected.ActiveStartFrame = 9;
		Expected.ActiveEndFrame = 14;
		Expected.CancelStartFrame = 16;
		Expected.CancelEndFrame = 29;
		Expected.NextAttackIds = Next;
		Expected.NextAttackIdCount = 1;
		Expected.BaseDamage = 14.0f;
		Expected.AttackCoefficient = 1.0f;
		Expected.HitOffsetFromFeet = FVector(95.0f, 0.0f, 90.0f);
		Expected.HitHalfExtent = FVector(85.0f, 50.0f, 70.0f);
		Expected.KnockbackSpeed = 120.0f;
		Expected.LaunchSpeed = 0.0f;
		Expected.HitStunSeconds = 0.28f;
		Expected.HitStopSeconds = 0.04f;
		Expected.AnimationPath = TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Attack/MM_Attack_02");
		Expected.ClipStartSeconds = 0.0f;
		Expected.ClipEndSeconds = 1.0f;
		Expected.bPlaceholderAnimation = false;
		return Expected;
	}

	static FExpectedAttack MakeLauncher()
	{
		static const TCHAR* const Next[] = { TEXT("aerial_01"), nullptr };
		FExpectedAttack Expected;
		Expected.ObjectPath = TEXT("/Game/UEMMO/Combat/Definitions/DA_launcher.DA_launcher");
		Expected.AttackId = TEXT("launcher");
		Expected.DurationFrames = 40;
		Expected.ActiveStartFrame = 12;
		Expected.ActiveEndFrame = 17;
		Expected.CancelStartFrame = 18;
		Expected.CancelEndFrame = 32;
		Expected.NextAttackIds = Next;
		Expected.NextAttackIdCount = 1;
		Expected.BaseDamage = 18.0f;
		Expected.AttackCoefficient = 1.0f;
		Expected.HitOffsetFromFeet = FVector(95.0f, 0.0f, 90.0f);
		Expected.HitHalfExtent = FVector(85.0f, 50.0f, 70.0f);
		Expected.KnockbackSpeed = 70.0f;
		Expected.LaunchSpeed = 700.0f;
		Expected.HitStunSeconds = 1.0f;
		Expected.HitStopSeconds = 0.04f;
		Expected.AnimationPath = TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Attack/MM_ChargedAttack");
		Expected.ClipStartSeconds = 0.0f;
		Expected.ClipEndSeconds = 1.8333333730697632f;
		Expected.bPlaceholderAnimation = true;
		return Expected;
	}

	static FExpectedAttack MakeAerial01()
	{
		static const TCHAR* const Next[] = { nullptr };
		FExpectedAttack Expected;
		Expected.ObjectPath = TEXT("/Game/UEMMO/Combat/Definitions/DA_aerial_01.DA_aerial_01");
		Expected.AttackId = TEXT("aerial_01");
		Expected.DurationFrames = 28;
		Expected.ActiveStartFrame = 6;
		Expected.ActiveEndFrame = 10;
		Expected.CancelStartFrame = 12;
		Expected.CancelEndFrame = 23;
		Expected.NextAttackIds = Next;
		Expected.NextAttackIdCount = 0;
		Expected.BaseDamage = 12.0f;
		Expected.AttackCoefficient = 1.0f;
		Expected.HitOffsetFromFeet = FVector(95.0f, 0.0f, 90.0f);
		Expected.HitHalfExtent = FVector(85.0f, 50.0f, 70.0f);
		Expected.KnockbackSpeed = 80.0f;
		Expected.LaunchSpeed = 60.0f;
		Expected.HitStunSeconds = 0.18f;
		Expected.HitStopSeconds = 0.04f;
		Expected.AnimationPath = TEXT("/Game/Characters/Mannequins/Anims/Unarmed/Attack/MM_Attack_03");
		Expected.ClipStartSeconds = 0.0f;
		Expected.ClipEndSeconds = 1.6666666269302368f;
		Expected.bPlaceholderAnimation = true;
		return Expected;
	}

	static int32 CountNext(const TCHAR* const* Next)
	{
		int32 Count = 0;
		while (Next[Count] != nullptr) { ++Count; }
		return Count;
	}

	// Compares every UAttackDefinition field against the JSON-mirrored
	// expectation. Arrays are compared element by element in JSON order and
	// vectors component by component. The caller reports the load failure and
	// passes null; this helper only runs when the asset exists.
	static void CheckAgainstJson(FAutomationTestBase& Test, const UAttackDefinition* Attack, const FExpectedAttack& Expected)
	{
		if (Attack == nullptr)
		{
			return;
		}
		Test.TestEqual(TEXT("AttackId matches the JSON attack_id"), Attack->AttackId, FName(Expected.AttackId));
		Test.TestEqual(TEXT("DurationFrames matches the JSON duration_frames"), Attack->DurationFrames, Expected.DurationFrames);
		Test.TestEqual(TEXT("ActiveWindow.StartFrame matches active_start_frame"), Attack->ActiveWindow.StartFrame, Expected.ActiveStartFrame);
		Test.TestEqual(TEXT("ActiveWindow.EndFrame matches active_end_frame"), Attack->ActiveWindow.EndFrame, Expected.ActiveEndFrame);
		Test.TestEqual(TEXT("CancelWindow.StartFrame matches cancel_start_frame"), Attack->CancelWindow.StartFrame, Expected.CancelStartFrame);
		Test.TestEqual(TEXT("CancelWindow.EndFrame matches cancel_end_frame"), Attack->CancelWindow.EndFrame, Expected.CancelEndFrame);

		if (Test.TestEqual(TEXT("AllowedNextAttacks count matches next_attack_ids"),
			Attack->AllowedNextAttacks.Num(), CountNext(Expected.NextAttackIds)))
		{
			for (int32 Index = 0; Index < Expected.NextAttackIdCount; ++Index)
			{
				Test.TestEqual(FString::Printf(TEXT("AllowedNextAttacks[%d] matches next_attack_ids order"), Index),
					Attack->AllowedNextAttacks[Index], FName(Expected.NextAttackIds[Index]));
			}
		}

		Test.TestEqual(TEXT("BaseDamage matches base_damage"), Attack->BaseDamage, Expected.BaseDamage);
		Test.TestEqual(TEXT("AttackCoefficient matches attack_coefficient"), Attack->AttackCoefficient, Expected.AttackCoefficient);
		Test.TestEqual(TEXT("HitOffsetFromFeet matches hit_offset_cm componentwise"), Attack->HitOffsetFromFeet, Expected.HitOffsetFromFeet);
		Test.TestEqual(TEXT("HitHalfExtent matches hit_half_extent_cm componentwise"), Attack->HitHalfExtent, Expected.HitHalfExtent);
		Test.TestEqual(TEXT("KnockbackSpeed matches knockback_cm_per_s"), Attack->KnockbackSpeed, Expected.KnockbackSpeed);
		Test.TestEqual(TEXT("LaunchSpeed matches launch_cm_per_s"), Attack->LaunchSpeed, Expected.LaunchSpeed);
		Test.TestEqual(TEXT("HitStunSeconds matches hit_stun_seconds"), Attack->HitStunSeconds, Expected.HitStunSeconds);
		Test.TestEqual(TEXT("HitStopSeconds matches hit_stop_seconds"), Attack->HitStopSeconds, Expected.HitStopSeconds);
		// The soft reference must match the JSON animation_path. UE 5.8 stores a
	// reference assigned from a resolved object as FTopLevelAssetPath(package,
	// asset), whose ToString() renders "path.Asset"; the stored package path
	// is exactly the JSON string, the sub path is empty (asset itself, not a
	// subobject), and both the stored and the JSON forms resolve to the same
	// UAnimSequence.
	const FSoftObjectPath StoredAnimation = Attack->Animation.ToSoftObjectPath();
	const FSoftObjectPath ExpectedAnimation(Expected.AnimationPath);
	Test.TestEqual(TEXT("Animation soft reference package path matches animation_path"),
		StoredAnimation.GetAssetPath().GetPackageName().ToString(), FString(Expected.AnimationPath));
	Test.TestTrue(TEXT("Animation soft reference points at the asset itself (empty sub path)"),
		StoredAnimation.GetSubPathString().IsEmpty());
	UAnimSequence* StoredResolved = Cast<UAnimSequence>(StoredAnimation.TryLoad());
	UAnimSequence* ExpectedResolved = Cast<UAnimSequence>(ExpectedAnimation.TryLoad());
	if (Test.TestTrue(TEXT("stored animation soft reference resolves to an asset"), StoredResolved != nullptr) &&
		Test.TestTrue(TEXT("JSON animation_path resolves to an asset"), ExpectedResolved != nullptr))
	{
		Test.TestEqual(TEXT("stored soft reference and animation_path resolve to the same UAnimSequence"),
			StoredResolved, ExpectedResolved);
	}
		Test.TestEqual(TEXT("ClipStartSeconds matches clip_start_seconds"), Attack->ClipStartSeconds, Expected.ClipStartSeconds);
		Test.TestEqual(TEXT("ClipEndSeconds matches clip_end_seconds"), Attack->ClipEndSeconds, Expected.ClipEndSeconds);
		if (Expected.bPlaceholderAnimation)
		{
			Test.TestTrue(TEXT("bPlaceholderAnimation matches placeholder_animation (true)"), Attack->bPlaceholderAnimation);
		}
		else
		{
			Test.TestFalse(TEXT("bPlaceholderAnimation matches placeholder_animation (false)"), Attack->bPlaceholderAnimation);
		}
	}

	static const TCHAR* const AllAssetObjectPaths[] =
	{
		TEXT("/Game/UEMMO/Combat/Definitions/DA_light_01.DA_light_01"),
		TEXT("/Game/UEMMO/Combat/Definitions/DA_light_02.DA_light_02"),
		TEXT("/Game/UEMMO/Combat/Definitions/DA_launcher.DA_launcher"),
		TEXT("/Game/UEMMO/Combat/Definitions/DA_aerial_01.DA_aerial_01")
	};

	static const TCHAR* const AllAssetLabels[] =
	{
		TEXT("DA_light_01"),
		TEXT("DA_light_02"),
		TEXT("DA_launcher"),
		TEXT("DA_aerial_01")
	};
}

using namespace UE::UEMMO::Tasks::M1_009;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_009AllFourGeneratedAssetsLoad,
	"UEMMO.Tasks.M1_009.AllFourGeneratedAssetsLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_009AllFourGeneratedAssetsLoad::RunTest(const FString& Parameters)
{
	for (int32 Index = 0; Index < 4; ++Index)
	{
		UAttackDefinition* Attack = LoadObject<UAttackDefinition>(nullptr, AllAssetObjectPaths[Index]);
		TestNotNull(FString::Printf(TEXT("%s loads as UAttackDefinition"), AllAssetLabels[Index]), Attack);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_009Light01AssetMatchesJson,
	"UEMMO.Tasks.M1_009.Light01AssetMatchesJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_009Light01AssetMatchesJson::RunTest(const FString& Parameters)
{
	const FExpectedAttack Expected = MakeLight01();
	UAttackDefinition* Attack = LoadObject<UAttackDefinition>(nullptr, Expected.ObjectPath);
	if (!TestNotNull(TEXT("DA_light_01 loads in a fresh game process"), Attack))
	{
		return true;
	}
	CheckAgainstJson(*this, Attack, Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_009Light02AssetMatchesJson,
	"UEMMO.Tasks.M1_009.Light02AssetMatchesJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_009Light02AssetMatchesJson::RunTest(const FString& Parameters)
{
	const FExpectedAttack Expected = MakeLight02();
	UAttackDefinition* Attack = LoadObject<UAttackDefinition>(nullptr, Expected.ObjectPath);
	if (!TestNotNull(TEXT("DA_light_02 loads in a fresh game process"), Attack))
	{
		return true;
	}
	CheckAgainstJson(*this, Attack, Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_009LauncherAssetMatchesJson,
	"UEMMO.Tasks.M1_009.LauncherAssetMatchesJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_009LauncherAssetMatchesJson::RunTest(const FString& Parameters)
{
	const FExpectedAttack Expected = MakeLauncher();
	UAttackDefinition* Attack = LoadObject<UAttackDefinition>(nullptr, Expected.ObjectPath);
	if (!TestNotNull(TEXT("DA_launcher loads in a fresh game process"), Attack))
	{
		return true;
	}
	CheckAgainstJson(*this, Attack, Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUEMMOTasksM1_009Aerial01AssetMatchesJson,
	"UEMMO.Tasks.M1_009.Aerial01AssetMatchesJson",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FUEMMOTasksM1_009Aerial01AssetMatchesJson::RunTest(const FString& Parameters)
{
	const FExpectedAttack Expected = MakeAerial01();
	UAttackDefinition* Attack = LoadObject<UAttackDefinition>(nullptr, Expected.ObjectPath);
	if (!TestNotNull(TEXT("DA_aerial_01 loads in a fresh game process"), Attack))
	{
		return true;
	}
	CheckAgainstJson(*this, Attack, Expected);
	return true;
}

#endif
