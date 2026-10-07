#include "TestRoomConfigDriver.h"

#include "../../Enemy/EnemyDefinition.h"
#include "../../Enemy/MeleeEnemy.h"
#include "../HealthComponent.h"
#include "../CombatComponent.h"
#include "ReactionResolver.h"
#include "../Data/CombatDataTableParser.h"
#include "../Data/CombatDataAssetBuilder.h"

#include "Components/CapsuleComponent.h"
#include "Engine/World.h"

namespace
{
	// M5-018A: the enemy definition and target reaction policy assets follow
	// the M5-008/M2 naming conventions (DA_<Id>.<Id> under their folders).
	const TCHAR* const EnemyDefinitionFolder = TEXT("/Game/UEMMO/Enemy/Definitions/DA_");
	const TCHAR* const TargetReactionFolder = TEXT("/Game/UEMMO/Combat/Data/DA_Target_");

	// The id syntax is the shared combat-config rule (a-z, 0-9, _, 1..64).
	bool IsValidTestRoomId(const FString& Id)
	{
		return IsValidDefinitionIdSyntax(Id);
	}

	const FCombatJsonValue* FindMember(const FCombatJsonValue& Object, const TCHAR* Name)
	{
		if (Object.Kind != FCombatJsonValue::EKind::Object)
		{
			return nullptr;
		}
		const TSharedPtr<FCombatJsonValue>* Found = Object.Object.Find(Name);
		return Found ? Found->Get() : nullptr;
	}

	bool ReadString(const FCombatJsonValue& Object, const TCHAR* Name, FString& Out, FString& OutError)
	{
		const FCombatJsonValue* Value = FindMember(Object, Name);
		if (Value == nullptr || Value->Kind != FCombatJsonValue::EKind::String)
		{
			OutError = FString::Printf(TEXT("field '%s' must be a string"), Name);
			return false;
		}
		Out = Value->String;
		return true;
	}

	bool ReadNumber(const FCombatJsonValue& Object, const TCHAR* Name, double& Out, FString& OutError)
	{
		const FCombatJsonValue* Value = FindMember(Object, Name);
		if (Value == nullptr || Value->Kind != FCombatJsonValue::EKind::Number)
		{
			OutError = FString::Printf(TEXT("field '%s' must be a number"), Name);
			return false;
		}
		Out = Value->Number;
		return true;
	}
}

ACombatTestRoomDriver::ACombatTestRoomDriver()
{
	PrimaryActorTick.bCanEverTick = false;
}

