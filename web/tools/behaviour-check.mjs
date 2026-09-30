/**
 * Listener page behaviour the other checks do not reach: tooltips, the
 * keyboard help, sharing, banners, theme, the receive and display controls,
 * typed tuning, the history and station tabs, reconnecting, preferences
 * across a reload, the 1024 px layout switch, the phone sheet, the audio
 * gate's failure states and a lost WebGL context.
 *
 * Needs a receiver with a site notice, operator, location and antenna, two
 * bands with `history = public` on the first, and a theme with a chat among
 * several widgets; docs/TESTING.md has the configuration. Writes what it
 * saw and every command the page sent to OUT/report.json, so two builds can
 * be compared message for message.
 *
 * usage: node tools/behaviour-check.mjs [URL] [OUT]
 */
import assert from 'node:assert/strict';
import { mkdirSync, writeFileSync } from 'node:fs';
import { chromium } from 'playwright';

const base = process.argv[2] ?? 'http://127.0.0.1:18099/';
const out = process.argv[3] ?? '/tmp/fernsdr-behaviour';
mkdirSync(out, { recursive: true });

const browser = await chromium.launch({ args: ['--autoplay-policy=no-user-gesture-required'] });
const report = {};
const errors = [];

async function open(name, options = {}, init) {
  const context = await browser.newContext({
    viewport: { width: 1280, height: 800 },
    colorScheme: 'dark',
    ...options,
  });
  await context.addInitScript(() => {
    window.__sent = [];
    const NativeSocket = window.WebSocket;
    window.WebSocket = class extends NativeSocket {
      send(data) {
        if (typeof data === 'string') window.__sent.push(JSON.parse(data));
        super.send(data);
      }
    };
    // Without the Web Share API the page copies the link and says so, which
    // is the path with something to check.
    Object.defineProperty(Navigator.prototype, 'share', { value: undefined, configurable: true });
  });
  if (init) await context.addInitScript(init);
  const page = await context.newPage();
  page.on('pageerror', (error) => errors.push(`${name}: ${error.message}`));
  // The socket is routed so a test can speak for the receiver and drop the
  // connection. Everything else passes through unchanged.
  const socket = { page: null, theme: null };
  await page.routeWebSocket(/\/ws$/, (ws) => {
    const server = ws.connectToServer();
    server.onMessage((message) => {
      if (typeof message === 'string' && socket.theme === null) {
        const parsed = JSON.parse(message);
        if (parsed.type === 'welcome') socket.theme = parsed.theme;
      }
      ws.send(message);
    });
    socket.page = ws;
  });
  const entry = { seen: {}, sent: [] };
  report[name] = entry;
  return { context, page, socket, seen: entry.seen, entry };
}

/** Everything sent since the last call, without the revision counters. */
async function takeSent(page, entry) {
  const messages = await page.evaluate(() => window.__sent.splice(0));
  for (const message of messages) {
    delete message.request_id;
    entry.sent.push(message);
  }
  return messages;
}

async function connected(page) {
  await page.waitForFunction(() => document.querySelector('.status__state')?.textContent?.startsWith('Connected'));
}

async function blur(page) {
  await page.evaluate(() => (document.activeElement instanceof HTMLElement) && document.activeElement.blur());
}

function inject(socket, message) {
  socket.page.send(JSON.stringify(message));
}

const bannerGeometry = (page) => page.evaluate(() => {
  const node = document.querySelector('.banner');
  const variable = getComputedStyle(document.documentElement).getPropertyValue('--banner-height').trim();
  if (!node) return { variable };
  return {
    text: node.textContent,
    role: node.getAttribute('role'),
    error: node.classList.contains('banner--error'),
    height: `${node.getBoundingClientRect().height}px`,
    variable,
  };
});

