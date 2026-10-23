// AnimGraph editor node for FAnimNode_MidearthFootIK (see Source/Midearth427/AnimNode_MidearthFootIK.h).
#pragma once

#include "CoreMinimal.h"
#include "AnimGraphNode_SkeletalControlBase.h"
#include "AnimNode_MidearthFootIK.h"
#include "AnimGraphNode_MidearthFootIK.generated.h"

UCLASS()
class UAnimGraphNode_MidearthFootIK : public UAnimGraphNode_SkeletalControlBase
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Settings")
	FAnimNode_MidearthFootIK Node;

	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override { return FText::FromString(TEXT("Midearth Foot IK")); }

protected:
	virtual FText GetControllerDescription() const override { return FText::FromString(TEXT("Midearth Foot IK")); }
	virtual const FAnimNode_SkeletalControlBase* GetNode() const override { return &Node; }
};
