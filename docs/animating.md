# Animating

## Controls

Each rig control is a Maya transform, and you animate it with Maya's own
tools: the move, rotate and scale manipulators, the Channel Box, the Graph
Editor and the timeline.

- **Translate, rotate and scale** are the control's `avars:tx` ... `avars:sz`.
- **Other avars** are keyable attributes with the same names, without the
  `avars:` prefix: `avars:space` becomes `space`, `avars:ikfk` becomes
  `ikfk`.
- **Rotate order** comes from the control's `avars:rotationOrder`.
- **Shapes** come from the control's `guide:*` attributes (shape, plane,
  scale, orient, offset and colour).

On the biped, 261 controls have shapes. The other transforms (`*_cst`,
`*_aim`, `*_follow`, `*_auto`) are the rig's own constraint and helper
transforms. They have no shape, and the rig positions them, so leave them
alone.

!!! note "Channels the rig drives itself"
    Avars that the rig's own movers write aren't exposed in Maya: setting
    them would freeze that part of the rig.

## Meshes and materials

Meshes deform live as you pose. Each keeps the rig's `UsdPreviewSurface`
materials as Maya Standard Surfaces, with their colour, roughness,
metalness, opacity and emission, including materials bound to face subsets.
Other shaders, such as the biped's eye shader, show Maya's default material.
Press ++6++ in the viewport to see them.

## Switching IK/FK and spaces

Use the picker's switches, which match so nothing jumps. See
[IK/FK and space switches](switching.md). Setting `ikfk` or `space` directly
in the Channel Box works too, but isn't matched.

## Undo and auto key

Picker switches and **Zero Ctrls** are single undo steps. With auto key on,
a switch keys every channel it sets.
