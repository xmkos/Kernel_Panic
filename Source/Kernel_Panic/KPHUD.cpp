#include "KPHUD.h"
#include "KPGameMode.h"
#include "Components/InputComponent.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "CanvasItem.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "Rendering/SlateRenderer.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	const FLinearColor Cyan(0.21f, 0.82f, 0.91f);
	const FLinearColor Gold(0.94f, 0.72f, 0.31f);
	const FLinearColor Ink(0.012f, 0.014f, 0.018f, 0.88f);
	const FLinearColor White(0.93f, 0.92f, 0.87f);
	const FLinearColor Muted(0.59f, 0.61f, 0.62f);
	const FLinearColor Red(1.f, 0.25f, 0.23f);
	const FLinearColor Track(0.15f, 0.17f, 0.18f, 0.85f);
	// Card kind palette / labels, indexed by EKPCardKind (Ranged, Melee, Offense, Defense, Utility).
	const FLinearColor KindColors[] = { Cyan, FLinearColor(0.48f, 0.64f, 0.94f), FLinearColor(0.95f, 0.49f, 0.22f), Gold, FLinearColor(0.49f, 0.76f, 0.57f) };
	const TCHAR* const KindLabels[] = { TEXT("RANGED WEAPON"), TEXT("MELEE WEAPON"), TEXT("OFFENSE"), TEXT("DEFENSE"), TEXT("UTILITY") };

	FLinearColor Alpha(FLinearColor C, float A) { C.A *= A; return C; }
	float Fraction(float Value, float Max) { return Max > 0.f ? FMath::Clamp(Value / Max, 0.f, 1.f) : 0.f; }
	float Ease(float T) { T = FMath::Clamp(T, 0.f, 1.f); return 1.f - FMath::Pow(1.f - T, 3.f); }
	FString Clock(float Seconds)
	{
		const int32 Total = FMath::FloorToInt(Seconds);
		return FString::Printf(TEXT("%02d:%02d"), Total / 60, Total % 60);
	}

	// All layout is in 1080-high design units; only these primitives touch pixels.
	struct FUI
	{
		AHUD* HUD;
		UCanvas* Canvas;
		const UFont* UIFont;
		float S, W;

		FUI(AHUD* InHUD, UCanvas* InCanvas, const UFont* InFont)
			: HUD(InHUD), Canvas(InCanvas), UIFont(InFont), S(InCanvas->ClipY / 1080.f), W(InCanvas->ClipX / S) {}

		// Runtime-cached font rasterized at the exact on-screen size: sharp at any resolution
		// (scaling the small engine bitmap font is what made the text blurry).
		// Canvas needs a real UFont object here; a plain Slate style font draws nothing.
		FSlateFontInfo FontFor(float Size) const
		{
			const int32 Points = FMath::Max(6, FMath::RoundToInt(Size * S * 0.72f)); // design px -> Slate points
			return FSlateFontInfo(UIFont, Points, Size >= 28.f ? FName("Bold") : FName("Regular"));
		}
		void Rect(float X, float Y, float Width, float Height, FLinearColor C) const
		{
			HUD->DrawRect(C, X * S, Y * S, Width * S, Height * S);
		}
		void Line(float X, float Y, float X2, float Y2, FLinearColor C, float Thickness = 1.f) const
		{
			HUD->DrawLine(X * S, Y * S, X2 * S, Y2 * S, C, Thickness * S);
		}
		float TextWidth(const FString& Text, float Size) const
		{
			if (!FSlateApplication::IsInitialized()) return Text.Len() * Size * 0.55f;
			return FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Text, FontFor(Size)).X / S;
		}
		void Text(const FString& Value, float X, float Y, float Size, FLinearColor C, float MaxWidth = 0.f) const
		{
			if (MaxWidth > 0.f) Size *= FMath::Min(1.f, MaxWidth / FMath::Max(1.f, TextWidth(Value, Size)));
			FCanvasTextItem Item(FVector2D(X * S, Y * S), FText::FromString(Value), FontFor(Size), C);
			Item.EnableShadow(FLinearColor(0.f, 0.f, 0.f, C.A * 0.8f), FVector2D(S, S));
			Canvas->DrawItem(Item);
		}
		void Center(const FString& Value, float CX, float Y, float Size, FLinearColor C, float MaxWidth = 0.f) const
		{
			const float Fit = MaxWidth > 0.f ? FMath::Min(1.f, MaxWidth / FMath::Max(1.f, TextWidth(Value, Size))) : 1.f;
			Text(Value, CX - TextWidth(Value, Size * Fit) * 0.5f, Y, Size * Fit, C);
		}
		void Paragraph(const FString& Value, float X, float Y, float Width, float Size, FLinearColor C) const
		{
			TArray<FString> Words;
			Value.ParseIntoArrayWS(Words);
			FString Row;
			for (const FString& Word : Words)
			{
				const FString Next = Row.IsEmpty() ? Word : Row + TEXT(" ") + Word;
				if (!Row.IsEmpty() && TextWidth(Next, Size) > Width)
				{
					Text(Row, X, Y, Size, C, Width);
					Y += Size * 1.5f;
					Row = Word;
				}
				else Row = Next;
			}
			Text(Row, X, Y, Size, C, Width);
		}
		void Right(const FString& Value, float X, float Y, float Size, FLinearColor C) const
		{
			Text(Value, X - TextWidth(Value, Size), Y, Size, C);
		}
		void Bar(float X, float Y, float Width, float Height, float Frac, FLinearColor C, int32 Segments = 1, float GhostFrac = 0.f) const
		{
			const float Step = Width / Segments;
			const float Gap = Segments == 1 ? 0.f : 2.f;
			for (int32 i = 0; i < Segments; ++i)
			{
				Rect(X + i * Step, Y, Step - Gap, Height, Track);
				const float GhostPart = FMath::Clamp(GhostFrac * Segments - i, 0.f, 1.f);
				if (GhostPart > 0.f) Rect(X + i * Step, Y, (Step - Gap) * GhostPart, Height, Alpha(White, 0.45f));
				const float Part = FMath::Clamp(Frac * Segments - i, 0.f, 1.f);
				if (Part > 0.f) Rect(X + i * Step, Y, (Step - Gap) * Part, Height, C);
			}
		}
		// Tinted straight-alpha texture in design pixels; a missing texture draws nothing.
		void Tex(UTexture2D* T, float X, float Y, float Width, float Height, FLinearColor C) const
		{
			if (T && C.A > 0.f) Canvas->K2_DrawTexture(T, FVector2D(X * S, Y * S), FVector2D(Width * S, Height * S), FVector2D::ZeroVector, FVector2D(1.f, 1.f), C, BLEND_Translucent);
		}
		void Lock(float X, float Y, FLinearColor C) const
		{
			Line(X + 4, Y + 9, X + 4, Y + 2, C, 2.f); Line(X + 4, Y + 2, X + 14, Y + 2, C, 2.f);
			Line(X + 14, Y + 2, X + 14, Y + 9, C, 2.f); Rect(X, Y + 9, 18, 14, C);
			Rect(X + 8, Y + 13, 2, 6, Ink);
		}
		void Dashed(float X, float Y, float Width, float Height, FLinearColor C) const // dashed box outline: an empty slot's icon
		{
			for (float T = 0.f; T < Width; T += 6.f) { Line(X + T, Y, X + FMath::Min(T + 3.f, Width), Y, C, 2.f); Line(X + T, Y + Height, X + FMath::Min(T + 3.f, Width), Y + Height, C, 2.f); }
			for (float T = 0.f; T < Height; T += 6.f) { Line(X, Y + T, X, Y + FMath::Min(T + 3.f, Height), C, 2.f); Line(X + Width, Y + T, X + Width, Y + FMath::Min(T + 3.f, Height), C, 2.f); }
		}
		void Controls(float X, float Y, float Width) const
		{
			static const TCHAR* Keys[] = { TEXT("W A S D"), TEXT("MOUSE"), TEXT("SPACE"), TEXT("LMB"), TEXT("RMB / F"), TEXT("E"), TEXT("1 / 2 / 3"), TEXT("ESC / P") };
			static const TCHAR* Actions[] = { TEXT("Move"), TEXT("Aim"), TEXT("Jump"), TEXT("Shoot"), TEXT("Melee"), TEXT("Firewall"), TEXT("Choose upgrade"), TEXT("Pause / resume") };
			const bool bTwoColumns = Width >= 600.f;
			const float Col = bTwoColumns ? Width * 0.5f : Width;
			for (int32 i = 0; i < 8; ++i)
			{
				const float PX = X + (bTwoColumns ? i / 4 : 0) * Col;
				const float PY = Y + (bTwoColumns ? i % 4 : i) * 30.f;
				const float KeyW = FMath::Min(110.f, Col * 0.42f);
				Text(Keys[i], PX, PY, 15, White, KeyW - 8);
				Text(Actions[i], PX + KeyW, PY, 16, Muted, Col - KeyW - 12);
			}
		}
	};
}

