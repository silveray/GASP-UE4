// Ported from the UE5.8 project (Source/Midearth/Characters/MidearthLedgeFinder) 10-01, unchanged logic (double -> float).

#include "MidearthLedgeFinder.h"

#include "CollisionQueryParams.h"
#include "Components/PrimitiveComponent.h"
#include "DrawDebugHelpers.h"
#include "GameFramework/Pawn.h"

// A named namespace, not an anonymous one: unity builds merge .cpp files, and generic
// helper names in anonymous namespaces collide there.
namespace MidearthLedgeFinderPrivate
{
	// Close enough to the edge to land on top of a thin rail, far enough that a
	// downward ray does not graze the face it runs beside.
	constexpr float Inset = 3.0;

	// Flatter than about 45 degrees: something that can be stood on.
	constexpr float MinTopNormalZ = 0.7;

	// Steeper than about 60 degrees: a wall, not a ramp. Ramps and stairs would
	// otherwise report a "ledge" a few centimetres high on every jump.
	constexpr float MaxFaceNormalZ = 0.5;

	struct FLedgeTracer
	{
		UPrimitiveComponent* Component = nullptr;
		FCollisionQueryParams Params;
		bool bDraw = false;
		float Duration = 0.0f;

		/** A ray against the hit component only. A ray that starts inside it is not a hit. */
		bool Ray(const FVector& Start, const FVector& End, FHitResult& OutHit) const
		{
			OutHit = FHitResult();
			const bool bHit = Component->LineTraceComponent(OutHit, Start, End, Params) && OutHit.Time > KINDA_SMALL_NUMBER;
			if (bDraw)
			{
				DrawDebugLine(Component->GetWorld(), Start, bHit ? FVector(OutHit.ImpactPoint) : End, bHit ? FColor::Green : FColor::Red, false, Duration);
			}
			return bHit;
		}

		/** Straight down onto a surface that can be stood on. */
		bool Down(const FVector& From, const FVector& To, FHitResult& OutHit) const
		{
			return Ray(From, To, OutHit) && OutHit.ImpactNormal.Z >= MinTopNormalZ;
		}

		/** Standable top within Tolerance of Z, just behind EdgePoint. */
		bool TopNear(const FVector& EdgePoint, const FVector& FaceNormal, float Z, float Tolerance, FHitResult& OutHit) const
		{
			const FVector In = EdgePoint - FaceNormal * Inset;
			return Down(FVector(In.X, In.Y, Z + Tolerance), FVector(In.X, In.Y, Z - Tolerance), OutHit);
		}
	};

	FVector Flat(const FVector& V)
	{
		return FVector(V.X, V.Y, 0.0).GetSafeNormal();
	}

	/** Slide Point horizontally onto the plane of a vertical face. */
	FVector OntoFace(const FVector& Point, const FVector& FacePoint, const FVector& FaceNormal)
	{
		return Point + FaceNormal * FVector::DotProduct(FacePoint - Point, FaceNormal);
	}

	/**
	 * The face being approached. Re-hit with flat rays because a capsule's ImpactNormal
	 * on an edge points up the corner, which would tilt every ledge built from it.
	 */
	bool FindFace(const FLedgeTracer& Tracer, const FVector& ImpactPoint, const FVector& ImpactNormal, const FVector& Approach, FVector& OutPoint, FVector& OutNormal)
	{
		FHitResult Face;
		for (const float Drop : { 1.0, 20.0 })
		{
			const FVector Mid = ImpactPoint - FVector(0.0, 0.0, Drop);
			if (Tracer.Ray(Mid - Approach * 50.0, Mid + Approach * 50.0, Face))
			{
				if (FMath::Abs(Face.ImpactNormal.Z) > MaxFaceNormalZ)
				{
					return false;
				}
				OutPoint = Face.ImpactPoint;
				OutNormal = Flat(Face.ImpactNormal);
				return !OutNormal.IsNearlyZero();
			}
		}
		OutPoint = ImpactPoint;
		OutNormal = Flat(ImpactNormal);
		return !OutNormal.IsNearlyZero();
	}

	/** How far the top continues from EdgePoint along Dir, up to Limit. */
	float TopReach(const FLedgeTracer& Tracer, const FVector& EdgePoint, const FVector& Dir, const FVector& FaceNormal, float Limit)
	{
		FHitResult Unused;
		if (Tracer.TopNear(EdgePoint + Dir * Limit, FaceNormal, EdgePoint.Z, 10.0, Unused))
		{
			return Limit;
		}
		float Reached = 0.0;
		float Missed = Limit;
		for (int32 Step = 0; Step < 6; ++Step)
		{
			const float Mid = (Reached + Missed) * 0.5;
			if (Tracer.TopNear(EdgePoint + Dir * Mid, FaceNormal, EdgePoint.Z, 10.0, Unused))
			{
				Reached = Mid;
			}
			else
			{
				Missed = Mid;
			}
		}
		return Reached;
	}

