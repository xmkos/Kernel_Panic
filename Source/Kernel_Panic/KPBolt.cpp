#include "KPBolt.h"
#include "KPGameMode.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

AKPBolt::AKPBolt()
{
	PrimaryActorTick.bCanEverTick = true;
	InitialLifeSpan = 4.f;
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> BoltMesh(TEXT("/Game/KernelPanic/Models/SM_KP_Bolt.SM_KP_Bolt"));
	if (BoltMesh.Succeeded()) Mesh->SetStaticMesh(BoltMesh.Object);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
	UStaticMeshComponent* Trail = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Trail"));
	Trail->SetupAttachment(Mesh);
	Trail->SetStaticMesh(BoltMesh.Object);
	Trail->SetRelativeLocation(FVector(-45.f, 0.f, 0.f));
	Trail->SetRelativeScale3D(FVector(1.2f, 0.18f, 0.18f));
	Trail->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Trail->SetCastShadow(false);
	SetActorScale3D(FVector(0.4f));
}

void AKPBolt::BeginPlay()
{
	Super::BeginPlay();
}

void AKPBolt::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	const FVector From = GetActorLocation();
	const FVector To = From + GetActorForwardVector() * Speed * DeltaTime;

	// Hit the player? Check distance to the player's body line (feet to head).
	if (APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0))
	{
		const FVector P = Player->GetActorLocation();
		const FVector Closest = FMath::ClosestPointOnSegment(To, P - FVector(0, 0, 80.f), P + FVector(0, 0, 80.f));
		if (FVector::Dist(Closest, To) < 55.f)
		{
			if (AKPGameMode* GM = GetWorld()->GetAuthGameMode<AKPGameMode>())
			{
				GM->HurtPlayer(Damage, EKPDmgSrc::Bolt, GetActorLocation());
				GM->FxBurst(To, 7.f, 30.f, FLinearColor(2.f, 0.4f, 4.f), 0.16f);
			}
			Destroy();
			return;
		}
	}

	// Stop on walls, floor and the core.
	FHitResult Hit;
	if (GetWorld()->LineTraceSingleByObjectType(Hit, From, To, FCollisionObjectQueryParams(ECC_WorldStatic)))
	{
		if (AKPGameMode* GM = GetWorld()->GetAuthGameMode<AKPGameMode>())
			GM->FxBurst(Hit.ImpactPoint, 6.f, 24.f, FLinearColor(2.f, 0.4f, 4.f), 0.14f);
		Destroy();
		return;
	}
	SetActorLocation(To);
}
