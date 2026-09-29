/**
 * A long listen, with the network taken away and given back.
 *
 * The complaint this exists to settle: after a reconnect the latency climbs to
 * about 300 ms and stays there. That is the failure the jitter policy is
 * written to avoid - each small underrun nudges the target up and nothing ever
 * brings it down - and unit tests cover the policy in isolation. They cannot
 * cover the case where the whole audio path is torn down and rebuilt, which is
 * what a reconnect is.
 *
 * So this runs the real client, in a real browser, against a real receiver,
 * for as long as you ask, and cuts the network at intervals. It records what
 * the listener sees: the buffered figure in the status bar, the target behind
 * it, and the dropout count.
 *
 * The number that matters is not the peak. A reconnect is allowed to cost
 * latency - the buffer has to refill. What is not allowed is for that cost to
 * be permanent, so the report compares each segment's settled latency against
 * the first segment's, and says plainly whether it came back.
 *
 *   node tools/soak.mjs [--url http://127.0.0.1:8073/] [--minutes 30]
 *                       [--cycle 240] [--outage 8] [--out /tmp/soak]
 */
import { chromium } from 'playwright';
import { mkdirSync, writeFileSync } from 'node:fs';

const options = Object.fromEntries(
  process.argv.slice(2).reduce((pairs, argument, index, all) => {
    if (argument.startsWith('--')) pairs.push([argument.slice(2), all[index + 1]]);
    return pairs;
  }, []),
);

const URL = options.url ?? 'http://127.0.0.1:8073/';
const MINUTES = Number(options.minutes ?? 30);
/** Seconds of listening between outages. */
const CYCLE = Number(options.cycle ?? 240);
/** Seconds the network stays away. Long enough that the socket really dies. */
const OUTAGE = Number(options.outage ?? 8);
const SAMPLE_MS = Number(options.sample ?? 2000);
const OUT = options.out ?? '/tmp/soak';
// Playwright finds its own browser; --chrome is for a system Chrome.
const CHROME = options.chrome;
/** How long after a reconnect the buffer is allowed to be refilling. */
const RECOVERY_S = Number(options.recovery ?? 60);

mkdirSync(OUT, { recursive: true });

const browser = await chromium.launch({
  ...(CHROME ? { executablePath: CHROME } : {}),
  args: ['--autoplay-policy=no-user-gesture-required', '--no-sandbox'],
});
const context = await browser.newContext({ viewport: { width: 1280, height: 800 } });
await context.addInitScript(() =>
  localStorage.setItem('fernsdr.preferences.v1', JSON.stringify({ theme: 'dark' })));

const page = await context.newPage();
const problems = [];
page.on('console', (message) => {
  if (message.type() === 'error') problems.push(`console: ${message.text()}`);
});
page.on('pageerror', (error) => problems.push(`uncaught: ${error.message}`));

await page.goto(URL, { waitUntil: 'networkidle' });
await page.evaluate(() => document.querySelector('.audio-gate .button--primary')?.click());
await page.waitForTimeout(3000);

// The target and the dropout count live in the Connection panel, so it stays
// open for the whole run. Everything else is in the status bar.
await page.evaluate(() => {
  const tab = [...document.querySelectorAll('button')].find(
    (b) => b.textContent?.trim() === 'Connection');
  tab?.click();
});
await page.waitForTimeout(500);

const read = () =>
  page.evaluate(() => {
    const number = (text) => {
      const found = /(-?\d+(?:\.\d+)?)/.exec(text ?? '');
      return found ? Number(found[1]) : null;
    };
    const stats = [...document.querySelectorAll('.status__stat')].map((e) => e.textContent ?? '');
    const buffered = stats.find((text) => text.includes('ms'));
    const panel = document.querySelector('.stats')?.textContent ?? '';
    const target = /target (\d+) ms/.exec(panel);
    const dropouts = /(\d+) dropouts/.exec(panel);
    return {
      state: document.querySelector('.status__state')?.textContent?.trim() ?? '',
      latency: number(buffered),
      target: target ? Number(target[1]) : null,
      dropouts: dropouts ? Number(dropouts[1]) : 0,
      audioRunning: !document.querySelector('.audio-gate'),
    };
  });

const rows = [];
const started = Date.now();
const endAt = started + MINUTES * 60_000;
let segment = 0;
let nextOutage = started + CYCLE * 1000;
let lastReconnectAt = started;

console.log(`soaking for ${MINUTES} min, an outage every ${CYCLE} s`);

while (Date.now() < endAt) {
  const sample = await read();
  const at = (Date.now() - started) / 1000;
  rows.push({ at, segment, sinceReconnect: (Date.now() - lastReconnectAt) / 1000, ...sample });
  if (rows.length % 15 === 0) {
    console.log(
      `  ${at.toFixed(0).padStart(5)}s  segment ${segment}  ${sample.state.padEnd(12)}` +
        `  ${String(sample.latency ?? '-').padStart(4)} ms` +
        `  target ${String(sample.target ?? '-').padStart(4)}  ${sample.dropouts} dropouts`,
    );
  }

  if (Date.now() >= nextOutage && Date.now() + (OUTAGE + 30) * 1000 < endAt) {
    segment++;
    console.log(`  --- taking the network away for ${OUTAGE} s (segment ${segment}) ---`);
    await context.setOffline(true);
    await page.waitForTimeout(OUTAGE * 1000);
    await context.setOffline(false);
    lastReconnectAt = Date.now();
    nextOutage = Date.now() + CYCLE * 1000;
  }

  await page.waitForTimeout(SAMPLE_MS);
}

await context.close();
await browser.close();

const csv = ['at_s,segment,since_reconnect_s,state,latency_ms,target_ms,dropouts']
  .concat(rows.map((r) =>
    [r.at.toFixed(1), r.segment, r.sinceReconnect.toFixed(1), r.state, r.latency ?? '', r.target ?? '', r.dropouts].join(',')))
  .join('\n');
writeFileSync(`${OUT}/soak.csv`, csv);

const median = (values) => {
  if (values.length === 0) return null;
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.floor(sorted.length / 2)];
};

// Settled means: this segment, past the recovery window, still connected.
const settled = (which) =>
  rows
    .filter((r) => r.segment === which && r.sinceReconnect > RECOVERY_S && (r.state === 'Live' || r.state === 'Connected') && r.latency !== null)
    .map((r) => r.latency);

console.log(`\n=== ${rows.length} samples over ${((Date.now() - started) / 60000).toFixed(1)} min ===`);
const baseline = median(settled(0));
console.log(`  segment 0 settled median  ${baseline} ms`);
let worst = 0;
for (let index = 1; index <= segment; index++) {
  const after = median(settled(index));
  if (after === null) {
    console.log(`  segment ${index}: no settled samples (too short, or never reconnected)`);
    continue;
  }
  const drift = after - baseline;
  worst = Math.max(worst, drift);
  console.log(`  segment ${index} settled median  ${after} ms  (${drift >= 0 ? '+' : ''}${drift} vs baseline)`);
}
const peak = Math.max(...rows.map((r) => r.latency ?? 0));
const dropouts = Math.max(...rows.map((r) => r.dropouts));
console.log(`  peak latency at any moment ${peak} ms, ${dropouts} dropouts reported`);
console.log(`  worst permanent drift ${worst} ms`);
for (const problem of [...new Set(problems)].slice(0, 10)) console.log(`  ! ${problem}`);
console.log(`  wrote ${OUT}/soak.csv`);
