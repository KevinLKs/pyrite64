# Input Map

The input map lets you name your controls once, in the editor, and read them by name in
code and node graphs. Instead of asking "is the A button down?" everywhere, a script asks
"is **jump** pressed?". Changing which button jumps is then one setting, not a search
through every script.

Reading the controller directly through libdragon (`joypad_get_inputs` etc.) keeps
working, so you can adopt the input map one script at a time, or not at all.

## Defining actions

Open **Project > Settings** and expand **Input**.

- **Add Common Actions** fills in a starting set: `move` (stick and D-pad), `camera`
  (C-buttons), `confirm` (A), `cancel` (B), `pause` (Start), `trigger_z`, `shoulder_l`,
  `shoulder_r` and `menu_up/down/left/right` (D-pad or stick).
- **Button Action** and **Stick Action** add an empty action of that type.
- Click an action to edit it. Its name becomes a C++ constant, so it may only use
  letters, numbers and `_`, and must not start with a number.

A project can have up to 32 actions. Changes take effect on the next build.

### Button actions

On or off. The action is held while **any** of its ticked sources is held. Besides the
buttons, a button action can use a stick direction (for example "Stick Up" for a menu
cursor). **Stick Threshold** sets how far the stick has to be pushed to count, in raw
stick units (a new N64 stick reaches about 85).

### Stick actions

A direction with a strength, for movement and cameras. Sources can be the analog stick,
the D-pad and the C-buttons (digital sources always give full strength).

| Setting | Meaning |
|---------|---------|
| Deadzone | Raw distance from the centre that still reads as 0. Hides stick drift. |
| Full At | Raw distance that reads as full strength (1.0). Lower it so worn controllers still reach top speed. |
| Curve | Exponent applied to the strength. 1 is linear, above 1 gives finer control near the centre. |

The graph under the settings shows the result: stick distance left to right, strength
bottom to top.

## In C++ scripts

Include the generated header, then use the action constants in `Action::`:

```cpp
#include "p64/inputMap.h"

void update(Object& obj, Data *data, float deltaTime)
{
  if(Input::pressed(Action::jump)) { /* ... */ }

  auto move = Input::stick(Action::move);
  // move.x: -1 (left) to 1 (right), move.y: -1 (down) to 1 (up)
  // move.mag: 0 (centred) to 1 (pushed to the edge)
  obj.pos.x += move.x * SPEED * deltaTime;
  obj.pos.z -= move.y * SPEED * deltaTime;
}
```

| Function | Returns |
|----------|---------|
| `Input::held(action)` | True while the action is held. |
| `Input::pressed(action)` | True only on the frame it started being held. |
| `Input::released(action)` | True only on the frame it was let go. |
| `Input::repeated(action, port, delay, rate)` | Like `pressed`, then again after `delay` seconds and every `rate` seconds while held. Made for menu cursors. |
| `Input::heldTime(action)` | Seconds it has been held. |
| `Input::stick(action)` | `x`, `y` and `mag` of a stick action. For a button action `mag` is 1 while held. |

Every function takes an optional controller port as the last argument
(`JOYPAD_PORT_1` to `JOYPAD_PORT_4`, port 1 by default).

The values are updated once per frame by the engine, before any script runs.

### Walk and run with one stick

`stick().mag` grows smoothly as the stick is pushed further, so a slow walk on a light
push and a run at the edge comes down to your own speed formula:

```cpp
auto move = Input::stick(Action::move);
float speed = move.mag < 0.6f ? WALK_SPEED * (move.mag / 0.6f) : RUN_SPEED;
```

If a script works in raw stick units (for example a curve ported from another game),
set the action's **Deadzone** to 0 and **Full At** to the raw value you treat as full,
then `mag * FullAt` gives back the raw distance, capped at that value.

## In node graphs

The **Input** group in the node menu has:

| Node | Does |
|------|------|
| **Action** | 1 when the action is held / pressed / released / repeated this frame, else 0. Feeds an **If-Else** or math. |
| **If Action** | Takes the True or False path depending on an action. |
| **Wait For Action** | Pauses the graph until the action is pressed. |
| **Stick** | The X, Y or strength of a stick action. |
| **Stick Direction** | A stick action as a ground direction (stick right = +X, stick up = -Z), length 0 to 1. Multiply by a speed and **Delta Time** to move something. |

Type the action name into the node. A name that does not exist stops the build with a
message like `'jum' is not a member of 'P64::Action'; did you mean 'jump'?`.

## Generated files

Each build writes `src/p64/inputMap.h` (the `Action::` constants) and
`src/p64/inputMap.cpp` (the table the engine reads). Do not edit them, they are
rewritten from the project settings. The settings are stored in the project file under
`"input"`, so editor scripts can also change them with `project.setConfig({input: ...})`.
