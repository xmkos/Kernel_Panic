#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Components/CapsuleComponent.h"
#include "KPEnemy.generated.h"

class UMaterialInstanceDynamic;
class UStaticMesh;
class UStaticMeshComponent;
class AKPFx;
class AKPGameMode;

// Final boss scheduled attacks (Tools/Plans/FINAL_BOSS.md): one committed attack at a time.
enum class ERkAtk : uint8 { None, Blink, Ring, Lanes, Hook, Fork };

// Hostile process archetypes from the design doc.
UENUM()
enum class EKPEnemyType : uint8
{
	Rusher, // closes distance on the player fast
	Ranged, // keeps distance and fires slow bolts
	Tank,   // slow and tough, goes for the core
	Boss,      // "Corrupted File": splits into rushers on death
	Ransomware, // boss: locks one player ability at a time
	Rootkit     // boss (CPU): five scheduled attacks (Blink Cross, Interrupt Ring, Root Lanes, Core Hook, Fork), three phases. Keep LAST: EKPDmgSrc and KillsByType mirror this order.
};

UCLASS()
class KERNEL_PANIC_API AKPEnemy : public ACharacter
{
	GENERATED_BODY()

public:
	AKPEnemy();
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaTime) override;
	virtual float TakeDamage(float Damage, const FDamageEvent& DamageEvent, AController* EventInstigator, AActor* DamageCauser) override;

	// Call right after spawning.
	void Init(EKPEnemyType InType, int32 Wave);
	bool IsDead() const { return Health <= 0.f; }
	float GetHealth() const { return Health; }
	float GetMaxHealth() const { return MaxHealth; }
	void Knockback(const FVector& Dir, float Strength);
	// Middle of the visible model (shots and aim assist aim here, not at the feet-level capsule center).
	FVector GetAimPoint() const;
	float GetBodyRadius() const;
	bool IsBossType() const { return Type >= EKPEnemyType::Boss; } // Boss, Ransomware, Rootkit
	FLinearColor GetColor() const { return BaseColor; }

	int32 GetRkPhase() const { return RkPhase; } // 1 RESIDENT, 2 FORKED, 3 EXPOSED (HUD)

	// Final boss geometry (cm; Tools/Plans/FINAL_BOSS.md + CPU_LEVEL.md). Pure, so tests need no timing. Anchors: index 0..3 = +X, +Y, -X, -Y at 10 m from the core (the arena centre is the origin).
	static FVector RkAnchorPos(int32 I) { return FVector(I == 0 ? 1000.f : I == 2 ? -1000.f : 0.f, I == 1 ? 1000.f : I == 3 ? -1000.f : 0.f, 0.f); }
	static int32 RkPhaseOf(float Hp01) { return Hp01 < 0.33f ? 3 : Hp01 < 0.66f ? 2 : 1; }
	static int32 RkPickAnchor(const FVector& Player, const FVector& Here, uint8 ClearMask, int32 Roll); // legal = clear, >= 400 cm from the player, not where the boss already stands; -1 = none
	static bool RkRingSwept(float R0, float R1, float D, float PlayerR) { return !(D + PlayerR < R0 - 30.f || D - PlayerR > R1 + 30.f); } // 60 cm band, radius grew R0 -> R1 this tick
	static bool RkInStrip(const FVector& P, const FVector& O, const FVector& Dir, float Len, float HalfW, float PlayerR); // XY: strip from O along unit Dir
	static float RkCoverClip(const FVector& O, const FVector& Dir, float Len); // strip length stopped at a 300x300 cache cover at (+-1000, +-1000)

	UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Body;
	EKPEnemyType Type = EKPEnemyType::Rusher;