bool ACombatTestRoomDriver::ParseTestRoomConfig(const FString& JsonText, FTestRoomConfig& OutConfig, FString& OutError)
{
	OutConfig = FTestRoomConfig();
	TSharedPtr<FCombatJsonValue> Root;
	if (!FCombatJsonParser::Parse(JsonText, Root, OutError) || !Root.IsValid()
		|| Root->Kind != FCombatJsonValue::EKind::Object)
	{
		OutError = OutError.IsEmpty() ? TEXT("the source text is not a JSON object") : OutError;
		return false;
	}

	FString SchemaText;
	double SchemaVersion = 0.0;
	if (!ReadNumber(*Root, TEXT("schema_version"), SchemaVersion, OutError)
		|| static_cast<int32>(SchemaVersion) != 1)
	{
		OutError = FString::Printf(TEXT("schema_version must be 1 (got %s)"), *OutError);
		return false;
	}
	FString TableText;
	if (!ReadString(*Root, TEXT("table"), TableText, OutError) || TableText != TEXT("test_room"))
	{
		OutError = FString::Printf(TEXT("table must be \"test_room\" (got %s)"), *OutError);
		return false;
	}

	const FCombatJsonValue* Rows = FindMember(*Root, TEXT("rows"));
	if (Rows == nullptr || Rows->Kind != FCombatJsonValue::EKind::Array || Rows->Array.Num() != 1)
	{
		OutError = TEXT("rows must hold exactly one test room row");
		return false;
	}
	const FCombatJsonValue& Row = *Rows->Array[0];
	if (Row.Kind != FCombatJsonValue::EKind::Object)
	{
		OutError = TEXT("rows[0] must be an object");
		return false;
	}

	FString RoomIdText;
	if (!ReadString(Row, TEXT("room_id"), RoomIdText, OutError) || !IsValidTestRoomId(RoomIdText))
	{
		OutError = FString::Printf(TEXT("room_id must be 1..64 characters of a-z, 0-9 and _ (parse: %s)"), *OutError);
		return false;
	}
	OutConfig.RoomId = FName(*RoomIdText);

	FString RewardText;
	if (FindMember(Row, TEXT("reward_pool_id")) != nullptr)
	{
		if (!ReadString(Row, TEXT("reward_pool_id"), RewardText, OutError) || !IsValidTestRoomId(RewardText))
		{
			OutError = FString::Printf(TEXT("reward_pool_id must be a valid id (parse: %s)"), *OutError);
			return false;
		}
		OutConfig.RewardPoolId = FName(*RewardText);
	}

	// The future vehicle list: legal only while empty. A non-empty list
	// refuses with the explicit not-implemented error (no stand-in spawn,
	// no silent ignore - the capability arrives with the vehicle segment).
	const FCombatJsonValue* Vehicles = FindMember(Row, TEXT("vehicle_spawns"));
	if (Vehicles != nullptr)
	{
		if (Vehicles->Kind != FCombatJsonValue::EKind::Array)
		{
			OutError = TEXT("vehicle_spawns must be an array");
			return false;
		}
		if (Vehicles->Array.Num() > 0)
		{
			OutError = TEXT("vehicle_spawns is not implemented yet (the vehicle segment M5-037+ owns the capability); the list must stay empty");
			return false;
		}
	}

	const FCombatJsonValue* Targets = FindMember(Row, TEXT("targets"));
	if (Targets == nullptr || Targets->Kind != FCombatJsonValue::EKind::Array || Targets->Array.Num() < 1)
	{
		OutError = TEXT("targets must hold at least one target row");
		return false;
	}
	for (int32 Index = 0; Index < Targets->Array.Num(); ++Index)
	{
		const FCombatJsonValue& TargetRow = *Targets->Array[Index];
		if (TargetRow.Kind != FCombatJsonValue::EKind::Object)
		{
			OutError = FString::Printf(TEXT("targets[%d] must be an object"), Index);
			return false;
		}
		FTestRoomTargetConfig Target;
		FString TargetIdText;
		FString EnemyIdText;
		FString PolicyText;
		double MaxHealth = 0.0;
		if (!ReadString(TargetRow, TEXT("target_id"), TargetIdText, OutError)
			|| !IsValidTestRoomId(TargetIdText))
		{
			OutError = FString::Printf(TEXT("targets[%d].target_id must be a valid id (parse: %s)"), Index, *OutError);
			return false;
		}
		Target.TargetId = FName(*TargetIdText);
		if (!ReadString(TargetRow, TEXT("enemy_id"), EnemyIdText, OutError)
			|| !IsValidTestRoomId(EnemyIdText))
		{
			OutError = FString::Printf(TEXT("targets[%d].enemy_id must be a valid id (parse: %s)"), Index, *OutError);
			return false;
		}
		Target.EnemyId = FName(*EnemyIdText);
		if (!ReadNumber(TargetRow, TEXT("max_health"), MaxHealth, OutError)
			|| !FMath::IsFinite(MaxHealth) || MaxHealth <= 0.0)
		{
			OutError = FString::Printf(TEXT("targets[%d].max_health must be a finite number greater than 0 (parse: %s)"), Index, *OutError);
			return false;
		}
		Target.MaxHealth = static_cast<float>(MaxHealth);
		if (!ReadString(TargetRow, TEXT("policy_id"), PolicyText, OutError)
			|| !IsValidTestRoomId(PolicyText))
		{
			OutError = FString::Printf(TEXT("targets[%d].policy_id must be a valid id (parse: %s)"), Index, *OutError);
			return false;
		}
		Target.PolicyId = FName(*PolicyText);
		const FCombatJsonValue* Location = FindMember(TargetRow, TEXT("location_cm"));
		if (Location == nullptr || Location->Kind != FCombatJsonValue::EKind::Object)
		{
			OutError = FString::Printf(TEXT("targets[%d].location_cm must be an object"), Index);
			return false;
		}
		double X = 0.0;
		double Y = 0.0;
		double Z = 0.0;
		if (!ReadNumber(*Location, TEXT("x"), X, OutError)
			|| !ReadNumber(*Location, TEXT("y"), Y, OutError)
			|| !ReadNumber(*Location, TEXT("z"), Z, OutError))
		{
			OutError = FString::Printf(TEXT("targets[%d].location_cm must carry numeric x/y/z (parse: %s)"), Index, *OutError);
			return false;
		}
		Target.LocationCm = FVector(X, Y, Z);
		OutConfig.Targets.Add(Target);
	}
	return true;
}

