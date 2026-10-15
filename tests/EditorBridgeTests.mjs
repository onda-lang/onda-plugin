import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { runInNewContext } from "node:vm";

// Execute the exact script passed to JUCE's withUserScript.
const source = readFileSync(new URL("../src/Editor.cpp", import.meta.url), "utf8");
const script = source.match(/constexpr auto juceHostBridgeScript = R"JS\(([\s\S]*?)\)JS";/)?.[1];
assert.ok(script, "Could not locate the editor's injected host bridge");

const commands = [];
let receiveState;
let renderedState;
let documentReady;
const smoothingAttributes = {};
const sampleRateRow = {};
const sampleRate = { closest() { return sampleRateRow; } };
const runSettings = { style: {} };
const blockSelect = { replaceChildren(...options) { this.options = options; } };
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
  _onHostMessage(message) { renderedState = message.state; },
};
runInNewContext(script, {
  window,
  document: {
    addEventListener(type, callback) {
      if (type === "DOMContentLoaded") documentReady = callback;
    },
    querySelector() { return null; },
    getElementById(id) {
      if (id === "run-sample-rate") return sampleRate;
      if (id === "run-block-size") return blockSelect;
      if (id === "file-picker-settings") return runSettings;
      assert.equal(id, "param-smoothing-ms");
      return { setAttribute(name, value) { smoothingAttributes[name] = value; } };
    },
    createElement() { return {}; },
  },
  __ONDA_PARAM_CONTROL_V2__: {
    createParamDomain: ({ minimum, maximum }) => ({
      plainToNormalized: value => (value - minimum) / (maximum - minimum),
      normalizedToPlain: value => minimum + value * (maximum - minimum),
    }),
  },
});
documentReady();
assert.equal(smoothingAttributes["aria-label"], "Editor parameter smoothing duration in milliseconds");
assert.match(smoothingAttributes.title, /DAW automation updates at block boundaries/);
assert.equal(sampleRate.disabled, true);
assert.equal(sampleRateRow.hidden, true);
const blockChoices = () => Array.from(blockSelect.options)
  .filter(option => !option.hidden).map(option => option.value);
assert.deepEqual(blockChoices(), ["0"]);
assert.equal(blockSelect.disabled, true);
receiveState({ type: "state", state: {
  hostBlockSamples: 96, preferredBlockSamples: 0, canSetBlockSize: true,
} });
assert.deepEqual(Array.from(blockSelect.options, option => option.value),
                 ["96", "48", "24", "12"]);
assert.equal(blockSelect.value, "96");
assert.equal(blockSelect.disabled, false);
window.ipc.postMessage({ type: "setRunSettings", sampleRateHz: 48000, blockSamples: 96 });
assert.equal(commands.pop().blockSamples, 0, "The maximum must follow the host");
window.ipc.postMessage({ type: "setRunSettings", sampleRateHz: 48000, blockSamples: 48 });
assert.equal(commands.pop().blockSamples, 48);
// Normal loaded-view rendering must not append a duplicate maximum or change
// the menu selection on each ordinary parameter update.
receiveState({ type: "state", state: {
  hostBlockSamples: 96, preferredBlockSamples: 0, canSetBlockSize: true,
} });
assert.deepEqual(blockChoices(), ["96", "48", "24", "12"]);
assert.equal(blockSelect.value, "96");
// Preserve an active fixed size after a host-capacity change while offering
// only the maximum and its subdivisions as new choices.
receiveState({ type: "state", state: {
  hostBlockSamples: 96, preferredBlockSamples: 32, canSetBlockSize: false,
} });
assert.equal(blockSelect.value, "32");
assert.equal(blockSelect.disabled, true);
assert.deepEqual(blockChoices(), ["96", "48", "24", "12"]);
assert.equal(blockSelect.options.at(-1).disabled, true);
receiveState({ type: "state", state: {
  hostBlockSamples: 64, preferredBlockSamples: 32, canSetBlockSize: true,
} });
assert.deepEqual(blockChoices(), ["64", "32", "16", "8"]);
assert.equal(blockSelect.value, "32");
receiveState({ type: "state", state: {
  hostBlockSamples: 101, preferredBlockSamples: 0, canSetBlockSize: true,
} });
assert.deepEqual(blockChoices(), ["101", "50", "25", "12"]);
receiveState({ type: "state", state: {
  hostBlockSamples: 2, preferredBlockSamples: 0, canSetBlockSize: true,
} });
assert.deepEqual(blockChoices(), ["2", "1"]);

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
// Event commands belong to the last rendered event schema, even if the native
// worker has already published a replacement. Keep 64-bit generations exact.
const eventState = generation => receiveState({
  type: "state", state: {
    engineGeneration: generation,
    events: [{ name: "configure", args: [] }],
  },
});
const trigger = () => window.ipc.postMessage(JSON.stringify({
  type: "triggerEvent", name: "configure", values: [0.75],
}));
eventState("9007199254740993");
trigger();
assert.equal(commands.pop().engineGeneration, "9007199254740993");
// A partial update must not reassign the event controls' generation.
state([first]);
trigger();
assert.equal(commands.pop().engineGeneration, "9007199254740993");
eventState("9007199254740994");
trigger();
assert.equal(commands.pop().engineGeneration, "9007199254740994");
console.log("Editor bridge gesture cancellation and event generations passed");

