#include "KPEnemy.h"
#include "KPBolt.h"
#include "KPShockwave.h"
#include "KPFx.h"
#include "KPGameMode.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/EngineTypes.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "NavigationPath.h"
#include "NavigationSystem.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/ConstructorHelpers.h"

AKPEnemy::AKPEnemy()
{
	PrimaryActorTick.bCanEverTick = true;
	GetCapsuleComponent()->InitCapsuleSize(45.f, 45.f);

	Body = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(GetCapsuleComponent());
	Body->SetRelativeScale3D(FVector(0.8f));
	// Models from Tools/ArtGen (Blender). Each fits a 1 m cube, so the per-type Size scale still works.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> RusherMesh(TEXT("/Game/KernelPanic/Models/SM_KP_Rusher.SM_KP_Rusher"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> RangedMesh(TEXT("/Game/KernelPanic/Models/SM_KP_Ranged.SM_KP_Ranged"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> TankMesh(TEXT("/Game/KernelPanic/Models/SM_KP_Tank.SM_KP_Tank"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> BossMesh(TEXT("/Game/KernelPanic/Models/SM_KP_Boss.SM_KP_Boss"));
	TypeMeshes[0] = RusherMesh.Object;
	TypeMeshes[1] = RangedMesh.Object;
	TypeMeshes[2] = TankMesh.Object;
	TypeMeshes[3] = BossMesh.Object;
	// Optional models (may not exist yet): missing = fall back to the old look, one warning, no error spam.
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> RansomMesh(TEXT("/Game/KernelPanic/Models/SM_KP_Ransomware.SM_KP_Ransomware"), LOAD_Quiet | LOAD_NoWarn);
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> RootkitMesh(TEXT("/Game/KernelPanic/Models/SM_KP_Rootkit.SM_KP_Rootkit"), LOAD_Quiet | LOAD_NoWarn);
	static bool bWarned = false;
	if (!bWarned && (!RansomMesh.Get() || !RootkitMesh.Get()))
	{
		bWarned = true;
		UE_LOG(LogTemp, Warning, TEXT("SM_KP_Ransomware / SM_KP_Rootkit missing: Ransomware uses the tank model, Rootkit the boss model."));
	}
	TypeMeshes[4] = RansomMesh.Get() ? RansomMesh.Get() : TankMesh.Object.Get();
	TypeMeshes[5] = RootkitMesh.Get() ? RootkitMesh.Get() : BossMesh.Object.Get();
	// Final boss model: 11 parts sharing one origin. All present = multipart Rootkit, any missing = the single model above (one warning).
	static const TCHAR* RkNames[11] = { TEXT("CrownA"), TEXT("CrownB"), TEXT("CrownC"), TEXT("CrownD"), TEXT("Body"), TEXT("RootA"), TEXT("RootB"), TEXT("RootC"), TEXT("RootD"), TEXT("Pins"), TEXT("Kernel") };
	static UStaticMesh* RkLoaded[11] = {};
	static bool bRkLoaded = false;
	if (!bRkLoaded)
	{
		bRkLoaded = true;
		int32 Found = 0;
		for (int32 i = 0; i < 11; ++i)
		{
			const FString Path = FString::Printf(TEXT("/Game/KernelPanic/Models/SM_KP_RootkitFinal_%s.SM_KP_RootkitFinal_%s"), RkNames[i], RkNames[i]); // the finder keeps the pointer and loads in Get(): the string must outlive it
			ConstructorHelpers::FObjectFinderOptional<UStaticMesh> Part(*Path, LOAD_Quiet | LOAD_NoWarn);
			Found += (RkLoaded[i] = Part.Get()) != nullptr;
		}
		if (Found < 11) UE_LOG(LogTemp, Warning, TEXT("SM_KP_RootkitFinal_* incomplete (%d/11): the Rootkit uses the single SM_KP_Rootkit model."), Found);
	}
	for (int32 i = 0; i < 11; ++i) RkMesh[i] = RkLoaded[i];
	Body->SetStaticMesh(TypeMeshes[0]);
	// Body only catches shots; the capsule does the walking collision.
	Body->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Body->SetCollisionResponseToAllChannels(ECR_Ignore);
	Body->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	Body->SetCastShadow(false); // cheap: many enemies
	Body->SetCollisionObjectType(ECC_WorldDynamic); // so wall/floor checks (WorldStatic) ignore enemies

	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->bRunPhysicsWithNoController = true; // gravity + walking without an AIController
	Move->bOrientRotationToMovement = true;
	Move->RotationRate = FRotator(0.f, 540.f, 0.f);
	bUseControllerRotationYaw = false;
}

void AKPEnemy::BeginPlay()
{
	Super::BeginPlay();
}

void AKPEnemy::EndPlay(const EEndPlayReason::Type Reason)
{
	if (IsValid(RkAnchor)) RkAnchor->Destroy(); // the Core Hook anchor never outlives its boss
	Super::EndPlay(Reason);
}

// Adds an emissive tint on top of the model's own glow (M_KP_VertexGlow "Flash"). Black = no flash.
void AKPEnemy::SetColor(const FLinearColor& C)
{
	if (Mat) Mat->SetVectorParameterValue(TEXT("Flash"), C);
	for (UMaterialInstanceDynamic* M : PartMats) if (M) M->SetVectorParameterValue(TEXT("Flash"), C); // Rootkit parts share the body's flash
}

