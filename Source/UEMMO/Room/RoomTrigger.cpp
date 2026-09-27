// M2-009 green implementation: the room activation trigger and the
// event-driven exit door.
//
// ARoomTrigger: player entry requests one run start through the world's
// URoomSessionSubsystem. Repetition is handled entirely by the session's own
// acceptance rule (only Idle starts a run; a rejected repeated Start never
// resets the running RunId), so re-entries are idempotent by construction.
// The trigger only carries a RoomId + optional definition asset; it is placed
// on wave-spawn maps only (never on the training map, which keeps its
// render-only semantics).
//
// M2-016: after an accepted StartRoom the production session chain runs -
// SetPlayer(entering pawn), BeginWaves(fallback-or-asset definition, enemy
// definition from EnemyDefinitionPath) and SetRoomRetryContext on the
// PrototypeHUD (the M2-012 retry button's production context). The session
// clock itself is injected by APrototypeCharacter::Tick (the game frame
// driver), so the wave pump advances with the real game frames.
//
// ARoomExit: the door state is driven exclusively by the session events
// (OnRunStarted relocks, OnRunEnded unlocks for a Cleared result only) plus
// one BeginPlay state sync; there is no timer and no win guessing. The lock
// feedback is the simple ready-made geometry this card allows: an engine cube
// tinted red (locked) / green (unlocked) through the BasicShapeMaterial
// "Color" parameter. While locked, player overlap signals nothing; once
// Cleared, the first overlap sends exactly one BeginExit (the real exit
// procedure is M2-011's scope) - afterwards the session is Exiting, a state
// this actor never signals from.
#include "RoomTrigger.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/UObjectGlobals.h"

#include "../PrototypeCharacter.h"
#include "../PrototypeHUD.h"
#include "../Enemy/EnemyDefinition.h"
#include "RoomDefinition.h"
#include "RoomSessionSubsystem.h"

#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"

namespace
{
	// M2_009-prefixed file-local constants; the per-file translation unit can
	// never collide with another module TU.

	/** Default room id of the transient fallback definition (map property until a catalog asset exists). */
	const TCHAR* M2_009DefaultRoomId = TEXT("room_combat_01");

	constexpr float M2_009TriggerHalfExtentX = 150.0f;
	constexpr float M2_009TriggerHalfExtentY = 150.0f;
	constexpr float M2_009TriggerHalfExtentZ = 90.0f;
	constexpr float M2_009ExitHalfExtentX = 80.0f;
	constexpr float M2_009ExitHalfExtentY = 80.0f;
	constexpr float M2_009ExitHalfExtentZ = 110.0f;
	constexpr float M2_009ExitCubeScale = 1.6f;

	/** Engine basic cube: the ready-made lock/unlock feedback geometry. */
	const TCHAR* M2_009CubeMeshPath = TEXT("/Engine/BasicShapes/Cube.Cube");

	/**
	 * The tint source. Assigned explicitly: the Cube asset's own slot 0 is
	 * WorldGridMaterial (no Color parameter), so the marker only works when
	 * the BasicShapeMaterial - whose "Color" vector parameter this actor
	 * writes - is on the slot first.
	 */
	const TCHAR* M2_009CubeMaterialPath = TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial");

	/** The BasicShapeMaterial vector parameter the tint is written to. */
	const TCHAR* M2_009ColorParameterName = TEXT("Color");

	/** Locked = red, unlocked = green (plain color marker, no physical blocking). */
	const FLinearColor M2_009LockedColor(0.85f, 0.12f, 0.12f, 1.0f);
	const FLinearColor M2_009UnlockedColor(0.16f, 0.75f, 0.22f, 1.0f);

