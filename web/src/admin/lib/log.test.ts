import { describe, expect, it } from 'vitest';
import { collapseRepeats, parseLogLine, recentProblems } from './log';

const line = (time: string, level: string, where: string, text: string) => `${time}.123 ${level} [${where}] ${text}`;

describe('parseLogLine', () => {
  it('splits a server line into its parts', () => {
    const entry = parseLogLine(line('04:11:44', 'WRN', 'band', 'rtl is offline'));
    expect(entry).toMatchObject({ time: '04:11:44', level: 'warn', where: 'band', text: 'rtl is offline', count: 1 });
  });

  it('keeps a line it cannot parse exactly as it came', () => {
    const entry = parseLogLine('something else entirely');
    expect(entry).toMatchObject({ level: 'plain', text: 'something else entirely', time: '' });
  });

  it('keeps text that spans lines', () => {
    expect(parseLogLine(line('04:11:44', 'ERR', 'dsp', 'first\nsecond')).text).toBe('first\nsecond');
  });
});

describe('collapseRepeats', () => {
  it('merges neighbours with the same message and remembers when the run began', () => {
    const entries = [
      line('10:00:00', 'WRN', 'band', 'offline'),
      line('10:00:30', 'WRN', 'band', 'offline'),
      line('10:01:00', 'WRN', 'band', 'offline'),
      line('10:01:05', 'INF', 'band', 'receiving'),
    ].map(parseLogLine);
    const collapsed = collapseRepeats(entries);
    expect(collapsed).toHaveLength(2);
    expect(collapsed[0]).toMatchObject({ count: 3, since: '10:00:00', time: '10:01:00' });
    expect(collapsed[1]).toMatchObject({ count: 1, text: 'receiving' });
  });

  it('does not merge the same message across something else', () => {
    const entries = [
      line('10:00:00', 'WRN', 'band', 'offline'),
      line('10:00:10', 'INF', 'admin', 'signed in'),
      line('10:00:30', 'WRN', 'band', 'offline'),
    ].map(parseLogLine);
    expect(collapseRepeats(entries)).toHaveLength(3);
  });
});

describe('recentProblems', () => {
  it('lists each warning or error once, latest first, with how often it came', () => {
    const entries = [
      line('10:00:00', 'WRN', 'band', 'offline'),
      line('10:00:05', 'ERR', 'dsp', 'overrun'),
      line('10:00:30', 'WRN', 'band', 'offline'),
      line('10:00:40', 'INF', 'band', 'receiving'),
    ].map(parseLogLine);
    const problems = recentProblems(entries, 5);
    expect(problems.map((entry) => entry.text)).toEqual(['offline', 'overrun']);
    expect(problems[0]).toMatchObject({ count: 2, time: '10:00:30', since: '10:00:00' });
  });

  it('keeps only as many as asked for', () => {
    const entries = ['a', 'b', 'c'].map((text, index) => parseLogLine(line(`10:00:0${index}`, 'WRN', 'x', text)));
    expect(recentProblems(entries, 2).map((entry) => entry.text)).toEqual(['c', 'b']);
  });
});
