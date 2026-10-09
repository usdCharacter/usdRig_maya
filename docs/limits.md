# Limits

## Controls and channels

- Only translate, rotate and scale get Maya manipulators. The other avars
  are plain keyable attributes.
- Avars that the rig's own movers write aren't exposed, because overriding
  them would freeze the rig.
- Control shapes come from `guide:shape`, not from authored curves.
- A channel the bake doesn't take as an input can be animated in Maya but
  doesn't move the rig. Loading the rig warns how many there are.
- Rig joints aren't built in Maya, so parts posed through joints, such as
  the biped's fingers, can't be animated yet, and their picker buttons don't
  appear.

## Switches

- An IK limb bends only at its hinge, so an FK pose that bends the elbow or
  knee off its hinge axis can't be matched exactly in IK.

## Animation

- The rig is baked at one frame, and Maya supplies every pose: animation
  stored in the USD file itself isn't played.

## Materials

- Only `UsdPreviewSurface` materials come across, as Maya Standard Surfaces
  with their colour, roughness, metalness, opacity and emission. Other
  shaders, such as the biped's eye shader, show Maya's default material.
- Textures aren't carried over. Meshes get their `st` UVs.

## Platform

- Built and tested on Windows with Maya 2027.
