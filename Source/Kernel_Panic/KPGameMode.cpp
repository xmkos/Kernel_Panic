#include "KPGameMode.h"
#include "KPEnemy.h"
#include "KPHUD.h"
#include "KPFx.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "EngineUtils.h"
#include "CoreGlobals.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "NavigationSystem.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	struct FKPUpgrade { const TCHAR* Name; const TCHAR* Desc; EKPCardKind Kind; const TCHAR* Icon; int32 Weapon; }; // Weapon: slot weapon a Ranged/Melee card equips
	using K = EKPCardKind;
	// Small, tested pool (design doc: "a limited, tested upgrade pool"). Indices 0..24 are stable (analytics, kp.Pick); append only.
	const FKPUpgrade GUpgrades[] = {
		{ TEXT("Overclock"),        TEXT("+1 shot damage"), K::Offense, TEXT("Overclock"), 0 },
		{ TEXT("Multithreading"),   TEXT("+25% fire rate"), K::Offense, TEXT("Multithreading"), 0 },
		{ TEXT("Fast Bus"),         TEXT("+15% move speed"), K::Utility, TEXT("FastBus"), 0 },
		{ TEXT("Piercing Packets"), TEXT("Shots go through +1 enemy"), K::Offense, TEXT("Piercing"), 0 },
		{ TEXT("Firewall Shield"),  TEXT("+30 shield, recharges when not hit"), K::Defense, TEXT("Shield"), 0 },
		{ TEXT("Memory Expansion"), TEXT("+40 max HP and full heal"), K::Defense, TEXT("Memory"), 0 },
		{ TEXT("Core Repair"),      TEXT("Restore 200 core HP, firewall ready now"), K::Defense, TEXT("CoreRepair"), 0 },
		{ TEXT("Kernel Blade"),     TEXT("+2 melee damage, wider reach"), K::Offense, TEXT("KernelBlade"), 0 },
		{ TEXT("Targeting Daemon"), TEXT("Stronger aim assist: shots snap to nearby targets"), K::Utility, TEXT("Targeting"), 0 },
		{ TEXT("Scatter Driver"),   TEXT("LMB weapon: 7 pellets, brutal up close, kicks you back. Replaces ranged weapon"), K::Ranged, TEXT("Scatter"), 1 },
		{ TEXT("Lance Protocol"),   TEXT("LMB weapon: slow 4x shot piercing every enemy in line. Replaces ranged weapon"), K::Ranged, TEXT("Lance"), 2 },
		{ TEXT("Chain Exploit"),    TEXT("Your kills burst for 1 damage to enemies within 3 m (+1 per stack)"), K::Offense, TEXT("Chain"), 0 },
		{ TEXT("Garbage Collector"), TEXT("Each kill heals 2 HP (+2 per stack)"), K::Defense, TEXT("Garbage"), 0 },
		{ TEXT("Hot Swap"),         TEXT("Firewall cooldown -30% (stacks)"), K::Utility, TEXT("HotSwap"), 0 },
		{ TEXT("Burst Protocol"),   TEXT("LMB weapon: 3 shots in 0.14 s, 0.6x damage each. Replaces ranged weapon"), K::Ranged, TEXT("Burst"), 3 },
		{ TEXT("Arc Thrower"),      TEXT("LMB weapon: lightning jumps to 2 more enemies. Replaces ranged weapon"), K::Ranged, TEXT("Arc"), 4 },
		{ TEXT("Sledge Interrupt"), TEXT("RMB weapon: slow 360 slam, huge knockback. Replaces melee weapon"), K::Melee, TEXT("Sledge"), 1 },
		{ TEXT("Dash Pointer"),     TEXT("RMB weapon: 7 m dash, 2x damage to all on the line. Replaces melee weapon"), K::Melee, TEXT("Pointer"), 2 },
		{ TEXT("Bit Shifter"),      TEXT("RMB weapon: fast daggers: left, right, then a top-down 2x crit. Replaces melee weapon"), K::Melee, TEXT("Shifter"), 3 },
		{ TEXT("Crit Handler"),     TEXT("Shots have +20% chance to deal double damage (stacks)"), K::Offense, TEXT("Crit"), 0 },
		{ TEXT("Defrag"),           TEXT("Each melee hit heals 2 HP (+2 per stack)"), K::Defense, TEXT("Defrag"), 0 },
		{ TEXT("Heat Sink"),        TEXT("Melee cooldown -20% (stacks)"), K::Utility, TEXT("HeatSink"), 0 },
		{ TEXT("Uptime"),           TEXT("Regenerate 1 HP per second (+1 per stack)"), K::Defense, TEXT("Uptime"), 0 },
		// Drivers: the basic weapon (index 0) for an empty slot; OfferUpgrades only offers them while their slot is empty. They reuse the weapon-0 icons.
		{ TEXT("Scanner Driver"),   TEXT("LMB weapon: the standard scanner. Fills your empty ranged slot"), K::Ranged, TEXT("Scanner"), 0 },
		{ TEXT("Kernel Blade Core"), TEXT("RMB weapon: the standard blade, wide slash with a lunge. Fills your empty melee slot"), K::Melee, TEXT("Blade"), 0 },
	};
	// Weapon tables. Ranged: interval x FireInterval. Melee: damage/range/interval multiply the base (so Kernel Blade helps all), arc in degrees, lunge speed (Pointer: dash length), knockback.
	struct FKPRanged { const TCHAR* Name; const TCHAR* Icon; float Interval; };
	const FKPRanged GRanged[] = { { TEXT("Scanner"), TEXT("Scanner"), 1.f }, { TEXT("Scatter"), TEXT("Scatter"), 2.6f }, { TEXT("Lance"), TEXT("Lance"), 4.f },
		{ TEXT("Burst"), TEXT("Burst"), 1.6f }, { TEXT("Arc"), TEXT("Arc"), 1.5f } };
	struct FKPMelee { const TCHAR* Name; const TCHAR* Icon; float Dmg, Range, Arc, Interval, Lunge, Knock; };
	const FKPMelee GMelee[] = {
		{ TEXT("Blade"),   TEXT("Blade"),   1.f,   1.f,   120.f, 1.f,   500.f,  1100.f },
		{ TEXT("Sledge"),  TEXT("Sledge"),  1.5f,  1.2f,  360.f, 2.f,   0.f,    1800.f },
		{ TEXT("Pointer"), TEXT("Pointer"), 2.f,   1.f,   0.f,   1.8f,  700.f,  0.f },   // dash: Arc/Range unused, no knockback
		{ TEXT("Shifter"), TEXT("Shifter"), 0.5f,  0.85f, 100.f, 0.45f, 250.f,  300.f },
	};
	static_assert(UE_ARRAY_COUNT(GRanged) == AKPGameMode::NumRanged && UE_ARRAY_COUNT(GMelee) == AKPGameMode::NumMelee, "weapon tables must match NumRanged/NumMelee");
	// Effects per FX_SPEC.md. Ranged: colour, beam width/opacity/life, muzzle, then impacts + feedback (miss/hit/kill). Impact = {R0, R1, colour, opacity, life}.
	struct FKPRFx { FLinearColor C; float BeamW, BeamO, BeamLife; FKPImp Muz; FKPAtk A; };
	const FKPRFx GRFx[] = {
		{ FLinearColor(0.15f, 3.2f, 4.f), 2.4f, 1.f, 0.055f, { 3.f, 9.f, FLinearColor(0.15f, 3.2f, 4.f), 0.65f, 0.045f },        // Scanner
			{ { 5.f, 15.f, FLinearColor(0.15f, 3.2f, 4.f), 0.8f, 0.075f }, { 9.f, 28.f, FLinearColor(0.7f, 4.f, 4.5f), 0.9f, 0.12f }, 1, { 0.025f, 0.04f, 0.07f }, { 0.f, 0.f, 0.015f } } },
		{ FLinearColor(0.15f, 4.f, 2.5f), 3.2f, 0.85f, 0.065f, { 9.f, 24.f, FLinearColor(0.15f, 4.f, 2.5f), 0.7f, 0.065f },      // Scatter
			{ { 9.f, 25.f, FLinearColor(0.15f, 4.f, 2.5f), 0.85f, 0.09f }, { 15.f, 42.f, FLinearColor(0.6f, 4.5f, 3.f), 0.9f, 0.14f }, 1, { 0.12f, 0.16f, 0.2f }, { 0.f, 0.018f, 0.025f } } },
		{ FLinearColor(2.f, 5.f, 6.f), 7.f, 1.f, 0.22f, { 10.f, 30.f, FLinearColor(2.f, 5.f, 6.f), 0.85f, 0.12f },               // Lance
			{ { 12.f, 32.f, FLinearColor(2.f, 5.f, 6.f), 0.8f, 0.14f }, { 18.f, 48.f, FLinearColor(3.f, 6.f, 7.f), 0.9f, 0.2f }, 3, { 0.22f, 0.28f, 0.34f }, { 0.f, 0.035f, 0.05f } } },
		{ FLinearColor(0.15f, 1.8f, 5.f), 3.5f, 1.f, 0.045f, { 4.f, 11.f, FLinearColor(0.15f, 1.8f, 5.f), 0.7f, 0.04f },        // Burst (impact on packets 1 and 3 only)
			{ { 6.f, 18.f, FLinearColor(0.15f, 1.8f, 5.f), 0.8f, 0.05f }, { 10.f, 29.f, FLinearColor(0.7f, 2.8f, 6.f), 0.9f, 0.08f }, 1, { 0.06f, 0.09f, 0.12f }, { 0.f, 0.f, 0.f } } },
		{ FLinearColor(1.4f, 2.5f, 5.f), 3.8f, 1.f, 0.11f, { 5.f, 13.f, FLinearColor(1.4f, 2.5f, 5.f), 0.7f, 0.07f },           // Arc
			{ { 8.f, 22.f, FLinearColor(1.4f, 2.5f, 5.f), 0.8f, 0.1f }, { 13.f, 36.f, FLinearColor(2.2f, 3.5f, 6.f), 0.9f, 0.15f }, 1, { 0.06f, 0.1f, 0.14f }, { 0.f, 0.012f, 0.022f } } },
	};
	static_assert(UE_ARRAY_COUNT(GRFx) == AKPGameMode::NumRanged, "ranged FX table must match NumRanged");
	// Melee: swing ribbon colour + feedback. Rows 0..3 = weapons, 4 = Shifter finisher. Pointer's impact is its tip cue.
	struct FKPMFx { FLinearColor ArcC; FKPAtk A; };
	const FKPMFx GMFx[] = {
		{ FLinearColor(0.25f, 2.8f, 4.f), { { 8.f, 24.f, FLinearColor(3.5f, 4.f, 5.f), 0.85f, 0.1f }, { 14.f, 38.f, FLinearColor(5.f, 2.4f, 0.55f), 0.9f, 0.16f }, 2, { 0.05f, 0.19f, 0.25f }, { 0.f, 0.04f, 0.055f } } },      // Blade
		{ FLinearColor(1.3f, 3.2f, 4.f), { { 14.f, 35.f, FLinearColor(4.f, 4.5f, 5.f), 0.85f, 0.14f }, { 20.f, 48.f, FLinearColor(6.f, 2.5f, 0.4f), 0.9f, 0.2f }, 2, { 0.16f, 0.32f, 0.4f }, { 0.f, 0.06f, 0.08f } } },        // Sledge
		{ FLinearColor::White, { { 16.f, 40.f, FLinearColor(4.f, 4.5f, 5.f), 0.9f, 0.15f }, { 22.f, 48.f, FLinearColor(5.f, 2.5f, 0.6f), 0.95f, 0.2f }, 1, { 0.08f, 0.22f, 0.3f }, { 0.f, 0.035f, 0.05f } } },             // Pointer
		{ FLinearColor(0.7f, 2.8f, 4.5f), { { 5.f, 15.f, FLinearColor(3.f, 4.f, 5.f), 0.8f, 0.07f }, { 9.f, 26.f, FLinearColor(5.f, 2.7f, 0.7f), 0.9f, 0.11f }, 1, { 0.025f, 0.07f, 0.11f }, { 0.f, 0.012f, 0.02f } } },    // Shifter
		{ FLinearColor(2.f, 4.f, 5.f), { { 10.f, 28.f, FLinearColor(4.f, 5.f, 6.f), 0.85f, 0.1f }, { 15.f, 40.f, FLinearColor(6.f, 2.8f, 0.6f), 0.9f, 0.15f }, 2, { 0.f, 0.18f, 0.24f }, { 0.f, 0.035f, 0.05f } } },      // Shifter finisher
	};
	// Kill feedback by enemy type (EKPEnemyType order): hit-stop seconds, trauma.
	const float GKill[][2] = { { 0.04f, 0.14f }, { 0.04f, 0.14f }, { 0.07f, 0.26f }, { 0.2f, 0.9f }, { 0.2f, 0.9f }, { 0.2f, 0.9f } };
	const TCHAR* GKitNames[] = { TEXT("RANGED KIT"), TEXT("CLOSE KIT") };
	const TCHAR* GKitDescs[] = { TEXT("Scanner hits 50% harder. Fight from a distance."), TEXT("Blade hits 50% harder, +40 HP, +10% speed. Get in their face.") };
	const TCHAR* GKitIcons[] = { TEXT("KitRanged"), TEXT("KitClose") };
	const int32 GMaxFx = 48;

	// Run state that must survive the level load. Filled just before OpenLevel and only read when the URL has ?KPCarry=1,
	// so a fresh start, PIE restart or death restart never sees stale state.
	struct FKPCarry
	{
		bool bValid = false;
		int32 Wave = 0, Kills = 0, Kit = 0;
		float RunTime = 0.f, Health = 0.f;
		TArray<int32> Picks;
		FKPRunStats Stats;
	};
	FKPCarry GCarry;
	// Component index -> map. Lvl_GPU / Lvl_CPU may not exist yet (see TravelToNextComponent).
	const TCHAR* GMaps[] = { TEXT("/Game/KernelPanic/Maps/Lvl_Motherboard"), TEXT("/Game/KernelPanic/Maps/Lvl_GPU"), TEXT("/Game/KernelPanic/Maps/Lvl_CPU") };
	const int32 GFinalWave = 12; // 3 components x (3 waves + boss)
}

