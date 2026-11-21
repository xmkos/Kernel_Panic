#include "KPShockwave.h"
#include "KPGameMode.h"
#include "Components/CapsuleComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

AKPShockwave::AKPShockwave()
{
	PrimaryActorTick.bCanEverTick = true;
	Ring = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Ring"));
	RootComponent = Ring;
	// Ring made of the bolt shard model, lying flat along the circle, tinted boss-green.
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Shard(TEXT("/Game/KernelPanic/Models/SM_KP_Bolt.SM_KP_Bolt"));
	if (Shard.Succeeded()) Ring->SetStaticMesh(Shard.Object);
	Ring->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Ring->SetCastShadow(false);
	InitialLifeSpan = 5.f; // safety net; normally destroyed at MaxRadius
}

void AKPShockwave::BeginPlay()
{
	Super::BeginPlay();
	for (int32 i = 0; i < Segments; ++i) Ring->AddInstance(FTransform::Identity);
	Ring->CreateDynamicMaterialInstance(0); // per-wave material so it can fade on its own
	LayoutRing();
}

void AKPShockwave::LayoutRing()
{
	// Shards grow a bit as the ring expands so it stays a solid line.
	const float Len = FMath::Clamp(2.f * PI * Radius / Segments / 100.f * 1.3f, 0.4f, 2.f);
	// Strong emissive so it blooms in the dark arena; fades as it spreads (still clearly visible at the edge).
	const float Fade = 1.f - 0.6f * FMath::Clamp(Radius / MaxRadius, 0.f, 1.f);
	if (UMaterialInstanceDynamic* M = Cast<UMaterialInstanceDynamic>(Ring->GetMaterial(0)))
		M->SetVectorParameterValue(TEXT("Flash"), FLinearColor(0.8f, 14.f, 1.6f) * Fade); // Corrupted File green
	TArray<FTransform> Xf;
	Xf.Reserve(Segments);
	for (int32 i = 0; i < Segments; ++i)
	{
		const float A = 2.f * PI * i / Segments;
		const FVector P(FMath::Cos(A) * Radius, FMath::Sin(A) * Radius, 15.f);
		Xf.Add(FTransform(FRotator(0.f, FMath::RadiansToDegrees(A) + 90.f, 0.f), P, FVector(Len, 0.5f, 0.45f)));
	}
	Ring->BatchUpdateInstancesTransforms(0, Xf, false, true);
}

void AKPShockwave::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	const float Prev = Radius;
	Radius += Speed * DeltaTime;
	if (Radius > MaxRadius) { Destroy(); return; }
	LayoutRing();

	ACharacter* Player = Cast<ACharacter>(UGameplayStatics::GetPlayerPawn(this, 0));
	if (!Player || bHitPlayer) return;
	const float PlayerR = Player->GetCapsuleComponent()->GetScaledCapsuleRadius();
	const float D = FVector::Dist2D(Player->GetActorLocation(), GetActorLocation());
	// Crossed this frame? (sweep from last radius so a low frame rate can't skip the player)
	if (D + PlayerR < Prev - 30.f || D - PlayerR > Radius + 30.f) return;
	// Grounded = feet close to the floor the ring runs on. Jumping clears it.
	const float Feet = Player->GetActorLocation().Z - Player->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	if (Feet - GetActorLocation().Z > 45.f) return;

	bHitPlayer = true;
	if (AKPGameMode* GM = GetWorld()->GetAuthGameMode<AKPGameMode>()) GM->HurtPlayer(Damage, EKPDmgSrc::Boss, GetActorLocation());
	const FVector Out = (Player->GetActorLocation() - GetActorLocation()).GetSafeNormal2D();
	Player->LaunchCharacter(Out * 600.f + FVector(0, 0, 450.f), true, true);
}