	// M2_016: production defaults of the session wiring. The enemy definition
	// asset is the one Scripts/Editor/create_enemy_assets.py generates from
	// Data/enemies.json (the melee_grunt row); the fallback wave shape mirrors
	// the Data/rooms.json room_training_01 waves (2 + 3 melee_grunt at the
	// catalog spawn locations, world space, cm). A trigger placed with class
	// defaults - like the M2-009 map placement on L_CombatRoom01, which was
	// saved before these properties existed - therefore carries them on load.
	const TCHAR* M2_016DefaultEnemyAssetPath =
		TEXT("/Game/UEMMO/Enemy/Definitions/DA_melee_grunt.DA_melee_grunt");

	const TCHAR* M2_016DefaultEnemyId = TEXT("melee_grunt");
}

// -- ARoomTrigger -------------------------------------------------------------

ARoomTrigger::ARoomTrigger()
	: RoomId(M2_009DefaultRoomId)
{
	PrimaryActorTick.bCanEverTick = false;

	ActivationVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("ActivationVolume"));
	RootComponent = ActivationVolume;
	ActivationVolume->InitBoxExtent(FVector(M2_009TriggerHalfExtentX, M2_009TriggerHalfExtentY, M2_009TriggerHalfExtentZ));
	ActivationVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ActivationVolume->SetCollisionObjectType(ECC_WorldStatic);
	ActivationVolume->SetCollisionResponseToAllChannels(ECR_Ignore);
	ActivationVolume->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	ActivationVolume->SetGenerateOverlapEvents(true);

	// M2-016 production defaults: the melee_grunt enemy asset (generated from
	// Data/enemies.json) and the Data/rooms.json room_training_01 wave shape
	// for the transient fallback definition. The existing M2-009 map placement
	// picks both up on load (the properties were never serialized there).
	EnemyDefinitionPath = TSoftObjectPtr<UEnemyDefinition>(FSoftObjectPath(M2_016DefaultEnemyAssetPath));

	FRoomWaveDefinition DefaultWave0;
	DefaultWave0.EnemyId = FName(M2_016DefaultEnemyId);
	DefaultWave0.Count = 2;
	DefaultWave0.SpawnLocations.Add(FVector(300.0, 0.0, 88.0));
	DefaultWave0.SpawnLocations.Add(FVector(450.0, -150.0, 88.0));
	FallbackWaves.Add(DefaultWave0);

	FRoomWaveDefinition DefaultWave1;
	DefaultWave1.EnemyId = FName(M2_016DefaultEnemyId);
	DefaultWave1.Count = 3;
	DefaultWave1.SpawnLocations.Add(FVector(600.0, 100.0, 88.0));
	DefaultWave1.SpawnLocations.Add(FVector(750.0, -100.0, 88.0));
	DefaultWave1.SpawnLocations.Add(FVector(0.0, 300.0, 88.0));
	FallbackWaves.Add(DefaultWave1);
}

void ARoomTrigger::BeginPlay()
{
	Super::BeginPlay();
	// Resolve the session early so GetSession() is valid right after spawn
	// (the tests and level scripts read it before any overlap happens).
	ResolveSession();
	if (ActivationVolume != nullptr)
	{
		ActivationVolume->OnComponentBeginOverlap.AddDynamic(this, &ARoomTrigger::HandleActivationBeginOverlap);
	}
}