bool ACombatTestRoomDriver::LoadConfig(FString& OutError)
{
	const FString AbsolutePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / ConfigJsonPath);
	if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*AbsolutePath))
	{
		OutError = FString::Printf(TEXT("the test room config file does not exist: %s"), *AbsolutePath);
		return false;
	}
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *AbsolutePath))
	{
		OutError = FString::Printf(TEXT("the test room config file could not be read: %s"), *AbsolutePath);
		return false;
	}
	return ParseTestRoomConfig(JsonText, Config, OutError);
}

int32 ACombatTestRoomDriver::SpawnTargets(UWorld& World, TArray<FName>& OutMissingIds)
{
	OutMissingIds.Reset();
	int32 Spawned = 0;
	for (const FTestRoomTargetConfig& Target : Config.Targets)
	{
		// The enemy definition resolves by business id; a missing id fails
		// explicitly (the target is named, nothing is spawned, no stand-in).
		const FString DefinitionPath = FString(EnemyDefinitionFolder) + Target.EnemyId.ToString() + ".DA_" + Target.EnemyId.ToString();
		UEnemyDefinition* Definition = LoadObject<UEnemyDefinition>(nullptr, *DefinitionPath);
		if (Definition == nullptr)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("UEMMO ACombatTestRoomDriver: target '%s' references unknown enemy id '%s' (looked up %s); the target is not spawned."),
				*Target.TargetId.ToString(), *Target.EnemyId.ToString(), *DefinitionPath);
			OutMissingIds.Add(Target.TargetId);
			continue;
		}
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
		AMeleeEnemy* Enemy = World.SpawnActor<AMeleeEnemy>(
			AMeleeEnemy::StaticClass(), Target.LocationCm, FRotator::ZeroRotator, Params);
		if (Enemy == nullptr)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("UEMMO ACombatTestRoomDriver: target '%s' could not spawn its enemy at %s."),
				*Target.TargetId.ToString(), *Target.LocationCm.ToCompactString());
			OutMissingIds.Add(Target.TargetId);
			continue;
		}
		Enemy->SetEnemyDefinition(Definition);
		// The configured values ride on top of the definition: health and the
		// reaction policy come from the test_room table (change a value,
		// reload the map - the room follows the config, not the class).
		if (UHealthComponent* Health = Enemy->GetHealthComponent())
		{
			Health->SetMaxHealth(Target.MaxHealth);
			Health->ResetHealth();
		}
		const FString PolicyPath = FString(TargetReactionFolder) + Target.PolicyId.ToString() + ".DA_Target_" + Target.PolicyId.ToString();
		if (UCombatTargetReactionAsset* PolicyAsset = LoadObject<UCombatTargetReactionAsset>(nullptr, *PolicyPath))
		{
			FTargetReaction Policy;
			Policy.PolicyId = PolicyAsset->PolicyId;
			Policy.bAllowStagger = PolicyAsset->bAllowStagger;
			Policy.bAllowLaunch = PolicyAsset->bAllowLaunch;
			Policy.bAllowKnockdown = PolicyAsset->bAllowKnockdown;
			Policy.MaxLaunchesPerAirCycle = PolicyAsset->MaxLaunchesPerAirCycle;
			Policy.LaunchZScales = PolicyAsset->LaunchZScales;
			Policy.MaxAirTimeSeconds = PolicyAsset->MaxAirTimeSeconds;
			Policy.PoiseMax = PolicyAsset->PoiseMax;
			Policy.PoiseRegenSeconds = PolicyAsset->PoiseRegenSeconds;
			Policy.KnockdownSeconds = PolicyAsset->KnockdownSeconds;
			Policy.RecoveringSeconds = PolicyAsset->RecoveringSeconds;
			Policy.bImmuneDamage = PolicyAsset->bImmuneDamage;
			Policy.bImmuneControl = PolicyAsset->bImmuneControl;
			Policy.bDeathResistant = PolicyAsset->bDeathResistant;
			if (UCombatComponent* Combat = Enemy->GetCombatComponent())
			{
				Combat->SetTargetReactionPolicy(Policy);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning,
				TEXT("UEMMO ACombatTestRoomDriver: target '%s' references unknown policy id '%s' (looked up %s); the target keeps the legacy default policy."),
				*Target.TargetId.ToString(), *Target.PolicyId.ToString(), *PolicyPath);
			OutMissingIds.Add(Target.TargetId);
		}
		++Spawned;
	}
	return Spawned;
}

bool ACombatTestRoomDriver::SpawnVehicles(UWorld& World, FString& OutError)
{
	// M5-037+ frozen seam: the capability does not exist yet - the refusal is
	// explicit, never a stand-in actor.
	OutError = TEXT("vehicle spawning is not implemented yet (the vehicle segment M5-037+ owns the capability)");
	return false;
}