private:
	friend class FKPRootkit;
	void Die(bool bCountAsKill);
	void MoveToward(const FVector& Target, float Sign = 1.f);
	void SetColor(const FLinearColor& C);

	UPROPERTY() TObjectPtr<UMaterialInstanceDynamic> Mat;
	UPROPERTY() TObjectPtr<UStaticMesh> TypeMeshes[6]; // indexed by EKPEnemyType
	FLinearColor RestFlash = FLinearColor::Black;      // emissive tint when not flashing
	float LockTimer = 4.f;                             // Ransomware: time to next ability lock
	// Boss pressure so the player can't just run away
	void BossTick(class AKPGameMode* GM, APawn* Player, float DeltaTime, bool bTouchingPlayer);
	float DashTimer = 3.f;      // Corrupted File: time to next dash
	float TelegraphLeft = 0.f;  // >0 = winding up a dash (stands still, glows)
	float DashLeft = 0.f;       // >0 = dashing (contact hurts more)
	float FanTimer = 2.5f;      // Ransomware: time to next bolt fan
	float SlamTimer = 6.f;      // Corrupted File: time to next ground slam (shockwave you must jump)
	float SlamWindup = 0.f;     // >0 = winding up a slam (stands still, glows)
	float SecondSlamIn = 0.f;   // >0 = a follow-up ring is queued (below half health)
	float BodySize = 1.f;       // model scale at rest
	// --- Rootkit (final boss) ---
	void RkTick(AKPGameMode* GM, APawn* Player, float Dt);
	void RkStart(AKPGameMode* GM, APawn* Player, ERkAtk A);
	void RkEnd(float Recover);
	void RkBeginDeath();
	void RkDeathTick(float Dt);
	void RkPose(float Dt);
	void RkInitParts();
	void InitAnchor();
	bool RkClear(const FVector& P, float R, float HH) const;
	bool RkCommitted() const { return RkAtk == ERkAtk::Hook || (RkAtk == ERkAtk::Blink && RkAtkT >= 1.f) || (RkAtk == ERkAtk::Ring && RkAtkT >= 1.2f) || (RkAtk == ERkAtk::Lanes && RkAtkT >= 1.1f); }
	void RkRect(AKPGameMode* GM, const FVector& C, const FVector& Dir, float HL, float HW, bool bSolid, float Life) const; // red floor footprint: outline (tell) or flat slab (impact)
	void RkDisc(AKPGameMode* GM, const FVector& C, float R, const FLinearColor& Col, float Life) const;
	float RkFloorZ() const { return GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight(); }
	ERkAtk RkAtk = ERkAtk::None, RkLast = ERkAtk::None;
	int32 RkPhase = 1, RkBlinkIdx = -1, RkForceMask = -1; // RkForceMask >= 0: tests replace the blink clearance mask
	float RkAtkT = 0.f, RkRecover = 2.f, RkWalk = 2.f, RkLookV = 0.f, RkLookRate = 1.f, RkLen[3] = {}, RkFold = 0.f, RkRingR = 0.f, RkHookDealt = 0.f, RkDeadT = 0.f;
	bool bForkPending = false, bRkHit = false, bRkDying = false, bRemoved = false, bRkMulti = false, bAnchor = false;
	FVector RkDest = FVector::ZeroVector, RkDir = FVector::ForwardVector, RkOrigin = FVector::ZeroVector, RkSpot[3] = {};
	int32 RkSpots = 0; // bit i set = RkSpot[i] is a legal spawn disc
	UPROPERTY() TObjectPtr<AKPEnemy> RkAnchor;
	UPROPERTY() TObjectPtr<UStaticMeshComponent> RkParts[11]; // CrownA..D, Body (= Body), RootA..D, Pins, Kernel
	UPROPERTY() TObjectPtr<UStaticMesh> RkMesh[11];
	UPROPERTY() TArray<TObjectPtr<UMaterialInstanceDynamic>> PartMats;
	TArray<TWeakObjectPtr<AKPEnemy>> RkKids;
	TWeakObjectPtr<AKPFx> RkRingFx;
	float MaxHealth = 1.f;
	void SpawnShockwave(class AKPGameMode* GM);
	FLinearColor BaseColor = FLinearColor::Red;       // accent, used for the ranged "about to fire" glow
	float Health = 3.f;
	float ContactDps = 20.f;
	float FireTimer = 2.f;
	float FlashTime = 0.f;

	// Navmesh path following
	TArray<FVector> PathPoints;
	int32 PathIndex = 0;
	float RepathTimer = 0.f;
	FVector LastGoal = FVector::ZeroVector;
	FVector StuckCheckPos = FVector::ZeroVector;
	float StuckTimer = 0.f;
};
