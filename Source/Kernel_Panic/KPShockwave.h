#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "KPShockwave.generated.h"

class UInstancedStaticMeshComponent;

// Corrupted File slam: a glowing ring that runs along the floor. Jump over it or get hit.
UCLASS()
class KERNEL_PANIC_API AKPShockwave : public AActor
{
	GENERATED_BODY()

public:
	AKPShockwave();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	UPROPERTY(VisibleAnywhere) TObjectPtr<UInstancedStaticMeshComponent> Ring;
	float Radius = 150.f;
	float Speed = 950.f;
	float MaxRadius = 2600.f;
	float Damage = 20.f;

private:
	static constexpr int32 Segments = 72;
	bool bHitPlayer = false;
	void LayoutRing();
};
