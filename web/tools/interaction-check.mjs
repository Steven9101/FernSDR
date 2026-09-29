import assert from 'node:assert/strict';
import { mkdirSync, writeFileSync } from 'node:fs';
import { chromium, devices } from 'playwright';

const url = process.argv[2] ?? 'http://127.0.0.1:18073/';
const out = process.argv[3] ?? '/tmp/websdr-interactions';
mkdirSync(out, { recursive: true });
const browser = await chromium.launch({ args: ['--no-sandbox', '--autoplay-policy=no-user-gesture-required'] });
const report = [];
const errors = [];

async function open(options = {}) {
  const context = await browser.newContext(options);
  await context.addInitScript(() => {
    window.__sent = [];
    window.__audioStats = [];
    window.__delayControl = 0;
    const NativeSocket = window.WebSocket;
    window.WebSocket = class extends NativeSocket {
      constructor(...args) {
        super(...args);
        this.addEventListener('message', (event) => {
          const delay = typeof event.data === 'string' && JSON.parse(event.data)?.type === 'state' ? window.__delayControl : 0;
          if (delay) setTimeout(() => this.handler?.call(this, event), delay);
          else this.handler?.call(this, event);
        });
      }
      set onmessage(handler) { this.handler = handler; }
      get onmessage() { return this.handler; }
      send(data) { if (typeof data === 'string') window.__sent.push({ at: performance.now(), ...JSON.parse(data) }); super.send(data); }
    };
    const NativeNode = window.AudioWorkletNode;
    if (NativeNode) window.AudioWorkletNode = class extends NativeNode {
      constructor(...args) {
        super(...args);
        this.port.addEventListener('message', (event) => {
          if (event.data.type === 'stats') window.__audioStats.push(event.data);
        });
      }
    };
  });
  const page = await context.newPage();
  page.on('pageerror', (error) => errors.push(error.message));
  page.on('console', (message) => { if (message.type() === 'error') errors.push(message.text()); });
  await page.goto(url);
  await page.waitForFunction(() => Number(document.querySelector('.spectrum')?.dataset.viewHigh) > 0);
  await page.locator('.audio-gate .button--primary').click();
  await page.waitForTimeout(1000);
  return { context, page };
}

