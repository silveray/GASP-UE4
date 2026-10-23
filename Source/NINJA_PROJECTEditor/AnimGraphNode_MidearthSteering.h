// AnimGraph editor node for FAnimNode_MidearthSteering (see Source/Midearth427/AnimNode_MidearthSteering.h).
#pragma once

#include "CoreMinimal.h"
#include "AnimGraphNode_SkeletalControlBase.h"
#include "AnimNode_MidearthSteering.h"
#include "AnimGraphNode_MidearthSteering.generated.h"

UCLASS()
class UAnimGraphNode_MidearthSteering : public UAnimGraphNode_SkeletalControlBase
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Settings")
	FAnimNode_MidearthSteering Node;

	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override { return FText::FromString(TEXT("Midearth Steering (GASP)")); }

protected:
	virtual FText GetControllerDescription() const override { return FText::FromString(TEXT("Midearth Steering (GASP)")); }
	virtual const FAnimNode_SkeletalControlBase* GetNode() const override { return &Node; }
};
