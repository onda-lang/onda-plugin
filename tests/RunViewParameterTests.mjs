import assert from "node:assert/strict";
import { once } from "node:events";
import { chromium } from "playwright";
import { readFile, mkdir, writeFile } from "node:fs/promises";
import { createServer } from "node:http";

const [htmlPath, numberPath, domainPath] = process.argv.slice(2);
assert.ok(domainPath, "Expected run view, number input and parameter domain paths");
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
      send({ ...parameter, value: 0.25, default: 0.25, normalizedValue: 0.25 });
      document.documentElement.lang = "it-IT";
      focus();
      number().select();
      equal(window.getSelection().toString(), "0.25",
        "Visible decimals must use a dot regardless of the host locale");
      number().blur();
      document.documentElement.lang = "en";
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
    return { ok: true };
  } catch (error) {
    return { error: String(error), stack: error.stack };
  }
})();
`;
const html = (await readFile(htmlPath, "utf8"))
  .replace("<head>", `<head><script>${transport}${bridge}</script>`);
const routes = new Map([
  ["/", ["text/html", html]],
  ["/number-input.js", ["text/javascript", await readFile(numberPath, "utf8")]],
  ["/param-control.js", ["text/javascript", await readFile(domainPath, "utf8")]],
]);
const server = createServer((request, response) => {
  const route = routes.get(request.url);
  response.writeHead(route ? 200 : 404, { "Content-Type": route?.[0] ?? "text/plain" });
  response.end(route?.[1] ?? "");
});
let browser;
let page;
try {
  server.listen(0, "127.0.0.1");
  await once(server, "listening");
  browser = await chromium.launch();
  page = await browser.newPage({ viewport: { width: 480, height: 480 } });
  page.setDefaultTimeout(5000);
  const errors = [];
  page.on("pageerror", error => errors.push(String(error)));
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  await page.bringToFront();
  const result = await page.evaluate(checks);
  assert.equal(result.error, undefined, result.stack ?? result.error);
  assert.equal(result.ok, true);
  const decimal = page.locator("#params .param-number");
  await decimal.focus();
  await decimal.fill("0.625");
  await decimal.press("ArrowUp");
  await decimal.press("Enter");
  assert.equal(await decimal.inputValue(), "0.6251", "Arrow edits must retain numeric step behavior");
  const capabilities = await page.evaluate(() => {
    window.__receiveParameters({ type: "state", state: {
      supportsViewState: true,
      logText: "Native log message\n",
      logEntries: [{ source: { file: "fixture.onda", line: 7 } }],
      delegateOverflowCount: 2,
      midi: { available: true, noteOn: true, noteOff: true },
      midiKeyboardInteractive: false,
    } });
    document.querySelector("#clear-log").click();
    window.__receiveParameters({ type: "midiActivity", activeNotes: [60] });
    return {
      message: document.querySelector(".log-message")?.textContent,
      source: document.querySelector(".log-context")?.textContent,
      loss: document.querySelector("#log-loss")?.textContent,
      cleared: window.__parameterCommands.some(command => command.type === "clearLog"),
      monitored: document.querySelector('[aria-label="MIDI note 60"]')?.classList.contains("active"),
    };
  });
  assert.deepEqual(capabilities, {
    message: "Native log message", source: "fixture.onda:7",
    loss: "delegates: 2 generated overflow · 0 transport drops", cleared: true, monitored: true,
  });
  await page.evaluate(() => {
    window.__receiveParameters({ type: "midiActivity", activeNotes: [] });
    window.__receiveParameters({ type: "state", state: { logText: "", logEntries: [] } });
    const scroll = document.querySelector(".shell");
    scroll.scrollTop = 100;
    scroll.dispatchEvent(new Event("scroll"));
  });
  await page.waitForFunction(() => window.__parameterCommands.some(command =>
    command.type === "viewState" && command.state.scrollY === 100));
  await page.evaluate(() => window.__receiveParameters({ type: "viewState", state: {
    path: "defaults.onda", scrollY: 50, readyId: 42,
  } }));
  await page.waitForFunction(() => window.__parameterCommands.some(command =>
    command.type === "runViewReady" && command.readyId === 42));
  assert.deepEqual(await page.evaluate(() => ({
    scroll: document.querySelector(".shell").scrollTop,
    logLines: document.querySelectorAll(".log-line").length,
    activeNotes: document.querySelectorAll(".piano-key.active").length,
  })), { scroll: 50, logLines: 0, activeNotes: 0 });
  assert.deepEqual(errors, [], "The shipped view raised a browser error");
  for (const width of [360, 480, 720]) {
    await page.setViewportSize({ width, height: 480 });
    const labels = await page.locator("[data-param-layout]").evaluateAll(buttons =>
      buttons.map(button => {
        const box = button.getBoundingClientRect();
        const range = document.createRange();
        range.selectNodeContents(button);
        const text = range.getBoundingClientRect();
        return { label: button.textContent,
          offset: (text.left + text.right - box.left - box.right) / 2 };
      }));
    for (const { label, offset } of labels)
      assert.ok(Math.abs(offset) <= 0.5, `${label} text is off-center by ${offset}px`);
  }
  await page.close();
  page = await browser.newPage();
  page.setDefaultTimeout(5000);
  page.on("pageerror", error => errors.push(String(error)));
  await page.addInitScript(() => {
    // REAPER's macOS webview rejects this selector even though standalone
    // WebKit supports popovers. Replay that feature boundary at the DOM seam.
    const supports = CSS.supports.bind(CSS);
    CSS.supports = (...args) => args[0] === "selector(:popover-open)"
      ? false : supports(...args);
    const matches = Element.prototype.matches;
    Element.prototype.matches = function (selector) {
      if (selector === ":popover-open")
        throw new DOMException("Unsupported :popover-open selector", "SyntaxError");
      return matches.call(this, selector);
    };
    HTMLElement.prototype.showPopover = undefined;
    HTMLElement.prototype.hidePopover = undefined;
    document.addEventListener("DOMContentLoaded", () => {
      // Without popover support the attribute does not hide the element.
      document.querySelectorAll("[popover]").forEach(node => node.removeAttribute("popover"));
    });
  });
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  await page.evaluate(() => window.__receiveParameters({ type: "state", state: {
    supportsSourceSelection: true, supportsViewState: true,
    status: "No Onda file loaded", viewState: { reset: true, readyId: 1 },
  } }));
  await page.waitForFunction(() => window.__parameterCommands.some(command =>
    command.type === "runViewReady" && command.readyId === 1));
  assert.equal(await page.locator("#choose-onda-file").isVisible(), true,
    "The fresh editor must become visible without popover support");
  assert.equal(await page.locator("#settings-popover").isVisible(), false,
    "The unavailable device panel must not leave an empty floating capsule");
  await page.setViewportSize({ width: 720, height: 800 });
  await page.evaluate(() => {
    document.querySelector(".shell").dispatchEvent(new Event("scroll"));
    window.__receiveParameters({ type: "state", state: {
      path: "defaults.onda", params: [{ name: "gain", label: "Gain", type: "f32", scalar: true,
        hostValueDomain: "normalized", normalizedValue: 0.5, value: 0.5, default: 0.5,
        rangeMin: 0, rangeMax: 1, scale: "linear", curve: null, unit: "",
        step: null, stepCount: null }],
    } });
  });
  assert.equal(await page.locator("#params .param-number").isVisible(), true,
    "Loaded controls must render without popover support");
  assert.equal(await page.locator("#settings-popover").isVisible(), false,
    "Loaded editors must not show an empty floating device panel");
  assert.deepEqual(errors, [], "Unsupported popovers must not interrupt the editor");
  console.log("Run view defaults, editing, log, MIDI monitor and scroll restoration passed");
} catch (error) {
  try {
    if (page) {
      await mkdir("test-artifacts", { recursive: true });
      await page.screenshot({ path: "test-artifacts/run-view.png", fullPage: true });
      await writeFile("test-artifacts/run-view.html", await page.content());
      await writeFile("test-artifacts/run-view.json", JSON.stringify(await page.evaluate(() => ({
        commands: window.__parameterCommands,
        scrollTop: document.querySelector(".shell")?.scrollTop,
        scrollHeight: document.querySelector(".shell")?.scrollHeight,
        clientHeight: document.querySelector(".shell")?.clientHeight,
      })), null, 2));
    }
  } catch (diagnosticError) {
    console.error("Could not collect browser diagnostics:", diagnosticError);
  }
  throw error;
} finally {
  await browser?.close();
  await new Promise(resolve => server.close(resolve));
}
