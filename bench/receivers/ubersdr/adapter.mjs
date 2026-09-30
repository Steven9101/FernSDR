// SPDX-License-Identifier: AGPL-3.0-or-later
// Shipped 0.1.66 v2 UI; tune through visible controls, confirm server status.
export const id = 'ubersdr';
const state = new WeakMap();

export async function open(page, baseUrl) {
  const current = {status: null};
  state.set(page, current);
  page.on('websocket', socket => {
    if (new URL(socket.url()).pathname !== '/ws') return;
    socket.on('framereceived', ({payload}) => {
      if (typeof payload !== 'string') return;
      let message;
      try { message = JSON.parse(payload); } catch { return; }
      if (message.type === 'status') current.status = message;
    });
  });
  await page.goto(baseUrl, {waitUntil: 'domcontentloaded'});
  // In a small window the "Start listening" dialog covers the dial until it
  // is dismissed, so either one means the page is up.
  await page.locator('.dial__digit').first()
    .or(page.getByRole('dialog', {name: 'Start listening'})).first().waitFor({state: 'visible'});
}

export async function startAudio(page) {
  const dialog = page.getByRole('dialog', {name: 'Start listening'});
  if (await dialog.isVisible())
    await dialog.getByRole('button', {name: /Click to start|Tap to start/}).click();
}

export async function tune(page, {freq, mode}) {
  const wireMode = mode === 'cw' ? 'cwu' : mode;
  await page.locator('.dial__digit').first().waitFor({state: 'visible'});
  await page.locator('.dial__digit').first().click();
  const input = page.getByRole('textbox', {name: 'Frequency in kHz'});
  await input.fill(`${freq}hz`);
  await input.press('Enter');
  await page.getByRole('group', {name: 'Mode', exact: true})
    .getByRole('button', {name: wireMode.toUpperCase(), exact: true}).click();
  const current = state.get(page);
  for (let i = 0; i < 100; i++) {
    if (current.status?.frequency === freq && current.status?.mode === wireMode) return;
    await page.waitForTimeout(100);
  }
  throw new Error(`server did not confirm tuning: ${JSON.stringify(current.status)}`);
}

export async function readback(page) {
  const status = state.get(page).status;
  return {freq: status?.frequency ?? null, mode: status?.mode === 'cwu' ? 'cw' : status?.mode,
          serverStatus: status};
}

export function classify(url, frame) {
  if (typeof frame === 'string') return 'control';
  const pathname = new URL(url).pathname;
  if (pathname === '/ws/user-spectrum') return 'waterfall';
  if (pathname === '/ws') return 'audio';
  return 'other';
}
