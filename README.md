# GASP-UE4

GASP (Game Animation Sample Project) locomotion logic ported to Unreal Engine 4.27, C++ only.

- `Source/NINJA_PROJECT` - motion matching (`MotionMatchingComponent`, `MotionMatchingDatabase`), warping and anim nodes (offset root, orientation warping, steering, foot IK, spine twist, reset root), camera, traversal, hang, ledge finder, avoidance.
- `Source/NINJA_PROJECTEditor` - AnimGraph editor nodes for the anim nodes above.

No animation assets, attack/combat code or content are included. Module build files are trimmed to what these sources use and have not been compiled in isolation.
