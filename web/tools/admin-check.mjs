/**
 * End-to-end check of the admin panel against a running receiver.
 *
 * Signs in, then opens every page at the widths operators use (a small phone,
 * a large phone, a tablet, a laptop and a desktop monitor) in both themes, and
 * saves a screenshot of each. It fails on what a screenshot review keeps missing:
 *
 *   - a page that asks the receiver for the same thing over and over. A Svelte
 *     effect that reads the state it assigns runs again on every answer, and
 *     nothing looks wrong until the receiver's per-connection limit answers
 *     429 and the page stops updating;
 *   - console errors, uncaught exceptions and failed requests;
 *   - a page wider than the phone it is on;
 *   - settings stretched across a wide screen, where a row's label and its
 *     value end up a monitor's width apart;
 *   - content left underneath the phone's bottom navigation;
 *   - controls without a name a screen reader can say.
 *
 * The password comes from the environment, not the command line, where it
 * would be in the shell history and visible in `ps`.
 *
 *   FERNSDR_ADMIN_PASSWORD=... node tools/admin-check.mjs
 *       [--url http://127.0.0.1:8073/admin.html] [--out /tmp/admin-shots]
 *       [--idle 6000] [--themes dark,light] [--pages overview,bands]
 *       [--carrier 7.0992]   a carrier on the first band, in MHz, for the
 *                            calibration flow; the test source has one there
 */
import { chromium } from 'playwright';
import { mkdirSync } from 'node:fs';

const options = Object.fromEntries(
  process.argv.slice(2).reduce((pairs, argument, index, all) => {
    if (argument.startsWith('--')) pairs.push([argument.slice(2), all[index + 1]]);
    return pairs;
  }, []),
);

const URL_ = options.url ?? 'http://127.0.0.1:8073/admin.html';
const OUT = options.out ?? '/tmp/admin-shots';
const IDLE = Number(options.idle ?? 6000);
const THEMES = (options.themes ?? 'dark,light').split(',');
const PASSWORD = process.env.FERNSDR_ADMIN_PASSWORD;
if (!PASSWORD) {
  console.error('Set FERNSDR_ADMIN_PASSWORD to the admin password.');
  process.exit(2);
}

// The widest a group of settings may be drawn. Rows put the label on the left
// and the value on the right; much wider and the eye loses the line between.
const SETTINGS_MAX_PX = 720;
// How often any one endpoint may be asked, per second, while nobody touches
// the page. The live waterfall asks twice a second; a loop asks as fast as the
// receiver answers.
const MAX_PER_SECOND = 3;

const VIEWPORTS = [
  { name: 'phone-small', width: 360, height: 740, phone: true },
  { name: 'phone', width: 390, height: 844, phone: true },
  { name: 'tablet', width: 820, height: 1180, phone: true },
  { name: 'laptop', width: 1280, height: 800 },
  { name: 'desktop', width: 1920, height: 1080 },
];

mkdirSync(OUT, { recursive: true });
const problems = [];
const browser = await chromium.launch({ args: ['--no-sandbox'] });

function watch(page, where) {
  page.on('console', (message) => {
    if (message.type() === 'error') problems.push(`[${where()}] console: ${message.text()}`);
  });
  page.on('pageerror', (error) => problems.push(`[${where()}] uncaught: ${error.message}`));
  page.on('requestfailed', (request) =>
    problems.push(`[${where()}] request failed: ${request.url()} ${request.failure()?.errorText ?? ''}`),
  );
  page.on('response', (response) => {
    if (response.status() === 429) {
      problems.push(`[${where()}] 429 for ${response.request().method()} ${new URL(response.url()).pathname}`);
    }
  });
}

async function open(page, route) {
  await page.evaluate((hash) => {
    location.hash = hash;
  }, `#/${route}`);
  // The router swaps pages after the hash changes, inside a view transition; until then the old
  // page is still there to be measured by mistake.
  await page.waitForSelector(`main[data-route="${route}"]`, { timeout: 10000 });
}

// A page is measured once it has its content, not after a fixed pause: on a slow receiver a pause
// measures the loading placeholders and passes without having looked at anything.
async function settled(page) {
  await page
    .waitForFunction(() => !document.querySelector('main [aria-busy="true"], main .shimmer'), null, { timeout: 10000 })
    .catch(() => problems.push(`[${page.url().split('#')[1] ?? 'page'}] still loading after 10 s`));
}

