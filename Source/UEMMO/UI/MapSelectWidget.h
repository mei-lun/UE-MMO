#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "MapSelectWidget.generated.h"

class UBorder;
class UButton;
class UCanvasPanel;
class UGameFlowSubsystem;
class UTextBlock;
class UVerticalBox;

/**
 * M3-017: the map select menu (native-only UUserWidget, same code-built
 * pattern as M2-012's URoomResultWidget - no UMG asset, no external UI
 * package). The card's list rule: the menu offers EXACTLY ONE selectable map
 * (TrainingArena); there is no multi-map store. The enter button asks the
 * GameInstance-level UGameFlowSubsystem to enter the selectable room; while a
 * load is in flight the button is disabled (the flow's bLoading state guard
 * is the machine half, the disabled UButton the visible half), and a failed
 * open shows the flow's error line and returns the button to the player so
 * the entry stays retryable. The settings/volume row is a plain text
 * placeholder by the card (no store, no settings implementation here).
 *
 * The widget resolves the flow subsystem through its owning game instance, so
 * it works headless (automation creates it in a plain game world) and inside
 * the future menu HUD wiring without extra plumbing.
 */
UCLASS()
class UEMMO_API UMapSelectWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** The card's list rule as code: exactly one selectable map row. */
	static constexpr int32 MapEntryCount = 1;

	virtual void NativeOnInitialized() override;

	/**
	 * Arms one fresh menu presentation (the BindResult precedent of the M2-012
	 * result screen): builds the control tree when NativeOnInitialized has not
	 * run yet (engine 5.8 runs it on CreateWidget only with a valid player
	 * context), clears the error line and enables the enter button. The menu
	 * wiring calls this on every presentation.
	 */
	void BindMenu();

	/**
	 * The enter button's click path: while the flow cannot accept an entry
	 * (a load in flight or a room already active) the button stays disabled
	 * and nothing is requested; otherwise the flow's EnterRoom runs for the
	 * single selectable definition. Success keeps the button disabled and
	 * clears the error line (the room world takes over); failure shows the
	 * flow's recorded error and re-enables the button (retry stays possible).
	 */
	void HandleEnterClicked();

	/**
	 * Shows Message on the error line; an empty Message collapses the line.
	 */
	void SetErrorText(const FString& Message);

	/** Enables/disables the enter button (the visible guard half). */
	void SetEnterButtonEnabled(bool bNewEnabled);

	// -- Read seams (tests and the future menu HUD wiring) -----------------------

	UTextBlock* PeekTitleBlock() const { return TitleBlock.Get(); }
	UTextBlock* PeekMapEntryBlock() const { return MapEntryBlock.Get(); }
	UTextBlock* PeekSettingsPlaceholderBlock() const { return SettingsPlaceholderBlock.Get(); }
	UButton* PeekEnterButton() const { return EnterButton.Get(); }
	UTextBlock* PeekEnterLabel() const { return EnterLabel.Get(); }
	UTextBlock* PeekErrorBlock() const { return ErrorBlock.Get(); }

private:
	/** Builds the whole control tree in code (idempotent). */
	void BuildControls();

	/** Null-safe flow resolution through the owning game instance. */
	UGameFlowSubsystem* GetGameFlowSubsystem() const;

	UFUNCTION()
	void HandleEnterButtonClicked();

	UPROPERTY(Transient)
	TObjectPtr<UCanvasPanel> RootCanvas;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> PanelBorder;

	UPROPERTY(Transient)
	TObjectPtr<UVerticalBox> Stack;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> TitleBlock;

	UPROPERTY(Transient)
	TObjectPtr<UBorder> MapEntryRow;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> MapEntryBlock;

	UPROPERTY(Transient)
	TObjectPtr<UButton> EnterButton;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> EnterLabel;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> ErrorBlock;

	UPROPERTY(Transient)
	TObjectPtr<UTextBlock> SettingsPlaceholderBlock;

	bool bControlsBuilt = false;
};