void AKPEnemy::Init(EKPEnemyType InType, int32 Wave)
{
	Type = InType;
	float Size = 0.6f, Speed = 420.f;
	switch (Type)
	{
	case EKPEnemyType::Rusher: Health = 3.f;  Speed = FMath::Min(330.f + Wave * 30.f, 560.f); Size = 0.6f; ContactDps = 10.f + Wave * 1.5f; BaseColor = FLinearColor(1.f, 0.05f, 0.05f); break;
	case EKPEnemyType::Ranged: Health = 2.f;  Speed = 300.f; Size = 0.5f; ContactDps = 10.f; BaseColor = FLinearColor(0.6f, 0.1f, 1.f); break;
	case EKPEnemyType::Tank:   Health = 12.f; Speed = 180.f; Size = 1.2f; ContactDps = 15.f; BaseColor = FLinearColor(1.f, 0.45f, 0.f); break;
	case EKPEnemyType::Boss:   Health = 60.f; Speed = 330.f; Size = 2.2f; ContactDps = 30.f; BaseColor = FLinearColor(0.1f, 1.f, 0.2f); break;
	case EKPEnemyType::Ransomware: Health = 50.f; Speed = 360.f; Size = 1.8f; ContactDps = 30.f; BaseColor = FLinearColor(1.f, 0.8f, 0.f); break; // own model now: no rest tint, it hid the padlock detail
	case EKPEnemyType::Rootkit: Health = 84.f; Speed = 300.f; Size = 2.0f; ContactDps = 30.f; BaseColor = FLinearColor(0.6f, 0.8f, 1.f); break; // 84 = 1.4 x Corrupted File
	}
	BodySize = Size;
	Health *= 1.f + 0.15f * (Wave - 1); // later waves are tougher
	MaxHealth = Health;
	FireTimer = FMath::FRandRange(1.f, 2.5f);

	Body->SetStaticMesh(TypeMeshes[(int32)Type]);
	Mat = Body->CreateDynamicMaterialInstance(0);
	Body->SetRelativeScale3D(FVector(Size));
	GetCapsuleComponent()->SetCapsuleSize(Size * 60.f, Size * 50.f);
	GetCharacterMovement()->MaxWalkSpeed = Speed;
	SetColor(RestFlash);
	if (Type == EKPEnemyType::Rootkit)
	{
		bRkMulti = true;
		for (const TObjectPtr<UStaticMesh>& M : RkMesh) bRkMulti &= M != nullptr;
		if (bRkMulti) RkInitParts();
	}
}

void AKPEnemy::MoveToward(const FVector& Target, float Sign)
{
	const FVector Here = GetActorLocation();
	const float Dt = GetWorld()->GetDeltaSeconds();

	// Backing away (ranged kiting) stays a straight move.
	if (Sign < 0.f)
	{
		AddMovementInput(FVector(Here.X - Target.X, Here.Y - Target.Y, 0.f).GetSafeNormal());
		return;
	}

	// Re-path every 0.5s or when the goal moved a lot; walk the navmesh corners in order.
	RepathTimer -= Dt;
	if (RepathTimer <= 0.f || FVector::DistSquared2D(Target, LastGoal) > FMath::Square(300.f))
	{
		RepathTimer = 0.5f + FMath::FRand() * 0.2f; // jitter so enemies don't all path on the same frame
		LastGoal = Target;
		PathPoints.Reset();
		PathIndex = 1;
		if (UNavigationPath* Path = UNavigationSystemV1::FindPathToLocationSynchronously(GetWorld(), Here, Target, this))
			if (Path->IsValid()) PathPoints = Path->PathPoints;
	}

	FVector Next = Target; // no path (off navmesh) -> straight line fallback
	while (PathPoints.IsValidIndex(PathIndex) && FVector::DistSquared2D(Here, PathPoints[PathIndex]) < FMath::Square(80.f)) ++PathIndex;
	if (PathPoints.IsValidIndex(PathIndex)) Next = PathPoints[PathIndex];
	AddMovementInput(FVector(Next.X - Here.X, Next.Y - Here.Y, 0.f).GetSafeNormal());

	// Stuck? (barely moved in 1s) -> hop and force a new path.
	StuckTimer += Dt;
	if (StuckTimer >= 1.f)
	{
		if (FVector::DistSquared2D(Here, StuckCheckPos) < FMath::Square(40.f))
		{
			Jump();
			RepathTimer = 0.f;
		}
		StuckCheckPos = Here;
		StuckTimer = 0.f;
	}
}

FVector AKPEnemy::GetAimPoint() const
{
	return Body ? Body->Bounds.Origin : GetActorLocation();
}

float AKPEnemy::GetBodyRadius() const
{
	return Body ? Body->Bounds.SphereRadius * 0.6f : 40.f; // sphere bounds overshoot the box-ish models
}

