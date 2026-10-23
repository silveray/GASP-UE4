// AnimGraph editor node for FAnimNode_MidearthOrientationWarping (see Source/Midearth427/AnimNode_MidearthOrientationWarping.h).
#pragma once

#include "CoreMinimal.h"
#include "AnimGraphNode_SkeletalControlBase.h"
#include "AnimNode_MidearthOrientationWarping.h"
#include "AnimGraphNode_MidearthOrientationWarping.generated.h"

UCLASS()
class UAnimGraphNode_MidearthOrientationWarping : public UAnimGraphNode_SkeletalControlBase
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Settings")
	FAnimNode_MidearthOrientationWarping Node;

	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override { return FText::FromString(TEXT("Midearth Orientation Warping (GASP)")); }

protected:
	virtual FText GetControllerDescription() const override { return FText::FromString(TEXT("Midearth Orientation Warping (GASP)")); }
	virtual const FAnimNode_SkeletalControlBase* GetNode() const override { return &Node; }
};
