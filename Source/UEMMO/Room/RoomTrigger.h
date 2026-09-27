#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RoomDefinition.h"
#include "RoomTrigger.generated.h"

class APrototypeCharacter;
class APrototypeHUD;
class UBoxComponent;
class UEnemyDefinition;
class URoomDefinition;
class URoomSessionSubsystem;
class UStaticMeshComponent;
struct FRoomResult;

/**
 * M2-009: player-activation trigger of one combat room (interface contract
 * section 7). A box overlap volume placed ONLY on wave/刷怪 maps: when the
 * player pawn (APrototypeCharacter) first enters, the actor asks the world's
 * URoomSessionSubsystem to StartRoom. Repetition is not implemented here on
 * purpose - the session's own acceptance rule ("only Idle may start a run")
 * rejects every re-entry while the run is Running without resetting the
 * RunId, so repeated overlaps can never start a second run. A trigger that
 * never fires cannot start combat either: this actor is deliberately NOT
 * placed on the training map, so the M1 training arena keeps its
 * render-only, never-auto-fighting semantics (explicit render vs wave-spawn
 * mode separation of this card).
 *
 * The room definition is optional: when the soft asset reference is empty
 * (the current state of the map until a later room-catalog task provides
 * definition assets) the trigger builds a transient definition carrying its
 * RoomId and the FallbackWaves wave table (M2-016), which is everything
 * StartRoom and BeginWaves read.
 *
 * M2-016: the production session chain. After an accepted StartRoom the
 * trigger (1) registers the entering pawn through Session->SetPlayer (the
 * M2-010 death-failure binding; the retry-loop rebind is idempotent there),
 * (2) loads the enemy definition asset from EnemyDefinitionPath
 * (LoadSynchronous; a failure keeps the run StartRoom-only with one
 * diagnostic - no waves without a definition), (3) calls
 * Session->BeginWaves(Definition, EnemyDef) so the M2-008 progression runs,
 * and (4) hands the resolved definitions to the PrototypeHUD through
 * SetRoomRetryContext, so the M2-012 retry button has a production context
 * (HUD path chosen over a Session extension on purpose: the session's public
 * interface stays untouched).
 *
 * The session is located through the owning world at first use and held
 * weakly, so world teardown can never leave a dangling pointer.
 */
UCLASS()
class UEMMO_API ARoomTrigger : public AActor
{
	GENERATED_BODY()

public:
	ARoomTrigger();

	/** Session resolved through the owning world (weak; null outside game/PIE worlds). */
	URoomSessionSubsystem* GetSession() const { return SessionPtr.Get(); }

	/** RoomId handed to StartRoom when no definition asset is assigned. */
	FName GetRoomId() const { return RoomId; }

	/** Activation volume (overlap-only query box; exposed for tests and level scripting). */
	UBoxComponent* GetActivationVolume() const { return ActivationVolume; }

protected:
	/** Resolves the session early (GetSession is valid right after spawn) and binds the overlap handler. */
	virtual void BeginPlay() override;

private:
	/**
	 * Player entry: resolves definition + session and requests one run start.
	 * Every rejection (non-player overlap, missing session, refused StartRoom
	 * while a run is active) is logged and changes nothing. After an accepted
	 * start the M2-016 production chain runs (SetPlayer, BeginWaves, the HUD
	 * retry context).
	 */
	UFUNCTION()
	void HandleActivationBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	/** World-based session lookup; re-resolves after the weak pointer expired. */
	URoomSessionSubsystem* ResolveSession();

	/** The assigned definition asset, or a cached transient definition from RoomId + FallbackWaves. */
	const URoomDefinition* ResolveDefinition();

	/**
	 * M2-016: the enemy definition from EnemyDefinitionPath (LoadSynchronous,
	 * cached per actor). Null when the path is empty or the asset fails to
	 * load; a failure logs exactly one diagnostic per actor.
	 */
	UEnemyDefinition* ResolveEnemyDefinition();

	/**
	 * M2-016: resolves the HUD the retry context is handed to. The owning
	 * player controller's HUD first (the production form), then one
	 * class-filtered iterator pass (the M1-028 precedent) so the headless
	 * test worlds without a player controller resolve a spawned HUD too.
	 */
	APrototypeHUD* ResolveHud();

	/**
	 * M2-016: the post-StartRoom production chain: registers the entering
	 * pawn (SetPlayer), starts the wave progression (BeginWaves) when the
	 * enemy definition resolved, and hands the retry context to the HUD.
	 */
	void StartProductionSession(APrototypeCharacter* Player, const URoomDefinition* Definition);

	/** Activation volume: overlap-only, fires for the player pawn. */
	UPROPERTY(VisibleAnywhere, Category = "Room|Trigger")
	TObjectPtr<UBoxComponent> ActivationVolume;

	/** RoomId used for the transient fallback definition (e.g. "room_combat_01"). */
	UPROPERTY(EditAnywhere, Category = "Room|Trigger")
	FName RoomId;

	/**
	 * Optional data-driven definition; empty on the combat map until the later
	 * room-catalog task provides definition assets (then no code change is
	 * needed, the map assigns the asset).
	 */
	UPROPERTY(EditAnywhere, Category = "Room|Trigger")
	TSoftObjectPtr<URoomDefinition> RoomDefinitionAsset;