AKPHUD::AKPHUD()
{
	PrimaryActorTick.bTickEvenWhenPaused = true;
	static ConstructorHelpers::FObjectFinder<UFont> Roboto(TEXT("/Engine/EngineFonts/Roboto.Roboto"));
	UIFont = Roboto.Object;
	static ConstructorHelpers::FObjectFinder<UTexture2D> Frame(TEXT("/Game/KernelPanic/UI/T_KP_CardFrame.T_KP_CardFrame"));
	static ConstructorHelpers::FObjectFinder<UTexture2D> Accent(TEXT("/Game/KernelPanic/UI/T_KP_CardAccent.T_KP_CardAccent"));
	static ConstructorHelpers::FObjectFinder<UTexture2D> Glow(TEXT("/Game/KernelPanic/UI/T_KP_CardGlow.T_KP_CardGlow"));
	CardFrame = Frame.Object; CardAccent = Accent.Object; CardGlow = Glow.Object;
	// Every slug the gameplay tables can name; hard references so the textures cook.
	auto Load = [this](const TCHAR* Slug)
	{
		if (Icons.Contains(FName(Slug))) return;
		const FString Path = FString::Printf(TEXT("/Game/KernelPanic/UI/T_KP_Icon_%s.T_KP_Icon_%s"), Slug, Slug);
		ConstructorHelpers::FObjectFinder<UTexture2D> Finder(*Path);
		if (Finder.Succeeded()) Icons.Add(FName(Slug), Finder.Object);
	};
	for (int32 i = 0; i < AKPGameMode::NumUpgrades(); ++i) Load(AKPGameMode::UpgradeIcon(i));
	for (int32 i = 0; i < AKPGameMode::NumRanged; ++i) Load(AKPGameMode::RangedIcon(i));
	for (int32 i = 0; i < AKPGameMode::NumMelee; ++i) Load(AKPGameMode::MeleeIcon(i));
	for (int32 i = 0; i < 2; ++i) Load(AKPGameMode::KitIcon(i));
}