try {
  const { context, page } = await open({ viewport: { width: 1440, height: 900 } });
  const cdp = await context.newCDPSession(page);
  await cdp.send('Emulation.setCPUThrottlingRate', { rate: 6 });
  const zoom = await page.evaluate(async () => {
    window.__delayControl = 300;
    const el = document.querySelector('.spectrum');
    const rect = el.getBoundingClientRect();
    const read = () => ({ low: Number(el.dataset.viewLow), high: Number(el.dataset.viewHigh) });
    const start = read();
    const clientX = Math.round(rect.left + rect.width * 0.3);
    const fraction = (clientX - rect.left) / rect.width;
    const events = [];
    const sentBefore = window.__sent.length;
    const renderer = window.__fernsdrWaterfall;
    const render = renderer.render.bind(renderer);
    let paints = 0;
    renderer.render = (...args) => { paints++; return render(...args); };
    for (let i = 0; i < 30; i++) {
      const before = performance.now();
      el.dispatchEvent(new WheelEvent('wheel', { deltaY: -8, clientX, bubbles: true, cancelable: true }));
      await new Promise(requestAnimationFrame);
      events.push(performance.now() - before);
    }
    const end = read();
    renderer.render = render;
    const anchor = (v) => v.low + (v.high - v.low) * fraction;
    return { start, end, paints, anchorErrorHz: Math.abs(anchor(start) - anchor(end)), maxFrameMs: Math.max(...events), meanFrameMs: events.reduce((a,b)=>a+b)/events.length,
      commands: window.__sent.slice(sentBefore).filter(m => m.type === 'viewport').length };
  });
  assert(zoom.end.high - zoom.end.low < zoom.start.high - zoom.start.low);
  assert(zoom.anchorErrorHz < 2, JSON.stringify(zoom));
  assert(zoom.commands <= 30);
  assert(zoom.paints >= 29, 'live gestures did not repaint each changed view');
  assert(zoom.meanFrameMs < 60, JSON.stringify(zoom));
  report.push({ scenario: 'zoom, 6x CPU slowdown, 300 ms state replies', ...zoom });
  await page.waitForTimeout(500);
  const settled = await page.locator('.spectrum').evaluate(el => ({ low: Number(el.dataset.viewLow), high: Number(el.dataset.viewHigh) }));
  assert.deepEqual(settled, zoom.end, 'late state replies moved the view');
  const burst = await page.evaluate(async () => {
    const el = document.querySelector('.spectrum');
    const rect = el.getBoundingClientRect();
    const before = window.__sent.length;
    for (let i = 0; i < 200; i++) el.dispatchEvent(new WheelEvent('wheel', { deltaY: -0.25, clientX: rect.left + rect.width / 2, bubbles: true, cancelable: true }));
    await new Promise(requestAnimationFrame);
    await new Promise(requestAnimationFrame);
    return window.__sent.slice(before).filter(m => m.type === 'viewport').length;
  });
  assert(burst <= 2);
  report.push({ scenario: '200 trackpad events before paint', viewportCommands: burst });
  await cdp.send('Emulation.setCPUThrottlingRate', { rate: 1 });
  await page.screenshot({ path: `${out}/desktop.png` });
  await context.close();

  for (const [name, options] of [
    ['laptop', { viewport: { width: 1280, height: 720 } }],
    ['tablet', { viewport: { width: 834, height: 1112 } }],
    ['mobile', devices['iPhone 13']],
    ['small-phone', { ...devices['iPhone SE'], viewport: { width: 320, height: 568 } }],
    ['landscape', devices['iPhone 13 landscape']],
    ['reduced-motion', { viewport: { width: 1024, height: 768 }, reducedMotion: 'reduce' }],
  ]) {
    const { context, page } = await open(options);
    const geometry = await page.evaluate(() => ({
      overflow: document.documentElement.scrollWidth - innerWidth,
      stageHeight: document.querySelector('.stage').getBoundingClientRect().height,
      width: innerWidth, height: innerHeight,
    }));
    assert(geometry.overflow <= 1, `${name} has horizontal overflow: ${JSON.stringify(geometry)}`);
    assert(geometry.stageHeight > 100);
    if (name === 'mobile') {
      const session = await context.newCDPSession(page);
      const rect = await page.locator('.spectrum').boundingBox();
      const y = rect.y + rect.height * 0.65;
      const left = rect.x + rect.width * 0.2;
      const right = rect.x + rect.width * 0.4;
      const read = () => page.locator('.spectrum').evaluate(el => ({ low: +el.dataset.viewLow, high: +el.dataset.viewHigh }));
      const before = await read();
      await session.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x: left, y }, { x: right, y }] });
      await session.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ x: left - 10, y }, { x: right + 30, y }] });
      await page.waitForTimeout(50);
      const after = await read();
      const anchorBefore = before.low + (before.high - before.low) * 0.3;
      const anchorAfter = after.low + (after.high - after.low) * (0.3 + 10 / rect.width);
      assert(Math.abs(anchorBefore - anchorAfter) < 5, `pinch anchor moved: ${anchorBefore - anchorAfter}`);
      await session.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
      report.push({ scenario: 'pinch anchoring', errorHz: Math.abs(anchorBefore - anchorAfter) });
      await page.locator('.sheet__grip').focus();
      await page.keyboard.press('Enter');
      await page.waitForTimeout(300);
      assert.equal(await page.locator('.sheet').getAttribute('data-snap'), 'half');
    }
    await page.screenshot({ path: `${out}/${name}.png` });
    report.push({ scenario: name, ...geometry });
    await context.close();
  }
  assert.deepEqual(errors, []);
  writeFileSync(`${out}/report.json`, JSON.stringify(report, null, 2));
  console.log(JSON.stringify(report, null, 2));
} finally { await browser.close(); }
