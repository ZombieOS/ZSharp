Z# Spring Bones (1.4.1.0)

From the repository root, build and open this example with a Z# runtime that
includes the native game renderer:
  zsharp package game examples/spring-bones SpringBones
  zsharp open examples/spring-bones/Packages/SpringBones.zgame

The blue and orange strips share the same skinned FBX and looping authored
ManeRoot animation. Each placement has its own spring simulation. A/D moves
the blue strip, Q/E rotates it, and Space switches its springs on or off.
With springs disabled, its authored animation continues playing.

Assets/Chain.fbx is a small original test mesh, not a character model. Its
three bones are ManeRoot -> ManeMid -> ManeTip. The final bone is the chain's
tip marker: the first two bones receive spring rotations, and actual FBX
vertex weights deform the mesh. Replace the FBX and these bone names with
your own rig to add motion to a mane, hair strand, tail, or similar chain.

Mane.zmodel contains the configuration. springBones[JSON] holds chain objects;
name and bones are required. Bone names must be unique and follow direct
parent-child links. Different chains cannot reuse a bone. The defaults are:
  stiffness: 20 (restoring acceleration, inverse seconds squared)
  damping: 4 (velocity decay, inverse seconds)
  gravity: {"x":0,"y":-9.8,"z":0} (world game units per second squared)
  radius: 0.02 (tip collision radius in game units before instance scaling)
  maxAngle: 60 (degrees away from the authored bone pose)

The optional springColliders[JSON] block defines simple animated body shapes.
name and bone identify each collider and its anchor. offset uses FBX bone-local
coordinates. A collider without tail is a sphere; adding a bone-local tail
makes a capsule between the two points. Radius must be positive (default 0.25).
Colliders follow the authored animation rather than the simulated spring pose.
The example includes both kinds. These constrain spring tips, not the game's
ordinary object-body collision system; they do not simulate cloth or a mesh.

springBonesEnabled: alive: is the default. Change one live placement with:
  Demo.Left.springBonesEnabled.set: dead:
  Demo.Left.springBonesEnabled.set: alive:
Use a status-valued expression to read that placement's current setting.

Springs run after authored .zanimation tracks at a fixed 120 Hz. A scene change,
large teleport, scale change, or long/nonfinite frame interval resets their
secondary motion. After a simulation step with springs disabled, turning them
back on starts fresh from the current authored pose.
Native desktop and PCVR renderers use the same resulting bone poses.
The FBX includes an embedded animation curve for regression verification;
this example deliberately plays Sway.zanimation, the supported runtime format.

Leave room for the chain to move around its body colliders. If a collider
encloses its entire reachable arc, or maxAngle prevents escape, the solver
preserves bone lengths and angle limits and may leave an intersection.
