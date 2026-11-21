#if WITH_DEV_AUTOMATION_TESTS
#include "KPGameMode.h"
#include "KPFx.h"
#include "KPShockwave.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/AutomationTest.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "Tests/AutomationCommon.h"

class FPolishFrame : public IAutomationLatentCommand
{
public:
	explicit FPolishFrame(TFunction<void()> InAction) : Action(MoveTemp(InAction)) {}
	virtual bool Update() override { Action(); return true; }
private:
	TFunction<void()> Action;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKPCombatFeedback, "KernelPanic.Combat.Feedback",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FKPCombatFeedback::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
		if (Context.WorldType == EWorldType::Game) { World = Context.World(); break; }
	AKPGameMode* GM = World ? World->GetAuthGameMode<AKPGameMode>() : nullptr;
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!TestNotNull(TEXT("Game mode"), GM) || !TestNotNull(TEXT("Controller"), PC) || !PC->GetPawn()) return false;
	GM->StartRun();
	GM->Stats.bWritten = true; // Synthetic combat must not enter the player's run analytics.
	GM->Weapon = 0; GM->MeleeWeapon = 0; // the ranged kit leaves the melee slot empty; this test needs both weapons
	APawn* Pawn = PC->GetPawn();
	const FVector Original = Pawn->GetActorLocation();
	const FRotator OriginalRotation = Pawn->GetActorRotation();
	const FVector Origin(10000, 10000, 1000);
	Pawn->SetActorLocationAndRotation(Origin, FRotator::ZeroRotator);
	TArray<AKPEnemy*> Targets;
	for (const FVector& Offset : { FVector(150, 0, 0), FVector(0, 150, 0), FVector(-150, 0, 0), FVector(290, 0, 0) })
	{
		AKPEnemy* Enemy = World->SpawnActor<AKPEnemy>(Origin + Offset, FRotator::ZeroRotator);
		if (!TestNotNull(TEXT("Target spawned"), Enemy)) return false;
		Enemy->Init(EKPEnemyType::Rusher, 1);
		Enemy->SetActorTickEnabled(false);
		Targets.Add(Enemy);
	}
	const float PreviousDamage = GM->MeleeDamage;
	GM->MeleeDamage = 1.f;
	GM->Melee(Pawn);
	TestEqual(TEXT("Forward target takes melee damage"), Targets[0]->GetHealth(), 2.f);
	TestEqual(TEXT("Side target outside arc is untouched"), Targets[1]->GetHealth(), 3.f);
	TestEqual(TEXT("Rear target is untouched"), Targets[2]->GetHealth(), 3.f);
	TestEqual(TEXT("Target beyond base reach is untouched"), Targets[3]->GetHealth(), 3.f);
	GM->MeleeRange = 280.f;
	GM->Melee(Pawn);
	TestEqual(TEXT("Upgraded reach hits the farther target"), Targets[3]->GetHealth(), 2.f);
	GM->MeleeRange = 220.f;
	GM->MeleeWeapon = 1; // Sledge: 360 degrees, so the rear target (untouched by the Blade above) is hit
	GM->Melee(Pawn);
	TestTrue(TEXT("Sledge hits the enemy behind the player"), Targets[2]->GetHealth() < 3.f);
	GM->MeleeWeapon = 0;
	GM->MeleeDamage = PreviousDamage;
	for (AKPEnemy* Enemy : Targets) Enemy->Destroy();
	AKPEnemy* Far = World->SpawnActor<AKPEnemy>(Origin + FVector(350, 0, 0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Far target spawned"), Far)) return false;
	Far->Init(EKPEnemyType::Rusher, 1);
	Far->SetActorTickEnabled(false);
	GM->Melee(Pawn);
	TestEqual(TEXT("Blade does not reach 350 cm"), Far->GetHealth(), 3.f);
	Far->Destroy();
	auto Put = [&](const FVector& Offset, EKPEnemyType Type)
	{
		AKPEnemy* E = World->SpawnActor<AKPEnemy>(Origin + Offset, FRotator::ZeroRotator);
		if (E) { E->Init(Type, 1); E->SetActorTickEnabled(false); }
		return E;
	};
	GM->Alive = 20; // synthetic kills below must not look like a cleared wave
	GM->MeleeDamage = 1.f;
	GM->MeleeWeapon = 2; // Pointer: dash damages everything within 110 cm of the line once, not the enemy 300 cm to the side
	AKPEnemy* Dash1 = Put(FVector(300, 0, 0), EKPEnemyType::Tank);
	AKPEnemy* Dash2 = Put(FVector(550, 0, 0), EKPEnemyType::Tank);
	AKPEnemy* DashSide = Put(FVector(0, 300, 0), EKPEnemyType::Tank);
	if (!TestTrue(TEXT("Dash targets spawned"), Dash1 && Dash2 && DashSide)) return false;
	GM->Melee(Pawn);
	TestEqual(TEXT("Dash hits the enemy 300 cm ahead for x2"), Dash1->GetHealth(), 10.f);
	TestEqual(TEXT("Dash pierces to the enemy 550 cm ahead"), Dash2->GetHealth(), 10.f);
	TestEqual(TEXT("Dash misses the enemy 300 cm to the side"), DashSide->GetHealth(), 12.f);
	GM->TickDash(true); // finish the dash: restores the player's pawn collision
	Dash1->Destroy(); Dash2->Destroy(); DashSide->Destroy();
	GM->MeleeWeapon = 3; // Shifter: left, right (half damage each), then the top-down x2 crit
	GM->ShifterChain = 0;
	AKPEnemy* Dagger = Put(FVector(150, 0, 0), EKPEnemyType::Tank);
	float Before = Dagger->GetHealth();
	for (int32 i = 0; i < 2; ++i) GM->Melee(Pawn);
	TestEqual(TEXT("Shifter left + right hits deal half damage each"), Dagger->GetHealth(), Before - 1.f);
	Before = Dagger->GetHealth();
	GM->Melee(Pawn);
	TestEqual(TEXT("Shifter 3rd swing is the x2 crit"), Dagger->GetHealth(), Before - 1.f);
	Dagger->Destroy();
	GM->MeleeWeapon = 0; // a kill must shake harder than a non-lethal hit; time dilation must come back after the hit-stop
	AKPEnemy* Tough = Put(FVector(150, 0, 0), EKPEnemyType::Tank);
	GM->Trauma = 0.f;
	const float DealtBefore = GM->Stats.DamageDealt[(int32)EKPDealt::Melee], ToughBefore = Tough->GetHealth();
	GM->Melee(Pawn);
	const float HitTrauma = GM->Trauma;
	TestTrue(TEXT("Blade hit is counted as melee damage dealt"), ToughBefore > Tough->GetHealth());
	TestEqual(TEXT("Melee DamageDealt grows by the health actually removed"), GM->Stats.DamageDealt[(int32)EKPDealt::Melee] - DealtBefore, ToughBefore - Tough->GetHealth(), 0.001f);
	Tough->Destroy();
	AKPEnemy* Weak = Put(FVector(150, 0, 0), EKPEnemyType::Rusher);
	GM->MeleeDamage = 10.f;
	GM->Trauma = 0.f;
	GM->Melee(Pawn);
	TestTrue(TEXT("A kill shakes harder than a non-lethal hit"), GM->Trauma > HitTrauma);
	GM->HitStopUntil = 1.0; // long past
	GM->UpdateHitStop(FPlatformTime::Seconds());
	TestEqual(TEXT("Time dilation returns to 1 after a hit-stop"), UGameplayStatics::GetGlobalTimeDilation(GM), 1.f);
	if (IsValid(Weak)) Weak->Destroy();
	GM->MeleeDamage = PreviousDamage;
	PC->SetControlRotation(FRotator::ZeroRotator);
	PC->PlayerCameraManager->UpdateCamera(1.f);
	FVector Camera;
	FRotator Look;
	PC->GetPlayerViewPoint(Camera, Look);
	AKPEnemy* ShotTarget = World->SpawnActor<AKPEnemy>(Camera + Look.Vector() * 1000.f, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Imported shooting target"), ShotTarget)) return false;
	ShotTarget->Init(EKPEnemyType::Tank, 1);
	FHitResult VisibilityHit;
	FCollisionQueryParams Query(TEXT("KPImportedCollision"), true, Pawn);
	const FVector Aim = ShotTarget->GetAimPoint();
	TestTrue(TEXT("Imported mesh still responds to visibility traces"),
		World->LineTraceSingleByChannel(VisibilityHit, Aim - Look.Vector() * 200.f, Aim + Look.Vector() * 200.f, ECC_Visibility, Query));
	TestEqual(TEXT("Visibility hit is the imported body"), VisibilityHit.GetComponent(), static_cast<UPrimitiveComponent*>(ShotTarget->Body));
	GM->Shoot(PC);
	TestTrue(TEXT("Actual scanner damages the imported enemy"), ShotTarget->GetHealth() < ShotTarget->GetMaxHealth());
	GM->Weapon = 3; // Burst: one pull = 1 shot counted, 3 packets over time (0.6x each)
	const float BurstStart = ShotTarget->GetHealth();
	const int32 Fired = GM->Stats.ShotsFired;
	GM->Shoot(PC);
	TestEqual(TEXT("Burst pull fires only the first packet"), ShotTarget->GetHealth(), BurstStart - GM->ShotDamage * 0.6f, 0.01f);
	TestEqual(TEXT("Burst queues two more packets"), GM->BurstLeft, 2);
	for (int32 i = 0; i < 2; ++i) { GM->BurstNextAt = 0.0; GM->TickBurst(PC); }
	TestEqual(TEXT("Burst deals its 3 hits over time"), ShotTarget->GetHealth(), BurstStart - 3.f * GM->ShotDamage * 0.6f, 0.01f);
	TestEqual(TEXT("Burst counts one shot per pull"), GM->Stats.ShotsFired, Fired + 1);
	GM->Weapon = 0;
	GM->BurstLeft = 0;
	ShotTarget->Destroy();
	Pawn->SetActorLocationAndRotation(Original, OriginalRotation);
	UGameplayStatics::SetGlobalTimeDilation(GM, 1.f);
	for (int32 i = 0; i < AKPGameMode::NumRanged; ++i)
	{
		GM->Weapon = i;
		GM->Shoot(PC);
	}
	GM->Weapon = 0;
	GM->BurstLeft = 0;
	for (int32 i = 0; i < AKPGameMode::NumMelee; ++i)
	{
		GM->MeleeWeapon = i;
		GM->Melee(Pawn);
	}
	GM->MeleeWeapon = 0;
	GM->TickDash(true);
	GM->ApplyUpgrade(15);
	TestEqual(TEXT("Arc Thrower card replaces the ranged weapon"), GM->GetRangedWeapon(), 4);
	GM->ApplyUpgrade(16);
	TestEqual(TEXT("Sledge card replaces the melee weapon"), GM->GetMeleeWeapon(), 1);
	bool bBadOffer = false;
	for (int32 n = 0; n < 200; ++n)
	{
		GM->OfferUpgrades();
		int32 WeaponCards = 0;
		for (int32 U : GM->UpgradeOffer)
		{
			WeaponCards += AKPGameMode::UpgradeKind(U) <= EKPCardKind::Melee;
			bBadOffer |= U == 15 || U == 16; // the held weapons' cards
		}
		bBadOffer |= WeaponCards > 1;
	}
	TestFalse(TEXT("Offers never hold two weapon cards or a held weapon's card"), bBadOffer);
	// Weighted offers: cards matching the start kit show up clearly more often, and holding a non-default melee weapon makes other melee cards rarer.
	GM->Weapon = 0;
	GM->MeleeWeapon = 0;
	auto CountCards = [&](int32 InKit, TFunction<bool(int32)> Match)
	{
		GM->Kit = InKit;
		int32 Count = 0;
		for (int32 n = 0; n < 600; ++n)
		{
			GM->OfferUpgrades();
			for (int32 U : GM->UpgradeOffer) Count += Match(U);
		}
		return Count;
	};
	auto IsMeleeBuild = [](int32 U) { return AKPGameMode::UpgradeAffinity(U) == 2; };
	auto IsMeleeWeapon = [](int32 U) { return AKPGameMode::UpgradeKind(U) == EKPCardKind::Melee; };
	TestTrue(TEXT("Close kit offers melee-build cards more often"), CountCards(1, IsMeleeBuild) > CountCards(0, IsMeleeBuild) * 1.3f);
	const int32 MeleeBefore = CountCards(0, IsMeleeWeapon);
	GM->MeleeWeapon = 1;
	TestTrue(TEXT("Holding a melee weapon makes other melee cards rarer"), CountCards(0, IsMeleeWeapon) < MeleeBefore * 0.8f);
	GM->bChoosingUpgrade = false;
	UGameplayStatics::SetGamePaused(GM, false);
	GM->Weapon = 0;
	GM->MeleeWeapon = 0;
	GM->SpawnCore(Pawn);
	if (!TestNotNull(TEXT("Core created before firewall"), GM->GetCore())) return false;
	AKPEnemy* Burned = World->SpawnActor<AKPEnemy>(GM->GetCore()->GetActorLocation() + FVector(150, 0, 100), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Enemy near the core"), Burned)) return false;
	Burned->Init(EKPEnemyType::Tank, 1);
	Burned->SetActorTickEnabled(false);
	const float BurnedBefore = Burned->GetHealth();
	GM->ActivateFirewall(Pawn);
	TestEqual(TEXT("Firewall burst deals 2 to an enemy near the core"), BurnedBefore - Burned->GetHealth(), 2.f);
	TestTrue(TEXT("Firewall damage is counted in DamageDealt"), GM->Stats.DamageDealt[(int32)EKPDealt::Firewall] >= 2.f);
	Burned->Destroy();
	for (int32 i = 0; i < (int32)EKPSfx::Count; ++i)
		TestNotNull(TEXT("Every audio enum has an imported sound"), GM->Sounds[i].Get());
	for (int32 i = 0; i < 100; ++i)
		GM->FxBurst(Original, 10.f, 55.f, FLinearColor(1, 3, 4), 0.2f);
	TestEqual(TEXT("Crowded feedback stays within 48 pooled actors"), GM->FxPool.Num(), 48);
	if (FApp::CanEverRender())
	{
		for (AKPFx* Fx : GM->FxPool) { Fx->SetActorHiddenInGame(true); Fx->SetActorTickEnabled(false); }
		GM->Melee(Pawn);
		for (AKPFx* Fx : GM->FxPool)
			if (!Fx->IsHidden()) { Fx->Tick(0.1f); Fx->SetActorTickEnabled(false); }
		GM->Trauma = 0.f;
		GM->BannerUntil = 0.0;
		UGameplayStatics::SetGlobalTimeDilation(GM, 0.0001f);
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(2.f));
		ADD_LATENT_AUTOMATION_COMMAND(FExecStringLatentCommand(FString::Printf(TEXT("HighResShot 1 filename=\"%s\""),
			*(FPaths::ProjectSavedDir() / TEXT("Screenshots/Polish/combat.png")))));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.f));
		FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShareable(new FPolishFrame([GM, Pawn, World]()
		{
			for (TActorIterator<AKPFx> It(World); It; ++It) { It->SetActorHiddenInGame(true); It->SetActorTickEnabled(false); }
			const FVector Front = Pawn->GetActorForwardVector();
			const FVector Right = Pawn->GetActorRightVector();
			const FVector Center = GM->GetCore()->GetActorLocation();
			AKPShockwave* Wave = World->SpawnActor<AKPShockwave>(Center - FVector(0, 0, 110.f), FRotator::ZeroRotator);
			if (Wave) { Wave->Tick(0.35f); Wave->SetActorTickEnabled(false); }
			GM->SpawnEnemy(EKPEnemyType::Boss, Center + Front * 500.f);
			const FVector Muzzle = Pawn->GetActorLocation() + FVector(0, 0, 50.f);
			for (int32 i = 0; i < 3; ++i)
				GM->FxBeam(Muzzle + Right * (i * 100.f), Center + Right * (i * 250.f), i == 2 ? 14.f : 3.f,
					FLinearColor(0.2f, 1.f, 2.f) * (i + 1.f), 1.f);
			for (AKPFx* Fx : GM->FxPool) Fx->SetActorTickEnabled(false);
		})));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.f));
		ADD_LATENT_AUTOMATION_COMMAND(FExecStringLatentCommand(FString::Printf(TEXT("HighResShot 1 filename=\"%s\""),
			*(FPaths::ProjectSavedDir() / TEXT("Screenshots/Polish/weapons-shockwave.png")))));
		ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(1.f));
	}
	return !HasAnyErrors();
}

