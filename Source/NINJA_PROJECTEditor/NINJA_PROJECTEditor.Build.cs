using UnrealBuildTool;

// Editor-only module: AnimGraph editor nodes for the game module's anim nodes (Foot IK, Hand IK, Offset Root, ...).
public class NINJA_PROJECTEditor : ModuleRules
{
	public NINJA_PROJECTEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "AnimGraph", "AnimGraphRuntime", "BlueprintGraph", "UnrealEd", "NINJA_PROJECT" });
		// The game module keeps its headers in its root folder (no Public/), so add it to the include path.
		PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "..", "NINJA_PROJECT"));
	}
}
