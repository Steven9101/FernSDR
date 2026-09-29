import assert from 'node:assert/strict';
import { mkdirSync } from 'node:fs';
import { chromium, firefox, webkit } from 'playwright';

const url = process.argv[2] ?? 'http://127.0.0.1:8073/';
const out = process.argv[3] ?? '/tmp/fernsdr-compatibility';
mkdirSync(out, { recursive: true });

for (const [name, browserType, fallback] of [
  ['chromium', chromium, false], ['firefox', firefox, false],
  ['webkit', webkit, false], ['fallback', chromium, true],
]) {
  if (process.argv[4] && process.argv[4] !== name) continue;
  const browser = await browserType.launch();
  try {
    const page = await browser.newPage({ viewport: { width: 1280, height: 720 } });
    const errors = [];
    page.on('pageerror', (error) => errors.push(error.message));
    await page.addInitScript((forceFallback) => {
      localStorage.setItem('fernsdr.preferences.v1', JSON.stringify({ theme: 'dark' }));
      const NativeContext = window.AudioContext;
      window.AudioContext = class extends NativeContext {
        constructor(...args) { super(...args); window.__audioContext = this; }
      };
      if (forceFallback) {
        Object.defineProperty(NativeContext.prototype, 'audioWorklet', { get: () => undefined });
        const getContext = HTMLCanvasElement.prototype.getContext;
        HTMLCanvasElement.prototype.getContext = function (kind, ...args) {
          return kind === 'webgl2' ? null : getContext.call(this, kind, ...args);
        };
      }
    }, fallback);
    await page.goto(url);
    await page.locator('.audio-gate__card').click();
    await page.waitForFunction(() => window.__audioContext?.state === 'running' && window.__fernsdrWaterfall);
    await page.waitForTimeout(4000);
    assert.equal(await page.locator('.audio-gate').count(), 0);
    const tab = page.getByRole('tab', { name: 'Receive', exact: true });
    await tab.focus();
    await page.keyboard.press('ArrowRight');
    assert.equal(await page.getByRole('tab', { name: 'Display', exact: true }).getAttribute('aria-selected'), 'true');
    const before = await page.locator('.spectrum').getAttribute('data-view-high');
    await page.locator('.spectrum').focus();
    await page.keyboard.press('x');
    await page.waitForTimeout(400);
    assert.notEqual(await page.locator('.spectrum').getAttribute('data-view-high'), before);
    const controls = await page.evaluate(() => {
      const frequency = document.querySelector('.frequency').getBoundingClientRect();
      return {
        height: document.querySelector('.morph').getBoundingClientRect().height,
        tunerVisible: !!document.elementFromPoint(frequency.x + 30, frequency.y + 20)?.closest('.tuner'),
      };
    });
    assert(controls.height > 300, `${name}: controls collapsed after changing tabs`);
    assert(controls.tunerVisible, `${name}: focused spectrum covers tuning controls`);
    await page.getByRole('switch', { name: 'Automatic', exact: true }).click();
    await page.waitForFunction(before => document.querySelector('.morph').getBoundingClientRect().height > before + 40,
      controls.height, { timeout: 3000 });
    const expanded = await page.locator('.morph').evaluate(el => el.getBoundingClientRect().height);
    assert(expanded > controls.height + 40, `${name}: new controls were clipped after the panel grew: ${JSON.stringify({ before: controls.height, expanded, checked: await page.getByRole('switch', { name: 'Automatic', exact: true }).getAttribute('aria-checked'), inner: await page.locator('.morph__view').evaluate(el => el.getBoundingClientRect().height) })}`);
    await page.getByRole('switch', { name: 'Automatic', exact: true }).click();
    await page.waitForTimeout(400);
    if (name === 'chromium') {
      await page.evaluate(() => document.querySelector('.spectrum__waterfall').getContext('webgl2')
        .getExtension('WEBGL_lose_context').loseContext());
      await page.waitForFunction(() => document.querySelector('.spectrum').dataset.renderer === '2d');
      await page.waitForTimeout(2000);
    }
    if (fallback || name === 'chromium') {
      const pixels = await page.locator('.spectrum__waterfall').evaluate((canvas) => {
        const rgba = canvas.getContext('2d').getImageData(0, 0, canvas.width, canvas.height).data;
        let bright = 0;
        for (let i = 0; i < rgba.length; i += 4) if (rgba[i] + rgba[i + 1] + rgba[i + 2] > 80) bright++;
        return bright;
      });
      assert(pixels > 5, `${name}: no live waterfall below the ruler`);
    }
    await page.screenshot({ path: `${out}/${name}.png` });
    assert.deepEqual(errors, []);
    console.log(`${name}: audio, live waterfall, keyboard controls passed`);
  } finally { await browser.close(); }
}
