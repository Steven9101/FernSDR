/**
 * The published vectors, because a hash that is merely plausible is worse than
 * none: it produces confident output that no other implementation agrees with,
 * and the failure surfaces as "the password is wrong" long after the mistake.
 */
import { describe, expect, it } from 'vitest';
import { fromHex, hmacSha256, pbkdf2Sha256, sha256, toHex, utf8 } from './crypto';

describe('sha256', () => {
  // RFC 6234 section 8.5
  it('matches the RFC 6234 vectors', () => {
    expect(toHex(sha256(utf8('abc')))).toBe(
      'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',
    );
    expect(toHex(sha256(utf8('')))).toBe(
      'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',
    );
    expect(
      toHex(sha256(utf8('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq'))),
    ).toBe('248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1');
  });

  // The padding has two shapes - one block or two - and the boundary between
  // them is exactly where a hand-written implementation goes wrong.
  it('pads correctly either side of the block boundary', () => {
    const at55 = toHex(sha256(utf8('a'.repeat(55))));
    const at56 = toHex(sha256(utf8('a'.repeat(56))));
    const at64 = toHex(sha256(utf8('a'.repeat(64))));
    expect(at55).toBe('9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318');
    expect(at56).toBe('b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a');
    expect(at64).toBe('ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb');
  });

  it('hashes a megabyte without drifting', () => {
    const big = new Uint8Array(1_000_000).fill(0x61); // 'a'
    expect(toHex(sha256(big))).toBe(
      'cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0',
    );
  });
});

describe('hmacSha256', () => {
  // RFC 4231 section 4
  it('matches the RFC 4231 vectors', () => {
    expect(toHex(hmacSha256(new Uint8Array(20).fill(0x0b), utf8('Hi There')))).toBe(
      'b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7',
    );
    expect(toHex(hmacSha256(utf8('Jefe'), utf8('what do ya want for nothing?')))).toBe(
      '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843',
    );
  });

  it('hashes a key longer than the block first', () => {
    // RFC 4231 case 6: a 131-byte key
    expect(
      toHex(
        hmacSha256(
          new Uint8Array(131).fill(0xaa),
          utf8('Test Using Larger Than Block-Size Key - Hash Key First'),
        ),
      ),
    ).toBe('60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54');
  });
});

describe('pbkdf2Sha256', () => {
  // RFC 7914 section 11
  it('matches the published vectors', () => {
    const p = utf8('password');
    const s = utf8('salt');
    expect(toHex(pbkdf2Sha256(p, s, 1, 32))).toBe(
      '120fb6cffcf8b32c43e7225256c4f837a86548c92ccc35480805987cb70be17b',
    );
    expect(toHex(pbkdf2Sha256(p, s, 2, 32))).toBe(
      'ae4d0c95af6b46d32d0adff928f06dd02a303f8ef3c251dfd6e2d85a95474c43',
    );
    expect(toHex(pbkdf2Sha256(p, s, 4096, 32))).toBe(
      'c5e478d59288c841aa530db6845c4c8d962893a001ce4e11a4963873aa98134a',
    );
  });

  it('produces more than one block when asked for one', () => {
    // RFC 7914: 40 bytes, which needs two PBKDF2 blocks
    const out = pbkdf2Sha256(
      utf8('passwordPASSWORDpassword'),
      utf8('saltSALTsaltSALTsaltSALTsaltSALTsalt'),
      4096,
      40,
    );
    expect(toHex(out)).toBe(
      '348c89dbcbd32b2f32d814b8116e84cf2b17347ebc1800181c4e2a1fb8dd53e1c635518c7dac47e9',
    );
  });

  it('is fast enough to log in with', () => {
    // The server writes 200 000 by default; if this side cannot do that in
    // about a second the scheme is not usable and the number has to change.
    const started = Date.now();
    pbkdf2Sha256(utf8('correct horse'), fromHex('a1b2c3d4'), 200_000, 32);
    expect(Date.now() - started).toBeLessThan(4000);
  });
});
