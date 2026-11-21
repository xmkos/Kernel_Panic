#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "KPBolt.generated.h"

// Slow, dodgeable projectile fired by ranged enemies.
UCLASS()
class KERNEL_PANIC_API AKPBolt : public AActor
{
	GENERATED_BODY()

public:
	AKPBolt();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Mesh;
	float Speed = 1300.f;
	float Damage = 12.f;
};
