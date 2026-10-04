#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Templates/Function.h"

// M3-017: the startup pass reuses the M3-015 save service chain (read-only);
// FStartupLoadOutcome/UProfileSaveService live here. The single selectable
// entry is a transient URoomDefinition (full type needed for the reflected
// TObjectPtr below).
#include "../Persistence/ProfileSaveService.h"
#include "../Room/RoomDefinition.h"

#include "GameFlowSubsystem.generated.h"

class UProfileSaveService;

/**
 * Lifecycle state of the single-player game flow (M3-017, interface contract
 * section 8 "menu -> room" chain). The flow lives on the GameInstance, so it
 * spans World switches exactly like UProfileSubsystem:
 *
 * - Menu:    the map select menu is the active surface; no room load is in
 *            flight and no room run is owned by the flow.
 * - Loading: a room map open is in flight (the bLoading guard window). A
 *            second EnterRoom inside this window is refused (the card's
 *            anti-double-open rule).
 * - Room:    the room world is current; the flow owns the enter/leave
 *            bookkeeping while the room session chain runs the run itself.
 * - Result:  the room run reached its terminal state (the result screen
 *            surface); only the return to the menu may follow.
 *
 * Legal transitions (everything else is refused and recorded):
 *   Menu -> Loading -> Room -> Result -> Menu
 *   Loading -> Menu (a failed map open; error recorded, profile untouched)
 *   Room -> Menu / Result -> Menu (ReturnToMenu; LeaveRoom semantics)
 */
enum class EGameFlowState : uint8
{
	Menu,
	Loading,
	Room,
	Result
};

/**
 * M3-017: map-open seam of UGameFlowSubsystem::EnterRoom. The function executes
 * the world switch to MapPath (the room definition's soft map path, e.g.
 * "/Game/UEMMO/Maps/L_TrainingArena") and returns true when the open was
 * accepted. Production leaves the seam unset and uses the real
 * UGameplayStatics::OpenLevelBySoftObjectPtr path; automation injects a
 * counting simulator because a temporary test world cannot truly travel.
 */
using FGameFlowMapOpener = TFunction<bool(const FString& MapPath)>;

/**
 * M3-017: the game flow state machine between the map select menu and the
 * combat room (task card "single map select menu and room enter/leave").
 *
 * A UGameInstanceSubsystem on purpose: map switches and ordinary returns to
 * the menu destroy UWorlds but never this subsystem - and never the
 * GameInstance-level UProfileSubsystem. ReturnToMenu therefore destroys no
 * profile state by construction; the card's "return does not clear the
 * cross-session profile" is the lifecycle itself, pinned by the three-cycle
 * automation test.
 *
 * Explicit API only:
 * - EnterRoom(RoomDef): the single entry from Menu into a room. Validates the
 *   definition (non-null, RoomId, MapPath), refuses anything not in Menu or
 *   while a load is in flight (bLoading - the fast double-click produces
 *   exactly ONE world switch), then Menu -> Loading -> (map open) -> Room, or
 *   back to Menu with a recorded error when the open fails. A failed open
 *   NEVER clears the profile and the menu stays immediately retryable.
 * - NotifyRunEnded(): Room -> Result, the terminal-run notification surface
 *   (the result screen presentation hooks here; the existing HUD chain keeps
 *   its own FRoomResult handling).
 * - ReturnToMenu(): Room/Result -> Menu; calls the current world's
 *   URoomSessionSubsystem::LeaveRoom (the M2-011 semantics: cancels pending
 *   waves, cleans the run scope, idempotent). No profile state is touched.
 *
 * Startup (the M3-015 chain): Initialize makes the profile subsystem an
 * explicit collection dependency, then runs UProfileSaveService::StartupLoad
 * and restores the profile through UProfileSubsystem::RestoreFromSave on a
 * recovered outcome. RecoveryError keeps the "no profile" state with the
 * reason readable (GetStartupLoadOutcome); NoSaveFound - the legal FIRST BOOT
 * answer of a fresh install - bootstraps the new-game profile through
 * NewProfile (M3-029). Automation skips the real "Profile_" pass (machine
 * state must never leak into tests) unless a test injected its own service
 * via SetStartupSaveServiceForTests; the synthesized skip outcome creates no
 * profile, so the M3-003 fresh-instance guarantee holds there.
 */
