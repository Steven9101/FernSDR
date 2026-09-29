import { afterEach, describe, expect, it, vi } from 'vitest';
import { copyText } from './clipboard';

function fakeDocument(copyWorks: boolean) {
  const appended: { value: string; removed: boolean }[] = [];
  const execCommand = vi.fn(() => copyWorks);
  vi.stubGlobal('document', {
    createElement: () => {
      const area = { value: '', removed: false, style: {}, setAttribute() {}, select() {}, remove() { area.removed = true; } };
      return area;
    },
    body: { appendChild: (area: { value: string; removed: boolean }) => appended.push(area) },
    execCommand,
  });
  return { appended, execCommand };
}

afterEach(() => vi.unstubAllGlobals());

describe('copyText', () => {
  it('uses the Clipboard API where the page is secure', async () => {
    const writeText = vi.fn(async () => {});
    vi.stubGlobal('isSecureContext', true);
    vi.stubGlobal('navigator', { clipboard: { writeText } });
    const { execCommand } = fakeDocument(true);
    expect(await copyText('https://example.org/#f=7074000')).toBe(true);
    expect(writeText).toHaveBeenCalledWith('https://example.org/#f=7074000');
    expect(execCommand).not.toHaveBeenCalled();
  });

  it('copies over plain HTTP, where there is no Clipboard API', async () => {
    vi.stubGlobal('isSecureContext', false);
    vi.stubGlobal('navigator', {});
    const { appended, execCommand } = fakeDocument(true);
    expect(await copyText('http://203.0.113.5:8073/#f=7074000')).toBe(true);
    expect(execCommand).toHaveBeenCalledWith('copy');
    expect(appended[0].value).toBe('http://203.0.113.5:8073/#f=7074000');
    expect(appended[0].removed).toBe(true);
  });

  it('falls back when the Clipboard API refuses, and says when nothing worked', async () => {
    vi.stubGlobal('isSecureContext', true);
    vi.stubGlobal('navigator', { clipboard: { writeText: vi.fn(async () => { throw new Error('denied'); }) } });
    fakeDocument(false);
    expect(await copyText('x')).toBe(false);
  });
});
