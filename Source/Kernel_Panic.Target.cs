using UnrealBuildTool;

public class Kernel_PanicTarget : TargetRules
{
	public Kernel_PanicTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("Kernel_Panic");
	}
}