// Build affinity for weighted offers: ranged build (all Ranged cards + Overclock, Multithreading, Piercing, Targeting, Crit), melee build (all Melee cards + Kernel Blade, Defrag, Heat Sink).
int32 AKPGameMode::UpgradeAffinity(int32 I)
{
	return GUpgrades[I].Kind == K::Ranged || I == 0 || I == 1 || I == 3 || I == 8 || I == 19 ? 1 : GUpgrades[I].Kind == K::Melee || I == 7 || I == 20 || I == 21 ? 2 : 0;
}
int32 AKPGameMode::NumUpgrades() { return UE_ARRAY_COUNT(GUpgrades); }
const TCHAR* AKPGameMode::UpgradeName(int32 I) { return GUpgrades[I].Name; }
const TCHAR* AKPGameMode::UpgradeDesc(int32 I) { return GUpgrades[I].Desc; }
EKPCardKind AKPGameMode::UpgradeKind(int32 I) { return GUpgrades[I].Kind; }
const TCHAR* AKPGameMode::UpgradeIcon(int32 I) { return GUpgrades[I].Icon; }
const TCHAR* AKPGameMode::RangedName(int32 I) { return GRanged[FMath::Clamp(I, 0, NumRanged - 1)].Name; }
const TCHAR* AKPGameMode::RangedIcon(int32 I) { return GRanged[FMath::Clamp(I, 0, NumRanged - 1)].Icon; }
const TCHAR* AKPGameMode::MeleeName(int32 I) { return GMelee[FMath::Clamp(I, 0, NumMelee - 1)].Name; }
const TCHAR* AKPGameMode::MeleeIcon(int32 I) { return GMelee[FMath::Clamp(I, 0, NumMelee - 1)].Icon; }
const TCHAR* AKPGameMode::KitName(int32 I) { return GKitNames[FMath::Clamp(I, 0, 1)]; }
const TCHAR* AKPGameMode::KitDesc(int32 I) { return GKitDescs[FMath::Clamp(I, 0, 1)]; }
const TCHAR* AKPGameMode::KitIcon(int32 I) { return GKitIcons[FMath::Clamp(I, 0, 1)]; }
const TCHAR* AKPGameMode::WeaponName() const { return Weapon < 0 ? TEXT("none") : RangedName(Weapon); } // the Clamp in RangedName would show the Scanner for an empty slot
const TCHAR* AKPGameMode::MeleeWeaponName() const { return MeleeWeapon < 0 ? TEXT("none") : MeleeName(MeleeWeapon); }
FString AKPGameMode::BestCard() const
{
	for (int32 U : PickIdx) if (UpgradeKind(U) <= EKPCardKind::Melee) return UpgradeName(U);
	return PickIdx.Num() ? FString(UpgradeName(PickIdx[0])) : FString(TEXT("none"));
}
void AKPGameMode::InsertRecord(TArray<FKPRecord>& List, const FKPRecord& Run)
{
	List.Add(Run);
	List.StableSort([](const FKPRecord& A, const FKPRecord& B)
	{
		if (A.Wave != B.Wave) return A.Wave > B.Wave;
		if (A.bWon != B.bWon) return A.bWon;
		return A.bWon ? A.Seconds < B.Seconds : A.Seconds > B.Seconds;
	});
	if (List.Num() > NumRecords) List.SetNum(NumRecords);
}
float AKPGameMode::WeaponInterval() const { return FireInterval * GRanged[FMath::Clamp(Weapon, 0, NumRanged - 1)].Interval * (IsOverheated() ? 2.f : 1.f); }
float AKPGameMode::MeleeIntervalNow() const { return MeleeInterval * GMelee[FMath::Clamp(MeleeWeapon, 0, NumMelee - 1)].Interval; }

AKPGameMode::AKPGameMode()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostPhysics; // camera shake after the player moved
	// Keep the template's pawn and controller: they already handle move/look/jump input.
	static ConstructorHelpers::FClassFinder<APawn> PawnBP(TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"));
	if (PawnBP.Succeeded()) DefaultPawnClass = PawnBP.Class;
	static ConstructorHelpers::FClassFinder<APlayerController> PCBP(TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonPlayerController"));
	if (PCBP.Succeeded()) PlayerControllerClass = PCBP.Class;
	HUDClass = AKPHUD::StaticClass();
	// Models from Tools/ArtGen (Blender); hard refs here so they get cooked.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> HeroFinder(TEXT("/Game/KernelPanic/Models/SM_KP_Hero.SM_KP_Hero"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CoreFinder(TEXT("/Game/KernelPanic/Models/SM_KP_Core.SM_KP_Core"));
	HeroMesh = HeroFinder.Object;
	CoreMesh = CoreFinder.Object;
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> ClockFrontFinder(TEXT("/Game/KernelPanic/LevelCPU/SM_KP_CPU_ClockFront.SM_KP_CPU_ClockFront")); // optional: maps without it are unaffected
	ClockFrontMesh = ClockFrontFinder.Get();

	static const TCHAR* GSfxNames[] = { TEXT("Shoot"), TEXT("Melee"), TEXT("Hit"), TEXT("Kill"), TEXT("BoltFire"), TEXT("PlayerHurt"),
		TEXT("CoreHit"), TEXT("Firewall"), TEXT("Lock"), TEXT("WaveStart"), TEXT("Boss"), TEXT("Upgrade"), TEXT("RunLost"), TEXT("Shockwave"), TEXT("Scatter"), TEXT("Lance") };
	static_assert(UE_ARRAY_COUNT(GSfxNames) == (int32)EKPSfx::Count, "GSfxNames must match EKPSfx");
	for (const TCHAR* N : GSfxNames)
	{
		ConstructorHelpers::FObjectFinder<USoundBase> S(*FString::Printf(TEXT("/Game/KernelPanic/Audio/KP_%s.KP_%s"), N, N));
		Sounds.Add(S.Object); // null if missing -> PlaySfx just stays silent
	}
}

void AKPGameMode::PlaySfx(EKPSfx Id, float Volume, float Pitch)
{
	const bool bTransient = Id == EKPSfx::Shoot || Id == EKPSfx::Scatter || Id == EKPSfx::Lance || Id == EKPSfx::Melee
		|| Id == EKPSfx::Hit || Id == EKPSfx::Kill || Id == EKPSfx::BoltFire;
	if (Sounds.IsValidIndex((int32)Id) && Sounds[(int32)Id])
		UGameplayStatics::PlaySound2D(this, Sounds[(int32)Id], Volume, Pitch > 0.f ? Pitch : bTransient ? FMath::FRandRange(0.96f, 1.04f) : 1.f);
}

// Dev console commands. Open the console in Play with the ~ key (or use the editor's Cmd box during Play).
// The editor Cmd box passes the editor world, so fall back to the running game / PIE world.
// CLOCK EDGE execution tiles on the CPU map (Tools/Plans/CPU_LEVEL.md: X and Y each [-26,-6] or [6,26] m, in cm). Tile i = X range i & 1, Y range i >> 1.
static const float GClockLo[2] = { -2600.f, 600.f }, GClockHi[2] = { -600.f, 2600.f };

bool AKPGameMode::ClockPassSwept(float Lo, float Hi, float Dir, float T0, float T1, int32 Pass, float X, float R)
{
	const float S = ClockPassStart(Pass), D = ClockPassSeconds(Lo, Hi);
	if (T1 < S || T0 > S + D) return false; // this pass was not moving during the tick
	const float C0 = ClockFrontX(Lo, Hi, Dir, FMath::Clamp(T0 - S, 0.f, D)), C1 = ClockFrontX(Lo, Hi, Dir, FMath::Clamp(T1 - S, 0.f, D));
	return X >= FMath::Min(C0, C1) - 30.f - R && X <= FMath::Max(C0, C1) + 30.f + R; // front is 60 cm thick; the actor is a disc of radius R
}

static AKPGameMode* FindRunningGameMode(UWorld* World)
{
	if (AKPGameMode* GM = World ? World->GetAuthGameMode<AKPGameMode>() : nullptr) return GM;
	for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		if ((Ctx.WorldType == EWorldType::PIE || Ctx.WorldType == EWorldType::Game) && Ctx.World())
			if (AKPGameMode* GM = Ctx.World()->GetAuthGameMode<AKPGameMode>()) return GM;
	return nullptr;
}
static FAutoConsoleCommandWithWorldAndArgs GKPWaveCmd(
	TEXT("kp.Wave"), TEXT("Jump to a wave now, e.g. kp.Wave 8 (4 = Corrupted File, 8 = Ransomware, 12 = Rootkit + final). Stays on this map."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		AKPGameMode* GM = FindRunningGameMode(World);
		if (GM && Args.Num() > 0) GM->DebugJumpToWave(FCString::Atoi(*Args[0]));
	}));
static FAutoConsoleCommandWithWorldAndArgs GKPHazardCmd(
	TEXT("kp.Hazard"), TEXT("Start a random hazard event now (Power Surge, Overheat or Clock Edge, whichever the map supports)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
	{
		if (AKPGameMode* GM = FindRunningGameMode(World)) GM->DebugHazard();
	}));
static FAutoConsoleCommandWithWorldAndArgs GKPKillAllCmd(
	TEXT("kp.KillAll"), TEXT("Kill every enemy (counts as kills, so the upgrade cards appear)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
	{
		if (AKPGameMode* GM = FindRunningGameMode(World)) GM->DebugKillAll();
	}));

static bool GKPGod = false; // dev only: "kp.God" toggles no damage to player and core, for testing the run flow
static FAutoConsoleCommand GKPGodCmd(TEXT("kp.God"), TEXT("Toggle: player and core take no damage."),
	FConsoleCommandDelegate::CreateLambda([]() { GKPGod = !GKPGod; if (AKPGameMode* GM = FindRunningGameMode(nullptr)) GM->MarkSynthetic(); }));
static FAutoConsoleCommandWithWorldAndArgs GKPPickCmd(
	TEXT("kp.Pick"), TEXT("Pick the first upgrade card (same as key 1). kp.Pick N forces card index N (0..24; 9 Scatter, 15 Arc Thrower, 16 Sledge Interrupt)."),
	FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
	{
		AKPGameMode* GM = FindRunningGameMode(World);
		if (!GM) return;
		const int32 N = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : -1;
		if (N >= 0 && N < AKPGameMode::NumUpgrades() && GM->UpgradeOffer.Num() > 0) GM->UpgradeOffer[0] = N;
		GM->MarkSynthetic();
		GM->ChooseUpgrade(0);
	}));

void AKPGameMode::DebugJumpToWave(int32 NewWave)
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (bRunLost || bRunWon || NewWave < 1 || !PC || !PC->GetPawn()) return;
	if (!bStarted) StartRun();
	Stats.bSynthetic = true;
	if (bChoosingUpgrade) { bChoosingUpgrade = false; UGameplayStatics::SetGamePaused(this, false); }
	// Remove current enemies without kill credit or upgrade cards.
	for (TActorIterator<AKPEnemy> It(GetWorld()); It; ++It) It->Destroy();
	Alive = 0;
	Boss.Reset();
	for (double& T : LockedUntil) T = 0.0;
	Wave = NewWave - 1;
	StartNextWave(PC->GetPawn());
}

void AKPGameMode::DebugHazard()
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (bStarted && !bRunLost && !bRunWon && PC && PC->GetPawn()) { Stats.bSynthetic = true; StartHazard(PC); }
}

void AKPGameMode::DebugKillAll()
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	Stats.bSynthetic = true;
	TArray<AKPEnemy*> Targets;
	for (TActorIterator<AKPEnemy> It(GetWorld()); It; ++It)
		if (!It->IsDead() && It->Type != EKPEnemyType::Boss) Targets.Add(*It); // boss last: it splits into rushers
	for (TActorIterator<AKPEnemy> It(GetWorld()); It; ++It)
		if (!It->IsDead() && It->Type == EKPEnemyType::Boss) Targets.Add(*It);
	for (AKPEnemy* E : Targets)
		if (IsValid(E) && !E->IsDead()) DealEnemyDamage(E, 100000.f, PC, PC ? PC->GetPawn() : nullptr);
	// The Corrupted File's split rushers (and the Rootkit's children) spawn separately: clear them too.
	for (TActorIterator<AKPEnemy> It(GetWorld()); It; ++It)
		if (!It->IsDead()) DealEnemyDamage(*It, 100000.f, PC, PC ? PC->GetPawn() : nullptr);
}

void AKPGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);
	bCarryPending = GCarry.bValid && UGameplayStatics::HasOption(Options, TEXT("KPCarry"));
	if (!bCarryPending) GCarry = FKPCarry();
}

void AKPGameMode::RestartRun()
{
	GCarry = FKPCarry();
	UGameplayStatics::OpenLevel(this, FName(GMaps[0]));
}

void AKPGameMode::CollectTraces()
{
	// Circuit traces and GPU heat plates are plain level StaticMeshActors; none on a map = no Power Surge / Overheat there.
	for (TActorIterator<AStaticMeshActor> It(GetWorld()); It; ++It)
	{
		UStaticMeshComponent* C = It->GetStaticMeshComponent();
		if (!C || !C->GetStaticMesh()) continue;
		const FString Mesh = C->GetStaticMesh()->GetName();
		if (Mesh == TEXT("SM_KP_CPU_ClockRail5")) { ClockMats.Add(C->CreateDynamicMaterialInstance(0)); continue; } // CLOCK EDGE tile borders; any rail = this map's hazard
		const bool bHot = Mesh.StartsWith(TEXT("SM_KP_GPU_HotZone_")), bFan = Mesh.StartsWith(TEXT("SM_KP_GPU_FanPad"));
		if (bHot || bFan)
		{
			// shortcut: radius = smaller half-extent of the bounds, so octagons/ovals are a bit stingy; fine for round plates.
			const FBoxSphereBounds& B = C->Bounds;
			(bHot ? HeatZones : FanZones).Add(FVector4(B.Origin.X, B.Origin.Y, B.Origin.Z + B.BoxExtent.Z, FMath::Min(B.BoxExtent.X, B.BoxExtent.Y)));
			if (bHot) HeatMats.Add(C->CreateDynamicMaterialInstance(0));
			continue;
		}
		if (!Mesh.StartsWith(TEXT("SM_KP_MB_Trace_"))) continue;
		if (Mesh.Contains(TEXT("SpawnMark"))) continue; // enemies spawn there: a surge must not delete a fresh wave
		// shortcut: world AABB, so diagonal strips are generous; use oriented boxes if it feels unfair.
		TraceBoxes.Add(C->Bounds.GetBox().ExpandBy(FVector(60.f, 60.f, 0.f)));
		TraceMats.Add(C->CreateDynamicMaterialInstance(0));
	}
}