// Input controls share conversion and gesture lifetimes without sharing a bank.
const control = { ...first, name: "Input 1 -> cv", rangeMin: -2, rangeMax: 2, enabled: true };
receiveState({ type: "state", state: { path: "input.onda", params: [first], inputControls: [control] } });
commands.length = 0;
window.ipc.postMessage({ type: "setInputControl", name: control.name, value: 1, commit: false });
assert.equal(commands.pop().value, 0.75);
receiveState({ type: "state", state: { params: [{ ...first, normalizedValue: 0.25 }] } });
assertEnded();
receiveState({ type: "state", state: { inputControls: [{ ...control, enabled: false }] } });
assert.deepEqual(commands.splice(0).map(message => [message.type, message.name]),
  [["endInputControlGesture", control.name]]);
receiveState({ type: "state", state: { inputControls: [{ ...control, rangeMin: 2, rangeMax: 2 }] } });
window.ipc.postMessage({ type: "setInputControl", name: control.name, value: 2, commit: true });
assert.equal(commands.pop().value, control.normalizedValue);

receiveState({ type: "state", state: { inputControls: [{ ...control,
  rangeMin: -1e30, rangeMax: 1e30, normalizedValue: 0.5, value: 0.25 }] } });
assert.equal(renderedState.inputControls[0].value, 0.25, "Exact plain defaults survive UI normalization");

for (const value of ["9007199254740993", "-9007199254740993",
                     "9223372036854775807", "-9223372036854775808"]) {
  state([{ ...first, type: "i64", value, default: value, step: 1, stepCount: 1 }]);
  assert.equal(renderedState.params[0].value, value, "Exact i64 defaults survive bridge conversion");
}

const diagnosticScript = source.match(/constexpr auto juceEditorDiagnosticScript = R"JS\(([\s\S]*?)\)JS";/)?.[1];
assert.ok(diagnosticScript, "Could not locate the opt-in startup diagnostics");
const diagnosticEvents = new Map();
const diagnostics = [];
runInNewContext(diagnosticScript, { window: {
  addEventListener: (name, callback) => diagnosticEvents.set(name, callback),
  __JUCE__: { backend: { emitEvent(name, message) {
    assert.equal(name, "ondaCommand");
    assert.equal(message.type, "editorDiagnostic");
    diagnostics.push(message.message);
  } } },
} });
diagnosticEvents.get("error")({ error: { stack: "startup exception" } });
diagnosticEvents.get("error")({ target: { src: "juce://juce.backend/number-input.js" } });
diagnosticEvents.get("unhandledrejection")({ reason: { stack: "startup rejection" } });
assert.deepEqual(diagnostics, [
  "JavaScript initialization script started", "startup exception",
  "Resource failed: juce://juce.backend/number-input.js", "startup rejection",
]);
