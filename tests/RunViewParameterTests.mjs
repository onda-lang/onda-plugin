import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { readFile, mkdtemp, rm, writeFile } from "node:fs/promises";
import { createServer } from "node:http";
import { tmpdir } from "node:os";
import { join } from "node:path";

const [htmlPath, numberPath, domainPath, firefox] = process.argv.slice(2);
assert.ok(firefox, "Expected run view, number input, parameter domain and Firefox paths");
const editor = await readFile(new URL("../src/Editor.cpp", import.meta.url), "utf8");
const bridge = editor.match(/constexpr auto juceHostBridgeScript = R"JS\(([\s\S]*?)\)JS";/)?.[1];
assert.ok(bridge, "Could not locate the editor's injected host bridge");

// Use the shipped view and bridge. Only the native transport is replaced.
const transport = `
window.__parameterCommands = [];
window.__JUCE__ = { backend: {
  emitEvent: (_name, value) => window.__parameterCommands.push(value),
  addEventListener: (_name, receive) => { window.__receiveParameters = receive; },
} };
`;
const checks = `
(async () => {
  try {
    // An active element in an inactive window receives no focus/blur events.
    // Wait for the native window before exercising the shipped edit handlers.
    if (!document.hasFocus()) {
      await new Promise(resolve => window.addEventListener("focus", resolve, { once: true }));
    }
    const equal = (actual, expected, message) => {
      if (actual !== expected) throw new Error(message + ": " + actual + ", expected " + expected);
    };
    const parameter = {
      name: "Slot 1 -> test", label: "test", type: "f32", scalar: true,
      hostValueDomain: "normalized", normalizedValue: 1, value: 440, default: 440,
      rangeMin: 0, rangeMax: 1, scale: "linear", curve: null, unit: "",
      step: null, stepCount: null,
    };
    const send = (param, bank = "params") => window.__receiveParameters({
      type: "state", state: { path: "defaults.onda", running: true, connected: true,
        [bank]: [{ ...param }] },
    });
    const number = (bank = "params") => document.querySelector("#" +
      (bank === "params" ? "params" : "input-controls") + " .param-number");
    const focus = (bank = "params") => {
      const input = number(bank);
      input.focus();
      equal(document.activeElement, input, "Number input must have focus");
      equal(input.readOnly, false, "Focus must enable numeric editing");
    };
    const writes = () => window.__parameterCommands.filter(command =>
      command.type === "setParam" || command.type === "setInputControl");
    for (const layout of ["sliders", "knobs"]) {
      window.__receiveParameters({ type: "state", state: { paramLayout: layout } });
      send(parameter);
      equal(number().value, "440", "Initial authored default");
      send(parameter);
      equal(number().value, "440", "Host publication must preserve an out-of-range default");
      const before = writes().length;
      focus();
      number().blur();
      equal(number().value, "440", "Unedited blur must preserve an authored default");
      equal(writes().length, before, "Unedited blur must not write to DSP");
      for (const value of ["9007199254740993", "-9007199254740993",
                           "9223372036854775807", "-9223372036854775808"]) {
        const integer = { ...parameter, type: "i64", value, default: value,
          normalizedValue: value.startsWith("-") ? 0 : 1, step: 1, stepCount: 1 };
        send(integer);
        equal(number().value, value, "Initial exact i64 default");
        send(integer);
        equal(number().value, value, "Published exact i64 default");
        focus();
        equal(number().value, value, "Focused exact i64 default");
        number().blur();
        equal(number().value, value, "Unedited exact i64 default");
      }
      send(parameter);
      focus();
      number().value = "0.75";
      number().dispatchEvent(new Event("input", { bubbles: true }));
      // Host updates must retain the focused draft.
      send({ ...parameter, value: 0.25, normalizedValue: 0.25 });
      equal(number().value, "0.75", "Host update replaced a focused draft");
      number().dispatchEvent(new Event("change", { bubbles: true }));
      number().blur();
      equal(writes().length, before + 1, "Editing must emit one parameter write");
      equal(writes().at(-1)?.value, 0.75, "Edits must retain normalized conversion");
      send(parameter);
      focus();
      number().value = "999";
      number().dispatchEvent(new Event("input", { bubbles: true }));
      number().dispatchEvent(new Event("change", { bubbles: true }));
      number().blur();
      equal(number().value, "1", "Edits must respect the fallback range");
      equal(writes().at(-1)?.value, 1, "Out-of-range edit must be clamped");
    }
    const control = { ...parameter, name: "Input 1 -> cv", value: 0.1234567,
      default: 0.1234567, normalizedValue: 0.1234567, enabled: true };
    send(control, "inputControls");
    send(control, "inputControls");
    equal(number("inputControls").value, "0.1235", "Control display formatting changed");
    const beforeControl = writes().length;
    focus("inputControls");
    number("inputControls").blur();
    equal(writes().length, beforeControl, "Unedited input control must not write to DSP");
    await fetch("/result", { method: "POST", body: JSON.stringify({ ok: true }) });
  } catch (error) {
    await fetch("/result", { method: "POST", body: JSON.stringify({ error: String(error), stack: error.stack }) });
  }
})();
`;
const html = (await readFile(htmlPath, "utf8"))
  .replace("<head>", `<head><script>${transport}${bridge}</script>`)
  .replace("</body>", `<script>${checks}</script></body>`);
const routes = new Map([
  ["/", ["text/html", html]],
  ["/number-input.js", ["text/javascript", await readFile(numberPath, "utf8")]],
  ["/param-control.js", ["text/javascript", await readFile(domainPath, "utf8")]],
]);
let report;
const completed = new Promise(resolve => { report = resolve; });
const server = createServer(async (request, response) => {
  if (request.url === "/result") {
    let body = "";
    for await (const chunk of request) body += chunk;
    response.end();
    report(JSON.parse(body));
    return;
  }
  const route = routes.get(request.url);
  response.writeHead(route ? 200 : 404, { "Content-Type": route?.[0] ?? "text/plain" });
  response.end(route?.[1] ?? "");
});
const profile = await mkdtemp(join(tmpdir(), "onda-parameter-view-"));
let browser;
let deadline;
try {
  // The same focus preference used by geckodriver, scoped to this profile.
  await writeFile(join(profile, "user.js"), 'user_pref("focusmanager.testmode", true);\n');
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  browser = spawn(firefox, ["--headless", "--no-remote", "--profile", profile,
    `http://127.0.0.1:${server.address().port}/`], { stdio: "ignore" });
  const result = await Promise.race([
    completed,
    new Promise((_, reject) => {
      deadline = setTimeout(() => reject(new Error("Parameter view test timed out")), 30000);
      browser.once("error", reject);
      browser.once("exit", code => reject(new Error(`Firefox exited before reporting (${code})`)));
    }),
  ]);
  assert.equal(result.error, undefined, result.stack ?? result.error);
  assert.equal(result.ok, true);
  console.log("Run view exact defaults, publication, focus and editing passed");
} finally {
  clearTimeout(deadline);
  if (browser && browser.exitCode === null && browser.signalCode === null) {
    const exited = once(browser, "exit");
    browser.kill();
    await exited;
  }
  await new Promise(resolve => server.close(resolve));
  await rm(profile, { recursive: true, force: true });
}