// Kit locks the other weapon: empty slot does nothing, the offer after wave 2 holds its driver card, the card fills the slot.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKPKitLock, "KernelPanic.Combat.KitLock",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FKPKitLock::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
		if (Context.WorldType == EWorldType::Game) { World = Context.World(); break; }
	AKPGameMode* GM = World ? World->GetAuthGameMode<AKPGameMode>() : nullptr;
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!TestNotNull(TEXT("Game mode"), GM) || !TestNotNull(TEXT("Controller"), PC) || !PC->GetPawn()) return false;
	GM->StartRun();
	GM->Stats.bWritten = true;
	APawn* Pawn = PC->GetPawn();
	const FVector Original = Pawn->GetActorLocation();
	const FRotator OriginalRotation = Pawn->GetActorRotation();
	const FVector Origin(10000, 10000, 1000);
	Pawn->SetActorLocationAndRotation(Origin, FRotator::ZeroRotator);
	PC->SetControlRotation(FRotator::ZeroRotator);
	PC->PlayerCameraManager->UpdateCamera(1.f);
	FVector Camera;
	FRotator Look;
	PC->GetPlayerViewPoint(Camera, Look);
	auto Put = [&](const FVector& At)
	{
		AKPEnemy* E = World->SpawnActor<AKPEnemy>(At, FRotator::ZeroRotator);
		if (E) { E->Init(EKPEnemyType::Tank, 1); E->SetActorTickEnabled(false); }
		return E;
	};
	AKPEnemy* Far = Put(Camera + Look.Vector() * 1000.f); // on the crosshair
	if (!TestNotNull(TEXT("Target spawned"), Far)) return false;
	GM->Alive = 20; // synthetic hits must not look like a cleared wave
	auto Swing = [&]() // melee damage dealt to a fresh target in Blade reach (it would also stand on the Scanner's line, so it lives only for the swing)
	{
		AKPEnemy* Near = Put(Origin + FVector(150, 0, 0));
		const float Before = Near ? Near->GetHealth() : 0.f;
		GM->Melee(Pawn);
		const float Dealt = Near ? Before - Near->GetHealth() : 0.f;
		if (Near) Near->Destroy();
		return Dealt;
	};

	GM->ApplyKit(0);
	TestTrue(TEXT("Ranged kit holds the Scanner and an empty melee slot"), GM->GetRangedWeapon() == 0 && GM->GetMeleeWeapon() == -1);
	const int32 Swings = GM->Stats.MeleeSwings;
	TestEqual(TEXT("Empty melee slot does nothing"), Swing(), 0.f);
	TestEqual(TEXT("Empty melee slot is not counted as a swing"), GM->Stats.MeleeSwings, Swings);
	GM->Shoot(PC);
	TestTrue(TEXT("The Scanner still shoots"), Far->GetHealth() < Far->GetMaxHealth());
	GM->LockAbility(0, 5.f);
	TestTrue(TEXT("Ransomware never locks the only weapon: the firewall takes the lock"), !GM->IsLocked(0) && GM->IsLocked(2));
	for (double& T : GM->LockedUntil) T = 0.0;

	GM->ApplyKit(1);
	TestTrue(TEXT("Close kit holds the Blade and an empty ranged slot"), GM->GetRangedWeapon() == -1 && GM->GetMeleeWeapon() == 0);
	const float FarBefore = Far->GetHealth();
	const int32 Fired = GM->Stats.ShotsFired;
	GM->Shoot(PC);
	TestEqual(TEXT("Empty ranged slot does nothing"), Far->GetHealth(), FarBefore);
	TestEqual(TEXT("Empty ranged slot is not counted as a shot"), GM->Stats.ShotsFired, Fired);
	TestTrue(TEXT("The Blade still swings"), Swing() > 0.f);

	// Pity rule: the offer after wave 2 always holds a weapon card for the empty slot.
	GM->Wave = 2;
	bool bMeleeKitPity = true, bRangedKitPity = true;
	for (int32 n = 0; n < 200; ++n)
	{
		GM->OfferUpgrades(); // close kit: ranged slot empty
		bool bHas = false;
		for (int32 U : GM->UpgradeOffer) bHas |= AKPGameMode::UpgradeKind(U) == EKPCardKind::Ranged;
		bRangedKitPity &= bHas;
	}
	GM->ApplyKit(0);
	for (int32 n = 0; n < 200; ++n)
	{
		GM->OfferUpgrades(); // ranged kit: melee slot empty
		bool bHas = false;
		for (int32 U : GM->UpgradeOffer) bHas |= AKPGameMode::UpgradeKind(U) == EKPCardKind::Melee;
		bMeleeKitPity &= bHas;
	}
	TestTrue(TEXT("Offer after wave 2 holds a ranged weapon card (close kit)"), bRangedKitPity);
	TestTrue(TEXT("Offer after wave 2 holds a melee weapon card (ranged kit)"), bMeleeKitPity);

	// The card fills the slot.
	GM->UpgradeOffer.Reset();
	GM->UpgradeOffer.Add(24); // Kernel Blade Core
	GM->bChoosingUpgrade = true;
	GM->ChooseUpgrade(0);
	TestEqual(TEXT("Kernel Blade Core fills the melee slot"), GM->GetMeleeWeapon(), 0);
	TestEqual(TEXT("slot_filled_wave records the wave"), GM->Stats.SlotFilledWave, 2);
	TestTrue(TEXT("The filled slot works"), Swing() > 0.f);
	GM->LockAbility(0, 5.f);
	TestTrue(TEXT("With both weapons Ransomware can lock the Scanner"), GM->IsLocked(0));

	for (double& T : GM->LockedUntil) T = 0.0;
	GM->bChoosingUpgrade = false;
	UGameplayStatics::SetGamePaused(GM, false);
	UGameplayStatics::SetGlobalTimeDilation(GM, 1.f);
	GM->Weapon = 0; GM->MeleeWeapon = 0;
	Far->Destroy();
	Pawn->SetActorLocationAndRotation(Original, OriginalRotation);
	return !HasAnyErrors();
}

