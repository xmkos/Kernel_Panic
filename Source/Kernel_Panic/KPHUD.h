#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "KPHUD.generated.h"

class UTexture2D;

// Resolution-scaled combat HUD and run overlays; icons and card layers are /Game/KernelPanic/UI textures (a missing one draws nothing).
UCLASS()
class KERNEL_PANIC_API AKPHUD : public AHUD
{
	GENERATED_BODY()

public:
	AKPHUD();
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void DrawHUD() override;
	int32 NumCards() const { return CardRects.Num(); } // kit / upgrade cards drawn last frame (UI automation)

private:
	void Confirm();
	void StartWithMouse();
	void Pause();
	void Restart(); // R on the lost / won screen
	void Pick1() { PickCard(0); }
	void Pick2() { PickCard(1); }
	void Pick3() { PickCard(2); }
	void PickCard(int32 Slot);
	void SetCursorVisible(bool bVisible);
	TArray<FBox2D> CardRects; // kit / upgrade cards in screen pixels, rebuilt every frame they are drawn
	int32 HoveredCard = -1;
	float HoverT[3] = {}; // per-card hover ease 0..1, advanced in real time (the game is paused on the upgrade screen)
	UPROPERTY() TMap<FName, TObjectPtr<UTexture2D>> Icons; // keyed by icon slug (T_KP_Icon_<slug>)
	UPROPERTY() TObjectPtr<UTexture2D> CardFrame;
	UPROPERTY() TObjectPtr<UTexture2D> CardAccent;
	UPROPERTY() TObjectPtr<UTexture2D> CardGlow;
	bool bCursorShown = false;
	UPROPERTY() TObjectPtr<UInputComponent> UIInput;
	UPROPERTY() TObjectPtr<UFont> UIFont;
	double OverlayOpenedAt = 0.0;
	int32 LastOverlay = -1;
	double LastDrawAt = 0.0;
	float LastHealth = -1.f;
	float GhostHealth = 0.f;
	float GhostDelay = 0.f;
	float LastCoreHealth = -1.f;
	float CoreWarningLeft = 0.f;
};