void AKPEnemy::BossTick(AKPGameMode* GM, APawn* Player, float DeltaTime, bool bTouchingPlayer)
{
	const FVector ToPlayer = Player->GetActorLocation() - GetActorLocation();
	const float Dist = ToPlayer.Size2D();
	const float Contact = DashLeft > 0.f ? ContactDps * 2.f : ContactDps;

	// Kiting punishment: if the player runs far away, the boss goes for the core instead.
	AActor* Core = GM->GetCore();
	const bool bHuntCore = Core && Dist > 1800.f;

	if (Type == EKPEnemyType::Boss)
	{
		// Corrupted File slam: stands still and glows, then sends a shockwave ring along the floor.
		if (SecondSlamIn > 0.f && (SecondSlamIn -= DeltaTime) <= 0.f) SpawnShockwave(GM);
		if (SlamWindup > 0.f)
		{
			if ((SlamWindup -= DeltaTime) <= 0.f)
			{
				SpawnShockwave(GM);
				if (Health < MaxHealth * 0.5f) SecondSlamIn = 0.7f; // enraged: double ring
				SetColor(RestFlash);
			}
			if (bTouchingPlayer) GM->HurtPlayer(Contact * DeltaTime, EKPDmgSrc::Boss, GetActorLocation());
			return;
		}
		if ((SlamTimer -= DeltaTime) <= 0.f && TelegraphLeft <= 0.f && DashLeft <= 0.f && !bHuntCore && Dist < 2200.f)
		{
			SlamTimer = 7.f;
			DashTimer = FMath::Max(DashTimer, 2.f); // don't chain a dash right after a slam
			SlamWindup = 0.6f;
			FlashTime = 0.f;
			SetColor(FLinearColor(0.6f, 5.f, 0.8f));
			return;
		}

		// Corrupted File: wind up (stands still, glows), then dashes at the player.
		if (TelegraphLeft > 0.f)
		{
			if ((TelegraphLeft -= DeltaTime) <= 0.f)
			{
				LaunchCharacter(ToPlayer.GetSafeNormal2D() * 1800.f + FVector(0, 0, 120.f), true, true);
				DashLeft = 0.7f;
				SetColor(RestFlash);
			}
			if (bTouchingPlayer) GM->HurtPlayer(Contact * DeltaTime, EKPDmgSrc::Boss, GetActorLocation());
			return;
		}
		DashLeft = FMath::Max(0.f, DashLeft - DeltaTime);
		if ((DashTimer -= DeltaTime) <= 0.f && !bHuntCore && Dist > 350.f && Dist < 1700.f)
		{
			DashTimer = 5.f;
			TelegraphLeft = 0.7f;
			FlashTime = 0.f;
			SetColor(BaseColor * 4.f);
			return;
		}
	}
	else if (Type == EKPEnemyType::Rootkit)
	{
		RkTick(GM, Player, DeltaTime); // no contact damage: every hit is a telegraphed attack
		return;
	}
	else if (Type == EKPEnemyType::Ransomware)
	{
		// Encrypts one player ability every few seconds (design doc: "locks selected player abilities").
		if ((LockTimer -= DeltaTime) <= 0.f)
		{
			LockTimer = 6.f;
			GM->LockAbility(FMath::RandRange(0, 2), 4.f);
			FlashTime = 0.3f;
			SetColor(BaseColor * 4.f);
		}
		// Fan of 5 bolts: running away in a straight line gets you hit.
		if ((FanTimer -= DeltaTime) <= 0.f && Dist < 2200.f)
		{
			FanTimer = 3.5f;
			const FVector From = GetActorLocation() + FVector(0, 0, 40.f);
			const FRotator Aim = (Player->GetActorLocation() + FVector(0, 0, 30.f) - From).Rotation();
			FActorSpawnParameters P; P.Owner = this;
			for (int32 i = -2; i <= 2; ++i)
				GetWorld()->SpawnActor<AKPBolt>(AKPBolt::StaticClass(), From, Aim + FRotator(0.f, i * 11.f, 0.f), P);
			GM->PlaySfx(EKPSfx::BoltFire, 0.6f);
		}
	}

	if (bHuntCore)
	{
		if ((Core->GetActorLocation() - GetActorLocation()).Size2D() < GetCapsuleComponent()->GetScaledCapsuleRadius() + 160.f)
			GM->HurtCore(ContactDps * DeltaTime, DmgSrcOf(Type));
		else MoveToward(Core->GetActorLocation());
	}
	else if (bTouchingPlayer) GM->HurtPlayer(Contact * DeltaTime, DmgSrcOf(Type), GetActorLocation());
	else MoveToward(Player->GetActorLocation());
}

int32 AKPEnemy::RkPickAnchor(const FVector& Player, const FVector& Here, uint8 ClearMask, int32 Roll)
{
	int32 Ok[4], N = 0;
	for (int32 i = 0; i < 4; ++i)
		if (((ClearMask >> i) & 1) && FVector::Dist2D(RkAnchorPos(i), Player) >= 400.f && FVector::Dist2D(RkAnchorPos(i), Here) > 200.f) Ok[N++] = i;
	return N ? Ok[FMath::Abs(Roll) % N] : -1;
}

bool AKPEnemy::RkInStrip(const FVector& P, const FVector& O, const FVector& Dir, float Len, float HalfW, float PlayerR)
{
	const double Vx = P.X - O.X, Vy = P.Y - O.Y, Along = Vx * Dir.X + Vy * Dir.Y, Side = Vx * Dir.Y - Vy * Dir.X;
	return Along >= -PlayerR && Along <= Len + PlayerR && FMath::Abs(Side) <= HalfW + PlayerR;
}

float AKPEnemy::RkCoverClip(const FVector& O, const FVector& Dir, float Len)
{
	// shortcut: the cache covers are hard-coded from CPU_LEVEL.md (300 cm boxes at (+-1000, +-1000)); other maps simply have no clipping. Slab test on the strip's centre line.
	float Best = Len;
	for (int32 i = 0; i < 4; ++i)
	{
		float T0 = 0.f, T1 = Len;
		for (int32 a = 0; a < 2 && T0 <= T1; ++a)
		{
			const float Oa = a ? O.Y : O.X, Da = a ? Dir.Y : Dir.X, Lo = ((a ? (i & 2) : (i & 1)) ? 1000.f : -1000.f) - 150.f, Hi = Lo + 300.f;
			if (FMath::Abs(Da) < KINDA_SMALL_NUMBER) { if (Oa < Lo || Oa > Hi) T1 = -1.f; continue; }
			float A = (Lo - Oa) / Da, B = (Hi - Oa) / Da;
			if (A > B) Swap(A, B);
			T0 = FMath::Max(T0, A);
			T1 = FMath::Min(T1, B);
		}
		if (T0 <= T1) Best = FMath::Min(Best, T0);
	}
	return Best;
}