void AKPGameMode::ApplyKit(int32 K)
{
	Kit = K;
	Weapon = K == 0 ? 0 : -1; // each kit holds only its own weapon; the other slot waits for a driver card
	MeleeWeapon = K == 0 ? -1 : 0;
	if (K == 0) ShotDamage = 1.5f;
	else
	{
		MeleeDamage = 6.f;
		PlayerMaxHealth = PlayerHealth = 140.f;
		if (ACharacter* C = Cast<ACharacter>(UGameplayStatics::GetPlayerPawn(this, 0))) C->GetCharacterMovement()->MaxWalkSpeed *= 1.10f;
	}
}

void AKPGameMode::StartRun(int32 InKit)
{
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (bStarted || !PC || !PC->GetPawn()) return;
	bStarted = true;
	if (Kit < 0) ApplyKit(FMath::Clamp(InKit, 0, 1)); // a restored run already applied its carried kit in Tick
	RunStartedAt = FPlatformTime::Seconds();
	// A click that dismisses the briefing should not also fire a shot.
	FireCooldown = FireInterval;
	bBlockFireUntilRelease = true;
	Stats.WaveStartedAt = RunStartedAt;
	PC->SetIgnoreMoveInput(false);
	PC->SetIgnoreLookInput(false);
	CollectTraces();
	HazardNextAt = GetWorld()->GetTimeSeconds() + 12.0; // first hazard event
}

bool AKPGameMode::IsRunPaused() const
{
	// The upgrade freeze also pauses the game, but it is not the pause menu.
	return UGameplayStatics::IsGamePaused(this) && !bChoosingUpgrade;
}

void AKPGameMode::TogglePause()
{
	if (!bStarted || bRunLost || bRunWon || bChoosingUpgrade) return;
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!PC) return;
	const bool bPause = !IsRunPaused();
	if (!UGameplayStatics::SetGamePaused(this, bPause)) return;
	const double Now = FPlatformTime::Seconds();
	if (bPause) PauseStartedAt = Now;
	else if (PauseStartedAt >= 0.0)
	{
		const double Duration = Now - PauseStartedAt;
		PausedSeconds += Duration;
		// Gameplay deadlines freeze during pause; HUD motion still uses real time.
		auto ShiftDeadline = [&](double& Deadline)
		{
			if (Deadline > PauseStartedAt) Deadline += Duration;
		};
		if (BannerUntil > PauseStartedAt) BannerStartedAt += Duration;
		ShiftDeadline(BannerUntil);
		ShiftDeadline(HitMarkerUntil);
		ShiftDeadline(HurtFlashUntil);
		ShiftDeadline(HitStopUntil);
		ShiftDeadline(CoreFlashUntil);
		LastHurtTime += Duration;
		LastHurtSfx += Duration;
		LastCoreSfx += Duration;
		PauseStartedAt = -1.0;
	}
	PC->SetIgnoreMoveInput(bPause);
	PC->SetIgnoreLookInput(bPause);
}

float AKPGameMode::GetAbilityCooldown(int32 Ability) const
{
	return FMath::Max(0.f, Ability == 0 ? FireCooldown : Ability == 1 ? MeleeCooldown : Ability == 2 ? FirewallCooldown : 0.f);
}

float AKPGameMode::GetAbilityReadyFraction(int32 Ability) const
{
	const float Interval = Ability == 0 ? WeaponInterval() : Ability == 1 ? MeleeIntervalNow() : Ability == 2 ? FirewallCooldownTime : 0.f;
	return Interval > 0.f ? 1.f - FMath::Clamp(GetAbilityCooldown(Ability) / Interval, 0.f, 1.f) : 0.f;
}

float AKPGameMode::GetRunTime() const
{
	if (!bStarted) return 0.f;
	if (bRunLost || bRunWon) return FinalRunTime;
	const double End = PauseStartedAt >= 0.0 ? PauseStartedAt : FPlatformTime::Seconds();
	return FMath::Max(0.f, (float)(End - RunStartedAt - PausedSeconds));
}

float AKPGameMode::GetRestartTimeLeft() const
{
	return bRunLost ? FMath::Max(0.f, (float)(RestartAt - FPlatformTime::Seconds())) : 0.f;
}

void AKPGameMode::DealEnemyDamage(AKPEnemy* Enemy, float Damage, AController* DamageInstigator, AActor* Causer, EKPDealt Src)
{
	const bool bWasAlive = !Enemy->IsDead();
	const FVector At = Enemy->GetActorLocation(); // read before the kill destroys it
	const float HpBefore = Enemy->GetHealth();
	UGameplayStatics::ApplyDamage(Enemy, Damage, DamageInstigator, Causer, nullptr);
	Stats.DamageDealt[(int32)Src] += FMath::Max(0.f, HpBefore - FMath::Max(0.f, Enemy->GetHealth()));
	if (ChainDamage <= 0.f || !bWasAlive || !Enemy->IsDead() || !DamageInstigator || ChainDepth >= 4) return;
	// Chain Exploit: collect first (killing changes the actor list); each nested burst needs a new kill and depth is capped at 4.
	// It runs after OnEnemyRemoved returned, so wave-clear / upgrade offer logic never re-enters.
	TArray<AKPEnemy*> Near;
	for (TActorIterator<AKPEnemy> It(GetWorld()); It; ++It)
		if (*It != Enemy && !It->IsDead() && FVector::DistSquared(It->GetActorLocation(), At) < FMath::Square(300.f)) Near.Add(*It);
	++ChainDepth;
	for (AKPEnemy* E : Near)
		if (IsValid(E) && !E->IsDead()) DealEnemyDamage(E, ChainDamage, DamageInstigator, nullptr, EKPDealt::Chain);
	--ChainDepth;
}

void AKPGameMode::SetPlayerDefaults(APawn* PlayerPawn)
{
	Super::SetPlayerDefaults(PlayerPawn);
	ACharacter* Char = Cast<ACharacter>(PlayerPawn);
	if (!Char) return;

	// Shooter-style: body faces where the camera looks, so shots leave from the front.
	Char->GetCharacterMovement()->bOrientRotationToMovement = false;
	Char->GetCharacterMovement()->bUseControllerDesiredRotation = true;
	Char->GetCharacterMovement()->RotationRate = FRotator(0.f, 1080.f, 0.f);

	// Over-the-right-shoulder camera with a tighter FOV.
	if (USpringArmComponent* Arm = Char->FindComponentByClass<USpringArmComponent>())
		Arm->SocketOffset = BaseSocketOffset;
	if (UCameraComponent* Cam = Char->FindComponentByClass<UCameraComponent>())
		Cam->SetFieldOfView(70.f);

	// Hero look: hide the mannequin, show the floating anti-malware drone instead (no animation needed).
	if (HeroMesh)
	{
		Char->GetMesh()->SetHiddenInGame(true);
		HeroBody = NewObject<UStaticMeshComponent>(Char, TEXT("HeroBody"));
		HeroBody->SetStaticMesh(HeroMesh);
		HeroBody->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		HeroBody->SetupAttachment(Char->GetRootComponent());
		HeroBody->RegisterComponent();
	}
}

void AKPGameMode::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!bStarted && bCarryPending)
	{
		// Arrived from the previous component: restore the run and skip the briefing.
		APlayerController* P0 = GetWorld()->GetFirstPlayerController();
		if (!P0 || !P0->GetPawn()) return;
		bCarryPending = false;
		Wave = GCarry.Wave; Kills = GCarry.Kills; Stats = GCarry.Stats; PickIdx = GCarry.Picks;
		ApplyKit(GCarry.Kit); // kit first, then upgrades (the kit sets absolute values)
		for (int32 U : PickIdx) ApplyUpgrade(U); // same path as picking (weapon cards re-equip their weapons); analytics already live in the carried Stats
		UpgradesTaken = PickIdx.Num();
		PlayerHealth = FMath::Min(PlayerMaxHealth, GCarry.Health);
		StartRun();
		RunStartedAt -= GCarry.RunTime;
		Stats.WaveStartedAt = FPlatformTime::Seconds();
		GCarry.bValid = false;
		const int32 Comp = ComponentOf(Wave + 1);
		ShowBanner(FString::Printf(TEXT("%s / LEVEL %02d"), ComponentName(Comp), Comp + 1), 2.5);
	}
	if (!bStarted || IsRunPaused()) return;
	const double Now = FPlatformTime::Seconds();
	const float RealDt = FMath::Min((float)FApp::GetDeltaTime(), 0.1f);

	UpdateHitStop(Now);
	if (CoreMat && CoreFlashUntil > 0.0 && Now >= CoreFlashUntil)
	{
		CoreMat->SetVectorParameterValue(TEXT("Flash"), FLinearColor::Black);
		CoreFlashUntil = 0.0;
	}

	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (!PC || !PC->GetPawn()) return;
	UpdateShake(PC->GetPawn(), RealDt);

	// Drone hover: bob up/down and lean into movement (visual only).
	if (HeroBody)
	{
		const FVector Vel = PC->GetPawn()->GetVelocity();
		const FVector LocalVel = PC->GetPawn()->GetActorRotation().UnrotateVector(Vel);
		HeroBody->SetRelativeLocation(FVector(0.f, 0.f, 8.f * FMath::Sin(Now * 3.0)));
		HeroBody->SetRelativeRotation(FRotator(-LocalVel.X * 0.02f, 0.f, LocalVel.Y * 0.02f));
	}

	if (bRunLost)
	{
		if (Now >= RestartAt) RestartRun(); // death always restarts from component 0
		return;
	}
	if (bRunWon) return; // frozen behind the SYSTEM CLEAN overlay
	TickDash();
	TickBurst(PC);
	if (PlayerHealth < PlayerMaxHealth * 0.3f && Now >= NextBeatAt) // low HP: lub-dub heartbeat pulse in the camera
	{
		AddTrauma((BeatN & 1) ? 0.14f : 0.2f);
		NextBeatAt = Now + ((++BeatN & 1) ? 0.22 : 0.8);
	}

	// Shield recharges after 3s without damage.
	if (MaxShield > 0.f && Now - LastHurtTime > 3.0) Shield = FMath::Min(MaxShield, Shield + 20.f * DeltaTime);
	if (RegenPerSec > 0.f) PlayerHealth = FMath::Min(PlayerMaxHealth, PlayerHealth + RegenPerSec * DeltaTime); // Uptime

	FireCooldown -= DeltaTime;
	MeleeCooldown -= DeltaTime;
	FirewallCooldown -= DeltaTime;
	if (PC->WasInputKeyJustPressed(EKeys::E) && FirewallCooldown <= 0.f && Core && !IsLocked(2)) ActivateFirewall(PC->GetPawn());
	UpdateHazards(PC, DeltaTime);
	if (IsFirewallUp() && Core)
	{
		// Recharge window (design doc): heal while standing near the protected core.
		if (FVector::Dist2D(PC->GetPawn()->GetActorLocation(), Core->GetActorLocation()) < FirewallRadius)
			PlayerHealth = FMath::Min(PlayerMaxHealth, PlayerHealth + FirewallHealPerSec * DeltaTime);
	}

	const bool bMeleeKey = PC->WasInputKeyJustPressed(EKeys::RightMouseButton) || PC->WasInputKeyJustPressed(EKeys::F);
	auto Denied = [&]() { if (Now - LastDeniedSfx > 0.4) { LastDeniedSfx = Now; PlaySfx(EKPSfx::Lock, 0.3f); } }; // empty slot: quiet, at most every 0.4 s
	if (bMeleeKey && MeleeWeapon < 0) Denied();
	if (bMeleeKey && MeleeWeapon >= 0 && MeleeCooldown <= 0.f && !IsLocked(1))
	{
		Melee(PC->GetPawn());
		MeleeCooldown = MeleeIntervalNow();
		FireCooldown = FMath::Max(FireCooldown, 0.2f); // no shooting mid-swing
	}
	else if (bBlockFireUntilRelease)
	{
		if (!PC->IsInputKeyDown(EKeys::LeftMouseButton)) bBlockFireUntilRelease = false;
	}
	else if (PC->IsInputKeyDown(EKeys::LeftMouseButton) && Weapon < 0) Denied();
	else if (PC->IsInputKeyDown(EKeys::LeftMouseButton) && FireCooldown <= 0.f && !IsLocked(0))
	{
		Shoot(PC);
		FireCooldown = WeaponInterval(); // Overheat debuff is applied there, so upgrades never compound it
	}

	if (!bChoosingUpgrade && Alive == 0 && (WaveDelay -= DeltaTime) <= 0.f) StartNextWave(PC->GetPawn());
}