async function desktop() {
  const { context, page, socket, seen, entry } = await open('desktop', {
    permissions: ['clipboard-read', 'clipboard-write'],
  });
  await page.goto(base);
  await page.locator('.audio-gate__card').waitFor();
  await connected(page);

  seen.identity = await page.evaluate(() => ({
    name: document.querySelector('.topbar__name')?.textContent,
    operator: document.querySelector('.topbar__operator')?.textContent,
    site: document.querySelector('.status__site')?.textContent,
    antennaTitle: document.querySelector('.status__antenna')?.getAttribute('title'),
    tabs: [...document.querySelectorAll('[role="tab"]')].map((tab) => tab.textContent),
  }));
  assert.equal(seen.identity.operator, 'Test operator');
  assert.deepEqual(seen.identity.tabs, ['Receive', 'Display', 'Stream', 'History', 'Station']);

  seen.notice = await bannerGeometry(page);
  assert.equal(seen.notice.text, 'Maintenance tonight at 22 UTC');
  assert.equal(seen.notice.variable, seen.notice.height);

  // Tooltips: a delay for the first, none while the group is warm, and the
  // delay again once it has cooled.
  const keys = page.getByRole('button', { name: 'Keyboard shortcuts' });
  const share = page.getByRole('button', { name: 'Copy a link to this frequency' });
  const tooltip = page.locator('[role="tooltip"]');
  const idle = page.locator('.topbar__name');
  await idle.hover();
  await keys.hover();
  await page.waitForTimeout(150);
  seen.tooltipBeforeDelay = await tooltip.count();
  assert.equal(seen.tooltipBeforeDelay, 0);
  await tooltip.waitFor({ timeout: 2000 });
  seen.tooltipCold = { text: await tooltip.textContent(), cold: await tooltip.getAttribute('data-cold') };
  assert.deepEqual(seen.tooltipCold, { text: 'Keyboard shortcuts', cold: 'true' });
  await share.hover();
  await page.waitForTimeout(80);
  seen.tooltipWarm = { count: await tooltip.count(), text: await tooltip.first().textContent(), cold: await tooltip.first().getAttribute('data-cold') };
  assert.deepEqual(seen.tooltipWarm, { count: 1, text: 'Copy a link that opens on this frequency', cold: 'false' });
  await idle.hover();
  await page.waitForTimeout(450);
  await keys.hover();
  await page.waitForTimeout(150);
  seen.tooltipCooled = await tooltip.count();
  assert.equal(seen.tooltipCooled, 0);
  await idle.hover();
  await tooltip.waitFor({ state: 'detached' });
  await keys.focus();
  await tooltip.waitFor({ timeout: 300 });
  seen.tooltipOnFocus = await tooltip.textContent();
  await blur(page);
  await tooltip.waitFor({ state: 'detached' });

  // Sharing: the link goes to the clipboard and the button says so for a
  // moment. The click is also the first gesture, so audio starts.
  await share.click();
  await page.waitForFunction(() => document.querySelector('.topbar__actions svg[class*="lucide-check"]'));
  const copied = new URL(await page.evaluate(() => navigator.clipboard.readText()));
  seen.sharedLink = Object.fromEntries(new URLSearchParams(copied.hash.slice(1)));
  assert.deepEqual(Object.keys(seen.sharedLink), ['band', 'f', 'm', 'bw', 'view']);
  assert.equal(seen.sharedLink.band, '20m');
  await page.waitForFunction(() => document.querySelector('[role="tooltip"]')?.textContent === 'Link copied');
  await page.waitForFunction(() => document.querySelector('.topbar__actions svg[class*="lucide-link"]'), null, { timeout: 4000 });
  seen.copiedReverted = true;
  await page.locator('.audio-gate').waitFor({ state: 'detached' });
  await idle.hover();

  // Keyboard help: ?, Escape, the backdrop, the close button and a click
  // inside the card, which must not close it.
  const dialog = page.getByRole('dialog', { name: 'Keyboard shortcuts' });
  await blur(page);
  await page.keyboard.press('?');
  await dialog.waitFor();
  seen.help = { rows: await dialog.locator('.shortcuts__row').count(), keys: await dialog.locator('kbd').allTextContents() };
  assert.equal(seen.help.rows, 11);
  await page.keyboard.press('Escape');
  await dialog.waitFor({ state: 'detached' });
  await page.keyboard.press('?');
  await dialog.waitFor();
  await page.mouse.click(8, 8);
  await dialog.waitFor({ state: 'detached' });
  await page.keyboard.press('?');
  await dialog.waitFor();
  await dialog.locator('.modal__title').click();
  await page.waitForTimeout(150);
  seen.helpAfterCardClick = await dialog.count();
  assert.equal(seen.helpAfterCardClick, 1);
  await dialog.getByRole('button', { name: 'Close' }).click();
  await dialog.waitFor({ state: 'detached' });
  await keys.click();
  await dialog.waitFor();
  // Focus is on the button that opened it, and the page's shortcuts leave
  // keys aimed at buttons alone. Recorded, not asserted.
  await page.keyboard.press('Escape');
  await page.waitForTimeout(150);
  seen.helpAfterEscapeOnButton = await dialog.count();
  await dialog.getByRole('button', { name: 'Close' }).click();
  await dialog.waitFor({ state: 'detached' });
  await takeSent(page, entry);

  // Theme: explicit choices lock it, Auto follows the system as it changes.
  await page.locator('#control-tab-display').click();
  const scheme = () => page.evaluate(() => ({
    scheme: document.documentElement.dataset.colorScheme,
    locked: document.documentElement.getAttribute('data-theme-locked'),
  }));
  await page.getByRole('radio', { name: 'Light', exact: true }).click();
  seen.themeLight = await scheme();
  assert.deepEqual(seen.themeLight, { scheme: 'light', locked: 'true' });
  await page.getByRole('radio', { name: 'Auto', exact: true }).click();
  seen.themeAuto = await scheme();
  assert.deepEqual(seen.themeAuto, { scheme: 'dark', locked: null });
  await page.emulateMedia({ colorScheme: 'light' });
  await page.waitForFunction(() => document.documentElement.dataset.colorScheme === 'light');
  await page.emulateMedia({ colorScheme: 'dark' });
  await page.waitForFunction(() => document.documentElement.dataset.colorScheme === 'dark');

  // Display controls.
  const palette = page.locator('button.palette', { hasText: 'Aurora' });
  await palette.click();
  seen.palette = await page.locator('button.palette[aria-pressed="true"]').allTextContents();
  const automatic = page.getByRole('switch', { name: 'Automatic' });
  await automatic.click();
  seen.manualLevels = {
    checked: await automatic.getAttribute('aria-checked'),
    sliders: await page.locator('.rack', { hasText: 'Levels' }).locator('input[type="range"]').count(),
  };
  assert.deepEqual(seen.manualLevels, { checked: 'false', sliders: 2 });
  await page.getByLabel('Floor', { exact: true }).focus();
  await page.keyboard.press('Home');
  seen.floor = await page.locator('.field', { hasText: 'Floor' }).locator('.field__value').textContent();
  assert.equal(seen.floor, '\u2212150 dBFS');
  await automatic.click();
  await page.getByRole('switch', { name: 'Band plan' }).click();
  await page.getByRole('switch', { name: 'Band plan' }).click();

  // Sound shaping, de-emphasis among it, is shown with the Full controls.
  await page.getByRole('radio', { name: 'Full', exact: true }).click();

  // Receive controls.
  await page.locator('#control-tab-receive').click();
  await page.getByRole('radio', { name: 'LSB', exact: true }).click();
  await page.waitForFunction(() => document.querySelector('[role="radiogroup"][aria-label="Filter width"] [aria-checked="true"]'));
  seen.lsbFilter = await page.locator('[role="radiogroup"][aria-label="Filter width"] [aria-checked="true"]').textContent();
  await page.getByRole('radio', { name: 'Off', exact: true }).first().click();
  const gain = page.getByLabel('Gain', { exact: true });
  await gain.focus();
  await page.keyboard.press('End');
  seen.gain = await page.locator('.field', { has: gain }).locator('.field__value').textContent();
  assert.equal(seen.gain, '60 dB');
  await page.getByRole('radio', { name: 'Slow', exact: true }).click();
  await page.getByRole('switch', { name: 'Auto notch' }).click();
  seen.autonotch = await page.getByRole('switch', { name: 'Auto notch' }).getAttribute('aria-checked');
  assert.equal(seen.autonotch, 'true');
  const squelch = page.getByLabel('Squelch', { exact: true });
  await squelch.focus();
  await page.keyboard.press('End');
  seen.squelchClosed = await page.locator('.field', { has: squelch }).locator('.field__value').textContent();
  await page.keyboard.press('Home');
  seen.squelchOpen = await page.locator('.field', { has: squelch }).locator('.field__value').textContent();
  assert.deepEqual([seen.squelchClosed, seen.squelchOpen], ['\u221220 dBFS', 'open']);
  const reduction = page.getByLabel('Reduction', { exact: true });
  await reduction.focus();
  for (let i = 0; i < 5; i++) await page.keyboard.press('ArrowRight');
  seen.reduction = await page.locator('.field', { has: reduction }).locator('.field__value').textContent();
  assert.equal(seen.reduction, '5%');
  await page.getByRole('radio', { name: 'NFM', exact: true }).click();
  seen.deemphasis = await page.getByRole('radiogroup', { name: 'De-emphasis' }).count();
  assert.equal(seen.deemphasis, 1);
  await page.getByRole('radio', { name: 'LSB', exact: true }).click();
  await page.waitForTimeout(100);
  const dspSent = (await takeSent(page, entry)).filter((message) => message.type === 'dsp');
  seen.lastDsp = dspSent.at(-1);
  assert.equal(seen.lastDsp.squelch, -200);
  assert.equal(seen.lastDsp.nr, 0.05);

  // Typed tuning, and Escape.
  const readout = page.locator('.frequency__digits');
  await page.getByRole('button', { name: 'Type a frequency' }).click();
  const entryField = page.getByLabel('Frequency', { exact: true });
  seen.typedDraft = { value: await entryField.inputValue(), focused: await entryField.evaluate((node) => node === document.activeElement) };
  await entryField.fill('14.1');
  await entryField.press('Enter');
  await page.waitForFunction(() => document.querySelector('.frequency__digits')?.getAttribute('aria-label') === 'Tuned to 14.100000 megahertz');
  await page.getByRole('button', { name: 'Type a frequency' }).click();
  await page.getByLabel('Frequency', { exact: true }).fill('7.5');
  await page.getByLabel('Frequency', { exact: true }).press('Escape');
  await readout.waitFor();
  await page.waitForTimeout(100);
  // Chrome blurs the field as it is removed, and the blur commits the draft.
  // Recorded, not asserted.
  seen.afterEscape = await readout.getAttribute('aria-label');
  await takeSent(page, entry);

  // History.
  const historyRequests = [];
  page.on('request', (request) => {
    const url = new URL(request.url());
    if (url.pathname === '/api/history') historyRequests.push(Number(url.searchParams.get('to')) - Number(url.searchParams.get('from')));
  });
  const historyRequest = () => page.waitForRequest((request) => new URL(request.url()).pathname === '/api/history');
  const settled = () => page.waitForFunction(() => document.querySelector('.history')?.getAttribute('aria-busy') === 'false');
  let requested = historyRequest();
  await page.locator('#control-tab-history').click();
  await requested;
  await settled();
  seen.history = await page.evaluate(() => ({
    spans: [...document.querySelectorAll('.history__span')].map((button) => `${button.textContent}:${button.getAttribute('aria-pressed')}`),
    notes: [...document.querySelectorAll('.history__note')].map((note) => note.textContent),
    plot: document.querySelectorAll('.history__canvas').length,
  }));
  requested = historyRequest();
  await page.getByRole('button', { name: '1 h', exact: true }).click();
  await requested;
  await settled();
  seen.historyPressed = await page.locator('.history__span[aria-pressed="true"]').textContent();
  requested = historyRequest();
  await page.getByRole('button', { name: 'Refresh', exact: true }).click();
  await requested;
  await settled();
  seen.historySpans = historyRequests.map((ms) => Math.round(ms / 60_000));
  assert.deepEqual(seen.historySpans, [15, 60, 60]);

  // Station: five widgets, the chat among them.
  await page.locator('#control-tab-station').click();
  await page.locator('.widgets').waitFor();
  seen.widgets = await page.locator('.widget__head').allTextContents();
  assert.deepEqual(seen.widgets, ['Chat', 'Time', 'About', 'Links', 'Bands']);
  seen.clock = await page.locator('.clock__time').textContent();
  assert.match(seen.clock, /^\d\d:\d\d:\d\d$/);
  seen.links = await page.locator('.widget__links a').evaluateAll((links) => links.map((link) => `${link.textContent}|${link.target}|${link.rel}`));
  seen.bars = await page.locator('.widget__bar-label').allTextContents();
  const name = page.getByLabel('Your name or callsign');
  await name.fill('DL1TEST');
  await name.press('Enter');
  await page.locator('.chat__as').waitFor();
  seen.chatAs = await page.locator('.chat__as').textContent();
  // The receiver keeps its chat across connections, so the message carries
  // a mark of its own.
  const mark = `run ${Date.now().toString(36)}`;
  await page.getByLabel('Message', { exact: true }).fill(`Try 14.074 now, ${mark}`);
  await page.getByRole('button', { name: 'Send', exact: true }).click();
  const line = page.locator('.chat__line', { hasText: mark });
  await line.waitFor();
  seen.chatLine = (await line.evaluate((node) => node.innerHTML.replace(/<!--.*?-->/g, ''))).replace(mark, 'MARK');
  await line.locator('.chat__spot').click();
  await page.waitForFunction(() => document.querySelector('.frequency__digits')?.getAttribute('aria-label') === 'Tuned to 14.074000 megahertz');
  await page.getByRole('button', { name: 'Share where you are listening' }).click();
  seen.sharedDraft = await page.getByLabel('Message', { exact: true }).inputValue();
  await page.locator('.chat__as').click();
  seen.renaming = await name.inputValue();
  await page.getByRole('button', { name: 'Done', exact: true }).click();
  for (let i = 0; i < 30; i++) inject(socket, { type: 'chat', id: 10_000 + i, name: 'N0CALL', text: `line ${i}`, at: Date.now() });
  await page.locator('.chat__line', { hasText: 'line 29' }).waitFor();
  await page.waitForTimeout(100);
  const scroll = () => page.locator('.chat__log').evaluate((log) => Math.round(log.scrollHeight - log.scrollTop - log.clientHeight));
  seen.chatFollows = await scroll();
  assert.ok(seen.chatFollows < 2, `chat did not follow: ${seen.chatFollows}`);
  await page.locator('.chat__log').evaluate((log) => { log.scrollTop = 0; });
  inject(socket, { type: 'chat', id: 20_000, name: 'N0CALL', text: 'one more', at: Date.now() });
  await page.locator('.chat__line', { hasText: 'one more' }).waitFor();
  await page.waitForTimeout(100);
  seen.chatHoldsPosition = await page.locator('.chat__log').evaluate((log) => log.scrollTop);
  assert.equal(seen.chatHoldsPosition, 0);
  inject(socket, { type: 'chat-refused', reason: 'Slow down' });
  seen.refusal = await page.locator('.chat__refusal').textContent();
  await takeSent(page, entry);

  // An error takes the banner for a while.
  inject(socket, { type: 'error', message: 'Test failure' });
  await page.locator('.banner--error').waitFor();
  seen.errorBanner = await bannerGeometry(page);
  assert.equal(seen.errorBanner.variable, seen.errorBanner.height);
  await page.locator('.banner--error').waitFor({ state: 'detached', timeout: 8000 });
  seen.bannerAfterError = await bannerGeometry(page);

  // Meter faces, as an operator switching the theme would send them.
  seen.meters = {};
  for (const face of ['needle', 'numeric', 'history', 'bar']) {
    inject(socket, { type: 'theme', theme: { ...socket.theme, meter: face } });
    await page.locator(`.smeter--${face}`).waitFor();
    await page.waitForTimeout(face === 'history' ? 800 : 100);
    seen.meters[face] = await page.locator('.smeter').evaluate((meter) => ({
      children: [...meter.children].map((child) => child.getAttribute('class')),
      svg: [...meter.querySelectorAll('svg')].map((svg) => svg.getAttribute('class')),
      readout: [...meter.querySelectorAll('.smeter__readout > *')].map((child) => child.getAttribute('class')),
    }));
  }

  // Reconnecting says why, then recovers.
  socket.page.close({ code: 4000, reason: 'test' });
  await page.waitForFunction(() => document.querySelector('.status__state')?.textContent?.startsWith('Reconnecting'));
  seen.reconnecting = await page.locator('.status__state').textContent();
  await connected(page);
  await page.waitForTimeout(300);
  // Keepalive pings go out on a timer, so how many fall in here is chance.
  seen.afterReconnect = (await takeSent(page, entry)).map((message) => message.type).filter((type) => type !== 'ping');

  // Preferences survive a reload; the tuning comes back from the link.
  await page.locator('#control-tab-connection').click();
  await page.getByRole('radio', { name: 'High', exact: true }).click();
  await page.getByLabel('Volume').focus();
  for (let i = 0; i < 10; i++) await page.keyboard.press('ArrowLeft');
  await page.locator('#control-tab-display').click();
  await page.getByRole('radio', { name: 'Light', exact: true }).click();
  await page.waitForTimeout(800);
  seen.stored = await page.evaluate(() => JSON.parse(localStorage.getItem('fernsdr.preferences.v1')));
  seen.hash = new URL(page.url()).hash;
  await takeSent(page, entry);
  await page.reload();
  await connected(page);
  await page.waitForTimeout(500);
  seen.restored = await page.evaluate(() => ({
    volume: document.querySelector('input[aria-label="Volume"]')?.value,
    scheme: document.documentElement.dataset.colorScheme,
    readout: document.querySelector('.frequency__digits')?.getAttribute('aria-label'),
    mode: document.querySelector('[role="radiogroup"][aria-label="Mode"] [aria-checked="true"]')?.textContent ?? null,
  }));
  await page.locator('#control-tab-connection').click();
  seen.restoredProfile = await page.getByRole('radio', { name: 'High', exact: true }).getAttribute('aria-checked');
  await page.locator('#control-tab-display').click();
  seen.restoredPalette = await page.locator('button.palette[aria-pressed="true"]').allTextContents();
  assert.deepEqual([seen.restored.volume, seen.restored.scheme, seen.restoredProfile], ['70', 'light', 'true']);
  await takeSent(page, entry);

  // Across 1024 px the sidebar becomes a sheet with the same controls.
  await page.locator('#control-tab-history').click();
  await page.setViewportSize({ width: 900, height: 800 });
  await page.locator('.sheet').waitFor();
  seen.narrow = await page.evaluate(() => ({
    app: document.querySelector('.app')?.className,
    panels: document.querySelectorAll('#receiver-panel').length,
    tabs: [...document.querySelectorAll('[role="tab"]')].map((tab) => `${tab.textContent}:${tab.getAttribute('aria-selected')}`),
  }));
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.locator('.sidebar').waitFor();
  seen.wide = await page.evaluate(() => ({
    app: document.querySelector('.app')?.className,
    tabs: [...document.querySelectorAll('[role="tab"]')].map((tab) => `${tab.textContent}:${tab.getAttribute('aria-selected')}`),
  }));

  // A lost WebGL context falls back to the 2D renderer and keeps drawing.
  const before = await page.evaluate(() => {
    const canvas = document.querySelector('.spectrum__waterfall');
    canvas.__original = true;
    return document.querySelector('.spectrum')?.dataset.renderer;
  });
  await page.evaluate(() => document.querySelector('.spectrum__waterfall').getContext('webgl2')?.getExtension('WEBGL_lose_context')?.loseContext());
  await page.waitForFunction(() => document.querySelector('.spectrum')?.dataset.renderer === '2d');
  await page.waitForTimeout(1000);
  seen.contextLoss = await page.evaluate((renderer) => ({
    before: renderer,
    after: document.querySelector('.spectrum')?.dataset.renderer,
    canvases: document.querySelectorAll('.spectrum__waterfall').length,
    replaced: !document.querySelector('.spectrum__waterfall').__original,
    filling: document.querySelectorAll('.spectrum__filling').length,
  }), before);
  assert.deepEqual(seen.contextLoss, { before: 'webgl', after: '2d', canvases: 1, replaced: true, filling: seen.contextLoss.filling });
  await takeSent(page, entry);
  await context.close();
}

