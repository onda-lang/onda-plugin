import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";

// Execute the exact script passed to JUCE's withUserScript.
const source = readFileSync(new URL("../src/Editor.cpp", import.meta.url), "utf8");
const script = source.match(/constexpr auto juceHostBridgeScript = R"JS\(([\s\S]*?)\)JS";/)?.[1];
assert.ok(script, "Could not locate the editor's injected host bridge");

const commands = [];
let receiveState;
const window = {
  __JUCE__: {
    backend: {
      emitEvent(type, message) {
        assert.equal(type, "ondaCommand");
        commands.push(message);
      },
      addEventListener(type, callback) {
        assert.equal(type, "ondaState");
        receiveState = callback;
      },
    },
  },
  _onHostMessage() {},
};
runInNewContext(script, {
  window,
  document: { addEventListener() {}, querySelector() { return null; } },
  __ONDA_PARAM_CONTROL_V2__: {
    createParamDomain: ({ minimum, maximum }) => ({
      plainToNormalized: value => (value - minimum) / (maximum - minimum),
      normalizedToPlain: value => minimum + value * (maximum - minimum),
    }),
  },
});

const first = {
  name: "Slot 1 -> level", label: "level", type: "f32",
  hostValueDomain: "normalized", normalizedValue: 0.5,
  default: 0.5, rangeMin: 0, rangeMax: 1, scale: "linear",
  curve: null, unit: "", step: null, stepCount: null,
};
const second = { ...first, name: "Slot 2 -> mix", label: "mix" };
const state = (params, path = "patch.onda") => {
  receiveState({ type: "state", state: { path, params: params.map(p => ({ ...p })) } });
};
const drag = (parameter, value = 0.75, commit = false) => {
  window.ipc.postMessage({ type: "setParam", name: parameter.name, value, commit });
};
const assertEnded = (...names) => {
  assert.deepEqual(commands.splice(0).map(message => [message.type, message.name]),
    names.map(name => ["endParamGesture", name]));
};

state([first, second]);
drag(first);
drag(second);
commands.length = 0;
// DAW automation changes values without cancelling ongoing user gestures.
state([{ ...first, normalizedValue: 0.2 }, second]);
assertEnded();
// Reloading changed metadata cancels only the obsolete control, with no value write.
state([{ ...first, rangeMax: 100 }, second]);
assertEnded(first.name);
state([{ ...first, rangeMax: 100 }]);
assertEnded(second.name);
// Selecting a different program cancels even a control with identical metadata.
drag(first);
commands.length = 0;
state([{ ...first, rangeMax: 100 }], "other.onda");
assertEnded(first.name);
drag(first);
commands.length = 0;
state([], "");
assertEnded(first.name);
// A normally committed gesture must not be ended again on removal.
state([first]);
drag(first, 0.75, true);
commands.length = 0;
state([]);
assertEnded();
console.log("Editor bridge gesture cancellation passed");
