import assert from 'node:assert/strict';
import { mkdirSync } from 'node:fs';
import { chromium, devices } from 'playwright';

const base = process.argv[2] ?? 'http://127.0.0.1:18075/';
const out = process.argv[3] ?? '/tmp/fernsdr-bands';
mkdirSync(out, { recursive: true });
const browser = await chromium.launch();
try {
  for (const [name, options] of [
    ['desktop', { viewport: { width: 1440, height: 900 } }],
    ['small-phone', { ...devices['iPhone SE'], viewport: { width: 320, height: 568 } }],
  ]) {
    const context = await browser.newContext(options);
    const page = await context.newPage();
    const errors = [];
    page.on('pageerror', e => errors.push(e.message));
    const link = `${base}#band=b0&f=1009000&m=cw&pitch=900&bw=650,1150&view=998000,1018000`;
    await page.goto(link);
    await page.locator('.audio-gate__card').click();
    const view = () => page.locator('.spectrum').evaluate(el => [+el.dataset.viewLow, +el.dataset.viewHigh]);
    await page.waitForFunction(() => Number(document.querySelector('.spectrum')?.dataset.viewLow) === 998000);
    await page.waitForTimeout(700);
    assert.equal(await page.locator('.frequency__digits').getAttribute('aria-label'), 'Tuned to 1.009000 megahertz');
    if (name === 'small-phone') await page.getByRole('tab', { name: 'Bands', exact: true }).click();
    await page.locator('.band-list').getByRole('button', { name: /^Band 10 / }).click();
    await page.waitForFunction(() => document.querySelector('.frequency__digits')?.getAttribute('aria-label')?.includes('10.'));
    await page.locator('.band-list').getByRole('button', { name: /^Band 1 / }).click();
    await page.waitForTimeout(700);
    assert.deepEqual(await view(), [998000, 1018000]);
    assert.equal(await page.locator('.frequency__digits').getAttribute('aria-label'), 'Tuned to 1.009000 megahertz');
    await page.reload();
    await page.waitForFunction(() => Number(document.querySelector('.spectrum')?.dataset.viewLow) === 998000);
    await page.waitForTimeout(700);
    assert.equal(await page.locator('.frequency__digits').getAttribute('aria-label'), 'Tuned to 1.009000 megahertz');
    if (await page.locator('.audio-gate__card').isVisible()) await page.locator('.audio-gate__card').click();
    if (name === 'small-phone') await page.getByRole('tab', { name: 'Bands', exact: true }).click();
    assert.equal(await page.locator('.band-list__item').count(), 10);
    assert((await page.evaluate(() => document.documentElement.scrollWidth - innerWidth)) <= 1);
    await page.screenshot({ path: `${out}/${name}.png` });
    assert.deepEqual(errors, []);
    console.log(`${name}: ten bands, saved view, CW share link and reload passed`);
    await context.close();
  }
} finally { await browser.close(); }
