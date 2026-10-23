# GASP-UE4

C++ port of the locomotion logic of Epic's **Game Animation Sample Project (GASP, UE 5.x)** to **Unreal Engine 4.27**.
UE 4.27 has no Pose Search, no Motion Warping and no UE5 Animation Warping nodes, so the pieces GASP relies on are re-implemented here.

Only C++ is included: no animation assets, no Content, and no combat or attack code.
The module build files are trimmed to what these sources use and have not been compiled in isolation.

## How the pieces fit together

```
Input -> MotionMatchingComponent (picks the best clip from a MotionMatchingDatabase, plays it in DefaultSlot)
      -> AnimGraph: Slot -> Offset Root -> Steering -> Orientation Warping -> Reset Root -> Foot IK / Spine Twist
Character extras: GaspCameraComponent, TraversalComponent (+ LedgeFinder), HangComponent, AvoidanceComponent
```

The warping nodes only run when the console variable `gasp.Warping` is set: `0` = all nodes pass the pose through, `1` = Offset Root only, `2` = all (default).

## Game module: `Source/NINJA_PROJECT`

### Motion matching

| File | What it does |
| --- | --- |
| `MotionMatchingDatabase` | Data asset that stores every frame of the listed animations, described by one of GASP's search schemas: *Locomotion* (`PSS_Default`: 2D trajectory, foot positions and velocities, pelvis heading) or *Jump* (`PSS_Jump`: 3D trajectory). Fill `Sequences`, press **Build** in Details, save. Also holds GASP's per-clip biases and sampling ranges. |
| `MotionMatchingComponent` | The runtime. Predicts where the input is taking the character, finds the database frame that matches best and plays it with a short crossfade. Picks the database like GASP's chooser table: locomotion on the ground, crouch database while crouched, jump database in the air and right after landing. On the ground the clip's root motion moves the character; in the air Character Movement does. Can be suspended while a montage (traversal, hang, etc.) owns the body. |

### Warping anim nodes (ports of the UE5 AnimationWarping nodes)

| File | What it does |
| --- | --- |
| `MidearthWarping` | Shared helpers: the `gasp.Warping` console variable, the channel the Offset Root node shares with the nodes below it (UE 4.27 has neither root-motion attributes nor graph messages), and the game-thread state the GASP ABP functions read (IsMoving, desired facing, ...). |
| `AnimNode_MidearthOffsetRoot` | Port of *Offset Root Bone*. The visible body follows the animation's own root motion while the capsule moves independently; the gap between them is damped or kept. This is what lets the capsule turn instantly while the body turns with the clip. |
| `AnimNode_MidearthSteering` | Port of *Steering*. Turns the clip's root motion toward the desired facing: first scales the clip's own turn so it ends on the target, then adds a damped correction. Moves no bones itself; Offset Root applies the result. |
| `AnimNode_MidearthOrientationWarping` | Port of *Orientation Warping*. Rotates the lower body toward the real movement direction and counter-rotates the spine when the clip's root motion goes a different way than the character (for example a forward-left loop while moving straight). |
| `AnimNode_MidearthResetRoot` | Port of *Reset Root*. Puts the root bone back to its reference pose while its children keep their component-space transforms, so Offset Root receives a clean root after Orientation Warping. |
| `AnimNode_MidearthFootIK` | Foot IK for slopes and stairs (4.27 has no GASP Foot Placement node). Traces the ground under each foot, moves the feet with two-bone IK, tilts them to the ground and lowers the pelvis only as far as needed. Offsets are capped to avoid the body sinking into steps. |
| `AnimNode_MidearthSpineTwist` | Adds the yaw from `AvoidanceComponent` to the spine in component space, so the upper body turns away from a nearby wall. |

### Character components

| File | What it does |
| --- | --- |
| `GaspCameraComponent` | GASP-style third-person camera. Drives the spring arm and camera every frame: over-the-shoulder offset (swappable), distance, height and FOV blended from a Walk to a Run preset by ground speed, closer and higher when looking down, and spring-arm lag. Handles camera collision while a traversal moves the capsule. |
| `MidearthLedgeFinder` | Blueprint function library that finds the front and back ledges of **any** obstacle from its collision. GASP's traversal only works on hand-placed traversal blocks with spline ledges; this lets traversal work on ordinary walls, rocks and crates. |
| `TraversalComponent` | Climb, mantle, vault and hurdle on any geometry (replaces GASP traversal logic and Motion Warping). Sweeps ahead, asks the ledge finder for the ledges, picks the action from the obstacle's height and depth, picks a clip by gait, then moves the capsule itself, scaling and skewing the clip's root track so hands and feet land on the real ledge. |
| `HangComponent` | Ledge hang built on top of traversal: climb up to a hang, lower from a roof edge into a hang, shimmy left and right, climb up, hop up, or let go. Works together with `TraversalComponent`. |
| `AvoidanceComponent` | Wall reaction (port of the UE5 avoidance component). Two sideways sweeps along the path measure how close a wall or neutral NPC is on each side; the upper body turns away from it (via the Spine Twist node) and the capsule narrows in tight gaps. |

## Editor module: `Source/NINJA_PROJECTEditor`

AnimGraph editor nodes that expose the runtime nodes in the Animation Blueprint graph:
`AnimGraphNode_MidearthFootIK`, `OffsetRoot`, `OrientationWarping`, `ResetRoot`, `SpineTwist`, `Steering`.

## AnimBP setup

1. Mesh Animation Blueprint, minimum for motion matching: `Slot 'DefaultSlot' -> Output Pose`.
2. For warping, after the Slot in local space: **Offset Root**, then **Steering**, then **Orientation Warping**, then **Reset Root** (component space).
3. For IK: `Local To Component -> Midearth Foot IK -> Midearth Spine Twist -> Component To Local -> Output`.
4. Add `MotionMatchingComponent` (set `Database`, optionally `CrouchDatabase` and `JumpDatabase`), `GaspCameraComponent`, `TraversalComponent` and `AvoidanceComponent` to the Character.

## Notes

- Several comments refer to the original project's character class, dev panel and bridge commands; those are not part of this repo.
- `HangComponent` expects the character to create it and link it to the `TraversalComponent` in `BeginPlay`.
