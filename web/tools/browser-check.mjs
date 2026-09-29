/**
 * End-to-end check of the built client against a running receiver.
 *
 * Loads the real page in a real browser, drives it, and reports what it found
 * along with screenshots. This is what catches the things unit tests cannot:
 * a shader that silently renders nothing, a layout that squeezes the waterfall
 * off a phone screen, an audio path that never starts.
 *
 *   node tools/browser-check.mjs [--url http://127.0.0.1:8073/] [--settle 15000]
 *                               [--out /tmp/shots]
 */
import { chromium, devices } from 'playwright';
import { mkdirSync } from 'node:fs';

const options = Object.fromEntries(
  process.argv.slice(2).reduce((pairs, argument, index, all) => {
    if (argument.startsWith('--')) pairs.push([argument.slice(2), all[index + 1]]);
    return pairs;
  }, []),
);

const URL = options.url ?? 'http://127.0.0.1:8073/';
const SETTLE = Number(options.settle ?? 15000);
const OUT = options.out ?? '/tmp/shots';
// Playwright finds its own browser; --chrome is for a system Chrome.
const CHROME = options.chrome;

mkdirSync(OUT, { recursive: true });

const problems = [];
const browser = await chromium.launch({
  ...(CHROME ? { executablePath: CHROME } : {}),
  args: ['--autoplay-policy=no-user-gesture-required', '--no-sandbox'],
});

/**
 * Runs one scenario. `theme` is applied through the app's stored preference,
 * because headless Chromium ignores an emulated colour-scheme preference.
 */
async function scenario({ name, context: contextOptions = {}, theme = 'dark', settle = SETTLE, act }) {
  const context = await browser.newContext(contextOptions);
  // On the context rather than the page: a page-level init script does not
  // reliably run before the document's own inline scripts.
  await context.addInitScript((value) => {
    localStorage.setItem('fernsdr.preferences.v1', JSON.stringify({ theme: value }));
  }, theme);

  const page = await context.newPage();
  page.on('console', (message) => {
    if (message.type() === 'error') problems.push(`[${name}] console: ${message.text()}`);
  });
  page.on('pageerror', (error) => problems.push(`[${name}] uncaught: ${error.message}`));
  page.on('requestfailed', (request) =>
    problems.push(`[${name}] request failed: ${request.url()} ${request.failure()?.errorText ?? ''}`),
  );

  await page.goto(URL, { waitUntil: 'networkidle' });
  await page.waitForTimeout(settle);
  if (act) await act(page);
  await page.waitForTimeout(1200);

  const report = await page.evaluate(() => {
    const text = (selector) => document.querySelector(selector)?.textContent?.trim() ?? null;
    const waterfall = document.querySelector('.spectrum__waterfall');
    return {
      connection: text('.status__state'),
      site: text('.topbar__name'),
      frequency: [...document.querySelectorAll('.frequency__digit')].map((d) => d.textContent).join(''),
      bandLabel: text('.frequency__band'),
      filter: text('.field__value'),
      signal: text('.smeter__readout'),
      audioRunning: !document.querySelector('.audio-gate'),
      colorScheme: document.documentElement.dataset.colorScheme,
      waterfallSize: waterfall ? `${waterfall.width}x${waterfall.height}` : null,
      // The share of the viewport the display actually gets. On a phone this
      // is the number that decides whether the receiver is pleasant to use.
      displayShare: (() => {
        const stage = document.querySelector('.stage');
        if (!stage) return null;
        return Math.round((stage.getBoundingClientRect().height / window.innerHeight) * 100) + '%';
      })(),
      fallbackRenderer: !!document.querySelector('.spectrum__badge'),
    };
  });

  await page.screenshot({ path: `${OUT}/${name}.png` });
  console.log(`\n=== ${name} ===`);
  for (const [key, value] of Object.entries(report)) console.log(`  ${key.padEnd(16)} ${value}`);
  await context.close();
  return report;
}

const startAudio = async (page) => {
  await page.evaluate(() => document.querySelector('.audio-gate .button--primary')?.click());
  await page.waitForTimeout(1500);
};

await scenario({ name: 'desktop-dark', context: { viewport: { width: 1440, height: 900 } }, act: startAudio });
await scenario({ name: 'desktop-light', context: { viewport: { width: 1280, height: 800 } }, theme: 'light' });
await scenario({ name: 'mobile', context: devices['iPhone 13'], act: startAudio });
await scenario({
  name: 'mobile-controls',
  context: devices['iPhone 13'],
  act: async (page) => {
    await page.click('.sheet__grip');
    await page.waitForTimeout(500);
  },
});
await scenario({
  name: 'mobile-landscape',
  context: devices['iPhone 13 landscape'],
  settle: 8000,
});

