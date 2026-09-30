import assert from 'node:assert/strict';
import { mkdirSync } from 'node:fs';
import { chromium } from 'playwright';

const base = process.argv[2] ?? 'http://127.0.0.1:18073/';
const out = process.argv[3] ?? '/tmp/fernsdr-controls';
mkdirSync(out, { recursive: true });
const browser = await chromium.launch();
try {
  for (const [width, height] of [[320, 568], [390, 844], [768, 1024], [1024, 768], [1440, 900], [844, 390], [667, 320]]) {
    const page = await browser.newPage({ viewport: { width, height } });
    await page.goto(base);
    await page.locator('.audio-gate__card').waitFor();
    const geometry = await page.evaluate(() => {
      const rect = s => document.querySelector(s).getBoundingClientRect().toJSON();
      return { gate: rect('.audio-gate'), title: rect('.audio-gate__title'),
        action: rect('.audio-gate__action'), tuner: rect('.tuner'), stage: rect('.stage'),
        spectrum: rect('.spectrum'), trace: Number(document.querySelector('.spectrum').dataset.traceHeight),
        overflow: document.documentElement.scrollWidth - innerWidth };
    });
    assert.equal(geometry.overflow, 0);
    assert(geometry.gate.x >= 0 && geometry.gate.right <= width);
    assert(geometry.gate.bottom <= geometry.tuner.top + 1, JSON.stringify(geometry));
    // The prompt to start the audio floats over the waterfall, so that its
    // going on the first tap moves nothing; the spectrum's trace stays in
    // view (a phone on its side too short for one shows none).
    assert(geometry.spectrum.top + geometry.trace <= geometry.gate.top + 1 || geometry.trace === 0,
           'the audio prompt covers the spectrum');
    assert(geometry.title.height < 30 && geometry.action.height < 48, JSON.stringify(geometry));
    await page.screenshot({ path: `${out}/audio-${width}x${height}.png` });
    await page.locator('.audio-gate__card').focus();
    await page.keyboard.press('Enter');
    await page.waitForFunction(() => !document.querySelector('.audio-gate'));
    if (width < 1024) {
      for (const snap of ['half', 'full']) {
        await page.locator('.sheet__grip').focus();
        await page.keyboard.press('Enter');
        await page.waitForFunction(snap => document.querySelector('.sheet').dataset.snap === snap, snap);
        await page.waitForTimeout(300);
        const compact = await page.evaluate(() => {
          const spectrum = document.querySelector('.spectrum').getBoundingClientRect();
          const tuner = document.querySelector('.tuner').getBoundingClientRect();
          return { bottom: spectrum.bottom, tunerTop: tuner.top, height: spectrum.height,
            overflowX: document.documentElement.scrollWidth - innerWidth,
            overflowY: document.documentElement.scrollHeight - innerHeight };
        });
        assert(compact.bottom <= compact.tunerTop + 1 && compact.height > 44, JSON.stringify(compact));
        assert(compact.overflowX <= 1 && compact.overflowY <= 1, JSON.stringify(compact));
      }
      await page.screenshot({ path: `${out}/full-sheet-${width}x${height}.png` });
    }
    await page.close();
  }

  for (const deviceScaleFactor of [1, 2]) for (const mode of ['cw', 'cwl', 'usb', 'lsb']) {
    const page = await browser.newPage({ viewport: { width: 1440, height: 900 }, deviceScaleFactor });
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.addInitScript(() => {
      window.__sent = [];
      const Native = window.WebSocket;
      window.WebSocket = class extends Native {
        send(data) { if (typeof data === 'string') window.__sent.push(JSON.parse(data)); super.send(data); }
      };
    });
    await page.goto(`${base}#band=20m&f=14200000&m=${mode}&pitch=900&view=14195000,14205000`);
    await page.locator('.audio-gate__card').click();
    await page.waitForTimeout(500);
    const signal = () => page.locator('.frequency__digits').getAttribute('aria-label');
    const original = await signal();
    const view = () => page.locator('.spectrum').evaluate(el => [+el.dataset.viewLow, +el.dataset.viewHigh]);
    const bounds = await view();
    if (mode === 'cw' || mode === 'cwl') {
      for (const label of ['250', '500', '100', '500']) {
        await page.getByRole('radiogroup', { name: 'Filter width', exact: true }).getByRole('radio', { name: label, exact: true }).click();
        await page.waitForTimeout(100);
        assert.equal(await signal(), original);
        const last = await page.evaluate(() => window.__sent.filter(m => m.type === 'tune').at(-1));
        const pitch = mode === 'cw' ? 900 : -900;
        assert.equal((last.low + last.high) / 2, pitch);
        assert.equal(last.high - last.low, +label);
      }
    }
    const rect = await page.locator('.spectrum').boundingBox();
    const scale = rect.width / (bounds[1] - bounds[0]);
    const marker = rect.x + (14_200_000 - bounds[0]) * scale;
    // The marker must remain tunable even beside the USB/LSB low cutoff.
    await page.mouse.move(marker, rect.y + 40);
    await page.mouse.down();
    await page.mouse.move(marker + 23, rect.y + 40, { steps: 8 });
    await page.mouse.up();
    await page.waitForTimeout(200);
    assert.deepEqual(await view(), bounds, `${mode}: tuning drag panned the view`);
    const moved = await page.evaluate(() => window.__sent.filter(m => m.type === 'tune').at(-1));
    const pitch = mode === 'cw' ? 900 : mode === 'cwl' ? -900 : 0;
    assert(Math.abs(moved.freq + pitch - (14_200_000 + 23 / scale)) < 2, JSON.stringify(moved));

    const edgeX = rect.x + (moved.freq + moved.high - bounds[0]) * scale;
    await page.mouse.move(edgeX, rect.y + 40);
    await page.mouse.down();
    await page.mouse.move(edgeX - 2, rect.y + 40);
    await page.mouse.up();
    await page.waitForTimeout(200);
    const resized = await page.evaluate(() => window.__sent.filter(m => m.type === 'tune').at(-1));
    assert.equal(resized.freq, moved.freq);
    assert.equal(resized.low, moved.low);
    assert(Math.abs(resized.high - (moved.high - 2 / scale)) < 1, JSON.stringify({ moved, resized }));

    // The visible edge continues through the waterfall. Grabbing that part
    // must resize the same filter, with the same CSS-pixel mapping at each DPR.
    const lowX = rect.x + (resized.freq + resized.low - bounds[0]) * scale;
    const waterY = rect.y + rect.height * 0.7;
    await page.mouse.move(lowX, waterY);
    assert.equal(await page.locator('.spectrum').evaluate(el => el.style.cursor), 'ew-resize');
    await page.mouse.down();
    await page.mouse.move(lowX - 7, waterY, { steps: 4 });
    await page.waitForTimeout(200);
    const below = await page.evaluate(() => window.__sent.filter(m => m.type === 'tune').at(-1));
    assert.equal(below.freq, resized.freq);
    assert(Math.abs(below.high - resized.high) < 0.00001, 'resizing low moved the opposite edge');
    assert(Math.abs(below.low - (resized.low - 7 / scale)) < 1, JSON.stringify({ resized, below }));
    assert.deepEqual(await view(), bounds, 'dragging the lower edge panned the waterfall');
    const painted = await page.locator('.spectrum__overlay').evaluate((canvas, cssX) => {
      const ratio = canvas.width / canvas.getBoundingClientRect().width;
      const start = Math.floor((cssX - 3) * ratio);
      const data = canvas.getContext('2d').getImageData(start, Math.floor(canvas.height * 0.7), Math.ceil(7 * ratio), 1).data;
      let weight = 0, sum = 0;
      for (let i = 0; i < data.length; i += 4) {
        if (data[i + 3] < 100 || data[i] < 200 || data[i + 1] < 180 || data[i + 2] > 200) continue;
        sum += (start + i / 4 + 0.5) * data[i + 3];
        weight += data[i + 3];
      }
      return weight ? sum / weight / ratio : null;
    }, lowX - 7 - rect.x);
    assert(painted !== null && Math.abs(painted - (lowX - 7 - rect.x)) < 1.5,
      `${mode}, DPR ${deviceScaleFactor}: painted edge missed pointer: ${painted}`);
    await page.mouse.up();
    await page.waitForTimeout(60);
    const idleAlpha = await page.locator('.spectrum__overlay').evaluate(canvas => {
      const pixels = canvas.getContext('2d').getImageData(0, Math.floor(canvas.height * 0.7), canvas.width, 1).data;
      let maximum = 0;
      for (let i = 3; i < pixels.length; i += 4) maximum = Math.max(maximum, pixels[i]);
      return maximum;
    });
    assert.equal(idleAlpha, 0, 'idle filter guides obscure the waterfall');
    const beforeDigit = await signal();
    const digit = page.locator('.frequency__digit').last();
    const digitBox = await digit.boundingBox();
    await page.mouse.move(digitBox.x + digitBox.width / 2, digitBox.y + digitBox.height / 2);
    await page.mouse.down();
    await page.mouse.move(digitBox.x + digitBox.width / 2, digitBox.y + digitBox.height / 2 - 18);
    await page.mouse.move(digitBox.x + digitBox.width / 2, digitBox.y + digitBox.height / 2);
    await page.mouse.up();
    await page.waitForTimeout(150);
    assert.equal(await signal(), beforeDigit, 'returning a digit drag added a click');
    assert.deepEqual(errors, []);
    await page.screenshot({ path: `${out}/${mode}-dpr${deviceScaleFactor}.png` });
    await page.close();
  }
  console.log('Audio prompt at seven sizes, expanded mobile controls, CW/CW-L presets, tuning drags, painted full-height edges at DPR 1/2 and digit return passed');
} finally { await browser.close(); }
