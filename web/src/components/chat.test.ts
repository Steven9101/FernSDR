import { describe, expect, it, vi } from 'vitest';
import { render } from 'svelte/server';

vi.mock('../audio/player', () => ({ AudioPlayer: class { reset() {} } }));
vi.mock('../state/theme', () => ({ applyTheme() {} }));
vi.mock('../net/client', () => ({
  defaultWebSocketUrl: () => 'ws://test',
  SdrClient: class { send() {} setUrl() {} connect() {} close() {} },
}));

import Chat from './Chat.svelte';
import { chatLog } from '../state/store';

// A chat that rendered markup from a message would let any listener run script
// in every other listener's page. That happened in UberSDR before 0.1.58
// (CVE-2026-97723): a URL in a message became a link, and a quotation mark
// in it closed the href and added an attribute that ran script, with no
// click. Names and messages are text here, URLs stay text, and a frequency
// in a message becomes a button with the message's own characters as its
// label.
describe('chat', () => {
  it('shows markup in a name or a message as text', () => {
    chatLog.value = [{
      id: 1,
      name: '<img src=x onerror=alert(1)>',
      text: '<script>alert(1)</script> CQ on 7.074 <b>now</b>',
      at: 0,
    }];
    const { body } = render(Chat, { props: { height: 240 } });
    expect(body).not.toContain('<img src=x');
    expect(body).not.toContain('<script>alert');
    expect(body).not.toContain('<b>now');
    expect(body).toContain('&lt;img src=x onerror=alert(1)>');
    expect(body).toContain('&lt;script>alert(1)&lt;/script>');
    expect(body).toContain('&lt;b>now&lt;/b>');
  });

  it('keeps a URL with a quotation mark in it as text, not a link', () => {
    chatLog.value = [{
      id: 2,
      name: 'op',
      text: 'see https://example.org/" onmouseover="alert(1)" x="',
      at: 0,
    }];
    const { body } = render(Chat, { props: { height: 240 } });
    expect(body).not.toContain('<a');
    // In the text, where it belongs, and in no tag.
    expect(body).not.toMatch(/<[^>]*onmouseover/);
    expect(body).toContain('https://example.org/" onmouseover="alert(1)" x="');
  });
});