async function phone() {
  const { context, page, seen, entry } = await open('phone', {
    viewport: { width: 390, height: 844 },
    deviceScaleFactor: 2,
    isMobile: true,
    hasTouch: true,
  });
  await page.goto(base);
  await page.locator('.audio-gate__card').waitFor();
  await connected(page);
  const sheet = page.locator('.sheet');
  const grip = page.locator('.sheet__grip');
  const snap = () => sheet.getAttribute('data-snap');
  seen.tabs = await page.locator('[role="tab"]').allTextContents();
  seen.snaps = [await snap()];
  for (let i = 0; i < 3; i++) {
    await grip.tap();
    await page.waitForTimeout(250);
    seen.snaps.push(await snap());
  }
  assert.deepEqual(seen.snaps, ['peek', 'half', 'full', 'peek']);
  seen.gripLabel = await grip.getAttribute('aria-label');
  await grip.focus();
  seen.keys = [];
  for (const key of ['ArrowUp', 'ArrowUp', 'ArrowDown', 'Enter', ' ']) {
    await page.keyboard.press(key);
    seen.keys.push(await snap());
  }
  assert.deepEqual(seen.keys, ['half', 'full', 'half', 'full', 'peek']);
  seen.volumeAfterKeys = await page.locator('input[aria-label="Volume"]').inputValue();

  // A quick flick goes one step; a slow drag lands on the nearest height.
  // The pointer events carry their own times, so the speed the sheet
  // measures does not depend on how busy the machine is.
  const cdp = await context.newCDPSession(page);
  const box = await grip.boundingBox();
  const x = box.x + box.width / 2;
  const drag = async (steps, stepPx, stepMs, beforeRelease) => {
    const handle = await grip.boundingBox();
    const y = handle.y + handle.height / 2;
    const t0 = Date.now() / 1000;
    const send = (type, dy, index, buttons) => cdp.send('Input.dispatchMouseEvent', {
      type, x, y: y - dy, button: 'left', buttons, clickCount: 1, timestamp: t0 + index * stepMs / 1000,
    });
    await send('mousePressed', 0, 0, 1);
    for (let i = 1; i <= steps; i++) await send('mouseMoved', i * stepPx, i, 1);
    await page.waitForTimeout(100);
    if (beforeRelease) await beforeRelease();
    await send('mouseReleased', steps * stepPx, steps + 1, 0);
    await page.waitForTimeout(300);
  };
  await drag(4, 15, 16);
  seen.flick = await snap();
  await drag(12, 5, 100, async () => {
    seen.dragging = await sheet.getAttribute('data-dragging');
  });
  seen.slowDrag = await snap();
  assert.deepEqual([seen.flick, seen.dragging, seen.slowDrag], ['half', 'true', 'half']);

  // A long press shows a tooltip and the next touch elsewhere dismisses it.
  const tooltip = page.locator('[role="tooltip"]');
  const band = page.locator('.bands__item.is-active');
  const target = await band.boundingBox();
  const point = { x: target.x + target.width / 2, y: target.y + target.height / 2 };
  await cdp.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [point] });
  await page.waitForTimeout(700);
  seen.longPress = { count: await tooltip.count(), text: await tooltip.first().textContent().catch(() => null) };
  assert.equal(seen.longPress.count, 1);
  await cdp.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
  await page.waitForTimeout(200);
  seen.afterLift = await tooltip.count();
  await page.touchscreen.tap(195, 300);
  await page.waitForTimeout(200);
  seen.afterTapElsewhere = await tooltip.count();
  assert.equal(seen.afterTapElsewhere, 0);
  // A tap focuses the button, and focus opens its tooltip. Recorded, not
  // asserted.
  await page.getByRole('button', { name: 'Keyboard shortcuts' }).tap();
  await page.getByRole('dialog', { name: 'Keyboard shortcuts' }).waitFor();
  seen.tapTooltip = await tooltip.count();
  await page.getByRole('button', { name: 'Close' }).tap();
  await takeSent(page, entry);
  await context.close();
}