UCLASS()
class UEMMO_API UGameFlowSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** The card's list rule: exactly one selectable map (no multi-map store). */
	static constexpr int32 SelectableRoomCount = 1;

	// -- Lifecycle -------------------------------------------------------------

	/**
	 * Resets the machine to Menu (the history seeds with the initial Menu
	 * entry) and runs the startup profile restore (the M3-015 chain, see the
	 * class comment).
	 */
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	/** Drops the injected map opener (and any test seam state). */
	virtual void Deinitialize() override;

	// -- Test seams ------------------------------------------------------------

	/**
	 * Replaces the production map open (UGameplayStatics) with the given
	 * simulator for this subsystem instance. Automation-only; production code
	 * never calls this. A null function restores the production path.
	 */
	void SetMapOpenerForTests(FGameFlowMapOpener InOpener);

	/**
	 * Automation-only startup seam: parks a storage-injected
	 * UProfileSaveService that the NEXT UGameFlowSubsystem::Initialize
	 * consumes (exactly once, then the seam is empty). The service is rooted
	 * while parked. Production never calls this.
	 */
	static void SetStartupSaveServiceForTests(UProfileSaveService* Service);

	// -- State machine ---------------------------------------------------------

	/**
	 * Enters the room of RoomDef from the menu: Menu -> Loading -> Room on a
	 * successful map open, Loading -> Menu with a recorded error (profile
	 * untouched, retry stays possible) on a failed one. Returns false and
	 * changes NOTHING when the state is not Menu, a load is already in flight
	 * (the bLoading guard: a fast double-click creates exactly one world
	 * switch), or the definition is null / carries no RoomId or no MapPath -
	 * every refusal is readable through GetLastError().
	 */
	bool EnterRoom(const URoomDefinition* RoomDef);

	/**
	 * Leaves the room from Room or Result: calls the current world's room
	 * session LeaveRoom (M2-011 semantics, idempotent) and moves the flow back
	 * to Menu. The GameInstance-level profile survives untouched by
	 * construction. Returns false for every other state (refusal recorded).
	 */
	bool ReturnToMenu();

	/**
	 * Room -> Result: the run of the current room reached its terminal state
	 * (the result screen surface). Refused from every other state.
	 */
	bool NotifyRunEnded();

	// -- Introspection ----------------------------------------------------------

	/** Current lifecycle state (Menu before the first accepted transition). */
	EGameFlowState GetState() const { return State; }

	/**
	 * Target state of every ACCEPTED transition in order; the history seeds
	 * with the initial Menu, so the legal full cycle reads
	 * [Menu, Loading, Room, Result, Menu]. Refused requests append nothing.
	 */
	const TArray<EGameFlowState>& GetStateHistory() const { return StateHistory; }

	/** True while a room map open is in flight (the bLoading guard window). */
	bool IsLoading() const { return bLoading; }

	/**
	 * The last recorded problem (refused request or failed map open); empty
	 * when the last accepted enter/leave cycle ended cleanly. A successful
	 * EnterRoom clears it, so an empty line always means "no pending error".
	 */
	const FString& GetLastError() const { return LastError; }

	/** Room definition of the current/last accepted EnterRoom (null in Menu). */
	const URoomDefinition* GetCurrentRoomDefinition() const { return CurrentRoomDef.Get(); }

	/**
	 * The single selectable map entry (the card's one-map menu): a transient
	 * URoomDefinition double of the Data/rooms.json room_training_01 identity
	 * (RoomId + the L_TrainingArena MapPath + the starter reward table; the
	 * wave content stays with the room session's existing chain). Created on
	 * first use and cached for the subsystem lifetime.
	 */
	URoomDefinition* GetSelectableRoomDefinition();

	// -- Startup restore (the M3-015 chain) --------------------------------------

	/** True when Initialize restored the profile through RestoreFromSave. */
	bool WasStartupProfileRestored() const { return bStartupProfileRestored; }

	/**
	 * The structured result of the Initialize-time startup pass: Recovered /
	 * RecoveredFallback restored the profile, NoSaveFound is the legal fresh
	 * state whose profile the flow bootstrapped through NewProfile (M3-029;
	 * the synthesized automation-skip outcome below ALSO reports NoSaveFound
	 * but creates no profile), RecoveryError keeps "no profile" with the
	 * corruption evidence readable.
	 */
	const FStartupLoadOutcome& GetStartupLoadOutcome() const { return StartupOutcome; }

private:
	/**
	 * Production map open behind the unset test seam: refuses an empty path or
	 * a package that does not exist (the card's "resource path error" guard)
	 * and otherwise issues the real absolute travel through
	 * UGameplayStatics::OpenLevelBySoftObjectPtr. True = the travel was
	 * issued; the asynchronous completion (and any streaming-time failure) is
	 * not observable here - the real open effect is the user's H01 check.
	 */
	bool OpenRoomMapProduction(const FString& MapPath);

	/**
	 * The only place the state changes: appends the target to the history and
	 * logs the transition. Callers guard legality before calling.
	 */
	void TransitionTo(EGameFlowState NewState);

	/** Records the last problem for GetLastError (the UI's error line). */
	void RecordError(const FString& Message);

	/**
	 * Consumes the parked startup test service (unroots it); null when the
	 * production path must build its own service.
	 */
	static UProfileSaveService* ConsumeStartupSaveServiceOverride();

	/** Lifecycle state; starts Menu. */
	EGameFlowState State = EGameFlowState::Menu;

	/** Target state per accepted transition (seeds with the initial Menu). */
	TArray<EGameFlowState> StateHistory;

	/** The bLoading guard: true from an accepted enter until Room or Menu. */
	bool bLoading = false;

	/** The last recorded problem; cleared by a successful EnterRoom. */
	FString LastError;

	/** Room definition of the current/last accepted enter (weak; transient). */
	TWeakObjectPtr<const URoomDefinition> CurrentRoomDef;

	/** The cached single selectable map entry (created on first use). */
	UPROPERTY(Transient)
	TObjectPtr<URoomDefinition> CachedSelectableRoom;

	/** Map-open seam (unset in production: the real OpenLevel path runs). */
	FGameFlowMapOpener MapOpener;

	/** The structured startup pass result (filled by Initialize). */
	FStartupLoadOutcome StartupOutcome;

	/** True only when StartupOutcome recovered AND RestoreFromSave accepted it. */
	bool bStartupProfileRestored = false;
};