void AKPHUD::BeginPlay()
{
	Super::BeginPlay();
	if (!PlayerOwner) return;
	PlayerOwner->bShowMouseCursor = false;
	FInputModeGameOnly Mode;
	Mode.SetConsumeCaptureMouseDown(false);
	PlayerOwner->SetInputMode(Mode);
	if (GetWorld()->GetGameViewport())
		GetWorld()->GetGameViewport()->SetMouseCaptureMode(EMouseCaptureMode::CapturePermanently_IncludingInitialMouseDown);
	if (const AKPGameMode* GM = GetWorld()->GetAuthGameMode<AKPGameMode>(); GM && !GM->HasStarted())
	{
		PlayerOwner->SetIgnoreMoveInput(true);
		PlayerOwner->SetIgnoreLookInput(true);
	}
	UIInput = NewObject<UInputComponent>(this, TEXT("KPUIInput"));
	UIInput->RegisterComponent();
	UIInput->Priority = 100;
	auto Bind = [&](FKey Key, void (AKPHUD::*Handler)())
	{
		FInputKeyBinding& Binding = UIInput->BindKey(Key, IE_Pressed, this, Handler);
		Binding.bExecuteWhenPaused = true;
		Binding.bConsumeInput = false;
	};
	Bind(EKeys::Enter, &AKPHUD::Confirm);
	Bind(EKeys::R, &AKPHUD::Restart);
	Bind(EKeys::LeftMouseButton, &AKPHUD::StartWithMouse);
	Bind(EKeys::Escape, &AKPHUD::Pause);
	Bind(EKeys::One, &AKPHUD::Pick1);
	Bind(EKeys::Two, &AKPHUD::Pick2);
	Bind(EKeys::Three, &AKPHUD::Pick3);
	Bind(EKeys::P, &AKPHUD::Pause);
	PlayerOwner->PushInputComponent(UIInput);
}

void AKPHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (PlayerOwner)
	{
		if (UIInput) PlayerOwner->PopInputComponent(UIInput);
	}
	Super::EndPlay(EndPlayReason);
}

void AKPHUD::Confirm()
{
	if (AKPGameMode* GM = GetWorld()->GetAuthGameMode<AKPGameMode>())
	{
		if (!GM->HasStarted()) {} // a kit must be chosen first: keys 1/2 or a click, never ENTER
		else if (GM->IsRunWon()) GM->RestartRun();
		else if (GM->IsRunPaused()) GM->TogglePause();
	}
}

void AKPHUD::Restart()
{
	if (AKPGameMode* GM = GetWorld()->GetAuthGameMode<AKPGameMode>(); GM && (GM->IsRunLost() || GM->IsRunWon())) GM->RestartRun(); // no waiting for the reboot timer
}

void AKPHUD::StartWithMouse()
{
	if (HoveredCard >= 0) PickCard(HoveredCard);
}

void AKPHUD::PickCard(int32 Slot)
{
	AKPGameMode* GM = GetWorld()->GetAuthGameMode<AKPGameMode>();
	if (!GM) return;
	if (!GM->HasStarted()) { if (Slot < 2) GM->StartRun(Slot); } // start screen: the two cards are the start kits
	else if (GM->bChoosingUpgrade) GM->ChooseUpgrade(Slot);
}

void AKPHUD::SetCursorVisible(bool bVisible)
{
	// Cursor only while picking a patch; otherwise the mouse stays captured for aiming.
	if (!PlayerOwner || bCursorShown == bVisible) return;
	bCursorShown = bVisible;
	PlayerOwner->bShowMouseCursor = bVisible;
	UGameViewportClient* Viewport = GetWorld()->GetGameViewport();
	if (bVisible)
	{
		FInputModeGameAndUI Mode;
		Mode.SetHideCursorDuringCapture(false);
		PlayerOwner->SetInputMode(Mode);
		if (Viewport) Viewport->SetMouseCaptureMode(EMouseCaptureMode::CaptureDuringMouseDown);
	}
	else
	{
		FInputModeGameOnly Mode;
		Mode.SetConsumeCaptureMouseDown(false);
		PlayerOwner->SetInputMode(Mode);
		if (Viewport) Viewport->SetMouseCaptureMode(EMouseCaptureMode::CapturePermanently_IncludingInitialMouseDown);
	}
}

void AKPHUD::Pause()
{
	if (AKPGameMode* GM = GetWorld()->GetAuthGameMode<AKPGameMode>()) GM->TogglePause();
}

