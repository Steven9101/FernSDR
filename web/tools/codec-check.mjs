import assert from 'node:assert/strict';
import { chromium } from 'playwright';

const base = process.argv[2] ?? 'http://127.0.0.1:18073/';
const browser = await chromium.launch();
try {
  for (const fallback of [false, true]) for (const deviceScaleFactor of [1, 2]) {
    const page = await browser.newPage({ viewport: { width: 1000, height: 700 }, deviceScaleFactor });
    const errors = [];
    const packets = { nac3: 0, native: 0, adaptive: 0, fine: 0, coarse: 0 };
    page.on('pageerror', error => errors.push(error.message));
    page.on('websocket', socket => socket.on('framereceived', ({ payload }) => {
      if (typeof payload === 'string') return;
      if (payload[0] === 1 && (payload[1] & 8)) packets.nac3++;
      if (payload[0] === 2 && (payload[1] & 4)) packets.native++;
      if (payload[0] === 2 && (payload[1] & 2)) packets.adaptive++;
      if (payload[0] === 2) packets[payload[1] & 8 ? 'coarse' : 'fine']++;
    }));
    await page.addInitScript(forceFallback => {
      if (!forceFallback) return;
      const getContext = HTMLCanvasElement.prototype.getContext;
      HTMLCanvasElement.prototype.getContext = function (kind, ...args) {
        return kind === 'webgl2' ? null : getContext.call(this, kind, ...args);
      };
    }, fallback);
    await page.goto(`${base}#band=20m&f=14199200&m=usb&view=14199000,14201000`);
    await page.locator('.audio-gate__card').click();
    await page.waitForFunction(() => window.__fernsdrWaterfall);
    await page.waitForTimeout(2500);
    assert(packets.nac3 > 20 && packets.native > 3 && packets.coarse > 3, JSON.stringify(packets));
    await page.locator('#control-tab-connection').click();
    await page.getByRole('radio', { name: 'High', exact: true }).click();
    const fineBefore = packets.fine;
    await page.waitForTimeout(1500);
    assert(packets.fine > fineBefore + 3, JSON.stringify(packets));
    await page.getByRole('radio', { name: 'Balanced', exact: true }).click();
    const coarseBefore = packets.coarse;
    await page.waitForTimeout(1500);
    assert(packets.coarse > coarseBefore + 3, JSON.stringify(packets));
    const comparison = await page.evaluate(fallback => {
      const renderer = window.__fernsdrWaterfall;
      const canvas = document.querySelector('.spectrum__waterfall');
      const gl = fallback ? null : canvas.getContext('webgl2');
      renderer.setReference(0);
      renderer.setLevels(-200, 100);
      renderer.setPalette('mono');
      const read = () => {
        if (!gl) return canvas.getContext('2d').getImageData(0, 0, canvas.width, 1).data;
        const bytes = new Uint8Array(canvas.width * 4);
        gl.readPixels(0, canvas.height - 1, canvas.width, 1, gl.RGBA, gl.UNSIGNED_BYTE, bytes);
        if (gl.getError()) throw new Error('WebGL codec rendering failed');
        return bytes;
      };
      let worst = 0;
      for (const [a, b] of [[-200, -40], [-160, 0], [-120, 100]]) {
        for (const [low, high] of [[0, 20], [9, 11], [0, 1], [19, 20]]) {
          const levels = Float32Array.of(a, b);
          renderer.clear();
          renderer.pushLine({ lowHz: 0, highHz: 20, width: 2, levels });
          renderer.render(low, high);
          const native = read();
          const dense = Float32Array.from({ length: 2048 }, (_, i) => {
            const position = Math.max(0, Math.min(1, (i + 0.5) * 2 / 2048 - 0.5));
            return a * (1 - position) + b * position;
          });
          renderer.clear();
          renderer.pushLine({ lowHz: 0, highHz: 20, width: dense.length, levels: dense });
          renderer.render(low, high);
          const reference = read();
          for (let i = 0; i < native.length; i++) worst = Math.max(worst, Math.abs(native[i] - reference[i]));
        }
      }
      return { worst };
    }, fallback);
    assert(comparison.worst <= 3, `native interpolation disagrees with dense reference: ${JSON.stringify(comparison)}`);
    assert.deepEqual(errors, []);
    console.log(JSON.stringify({ renderer: fallback ? 'canvas' : 'webgl', deviceScaleFactor, packets, ...comparison }));
    await page.close();
  }
} finally {
  await browser.close();
}
