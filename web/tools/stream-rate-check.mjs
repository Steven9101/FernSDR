// Listens to a receiver page and records, second by second, how many
// waterfall rows and audio messages reached it, then the player's own buffer
// and dropout count. tools/link-lab.sh runs it over emulated links.
//
//   node tools/stream-rate-check.mjs URL [SECONDS] [REPORT.json]
import assert from 'node:assert/strict';
import { writeFile } from 'node:fs/promises';
import { chromium } from 'playwright';

const url = new URL(process.argv[2] ?? 'http://127.0.0.1:8073/');
const seconds = Number(process.argv[3] ?? 60);
const report = process.argv[4];
// The first ten seconds are left out of the summary, so it needs more.
assert(Number.isInteger(seconds) && seconds > 10);

const browser = await chromium.launch();
try {
  const page = await browser.newPage({ viewport: { width: 1280, height: 900 } });
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  const counts = new Map();
  let started = 0;
  page.on('websocket', socket => socket.on('framereceived', ({ payload }) => {
    if (typeof payload === 'string' || !started) return;
    const second = Math.floor((Date.now() - started) / 1000);
    const row = counts.get(second) ?? { rows: 0, audio: 0, bytes: 0 };
    if (payload[0] === 2) row.rows++;
    if (payload[0] === 1) row.audio++;
    row.bytes += payload.length;
    counts.set(second, row);
  }));
  await page.goto(url.href, { waitUntil: 'load' });
  // The gate starts audio on any gesture; its visible label is aria-hidden.
  await page.locator('.audio-gate__card').first().click({ timeout: 10000 });
  // The Connection tab shows the player's buffer and its dropouts; read them
  // every second, so a report shows when they happened and not only how many.
  await page.getByRole('tab', { name: /Connection/i }).first().click();
  const bufferRow = page.locator('.stats__row', { hasText: 'Buffer' });
  const readBuffer = async () => (await bufferRow.innerText()).replace(/\s+/g, ' ');
  const dropoutsIn = text => Number(text.match(/(\d+) dropouts/)?.[1] ?? 0);
  started = Date.now();
  const dropoutsAt = [];
  for (let second = 0; second < seconds; second++) {
    await page.waitForTimeout(Math.max(0, started + (second + 1) * 1000 - Date.now()));
    dropoutsAt.push(dropoutsIn(await readBuffer()));
  }
  const buffer = await readBuffer();
  const dropouts = dropoutsIn(buffer);

  const series = [];
  for (let second = 0; second < seconds; second++) {
    series.push({ second, ...(counts.get(second) ?? { rows: 0, audio: 0, bytes: 0 }), dropouts: dropoutsAt[second] });
  }
  // The first ten seconds hold the page load and the budget's first probe.
  const settled = series.slice(10);
  const mean = key => settled.reduce((sum, row) => sum + row[key], 0) / settled.length;
  const summary = {
    rowsPerSecond: Number(mean('rows').toFixed(2)),
    audioPerSecond: Number(mean('audio').toFixed(2)),
    kbitPerSecond: Number((mean('bytes') * 8 / 1000).toFixed(1)),
    dropouts,
    buffer,
  };
  console.log(JSON.stringify(summary));
  if (report) await writeFile(report, JSON.stringify({ url: url.href, seconds, summary, series, errors }, null, 1));
  assert.deepEqual(errors, []);
} finally {
  await browser.close();
}
