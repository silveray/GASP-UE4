// AnimGraph editor node for FAnimNode_MidearthSpineTwist (see Source/Midearth427/AnimNode_MidearthSpineTwist.h).
#pragma once

#include "CoreMinimal.h"
#include "AnimGraphNode_SkeletalControlBase.h"
#include "AnimNode_MidearthSpineTwist.h"
#include "AnimGraphNode_MidearthSpineTwist.generated.h"

UCLASS()
class UAnimGraphNode_MidearthSpineTwist : public UAnimGraphNode_SkeletalControlBase
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Settings")
	FAnimNode_MidearthSpineTwist Node;

	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override { return FText::FromString(TEXT("Midearth Spine Twist")); }

protected:
	virtual FText GetControllerDescription() const override { return FText::FromString(TEXT("Midearth Spine Twist")); }
	virtual const FAnimNode_SkeletalControlBase* GetNode() const override { return &Node; }
};
