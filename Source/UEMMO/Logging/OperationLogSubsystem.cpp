// M3-023: operation log subsystem (user-requested 2026-09-30). GREEN
// implementation of the header contract:
// - every Log call appends one "[HH:MM:SS.mmm][Category] Message" line to
//   ProjectSavedDir/OperationLogs/OperationLog.log in its own write call
//   (crash safety: a process death can lose at most the line being written),
// - every write first runs the daily-rollover gate: when the injectable
//   provider's date differs from ActiveLogDate (or no day is open yet) the
//   file is deleted and reopened with a fresh "=== Operation log started
//   YYYY-MM-DD (build) ===" header (the user's cross-day clear requirement),
// - RegisterPlayer binds the player's CombatComponent Started/Finished/
//   HitConfirmed and the world's RoomSessionSubsystem OnRunStarted/OnRunEnded
//   through weak lambdas with remembered handles.

#include "OperationLogSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Logging/LogMacros.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "../Combat/CombatComponent.h"
#include "../Combat/CombatHitTypes.h"
#include "../PrototypeCharacter.h"
#include "../Room/RoomResult.h"
#include "../Room/RoomSessionSubsystem.h"

namespace
{
	// The three fixed categories of the card plus the file layout. Every
	// anonymous-namespace symbol carries the M3_023 prefix so the per-file
	// translation unit can never collide with another module TU.
	const FName M3_023_CategoryInput(TEXT("Input"));
	const FName M3_023_CategoryState(TEXT("State"));
	const FName M3_023_CategoryResponse(TEXT("Response"));
	const TCHAR* M3_023_LogDirectory = TEXT("OperationLogs");
	const TCHAR* M3_023_LogFileName = TEXT("OperationLog.log");
}

void UOperationLogSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	LogFilePath = FPaths::Combine(FPaths::ProjectSavedDir(), M3_023_LogDirectory, M3_023_LogFileName);
	// No day is open yet: the first write opens the current (provider) day.
	ActiveLogDate = FDateTime();
}

void UOperationLogSubsystem::Deinitialize()
{
	// World teardown drops the delegates with their owners anyway; the explicit
	// unbind only keeps the handles tidy for a still-alive world.
	M3_023_UnbindAll();
	NowProvider = nullptr;
	Super::Deinitialize();
}

UOperationLogSubsystem* UOperationLogSubsystem::FindForContext(const UObject* Context)
{
	if (Context == nullptr)
	{
		return nullptr;
	}
	// 1) Outer chain: game instance subsystems (and everything created below
	//    them, e.g. UObject services owned by a subsystem) resolve directly.
	UObject* MutableContext = const_cast<UObject*>(Context);
	for (UObject* Outer = MutableContext->GetOuter(); Outer != nullptr; Outer = Outer->GetOuter())
	{
		if (UGameInstance* GameInstance = Cast<UGameInstance>(Outer))
		{
			return GameInstance->GetSubsystem<UOperationLogSubsystem>();
		}
	}
	// 2) World path: actors, components and world subsystems reach the game
	//    instance through their world. A world-less context (bare test
	//    objects, CDOs) stops here and the caller skips the log.
	if (UWorld* World = MutableContext->GetWorld())
	{
		if (UGameInstance* GameInstance = World->GetGameInstance())
		{
			return GameInstance->GetSubsystem<UOperationLogSubsystem>();
		}
	}
	return nullptr;
}

void UOperationLogSubsystem::SetNowProvider(TFunction<FDateTime()> InProvider)
{
	NowProvider = MoveTempIfPossible(InProvider);
}

void UOperationLogSubsystem::ResetNowProvider()
{
	NowProvider = nullptr;
}

void UOperationLogSubsystem::Log(FName Category, const FString& Message)
{
	// The now source first (the provider decides which day this write belongs
	// to; default is the local wall clock, matching the user's calendar).
	const FDateTime Now = NowProvider ? NowProvider() : FDateTime::Now();
	M3_023_EnsureDailyFile(Now);
	const FString Line = FString::Printf(TEXT("[%02d:%02d:%02d.%03d][%s] %s"),
		Now.GetHour(), Now.GetMinute(), Now.GetSecond(), Now.GetMillisecond(),
		*Category.ToString(), *Message);
	M3_023_AppendLine(Line);
}

