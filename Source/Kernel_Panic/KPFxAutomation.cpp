#if WITH_DEV_AUTOMATION_TESTS
#include "KPFx.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "ProceduralMeshComponent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FKPFxGeometry, "KernelPanic.Effects.GeometryAndReuse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FKPFxGeometry::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
		if (Context.WorldType == EWorldType::Editor) { World = Context.World(); break; }
	if (!TestNotNull(TEXT("Editor world"), World)) return false;
	AKPFx* Fx = World->SpawnActor<AKPFx>();
	if (!TestNotNull(TEXT("Effect spawned"), Fx)) return false;
	auto* Arc = Fx->FindComponentByClass<UProceduralMeshComponent>();
	auto* Mesh = Fx->FindComponentByClass<UStaticMeshComponent>();
	for (float Radius : { 220.f, 280.f })
	{
		Fx->PlayArc(FVector::ZeroVector, FRotator(0, 90, 0), Radius, 120.f, FLinearColor::White);
		const FProcMeshSection* Section = Arc->GetProcMeshSection(0);
		if (!TestNotNull(TEXT("Arc section"), Section)) break;
		TestEqual(TEXT("32 arc quads"), Section->ProcIndexBuffer.Num(), 192);
		for (const FProcMeshVertex& V : Section->ProcVertexBuffer)
		{
			TestTrue(TEXT("Every vertex stays inside forward +/-60 degree sector"),
				V.Position.X > 0.f && FMath::Abs(FMath::RadiansToDegrees(FMath::Atan2(V.Position.Y, V.Position.X))) <= 60.001f);
			TestTrue(TEXT("Arc never exceeds damage range"), V.Position.Size() <= Radius + 0.001f);
		}
		TestTrue(TEXT("Arc visible, basic shape hidden"), Arc->IsVisible() && !Mesh->IsVisible());
		Fx->Tick(0.1f);
		TestTrue(TEXT("Arc grows toward full range"), Fx->GetActorScale3D().X > 0.55f && Fx->GetActorScale3D().X <= 1.f);
		for (bool Sphere : { false, true })
		{
			Fx->Play(Sphere, FVector(1, 2, 3), FRotator::ZeroRotator, FVector(2), FVector(3), FLinearColor::Red, 0.2f, 0.8f, false);
			TestTrue(TEXT("Pool reuse restores shape and hides arc"), Mesh->IsVisible() && !Arc->IsVisible());
			TestEqual(TEXT("Pool reuse resets scale"), Fx->GetActorScale3D(), FVector(2));
			Fx->Tick(0.3f);
			TestTrue(TEXT("Expired pooled effect hides and stops ticking"), Fx->IsHidden() && !Fx->IsActorTickEnabled());
		}
	}
	Fx->Destroy();
	return !HasAnyErrors();
}
#endif