// Signs in the way an operator does, in every context: the key that signs changes lives in the
// tab's sessionStorage, which a copied cookie does not bring along, and a panel without it shows
// the sign-in page, which would pass every layout check without a single real page measured.
async function signIn(page) {
  await page.goto(URL_, { waitUntil: 'networkidle' });
  await page.getByLabel('Admin password').fill(PASSWORD);
  await page.getByRole('button', { name: 'Sign in' }).click();
  await page.waitForFunction(() => !document.querySelector('input[type="password"]'), null, { timeout: 15000 });
}

const first = await (await browser.newContext({ viewport: { width: 1280, height: 800 } })).newPage();
watch(first, () => 'sign-in');
await signIn(first);
const state = await first.evaluate(async () => (await fetch('/api/admin/state')).json());
await first.context().close();

const PAGES = options.pages
  ? options.pages.split(',')
  : [
      'overview',
      'bands',
      ...state.bands.map((band) => `bands/${band.id}`),
      'modules',
      'log',
      'station',
      'appearance',
      'widgets',
      'config',
    ];

// Requests from opening each page until it has sat idle a while, on a desktop.
// Opening a page may ask for several things at once, which is not a loop; a
// loop runs until the receiver refuses, so the count starts at the click.
{
  const context = await browser.newContext({ viewport: { width: 1440, height: 900 } });
  const page = await context.newPage();
  let route = 'idle';
  watch(page, () => `idle ${route}`);
  const requests = [];
  page.on('request', (request) => {
    const url = new URL(request.url());
    if (url.pathname.startsWith('/api/')) requests.push({ path: url.pathname, at: Date.now() });
  });
  await signIn(page);
  for (route of PAGES) {
    requests.length = 0;
    const from = Date.now();
    await open(page, route);
    await page.waitForTimeout(1500 + IDLE);
    const seconds = (Date.now() - from) / 1000;
    const counts = new Map();
    for (const request of requests) counts.set(request.path, (counts.get(request.path) ?? 0) + 1);
    const summary = [...counts].map(([path, count]) => `${path} ${count}`).join(', ');
    console.log(`idle ${route.padEnd(18)} ${summary || 'no requests'}`);
    for (const [path, count] of counts) {
      if (count > 5 + MAX_PER_SECOND * seconds) {
        problems.push(`[${route}] asked for ${path} ${count} times in ${seconds.toFixed(1)} s`);
      }
    }
  }
  await context.close();
}

