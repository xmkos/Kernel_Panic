using UnrealBuildTool;

public class Kernel_Panic : ModuleRules
{
	public Kernel_Panic(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "NavigationSystem", "Slate", "SlateCore", "ProceduralMeshComponent" });
	}
}