// Records: best NumRecords kept, best first (pure function; the real file is never touched).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKPRecords, "KernelPanic.Combat.Records",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FKPRecords::RunTest(const FString& Parameters)
{
	TArray<FKPRecord> List;
	const FKPRecord Runs[] = { { 3, 10, 100.f, false }, { 12, 80, 600.f, true }, { 7, 40, 300.f, false }, { 12, 90, 500.f, true }, { 7, 45, 200.f, false }, { 1, 2, 20.f, false } };
	for (const FKPRecord& R : Runs) AKPGameMode::InsertRecord(List, R);
	if (!TestEqual(TEXT("Six runs keep the best five"), List.Num(), (int32)AKPGameMode::NumRecords)) return false;
	TestTrue(TEXT("Faster win first"), List[0].bWon && List[0].Seconds == 500.f);
	TestTrue(TEXT("Slower win second"), List[1].bWon && List[1].Seconds == 600.f);
	TestTrue(TEXT("Same waves lost: the longer run first"), List[2].Wave == 7 && List[2].Seconds == 300.f);
	TestTrue(TEXT("Then the shorter run"), List[3].Wave == 7 && List[3].Seconds == 200.f);
	TestEqual(TEXT("Wave 3 is the fifth"), List[4].Wave, 3);
	return !HasAnyErrors();
}

