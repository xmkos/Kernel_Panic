#include "KPFx.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"
#include "ProceduralMeshComponent.h"

AKPFx::AKPFx()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false; // only while playing
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cyl(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sph(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	// Shared additive shader: Color/Opacity, optional sphere rim and tapered arc.
	static ConstructorHelpers::FObjectFinderOptional<UMaterialInterface> Glow(TEXT("/Game/KernelPanic/Materials/M_KP_GlowPolish.M_KP_GlowPolish"), LOAD_Quiet | LOAD_NoWarn);
	static ConstructorHelpers::FObjectFinder<UMaterialInterface> Basic(TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	Shapes[0] = Cyl.Object;
	Shapes[1] = Sph.Object;
	Mesh->SetMaterial(0, Glow.Get() ? Glow.Get() : Basic.Object.Get());
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetGenerateOverlapEvents(false);
	Mesh->SetCastShadow(false);
	Arc = CreateDefaultSubobject<UProceduralMeshComponent>(TEXT("Arc"));
	Arc->SetupAttachment(Mesh);
	Arc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Arc->SetCastShadow(false);
	Arc->SetVisibility(false);
}

void AKPFx::Play(bool bSphere, const FVector& At, const FRotator& Rot, const FVector& S0, const FVector& S1, const FLinearColor& Color, float InLife, float InOpacity, bool bInDome)
{
	Mesh->SetVisibility(true);
	Arc->SetVisibility(false);
	Mesh->SetStaticMesh(Shapes[bSphere ? 1 : 0]);
	if (!Mat) Mat = Mesh->CreateDynamicMaterialInstance(0);
	else Mesh->SetMaterial(0, Mat); // swapping the mesh must not drop the instance
	Scale0 = S0; Scale1 = S1; Life = FMath::Max(InLife, 0.01f); Age = 0.f; Opacity = InOpacity; bDome = bInDome;
	SetActorLocationAndRotation(At, Rot);
	SetActorScale3D(S0);
	if (Mat)
	{
		Mat->SetVectorParameterValue(TEXT("Color"), Color);
		Mat->SetScalarParameterValue(TEXT("Opacity"), Opacity);
		Mat->SetScalarParameterValue(TEXT("RimStrength"), bSphere ? 1.f : 0.f);
		Mat->SetScalarParameterValue(TEXT("ArcShape"), 0.f);
	}
	SetActorHiddenInGame(false);
	SetActorTickEnabled(true);
}

void AKPFx::PlayArc(const FVector& At, const FRotator& Rot, float Radius, float ArcDeg, const FLinearColor& Color)
{
	Play(false, At, Rot, FVector(0.55f), FVector::OneVector, Color, 0.22f, 0.85f, false);
	TArray<FVector> Vertices;
	TArray<int32> Triangles;
	TArray<FVector2D> UVs;
	constexpr int32 Segments = 32;
	for (int32 i = 0; i <= Segments; ++i)
	{
		const float Angle = FMath::DegreesToRadians(ArcDeg * (float(i) / Segments - 0.5f));
		const FVector Direction(FMath::Cos(Angle), FMath::Sin(Angle), 0.f);
		Vertices.Add(Direction * Radius * 0.78f);
		Vertices.Add(Direction * Radius);
		UVs.Add(FVector2D(float(i) / Segments, 0.f));
		UVs.Add(FVector2D(float(i) / Segments, 1.f));
		if (i < Segments)
		{
			const int32 V = i * 2;
			Triangles.Append({ V, V + 1, V + 3, V, V + 3, V + 2 });
		}
	}
	Arc->CreateMeshSection_LinearColor(0, Vertices, Triangles, {}, UVs, {}, {}, false);
	Arc->SetMaterial(0, Mat);
	if (Mat) Mat->SetScalarParameterValue(TEXT("ArcShape"), 1.f);
	Mesh->SetVisibility(false);
	Arc->SetVisibility(true);
}

void AKPFx::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	Age += DeltaTime;
	const float A = Age / Life;
	if (A >= 1.f) { Stop(); return; }
	SetActorScale3D(FMath::Lerp(Scale0, Scale1, 1.f - FMath::Square(1.f - A)));
	const float Env = bDome ? FMath::Min(1.f, (1.f - A) / 0.15f) * (0.75f + 0.25f * FMath::Sin(Age * 8.f)) : 1.f - A;
	if (Mat) Mat->SetScalarParameterValue(TEXT("Opacity"), Opacity * Env * FMath::Min(Age / 0.018f, 1.f));
}

void AKPFx::Stop()
{
	if (bDome) { Destroy(); return; }
	SetActorHiddenInGame(true);
	SetActorTickEnabled(false);
}