void AKPHUD::DrawHUD()
{
	Super::DrawHUD();
	const AKPGameMode* GM = GetWorld()->GetAuthGameMode<AKPGameMode>();
	if (!GM || !Canvas || Canvas->ClipY <= 0.f || Canvas->ClipX <= 0.f || !GEngine || !UIFont) return;
	const FUI UI(this, Canvas, UIFont);
	const float W = UI.W, CX = W * 0.5f, CY = 540.f, Margin = 28.f;
	const double Now = FPlatformTime::Seconds();
	const bool bPaused = GM->IsRunPaused();
	const int32 Overlay = !GM->HasStarted() ? 1 : GM->IsRunLost() ? 2 : GM->IsRunWon() ? 5 : bPaused ? 3 : GM->bChoosingUpgrade ? 4 : 0;
	if (Overlay != LastOverlay) { LastOverlay = Overlay; OverlayOpenedAt = Now; for (float& H : HoverT) H = 0.f; }
	SetCursorVisible(Overlay == 4 || Overlay == 1);
	if (Overlay != 4 && Overlay != 1) { CardRects.Reset(); HoveredCard = -1; }
	const float Age = (float)(Now - OverlayOpenedAt);
	const float Entrance = Ease(Age / 0.32f);
	const float DT = LastDrawAt > 0.0 ? FMath::Clamp((float)(Now - LastDrawAt), 0.f, 0.1f) : 0.f;
	LastDrawAt = Now;

	// Absolute HP survives max-health patches; UI feedback freezes with the run.
	const float Health = FMath::Max(0.f, GM->PlayerHealth);
	if (!GM->HasStarted() || LastHealth < 0.f)
	{
		GhostHealth = Health;
		GhostDelay = 0.f;
		LastCoreHealth = GM->CoreHealth;
		CoreWarningLeft = 0.f;
	}
	else if (!bPaused && !GM->bChoosingUpgrade)
	{
		if (Health > LastHealth) { GhostHealth = Health; GhostDelay = 0.f; }
		else if (Health < LastHealth && GhostHealth <= LastHealth) GhostDelay = 0.14f;
		GhostDelay = FMath::Max(0.f, GhostDelay - DT);
		if (GhostDelay <= 0.f)
			GhostHealth = FMath::FInterpConstantTo(GhostHealth, Health, DT, FMath::Max(1.f, GM->PlayerMaxHealth) * 1.5f);
		CoreWarningLeft = FMath::Max(0.f, CoreWarningLeft - DT);
		if (GM->CoreHealth < LastCoreHealth) CoreWarningLeft = 0.8f;
	}
	// Firewall healing can continue during patch selection; preserve the next damage trail.
	GhostHealth = FMath::Max(GhostHealth, Health);
	LastHealth = Health;
	LastCoreHealth = GM->CoreHealth;

	if (GM->HasStarted())
	{
		// Edge-only hurt falloff leaves the aiming area clear.
		if (!bPaused && Now < GM->HurtFlashUntil)
		{
			const float Flash = FMath::Clamp((float)(GM->HurtFlashUntil - Now) / 0.15f, 0.f, 1.f);
			for (int32 i = 0; i < 24; ++i)
			{
				const float T = (float)i / 24.f;
				const FLinearColor C = Alpha(Red, GM->HurtFlashAlpha * Flash * FMath::Square(1.f - T));
				UI.Rect(T * 120.f, 0, 5, 1080, C); UI.Rect(W - (i + 1) * 5.f, 0, 5, 1080, C);
				UI.Rect(120, T * 72.f, FMath::Max(0.f, W - 240.f), 3, C);
				UI.Rect(120, 1080 - (i + 1) * 3.f, FMath::Max(0.f, W - 240.f), 3, C);
			}
		}

		const bool bNarrowTop = W < 1400.f;
		const float StatsY = bNarrowTop ? 116.f : Margin;
		UI.Text(FString::Printf(TEXT("WAVE %d/4   /   %02d alive   /   %d kills   /   %s"), AKPGameMode::WaveInComponent(GM->Wave), GM->Alive, GM->Kills, *Clock(GM->GetRunTime())),
			Margin, StatsY, 16, White, W - Margin * 2.f);
		UI.Text(TEXT("ESC / P  pause"), Margin, StatsY + 25, 13, Muted);

		const float CoreW = FMath::Min(320.f, W - Margin * 2.f), CoreX = CX - CoreW * 0.5f;
		const float CoreFrac = Fraction(GM->CoreHealth, GM->CoreMaxHealth);
		const bool bCoreDanger = CoreWarningLeft > 0.f || CoreFrac <= 0.3f;
		const FLinearColor CoreColor = bCoreDanger ? Red : Gold;
		UI.Text(CoreWarningLeft > 0.f ? TEXT("CORE / UNDER ATTACK") : CoreFrac <= 0.3f ? TEXT("CORE / CRITICAL") : TEXT("CORE"), CoreX, 28, 16, CoreColor);
		UI.Right(FString::Printf(TEXT("%.0f%%"), CoreFrac * 100.f), CoreX + CoreW, 26, 20, CoreColor);
		UI.Bar(CoreX, 55, CoreW, 5, CoreFrac, CoreColor);
		const FString CoreStatus = GM->IsFirewallUp() ? TEXT("FIREWALL ACTIVE / CORE PROTECTED") : GM->IsLocked(2) ? TEXT("FIREWALL LOCKED / SEE [ E ]")
			: GM->GetFirewallCooldown() > 0.f ? FString::Printf(TEXT("FIREWALL / %.1fs"), GM->GetFirewallCooldown()) : TEXT("[ E ] FIREWALL READY");
		UI.Text(CoreStatus, CoreX, 70, 13, GM->IsFirewallUp() ? Cyan : GM->IsLocked(2) ? Gold : Muted, CoreW);
		const int32 Comp = AKPGameMode::ComponentOf(GM->Wave);
		if (!bNarrowTop) UI.Right(FString::Printf(TEXT("%s / LEVEL %02d"), AKPGameMode::ComponentName(Comp), Comp + 1), W - Margin, Margin, 13, Muted);

		if (const AKPEnemy* Boss = GM->GetBoss(); Boss && !Boss->IsDead())
		{
			const bool bRansom = Boss->Type == EKPEnemyType::Ransomware, bRoot = Boss->Type == EKPEnemyType::Rootkit;
			const FLinearColor C = bRoot ? FLinearColor(0.6f, 0.8f, 1.f) : bRansom ? FLinearColor(1.f, 0.8f, 0.f) : FLinearColor(0.1f, 1.f, 0.2f);
			const float BW = FMath::Min(440.f, W - Margin * 2.f), BX = CX - BW * 0.5f;
			const float BY = bNarrowTop ? 192.f : 112.f;
			const FString BossName = bRoot ? FString::Printf(TEXT("ROOTKIT / %s"), Boss->GetRkPhase() >= 3 ? TEXT("EXPOSED") : Boss->GetRkPhase() == 2 ? TEXT("FORKED") : TEXT("RESIDENT")) : bRansom ? FString(TEXT("RANSOMWARE")) : FString(TEXT("CORRUPTED FILE"));
			UI.Text(BossName, BX, BY, 18, C, BW * 0.65f);
			UI.Right(FString::Printf(TEXT("%.0f / %.0f"), GM->GetBossHealth(), GM->GetBossMaxHealth()), BX + BW, BY + 2, 15, White);
			UI.Bar(BX, BY + 30, BW, 6, Fraction(GM->GetBossHealth(), GM->GetBossMaxHealth()), C);
		}

		const float VitalsW = FMath::Min(226.f, W - Margin * 2.f), VY = 938.f;
		UI.Rect(Margin - 10, VY - 8, VitalsW + 20, 112, Alpha(Ink, 0.48f));
		const float HP = Fraction(Health, GM->PlayerMaxHealth);
		const FLinearColor HPColor = HP <= 0.3f ? Red : Cyan;
		const FString HPText = FString::Printf(TEXT("%.0f"), Health);
		UI.Text(HPText, Margin, VY, 42, HPColor);
		UI.Text(FString::Printf(TEXT("/ %.0f HP"), GM->PlayerMaxHealth), Margin + UI.TextWidth(HPText, 42) + 10, VY + 21, 15, White);
		UI.Bar(Margin, VY + 55, VitalsW, 8, HP, HPColor, 10, Fraction(GhostHealth, GM->PlayerMaxHealth));
		UI.Text(GM->MaxShield > 0.f ? FString::Printf(TEXT("SHIELD  %.0f / %.0f"), FMath::Max(0.f, GM->Shield), GM->MaxShield) : TEXT("SHIELD  /  not installed"),
			Margin, VY + 73, 13, GM->MaxShield > 0.f ? Cyan : Muted, VitalsW);
		UI.Bar(Margin, VY + 96, VitalsW, 3, Fraction(GM->Shield, GM->MaxShield), Alpha(Cyan, 0.65f), 10);

		if (GM->IsOverheated()) // debuff: slower shooting + walking, a fan pad clears it
			UI.Text(FString::Printf(TEXT("OVERHEATED - find a fan  %.1fs"), GM->OverheatLeft()), Margin, VY - 28.f, 16, Red, FMath::Min(360.f, W - Margin * 2.f));

		const int32 Pips = 3;
		const float AbilityW = FMath::Min(128.f, (W - Margin * 2.f - 24.f) / Pips), Gap = 12.f;
		const bool bStackAbilities = W < Margin * 2.f + VitalsW + 32.f + AbilityW * Pips + Gap * (Pips - 1);
		const float AX = bStackAbilities ? Margin : Margin + VitalsW + 32.f;
		const float AY = bStackAbilities ? 826.f : 956.f;
		static const TCHAR* AbilityNames[] = { TEXT("Shoot"), TEXT("Melee"), TEXT("Firewall") };
		static const TCHAR* AbilityKeys[] = { TEXT("LMB"), TEXT("RMB / F"), TEXT("E") };
		for (int32 i = 0; i < Pips; ++i)
		{
			const float X = AX + i * (AbilityW + Gap);
			const bool bLocked = GM->IsLocked(i);
			const bool bEmpty = (i == 0 && GM->GetRangedWeapon() < 0) || (i == 1 && GM->GetMeleeWeapon() < 0); // kit left this slot without a weapon
			const float Cooldown = GM->GetAbilityCooldown(i);
			const bool bActive = i == 2 && GM->IsFirewallUp();
			const FLinearColor C = bLocked ? Gold : Cyan;
			UI.Rect(X - 5, AY - 5, AbilityW + 10, 90, Alpha(Ink, 0.38f));
			UI.Text(AbilityKeys[i], X, AY, 14, bLocked ? Gold : White, AbilityW - 25);
			UI.Text(bEmpty ? TEXT("Empty") : i == 0 ? GM->WeaponName() : i == 1 ? GM->MeleeWeaponName() : AbilityNames[i], X, AY + 24, 17, bEmpty ? Muted : White, AbilityW);
			if (bEmpty) UI.Dashed(X + AbilityW - 26, AY - 4, 22, 22, Muted);
			else if (i < 2 && !bLocked) // held weapon, in the slot the lock icon uses
				UI.Tex(Icons.FindRef(FName(i == 0 ? AKPGameMode::RangedIcon(GM->GetRangedWeapon()) : AKPGameMode::MeleeIcon(GM->GetMeleeWeapon()))).Get(), X + AbilityW - 26, AY - 4, 26, 26, Cyan);
			if (bLocked)
			{
				UI.Lock(X + AbilityW - 20, AY - 2, Gold);
				UI.Text(FString::Printf(TEXT("LOCK %.1fs"), GM->LockTimeLeft(i)), X, AY + 51, 13, Gold, AbilityW);
			}
			else if (bEmpty) UI.Text(TEXT("empty - find a driver"), X, AY + 51, 13, Muted, AbilityW);
			else UI.Text(bActive ? TEXT("CORE PROTECTED") : Cooldown > 0.f ? FString::Printf(TEXT("%.1fs"), Cooldown) : TEXT("Ready"),
				X, AY + 51, 13, Cooldown > 0.f && !bActive ? Muted : Cyan, AbilityW);
			UI.Bar(X, AY + 76, AbilityW, 3, bLocked || bEmpty ? 0.f : GM->GetAbilityReadyFraction(i), C);
		}

		if (Overlay == 0)
		{
			UI.Line(CX - 12, CY, CX - 5, CY, Cyan, 2); UI.Line(CX + 5, CY, CX + 12, CY, Cyan, 2);
			UI.Line(CX, CY - 12, CX, CY - 5, Cyan, 2); UI.Line(CX, CY + 5, CX, CY + 12, Cyan, 2);
			if (Now < GM->HitMarkerUntil)
			{
				const FLinearColor C = GM->bHitMarkerKill ? Red : White;
				for (int32 i = 0; i < 4; ++i)
				{
					const float DX = i % 2 ? 1.f : -1.f, DY = i / 2 ? 1.f : -1.f;
					UI.Line(CX + DX * 15, CY + DY * 15, CX + DX * 8, CY + DY * 8, C, 2);
				}
			}
			if (Now < GM->BannerUntil)
			{
				const float In = Ease((float)(Now - GM->BannerStartedAt) / 0.3f);
				const float Out = Ease((float)(GM->BannerUntil - Now) / 0.35f);
				const float A = FMath::Min(In, Out);
				const float Y = (bNarrowTop ? 290.f : 216.f) - (1.f - In) * 18.f - (1.f - Out) * 10.f;
				UI.Center(GM->Banner, CX, Y, 27, Alpha(White, A), FMath::Min(680.f, W - Margin * 2.f));
			}
		}
	}

	if (Overlay == 0) return;
	UI.Rect(0, 0, W, 1080, FLinearColor(0.008f, 0.01f, 0.014f, Overlay == 4 ? 0.86f : 0.93f));
	const float Left = FMath::Clamp(W * 0.10f, Margin, 160.f);
	const float ContentW = FMath::Min(740.f, W - Left * 2.f);
	const float Slide = (1.f - Entrance) * 12.f;

	// Kit pick (overlay 1) and upgrade offer (overlay 4) share one card layout, hover ease and reveal. Returns the group's left edge and width.
	auto DrawCards = [&](int32 Count, bool bStack, float StackY)
	{
		const float Gap = 18.f, K = bStack ? 1.f : FMath::Min(1.f, (W - Margin * 2.f) / (Count * 360.f + FMath::Max(0, Count - 1) * 48.f)); // K: uniform fit scale
		const float CW = bStack ? FMath::Min(650.f, W - Margin * 2.f) : 360.f * K, CH = bStack ? 196.f : 504.f * K;
		const float GroupW = bStack ? CW : Count * CW + FMath::Max(0, Count - 1) * 48.f * K, X0 = CX - GroupW * 0.5f;
		// Mouse hover: hit-test last frame's untransformed card rectangles.
		float MX = -1.f, MY = -1.f;
		HoveredCard = -1;
		if (PlayerOwner && PlayerOwner->GetMousePosition(MX, MY))
			for (int32 i = 0; i < CardRects.Num(); ++i)
				if (CardRects[i].IsInside(FVector2D(MX, MY))) HoveredCard = i;
		CardRects.SetNum(Count);
		for (int32 i = 0; i < 3; ++i) HoverT[i] = FMath::Clamp(HoverT[i] + (i == HoveredCard ? DT / 0.14f : -DT / 0.11f), 0.f, 1.f);
		for (int32 Pass = 0; Pass < 2; ++Pass) // the hovered card is drawn last
			for (int32 i = 0; i < Count; ++i)
			{
				if ((i == HoveredCard) != (Pass == 1)) continue;
				const int32 U = Overlay == 4 ? GM->UpgradeOffer[i] : -1;
				if (Overlay == 4 && (U < 0 || U >= AKPGameMode::NumUpgrades())) continue;
				const int32 Kind = (int32)(Overlay == 4 ? AKPGameMode::UpgradeKind(U) : i == 0 ? EKPCardKind::Ranged : EKPCardKind::Melee);
				const FLinearColor KC = KindColors[Kind];
				const FString Name = Overlay == 4 ? AKPGameMode::UpgradeName(U) : AKPGameMode::KitName(i);
				const FString Desc = Overlay == 4 ? AKPGameMode::UpgradeDesc(U) : AKPGameMode::KitDesc(i);
				UTexture2D* Icon = Icons.FindRef(FName(Overlay == 4 ? AKPGameMode::UpgradeIcon(U) : AKPGameMode::KitIcon(i))).Get();
				const bool bHover = HoveredCard == i;
				const float Reveal = Ease((Age - i * 0.055f) / 0.18f), H = Ease(HoverT[i]);
				const float X = X0 + (bStack ? 0.f : i * (CW + 48.f * K)), Y = bStack ? StackY + i * (CH + Gap) : 326.f;
				CardRects[i] = FBox2D(FVector2D(X * UI.S, Y * UI.S), FVector2D((X + CW) * UI.S, (Y + CH) * UI.S));
				const FString Hint = Overlay == 4 ? (bHover ? FString(TEXT("Install patch")) : FString(TEXT("Press to install"))) : FString::Printf(TEXT("Press %d / click"), i + 1);
				if (bStack) // narrow screens: the old flat stacked card, same icon and kind colour
				{
					const float Yo = Y + (1.f - Reveal) * 18.f;
					UI.Rect(X, Yo, CW, CH, Alpha(Ink, Reveal));
					UI.Line(X, Yo, X + CW, Yo, bHover ? Alpha(KC, Reveal) : Alpha(Muted, 0.45f * Reveal), bHover ? 2.f : 1.f);
					if (bHover) UI.Line(X, Yo + CH, X + CW, Yo + CH, Alpha(KC, Reveal), 2.f);
					UI.Text(FString::Printf(TEXT("[ %d ]"), i + 1), X + 20, Yo + 19, 23, Alpha(White, Reveal));
					if (Overlay == 4) UI.Right(FString::Printf(TEXT("v%d.%d"), GM->Wave, i + 1), X + CW - 20, Yo + 22, 14, Alpha(Muted, Reveal));
					UI.Tex(Icon, X + 20, Yo + 58, 40, 40, Alpha(KC, Reveal));
					UI.Text(KindLabels[Kind], X + 70, Yo + 70, 13, Alpha(KC, Reveal));
					UI.Text(Name, X + 20, Yo + 104, 26, Alpha(White, Reveal), CW - 40);
					UI.Paragraph(Desc, X + 20, Yo + 147, CW - 40, 17, Alpha(White, Reveal));
					continue;
				}
				// Spec layout in 360x504 card units; scale about the card center, then lift (hover) and slide (reveal).
				const float Sk = K * (1.f + 0.03f * H), OY = -10.f * K * H + (1.f - Reveal) * 18.f;
				auto PX = [&](float L) { return X + K * 180.f + Sk * (L - 180.f); };
				auto PY = [&](float L) { return Y + K * 252.f + Sk * (L - 252.f) + OY; };
				UI.Tex(CardFrame, PX(0), PY(0), 360.f * Sk, 504.f * Sk, FLinearColor(1.f, 1.f, 1.f, Reveal));
				UI.Tex(CardAccent, PX(0), PY(0), 360.f * Sk, 504.f * Sk, Alpha(KC, Reveal));
				UI.Tex(CardGlow, PX(0), PY(0), 360.f * Sk, 504.f * Sk, Alpha(KC, 0.85f * H * Reveal));
				UI.Tex(Icon, PX(116), PY(88), 128.f * Sk, 128.f * Sk, Alpha(KC, Reveal));
				UI.Text(KindLabels[Kind], PX(32), PY(35), 16.f * Sk, Alpha(KC, Reveal), 296.f * Sk);
				UI.Text(Name, PX(32), PY(263), 28.f * Sk, Alpha(White, Reveal), 296.f * Sk);
				UI.Paragraph(Desc, PX(32), PY(318), 296.f * Sk, 17.f * Sk, Alpha(White, Reveal));
				if (Overlay == 4)
				{
					UI.Text(FString::Printf(TEXT("[%d]"), i + 1), PX(32), PY(445), 18.f * Sk, Alpha(KC, Reveal));
					UI.Text(Hint, PX(82), PY(445), 16.f * Sk, Alpha(Muted, Reveal), 246.f * Sk);
				}
				else UI.Text(Hint, PX(32), PY(445), 16.f * Sk, Alpha(bHover ? KC : Muted, Reveal), 296.f * Sk);
			}
		return FVector2D(X0, GroupW);
	};

	// Run summary on the lost and won screens: waves, kills, time, patches, best card, damage by source. NEW RECORD marks a line that beat the saved best.
	auto Summary = [&](float Y0)
	{
		const FKPRunStats& St = GM->GetStats();
		const float Other = St.DamageDealt[(int32)EKPDealt::Chain] + St.DamageDealt[(int32)EKPDealt::Firewall] + St.DamageDealt[(int32)EKPDealt::Hazard] + St.DamageDealt[(int32)EKPDealt::Other];
		const TCHAR* Labels[] = { TEXT("waves reached"), TEXT("processes killed"), TEXT("run time"), TEXT("patches installed"), TEXT("best card"), TEXT("damage / ranged"), TEXT("damage / melee"), TEXT("damage / other") };
		const FString Values[] = { FString::FromInt(GM->Wave), FString::FromInt(GM->Kills), Clock(GM->GetRunTime()), FString::FromInt(GM->GetUpgradesTaken()), GM->BestCard(),
			FString::Printf(TEXT("%.0f"), St.DamageDealt[(int32)EKPDealt::Ranged]), FString::Printf(TEXT("%.0f"), St.DamageDealt[(int32)EKPDealt::Melee]), FString::Printf(TEXT("%.0f"), Other) };
		const bool Record[] = { GM->bNewRecWave, GM->bNewRecKills, GM->bNewRecTime, false, false, false, false, false };
		const float DumpW = FMath::Min(460.f, ContentW);
		for (int32 i = 0; i < 8; ++i)
		{
			const float Y = Y0 + i * 32.f + Slide;
			UI.Text(Labels[i], Left, Y, 17, Muted, DumpW * 0.5f);
			UI.Right(Values[i], Left + DumpW, Y - 2, 22, White);
			if (Record[i]) UI.Text(TEXT("NEW RECORD"), Left + DumpW + 16.f, Y + 1, 16, Gold);
		}
		return DumpW;
	};

	if (Overlay == 1)
	{
		const bool bStack = W < 1000.f;
		const float CtrlW = FMath::Min(ContentW, CX - 408.f - Left); // controls live left of the kit pair, or on the pause screen only
		UI.Text(TEXT("KERNEL PANIC"), Left, (bStack ? 40.f : 64.f) + Slide, bStack ? 56 : 72, White, ContentW);
		UI.Text(TEXT("Defend the core. Survive the next wave."), Left, (bStack ? 115.f : 162.f) + Slide, 23, White, ContentW);
		// A completed boot log: no looping cursor, invented progress or flicker.
		const TCHAR* BootLog[] = { TEXT("kernel / motherboard online"), TEXT("core / defend this to stay alive"), TEXT("drone / move, aim, eliminate threats"), TEXT("patches / choose one after each wave") };
		for (int32 i = 0; i < 4; ++i)
		{
			const float Y = (bStack ? 165.f : 205.f) + i * 31.f + Slide;
			UI.Text(TEXT("[ok]"), Left, Y, 15, Muted);
			UI.Text(BootLog[i], Left + 52, Y, 16, i == 1 ? Gold : Muted, ContentW - 52);
		}
		if (!bStack && CtrlW >= 240.f) UI.Controls(Left, 326.f + Slide, CtrlW);
		DrawCards(2, bStack, 330.f);
		const float PromptY = bStack ? 770.f : 862.f;
		UI.Center(TEXT("Choose your kit to start"), CX, PromptY + Slide, 27, White, W - Margin * 2.f);
		UI.Center(TEXT("Press 1 / 2, or click a card"), CX, PromptY + 43 + Slide, 16, Muted, W - Margin * 2.f);
		UI.Center(TEXT("If you or the core fall, the entire run restarts."), CX, PromptY + 89 + Slide, 16, Muted, W - Margin * 2.f);
	}
	else if (Overlay == 3)
	{
		UI.Text(TEXT("Paused"), Left, 220 + Slide, 60, White, ContentW);
		UI.Text(FString::Printf(TEXT("Wave %02d   /   %s"), GM->Wave, *Clock(GM->GetRunTime())), Left, 310 + Slide, 20, Muted, ContentW);
		UI.Paragraph(TEXT("Your drone, the core and all cooldowns are frozen."), Left, 361 + Slide, ContentW, 19, White);
		UI.Controls(Left, 449 + Slide, ContentW);
		const float PromptY = ContentW >= 600.f ? 671.f : 769.f;
		UI.Text(TEXT("[ ENTER ]  Resume"), Left, PromptY + Slide, 27, White, ContentW);
		UI.Text(TEXT("ESC / P also resumes"), Left, PromptY + 44 + Slide, 16, Muted, ContentW);
	}
	else if (Overlay == 2)
	{
		UI.Text(TEXT("KERNEL PANIC"), Left, 221 + Slide, 68, Red, ContentW);
		UI.Text(GM->GetFailureReason(), Left, 316 + Slide, 24, White, ContentW);
		UI.Text(TEXT("Run stopped. Crash dump follows."), Left, 363 + Slide, 17, Muted, ContentW);
		const float DumpW = Summary(405.f);
		UI.Text(TEXT("[ R ]  Restart now"), Left, 680 + Slide, 27, White, ContentW);
		UI.Text(FString::Printf(TEXT("Automatic reboot in %.1fs"), GM->GetRestartTimeLeft()), Left, 730 + Slide, 20, Red, ContentW);
		UI.Bar(Left, 768 + Slide, DumpW, 3, GM->GetRestartTimeLeft() / 10.f, Red);
		UI.Text(TEXT("All run upgrades will be lost."), Left, 788 + Slide, 16, Muted, ContentW);
	}
	else if (Overlay == 5)
	{
		UI.Text(TEXT("SYSTEM CLEAN"), Left, 221 + Slide, 68, Cyan, ContentW);
		UI.Text(TEXT("All three components are free of infection."), Left, 316 + Slide, 24, White, ContentW);
		UI.Text(TEXT("Run complete. Final report follows."), Left, 363 + Slide, 17, Muted, ContentW);
		Summary(405.f);
		UI.Text(TEXT("[ R ]  New run"), Left, 680 + Slide, 27, White, ContentW);
		UI.Text(TEXT("ENTER also starts a new run"), Left, 724 + Slide, 16, Muted, ContentW);
	}
	else if (Overlay == 4)
	{
		const bool bStack = W < 1000.f;
		const int32 Count = FMath::Min(3, GM->UpgradeOffer.Num());
		const FVector2D Group = DrawCards(Count, bStack, 250.f); // x = left edge, y = width
		UI.Text(TEXT("Install a patch"), Group.X, bStack ? 128.f : 158.f, 44, White, Group.Y);
		UI.Text(FString::Printf(TEXT("Wave %02d cleared. Install one upgrade."), GM->Wave), Group.X, bStack ? 193.f : 222.f, 20, Muted, Group.Y);
		const float FootY = bStack ? 250.f + Count * 214.f + 12.f : 886.f;
		UI.Text(TEXT("[ 1 / 2 / 3 ] or click a card.  Time is frozen until you choose."), Group.X, FootY, 18, Muted, Group.Y);
	}
}