void AKPGameMode::Shoot(APlayerController* PC, bool bFollow)
{
	if (Weapon < 0) return; // empty slot: the GRanged / GRFx lookups below would clamp to the Scanner
	APawn* Pawn = PC->GetPawn();
	FVector CamLoc; FRotator CamRot;
	PC->GetPlayerViewPoint(CamLoc, CamRot);
	FCollisionQueryParams Params(TEXT("KPShot"), false, Pawn);
	if (!bFollow) { ++Stats.ShotsFired; BurstTrauma = 0.f; bBurstHit = false; BurstShot = 0; } // one per trigger pull, whatever the weapon

	// 1) What is under the crosshair?
	FHitResult AimHit;
	const FVector AimEnd = CamLoc + CamRot.Vector() * 10000.f;
	FVector AimPoint = GetWorld()->LineTraceSingleByChannel(AimHit, CamLoc, AimEnd, ECC_Visibility, Params) ? AimHit.ImpactPoint : AimEnd;

	// Soft aim assist: an enemy close to the crosshair pulls the shot onto the middle of its model.
	const AKPEnemy* Assist = FindAssistTarget(CamLoc, CamRot.Vector(), Params);
	if (Assist) AimPoint = Assist->GetAimPoint();

	// 2) Shoot from the right shoulder toward that point.
	const FVector Muzzle = Pawn->GetActorLocation() + Pawn->GetActorRightVector() * 35.f + FVector(0, 0, 50.f) + Pawn->GetActorForwardVector() * 40.f;
	const FVector Dir = (AimPoint - Muzzle).GetSafeNormal();
	const FKPRFx& R = GRFx[FMath::Clamp(Weapon, 0, NumRanged - 1)];

	// One ray = the old shot. Walls/floor: thin line (so a thick shot never clips the floor at range).
	// Enemies: a thick tube around that line, so near-misses on small fast enemies still connect.
	int32 Hits = 0;
	bool bAssisted = false, bAlive = false, bKill = false;
	AKPEnemy* FirstHit = nullptr; // first enemy any Ray hit (Arc jumps from it)
	FVector FirstAt = FVector::ZeroVector;
	TArray<FKPContact> Cs; // where the shot touched something (impact cues are picked from these)
	TArray<FVector> Ends;  // each ray's actual end, for the beams
	auto Ray = [&](const FVector& D, float MaxRange, float Damage, float Radius, int32 MaxHits)
	{
		float Range = MaxRange;
		FHitResult Wall;
		const bool bWall = GetWorld()->LineTraceSingleByObjectType(Wall, Muzzle, Muzzle + D * Range, FCollisionObjectQueryParams(ECC_WorldStatic), Params);
		if (bWall) Range = Wall.Distance;

		TArray<TPair<float, AKPEnemy*>> InTube;
		for (TActorIterator<AKPEnemy> It(GetWorld()); It; ++It)
		{
			AKPEnemy* E = *It;
			if (E->IsDead()) continue;
			const FVector C = E->GetAimPoint();
			const float Along = FVector::DotProduct(C - Muzzle, D);
			if (Along < 0.f || Along > Range) continue;
			const float Off = FVector::Dist(C, Muzzle + D * Along);
			if (Off <= Radius + E->GetBodyRadius()) InTube.Add({ Along, E });
		}
		InTube.Sort([](const TPair<float, AKPEnemy*>& A, const TPair<float, AKPEnemy*>& B) { return A.Key < B.Key; });

		FVector LineEnd = Muzzle + D * Range;
		int32 Done = 0;
		for (int32 i = 0; i < InTube.Num() && Done < MaxHits; ++i)
		{
			AKPEnemy* Enemy = InTube[i].Value;
			if (Enemy->IsDead()) continue; // a Chain Exploit burst from an earlier hit may have killed it
			if (!FirstHit) { FirstHit = Enemy; FirstAt = Enemy->GetAimPoint(); }
			DealEnemyDamage(Enemy, FMath::FRand() < CritChance ? Damage * 2.f : Damage, PC, Pawn, EKPDealt::Ranged); // Crit Handler
			++Done; ++Hits;
			if (Assist == Enemy) bAssisted = true;
			const bool bDied = Enemy->IsDead();
			(bDied ? bKill : bAlive) = true;
			Cs.Add({ Muzzle + D * InTube[i].Key, InTube[i].Key, bDied, false });
			if (Done == MaxHits) LineEnd = Muzzle + D * InTube[i].Key;
		}
		if (bWall && Done < MaxHits) Cs.Add({ Wall.ImpactPoint + Wall.ImpactNormal * 3.f, Range, false, true });
		Ends.Add(LineEnd);
	};

	FVector Kick = -Dir; // the camera jolts back
	FVector MuzzleAt = Muzzle;
	TArray<TPair<FVector, FVector>> Links; // Arc: extra lightning links
	if (Weapon == 1) // Scatter: 7 weak pellets in a ~9 degree cone, short range
	{
		for (int32 i = 0; i < 7; ++i) Ray(FMath::VRandCone(Dir, FMath::DegreesToRadians(4.5f)), 1500.f, ShotDamage * 0.45f, ShotRadius, Pierce + 1);
		MuzzleAt += Pawn->GetActorForwardVector() * 5.f;
		if (ACharacter* Ch = Cast<ACharacter>(Pawn)) Ch->LaunchCharacter(-Dir.GetSafeNormal2D() * 250.f, true, false); // shotgun kick
		PlaySfx(EKPSfx::Scatter, 0.85f);
	}
	else if (Weapon == 2) // Lance: one heavy shot through everything on the line (Pierce ignored), a bit thicker
	{
		Ray(Dir, 10000.f, ShotDamage * 4.f, ShotRadius * 1.5f, 9999);
		Kick *= 2.f; // heavier recoil
		PlaySfx(EKPSfx::Lance, 0.85f);
	}
	else if (Weapon == 3) // Burst: one ray per packet, re-aimed each time (packets 2 and 3 come from TickBurst)
	{
		Ray(FMath::VRandCone(Dir, FMath::DegreesToRadians(0.75f)), 10000.f, ShotDamage * 0.6f, ShotRadius, Pierce + 1);
		PlaySfx(EKPSfx::Shoot, 0.6f);
	}
	else if (Weapon == 4) // Arc: one ray, then lightning jumps to the 2 nearest other enemies within 5 m of the first hit
	{
		Ray(Dir, 10000.f, ShotDamage * 0.8f, ShotRadius, 1);
		if (FirstHit)
		{
			TArray<AKPEnemy*> Near;
			for (TActorIterator<AKPEnemy> It(GetWorld()); It; ++It)
				if (*It != FirstHit && !It->IsDead() && FVector::DistSquared(It->GetActorLocation(), FirstAt) < FMath::Square(500.f)) Near.Add(*It);
			Near.Sort([&](const AKPEnemy& A, const AKPEnemy& B) { return FVector::DistSquared(A.GetActorLocation(), FirstAt) < FVector::DistSquared(B.GetActorLocation(), FirstAt); });
			for (int32 i = 0; i < FMath::Min(2, Near.Num()); ++i)
			{
				AKPEnemy* E = Near[i];
				if (!IsValid(E) || E->IsDead()) continue; // a Chain Exploit burst from an earlier jump may have killed it
				const FVector To = E->GetAimPoint();
				DealEnemyDamage(E, ShotDamage * 0.8f, PC, Pawn, EKPDealt::Ranged);
				Links.Add({ FirstAt, To });
				Cs.Add({ To, FVector::Dist(Muzzle, To), E->IsDead(), false });
				++Hits;
				(E->IsDead() ? bKill : bAlive) = true;
			}
		}
		PlaySfx(EKPSfx::Lance, 0.5f);
	}
	else // Scanner
	{
		Ray(Dir, 10000.f, ShotDamage, ShotRadius, Pierce + 1);
		PlaySfx(EKPSfx::Shoot, 0.45f);
	}

	// Effects: raw pooled spheres/beams per the spec (beams, muzzle, then the few selected impacts).
	if (Weapon == 4)
	{
		// Jagged lightning: each link is two beams bent at a frozen kink point K (random side, perpendicular in the floor plane).
		auto Jag = [&](const FVector& A, const FVector& B)
		{
			const float L = FVector::Dist(A, B);
			if (L < 1.f) return;
			FVector V = FVector::CrossProduct(B - A, FVector::UpVector).GetSafeNormal();
			if (V.IsNearlyZero()) V = FVector::ForwardVector; // vertical link
			const FVector K = (A + B) * 0.5f + V * (FMath::RandBool() ? 1.f : -1.f) * FMath::FRandRange(FMath::Min(12.f, 0.08f * L), FMath::Min(28.f, 0.16f * L));
			Bm(A, K, R.BeamW, R.BeamW * 0.2f, R.C, R.BeamO, R.BeamLife);
			Bm(K, B, R.BeamW, R.BeamW * 0.2f, R.C, R.BeamO, R.BeamLife);
		};
		Jag(Muzzle, Ends[0]);
		for (const TPair<FVector, FVector>& L : Links) Jag(L.Key, L.Value);
	}
	else
		for (const FVector& E : Ends)
		{
			if (Weapon == 2) Bm(Muzzle, E, 22.f, 11.f, FLinearColor(0.6f, 1.5f, 1.8f), 0.25f, R.BeamLife); // Lance envelope around the core
			Bm(Muzzle, E, R.BeamW, R.BeamW * 0.2f, R.C, R.BeamO, R.BeamLife);
		}
	Sp(MuzzleAt, R.Muz.R0, R.Muz.R1, R.Muz.C, R.Muz.O, R.Muz.Life);
	if (Weapon == 2) Bm(Muzzle - Pawn->GetActorRightVector() * 18.f, Muzzle + Pawn->GetActorRightVector() * 18.f, 4.f, 0.8f, R.C, 0.8f, 0.1f); // crossbar
	FKPAtk A = R.A;
	if (Weapon == 3 && BurstShot == 1) A.Max = 0; // the middle packet has no impact sphere
	Cues(A, Cs);

	// Feedback once per shot (a Scatter volley must not stack seven hit sounds; a Burst adds only what the best packet exceeds).
	const int32 Outcome = Hits ? (bKill ? 2 : 1) : 0;
	Feel(A, Outcome, Kick, BurstTrauma);
	if (Weapon == 3) BurstTrauma = FMath::Max(BurstTrauma, A.Trauma[Outcome]);
	if (Hits > 0)
	{
		if (!bBurstHit) ++Stats.ShotHits;
		bBurstHit = Weapon == 3;
		HitMarkerUntil = FPlatformTime::Seconds() + 0.12;
		bHitMarkerKill = bKill;
		if (bAlive) PlaySfx(EKPSfx::Hit, 0.6f);
	}
	if (bAssisted) ++Stats.AssistedShots;
	if (Weapon == 3 && !bFollow) { BurstLeft = 2; BurstNextAt = GetWorld()->GetTimeSeconds() + 0.07; }
}

void AKPGameMode::TickBurst(APlayerController* PC)
{
	if (BurstLeft <= 0) return;
	if (Weapon != 3 || IsLocked(0) || bRunLost) { BurstLeft = 0; return; } // locked or swapped away: the rest is cancelled
	if (GetWorld()->GetTimeSeconds() < BurstNextAt) return;
	BurstShot = 3 - BurstLeft--;
	BurstNextAt += 0.07;
	Shoot(PC, true);
}

void AKPGameMode::Cues(const FKPAtk& A, TArray<FKPContact>& Cs)
{
	Cs.Sort([](const FKPContact& X, const FKPContact& Y) { return X.bKill != Y.bKill ? X.bKill : X.Key < Y.Key; });
	for (int32 i = 0; i < FMath::Min(A.Max, Cs.Num()); ++i)
	{
		const FKPImp& I = Cs[i].bKill ? A.Kill : A.Hit; // kill replaces hit; wall contacts are the hit sphere at half opacity
		Sp(Cs[i].At, I.R0, I.R1, I.C, Cs[i].bWall ? I.O * 0.5f : I.O, I.Life);
	}
}

void AKPGameMode::Feel(const FKPAtk& A, int32 Outcome, const FVector& Kick, float Already)
{
	AddTrauma(FMath::Max(0.f, A.Trauma[Outcome] - Already), Kick);
	if (A.Stop[Outcome] > 0.f) HitStop(A.Stop[Outcome], 0.05f);
}

AKPFx* AKPGameMode::Sp(const FVector& At, float R0, float R1, const FLinearColor& C, float O, float Life)
{
	return PlayFx(true, At, FRotator::ZeroRotator, FVector(R0 / 50.f), FVector(R1 / 50.f), C, Life, O, false); // sphere mesh radius is 50 cm
}

AKPFx* AKPGameMode::Bm(const FVector& A, const FVector& B, float W0, float W1, const FLinearColor& C, float O, float Life)
{
	const float L = FVector::Dist(A, B);
	if (L < 1.f) return nullptr;
	return PlayFx(false, (A + B) * 0.5f, FRotationMatrix::MakeFromZ(B - A).Rotator(), FVector(W0 / 100.f, W0 / 100.f, L / 100.f), FVector(W1 / 100.f, W1 / 100.f, L / 100.f), C, Life, O, false);
}

AKPFx* AKPGameMode::PlayFx(bool bSphere, const FVector& At, const FRotator& Rot, const FVector& S0, const FVector& S1, const FLinearColor& Color, float Life, float Opacity, bool bDome)
{
	AKPFx* F = nullptr;
	if (bDome || FxPool.Num() < GMaxFx)
	{
		F = GetWorld()->SpawnActor<AKPFx>(AKPFx::StaticClass(), At, Rot);
		if (!bDome && IsValid(F)) FxPool.Add(F);
	}
	else F = FxPool[FxNext++ % GMaxFx]; // pool full: recycle the next slot in ring order
	if (IsValid(F)) F->Play(bSphere, At, Rot, S0, S1, Color, Life, Opacity, bDome);
	return F;
}

void AKPGameMode::FxBeam(const FVector& A, const FVector& B, float Width, const FLinearColor& Color, float Life)
{
	const float Len = FVector::Dist(A, B);
	if (Len < 1.f) return;
	if (Width >= 7.f)
	{
		PlayFx(false, (A + B) * 0.5f, FRotationMatrix::MakeFromZ(B - A).Rotator(), FVector(Width / 100.f, Width / 100.f, Len / 100.f),
			FVector(Width * 0.5f / 100.f, Width * 0.5f / 100.f, Len / 100.f), Color * 0.3f, Life, 0.25f, false);
		Width *= 0.32f; // Bright narrow core inside the colored Lance envelope.
	}
	// Cylinder mesh is 100 cm across and 100 cm tall along Z: stretch Z to the length, taper the width as it fades.
	Bm(A, B, Width, Width * 0.2f, Color, 1.f, Life);
}

void AKPGameMode::FxBurst(const FVector& At, float R0, float R1, const FLinearColor& Color, float Life)
{
	PlayFx(true, At, FRotator::ZeroRotator, FVector(R0 / 50.f), FVector(R1 / 50.f), Color, Life, 0.8f, false); // sphere mesh radius is 50 cm
	if (R1 >= 50.f)
		for (int32 i = 0; i < 6; ++i)
		{
			const FVector Direction = FRotator(i % 2 ? 25.f : -15.f, i * 60.f, 0.f).Vector();
			FxBeam(At + Direction * R0, At + Direction * R1 * 1.2f, 3.f, Color, Life * 0.7f);
		}
}

AKPEnemy* AKPGameMode::FindAssistTarget(const FVector& CamLoc, const FVector& CamDir, const FCollisionQueryParams& Params) const
{
	// Cone grows with AimAssist (0..1). Picks the visible enemy closest to the crosshair direction.
	const float ConeDeg = AimAssist * MaxAssistDeg;
	if (ConeDeg <= 0.f) return nullptr;
	const float MinCos = FMath::Cos(FMath::DegreesToRadians(ConeDeg));
	AKPEnemy* Best = nullptr;
	float BestCos = MinCos;
	for (TActorIterator<AKPEnemy> It(GetWorld()); It; ++It)
	{
		AKPEnemy* E = *It;
		if (E->IsDead()) continue;
		const FVector To = E->GetAimPoint() - CamLoc;
		if (To.SizeSquared() > FMath::Square(6000.f)) continue;
		const float Cos = FVector::DotProduct(To.GetSafeNormal(), CamDir);
		if (Cos <= BestCos) continue;
		FHitResult Block; // only assist onto enemies the player can actually see
		if (GetWorld()->LineTraceSingleByObjectType(Block, CamLoc, E->GetAimPoint(), FCollisionObjectQueryParams(ECC_WorldStatic), Params)) continue;
		Best = E;
		BestCos = Cos;
	}
	return Best;
}

