// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class NINJA_PROJECT : ModuleRules
{
	public NINJA_PROJECT(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "AnimGraphRuntime", "AnimationCore" });
	}
}
