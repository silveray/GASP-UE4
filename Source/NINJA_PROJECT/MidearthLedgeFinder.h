// Ported from the UE5.8 project (Source/Midearth/Characters/MidearthLedgeFinder) 10-01, unchanged logic (double -> float).
// Traversal — find the ledges of ANY obstacle.
//
// GASP's AC_TraversalLogic can only vault a LevelBlock_Traversable, because it asks
// the block where its edges are: four hand-placed spline "ledges" inside the block.
// Ordinary geometry has no splines. This works the same answer out from the
// obstacle's collision, so the GASP traversal runs on walls, rocks, crates — anything.
//
// It fills exactly the six S_TraversalCheckResult fields that
// LevelBlock_Traversable::GetLedgeTransforms fills, with the same conventions (read
// from that function and from AC_TraversalLogic::SetWarpTargets, 2026-09-14):
//   FrontLedgeLocation  on the top edge of the face that was hit, at least
//                       MinLedgeWidth/2 from either end of that edge
//   FrontLedgeNormal    flat, pointing OUT of the face toward the character
//                       (SetWarpTargets turns the character to face -FrontLedgeNormal)
//   BackLedgeLocation   on the far top edge, straight across from the front ledge
//   BackLedgeNormal     flat, pointing OUT of the far face, away from the character
//
// Stateless like UMidearthBallistics. Every trace is against the ONE component that
// was hit, never the world, so a character or prop standing on the obstacle cannot
// be mistaken for its top.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "MidearthLedgeFinder.generated.h"

class UPrimitiveComponent;

UCLASS()
class NINJA_PROJECT_API UMidearthLedgeFinder : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:

	/**
	 * Replaces "Cast To LevelBlock_Traversable -> Get Ledge Transforms" in
	 * AC_TraversalLogic::TryTraversalAction and writes the same six results.
	 *
	 * Takes the hit as three separate values rather than an FHitResult because that
	 * function's forward capsule trace has its Out Hit pin split, and a split pin
	 * cannot be recombined over the editor bridge.
	 *
	 * @param HitComponent     Out Hit Hit Component of the forward capsule trace.
	 * @param ImpactPoint      Out Hit Impact Point of the same trace.
	 * @param ImpactNormal     Out Hit Impact Normal of the same trace.
	 * @param ActorLocation    The traversing character's capsule centre.
	 * @param MinLedgeWidth    As on LevelBlock_Traversable (60): a narrower edge is not a ledge.
	 * @param MaxLedgeHeight   How far above ActorLocation the top may be. Taller: no ledge.
	 *                         This is the ONLY height limit: CHT_TraversalMontages_CMC has no
	 *                         height column, so without it a wall of any height gets a mantle
	 *                         warped up its full height.
	 * @param MaxObstacleDepth How far back to look for the far edge. A deeper obstacle is a
	 *                         platform and gets a front ledge only.
	 * @param bDrawDebug       Draw every trace (green hit, red miss) and the ledges found.
	 *
	 * Obstacles owned by a Pawn are always rejected: the capsule trace runs on Visibility,
	 * which weapons and shields block even though character capsules do not.
	 */
	UFUNCTION(BlueprintCallable, Category = "Midearth|Traversal", meta = (AdvancedDisplay = "MinLedgeWidth,MaxLedgeHeight,MaxObstacleDepth,DebugDuration"))
	static void FindLedges(
		UPrimitiveComponent* HitComponent,
		const FVector& ImpactPoint,
		const FVector& ImpactNormal,
		const FVector& ActorLocation,
		bool& HasFrontLedge,
		FVector& FrontLedgeLocation,
		FVector& FrontLedgeNormal,
		bool& HasBackLedge,
		FVector& BackLedgeLocation,
		FVector& BackLedgeNormal,
		float MinLedgeWidth = 60.0f,
		float MaxLedgeHeight = 125.0f,
		float MaxObstacleDepth = 250.0f,
		bool bDrawDebug = false,
		float DebugDuration = 2.0f);
};