void AKPGameMode::Melee(APawn* Pawn)
{
	// Close combat. GMelee shapes the swing, GMFx its effects and feel.
	if (MeleeWeapon < 0) return; // empty slot: the clamp below would swing the Blade
	const int32 Wi = FMath::Clamp(MeleeWeapon, 0, NumMelee - 1);
	if (Wi == 2) { Dash(Pawn); return; }
	const FKPMelee& W = GMelee[Wi];
	const double Now = FPlatformTime::Seconds();
	const FVector Origin = Pawn->GetActorLocation();
	const FVector Fwd = Pawn->GetActorForwardVector().GetSafeNormal2D();
	const float Range = MeleeRange * W.Range;
	float Arc = W.Arc, Dmg = MeleeDamage * W.Dmg;
	int32 Side = 0;
	bool bFin = false;
	if (Wi == 3) // Shifter combo: swing from the left, swing from the right, then a top-down crit (x2); resets after 1.2 s idle
	{
		if (Now - ShifterLastAt > 1.2) ShifterChain = 0;
		ShifterLastAt = Now;
		Side = ShifterChain == 0 ? -1 : 1;
		if ((bFin = ShifterChain == 2)) Dmg *= 2.f;
		ShifterChain = (ShifterChain + 1) % 3;
	}
	const float CosHalfArc = FMath::Cos(FMath::DegreesToRadians(Arc * 0.5f)) - 0.001f; // epsilon: a 360 degree swing must hit directly behind too

	if (ACharacter* Char = Cast<ACharacter>(Pawn); Char && W.Lunge > 0.f) Char->LaunchCharacter(Fwd * W.Lunge, true, false);
	++Stats.MeleeSwings;

	int32 Hits = 0;
	bool bKilled = false;
	TArray<FKPContact> Cs;
	APlayerController* PC = Cast<APlayerController>(Pawn->GetController());
	for (TActorIterator<AKPEnemy> It(GetWorld()); It; ++It)
	{
		AKPEnemy* E = *It;
		if (E->IsDead()) continue;
		const FVector To = E->GetActorLocation() - Origin;
		const float Reach = Range + E->GetSimpleCollisionRadius();
		if (To.Size2D() > Reach || FMath::Abs(To.Z) > 200.f) continue;
		if (FVector::DotProduct(To.GetSafeNormal2D(), Fwd) < CosHalfArc) continue;

		const FVector At = E->GetAimPoint(); // before the kill destroys it
		DealEnemyDamage(E, Dmg, PC, Pawn, EKPDealt::Melee);
		++Stats.MeleeHits;
		PlayerHealth = FMath::Min(PlayerMaxHealth, PlayerHealth + HealPerHit); // Defrag
		const bool bDied = E->IsDead();
		if (bDied) bKilled = true;
		else E->Knockback(To, W.Knock);
		Cs.Add({ At, To.Size2D(), bDied, false });
		++Hits;
	}
	if (Hits) ++Stats.MeleeSwingsHit;

	// Reuse the effect pool; the ribbon's leading edge matches the damage range (Shifter: the spec's short cuts).
	const FKPMFx& F = GMFx[bFin ? 4 : Wi];
	const FVector Up(0.f, 0.f, 1.f);
	const float Yaw = Fwd.Rotation().Yaw;
	auto Rib = [&](const FVector& At, float Yw, float R, float Deg, float Roll = 0.f) // Roll 90 = vertical ribbon
	{
		if (AKPFx* S = PlayFx(false, At, FRotator::ZeroRotator, FVector::OneVector, FVector::OneVector, FLinearColor::White, 0.22f, 1.f, false)) S->PlayArc(At, FRotator(0.f, Yw, Roll), R, Deg, F.ArcC);
	};
	if (Wi == 1) // Sledge: flat shockwave ring on the floor + central strike
	{
		const FVector G = Origin - FVector(0.f, 0.f, Pawn->GetSimpleCollisionHalfHeight());
		Rib(G + Up * 5.f, 0.f, Range, 360.f);
		Sp(G + Up * 12.f, 12.f, 42.f, FLinearColor(3.5f, 4.5f, 5.f), 0.8f, 0.14f);
	}
	else if (Wi == 3)
	{
		const float K = Range / 187.f; // spec sizes are for the base 187 reach
		if (bFin) { Rib(Origin, Yaw, 187.f * K, 120.f, 90.f); Rib(Origin, Yaw, 187.f * K, 120.f, -90.f); } // top-down crit; both rolls so the ribbon shows from either side
		else Rib(Origin - Up * 20.f + Pawn->GetActorRightVector() * (Side * 22.f * K), Yaw - Side * 25.f, 150.f * K, 55.f);
	}
	else Rib(Origin - Up * 30.f, Yaw, Range, Arc);
	Cues(F.A, Cs);

	const int32 Outcome = Hits ? (bKilled ? 2 : 1) : 0;
	Feel(F.A, Outcome, Fwd * (Hits ? 1.f : 0.5f)); // camera jolts along the swing, lighter on a whiff
	if (Hits > 0)
	{
		HitMarkerUntil = FPlatformTime::Seconds() + 0.15;
		bHitMarkerKill = bKilled;
		if (!bKilled) PlaySfx(EKPSfx::Hit);
	}
	PlaySfx(EKPSfx::Melee);
}

// Pointer: launch ~700 cm along the aim (flattened), stop at walls. Everything within 110 cm of the start->end segment is hit once, now.
void AKPGameMode::Dash(APawn* Pawn)
{
	ACharacter* Char = Cast<ACharacter>(Pawn);
	if (!Char) return;
	const FKPMelee& W = GMelee[2];
	const FVector S = Pawn->GetActorLocation(), Dir = Pawn->GetActorForwardVector().GetSafeNormal2D();
	UCapsuleComponent* Cap = Char->GetCapsuleComponent();
	const float Rad = Cap->GetScaledCapsuleRadius(), HH = Cap->GetScaledCapsuleHalfHeight();
	float Travel = W.Lunge;
	FHitResult Wall;
	// Capsule lifted/shortened 6 cm so the floor under the feet is not a wall.
	const FVector Lift(0.f, 0.f, 6.f);
	if (GetWorld()->SweepSingleByObjectType(Wall, S + Lift, S + Lift + Dir * Travel, FQuat::Identity, FCollisionObjectQueryParams(ECC_WorldStatic),
		FCollisionShape::MakeCapsule(Rad, FMath::Max(Rad, HH - 6.f)), FCollisionQueryParams(TEXT("KPDash"), false, Pawn)))
		Travel = FMath::Max(0.f, Wall.Distance);
	const FVector T = S + Dir * Travel;
	++Stats.MeleeSwings;

	int32 Hits = 0;
	bool bKilled = false;
	TArray<FKPContact> Cs;
	APlayerController* PC = Cast<APlayerController>(Pawn->GetController());
	for (TActorIterator<AKPEnemy> It(GetWorld()); It; ++It)
	{
		AKPEnemy* E = *It;
		const FVector L = E->GetActorLocation();
		if (E->IsDead() || FMath::Abs(L.Z - S.Z) > 200.f || FMath::PointDistToSegment(FVector(L.X, L.Y, 0.f), FVector(S.X, S.Y, 0.f), FVector(T.X, T.Y, 0.f)) > 110.f) continue;
		const FVector At = E->GetAimPoint();
		DealEnemyDamage(E, MeleeDamage * W.Dmg, PC, nullptr, EKPDealt::Melee); // no causer: TakeDamage would shove the enemy (the dash does no knockback)
		++Stats.MeleeHits;
		PlayerHealth = FMath::Min(PlayerMaxHealth, PlayerHealth + HealPerHit); // Defrag
		const bool bDied = E->IsDead();
		bKilled |= bDied;
		Cs.Add({ At, FVector::Dist(S, At), bDied, false });
		++Hits;
	}
	if (Hits) ++Stats.MeleeSwingsHit;
	Cs.Sort([](const FKPContact& X, const FKPContact& Y) { return X.bKill != Y.bKill ? X.bKill : X.Key < Y.Key; });

	// Dash state (visuals and feedback finish in TickDash). Ignoring pawn collision lets the player pass through enemies.
	const double Now = GetWorld()->GetTimeSeconds();
	DashT0 = Now; DashUntil = Now + 0.18; DashGraceUntil = Now + 0.25;
	DashTravel = Travel; DashSecs = FMath::Min(3, FMath::CeilToInt(Travel / 233.33f)); DashSecDone = 0;
	DashFrom = S + FVector(0.f, 0.f, 10.f); DashTo = T + FVector(0.f, 0.f, 10.f);
	DashOutcome = Hits ? (bKilled ? 2 : 1) : 0;
	bDashTip = Hits > 0;
	if (bDashTip) DashTipAt = Cs[0].At;
	Cap->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	Char->LaunchCharacter(Dir * (Travel / 0.18f) + FVector(0.f, 0.f, 150.f), true, true);
	Sp(DashFrom, 8.f, 22.f, FLinearColor(0.4f, 3.f, 4.f), 0.65f, 0.1f);

	if (Hits > 0)
	{
		HitMarkerUntil = FPlatformTime::Seconds() + 0.15;
		bHitMarkerKill = bKilled;
		if (!bKilled) PlaySfx(EKPSfx::Hit);
	}
	PlaySfx(EKPSfx::Melee);
}

void AKPGameMode::TickDash(bool bForceEnd)
{
	if (DashUntil <= 0.0) return;
	const double T = GetWorld()->GetTimeSeconds();
	const float Fr = bForceEnd ? 1.f : (float)((T - DashT0) / 0.18);
	const FVector Dir = (DashTo - DashFrom).GetSafeNormal();
	for (; DashSecDone < DashSecs; ++DashSecDone) // a trail section appears as the drone completes it
	{
		const float A = DashSecDone * 233.33f, B = FMath::Min(A + 233.33f, DashTravel);
		if (Fr < B / DashTravel) break;
		Bm(DashFrom + Dir * A, DashFrom + Dir * B, 12.f, 2.4f, FLinearColor(0.35f, 3.2f, 4.5f), 0.75f, 0.28f);
	}
	if (Fr < 1.f) return;
	DashUntil = 0.0;
	if (ACharacter* Char = Cast<ACharacter>(UGameplayStatics::GetPlayerPawn(this, 0)))
	{
		Char->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
		Char->GetCharacterMovement()->Velocity.X = Char->GetCharacterMovement()->Velocity.Y = 0.f; // stop dead at the end point
	}
	const float K = FMath::Min(1.f, DashTravel / 35.f);
	const FVector Rt = FVector::CrossProduct(FVector::UpVector, Dir);
	for (float Sg : { -1.f, 1.f }) Bm(DashTo - Dir * (35.f * K) + Rt * (Sg * 22.f * K), DashTo, 5.f, 1.f, FLinearColor(2.5f, 4.f, 5.f), 0.9f, 0.16f); // arrowhead
	const FKPAtk& A = GMFx[2].A;
	if (!bDashTip) Sp(DashTo, 8.f, 20.f, FLinearColor(1.5f, 3.f, 4.f), 0.6f, 0.1f);
	else { const FKPImp& I = DashOutcome == 2 ? A.Kill : A.Hit; Sp(DashTipAt, I.R0, I.R1, I.C, I.O, I.Life); }
	Feel(A, DashOutcome, Dir); // after the dash, so movement stays continuous
}

void AKPGameMode::SpawnCore(APawn* Player)
{
	// Level designers place an actor tagged "KP_Core" (e.g. a TargetPoint); otherwise the core goes in front of the player.
	FVector Loc = Player->GetActorLocation() + Player->GetActorForwardVector() * 500.f;
	TArray<AActor*> Marks;
	UGameplayStatics::GetAllActorsWithTag(this, TEXT("KP_Core"), Marks);
	if (Marks.Num() > 0) Loc = Marks[0]->GetActorLocation();
	FHitResult Floor;
	if (GetWorld()->LineTraceSingleByObjectType(Floor, Loc + FVector(0, 0, 500.f), Loc - FVector(0, 0, 2000.f), FCollisionObjectQueryParams(ECC_WorldStatic)))
		Loc = Floor.ImpactPoint;

	// SM_KP_Core fits a 1 m box centered on its origin (0.95 m tall) -> lift by half its scaled height.
	const float CoreScale = 2.4f;
	AStaticMeshActor* CoreActor = GetWorld()->SpawnActor<AStaticMeshActor>(Loc + FVector(0, 0, 47.5f * CoreScale), FRotator::ZeroRotator);
	if (!CoreActor) return;
	UStaticMeshComponent* M = CoreActor->GetStaticMeshComponent();
	M->SetMobility(EComponentMobility::Movable);
	M->SetStaticMesh(CoreMesh);
	CoreActor->SetActorScale3D(FVector(CoreScale));
	CoreMat = M->CreateDynamicMaterialInstance(0);
	Core = CoreActor;
}

AKPEnemy* AKPGameMode::SpawnEnemy(EKPEnemyType Type, const FVector& Location)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;
	const float Lift = Type >= EKPEnemyType::Boss ? 200.f : (Type == EKPEnemyType::Tank ? 100.f : 50.f); // bosses are the types from Boss up
	if (AKPEnemy* E = GetWorld()->SpawnActor<AKPEnemy>(AKPEnemy::StaticClass(), Location + FVector(0, 0, Lift), FRotator::ZeroRotator, Params))
	{
		E->Init(Type, Wave);
		if (E->IsBossType()) Boss = E; // the bar follows the newest boss and hands over in OnEnemyRemoved
		++Alive;
		return E;
	}
	return nullptr;
}

