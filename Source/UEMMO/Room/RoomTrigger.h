#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RoomTrigger.generated.h"

class UBoxComponent;
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
 * definition assets) the trigger builds a transient definition carrying only
 * its RoomId, which is everything StartRoom reads.
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
	 * while a run is active) is logged and changes nothing.
	 */
	UFUNCTION()
	void HandleActivationBeginOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);

	/** World-based session lookup; re-resolves after the weak pointer expired. */
	URoomSessionSubsystem* ResolveSession();

	/** The assigned definition asset, or a cached transient definition from RoomId. */
	const URoomDefinition* ResolveDefinition();

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

	/** Cached transient fallback definition (built once from RoomId). */
	UPROPERTY(Transient)
	TObjectPtr<URoomDefinition> FallbackDefinition;

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
