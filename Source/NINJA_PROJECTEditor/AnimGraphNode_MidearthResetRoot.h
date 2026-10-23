// AnimGraph editor node for FAnimNode_MidearthResetRoot (see Source/Midearth427/AnimNode_MidearthResetRoot.h).
#pragma once

#include "CoreMinimal.h"
#include "AnimGraphNode_SkeletalControlBase.h"
#include "AnimNode_MidearthResetRoot.h"
#include "AnimGraphNode_MidearthResetRoot.generated.h"

UCLASS()
class UAnimGraphNode_MidearthResetRoot : public UAnimGraphNode_SkeletalControlBase
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "Settings")
	FAnimNode_MidearthResetRoot Node;

	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override { return FText::FromString(TEXT("Midearth Reset Root (GASP)")); }

protected:
	virtual FText GetControllerDescription() const override { return FText::FromString(TEXT("Midearth Reset Root (GASP)")); }
	virtual const FAnimNode_SkeletalControlBase* GetNode() const override { return &Node; }
};
