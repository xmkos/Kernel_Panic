using UnrealBuildTool;

public class Kernel_PanicEditorTarget : TargetRules
{
	public Kernel_PanicEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("Kernel_Panic");
	}
}