for (const theme of THEMES) {
  for (const viewport of VIEWPORTS) {
    const context = await browser.newContext({
      viewport: { width: viewport.width, height: viewport.height },
      colorScheme: theme,
      hasTouch: viewport.phone,
      isMobile: viewport.phone,
    });
    await context.addInitScript((value) => localStorage.setItem('fernsdr.admin.appearance', value), theme);
    const page = await context.newPage();
    const label = `${theme} ${viewport.name}`;
    let route = '';
    watch(page, () => `${label} ${route}`);
    await signIn(page);
    for (route of PAGES) {
      await open(page, route);
      await settled(page);
      // Let the entrance transition finish before measuring geometry.
      await page.waitForTimeout(600);
      const found = await page.evaluate(
        ({ settingsMax }) => {
          const out = [];
          if (document.querySelector('input[type="password"]') || !document.querySelector('main')) {
            return ['not signed in: this measured the sign-in page, not the panel'];
          }
          const visible = (element) => {
            const box = element.getBoundingClientRect();
            const style = getComputedStyle(element);
            return box.width > 0 && box.height > 0 && style.visibility !== 'hidden' && style.display !== 'none';
          };
          const wide = document.documentElement.scrollWidth;
          if (wide > window.innerWidth + 1) out.push(`the page is ${wide}px wide in a ${window.innerWidth}px window`);

          for (const group of document.querySelectorAll('[data-settings-group]')) {
            const width = group.getBoundingClientRect().width;
            const title = group.querySelector('h2')?.textContent?.trim() ?? 'untitled';
            if (width > settingsMax + 0.5) out.push(`settings "${title}" are ${Math.round(width)}px wide`);
          }

          for (const element of document.querySelectorAll(
            'button, a[href], input:not([type="hidden"]), textarea, select, [role="switch"], [role="slider"], [role="radio"], [role="tab"]',
          )) {
            if (!visible(element)) continue;
            const labelledBy = element.getAttribute('aria-labelledby');
            const name =
              element.getAttribute('aria-label') ||
              (labelledBy && labelledBy.split(' ').map((id) => document.getElementById(id)?.textContent ?? '').join('').trim()) ||
              element.textContent?.trim() ||
              element.getAttribute('title') ||
              element.getAttribute('placeholder') ||
              (element.id && document.querySelector(`label[for="${CSS.escape(element.id)}"]`)?.textContent?.trim()) ||
              element.closest('label')?.textContent?.trim();
            if (!name) out.push(`a ${element.tagName.toLowerCase()} has no name: ${element.outerHTML.slice(0, 120)}`);
          }

          const nav = document.querySelector('[data-bottom-nav]');
          const main = document.querySelector('main');
          if (nav && main && visible(nav)) {
            window.scrollTo(0, document.documentElement.scrollHeight);
            const cover = nav.getBoundingClientRect().top;
            let bottom = 0;
            for (const element of main.querySelectorAll('*')) {
              if (!visible(element) || element.closest('[data-floating]')) continue;
              const position = getComputedStyle(element).position;
              if (position === 'fixed' || position === 'sticky') continue;
              // What a scrolling or clipping ancestor cuts off is not on the page.
              let seen = element.getBoundingClientRect().bottom;
              for (let parent = element.parentElement; parent && parent !== main; parent = parent.parentElement) {
                if (getComputedStyle(parent).overflowY !== 'visible') seen = Math.min(seen, parent.getBoundingClientRect().bottom);
              }
              bottom = Math.max(bottom, seen);
            }
            if (bottom > cover + 0.5) out.push(`content ends ${Math.round(bottom - cover)}px under the bottom navigation`);
            window.scrollTo(0, 0);
          }
          return out;
        },
        { settingsMax: SETTINGS_MAX_PX },
      );
      for (const problem of found) problems.push(`[${label} ${route}] ${problem}`);
      await page.screenshot({ path: `${OUT}/${route.replace(/\//g, '_')}-${theme}-${viewport.name}.png`, fullPage: true });
    }
    await context.close();
  }
}