bool AKPEnemy::RkClear(const FVector& P, float R, float HH) const
{
	return !GetWorld()->OverlapBlockingTestByChannel(P, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(R, HH), FCollisionQueryParams(NAME_None, false, this));
}

// Red floor footprint (CPU_LEVEL.md: red is for dangerous floor): thin outline during the tell, a flat solid slab at impact. An outline's Life is twice the tell, so it is still half bright when the hit lands.
void AKPEnemy::RkRect(AKPGameMode* GM, const FVector& C, const FVector& Dir, float HL, float HW, bool bSolid, float Life) const
{
	const FVector D = Dir.GetSafeNormal2D(), N(-D.Y, D.X, 0.f), Up(0.f, 0.f, 8.f);
	const FLinearColor Red(5.f, 0.3f, 0.2f);
	if (bSolid)
	{
		const FVector S(HL / 50.f, HW / 50.f, 0.05f); // sphere mesh radius is 50 cm
		GM->PlayFx(true, C + Up, D.Rotation(), S, S, Red, Life, 0.8f, false);
		return;
	}
	const FVector P[4] = { C + D * HL + N * HW, C + D * HL - N * HW, C - D * HL - N * HW, C - D * HL + N * HW };
	for (int32 i = 0; i < 4; ++i) GM->Bm(P[i] + Up, P[(i + 1) % 4] + Up, 12.f, 12.f, Red, 1.f, Life);
}

void AKPEnemy::RkDisc(AKPGameMode* GM, const FVector& C, float R, const FLinearColor& Col, float Life) const
{
	const FVector S(R / 50.f, R / 50.f, 0.05f);
	GM->PlayFx(true, C + FVector(0.f, 0.f, 8.f), FRotator::ZeroRotator, S, S, Col, Life, 0.55f, false);
}

void AKPEnemy::RkInitParts()
{
	GetCapsuleComponent()->SetCapsuleSize(120.f, 150.f); // plan: 1.2 m navigation capsule; the damage volume is the central shell (Body)
	BodySize = 1.f;
	Body->SetStaticMesh(RkMesh[4]);
	Body->SetRelativeScale3D(FVector::OneVector); // new multipart model: no 2x spawn scale
	Body->SetRelativeLocation(FVector(0.f, 0.f, -150.f)); // every part's origin = footprint centre at floor level
	Mat = Body->CreateDynamicMaterialInstance(0);
	for (int32 i = 0; i < 11; ++i)
	{
		if (i == 4) { RkParts[i] = Body; continue; }
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
		C->SetStaticMesh(RkMesh[i]);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(false);
		C->SetupAttachment(Body);
		C->RegisterComponent();
		PartMats.Add(C->CreateDynamicMaterialInstance(0));
		RkParts[i] = C;
	}
	SetColor(RestFlash);
}

// Phase look, blink fold and death separation, all by translation / scale of the rigid parts. Without the multipart model only the single body folds and shrinks.
void AKPEnemy::RkPose(float Dt)
{
	RkLookV = FMath::FInterpConstantTo(RkLookV, RkPhase - 1.f, Dt, RkLookRate);
	const float T = bRkDying ? RkDeadT : 0.f;
	if (!bRkMulti)
	{
		Body->SetRelativeScale3D(FVector(BodySize * (1.f - 0.5f * RkFold) * (1.f - FMath::Min(T / 2.f, 0.95f))));
		return;
	}
	const float L = RkLookV, Off = L <= 1.f ? 20.f * L : 20.f + 60.f * (L - 1.f); // phase 2 = 20 cm, phase 3 = 80 cm
	const float Sep = FMath::Clamp((T - 0.4f) / 0.8f, 0.f, 1.f), Ret = FMath::Clamp((T - 1.2f) / 0.8f, 0.f, 1.f);
	for (int32 i = 0; i < 4; ++i) // A top -Y, B top +Y, C bottom -Y, D bottom +Y; B also lifts 60 cm in phase 3
	{
		RkParts[i]->SetRelativeLocation(FVector(0.f, (i & 1 ? 1.f : -1.f) * (Off + 350.f * Sep), (i == 1 ? 60.f * FMath::Max(0.f, L - 1.f) : 0.f) + (i < 2 ? 150.f * Sep : 0.f)));
		RkParts[i]->SetRelativeScale3D(FVector(1.f - 0.7f * RkFold));
	}
	for (int32 i = 5; i < 9; ++i) RkParts[i]->SetRelativeScale3D(FVector(FMath::Max(1.f - Ret, 0.01f)));
	RkParts[9]->AddRelativeRotation(FRotator(0.f, 20.f * Dt, 0.f)); // pins turn slowly
	RkParts[9]->SetVisibility(T < 0.4f);
	RkParts[10]->SetRelativeScale3D(FVector(1.f, 1.f, 1.f - 0.85f * Ret)); // the kernel folds into a chip
}