/*
 * Panning must leave history where it was.
 *
 * Every waterfall row carries the frequency span it covered, and the shader
 * asks each row independently where a frequency sat in it. Pan, and the old
 * lines stay locked to frequency while the part of the view they never covered
 * goes blank. Getting this wrong does not throw or log; it just looks subtly
 * wrong, which is why it survived until somebody said so.
 *
 * The specific regression guarded here: the level texture is wider than a
 * line, and the shader used to test its "did this row cover this frequency"
 * bound in texture space rather than in line space. A 1024-bin line in a
 * 2048-column row therefore claimed data across twice the frequency range it
 * held, and panning into that region drew the zero padding as though it were a
 * noise floor - a dark band that looked like signal-free spectrum instead of
 * blank history. It was visible on one side only, because the other side made
 * the coordinate negative and blanked correctly.
 */
{
  const name = 'pan-anchoring';
  const context = await browser.newContext({ viewport: { width: 1100, height: 700 } });
  await context.addInitScript(() =>
    localStorage.setItem('fernsdr.preferences.v1', JSON.stringify({ theme: 'dark' })));
  const page = await context.newPage();
  page.on('pageerror', (error) => problems.push(`[${name}] uncaught: ${error.message}`));
  await page.goto(URL, { waitUntil: 'networkidle' });
  await page.waitForTimeout(1500);

  const box = await (await page.$('.spectrum')).boundingBox();
  // Zoom in, or the view is the whole band and there is nowhere to pan to.
  await page.mouse.move(box.x + box.width * 0.5, box.y + box.height * 0.85);
  for (let i = 0; i < 4; i++) {
    await page.mouse.wheel(0, -120);
    await page.waitForTimeout(150);
  }
  await page.waitForTimeout(24000);   // let history build at this zoom

  const readStrip = async (y, from, to) => {
    const shot = await page.screenshot({ clip: { x: box.x + from, y, width: to - from, height: 1 } });
    const url = 'data:image/png;base64,' + shot.toString('base64');
    return page.evaluate(async (source) => {
      const image = new Image();
      await new Promise((resolve) => { image.onload = resolve; image.src = source; });
      const canvas = document.createElement('canvas');
      canvas.width = image.width;
      canvas.height = 1;
      const context2d = canvas.getContext('2d');
      context2d.drawImage(image, 0, 0);
      const data = context2d.getImageData(0, 0, image.width, 1).data;
      let r = 0, g = 0, b = 0;
      for (let x = 0; x < image.width; x++) { r += data[x * 4]; g += data[x * 4 + 1]; b += data[x * 4 + 2]; }
      return [r, g, b].map((v) => Math.round(v / image.width));
    }, url);
  };

  const before = await page.evaluate(() => {
    const renderer = window.__fernsdrWaterfall;
    return renderer ? renderer.rowMeta(0) : null;
  });
  if (!before || !before.width) problems.push(`[${name}] no waterfall history to test`);

  // Drag LEFT specifically. Dragging right reveals a strip where the shader's
  // coordinate goes negative, which blanks correctly under either version of
  // this code - so a check that pans that way passes even when the bug is
  // present, which is exactly what the first version of this check did.
  const y = box.y + box.height * 0.85;
  const start = box.x + box.width * 0.7;
  await page.mouse.move(start, y);
  await page.mouse.down();
  for (let i = 1; i <= 18; i++) { await page.mouse.move(start - i * 10, y); await page.waitForTimeout(14); }
  await page.mouse.up();
  await page.waitForTimeout(500);

  // The newest row must have moved with the view; an older one must not.
  const after = await page.evaluate(() => {
    const renderer = window.__fernsdrWaterfall;
    return { newest: renderer.rowMeta(0), old: renderer.rowMeta(60) };
  });
  if (before && after.old.lowHz !== before.lowHz) {
    problems.push(`[${name}] an old row's span changed under it: history is not anchored`);
  }
  if (before && after.newest.lowHz === before.lowHz) {
    problems.push(`[${name}] the newest row did not follow the pan`);
  }

  // And the strip the pan revealed must be blank, not padding drawn as data.
  const revealed = await readStrip(box.y + 226, Math.round(box.width) - 130, Math.round(box.width) - 10);
  const EMPTY = [8, 10, 15];
  const off = revealed.map((v, i) => Math.abs(v - EMPTY[i]));
  if (Math.max(...off) > 4) {
    problems.push(
      `[${name}] the strip revealed by panning is rgb(${revealed}) rather than the empty ` +
      `colour rgb(${EMPTY}): history is claiming data it never had`,
    );
  }
  console.log(`\n=== ${name} ===`);
  console.log('  revealed strip  ', `rgb(${revealed})`, Math.max(...off) <= 4 ? 'blank, correct' : 'WRONG');
  console.log('  old row anchored', before && after.old.lowHz === before.lowHz ? 'yes' : 'NO');
  await context.close();
}

await browser.close();

console.log('\n=== problems ===');
console.log(problems.length ? problems.join('\n') : '(none)');
process.exit(problems.length ? 1 : 0);
