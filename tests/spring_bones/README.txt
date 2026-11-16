This fixture was authored for Z# regression tests. It contains no third-party
artwork. Chain.fbx is a small ASCII FBX with a closed 12-vertex strip, three
direct-parent bones (ManeRoot -> ManeMid -> ManeTip), real Skin/Cluster weights
and inverse bind transforms, and an embedded root-rotation animation curve.

In its rest pose, joint positions are (0,0,0), (1,0,0), (2,0,0). Mesh bounds are
2 by 0.2 by 0.2. Each four-vertex ring is weighted to its corresponding bone.
The C regression consumes the public spring API and applies returned local
quaternions through ufbx's actual hierarchy evaluation and weighted skinning.
The FBX's embedded animation demonstrates valid animation data; runtime demos
use .zanimation tracks, since embedded FBX-stack playback is not a Z# feature.