void AKPEnemy::InitAnchor()
{
	bAnchor = true; // a tiny AKPEnemy the existing shots and swings already hit; never counted (no Alive, kill or analytics), never acts: see Tick / TakeDamage / Die
	Health = MaxHealth = 2.f;
	BaseColor = FLinearColor(0.55f, 0.8f, 1.f);
	RestFlash = FLinearColor(1.5f, 2.5f, 4.f);
	Mat = Body->CreateDynamicMaterialInstance(0);
	SetColor(RestFlash);
}

void AKPEnemy::RkEnd(float Recover)
{
	RkAtk = ERkAtk::None;
	RkRecover = Recover;
	RkWalk = 2.f; // then it pursues the player for up to 2 s before the next attack
	RkRingFx.Reset();
	if (IsValid(RkAnchor)) RkAnchor->Destroy();
	RkAnchor = nullptr;
	if (FlashTime <= 0.f) SetColor(RestFlash);
}

void AKPEnemy::RkStart(AKPGameMode* GM, APawn* Player, ERkAtk A)
{
	const FVector Here = GetActorLocation(), Floor(Here.X, Here.Y, RkFloorZ());
	const float HH = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FLinearColor Red(5.f, 0.3f, 0.2f), Source(2.5f, 4.f, 6.f); // white-blue source mark under the boss
	if (A == ERkAtk::Blink)
	{
		uint8 Mask = 0;
		for (int32 i = 0; i < 4 && RkForceMask < 0; ++i)
			if (RkClear(RkAnchorPos(i) + FVector(0.f, 0.f, Floor.Z + HH + 5.f), GetCapsuleComponent()->GetScaledCapsuleRadius(), HH)) Mask |= 1 << i;
		RkBlinkIdx = RkPickAnchor(Player->GetActorLocation(), Here, RkForceMask >= 0 ? (uint8)RkForceMask : Mask, FMath::RandRange(0, 3));
		if (RkBlinkIdx < 0) A = ERkAtk::Ring; // no legal anchor: stay and strike with the Ring after its full tell
	}
	RkAtk = RkLast = A;
	RkAtkT = 0.f;
	bRkHit = false;
	RkOrigin = Floor;
	if (A == ERkAtk::Blink)
	{
		RkDest = RkAnchorPos(RkBlinkIdx) + FVector(0.f, 0.f, Floor.Z); // locked at tell start
		RkRect(GM, RkDest, FVector::ForwardVector, 200.f, 200.f, false, 2.f); // 4 x 4 m arrival square, harmless
		RkDisc(GM, Floor, 90.f, Source, 2.f);
	}
	else if (A == ERkAtk::Ring)
	{
		RkRingR = 250.f;
		RkDisc(GM, Floor, 250.f, Red, 2.4f); // starts at r = 2.5 m
		RkDisc(GM, Floor, 60.f, Source, 2.4f);
	}
	else if (A == ERkAtk::Lanes)
	{
		RkDir = (Player->GetActorLocation() - Here).GetSafeNormal2D(); // aimed now, locked: no tracking after the tell
		if (RkDir.IsNearlyZero()) RkDir = FVector::ForwardVector;
		const FVector Side(-RkDir.Y, RkDir.X, 0.f);
		for (int32 i = 0; i < 3; ++i)
		{
			const FVector O = Floor + Side * ((i - 1) * 400.f);
			RkLen[i] = RkCoverClip(O, RkDir, 1600.f);
			RkRect(GM, O + RkDir * (RkLen[i] * 0.5f), RkDir, RkLen[i] * 0.5f, 75.f, false, 2.2f);
		}
		RkDisc(GM, Floor, 90.f, Source, 2.2f);
	}
	else if (A == ERkAtk::Hook)
	{
		const AActor* Core = GM->GetCore();
		RkDest = Core ? FVector((Here.X + Core->GetActorLocation().X) * 0.5f, (Here.Y + Core->GetActorLocation().Y) * 0.5f, Floor.Z) : Floor;
		if (Core) GM->Bm(Here + FVector(0.f, 0.f, 150.f), Core->GetActorLocation(), 8.f, 8.f, Source, 1.f, 9.f); // tether drawn above the floor
		RkRect(GM, RkDest, FVector::ForwardVector, 100.f, 100.f, false, 9.f);
		GM->Bm(RkDest + FVector(-100.f, -100.f, 8.f), RkDest + FVector(100.f, 100.f, 8.f), 10.f, 10.f, Red, 1.f, 9.f); // stripes
		GM->Bm(RkDest + FVector(-100.f, 100.f, 8.f), RkDest + FVector(100.f, -100.f, 8.f), 10.f, 10.f, Red, 1.f, 9.f);
		RkHookDealt = 0.f;
	}
	else if (A == ERkAtk::Fork)
	{
		RkSpots = 0;
		for (int32 i = 0; i < 3; ++i)
			for (int32 k = 0; k < 12; ++k) // a blocked disc relocates to the next open spot around the ring
			{
				const FVector P = Floor + FRotator(0.f, i * 120.f + k * 30.f, 0.f).Vector() * 400.f;
				if (!RkClear(P + FVector(0.f, 0.f, 57.f), 35.f, 55.f)) continue;
				RkSpot[i] = P;
				RkSpots |= 1 << i;
				RkDisc(GM, P, 100.f, Red, 2.4f);
				break;
			}
		RkDisc(GM, Floor, 60.f, Source, 2.4f);
	}
}

