// AnimGraph editor node for FAnimNode_MidearthOffsetRoot (see Source/Midearth427/AnimNode_MidearthOffsetRoot.h).
#pragma once

#include "CoreMinimal.h"
#include "AnimGraphNode_Base.h"
#include "AnimNode_MidearthOffsetRoot.h"
#include "AnimGraphNode_MidearthOffsetRoot.generated.h"

UCLASS()
class UAnimGraphNode_MidearthOffsetRoot : public UAnimGraphNode_Base
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Settings")
	FAnimNode_MidearthOffsetRoot Node;

	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override { return FText::FromString(TEXT("Midearth Offset Root (GASP)")); }
	virtual FText GetTooltipText() const override { return FText::FromString(TEXT("UE5 Offset Root Bone port: body follows the clip's root motion, gap to the capsule damped. gasp.Warping >= 1.")); }
};
