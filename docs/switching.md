# IK/FK and space switches

The picker's switches change how a limb or control is driven without moving
it. In the [docked picker](picker.md) and the
[hover picker](hover-picker.md):

- **IK/FK switches** (such as **Hand** or **Foot**) flip a limb between IK
  and FK on a click.
- **Space switches** open a menu of the control's spaces, such as world,
  chest, head or hips.

## What a matched switch does

- **To IK:** the IK control and pole go where the FK arm or leg already is.
- **To FK:** the FK controls are posed onto the IK limb.
- **A new space:** the control keeps its place in the new space.

Each switch is one undo step. With auto key on, it keys every channel it
sets.

Matching uses usdRig's own IK/FK and space matching, run against the rig
inside Maya, so it gives the same results as in usdview. The rest offsets
between each limb's controls and joints are measured when the rig loads.

!!! info "An IK limb bends only at its hinge"
    An FK pose that bends the elbow or knee off its hinge axis can't be
    matched exactly in IK: no IK pose bends that way. On the biped, the
    elbow's hinge is its Z axis.

If matching fails for any reason, the switch still happens, without
matching, and Maya shows a warning.