void AKPEnemy::RkBeginDeath()
{
	bRkDying = true; // IsDead() is already true: shots, swings and hazards skip it. Collision off, attacks cancelled, children removed without kill credit.
	RkDeadT = 0.f;
	RkAtk = ERkAtk::None;
	bForkPending = false;
	RkRingFx.Reset();
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	GetCharacterMovement()->DisableMovement();
	if (IsValid(RkAnchor)) RkAnchor->Destroy();
	RkAnchor = nullptr;
	for (TWeakObjectPtr<AKPEnemy>& K : RkKids)
		if (AKPEnemy* E = K.Get(); E && !E->IsDead()) E->Die(false);
	RkKids.Reset();
}

void AKPEnemy::RkDeathTick(float Dt)
{
	RkDeadT += Dt;
	RkPose(Dt); // pins out 0-0.4 s, crown separates 0.4-1.2 s, roots retract and the kernel folds 1.2-2.0 s
	if (RkDeadT >= 2.f) Die(true); // the one removal: win path and kill count run in OnEnemyRemoved
}

void AKPEnemy::RkTick(AKPGameMode* GM, APawn* Player, float Dt)
{
	AActor* Core = GM->GetCore();
	const FVector Here = GetActorLocation();
	// Thresholds 66% / 33%: the latest look wins and one group of 3 Rushers is queued, however many are crossed; uncommitted attacks are cancelled; the change takes 1.2 s.
	const int32 P = RkPhaseOf(Health / MaxHealth);
	if (P > RkPhase)
	{
		RkPhase = P;
		bForkPending = true;
		RkLookRate = FMath::Abs(P - 1.f - RkLookV) / 1.2f;
		if (RkAtk != ERkAtk::None && !RkCommitted()) RkEnd(0.f);
		RkRecover = FMath::Max(RkRecover, 1.2f);
	}
	// Clock event: no new Blink / Ring / Lanes / Fork, uncommitted ones are cancelled (committed ones finish; the map event never waits for the boss).
	const bool bClock = GM->IsClockActive();
	if (bClock && RkAtk != ERkAtk::None && !RkCommitted()) RkEnd(0.3f);
	RkFold = FMath::Max(RkAtk == ERkAtk::Blink && RkAtkT < 1.f ? RkAtkT : 0.f, FMath::FInterpConstantTo(RkFold, 0.f, Dt, 4.f)); // the crown folds into the shell during the departure tell
	RkPose(Dt);

	const float FZ = RkFloorZ(), PR = Player->GetSimpleCollisionRadius();
	const FVector PL = Player->GetActorLocation();
	const bool bGround = !AKPGameMode::ClockSafeFeet(PL.Z - Player->GetSimpleCollisionHalfHeight() - FZ); // same jump rule as the clock front
	const float Recover = RkPhase >= 3 ? 1.f : 1.4f; // 1.4 s, 1.0 s in phase 3
	auto Hurt = [&](float Amount) { bRkHit = true; GM->HurtPlayer(Amount, EKPDmgSrc::Rootkit, GetActorLocation()); }; // once per cast

	if (RkAtk == ERkAtk::None)
	{
		if (RkRecover > 0.f) { RkRecover -= Dt; return; }
		bool bAt = false;
		for (int32 i = 0; i < 4; ++i) bAt |= FVector::Dist2D(Here, RkAnchorPos(i)) < 200.f; // Core Hook only while standing at a cardinal anchor
		const bool bHookOK = RkPhase >= 2 && bAt && Core;
		auto Pursue = [&] { MoveToward(Core && FVector::Dist2D(Here, Core->GetActorLocation()) > 1800.f ? Core->GetActorLocation() : PL); }; // 3 m/s, never further than 18 m from the core
		if (bClock && !bHookOK) { Pursue(); return; } // clock tell / live: only Core Hook or recovery
		if (!bClock && !bForkPending && RkWalk > 0.f && !bAt && FVector::Dist2D(Here, PL) > 350.f) { RkWalk -= Dt; Pursue(); return; }
		ERkAtk Next = bClock ? ERkAtk::Hook : bForkPending ? ERkAtk::Fork : ERkAtk::None;
		if (Next == ERkAtk::None)
		{
			TArray<ERkAtk, TInlineAllocator<4>> Opts;
			for (const ERkAtk O : { ERkAtk::Blink, ERkAtk::Ring, ERkAtk::Lanes, ERkAtk::Hook })
				if (O != RkLast && (O <= ERkAtk::Ring || (RkPhase >= 2 && (O != ERkAtk::Hook || bHookOK)))) Opts.Add(O);
			Next = Opts[FMath::RandRange(0, Opts.Num() - 1)];
		}
		RkStart(GM, Player, Next);
		return;
	}

	const float T0 = RkAtkT;
	RkAtkT += Dt;
	if (RkAtk == ERkAtk::Blink)
	{
		if (RkAtkT < 1.f) { if (FlashTime <= 0.f) SetColor(BaseColor * 4.f); return; } // departure tell: eyes stay, the body stays hittable
		if (T0 < 1.f)
		{
			const float R = GetBodyRadius();
			GM->FxBurst(GetAimPoint(), R * 0.8f, R * 2.5f, BaseColor * 4.f, 0.35f); // departure
			SetActorLocation(RkDest + FVector(0.f, 0.f, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.f), false, nullptr, ETeleportType::TeleportPhysics);
			GetCharacterMovement()->Velocity = FVector::ZeroVector;
			PathPoints.Reset();
			RepathTimer = 0.f;
			SetColor(RestFlash);
			GM->FxBurst(GetAimPoint(), R * 0.8f, R * 2.5f, BaseColor * 4.f, 0.35f); // arrival is harmless
			GM->PlaySfx(EKPSfx::BoltFire, 0.7f);
			RkRect(GM, RkDest, FVector::ForwardVector, 600.f, 75.f, false, 1.6f); // two crossing 12 x 1.5 m strips
			RkRect(GM, RkDest, FVector::RightVector, 600.f, 75.f, false, 1.6f);
		}
		if (T0 < 1.6f && RkAtkT >= 1.6f)
		{
			RkRect(GM, RkDest, FVector::ForwardVector, 600.f, 75.f, true, 0.4f);
			RkRect(GM, RkDest, FVector::RightVector, 600.f, 75.f, true, 0.4f);
		}
		if (RkAtkT >= 1.6f && RkAtkT < 1.8f && !bRkHit
			&& (RkInStrip(PL, RkDest - FVector(600.f, 0.f, 0.f), FVector::ForwardVector, 1200.f, 75.f, PR) || RkInStrip(PL, RkDest - FVector(0.f, 600.f, 0.f), FVector::RightVector, 1200.f, 75.f, PR)))
			Hurt(18.f); // once per cross, not once per arm
		if (RkAtkT >= 1.8f) RkEnd(Recover);
	}
	else if (RkAtk == ERkAtk::Ring)
	{
		if (RkAtkT < 1.2f) return; // windup
		if (T0 < 1.2f) RkRingFx = GM->PlayFx(true, Here, FRotator::ZeroRotator, FVector(5.f, 5.f, 0.45f), FVector(5.f, 5.f, 0.45f), FLinearColor(5.f, 0.3f, 0.2f), 0.3f, 0.85f, false);
		const float R0 = RkRingR;
		RkRingR = FMath::Min(R0 + 800.f * Dt, 1200.f); // 8 m/s out to 12 m
		if (!bRkHit && bGround && RkRingSwept(R0, RkRingR, FVector::Dist2D(PL, Here), PR)) Hurt(16.f); // 60 cm wide, 45 cm high: a jump or a refuge clears it; never hits the core
		if (AKPFx* F = RkRingFx.Get()) { const FVector S(RkRingR / 50.f, RkRingR / 50.f, 0.45f); F->Play(true, FVector(Here.X, Here.Y, FZ + 22.f), FRotator::ZeroRotator, S, S, FLinearColor(5.f, 0.3f, 0.2f), 0.3f, 0.85f, false); }
		if (RkRingR >= 1200.f) RkEnd(Recover);
	}
	else if (RkAtk == ERkAtk::Lanes)
	{
		if (RkAtkT < 1.1f) return;
		const FVector Side(-RkDir.Y, RkDir.X, 0.f);
		if (T0 < 1.1f)
			for (int32 i = 0; i < 3; ++i) RkRect(GM, RkOrigin + Side * ((i - 1) * 400.f) + RkDir * (RkLen[i] * 0.5f), RkDir, RkLen[i] * 0.5f, 75.f, true, 0.4f);
		if (RkAtkT >= 1.45f) { RkEnd(Recover); return; }
		for (int32 i = 0; i < 3 && !bRkHit; ++i)
			if (RkInStrip(PL, RkOrigin + Side * ((i - 1) * 400.f), RkDir, RkLen[i], 75.f, PR)) Hurt(20.f); // once per cast even at intersections; no jump clears roots
	}
	else if (RkAtk == ERkAtk::Hook)
	{
		if (!Core) { RkEnd(Recover); return; }
		if (RkAtkT < 1.5f) return;
		if (T0 < 1.5f) // the tell is over: the anchor appears at the midpoint, hittable by the player's own shots and swings
		{
			FActorSpawnParameters Sp;
			Sp.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			RkAnchor = GetWorld()->SpawnActor<AKPEnemy>(AKPEnemy::StaticClass(), RkDest + FVector(0.f, 0.f, 45.f), FRotator::ZeroRotator, Sp);
			if (RkAnchor) RkAnchor->InitAnchor();
		}
		if (!IsValid(RkAnchor) || RkAnchor->IsDead()) { RkEnd(Recover); return; } // broken
		if (RkHookDealt < 24.f)
		{
			const float D = FMath::Min(8.f * Dt, 24.f - RkHookDealt); // 8 HP/s for 3 s = 24 at most; a Firewall makes HurtCore ignore it
			RkHookDealt += D;
			GM->HurtCore(D, EKPDmgSrc::Rootkit);
		}
		if (RkAtkT >= 4.5f) RkEnd(Recover); // channel over (also removes the anchor)
	}
	else if (RkAtk == ERkAtk::Fork)
	{
		if (RkAtkT < 1.2f) return;
		RkKids.RemoveAll([](const TWeakObjectPtr<AKPEnemy>& K) { return !K.IsValid() || K->IsDead(); });
		for (int32 i = 0; i < 3; ++i)
			if (((RkSpots >> i) & 1) && RkKids.Num() < 6) // at most 6 children alive
				if (AKPEnemy* K = GM->SpawnEnemy(EKPEnemyType::Rusher, RkSpot[i])) RkKids.Add(K);
		bForkPending = false;
		RkEnd(Recover);
	}
}