void ARoomTrigger::HandleActivationBeginOverlap(UPrimitiveComponent* /*OverlappedComponent*/, AActor* OtherActor,
	UPrimitiveComponent* /*OtherComp*/, int32 /*OtherBodyIndex*/, bool /*bFromSweep*/, const FHitResult& /*SweepResult*/)
{
	APrototypeCharacter* Player = Cast<APrototypeCharacter>(OtherActor);
	if (Player == nullptr)
	{
		// Only the player pawn activates a room; enemies and props are ignored.
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO RoomTrigger: overlap of non-player actor %s ignored."),
			*GetNameSafe(OtherActor));
		return;
	}
	URoomSessionSubsystem* Session = ResolveSession();
	const URoomDefinition* Definition = ResolveDefinition();
	if (Session == nullptr || Definition == nullptr)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomTrigger: player entry ignored - no room session or no room definition available (this=%s)."),
			*GetName());
		return;
	}

	// Idempotency lives in the session: while a run is active (Running or any
	// later state) StartRoom refuses and the running RunId is kept untouched,
	// so repeated entries can never start a second run.
	if (Session->StartRoom(Definition))
	{
		UE_LOG(LogTemp, Display,
			TEXT("UEMMO RoomTrigger: player entered the activation volume; run started (RunId %llu, room %s)."),
			Session->GetRunId(), *Definition->RoomId.ToString());
		// M2-016: the production chain behind an accepted start - register the
		// pawn, start the wave progression, hand the retry context to the HUD.
		StartProductionSession(Player, Definition);
	}
	else
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO RoomTrigger: player re-entry ignored - the session keeps its active run (RunId %llu)."),
			Session->GetRunId());
	}
}

void ARoomTrigger::StartProductionSession(APrototypeCharacter* Player, const URoomDefinition* Definition)
{
	URoomSessionSubsystem* Session = ResolveSession();
	if (Session == nullptr || Player == nullptr || Definition == nullptr)
	{
		return;
	}
	// The death-failure binding (M2-010): the entering pawn is the run's
	// player. Re-registering the same pawn is the retry-loop rebind and the
	// SetPlayer guard keeps exactly one handler bound.
	Session->SetPlayer(Player);

	// The wave progression (M2-008) needs an enemy definition; without one the
	// run stays StartRoom-only with exactly one diagnostic (no waves, no
	// retries of the load inside the same actor lifetime).
	UEnemyDefinition* EnemyDefinition = ResolveEnemyDefinition();
	if (EnemyDefinition == nullptr)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomTrigger: run %llu started without waves - the enemy definition '%s' did not resolve."),
			Session->GetRunId(), *EnemyDefinitionPath.ToString());
		return;
	}
	if (Session->BeginWaves(Definition, EnemyDefinition))
	{
		UE_LOG(LogTemp, Display,
			TEXT("UEMMO RoomTrigger: run %llu wave progression started (%d wave(s), enemy %s)."),
			Session->GetRunId(), Definition->Waves.Num(), *EnemyDefinition->EnemyId.ToString());
	}
	// A refused BeginWaves (no waves on the definition, already active, ...)
	// keeps the M2-009 StartRoom-only semantics; the session logged the reason.

	// The M2-012 retry context: the HUD's retry button executes the real
	// URoomRetryService flow with exactly these definitions (the HUD path was
	// chosen over extending the session's public interface).
	if (APrototypeHUD* Hud = ResolveHud())
	{
		Hud->SetRoomRetryContext(Definition, EnemyDefinition);
	}
	else
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO RoomTrigger: no prototype HUD in this world; the retry context was not handed over."));
	}
}

UEnemyDefinition* ARoomTrigger::ResolveEnemyDefinition()
{
	if (CachedEnemyDefinition != nullptr)
	{
		return CachedEnemyDefinition;
	}
	if (EnemyDefinitionPath.IsNull())
	{
		return nullptr;
	}
	CachedEnemyDefinition = EnemyDefinitionPath.LoadSynchronous();
	if (CachedEnemyDefinition == nullptr && !bEnemyDefinitionFailureLogged)
	{
		// Exactly one diagnostic per actor for a broken/missing asset; a later
		// entry retries the load silently.
		bEnemyDefinitionFailureLogged = true;
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomTrigger: the enemy definition asset '%s' could not be loaded."),
			*EnemyDefinitionPath.ToString());
	}
	return CachedEnemyDefinition;
}

APrototypeHUD* ARoomTrigger::ResolveHud()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return nullptr;
	}
	// Production form first: the local player controller's HUD.
	if (APlayerController* PlayerController = World->GetFirstPlayerController())
	{
		if (APrototypeHUD* Hud = Cast<APrototypeHUD>(PlayerController->GetHUD()))
		{
			return Hud;
		}
	}
	// Headless/test fallback: one class-filtered iterator pass (the M1-028
	// reference-resolution precedent). Overlaps are rare, per-entry events.
	for (TActorIterator<APrototypeHUD> It(World); It; ++It)
	{
		return *It;
	}
	return nullptr;
}