// What an operator does, at a desktop and a phone width. Every flow discards what it changed,
// so the check can run against a receiver people are listening to.
for (const viewport of [VIEWPORTS.find((v) => v.name === 'desktop'), VIEWPORTS.find((v) => v.name === 'phone')]) {
  const context = await browser.newContext({
    viewport: { width: viewport.width, height: viewport.height },
    hasTouch: viewport.phone,
    isMobile: viewport.phone,
  });
  const page = await context.newPage();
  let flow = '';
  watch(page, () => `flow ${viewport.name} ${flow}`);
  await signIn(page);
  const saveBar = page.locator('[data-save-bar]');
  const check = (condition, message) => {
    if (!condition) problems.push(`[flow ${viewport.name} ${flow}] ${message}`);
  };
  const discard = async () => {
    await saveBar.getByRole('button', { name: 'Discard' }).click();
    await saveBar.waitFor({ state: 'detached', timeout: 3000 }).catch(() => check(false, 'the save bar stayed after Discard'));
  };

  const band = state.bands[0]?.id;
  if (band) {
    flow = 'band setting';
    await open(page, `bands/${band}`);
    await page.getByRole('button', { name: /Audio quality/ }).click();
    const sheet = page.getByRole('dialog');
    await sheet.waitFor({ timeout: 3000 });
    await sheet.locator('ul button:not([aria-current])').first().click();
    const offered = await saveBar.waitFor({ timeout: 3000 }).then(() => true, () => false);
    check(offered, 'choosing a value did not offer to save it');
    if (offered) {
      check((await saveBar.textContent({ timeout: 2000 }))?.includes('audio quality'), 'the save bar does not say what changed');
      await discard();
    }
  }

  if (band) {
    // Measures, never writes: the check must not change the receiver's configuration.
    flow = 'carrier measurement';
    await open(page, `bands/${band}`);
    await page.getByRole('button', { name: 'Measure against a known carrier' }).click();
    const sheet = page.getByRole('dialog');
    const carrier = options.carrier ?? '7.0992';
    await sheet.getByLabel('Its frequency, MHz').fill(carrier);
    await sheet.getByRole('button', { name: 'Measure' }).click();
    const measured = await sheet
      .getByText('Off by', { exact: true })
      .waitFor({ timeout: 10000 })
      .then(() => true, () => false);
    check(measured, `no reading of the carrier at ${carrier} MHz`);
    if (measured) {
      const text = (await sheet.textContent()) ?? '';
      const off = /Off by\s*([+\u2212-]?[\d.]+) Hz/.exec(text);
      check(off !== null && Math.abs(Number(off[1].replace('\u2212', '-'))) < 5, `the carrier reads ${off?.[1] ?? 'nowhere'} Hz off`);
    }
    await page.keyboard.press('Escape');
    await sheet.waitFor({ state: 'detached', timeout: 3000 }).catch(() => check(false, 'the sheet did not close'));
  }

  flow = 'station field';
  await open(page, 'station');
  const operator = page.getByLabel('Operator');
  await operator.waitFor({ timeout: 5000 });
  const before = await operator.inputValue();
  await operator.fill(`${before}X`);
  await saveBar.waitFor({ timeout: 3000 }).catch(() => check(false, 'typing did not offer to save'));
  await discard();
  check((await operator.inputValue()) === before, 'Discard did not put the field back');

  flow = 'appearance preset';
  await open(page, 'appearance');
  await page.locator('label', { hasText: 'Amber CRT' }).click();
  await saveBar.waitFor({ timeout: 3000 }).catch(() => check(false, 'a preset did not offer to apply it'));
  const previewBackground = await page
    .locator('[data-preview]')
    .evaluate((element) => getComputedStyle(element).backgroundColor);
  check(previewBackground === 'rgb(13, 11, 7)', `the preview did not take the preset (${previewBackground})`);
  await discard();

  flow = 'widgets';
  await open(page, 'widgets');
  await page.getByRole('button', { name: /UTC clock/ }).first().click();
  await saveBar.waitFor({ timeout: 3000 }).catch(() => check(false, 'adding a widget did not offer to apply it'));
  await discard();

  flow = 'config completion';
  await open(page, 'config');
  const editor = page.getByLabel('Configuration file');
  await editor.waitFor({ timeout: 5000 });
  await editor.click();
  await page.keyboard.press('Control+End');
  await page.keyboard.type('\n[band:check]\nsam');
  const completions = page.getByRole('listbox', { name: 'Settings that fit here' });
  await completions.waitFor({ timeout: 3000 }).catch(() => check(false, 'no completions while typing a key'));
  check((await completions.getByRole('option').count()) > 0, 'the completion list is empty');
  const box = await completions.boundingBox();
  check(box && box.x >= 0 && box.x + box.width <= viewport.width + 1, 'the completion list runs off the screen');
  await page.keyboard.press('Escape');
  await completions.waitFor({ state: 'detached', timeout: 2000 }).catch(() => check(false, 'Escape did not close the completions'));
  await discard();

  flow = 'log filter';
  await open(page, 'log');
  await settled(page);
  const lines = page.locator('main ol > li');
  const everything = await lines.count();
  const problemsLogged = await lines.filter({ hasText: /Warning|Error/ }).count();
  await page.getByRole('radio', { name: 'Problems' }).click();
  // The filter has worked once the list holds exactly the problems, or says there are none.
  const filtered = await page
    .waitForFunction(
      (expected) => {
        const items = [...document.querySelectorAll('main ol > li')];
        if (expected === 0) return items.length === 0;
        return items.length === expected && items.every((item) => /Warning|Error/.test(item.textContent ?? ''));
      },
      problemsLogged,
      { timeout: 3000 },
    )
    .then(() => true, () => false);
  check(everything > 0, 'the log shows nothing at all');
  check(filtered, `Problems did not narrow ${everything} lines to the ${problemsLogged} warnings and errors`);

  await context.close();
}

await browser.close();
console.log(`\nscreenshots in ${OUT}`);
if (problems.length) {
  console.log(`\n${problems.length} problem(s):`);
  for (const problem of [...new Set(problems)]) console.log(`  ${problem}`);
  process.exit(1);
}
console.log('no problems');