void AKPEnemy::SpawnShockwave(AKPGameMode* GM)
{
	// Ring runs on the floor under the boss.
	FVector Base = GetActorLocation() - FVector(0, 0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	FHitResult Floor;
	if (GetWorld()->LineTraceSingleByObjectType(Floor, GetActorLocation(), GetActorLocation() - FVector(0, 0, 1000.f), FCollisionObjectQueryParams(ECC_WorldStatic)))
		Base = Floor.ImpactPoint;
	GetWorld()->SpawnActor<AKPShockwave>(AKPShockwave::StaticClass(), Base, FRotator::ZeroRotator);
	GM->OnBossSlam();
}

void AKPEnemy::Knockback(const FVector& Dir, float Strength)
{
	if (bAnchor) return;
	const bool bHeavy = IsBossType() || Type == EKPEnemyType::Tank;
	const float S = bHeavy ? Strength * 0.2f : Strength;
	LaunchCharacter(Dir.GetSafeNormal2D() * S + FVector(0, 0, S * 0.4f), true, true);
}

void AKPEnemy::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (FlashTime > 0.f && (FlashTime -= DeltaTime) <= 0.f) SetColor(RestFlash);

	if (bAnchor) return; // the Core Hook anchor only stands there
	if (bRkDying) { RkDeathTick(DeltaTime); return; }
	AKPGameMode* GM = GetWorld()->GetAuthGameMode<AKPGameMode>();
	APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
	if (!GM || !Player) return;

	const FVector ToPlayer = Player->GetActorLocation() - GetActorLocation();
	if (ToPlayer.Z > 2000.f) { Die(false); return; } // fell out of the map

	const float Radius = GetCapsuleComponent()->GetScaledCapsuleRadius();
	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const bool bTouchingPlayer = ToPlayer.Size2D() < Radius + 60.f && FMath::Abs(ToPlayer.Z) < HalfHeight + 100.f;

	switch (Type)
	{
	case EKPEnemyType::Ransomware:
	case EKPEnemyType::Rootkit:
	case EKPEnemyType::Boss:
		BossTick(GM, Player, DeltaTime, bTouchingPlayer);
		break;

	case EKPEnemyType::Rusher:
		if (bTouchingPlayer) GM->HurtPlayer(ContactDps * DeltaTime, EKPDmgSrc::Rusher, GetActorLocation());
		else MoveToward(Player->GetActorLocation());
		break;

	case EKPEnemyType::Tank:
		if (AActor* Core = GM->GetCore())
		{
			if ((Core->GetActorLocation() - GetActorLocation()).Size2D() < Radius + 140.f) GM->HurtCore(ContactDps * DeltaTime, EKPDmgSrc::Tank);
			else MoveToward(Core->GetActorLocation());
		}
		else if (bTouchingPlayer) GM->HurtPlayer(ContactDps * DeltaTime, EKPDmgSrc::Tank, GetActorLocation());
		else MoveToward(Player->GetActorLocation());
		break;

	case EKPEnemyType::Ranged:
	{
		const float Dist = ToPlayer.Size2D();
		if (Dist > 1100.f) MoveToward(Player->GetActorLocation());
		else if (Dist < 650.f) MoveToward(Player->GetActorLocation(), -1.f);
		else AddMovementInput(FVector::CrossProduct(ToPlayer.GetSafeNormal2D(), FVector::UpVector)); // strafe

		FireTimer -= DeltaTime;
		if (FireTimer < 0.4f && FlashTime <= 0.f) SetColor(BaseColor * 3.f); // telegraph: glow before firing
		if (FireTimer <= 0.f)
		{
			FireTimer = 2.2f;
			SetColor(FLinearColor::Black);
			if (Dist < 1600.f)
			{
				const FVector From = GetActorLocation() + FVector(0, 0, 20.f);
				const FVector Aim = Player->GetActorLocation() + FVector(0, 0, 30.f);
				FActorSpawnParameters P; P.Owner = this;
				GetWorld()->SpawnActor<AKPBolt>(AKPBolt::StaticClass(), From, (Aim - From).Rotation(), P);
				GM->PlaySfx(EKPSfx::BoltFire, 0.4f);
			}
		}
		break;
	}
	}
}