URoomSessionSubsystem* ARoomTrigger::ResolveSession()
{
	if (!SessionPtr.IsValid())
	{
		SessionPtr = GetWorld() != nullptr
			? GetWorld()->GetSubsystem<URoomSessionSubsystem>()
			: nullptr;
	}
	return SessionPtr.Get();
}

const URoomDefinition* ARoomTrigger::ResolveDefinition()
{
	if (!RoomDefinitionAsset.IsNull())
	{
		// A real definition asset assigned on the actor wins (the later
		// room-catalog task fills this on the map; no code change needed).
		return RoomDefinitionAsset.LoadSynchronous();
	}
	// Transient fallback carrying the RoomId plus the trigger's wave table
	// (M2-016): everything StartRoom and BeginWaves read. Cached per actor;
	// transient, so it never saves into the map.
	if (FallbackDefinition == nullptr)
	{
		FallbackDefinition = NewObject<URoomDefinition>(GetTransientPackage(), NAME_None, RF_Transient);
		FallbackDefinition->RoomId = RoomId;
		FallbackDefinition->Waves = FallbackWaves;
	}
	return FallbackDefinition;
}

// -- ARoomExit ----------------------------------------------------------------

ARoomExit::ARoomExit()
{
	PrimaryActorTick.bCanEverTick = false;

	ExitVolume = CreateDefaultSubobject<UBoxComponent>(TEXT("ExitVolume"));
	RootComponent = ExitVolume;
	ExitVolume->InitBoxExtent(FVector(M2_009ExitHalfExtentX, M2_009ExitHalfExtentY, M2_009ExitHalfExtentZ));
	ExitVolume->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	ExitVolume->SetCollisionObjectType(ECC_WorldStatic);
	ExitVolume->SetCollisionResponseToAllChannels(ECR_Ignore);
	ExitVolume->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	ExitVolume->SetGenerateOverlapEvents(true);

	ExitVisual = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ExitVisual"));
	ExitVisual->SetupAttachment(RootComponent);
	ExitVisual->SetRelativeScale3D(FVector(M2_009ExitCubeScale));
	ExitVisual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ConstructorHelpers::FObjectFinder<UStaticMesh> CubeFinder(M2_009CubeMeshPath);
	if (CubeFinder.Succeeded())
	{
		ExitVisual->SetStaticMesh(CubeFinder.Object);
	}
	ConstructorHelpers::FObjectFinder<UMaterialInterface> MaterialFinder(M2_009CubeMaterialPath);
	if (MaterialFinder.Succeeded())
	{
		// The tintable base: the BasicShapeMaterial "Color" parameter is what
		// ApplyExitState writes (see the constant's comment above).
		ExitVisual->SetMaterial(0, MaterialFinder.Object);
	}
}

void ARoomExit::BeginPlay()
{
	Super::BeginPlay();
	BindSession();
	SyncFromSession();
	ApplyExitState();
	if (ExitVolume != nullptr)
	{
		ExitVolume->OnComponentBeginOverlap.AddDynamic(this, &ARoomExit::HandleExitBeginOverlap);
	}
}

void ARoomExit::HandleExitBeginOverlap(UPrimitiveComponent* /*OverlappedComponent*/, AActor* OtherActor,
	UPrimitiveComponent* /*OtherComp*/, int32 /*OtherBodyIndex*/, bool /*bFromSweep*/, const FHitResult& /*SweepResult*/)
{
	if (Cast<APrototypeCharacter>(OtherActor) == nullptr)
	{
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO RoomExit: overlap of non-player actor %s ignored."),
			*GetNameSafe(OtherActor));
		return;
	}
	SignalExitReached();
}