void UOperationLogSubsystem::LogInput(const FString& Message)
{
	Log(M3_023_CategoryInput, Message);
}

void UOperationLogSubsystem::LogState(const FString& Message)
{
	Log(M3_023_CategoryState, Message);
}

void UOperationLogSubsystem::LogResponse(const FString& Message)
{
	Log(M3_023_CategoryResponse, Message);
}

bool UOperationLogSubsystem::RegisterPlayer(APrototypeCharacter* Player)
{
	if (Player == nullptr)
	{
		return false;
	}
	// A re-register (a re-possessed pawn, or the test's explicit call on top
	// of BeginPlay's automatic one) replaces the previous binding set, so
	// exactly one binding set exists at any time.
	M3_023_UnbindAll();
	RegisteredPlayerPtr = Player;

	// Weak-lambda style: every handler re-checks this subsystem through a
	// TWeakObjectPtr, so a subsystem destroyed before its bound components
	// (a game instance shutdown ahead of a world teardown) only ever skips
	// the row instead of dereferencing a dead object.
	TWeakObjectPtr<UOperationLogSubsystem> WeakSelf(this);
	if (UCombatComponent* Combat = Player->GetCombat())
	{
		RegisteredCombatPtr = Combat;
		CombatStartedHandle = Combat->OnStarted.AddLambda([WeakSelf](FName AttackId, uint64 InstanceId)
		{
			if (UOperationLogSubsystem* Self = WeakSelf.Get())
			{
				Self->Log(M3_023_CategoryState, FString::Printf(
					TEXT("Combat Started: %s #%llu"), *AttackId.ToString(), InstanceId));
			}
		});
		CombatFinishedHandle = Combat->OnFinished.AddLambda([WeakSelf](FName AttackId, uint64 InstanceId)
		{
			if (UOperationLogSubsystem* Self = WeakSelf.Get())
			{
				Self->Log(M3_023_CategoryState, FString::Printf(
					TEXT("Combat Finished: %s #%llu"), *AttackId.ToString(), InstanceId));
			}
		});
		CombatHitHandle = Combat->OnHitConfirmed.AddLambda([WeakSelf](const FCombatHit& Hit)
		{
			if (UOperationLogSubsystem* Self = WeakSelf.Get())
			{
				const AActor* Target = Hit.Target.Get();
				Self->Log(M3_023_CategoryResponse, FString::Printf(
					TEXT("Combat HitConfirmed: %s #%llu damage=%.0f target=%s"),
					*Hit.AttackId.ToString(), Hit.AttackInstanceId, Hit.Damage,
					Target != nullptr ? *Target->GetName() : TEXT("none")));
			}
		});
	}
	if (UWorld* World = Player->GetWorld())
	{
		if (URoomSessionSubsystem* Session = World->GetSubsystem<URoomSessionSubsystem>())
		{
			RoomSessionPtr = Session;
			RoomStartedHandle = Session->OnRunStarted().AddLambda([WeakSelf]()
			{
				if (UOperationLogSubsystem* Self = WeakSelf.Get())
				{
					Self->M3_023_LogRoomRunStarted();
				}
			});
			RoomEndedHandle = Session->OnRunEnded().AddLambda([WeakSelf](const FRoomResult& Result)
			{
				if (UOperationLogSubsystem* Self = WeakSelf.Get())
				{
					Self->Log(M3_023_CategoryState, FString::Printf(
						TEXT("Room RunEnded: run=%llu cleared=%d killed=%d elapsed=%.2f settlement=%llu"),
						Result.RunId, Result.bCleared ? 1 : 0, Result.KilledCount,
						Result.ElapsedSeconds, Result.SettlementId));
				}
			});
		}
	}
	return true;
}

