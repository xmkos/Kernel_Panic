#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "KPFx.generated.h"

class UMaterialInstanceDynamic;
class UStaticMesh;
class UStaticMeshComponent;
class UProceduralMeshComponent;

// One glowing basic shape (cylinder or sphere) that scales and fades over its life. Recycled by AKPGameMode::PlayFx.
UCLASS()
class KERNEL_PANIC_API AKPFx : public AActor
{
	GENERATED_BODY()

public:
	AKPFx();
	virtual void Tick(float DeltaTime) override;
	// Scales are in mesh units (the engine shapes are 100 cm). bInDome: pulses, holds, fades at the end, and destroys itself instead of being recycled.
	void Play(bool bSphere, const FVector& At, const FRotator& Rot, const FVector& S0, const FVector& S1, const FLinearColor& Color, float InLife, float InOpacity, bool bInDome);
	void PlayArc(const FVector& At, const FRotator& Rot, float Radius, float ArcDeg, const FLinearColor& Color);

private:
	void Stop();
	UPROPERTY() TObjectPtr<UStaticMeshComponent> Mesh;
	UPROPERTY() TObjectPtr<UProceduralMeshComponent> Arc;
	UPROPERTY() TObjectPtr<UStaticMesh> Shapes[2]; // cylinder, sphere
	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> Mat;
	FVector Scale0 = FVector::OneVector, Scale1 = FVector::OneVector;
	float Life = 1.f, Age = 0.f, Opacity = 1.f;
	bool bDome = false;
};
