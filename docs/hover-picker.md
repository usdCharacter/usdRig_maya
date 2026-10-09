# Hover picker

**RigExec > Hover Picker** shows the picker's tabs right in the viewport, as
floating buttons with no window around them. Choose it again to hide it.

![The hover picker's Face and Body tabs over the biped](images/hover-picker.png)

Anywhere that isn't a button, a handle or the opacity knob stays the
viewport's: a click there selects, box-selects, drags a manipulator or moves
the camera as usual. The hover picker shows in the viewport that has focus.

## Gestures

| On | Gesture | Does |
| --- | --- | --- |
| Handle | Drag | Move the tab; its buttons follow |
| Handle | ++shift++ + drag | Scale the tab |
| Handle | Double-click | Collapse the tab to its handle, or expand it |
| Handle | Hover | Show the tab's name |
| Button | Click | Select, with the [docked picker's](picker.md) modifiers |
| Button | Drag | Box-select within the tab |
| Button or handle | Middle-drag left or right | Fade the tab |
| Knob (bottom right) | Drag left or right | Fade every tab at once |
| Knob | Double-click | Back to full opacity |

Switches work as in the docked picker, and are [matched](switching.md).

## A hotkey

Maya's ++p++ is Parent, so the hover picker doesn't take a key of its own. To
give it one, bind **RigExecToggleHoverPicker** (under Custom Scripts >
RigExec) in Maya's Hotkey Editor.

## Your layout

Where each tab sits, its size, its opacity and whether it's collapsed are
saved per user, in the same place usdview keeps its hover picker layout. So
tabs you arrange in one show up the same way in the other. The layout is
never saved in the scene.

!!! note "The `rigExecHoverPicker` locator"
    Showing the hover picker adds a small locator, `rigExecHoverPicker`, to
    the scene: Viewport 2.0 draws the picker through it. If you hide
    locators in the viewport's **Show** menu, the hover picker hides too.