async function gate(name, init, expected) {
  const { context, page, seen } = await open(name, {}, init);
  await page.goto(base);
  await page.locator('.audio-gate__card').click();
  await page.waitForFunction((title) => document.querySelector('.audio-gate__title')?.textContent === title, expected.title);
  await page.waitForFunction((text) => document.querySelector('.audio-gate__text')?.textContent === text, expected.text);
  seen.gate = await page.evaluate(() => ({
    title: document.querySelector('.audio-gate__title')?.textContent,
    text: document.querySelector('.audio-gate__text')?.textContent,
    action: document.querySelector('.audio-gate__action')?.textContent,
    label: document.querySelector('.audio-gate')?.getAttribute('aria-label'),
  }));
  assert.equal(seen.gate.action, expected.action);
  await context.close();
}

try {
  await desktop();
  await phone();
  await gate('audio-failed', () => {
    window.AudioContext = class { constructor() { throw new Error('no audio device'); } };
  }, { title: 'Audio could not start', text: 'Your browser would not open an audio device. Check that this page may play sound.', action: 'Try again' });
  await gate('audio-suspended', () => {
    const Native = window.AudioContext;
    window.AudioContext = class extends Native {
      resume() { return Promise.resolve(); }
      get state() { return 'suspended'; }
    };
  }, { title: 'Tap to listen', text: 'The browser accepted the tap but did not open the audio device. Check the silent switch and that this tab is not muted.', action: 'Start audio' });
} finally {
  writeFileSync(`${out}/report.json`, JSON.stringify({ report, errors }, null, 1));
  await browser.close();
}
assert.deepEqual(errors, []);
console.log('Tooltips, keyboard help, sharing, banners, theme, controls, typed tuning, history, station, reconnect, preferences, layout switch, sheet, audio gate states and context loss passed');
