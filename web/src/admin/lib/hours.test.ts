import { describe, expect, it } from 'vitest';
import { clockRange, complement, describeHours, hoursNow, usesSun } from './hours';

describe('band hours in the panel', () => {
  it('names the common hours', () => {
    expect(describeHours(undefined)).toBe('Always');
    expect(describeHours('always')).toBe('Always');
    expect(describeHours('sunrise-sunset')).toBe('In daylight at the station');
    expect(describeHours('sunset-sunrise')).toBe('At night at the station');
    expect(describeHours('06:00-18:00')).toBe('06:00 to 18:00 UTC');
    expect(describeHours('sunset-1h-sunrise')).toBe('sunset-1h-sunrise');
  });

  it('gives the band sharing the input the other hours', () => {
    expect(complement('sunrise-sunset')).toBe('sunset-sunrise');
    expect(complement('sunset-sunrise')).toBe('sunrise-sunset');
    expect(complement('06:00-18:30')).toBe('18:30-06:00');
    expect(complement('always')).toBeNull();
    expect(complement('06:00-08:00, 18:00-20:00')).toBeNull();
  });

  it('reads one clock range for editing', () => {
    expect(clockRange('22:00-06:00')).toEqual({ from: '22:00', until: '06:00' });
    expect(clockRange('sunset-sunrise')).toBeNull();
  });

  it('says whether the Sun is involved and where the band stands', () => {
    expect(usesSun('sunset-06:00')).toBe(true);
    expect(usesSun('18:00-06:00')).toBe(false);
    const six = Date.UTC(2026, 8, 29, 18, 0);
    expect(hoursNow(true, six)).toBe('On the air until 18:00 UTC');
    expect(hoursNow(false, six)).toBe('Off the air until 18:00 UTC');
    expect(hoursNow(true, -1)).toBeNull();
    expect(hoursNow(undefined, six)).toBeNull();
  });
});