void AKPGameMode::StartNextWave(APawn* Player)
{
	if (!Core) SpawnCore(Player);
	if (Wave == 0 && DebugStartWave > 1) { Wave = DebugStartWave - 1; Stats.bSynthetic = true; }
	// Dev: pressing Play directly on a later map starts at that component, so the right boss and label show.
	else if (Wave == 0)
		for (int32 i = 1; i < 3; ++i)
			if (GetWorld()->GetMapName().EndsWith(FPackageName::GetShortName(GMaps[i]))) Wave = i * 4;
	++Wave;
	Stats.WaveStartedAt = FPlatformTime::Seconds();
	WaveDelay = 2.f;

	// Spawn only on the navmesh (= inside the arena), not too close to the player.
	UNavigationSystemV1* Nav = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	const FVector Center = Core ? Core->GetActorLocation() : Player->GetActorLocation();
	// Level spawn zones: actors tagged "KP_Spawn". Pick a navmesh point near one that is away from the player.
	TArray<AActor*> SpawnMarks;
	UGameplayStatics::GetAllActorsWithTag(this, TEXT("KP_Spawn"), SpawnMarks);
	auto RandomSpot = [&]()
	{
		FNavLocation P;
		for (int32 Try = 0; Try < 10 && Nav && SpawnMarks.Num() > 0; ++Try)
		{
			const FVector Zone = SpawnMarks[FMath::RandRange(0, SpawnMarks.Num() - 1)]->GetActorLocation();
			if (FVector::Dist2D(Zone, Player->GetActorLocation()) > 1500.f && Nav->GetRandomReachablePointInRadius(Zone, 300.f, P))
				return P.Location;
		}
		for (int32 Try = 0; Try < 15; ++Try)
			if (Nav && Nav->GetRandomReachablePointInRadius(Center, 2500.f, P)
				&& FVector::Dist2D(P.Location, Player->GetActorLocation()) > 900.f
				&& FVector::Dist2D(P.Location, Center) > 700.f)
				return P.Location;
		// shortcut: no navmesh in this level -> old ring spawn (can land outside walls); add a NavMeshBoundsVolume.
		return Center + FRotator(0.f, FMath::FRandRange(0.f, 360.f), 0.f).Vector() * 1200.f;
	};

	if (Wave % 4 == 0)
	{
		// One boss per component: Motherboard = Corrupted File, GPU = Ransomware, CPU (final) = Rootkit.
		const int32 Comp = ComponentOf(Wave);
		ShowBanner(Comp == 0 ? TEXT("BOSS: CORRUPTED FILE") : Comp == 1 ? TEXT("BOSS: RANSOMWARE - it locks your abilities!") : TEXT("FINAL BOSS: ROOTKIT - it hides, blinks and strikes"), 3.0);
		PlaySfx(EKPSfx::Boss);
		SpawnEnemy(Comp == 0 ? EKPEnemyType::Boss : Comp == 1 ? EKPEnemyType::Ransomware : EKPEnemyType::Rootkit, RandomSpot());
		for (int32 i = 0; i < 4 && Comp < 2; ++i) SpawnEnemy(EKPEnemyType::Rusher, RandomSpot()); // the final wave is the Rootkit alone: its Forks bring the Rushers
		return;
	}

	ShowBanner(FString::Printf(TEXT("WAVE %d/4"), WaveInComponent(Wave)), 2.0);
	PlaySfx(EKPSfx::WaveStart, 0.7f);
	const int32 Count = 3 + Wave * 2;
	for (int32 i = 0; i < Count; ++i)
	{
		// Randomized composition (design doc): rushers first, ranged + tanks join from wave 2.
		const float R = FMath::FRand();
		const EKPEnemyType Type = (Wave >= 2 && R < 0.25f) ? EKPEnemyType::Ranged
			: (Wave >= 2 && R < 0.35f) ? EKPEnemyType::Tank
			: EKPEnemyType::Rusher;
		SpawnEnemy(Type, RandomSpot());
	}
}

void AKPGameMode::OnEnemyRemoved(AKPEnemy* Enemy, bool bKilled)
{
	if (Boss.Get() == Enemy)
	{
		Boss.Reset();
		for (TActorIterator<AKPEnemy> It(GetWorld()); It; ++It) // final wave: another boss may still be alive
			if (*It != Enemy && !It->IsDead() && It->IsBossType()) Boss = *It;
	}
	--Alive;
	if (bKilled)
	{
		++Kills;
		if ((int32)Enemy->Type < 6) ++Stats.KillsByType[(int32)Enemy->Type];
		if (HealPerKill > 0.f) PlayerHealth = FMath::Min(PlayerMaxHealth, PlayerHealth + HealPerKill); // Garbage Collector
		const double Now = FPlatformTime::Seconds();
		KillStreak = Now - LastKillAt < 0.5 ? KillStreak + 1 : 0; // multi-kills within 0.5 s climb in pitch
		LastKillAt = FovPunchAt = Now;
		PlaySfx(EKPSfx::Kill, 0.7f, KillStreak ? 1.f + 0.06f * FMath::Min(KillStreak, 6) : 0.f);
		const float R = Enemy->GetBodyRadius();
		const FVector At = Enemy->GetAimPoint();
		// Two-stage death: white core flash, then a coloured ring (bosses add the shard beams).
		Sp(At, R * 0.4f, R * 1.6f, FLinearColor(6.f, 6.f, 6.f), 1.f, 0.1f);
		if (Enemy->IsBossType()) FxBurst(At, R * 0.8f, R * 2.4f, Enemy->GetColor() * 4.f, 0.3f);
		else Sp(At, R * 0.8f, R * 2.4f, Enemy->GetColor() * 4.f, 0.8f, 0.3f);
		HitStop(GKill[(int32)Enemy->Type][0], 0.05f); // bigger enemy, longer freeze
		AddTrauma(GKill[(int32)Enemy->Type][1]);
	}
	if (Enemy->Type == EKPEnemyType::Ransomware)
	{
		for (double& T : LockedUntil) T = 0.0;
		ShowBanner(TEXT("FILES DECRYPTED - abilities unlocked"), 2.0);
	}
	if (Alive <= 0 && Wave > 0)
	{
		Alive = 0;
		Stats.WaveSeconds.Add((float)(FPlatformTime::Seconds() - Stats.WaveStartedAt));
		if (Wave >= GFinalWave) { WinRun(); return; }
		if (Wave % 4 == 0) ShowBanner(FString::Printf(TEXT("%s SECURED - infection keeps spreading..."), ComponentName(ComponentOf(Wave))), 3.0);
		OfferUpgrades();
	}
}

void AKPGameMode::OfferUpgrades()
{
	// Weighted sampling without replacement: build affinity x2 (ranged kit) / x2.5 (melee kit); holding a non-default weapon makes the other cards of that slot x0.3;
	// an empty slot makes its weapon cards x3, and the offer after wave 2 always holds the slot's driver card (pity).
	TArray<int32> Pool;
	TArray<float> Weight;
	int32 Pity = -1; // Pool index of the driver card to force
	for (int32 i = 0; i < NumUpgrades(); ++i)
	{
		const EKPCardKind Kind = UpgradeKind(i);
		const bool bWpn = Kind <= EKPCardKind::Melee;
		const int32 Held = Kind == EKPCardKind::Ranged ? Weapon : MeleeWeapon; // -1 = empty slot
		const int32 CardW = GUpgrades[i].Weapon;
		if (bWpn && Held >= 0 && (CardW == Held || CardW == 0)) continue; // never the held weapon's card; a driver only while its slot is empty
		const int32 A = UpgradeAffinity(i);
		float W = A == 1 && Kit == 0 ? 2.f : A == 2 && Kit == 1 ? 2.5f : 1.f;
		if (bWpn) W *= Held < 0 ? 3.f : Held > 0 ? 0.3f : 1.f;
		if (bWpn && Held < 0 && CardW == 0 && Wave == 2 && Pity < 0) Pity = Pool.Num();
		Pool.Add(i);
		Weight.Add(W);
	}
	UpgradeOffer.Reset();
	for (int32 n = 0; n < 3 && Pool.Num() > 0; ++n)
	{
		float R = 0.f;
		for (float W : Weight) R += W;
		R *= FMath::FRand();
		int32 Pick = 0;
		while (Pick < Pool.Num() - 1 && (R -= Weight[Pick]) >= 0.f) ++Pick;
		if (n == 0 && Pity >= 0) Pick = Pity;
		const int32 Card = Pool[Pick];
		UpgradeOffer.Add(Card);
		for (int32 j = Pool.Num() - 1; j >= 0; --j)
			if (j == Pick || (UpgradeKind(Card) <= EKPCardKind::Melee && UpgradeKind(Pool[j]) <= EKPCardKind::Melee)) { Pool.RemoveAt(j); Weight.RemoveAt(j); } // at most one weapon card per offer
	}
	if (Pity >= 0 && UpgradeOffer.Num() > 1) UpgradeOffer.Swap(0, FMath::RandRange(0, UpgradeOffer.Num() - 1)); // the forced card was picked first: move it to a random slot
	bChoosingUpgrade = true;
	Stats.Offers.Add(FString::Printf(TEXT("%d:%s|%s|%s"), Wave,
		UpgradeOffer.IsValidIndex(0) ? UpgradeName(UpgradeOffer[0]) : TEXT(""),
		UpgradeOffer.IsValidIndex(1) ? UpgradeName(UpgradeOffer[1]) : TEXT(""),
		UpgradeOffer.IsValidIndex(2) ? UpgradeName(UpgradeOffer[2]) : TEXT("")));
	// Freeze the world while the player reads the cards; the HUD keeps ticking and takes the input.
	UGameplayStatics::SetGamePaused(this, true);
}

void AKPGameMode::WinRun()
{
	if (bRunWon) return;
	FinalRunTime = GetRunTime();
	WriteRunStats(TEXT("won"));
	SaveRecord(true);
	bRunWon = true; // Tick and HurtPlayer/HurtCore freeze on this, like bRunLost
	PlaySfx(EKPSfx::Upgrade);
}

void AKPGameMode::TravelToNextComponent()
{
	GCarry.bValid = true;
	GCarry.Wave = Wave; GCarry.Kills = Kills; GCarry.RunTime = GetRunTime(); GCarry.Health = PlayerHealth;
	GCarry.Picks = PickIdx; GCarry.Stats = Stats; GCarry.Kit = Kit;
	bTraveling = true; // EndPlay must not log this run as "quit"
	const FString Next = GMaps[FMath::Min(ComponentOf(Wave) + 1, 2)];
	// shortcut: Lvl_GPU / Lvl_CPU don't exist yet, so reload the current map; delete the fallback once they do.
	const FName Level = FPackageName::DoesPackageExist(Next) ? FName(*Next) : FName(*UGameplayStatics::GetCurrentLevelName(this));
	UGameplayStatics::OpenLevel(this, Level, true, TEXT("KPCarry=1"));
}

void AKPGameMode::StartHazard(APlayerController* PC)
{
	const bool bS = TraceBoxes.Num() > 0, bH = HeatZones.Num() > 0, bC = ClockMats.Num() > 0;
	if (HazardKind || (!bS && !bH && !bC)) return;
	const int32 Kind = bC ? 3 : bS && bH ? FMath::RandRange(1, 2) : bS ? 1 : 2; // the CPU map's only hazard is the clock
	// Every zone of the kind goes live together (same rule on every map); the per-zone flags stay so a subset is one line away.
	if (Kind < 3) (Kind == 1 ? TraceOn : HeatOn).Init(true, Kind == 1 ? TraceBoxes.Num() : HeatZones.Num());
	const double T = GetWorld()->GetTimeSeconds();
	HazardKind = Kind;
	HazardLiveAt = T + (Kind == 1 ? 1.5 : 2.0); // telegraph
	HazardEndAt = HazardLiveAt + (Kind == 1 ? 3.0 : 5.0);
	++(Kind == 1 ? Stats.SurgeUses : Kind == 2 ? Stats.HeatEvents : Stats.ClockEvents);
	if (Kind == 3)
	{
		ClockDir = FMath::RandBool() ? 1.f : -1.f;
		ClockPrevT = 0.f;
		ClockHit[0].Reset(); ClockHit[1].Reset();
		if (ClockFronts.Num() == 0 && ClockFrontMesh)
			for (int32 i = 0; i < 4; ++i)
			{
				AStaticMeshActor* F = GetWorld()->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator);
				if (!F) continue;
				UStaticMeshComponent* M = F->GetStaticMeshComponent();
				M->SetMobility(EComponentMobility::Movable);
				M->SetStaticMesh(ClockFrontMesh);
				M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				F->SetActorHiddenInGame(true);
				ClockFronts.Add(F);
			}
	}
	ShowBanner(Kind == 1 ? TEXT("POWER SURGE - get off the glowing lines") : Kind == 2 ? TEXT("HEAT RISING - leave the glowing plates") : TEXT("CLOCK EDGE / JUMP"), 2.0); // the clock's text is refreshed every tick in UpdateHazards
	PlaySfx(EKPSfx::Firewall);
	AddTrauma(0.15f);
}