// Clock Edge sweep maths (pure, no map): pass timing, same hit count at any frame rate, jump / lane safety.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKPClockEdge, "KernelPanic.Combat.ClockEdge",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FKPClockEdge::RunTest(const FString& Parameters)
{
	const float Lo = -2600.f, Hi = -600.f; // one execution tile (CPU_LEVEL.md)
	TestNearlyEqual(TEXT("A pass takes 2.06 s"), AKPGameMode::ClockPassSeconds(Lo, Hi), 2.06f, 0.0001f);
	TestNearlyEqual(TEXT("The second pass starts at 2.5 s"), AKPGameMode::ClockPassStart(1), 2.5f, 0.0001f);
	TestNearlyEqual(TEXT("East front starts 30 cm outside the west edge"), AKPGameMode::ClockFrontX(Lo, Hi, 1.f, 0.f), Lo - 30.f, 0.01f);
	TestNearlyEqual(TEXT("West front ends 30 cm outside the west edge"), AKPGameMode::ClockFrontX(Lo, Hi, -1.f, 2.06f), Lo - 30.f, 0.01f);
	// Hits over the full 5 s live phase with a per-pass latch, as UpdateHazards does. Feet above 45 cm are safe.
	auto Hits = [](int32 Steps, float Dir, float X, float FeetZ)
	{
		bool Latched[2] = { false, false };
		int32 N = 0;
		for (int32 s = 0; s < Steps; ++s)
		{
			const float T0 = 5.f * s / Steps, T1 = 5.f * (s + 1) / Steps;
			for (int32 p = 0; p < 2; ++p)
				if (!Latched[p] && !AKPGameMode::ClockSafeFeet(FeetZ) && AKPGameMode::ClockPassSwept(-2600.f, -600.f, Dir, T0, T1, p, X, 34.f)) { Latched[p] = true; ++N; }
		}
		return N;
	};
	for (const int32 Steps : { 150, 300, 600 }) // dt = 1/30, 1/60, 1/120
		for (const float Dir : { 1.f, -1.f })
		{
			TestEqual(FString::Printf(TEXT("Ground actor is hit twice (%d steps, dir %.0f)"), Steps, Dir), Hits(Steps, Dir, -1600.f, 0.f), 2);
			TestEqual(FString::Printf(TEXT("Actor with feet at 50 cm is never hit (%d steps, dir %.0f)"), Steps, Dir), Hits(Steps, Dir, -1600.f, 50.f), 0);
			TestEqual(FString::Printf(TEXT("Central lane X = 0 is never hit (%d steps, dir %.0f)"), Steps, Dir), Hits(Steps, Dir, 0.f, 0.f), 0);
		}
	return !HasAnyErrors();
}