float AKPEnemy::TakeDamage(float Damage, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser)
{
	if (IsDead()) return 0.f;
	Health -= bAnchor ? (Damage >= 2.f ? 2.f : 1.f) : Damage; // Core Hook anchor (2 HP): any melee hit (>= 2) or two ranged hits break it

	// Hit feedback: white flash + knockback (bosses and tanks barely move).
	FlashTime = 0.08f;
	SetColor(FLinearColor(4.f, 4.f, 4.f));
	if (DamageCauser) Knockback(GetActorLocation() - DamageCauser->GetActorLocation(), 350.f);

	if (IsDead()) { if (Type == EKPEnemyType::Rootkit && !bAnchor) RkBeginDeath(); else Die(true); }
	return Damage;
}

void AKPEnemy::Die(bool bCountAsKill)
{
	if (bRemoved) return; // the boss is removed (and counted) exactly once
	bRemoved = true;
	Health = 0.f;
	if (bAnchor) { Destroy(); return; }
	if (AKPGameMode* GM = GetWorld()->GetAuthGameMode<AKPGameMode>())
	{
		// Corrupted File splits into smaller enemies (spawned before we are removed, so the wave isn't "cleared").
		if (Type == EKPEnemyType::Boss && bCountAsKill)
			for (int32 i = 0; i < 5; ++i)
				GM->SpawnEnemy(EKPEnemyType::Rusher, GetActorLocation() + FRotator(0.f, i * 72.f, 0.f).Vector() * 250.f);
		GM->OnEnemyRemoved(this, bCountAsKill);
	}
	Destroy();
}