void AKPGameMode::UpdateHazards(APlayerController* PC, float DeltaTime)
{
	if (TraceBoxes.Num() == 0 && HeatZones.Num() == 0 && FanZones.Num() == 0 && ClockMats.Num() == 0) return; // map has no hazard actors
	// World time stops with pause and the upgrade freeze, so the schedule and the debuff timer freeze with them; Tick skips lost/won.
	const double T = GetWorld()->GetTimeSeconds();
	if (!HazardKind && HazardNextAt >= 0.0 && T >= HazardNextAt && !bChoosingUpgrade && Alive > 0) StartHazard(PC);
	const bool bSurge = HazardKind == 1, bClock = HazardKind == 3, bLive = HazardKind && T >= HazardLiveAt && T < HazardEndAt;
	const float L0 = ClockPrevT, L1 = (float)(T - HazardLiveAt); // clock: live time at the previous / this tick
	if (bClock && bLive) ClockPrevT = L1;
	if (HazardKind)
	{
		// Drives the "Flash" emissive vector on the active zones' materials (same one the enemies use); black = idle look.
		const bool bEnd = T >= HazardEndAt, bTele = T < HazardLiveAt;
		// Pulse per FX_SPEC: p(f,t) = 0.5-0.5cos(2 pi f t), t = seconds since this phase started (gold Surge 2 Hz -> 5 Hz, orange Overheat breathing 1 Hz -> red 2 Hz).
		const float t = (float)(T - (bTele ? HazardLiveAt - (bSurge ? 1.5 : 2.0) : HazardLiveAt));
		auto P = [t](float F) { return 0.5f - 0.5f * FMath::Cos(2.f * PI * F * t); };
		const FLinearColor C = bEnd ? FLinearColor::Black
			: bClock ? (bTele ? FLinearColor(2.4f, 1.44f, 0.24f) * (0.3f + 0.7f * P(2.f)) : FLinearColor(6.f, 0.5f, 0.3f) * (0.9f + 0.1f * P(5.f))) // system gold tell, live red (CPU_LEVEL.md palette)
			: bSurge ? (bTele ? FLinearColor(2.4f, 1.1f, 0.08f) * (0.25f + 0.75f * P(2.f)) : FLinearColor(6.f, 2.7f, 0.12f) * (0.8f + 0.2f * P(5.f)))
			: (bTele ? FLinearColor(2.4f, 0.55f, 0.04f) * (0.3f + 0.7f * P(1.f)) : FLinearColor(6.f, 0.3f, 0.025f) * (0.9f + 0.1f * P(2.f)));
		const TArray<TObjectPtr<UMaterialInstanceDynamic>>& Mats = bClock ? ClockMats : bSurge ? TraceMats : HeatMats;
		const TArray<bool>& On = bSurge ? TraceOn : HeatOn;
		for (int32 i = 0; i < Mats.Num(); ++i) if ((bClock || On[i]) && Mats[i]) Mats[i]->SetVectorParameterValue(TEXT("Flash"), C);
		if (bClock)
		{
			// HUD text through the banner path: the tell counts down, the live phase says LIVE; refreshed every tick (BannerStartedAt was set by StartHazard).
			const TCHAR* Way = ClockDir > 0.f ? TEXT("EAST >>>") : TEXT("<<< WEST");
			Banner = bTele ? FString::Printf(TEXT("CLOCK EDGE / JUMP  %s  %.1f"), Way, (float)(HazardLiveAt - T)) : FString::Printf(TEXT("CLOCK EDGE / LIVE  %s"), Way);
			if (!bEnd) BannerUntil = FPlatformTime::Seconds() + 0.5;
			for (int32 i = 0; i < ClockFronts.Num(); ++i)
			{
				const float Lo = GClockLo[i & 1], Hi = GClockHi[i & 1], PassLen = ClockPassSeconds(Lo, Hi);
				int32 Show = -1;
				for (int32 p = 0; p < 2; ++p) if (bLive && L1 >= ClockPassStart(p) && L1 <= ClockPassStart(p) + PassLen) Show = p;
				ClockFronts[i]->SetActorHiddenInGame(Show < 0);
				if (Show >= 0) ClockFronts[i]->SetActorLocationAndRotation(FVector(ClockFrontX(Lo, Hi, ClockDir, L1 - ClockPassStart(Show)), 0.5f * (GClockLo[i >> 1] + GClockHi[i >> 1]), 0.f), FRotator(0.f, ClockDir > 0.f ? -90.f : 90.f, 0.f)); // local +Y = leading edge
			}
		}
		if (bLive && !bHazardOnset) // onset cue at the live transition: one beam per surge lane / one pop per plate, at most 8
		{
			bHazardOnset = true;
			int32 Fx = 0;
			if (bSurge) { for (int32 i = 0; i < TraceBoxes.Num() && Fx < 8; ++i) if (TraceOn[i])
			{
				++Fx;
				const FVector Sz = TraceBoxes[i].GetSize() - FVector(120.f, 120.f, 0.f); // undo the 60 cm padding; shortcut: AABB footprint
				const bool bX = Sz.X >= Sz.Y;
				const float Wd = FMath::Min(4.f, FMath::Max(1.f, bX ? Sz.Y : Sz.X)), Half = FMath::Min(60.f, FMath::Max(1.f, bX ? Sz.X : Sz.Y)) * 0.5f;
				const FVector Mid(TraceBoxes[i].GetCenter().X, TraceBoxes[i].GetCenter().Y, TraceBoxes[i].Max.Z + 6.f), Ax = bX ? FVector::ForwardVector : FVector::RightVector;
				Bm(Mid - Ax * Half, Mid + Ax * Half, Wd, Wd * 0.2f, FLinearColor(6.f, 3.5f, 0.25f), 0.9f, 0.12f);
			} }
			else for (int32 i = 0; i < HeatZones.Num() && Fx < 8; ++i) if (HeatOn[i])
			{
				++Fx;
				const FVector4& Z = HeatZones[i];
				Sp(FVector(Z.X, Z.Y, Z.Z + 6.f), Z.W < 6.f ? Z.W * 0.5f : 6.f, FMath::Min(24.f, Z.W), FLinearColor(5.f, 1.f, 0.05f), 0.65f, 0.16f);
			}
		}
		if (bEnd) { HazardKind = 0; bHazardOnset = false; HazardNextAt = T + FMath::FRandRange(10.0, 18.0); }
	}

	auto Feet = [](const ACharacter* Ch) { return Ch->GetActorLocation() - FVector(0.f, 0.f, Ch->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()); };
	auto InDisc = [](const FVector& F, const FVector4& Z) { return FMath::Square(F.X - Z.X) + FMath::Square(F.Y - Z.Y) <= FMath::Square(Z.W) && F.Z < Z.Z + 60.f; }; // a jump clears it
	auto Hot = [&](const ACharacter* Ch)
	{
		const FVector F = Feet(Ch);
		if (bSurge) { for (int32 i = 0; i < TraceBoxes.Num(); ++i) if (TraceOn[i] && TraceBoxes[i].IsInsideXY(F) && F.Z < TraceBoxes[i].Max.Z + 60.f) return true; }
		else for (int32 i = 0; i < HeatZones.Num(); ++i) if (HeatOn[i] && InDisc(F, HeatZones[i])) return true;
		return false;
	};
	// Clock edge: new passes that swept this actor since the last tick (feet above 45 cm = safe; each actor is hit once per pass, whatever the tile or frame rate).
	auto ClockHits = [&](const ACharacter* Ch)
	{
		const FVector L = Ch->GetActorLocation();
		if (!bClock || !bLive || ClockSafeFeet(Feet(Ch).Z)) return 0;
		const float R = Ch->GetCapsuleComponent()->GetScaledCapsuleRadius();
		int32 N = 0;
		for (int32 p = 0; p < 2; ++p)
			for (int32 i = 0; i < 4 && !ClockHit[p].Contains(Ch); ++i)
				if (L.Y >= GClockLo[i >> 1] - R && L.Y <= GClockHi[i >> 1] + R && ClockPassSwept(GClockLo[i & 1], GClockHi[i & 1], ClockDir, L0, L1, p, L.X, R)) { ClockHit[p].Add(Ch); ++N; }
		return N;
	};
	if (bLive)
		for (TActorIterator<AKPEnemy> It(GetWorld()); It; ++It)
			if (!It->IsDead())
			{
				const int32 N = ClockHits(*It);
				if (N) DealEnemyDamage(*It, 3.f * N, PC, nullptr, EKPDealt::Hazard); // 3 HP per pass
				else if (Hot(*It)) DealEnemyDamage(*It, 0.75f * (bSurge ? 3.f : 2.f) * DeltaTime, PC, nullptr, EKPDealt::Hazard);
			} // enemies take 75%: surge 2.25/s, a rusher dies in ~1.3s, a tank survives; shortcut: enemy AI does not avoid hot zones.
	ACharacter* Me = Cast<ACharacter>(PC->GetPawn());
	if (!Me) return;
	if (const int32 N = ClockHits(Me)) HurtPlayer(12.f * N, EKPDmgSrc::Clock); // 12 HP per pass
	if (bLive && Hot(Me))
	{
		HurtPlayer((bSurge ? 15.f : 8.f) * DeltaTime, bSurge ? EKPDmgSrc::Surge : EKPDmgSrc::Heat);
		if (!bSurge) OverheatUntil = T + 5.0;
	}
	for (const FVector4& Z : FanZones)
		if (InDisc(Feet(Me), Z))
		{
			OverheatUntil = 0.0;
			LastHurtTime = 0.0; // skips the 3s shield recharge delay while cooling
			break;
		}
	// Walk speed x0.75 while overheated; multiplicative and undone exactly, so Fast Bus (x1.07) picked meanwhile still stacks right.
	const bool bHotNow = T < OverheatUntil;
	if (bHotNow != bHeatSlowed)
	{
		bHeatSlowed = bHotNow;
		Me->GetCharacterMovement()->MaxWalkSpeed *= bHotNow ? 0.75f : 1.f / 0.75f;
	}
}

float AKPGameMode::OverheatLeft() const
{
	return GetWorld() ? FMath::Max(0.f, (float)(OverheatUntil - GetWorld()->GetTimeSeconds())) : 0.f;
}

void AKPGameMode::OnBossSlam()
{
	PlaySfx(EKPSfx::Shockwave);
	AddTrauma(0.45f);
	// The slam is named after Windows' "delete everything" command (rm -rf ./* on Windows).
	ShowBanner(bSlamHintShown ? TEXT("> del /s /q *.*") : TEXT("> del /s /q *.*   JUMP over it!  [SPACE]"), bSlamHintShown ? 1.2 : 2.5);
	bSlamHintShown = true;
}

void AKPGameMode::ChooseUpgrade(int32 Slot)
{
	if (!bChoosingUpgrade || !UpgradeOffer.IsValidIndex(Slot)) return;
	const int32 U = UpgradeOffer[Slot];
	const EKPCardKind Kind = UpgradeKind(U);
	if (Kind <= EKPCardKind::Melee && (Kind == EKPCardKind::Ranged ? Weapon : MeleeWeapon) < 0 && Stats.SlotFilledWave == 0) Stats.SlotFilledWave = Wave; // an empty slot gets its first weapon
	ApplyUpgrade(U);
	++UpgradesTaken;
	PickIdx.Add(U);
	Stats.Picks.Add(FString::Printf(TEXT("%d:%s"), Wave, UpgradeName(U)));
	PlaySfx(EKPSfx::Upgrade);
	ShowBanner(FString::Printf(TEXT("%s installed%s"), UpgradeName(U), Kind <= EKPCardKind::Melee ? *FString::Printf(TEXT(" - %s equipped"), Kind == EKPCardKind::Ranged ? WeaponName() : MeleeWeaponName()) : TEXT("")), 1.5);
	bChoosingUpgrade = false;
	WaveDelay = 2.f;
	bBlockFireUntilRelease = true;
	UGameplayStatics::SetGamePaused(this, false);
	HitStopUntil = 0.0; UGameplayStatics::SetGlobalTimeDilation(this, 1.f); // a kill's hit-stop must not outlive the card screen
	if (Wave % 4 == 0) TravelToNextComponent(); // boss cleared and patch chosen
}

void AKPGameMode::ApplyUpgrade(int32 Index)
{
	APawn* Pawn = UGameplayStatics::GetPlayerPawn(this, 0);
	const FKPUpgrade& Card = GUpgrades[Index];
	if (Card.Kind == EKPCardKind::Ranged) Weapon = Card.Weapon; // weapon cards replace the weapon in their slot
	else if (Card.Kind == EKPCardKind::Melee) MeleeWeapon = Card.Weapon;
	switch (Index)
	{
	case 0: ShotDamage += 1.f; break;
	case 1: FireInterval /= 1.25f; break;
	case 2: if (ACharacter* C = Cast<ACharacter>(Pawn)) C->GetCharacterMovement()->MaxWalkSpeed *= 1.15f; break;
	case 3: ++Pierce; break;
	case 4: MaxShield += 30.f; Shield = MaxShield; break;
	case 5: PlayerMaxHealth += 40.f; PlayerHealth = PlayerMaxHealth; break;
	case 6: CoreHealth = FMath::Min(CoreMaxHealth, CoreHealth + 200.f); FirewallCooldown = 0.f; break;
	case 7: MeleeDamage += 2.f; MeleeRange += 40.f; break;
	case 8: AimAssist = FMath::Min(1.f, AimAssist + 0.25f); ShotRadius += 6.f; break;
	case 11: ChainDamage += 1.f; break;
	case 12: HealPerKill += 2.f; break;
	case 13: FirewallCooldownTime *= 0.7f; break;
	case 19: CritChance += 0.2f; break;
	case 20: HealPerHit += 2.f; break;
	case 21: MeleeInterval *= 0.8f; break;
	case 22: RegenPerSec += 1.f; break;
	}
}

void AKPGameMode::ActivateFirewall(APawn* Pawn)
{
	// Firewall: core invulnerable for a few seconds + a burst that throws enemies out.
	// Risk (design doc): the burst also zaps the player if they stand too close to the core.
	FirewallUntil = GetWorld()->GetTimeSeconds() + FirewallDuration;
	FirewallCooldown = FirewallCooldownTime;
	++Stats.FirewallUses;
	const FVector C = Core->GetActorLocation();
	APlayerController* PC = Cast<APlayerController>(Pawn->GetController());
	for (TActorIterator<AKPEnemy> It(GetWorld()); It; ++It)
	{
		AKPEnemy* E = *It;
		const FVector To = E->GetActorLocation() - C;
		if (E->IsDead() || To.Size2D() > FirewallRadius + 200.f) continue;
		DealEnemyDamage(E, FirewallBurstDamage, PC, Core, EKPDealt::Firewall);
		if (!E->IsDead()) E->Knockback(To, 1400.f);
	}
	if (FVector::Dist2D(Pawn->GetActorLocation(), C) < 260.f) HurtPlayer(10.f, EKPDmgSrc::Firewall);
	// Translucent cyan dome over the core for the whole duration (own actor: pulses, fades out at the end, destroys itself).
	PlayFx(true, C, FRotator::ZeroRotator, FVector(FirewallRadius / 50.f), FVector(FirewallRadius / 50.f), FLinearColor(0.1f, 1.2f, 2.f), FirewallDuration, 0.25f, true);
	ShowBanner(TEXT("FIREWALL UP - core protected, heal near it"), 2.0);
	PlaySfx(EKPSfx::Firewall);
	AddTrauma(0.5f);
}

void AKPGameMode::LockAbility(int32 Ability, float Seconds)
{
	if (!LockedUntilIsValid(Ability)) return;
	// Never lock an empty slot or the only weapon the player has: the firewall is always usable and takes the lock instead.
	const bool bHeld[2] = { Weapon >= 0, MeleeWeapon >= 0 };
	if (Ability < 2 && !(bHeld[Ability] && bHeld[1 - Ability])) Ability = 2;
	LockedUntil[Ability] = GetWorld()->GetTimeSeconds() + Seconds;
	++Stats.AbilityLocks;
	static const TCHAR* Names[] = { TEXT("SHOOTING"), TEXT("MELEE"), TEXT("FIREWALL") };
	ShowBanner(FString::Printf(TEXT("%s ENCRYPTED!"), Names[Ability]), 1.5);
	PlaySfx(EKPSfx::Lock);
	AddTrauma(0.3f);
}

bool AKPGameMode::IsLocked(int32 Ability) const
{
	return LockedUntilIsValid(Ability) && GetWorld() && GetWorld()->GetTimeSeconds() < LockedUntil[Ability];
}

float AKPGameMode::LockTimeLeft(int32 Ability) const
{
	return IsLocked(Ability) ? (float)(LockedUntil[Ability] - GetWorld()->GetTimeSeconds()) : 0.f;
}