// Final boss (Tools/Plans/FINAL_BOSS.md): thresholds, blink anchors, Interrupt Ring jump rule, Core Hook anchor and core damage, safe death. Functions are driven directly (no ticking, no timing).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKPRootkit, "KernelPanic.Combat.Rootkit",
	EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FKPRootkit::RunTest(const FString& Parameters)
{
	// Pure geometry.
	TestEqual(TEXT("66% is still phase 1 (the change is below 66%)"), AKPEnemy::RkPhaseOf(0.66f), 1);
	TestEqual(TEXT("60% is phase 2"), AKPEnemy::RkPhaseOf(0.6f), 2);
	TestEqual(TEXT("20% is phase 3"), AKPEnemy::RkPhaseOf(0.2f), 3);
	TestEqual(TEXT("No clear anchor: no blink"), AKPEnemy::RkPickAnchor(FVector::ZeroVector, FVector(5000, 5000, 0), 0, 0), -1);
	for (int32 Roll = 0; Roll < 4; ++Roll)
	{
		const FVector Player(1000.f, 0.f, 0.f); // on anchor 0: it is illegal, the other three are 1414+ cm away
		const int32 I = AKPEnemy::RkPickAnchor(Player, FVector(5000, 5000, 0), 15, Roll);
		TestTrue(TEXT("Blink picks one of the other three anchors"), I >= 1 && I <= 3);
		TestTrue(TEXT("Blink arrival is at least 400 cm from the player"), FVector::Dist2D(AKPEnemy::RkAnchorPos(I), Player) >= 400.f);
	}
	TestTrue(TEXT("Ring sweeps a player standing in the band"), AKPEnemy::RkRingSwept(590.f, 600.f, 600.f, 34.f));
	TestFalse(TEXT("Ring has not reached a player 3 m ahead"), AKPEnemy::RkRingSwept(250.f, 260.f, 600.f, 34.f));
	TestNearlyEqual(TEXT("A lane stops at the cache cover"), AKPEnemy::RkCoverClip(FVector(-2000, 1000, 0), FVector::ForwardVector, 1600.f), 850.f, 0.01f);
	TestNearlyEqual(TEXT("A lane in the open is not clipped"), AKPEnemy::RkCoverClip(FVector(-2000, 0, 0), FVector::ForwardVector, 1600.f), 1600.f, 0.01f);
	TestTrue(TEXT("A player in the gap between lanes (200 cm off the centre line) is safe"), !AKPEnemy::RkInStrip(FVector(500, 200, 0), FVector::ZeroVector, FVector::ForwardVector, 1600.f, 75.f, 34.f));

	UWorld* World = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
		if (Context.WorldType == EWorldType::Game) { World = Context.World(); break; }
	AKPGameMode* GM = World ? World->GetAuthGameMode<AKPGameMode>() : nullptr;
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!TestNotNull(TEXT("Game mode"), GM) || !TestNotNull(TEXT("Controller"), PC) || !PC->GetPawn()) return false;
	GM->StartRun();
	GM->Stats.bWritten = true;
	GM->TickDash(true);
	GM->DashGraceUntil = 0.0; // the Feedback test dashes just before and leaves the Pointer i-frames (world time, frozen by its time dilation): HurtPlayer would ignore the Ring
	UGameplayStatics::SetGlobalTimeDilation(GM, 1.f);
	UGameplayStatics::SetGamePaused(GM, false);
	GM->Weapon = 0; GM->MeleeWeapon = 0;
	APawn* Pawn = PC->GetPawn();
	const FVector Original = Pawn->GetActorLocation();
	const FRotator OriginalRotation = Pawn->GetActorRotation();
	const FVector Origin(10000, 10000, 1000);
	Pawn->SetActorLocationAndRotation(Origin, FRotator::ZeroRotator);
	auto Clear = [&] { for (TActorIterator<AKPEnemy> It(World); It; ++It) It->Destroy(); };
	auto Rushers = [&] { int32 N = 0; for (TActorIterator<AKPEnemy> It(World); It; ++It) N += It->Type == EKPEnemyType::Rusher && !It->IsDead() && !It->bAnchor; return N; };
	auto Spawn = [&](const FVector& At)
	{
		GM->SpawnEnemy(EKPEnemyType::Rootkit, At);
		AKPEnemy* Boss = const_cast<AKPEnemy*>(GM->GetBoss());
		if (Boss) { Boss->SetActorTickEnabled(false); Boss->SetActorLocation(At); }
		return Boss;
	};
	GM->Alive = 20; // synthetic kills must not look like a cleared wave
	Clear();

	// (1) Thresholds: 60% = phase 2 and one group of 3; one hit 100% -> 20% = phase 3 look and still one group.
	AKPEnemy* B = Spawn(Origin);
	if (!TestNotNull(TEXT("Rootkit spawned"), B)) return false;
	auto Run = [&](int32 Steps) { for (int32 i = 0; i < Steps; ++i) B->RkTick(GM, Pawn, 0.1f); };
	B->Health = B->MaxHealth * 0.6f;
	Run(40);
	TestEqual(TEXT("60% health is phase 2"), B->GetRkPhase(), 2);
	TestEqual(TEXT("Phase 2 spawns exactly 3 Rushers"), Rushers(), 3);
	Clear();
	B = Spawn(Origin);
	if (!TestNotNull(TEXT("Second Rootkit spawned"), B)) return false;
	B->Health = B->MaxHealth * 0.2f;
	Run(40);
	TestEqual(TEXT("One hit to 20% is phase 3"), B->GetRkPhase(), 3);
	TestNearlyEqual(TEXT("Phase 3 look is fully open"), B->RkLookV, 2.f, 0.01f);
	TestEqual(TEXT("Skipping a threshold still spawns only one group of 3"), Rushers(), 3);

	// (2) Blink destination: an anchor, >= 400 cm from the player; none legal = Ring.
	Clear();
	B = Spawn(Origin);
	if (!TestNotNull(TEXT("Third Rootkit spawned"), B)) return false;
	Pawn->SetActorLocation(FVector(1000.f, 0.f, Origin.Z));
	B->RkForceMask = 15;
	for (int32 i = 0; i < 20; ++i)
	{
		B->RkStart(GM, Pawn, ERkAtk::Blink);
		const int32 I = B->RkBlinkIdx;
		TestTrue(TEXT("Blink starts and locks a legal anchor"), B->RkAtk == ERkAtk::Blink && I >= 1 && I <= 3);
		TestTrue(TEXT("Blink destination is that anchor, >= 400 cm from the player"), I >= 1 && FVector::Dist2D(B->RkDest, AKPEnemy::RkAnchorPos(I)) < 1.f && FVector::Dist2D(B->RkDest, Pawn->GetActorLocation()) >= 400.f);
	}
	B->RkForceMask = 0;
	B->RkStart(GM, Pawn, ERkAtk::Blink);
	TestTrue(TEXT("No legal anchor: the boss stays and uses the Ring"), B->RkAtk == ERkAtk::Ring);
	B->RkForceMask = -1;

	// (3) Interrupt Ring: a ground player is hit once, feet at 50 cm are safe.
	const float GroundZ = B->RkFloorZ() + Pawn->GetSimpleCollisionHalfHeight();
	for (const float Feet : { 0.f, 50.f })
	{
		Pawn->SetActorLocationAndRotation(FVector(Origin.X + 600.f, Origin.Y, GroundZ + Feet), FRotator::ZeroRotator);
		const float Before = GM->Stats.DamageTaken[(int32)EKPDmgSrc::Rootkit];
		B->RkStart(GM, Pawn, ERkAtk::Ring);
		Run(30); // windup 1.2 s, then the ring crosses 600 cm
		TestEqual(Feet == 0.f ? TEXT("Ring hits a grounded player once (16)") : TEXT("Ring misses a player with feet at 50 cm"), GM->Stats.DamageTaken[(int32)EKPDmgSrc::Rootkit] - Before, Feet == 0.f ? 16.f : 0.f, 0.001f);
	}

	// (4) Core Hook: one melee hit or two ranged hits break the anchor; core damage <= 24, 0 under the Firewall.
	Clear();
	GM->Alive = 20;
	Pawn->SetActorLocationAndRotation(Origin, FRotator::ZeroRotator);
	if (!GM->GetCore()) GM->SpawnCore(Pawn);
	if (!TestNotNull(TEXT("Core"), GM->GetCore())) return false;
	B = Spawn(Origin + FVector(0, -3000, 0));
	if (!TestNotNull(TEXT("Fourth Rootkit spawned"), B)) return false;
	auto Hook = [&]
	{
		B->RkStart(GM, Pawn, ERkAtk::Hook);
		Run(17); // 1.5 s tell, then the anchor exists
		return B->RkAnchor.Get();
	};
	auto Broken = [](const AKPEnemy* A) { return !IsValid(A) || A->IsDead(); };
	GM->MeleeDamage = 4.f;
	AKPEnemy* Anchor = Hook();
	if (!TestNotNull(TEXT("Core Hook spawns its anchor"), Anchor)) return false;
	TestTrue(TEXT("The anchor is not a wave enemy"), Anchor->bAnchor && GM->Alive == 21);
	Anchor->SetActorLocation(Origin + FVector(150, 0, 0));
	GM->Melee(Pawn);
	TestTrue(TEXT("One melee hit breaks the anchor"), Broken(Anchor));
	TestEqual(TEXT("The anchor is neither counted nor a kill"), GM->Alive, 21);
	Run(1);
	TestTrue(TEXT("A broken anchor ends the hook"), B->RkAtk == ERkAtk::None);
	Anchor = Hook();
	if (!TestNotNull(TEXT("Second anchor"), Anchor)) return false;
	PC->SetControlRotation(FRotator(0.f, 90.f, 0.f)); // look away from the core, which stands 500 cm ahead of the pawn's forward
	PC->PlayerCameraManager->UpdateCamera(1.f);
	FVector Camera;
	FRotator Look;
	PC->GetPlayerViewPoint(Camera, Look);
	Anchor->SetActorLocation(Camera + Look.Vector() * 800.f);
	GM->Shoot(PC);
	TestFalse(TEXT("One ranged hit leaves the anchor up"), Broken(Anchor));
	GM->Shoot(PC);
	TestTrue(TEXT("Two ranged hits break the anchor"), Broken(Anchor));
	PC->SetControlRotation(FRotator::ZeroRotator);
	B->RkEnd(0.f);
	GM->ActivateFirewall(Pawn);
	float CoreBefore = GM->CoreHealth;
	B->RkStart(GM, Pawn, ERkAtk::Hook);
	Run(50);
	TestEqual(TEXT("Firewall up: the hook deals no core damage"), GM->CoreHealth, CoreBefore);
	GM->FirewallUntil = -1.0;
	GM->FirewallCooldown = 0.f;
	B->RkEnd(0.f);
	CoreBefore = GM->CoreHealth;
	B->RkStart(GM, Pawn, ERkAtk::Hook);
	Run(50);
	TestTrue(TEXT("An unbroken hook deals 24 core damage at most"), CoreBefore - GM->CoreHealth > 23.9f && CoreBefore - GM->CoreHealth <= 24.01f);

	// (5) Death: children removed, boss counted exactly once after the 2 s sequence.
	Clear();
	GM->Alive = 20;
	GM->Boss.Reset();
	B = Spawn(Origin + FVector(0, -3000, 0));
	if (!TestNotNull(TEXT("Fifth Rootkit spawned"), B)) return false;
	B->RkStart(GM, Pawn, ERkAtk::Fork);
	Run(14);
	TestEqual(TEXT("Fork spawned 3 children"), Rushers(), 3);
	const int32 Kills = GM->Kills;
	GM->DealEnemyDamage(B, 1000000.f, PC, Pawn);
	TestTrue(TEXT("Boss is dead for shots and hazards at once"), B->IsDead());
	TestEqual(TEXT("Its children are gone"), Rushers(), 0);
	TestEqual(TEXT("Only the boss is still counted during the death sequence"), GM->Alive, 21);
	TestEqual(TEXT("No kill yet"), GM->Kills, Kills);
	for (int32 i = 0; i < 4; ++i) if (IsValid(B)) B->RkDeathTick(0.5f);
	TestEqual(TEXT("Boss removal counted once"), GM->Alive, 20);
	TestEqual(TEXT("Boss kill counted once"), GM->Kills, Kills + 1);
	B->Die(true);
	TestEqual(TEXT("A second removal does nothing"), GM->Alive, 20);
	TestNull(TEXT("The boss bar target is cleared"), GM->GetBoss());

	Clear();
	GM->Alive = 0;
	GM->Boss.Reset();
	UGameplayStatics::SetGlobalTimeDilation(GM, 1.f);
	Pawn->SetActorLocationAndRotation(Original, OriginalRotation);
	return !HasAnyErrors();
}
#endif
