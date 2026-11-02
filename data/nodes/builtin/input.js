// Input-map builtin nodes. Actions are defined in Project > Settings > Input,
// these nodes read them through P64::Input (n64/engine/include/input/input.h).

const INPUT_COLOR = [90, 160, 220];
const INPUT_PORTS = ["1", "2", "3", "4"];
const INPUT_MODES = ["Held", "Pressed", "Released", "Repeated"];
const INPUT_MODE_FUNCS = ["held", "pressed", "released", "repeated"];
const INPUT_MODE_ICONS = ["gesture-tap-hold", "gesture-tap", "gesture-tap-button", "repeat"];

// C++ for "is this action <mode> on <port>", also pulls in the generated action names.
// An unknown action name fails the build with "'x' is not a member of 'P64::Action'".
function _inputCheck(n, ctx) {
  ctx.include('"p64/inputMap.h"');
  const func = INPUT_MODE_FUNCS[n.mode.index];
  return "P64::Input::" + func + "(P64::Action::" + n.action + ", (joypad_port_t)" + n.port.index + ")";
}

function _inputActionProps() {
  return {
    action: Str({ default: "confirm", width: 90 }),
    mode: Enum({ values: INPUT_MODES, icons: INPUT_MODE_ICONS, width: 90 }),
    port: Enum({ values: INPUT_PORTS, label: "Port", width: 40 }),
  };
}

// 1 while the action is held / was pressed / ..., else 0. Feeds If-Else or math.
node({
  id: "core.inputAction",
  name: icon("gamepad-circle-outline") + " Action",
  color: INPUT_COLOR,
  rounding: 4.0,
  category: "Input",
  outputs: [valueOut("i32")],
  props: _inputActionProps(),
  title: "{mode.icon} {action}",
  value(n, ctx) {
    return "(int32_t)" + _inputCheck(n, ctx);
  },
});

// Branches on an action: True if it is held / was pressed / ... this frame.
node({
  id: "core.inputIf",
  name: icon("call-split") + " If Action",
  color: INPUT_COLOR,
  rounding: 4.0,
  category: "Input",
  inputs: [logicIn()],
  outputs: [logicOut("True"), logicOut("False")],
  props: _inputActionProps(),
  title: "{mode.icon} If {action}",
  build(n, ctx) {
    ctx.localVar("bool", "t_input", _inputCheck(n, ctx))
       .line("if(t_input) {")
       .jump(0)
       .line("} else {")
       .jump(1)
       .line("}");
  },
});

// Pauses the flow until the action is pressed, then continues.
node({
  id: "core.inputWait",
  name: icon("timer-sand") + " Wait For Action",
  color: Color.green,
  category: "Input",
  inputs: [logicIn()],
  outputs: [logicOut()],
  props: {
    action: Str({ default: "confirm", width: 90 }),
    port: Enum({ values: INPUT_PORTS, label: "Port", width: 40 }),
  },
  title: icon("timer-sand") + " Wait For {action}",
  build(n, ctx) {
    ctx.include('"p64/inputMap.h"');
    ctx.line("while(!P64::Input::pressed(P64::Action::" + n.action + ", (joypad_port_t)" + n.port.index + ")) {")
       .line("  coro_yield();")
       .line("  inst->time += P64::VI::SwapChain::getDeltaTime();")
       .line("}");
  },
});

// One part of a stick action: X (-1 left, 1 right), Y (-1 down, 1 up) or its strength (0 to 1).
node({
  id: "core.inputStick",
  name: icon("gamepad-round-outline") + " Stick",
  color: INPUT_COLOR,
  rounding: 4.0,
  category: "Input",
  outputs: [valueOut("f32")],
  props: {
    action: Str({ default: "move", width: 90 }),
    part: Enum({ values: ["X", "Y", "Strength"], width: 90 }),
    port: Enum({ values: INPUT_PORTS, label: "Port", width: 40 }),
  },
  title: icon("gamepad-round-outline") + " {action} {part}",
  value(n, ctx) {
    ctx.include('"p64/inputMap.h"');
    const field = ["x", "y", "mag"][n.part.index];
    return "P64::Input::stick(P64::Action::" + n.action + ", (joypad_port_t)" + n.port.index + ")." + field;
  },
});

// A stick action as a ground direction: stick right = +X, stick up = -Z (forward),
// length 0 to 1. Multiply by a speed and Delta Time to move an object.
node({
  id: "core.inputStickDir",
  name: icon("arrow-all") + " Stick Direction",
  color: typeColor("vec3"),
  rounding: 4.0,
  category: "Input",
  outputs: [valueOut("vec3")],
  props: {
    action: Str({ default: "move", width: 90 }),
    port: Enum({ values: INPUT_PORTS, label: "Port", width: 40 }),
  },
  title: icon("arrow-all") + " {action} Direction",
  value(n, ctx) {
    ctx.include('"p64/inputMap.h"');
    return lambda("auto s = P64::Input::stick(P64::Action::" + n.action + ", (joypad_port_t)" + n.port.index + ");"
      + " return fm_vec3_t{s.x, 0.0f, -s.y};");
  },
});
