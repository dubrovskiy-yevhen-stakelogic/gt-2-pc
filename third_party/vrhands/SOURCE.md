# UltimateXR hand assets

The hand geometry and source mask textures used to build these runtime assets
come from [VRMADA/ultimatexr-unity](https://github.com/VRMADA/ultimatexr-unity),
commit `985f20cb845bf94fa6307b1a6f969ff25a432399`.

Used source assets:

- `Runtime/Art/Avatars/BigHands/BigHandLeftGeo.fbx`
- `Runtime/Art/Avatars/BigHands/BigHandRightGeo.fbx`
- `Runtime/Art/Avatars/BigHands/HandPoses/Default.asset`
- `Runtime/Art/Avatars/BigHands/HandPoses/Grab.asset`
- `Runtime/Art/Avatars/BigHands/Textures/BigHandsTex_Mask1.png`
- `Runtime/Art/Avatars/BigHands/Textures/BigHandsTex_Mask2.png`
- `Runtime/Art/Avatars/BigHands/Textures/BigHandsTex_Mask3.png`

The FBX meshes are converted offline to the `UXRH` runtime format. The open,
grip, trigger and combined poses are baked from UltimateXR's authored finger
poses. `BigHandsAlbedo.png` is an offline Type 3 skin-colour approximation made
from the original masks; it does not include Unity or Shader Graph code.

UltimateXR is distributed under the MIT License. See
`ULTIMATEXR_LICENSE.txt` in this directory.

The current build embeds the artist-authored `GloveLeft` / `GloveRight` meshes
from the same two FBX files, with the original
`Runtime/Art/Avatars/BigHands/Textures/BigGlovesTechnicalTex_A.png` texture.
These are technical fabric gloves, not a licensed racing-brand asset.
`BigGloveLeft.uxrh` and `BigGloveRight.uxrh` contain four offline poses and
2,521 UV-split vertices / 4,444 triangles each. The original bare-hand files
remain as pose-transfer references.

Reproduce with `scripts/extract-ultimatexr-gloves.py` in background Blender,
then `scripts/bake-ultimatexr-gloves.py`. The latter reconstructs the existing
authored bone transforms from the hand pose files and applies the original
FBX skin weights to the glove mesh. It rejects incomplete UV matches and pose
transfer errors above 0.15 mm. The renderer's left-hand reflection and winding
correction remain unchanged. No Unity code or runtime download is required;
the glove meshes and original diffuse PNG are embedded in the executable.