void ARoomExit::HandleRunStarted()
{
	// Every new run relocks the door, whatever the previous run ended as.
	bUnlocked = false;
	ApplyExitState();
}

void ARoomExit::HandleRunEnded(const FRoomResult& Result)
{
	if (Result.bCleared)
	{
		// Only the Cleared result opens the door; a Failed result keeps it
		// locked (an uncleared room can never be exited as a victory).
		bUnlocked = true;
	}
	ApplyExitState();
}

void ARoomExit::SyncFromSession()
{
	// One-time sync for an actor spawned into an already running or already
	// finished session; afterwards only the events drive the state.
	const URoomSessionSubsystem* Session = SessionPtr.Get();
	bUnlocked = Session != nullptr && Session->GetState() == ERoomSessionState::Cleared;
}

void ARoomExit::SignalExitReached()
{
	URoomSessionSubsystem* Session = ResolveSession();
	if (Session == nullptr || !bUnlocked || Session->GetState() != ERoomSessionState::Cleared)
	{
		// The locked-door gate: no victory signal, no exit flow while the run
		// is Running (or after it left Cleared). This - not physical blocking
		// - is what enforces "an uncleared room cannot be exited as a win".
		UE_LOG(LogTemp, Verbose,
			TEXT("UEMMO RoomExit: overlap on a locked exit ignored (unlocked=%d, session state=%d)."),
			bUnlocked ? 1 : 0, Session != nullptr ? static_cast<int32>(Session->GetState()) : -1);
		return;
	}
	// Exactly once per Cleared state: the accepted BeginExit moves the session
	// to Exiting, a state this actor never signals from again.
	if (Session->BeginExit())
	{
		UE_LOG(LogTemp, Display,
			TEXT("UEMMO RoomExit: player reached the unlocked exit; BeginExit accepted (RunId %llu)."),
			Session->GetRunId());
	}
}

void ARoomExit::BindSession()
{
	URoomSessionSubsystem* Session = ResolveSession();
	if (Session == nullptr)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("UEMMO RoomExit: no room session in this world; the exit stays locked (this=%s)."),
			*GetName());
		return;
	}
	// Plain C++ delegates + non-reflected FRoomResult: weak lambdas instead of
	// dynamic UFUNCTION bindings. The weak binding detaches itself with the
	// actor; EndPlay removes it eagerly on the normal teardown path.
	Session->OnRunStarted().AddWeakLambda(this, [this]()
	{
		HandleRunStarted();
	});
	Session->OnRunEnded().AddWeakLambda(this, [this](const FRoomResult& Result)
	{
		HandleRunEnded(Result);
	});
}

void ARoomExit::ApplyExitState()
{
	if (ExitVisual == nullptr || ExitVisual->GetStaticMesh() == nullptr)
	{
		return;
	}
	UMaterialInstanceDynamic* Dynamic = Cast<UMaterialInstanceDynamic>(ExitVisual->GetMaterial(0));
	if (Dynamic == nullptr)
	{
		Dynamic = ExitVisual->CreateAndSetMaterialInstanceDynamic(0);
	}
	if (Dynamic != nullptr)
	{
		Dynamic->SetVectorParameterValue(M2_009ColorParameterName, bUnlocked ? M2_009UnlockedColor : M2_009LockedColor);
	}
}

URoomSessionSubsystem* ARoomExit::ResolveSession()
{
	if (!SessionPtr.IsValid())
	{
		SessionPtr = GetWorld() != nullptr
			? GetWorld()->GetSubsystem<URoomSessionSubsystem>()
			: nullptr;
	}
	return SessionPtr.Get();
}

void ARoomExit::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (URoomSessionSubsystem* Session = SessionPtr.Get())
	{
		Session->OnRunStarted().RemoveAll(this);
		Session->OnRunEnded().RemoveAll(this);
	}
	SessionPtr = nullptr;
	Super::EndPlay(EndPlayReason);
}