void UOperationLogSubsystem::UnregisterPlayer()
{
	M3_023_UnbindAll();
}

bool UOperationLogSubsystem::IsPlayerRegistered() const
{
	return RegisteredPlayerPtr.IsValid();
}

const FString& UOperationLogSubsystem::GetLogFilePath() const
{
	return LogFilePath;
}

const FDateTime& UOperationLogSubsystem::GetActiveLogDate() const
{
	return ActiveLogDate;
}

bool UOperationLogSubsystem::M3_023_EnsureDailyFile(const FDateTime& Now)
{
	const FDateTime Today = Now.GetDate();
	if (ActiveLogDate == Today)
	{
		return true;
	}
	// Day change (or the first write): the previous day's log is cleared and
	// the new day opens with a fresh header. The directory is ensured first
	// (a fresh Saved directory may not carry it yet).
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(LogFilePath), /*Tree*/ true);
	IFileManager::Get().Delete(*LogFilePath);
	ActiveLogDate = Today;
	M3_023_WriteRolloverHeader(Now);
	return true;
}

void UOperationLogSubsystem::M3_023_WriteRolloverHeader(const FDateTime& Now)
{
	const FString DateText = FString::Printf(TEXT("%04d-%02d-%02d"),
		Now.GetYear(), Now.GetMonth(), Now.GetDay());
	FString BuildId = FApp::GetBuildVersion();
	if (BuildId.IsEmpty())
	{
		BuildId = TEXT("dev");
	}
	M3_023_AppendLine(FString::Printf(TEXT("=== Operation log started %s (%s) ==="), *DateText, *BuildId));
}

void UOperationLogSubsystem::M3_023_AppendLine(const FString& Line)
{
	if (LogFilePath.IsEmpty())
	{
		return;
	}
	// One write call per line. ForceUTF8WithoutBOM instead of ForceUTF8: the
	// engine writes the BOM on EVERY SaveStringToFile call with FILEWRITE_Append
	// (Engine FileHelper.cpp serializes it unconditionally for ForceUTF8), so a
	// per-line append would sprinkle a BOM between the lines; without-BOM keeps
	// the file clean UTF-8. EvenIfReadOnly keeps the write alive beside a
	// read-only attribute flip.
	FString WithTerminator = Line;
	WithTerminator += LINE_TERMINATOR;
	FFileHelper::SaveStringToFile(WithTerminator, *LogFilePath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(),
		FILEWRITE_Append | FILEWRITE_EvenIfReadOnly);
}

void UOperationLogSubsystem::M3_023_UnbindAll()
{
	// Weak references only: a destroyed player/world already took its delegate
	// objects with it, so the unbind skips expired pointers safely.
	if (UCombatComponent* Combat = RegisteredCombatPtr.Get())
	{
		Combat->OnStarted.Remove(CombatStartedHandle);
		Combat->OnFinished.Remove(CombatFinishedHandle);
		Combat->OnHitConfirmed.Remove(CombatHitHandle);
	}
	if (URoomSessionSubsystem* Session = RoomSessionPtr.Get())
	{
		Session->OnRunStarted().Remove(RoomStartedHandle);
		Session->OnRunEnded().Remove(RoomEndedHandle);
	}
	RegisteredPlayerPtr = nullptr;
	RegisteredCombatPtr = nullptr;
	RoomSessionPtr = nullptr;
	CombatStartedHandle.Reset();
	CombatFinishedHandle.Reset();
	CombatHitHandle.Reset();
	RoomStartedHandle.Reset();
	RoomEndedHandle.Reset();
}

void UOperationLogSubsystem::M3_023_LogRoomRunStarted()
{
	const URoomSessionSubsystem* Session = RoomSessionPtr.Get();
	if (Session == nullptr)
	{
		return;
	}
	Log(M3_023_CategoryState, FString::Printf(
		TEXT("Room RunStarted: run=%llu room=%s seed=%d settlement=%llu"),
		Session->GetRunId(), *Session->GetRoomId().ToString(), Session->GetSeed(),
		Session->GetSettlementId()));
}