	bool FindBackLedge(const FLedgeTracer& Tracer, const FVector& FrontLedge, const FVector& FrontNormal, float MaxDepth, FVector& OutLocation, FVector& OutNormal)
	{
		FHitResult Probe;
		const FVector Far = FrontLedge - FrontNormal * MaxDepth;

		// Still standing on it at MaxDepth: a platform, not something to get over.
		if (Tracer.TopNear(Far, FVector::ZeroVector, FrontLedge.Z, 10.0, Probe))
		{
			return false;
		}

		// From beyond the far side, a flat ray back toward the front just under the top
		// meets the far face from outside.
		const float RayZ = FrontLedge.Z - 2.0 * Inset;
		const FVector From(Far.X, Far.Y, RayZ);
		const FVector To = FVector(FrontLedge.X, FrontLedge.Y, RayZ) - FrontNormal * Inset;
		if (!Tracer.Ray(From, To, Probe))
		{
			return false;
		}
		const FVector BackFace = Probe.ImpactPoint;
		const FVector BackNormal = Flat(Probe.ImpactNormal);
		if (FVector::DotProduct(BackNormal, -FrontNormal) < 0.3)
		{
			return false;
		}

		const FVector BackInside = BackFace - BackNormal * Inset;
		if (!Tracer.Down(FVector(BackInside.X, BackInside.Y, FrontLedge.Z + 50.0), FVector(BackInside.X, BackInside.Y, BackFace.Z - 1.0), Probe))
		{
			return false;
		}
		OutLocation = OntoFace(Probe.ImpactPoint, BackFace, BackNormal);
		OutNormal = BackNormal;
		return true;
	}
}

void UMidearthLedgeFinder::FindLedges(
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
	float MinLedgeWidth,
	float MaxLedgeHeight,
	float MaxObstacleDepth,
	bool bDrawDebug,
	float DebugDuration)
{
	using namespace MidearthLedgeFinderPrivate;

	HasFrontLedge = false;
	HasBackLedge = false;
	FrontLedgeLocation = FVector::ZeroVector;
	FrontLedgeNormal = FVector::ZeroVector;
	BackLedgeLocation = FVector::ZeroVector;
	BackLedgeNormal = FVector::ZeroVector;

	if (!HitComponent || Cast<APawn>(HitComponent->GetOwner()))
	{
		return;
	}

	FLedgeTracer Tracer;
	Tracer.Component = HitComponent;
	Tracer.Params = FCollisionQueryParams(FName(TEXT("MidearthLedgeFinder")), false);
	Tracer.bDraw = bDrawDebug;
	Tracer.Duration = DebugDuration;

	FVector Approach = Flat(ImpactPoint - ActorLocation);
	if (Approach.IsNearlyZero())
	{
		Approach = -Flat(ImpactNormal);
	}

	FVector FacePoint;
	FVector FaceNormal;
	if (Approach.IsNearlyZero()
		|| !FindFace(Tracer, ImpactPoint, ImpactNormal, Approach, FacePoint, FaceNormal)
		|| FVector::DotProduct(FaceNormal, -Approach) < 0.3) // a face turned away is not the one being approached
	{
		return;
	}

	// The top: straight down just inside the face, from the highest ledge allowed.
	const float SearchTop = ActorLocation.Z + MaxLedgeHeight;
	const FVector Inside = FacePoint - FaceNormal * Inset;
	FHitResult Top;
	if (SearchTop <= FacePoint.Z
		|| !Tracer.Down(FVector(Inside.X, Inside.Y, SearchTop), FVector(Inside.X, Inside.Y, FacePoint.Z - 1.0), Top))
	{
		return;
	}
	FVector Ledge = OntoFace(Top.ImpactPoint, FacePoint, FaceNormal);

	// Width, the LevelBlock_Traversable way: keep the ledge MinLedgeWidth/2 from both
	// ends of the edge, sliding it inward when the hit was near a corner.
	const FVector Along = FVector::CrossProduct(FVector::UpVector, FaceNormal).GetSafeNormal();
	const float Half = MinLedgeWidth * 0.5;
	const float ReachA = TopReach(Tracer, Ledge, Along, FaceNormal, MinLedgeWidth);
	const float ReachB = TopReach(Tracer, Ledge, -Along, FaceNormal, MinLedgeWidth);
	if (ReachA + ReachB < MinLedgeWidth - 1.0)
	{
		return;
	}
	if (ReachA < Half)
	{
		Ledge -= Along * (Half - ReachA);
	}
	else if (ReachB < Half)
	{
		Ledge += Along * (Half - ReachB);
	}
	if (Tracer.TopNear(Ledge, FaceNormal, Ledge.Z, 10.0, Top))
	{
		Ledge.Z = Top.ImpactPoint.Z;
	}

	HasFrontLedge = true;
	FrontLedgeLocation = Ledge;
	FrontLedgeNormal = FaceNormal;

	HasBackLedge = FindBackLedge(Tracer, FrontLedgeLocation, FrontLedgeNormal, MaxObstacleDepth, BackLedgeLocation, BackLedgeNormal);
	if (!HasBackLedge)
	{
		// TryTraversalAction still sweeps toward BackLedgeLocation to measure depth. Left
		// at zero it would sweep toward the world origin; a point across the top keeps it local.
		BackLedgeLocation = FrontLedgeLocation - FrontLedgeNormal * MaxObstacleDepth;
		BackLedgeNormal = -FrontLedgeNormal;
	}

	if (bDrawDebug)
	{
		const UWorld* World = HitComponent->GetWorld();
		DrawDebugSphere(World, FrontLedgeLocation, 10.0f, 12, FColor::Green, false, DebugDuration);
		if (HasBackLedge)
		{
			DrawDebugSphere(World, BackLedgeLocation, 10.0f, 12, FColor::Cyan, false, DebugDuration);
		}
	}
}
