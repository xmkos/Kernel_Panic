// Development-only integration check: run KernelPanic.UI.Flow in a fresh game instance.
#if WITH_DEV_AUTOMATION_TESTS
#include "KPGameMode.h"
#include "KPHUD.h"
#include "Engine/Texture2D.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "InputKeyEventArgs.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

namespace
{
	class FKPUIFlow : public IAutomationLatentCommand
	{
	public:
		explicit FKPUIFlow(FAutomationTestBase* InTest) : Test(InTest), Due(FPlatformTime::Seconds()), Timeout(Due + 60.0) {}
		virtual bool Update() override
		{
			const double Now = FPlatformTime::Seconds();
			if (Now > Timeout) { Test->AddError(TEXT("UI flow timed out waiting for a fresh playable game world")); return true; }
			UWorld* World = nullptr;
			for (const FWorldContext& Context : GEngine->GetWorldContexts())
				if (Context.WorldType == EWorldType::Game) { World = Context.World(); break; }
			AKPGameMode* GM = World ? World->GetAuthGameMode<AKPGameMode>() : nullptr;
			APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
			if (!GM || !PC || !PC->GetPawn() || !PC->GetHUD()) return false;
			if (ReleaseKey.IsValid())
			{
				PC->InputKey(FInputKeyEventArgs::CreateSimulated(ReleaseKey, IE_Released, 0.f));
				ReleaseKey = FKey();
			}
			if (Now < Due) return false;
			auto Tap = [&](FKey Key)
			{
				PC->InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Pressed, 1.f));
				ReleaseKey = Key;
			};
			auto Capture = [&](const TCHAR* Name)
			{
				FString Resolution = TEXT("unknown");
				if (World->GetGameViewport() && World->GetGameViewport()->Viewport)
				{
					const FIntPoint Size = World->GetGameViewport()->Viewport->GetSizeXY();
					Resolution = FString::Printf(TEXT("%dx%d"), Size.X, Size.Y);
				}
				FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("Screenshots/UI") / FString::Printf(TEXT("%s-%s.png"), Name, *Resolution), true, false);
			};
			switch (Step)
			{
			case 0:
				if (GM->HasStarted()) { Test->AddError(TEXT("Run UI.Flow in a fresh game, before starting")); return true; }
				{
					static const TCHAR* Textures[] = { TEXT("CardFrame"), TEXT("CardAccent"), TEXT("CardGlow"), TEXT("Icon_Arc"), TEXT("Icon_Blade"), TEXT("Icon_Burst"), TEXT("Icon_Chain"), TEXT("Icon_CoreRepair"), TEXT("Icon_Crit"),
						TEXT("Icon_Defrag"), TEXT("Icon_FastBus"), TEXT("Icon_Garbage"), TEXT("Icon_HeatSink"), TEXT("Icon_HotSwap"), TEXT("Icon_KernelBlade"), TEXT("Icon_KitClose"), TEXT("Icon_KitRanged"), TEXT("Icon_Lance"),
						TEXT("Icon_Memory"), TEXT("Icon_Multithreading"), TEXT("Icon_Overclock"), TEXT("Icon_Piercing"), TEXT("Icon_Pointer"), TEXT("Icon_Scanner"), TEXT("Icon_Scatter"), TEXT("Icon_Shield"), TEXT("Icon_Shifter"),
						TEXT("Icon_Sledge"), TEXT("Icon_Targeting"), TEXT("Icon_Uptime") };
					for (const TCHAR* Name : Textures)
						Test->TestNotNull(FString::Printf(TEXT("UI texture %s loads"), Name), LoadObject<UTexture2D>(nullptr, *FString::Printf(TEXT("/Game/KernelPanic/UI/T_KP_%s.T_KP_%s"), Name, Name)));
				}
				Capture(TEXT("start"));
				Due = Now + 2.3;
				break;
			case 1:
				Test->TestEqual(TEXT("No waves before confirmation"), GM->Wave, 0);
				if (const AKPHUD* HUD = Cast<AKPHUD>(PC->GetHUD())) Test->TestEqual(TEXT("Start screen shows the two kit cards"), HUD->NumCards(), 2);
				GM->HurtPlayer(10000.f);
				Test->TestEqual(TEXT("Briefing protects the drone"), GM->PlayerHealth, GM->PlayerMaxHealth);
				Tap(EKeys::Enter);
				Due = Now + 0.3;
				break;
			case 2:
				if (!bKitTried) // ENTER alone must not start; key 1 picks the ranged kit
				{
					bKitTried = true;
					Test->TestFalse(TEXT("ENTER without a kit does not start"), GM->HasStarted());
					Tap(EKeys::One);
					Due = Now + 0.3;
					return false;
				}
				Test->TestTrue(TEXT("Key 1 starts the run with the ranged kit"), GM->HasStarted() && GM->Kit == 0);
				Due = Now + 2.3;
				break;
			case 3:
				Test->TestEqual(TEXT("First wave starts after confirmation"), GM->Wave, 1);
				Test->TestNotNull(TEXT("Core spawned"), GM->GetCore());
				Capture(TEXT("hud"));
				Tap(EKeys::E);
				Due = Now + 0.3;
				break;
			case 4:
				Test->TestTrue(TEXT("Firewall input activates ability"), GM->IsFirewallUp());
				Tap(EKeys::P);
				Due = Now + 0.4;
				break;
			case 5:
				Test->TestTrue(TEXT("P pauses"), GM->IsRunPaused());
				PausedRunTime = GM->GetRunTime();
				PausedWorldTime = World->GetTimeSeconds();
				PausedCooldown = GM->GetAbilityCooldown(2);
				Capture(TEXT("pause"));
				Due = Now + 0.8;
				break;
			case 6:
				Test->TestEqual(TEXT("Pause freezes run time"), GM->GetRunTime(), PausedRunTime);
				Test->TestEqual(TEXT("Pause freezes world time"), (float)World->GetTimeSeconds(), PausedWorldTime);
				Test->TestEqual(TEXT("Pause freezes cooldown"), GM->GetAbilityCooldown(2), PausedCooldown);
				Tap(EKeys::Enter);
				Due = Now + 0.3;
				break;
			case 7:
				Test->TestFalse(TEXT("ENTER resumes while paused"), GM->IsRunPaused());
				Tap(EKeys::Escape);
				Due = Now + 0.3;
				break;
			case 8:
				Test->TestTrue(TEXT("ESC pauses"), GM->IsRunPaused());
				Tap(EKeys::P);
				Due = Now + 0.3;
				break;
			case 9:
				Test->TestFalse(TEXT("P resumes while paused"), GM->IsRunPaused());
				GM->SpawnEnemy(EKPEnemyType::Ransomware, PC->GetPawn()->GetActorLocation() + FVector(1200, 0, 0));
				Test->TestNotNull(TEXT("Boss health has a live target"), GM->GetBoss());
				for (int32 i = 0; i < 3; ++i) GM->LockAbility(i, 15.f); // ranged kit: the only weapon and the empty melee slot are never locked, so all three land on the firewall
				Test->TestTrue(TEXT("Locks never take the only weapon"), !GM->IsLocked(0) && !GM->IsLocked(1) && GM->IsLocked(2));
				Due = Now + 0.4;
				break;
			case 10:
				Capture(TEXT("boss-locks"));
				Due = Now + 0.3;
				break;
			case 11:
				{
					TArray<AKPEnemy*> Enemies;
					for (TActorIterator<AKPEnemy> It(World); It; ++It) Enemies.Add(*It);
					for (AKPEnemy* Enemy : Enemies) UGameplayStatics::ApplyDamage(Enemy, 100000.f, PC, PC->GetPawn(), nullptr);
				}
				Due = Now + 0.4;
				break;
			case 12:
				Test->TestTrue(TEXT("Cleared wave offers three upgrades"), GM->bChoosingUpgrade && GM->UpgradeOffer.Num() == 3);
				Test->TestNull(TEXT("Removed boss clears bar"), GM->GetBoss());
				Capture(TEXT("upgrades"));
				Due = Now + 0.3;
				break;
			case 13:
				if (const AKPHUD* HUD = Cast<AKPHUD>(PC->GetHUD())) Test->TestEqual(TEXT("Upgrade screen shows three cards"), HUD->NumCards(), 3);
				Tap(EKeys::One);
				Due = Now + 0.3;
				break;
			case 14:
				Test->TestFalse(TEXT("1 chooses upgrade"), GM->bChoosingUpgrade);
				Test->TestEqual(TEXT("Upgrade counted"), GM->GetUpgradesTaken(), 1);
				GM->HurtPlayer(100000.f);
				Due = Now + 0.4;
				break;
			case 15:
				Test->TestTrue(TEXT("Failure screen during restart delay"), GM->IsRunLost() && GM->GetRestartTimeLeft() > 0.f);
				PausedRunTime = GM->GetRunTime();
				Capture(TEXT("failure"));
				Due = Now + 0.3;
				break;
			case 16:
				Test->TestEqual(TEXT("Failure stats freeze elapsed time"), GM->GetRunTime(), PausedRunTime);
				GM->TogglePause();
				Test->TestFalse(TEXT("Failure cannot pause the reboot"), GM->IsRunPaused());
				return true;
			}
			++Step;
			return false;
		}
	private:
		FAutomationTestBase* Test;
		int32 Step = 0;
		bool bKitTried = false;
		double Due, Timeout;
		FKey ReleaseKey;
		float PausedRunTime = 0.f, PausedWorldTime = 0.f, PausedCooldown = 0.f;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKPUIFlowTest, "KernelPanic.UI.Flow", EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)
bool FKPUIFlowTest::RunTest(const FString& Parameters)
{
	ADD_LATENT_AUTOMATION_COMMAND(FKPUIFlow(this));
	return true;
}
#endif