	/**
	 * M2-016: the enemy definition asset the wave progression is started with
	 * (BeginWaves needs one; the M2-001 catalog is the source). Defaults to
	 * the melee_grunt asset generated from Data/enemies.json by
	 * Scripts/Editor/create_enemy_assets.py. Empty disables wave spawning for
	 * this trigger (StartRoom-only behavior).
	 */
	UPROPERTY(EditAnywhere, Category = "Room|Trigger")
	TSoftObjectPtr<UEnemyDefinition> EnemyDefinitionPath;

	/**
	 * M2-016: wave data of the transient fallback definition, used only when
	 * no RoomDefinitionAsset is assigned. The default mirrors the
	 * Data/rooms.json room_training_01 wave shape (2 + 3 melee_grunt at the
	 * catalog spawn locations), so a trigger placed with class defaults - as
	 * the M2-009 map placement is - spawns the catalog waves without any map
	 * edit. Clear the array to get the plain M2-009 StartRoom-only behavior.
	 */
	UPROPERTY(EditAnywhere, Category = "Room|Trigger")
	TArray<FRoomWaveDefinition> FallbackWaves;

	/** Cached transient fallback definition (built once from RoomId + FallbackWaves). */
	UPROPERTY(Transient)
	TObjectPtr<URoomDefinition> FallbackDefinition;

	/** M2-016: cached enemy definition loaded from EnemyDefinitionPath. */
	UPROPERTY(Transient)
	TObjectPtr<UEnemyDefinition> CachedEnemyDefinition;

	/** M2-016: the one-shot diagnostic guard of a failed enemy definition load. */
	bool bEnemyDefinitionFailureLogged = false;

	/** Session found through the world; re-resolved when the weak pointer expired. */
	TWeakObjectPtr<URoomSessionSubsystem> SessionPtr;
};

/**
 * M2-009: exit door of one combat room. The door state is EVENT-DRIVEN from
 * the session - never guessed from timers: OnRunStarted relocks the door for
 * every new run, OnRunEnded unlocks it ONLY for a Cleared result (a Failed
 * run stays locked), and BeginPlay syncs once from the current session state
 * so an actor spawned into an already finished world shows the right state.
 *
 * Lock feedback is the simple ready-made geometry this card allows: an
 * engine cube whose material color is red while locked and green while
 * unlocked (a plain color marker - the win-signal gate, not physical
 * blocking, is what enforces "an uncleared room cannot be exited as a
 * victory").
 *
 * While locked, player overlap does nothing (no victory signal, no exit).
 * Once unlocked (Cleared), the FIRST player overlap notifies the session
 * through BeginExit (exactly once - afterwards the session is Exiting, a
 * state this actor never signals from); the real exit procedure belongs to
 * M2-011, this card only emits the signal. The session is located through
 * the owning world and held weakly, so teardown is always safe.
 */
UCLASS()
class UEMMO_API ARoomExit : public AActor
{
	GENERATED_BODY()

public:
	ARoomExit();

	/** True when the session events unlocked the door (a Cleared run ended). */
	bool IsExitUnlocked() const { return bUnlocked; }

	/** Session resolved through the owning world (weak; null outside game/PIE worlds). */
	URoomSessionSubsystem* GetSession() const { return SessionPtr.Get(); }

	/** Exit volume (overlap-only query box; exposed for tests and level scripting). */
	UBoxComponent* GetExitVolume() const { return ExitVolume; }

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

protected:
	/** Binds the session events, syncs the door state once, applies the visual and binds the overlap handler. */
	virtual void BeginPlay() override;

private:
	/** Player reached the exit: signals BeginExit only when unlocked AND the session is Cleared. */
	UFUNCTION()
	void HandleExitBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	/**
	 * OnRunStarted: every new run relocks the door. Bound as a weak lambda
	 * (the session's events are plain C++ delegates and FRoomResult is not a
	 * reflected type, so the handlers cannot be UFUNCTIONs); the weak binding
	 * detaches itself with the actor, EndPlay removes it eagerly.
	 */
	void HandleRunStarted();

	/** OnRunEnded: only a Cleared result unlocks (a Failed run stays locked). */
	void HandleRunEnded(const FRoomResult& Result);

	/** One-time BeginPlay sync: shows the state of an already running/finished session. */
	void SyncFromSession();

	/** Sends BeginExit to the session when the door is unlocked and the session is Cleared. */
	void SignalExitReached();

	void BindSession();
	void ApplyExitState();
	URoomSessionSubsystem* ResolveSession();

	/** Exit volume: overlap-only, fires for the player pawn. */
	UPROPERTY(VisibleAnywhere, Category = "Room|Exit")
	TObjectPtr<UBoxComponent> ExitVolume;

	/** Simple geometric feedback: engine cube, red = locked, green = unlocked. */
	UPROPERTY(VisibleAnywhere, Category = "Room|Exit")
	TObjectPtr<UStaticMeshComponent> ExitVisual;

	/** Session found through the world; re-resolved when the weak pointer expired. */
	TWeakObjectPtr<URoomSessionSubsystem> SessionPtr;

	/** Event-driven door state; false until the session reports a Cleared run. */
	bool bUnlocked = false;
};