bool AKPGameMode::IsFirewallUp() const
{
	return GetWorld() && GetWorld()->GetTimeSeconds() < FirewallUntil;
}

float AKPGameMode::GetFirewallCooldown() const
{
	return FMath::Max(0.f, FirewallCooldown);
}

void AKPGameMode::HurtPlayer(float Amount, EKPDmgSrc Src, const FVector& From)
{
	if (GKPGod || !bStarted || bRunLost || bRunWon || UGameplayStatics::IsGamePaused(this) || GetWorld()->GetTimeSeconds() < DashGraceUntil) return; // Pointer dash: brief i-frames
	Stats.DamageTaken[(int32)Src] += Amount;
	const double Now = FPlatformTime::Seconds();
	const float Absorbed = FMath::Min(Shield, Amount);
	Shield -= Absorbed;
	PlayerHealth -= Amount - Absorbed;
	LastHurtTime = Now;
	if (Now - LastHurtSfx > 0.3) { PlaySfx(EKPSfx::PlayerHurt, 0.7f); LastHurtSfx = Now; } // contact damage ticks every frame
	HurtFlashUntil = Now + 0.15;
	// Feedback in beats: contact damage arrives every frame, so it accumulates and fires at most every 0.25 s; one big hit (>= 15) fires at once.
	HurtAccum += Amount;
	if (Amount >= 15.f || Now - LastHurtBeat >= 0.25)
	{
		const APawn* P = UGameplayStatics::GetPlayerPawn(this, 0);
		AddTrauma(FMath::Clamp(0.12f + HurtAccum * 0.02f, 0.f, 0.7f), P && !From.IsZero() ? (P->GetActorLocation() - From).GetSafeNormal2D() : FVector::ZeroVector); // jolt away from the hit
		HurtFlashAlpha = FMath::Clamp(0.3f + HurtAccum * 0.02f, 0.3f, 0.7f);
		if (Amount >= 15.f) HitStop(0.06, 0.05f);
		HurtAccum = 0.f;
		LastHurtBeat = Now;
	}
	if (PlayerHealth <= 0.f) LoseRun(TEXT("ANTI-MALWARE TERMINATED"));
}

void AKPGameMode::HurtCore(float Amount, EKPDmgSrc Src)
{
	if (GKPGod || !bStarted || bRunLost || bRunWon || UGameplayStatics::IsGamePaused(this) || IsFirewallUp()) return;
	Stats.CoreDamage[(int32)Src] += Amount;
	CoreHealth -= Amount;
	if (FPlatformTime::Seconds() - LastCoreSfx > 0.5) { PlaySfx(EKPSfx::CoreHit, 0.6f); LastCoreSfx = FPlatformTime::Seconds(); }
	if (CoreMat)
	{
		CoreMat->SetVectorParameterValue(TEXT("Flash"), FLinearColor(3.f, 0.2f, 0.2f));
		CoreFlashUntil = FPlatformTime::Seconds() + 0.1;
	}
	if (CoreHealth <= 0.f) LoseRun(TEXT("CORE INFECTED"));
}

void AKPGameMode::LoseRun(const TCHAR* Reason)
{
	// Failure rule: everything from this run is lost; restart from the beginning.
	FinalRunTime = GetRunTime();
	FailureReason = Reason;
	WriteRunStats(CoreHealth <= 0.f ? TEXT("core_lost") : TEXT("player_died"));
	SaveRecord(false);
	bRunLost = true;
	RestartAt = FPlatformTime::Seconds() + 10.0; // long enough to read the run summary; R restarts at once
	ShowBanner(FString::Printf(TEXT("%s - RUN LOST"), Reason), 3.0);
	UGameplayStatics::SetGlobalTimeDilation(this, 0.25f);
	PlaySfx(EKPSfx::RunLost);
	AddTrauma(1.f);
}

void AKPGameMode::ShowBanner(const FString& Text, double Seconds)
{
	Banner = Text;
	BannerStartedAt = FPlatformTime::Seconds();
	BannerUntil = BannerStartedAt + Seconds;
}

void AKPGameMode::HitStop(double Seconds, float Slow)
{
	if (bRunLost) return;
	UGameplayStatics::SetGlobalTimeDilation(this, Slow);
	// Newest hit-stop owns the restore time, so overlapping kills don't cut each other short.
	HitStopUntil = FMath::Max(HitStopUntil, FPlatformTime::Seconds() + Seconds);
}

void AKPGameMode::UpdateHitStop(double Now)
{
	if (HitStopUntil > 0.0 && Now >= HitStopUntil && !bRunLost)
	{
		UGameplayStatics::SetGlobalTimeDilation(this, 1.f);
		HitStopUntil = 0.0;
	}
}

void AKPGameMode::UpdateShake(APawn* Pawn, float RealDt)
{
	USpringArmComponent* Arm = Pawn->FindComponentByClass<USpringArmComponent>();
	if (!Arm) return;
	Trauma = FMath::Max(0.f, Trauma - 1.5f * RealDt);
	ShakeTime += RealDt * 30.f;
	const float S = Trauma * Trauma; // quadratic: small hits barely move, big ones punch
	// Two frequencies per axis (slow sway + fast tremor) so it reads as noise, not one sine. Camera only, never the body (game-feel skill).
	auto N = [&](float F, float Ph) { return 0.65f * FMath::Sin(ShakeTime * F + Ph) + 0.35f * FMath::Sin(ShakeTime * F * 2.9f + Ph * 1.7f); };
	KickVec = KickVec.GetClampedToMaxSize(40.f) * FMath::Exp(-14.f * RealDt); // directional jolt snaps out and eases back
	Arm->SocketOffset = BaseSocketOffset + FVector(10.f * S * N(0.6f, 2.f), 40.f * S * N(1.7f, 0.f), 28.f * S * N(2.3f, 1.1f)) + Arm->GetTargetRotation().UnrotateVector(KickVec);
	if (UCameraComponent* Cam = Pawn->FindComponentByClass<UCameraComponent>())
	{
		Cam->SetRelativeRotation(FRotator(2.5f * S * N(1.9f, 4.f), 0.f, 4.f * S * N(1.3f, 2.2f))); // pitch/roll on the camera itself, the controller rotation is untouched
		const float U = (float)((FPlatformTime::Seconds() - FovPunchAt) / 0.15); // kill FOV punch: +3, ease-out back over 0.15 s
		Cam->SetFieldOfView(70.f + (U < 1.f ? 3.f * FMath::Square(1.f - U) : 0.f));
	}
}

void AKPGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bStarted && !bRunLost && !bRunWon && !bTraveling) WriteRunStats(TEXT("quit")); // closed the game / stopped Play mid-run
	Super::EndPlay(EndPlayReason);
}

void AKPGameMode::WriteRunStats(const TCHAR* Result)
{
	// One JSON object per line in Saved/Analytics/runs.jsonl. Summarize with Tools/Analytics/summarize.py.
	if (Stats.bWritten || GIsAutomationTesting) return; // automation tests must not pollute the player's data
	Stats.bWritten = true;
	static const TCHAR* DealtNames[] = { TEXT("ranged"), TEXT("melee"), TEXT("chain"), TEXT("firewall"), TEXT("hazard"), TEXT("other") };
	static_assert(UE_ARRAY_COUNT(DealtNames) == (int32)EKPDealt::Count, "DealtNames must match EKPDealt");
	static const TCHAR* SrcNames[] = { TEXT("rusher"), TEXT("ranged"), TEXT("tank"), TEXT("boss"), TEXT("ransomware"), TEXT("rootkit"), TEXT("bolt"), TEXT("firewall"), TEXT("surge"), TEXT("heat"), TEXT("other"), TEXT("clock") };
	static_assert(UE_ARRAY_COUNT(SrcNames) == (int32)EKPDmgSrc::Count, "SrcNames must match EKPDmgSrc");
	auto Num = [](float V) { return FString::SanitizeFloat(FMath::RoundToFloat(V * 10.f) / 10.f); };
	auto Map = [&](const float* Values)
	{
		TArray<FString> Parts;
		for (int32 i = 0; i < (int32)EKPDmgSrc::Count; ++i) Parts.Add(FString::Printf(TEXT("\"%s\":%s"), SrcNames[i], *Num(Values[i])));
		return TEXT("{") + FString::Join(Parts, TEXT(",")) + TEXT("}");
	};
	auto StrList = [](const TArray<FString>& L)
	{
		TArray<FString> Q;
		for (const FString& X : L) Q.Add(TEXT("\"") + X.Replace(TEXT("\""), TEXT("'")) + TEXT("\""));
		return TEXT("[") + FString::Join(Q, TEXT(",")) + TEXT("]");
	};
	TArray<FString> Dealt;
	for (int32 i = 0; i < (int32)EKPDealt::Count; ++i) Dealt.Add(FString::Printf(TEXT("\"%s\":%s"), DealtNames[i], *Num(Stats.DamageDealt[i])));
	TArray<FString> Waves;
	for (float Sec : Stats.WaveSeconds) Waves.Add(Num(Sec));
	TArray<FString> KillParts;
	for (int32 i = 0; i < 6; ++i) KillParts.Add(FString::Printf(TEXT("\"%s\":%d"), SrcNames[i], Stats.KillsByType[i]));

	const FString Line = FString::Printf(
		TEXT("{\"time\":\"%s\",\"result\":\"%s\",\"wave\":%d,\"run_seconds\":%s,\"kills\":%d,\"kills_by_type\":{%s},")
		TEXT("\"damage_taken\":%s,\"core_damage\":%s,\"hp_left\":%s,\"core_left\":%s,")
		TEXT("\"shots\":%d,\"shot_hits\":%d,\"assisted_hits\":%d,\"aim_assist\":%s,\"melee_swings\":%d,\"melee_hits\":%d,")
		TEXT("\"firewall_uses\":%d,\"surge_uses\":%d,\"heat_events\":%d,\"clock_events\":%d,\"ability_locks\":%d,\"wave_seconds\":[%s],\"offers\":%s,\"picks\":%s,\"kit\":\"%s\",\"ranged\":\"%s\",\"melee\":\"%s\",")
		TEXT("\"melee_swings_hit\":%d,\"damage_dealt\":{%s},\"death_wave_time\":%s,\"enemies_alive\":%d,\"slot_filled_wave\":%d%s}\n"),
		*FDateTime::Now().ToIso8601(), Result, Wave, *Num(FinalRunTime > 0.f ? FinalRunTime : GetRunTime()), Kills, *FString::Join(KillParts, TEXT(",")),
		*Map(Stats.DamageTaken), *Map(Stats.CoreDamage), *Num(FMath::Max(0.f, PlayerHealth)), *Num(FMath::Max(0.f, CoreHealth)),
		Stats.ShotsFired, Stats.ShotHits, Stats.AssistedShots, *Num(AimAssist), Stats.MeleeSwings, Stats.MeleeHits,
		Stats.FirewallUses, Stats.SurgeUses, Stats.HeatEvents, Stats.ClockEvents, Stats.AbilityLocks, *FString::Join(Waves, TEXT(",")), *StrList(Stats.Offers), *StrList(Stats.Picks),
		Kit == 1 ? TEXT("close") : TEXT("ranged"), WeaponName(), MeleeWeaponName(),
		Stats.MeleeSwingsHit, *FString::Join(Dealt, TEXT(",")), *Num((float)(FPlatformTime::Seconds() - Stats.WaveStartedAt)), Alive, Stats.SlotFilledWave, Stats.bSynthetic ? TEXT(",\"synthetic\":true") : TEXT(""));
	const FString Path = FPaths::ProjectSavedDir() / TEXT("Analytics/runs.jsonl");
	FFileHelper::SaveStringToFile(Line, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
}

// Best runs live in Saved/Analytics/records.json: {"runs":[ {"wave":..,"kills":..,"seconds":..,"won":..}, ... ]}, one run per line, best first. Plain text so it can be read and hand-edited.
namespace
{
	FString RecordsPath() { return FPaths::ProjectSavedDir() / TEXT("Analytics/records.json"); }
	TArray<FKPRecord> LoadRecords()
	{
		TArray<FKPRecord> List;
		FString Text;
		TArray<FString> Runs;
		if (FFileHelper::LoadFileToString(Text, *RecordsPath())) Text.ParseIntoArray(Runs, TEXT("}"));
		for (const FString& Run : Runs)
		{
			if (!Run.Contains(TEXT("\"wave\":"))) continue; // the closing "]" piece
			auto Field = [&Run](const TCHAR* Key) { const int32 At = Run.Find(Key); return At == INDEX_NONE ? 0.0 : FCString::Atod(*Run.Mid(At + FCString::Strlen(Key))); };
			FKPRecord R;
			R.Wave = (int32)Field(TEXT("\"wave\":"));
			R.Kills = (int32)Field(TEXT("\"kills\":"));
			R.Seconds = (float)Field(TEXT("\"seconds\":"));
			R.bWon = Run.Contains(TEXT("\"won\":true"));
			List.Add(R);
		}
		return List;
	}
}

void AKPGameMode::SaveRecord(bool bWon)
{
	if (Stats.bSynthetic || GIsAutomationTesting) return; // debug-touched runs and automation tests never enter the records
	TArray<FKPRecord> List = LoadRecords();
	int32 BestWave = 0, BestKills = 0;
	float BestWin = 0.f; // 0 = no win stored yet
	for (const FKPRecord& R : List)
	{
		BestWave = FMath::Max(BestWave, R.Wave);
		BestKills = FMath::Max(BestKills, R.Kills);
		if (R.bWon && (BestWin <= 0.f || R.Seconds < BestWin)) BestWin = R.Seconds;
	}
	bNewRecWave = Wave > BestWave;
	bNewRecKills = Kills > BestKills;
	bNewRecTime = bWon && (BestWin <= 0.f || FinalRunTime < BestWin);
	FKPRecord Run;
	Run.Wave = Wave; Run.Kills = Kills; Run.Seconds = FinalRunTime; Run.bWon = bWon;
	InsertRecord(List, Run);
	TArray<FString> Lines;
	for (const FKPRecord& R : List)
		Lines.Add(FString::Printf(TEXT("{\"wave\":%d,\"kills\":%d,\"seconds\":%.1f,\"won\":%s}"), R.Wave, R.Kills, R.Seconds, R.bWon ? TEXT("true") : TEXT("false")));
	FFileHelper::SaveStringToFile(TEXT("{\"runs\":[\n") + FString::Join(Lines, TEXT(",\n")) + TEXT("\n]}\n"), *RecordsPath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
