/**
 * SHA-256, HMAC-SHA256 and PBKDF2, by hand.
 *
 * The panel uses native Web Crypto for password derivation when available.
 * These routines supply request HMACs and a derivation fallback for local
 * browsers without Web Crypto. Remote administration requires HTTPS.
 *
 * These are the primitives the server already has in `util/password.h`, so the
 * two sides derive the same key from the same password. Checked against the
 * RFC 6234 and RFC 6070 vectors in crypto.test.ts; a hand-rolled hash that is
 * merely plausible is worse than none, because it looks like it works.
 */

const K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
]);

const BLOCK = 64;
const DIGEST = 32;

function rotr(x: number, n: number): number {
  return (x >>> n) | (x << (32 - n));
}

/** One SHA-256 compression over `block`, updating `h` in place. */
function compress(h: Uint32Array, block: Uint8Array, offset: number, w: Uint32Array): void {
  for (let i = 0; i < 16; i++) {
    const j = offset + i * 4;
    w[i] = (block[j] << 24) | (block[j + 1] << 16) | (block[j + 2] << 8) | block[j + 3];
  }
  for (let i = 16; i < 64; i++) {
    const a = w[i - 15];
    const b = w[i - 2];
    const s0 = rotr(a, 7) ^ rotr(a, 18) ^ (a >>> 3);
    const s1 = rotr(b, 17) ^ rotr(b, 19) ^ (b >>> 10);
    w[i] = (w[i - 16] + s0 + w[i - 7] + s1) | 0;
  }

  let a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (let i = 0; i < 64; i++) {
    const S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const ch = (e & f) ^ (~e & g);
    const t1 = (hh + S1 + ch + K[i] + w[i]) | 0;
    const S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const maj = (a & b) ^ (a & c) ^ (b & c);
    const t2 = (S0 + maj) | 0;
    hh = g; g = f; f = e;
    e = (d + t1) | 0;
    d = c; c = b; b = a;
    a = (t1 + t2) | 0;
  }
  h[0] = (h[0] + a) | 0; h[1] = (h[1] + b) | 0; h[2] = (h[2] + c) | 0; h[3] = (h[3] + d) | 0;
  h[4] = (h[4] + e) | 0; h[5] = (h[5] + f) | 0; h[6] = (h[6] + g) | 0; h[7] = (h[7] + hh) | 0;
}

export function sha256(message: Uint8Array): Uint8Array {
  const h = new Uint32Array([
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
  ]);
  const w = new Uint32Array(64);

  const full = Math.floor(message.length / BLOCK) * BLOCK;
  for (let i = 0; i < full; i += BLOCK) compress(h, message, i, w);

  // The tail: the remainder, a 0x80 byte, zero padding, and the bit length as
  // a 64-bit big-endian integer. Two blocks when the remainder leaves no room.
  const rest = message.length - full;
  const tail = new Uint8Array(rest + 9 > BLOCK ? BLOCK * 2 : BLOCK);
  tail.set(message.subarray(full));
  tail[rest] = 0x80;
  const bits = message.length * 8;
  // Lengths beyond 2^32 bits cannot occur here and would need a BigInt to
  // express, so only the low 32 bits are written.
  const end = tail.length;
  tail[end - 4] = (bits >>> 24) & 0xff;
  tail[end - 3] = (bits >>> 16) & 0xff;
  tail[end - 2] = (bits >>> 8) & 0xff;
  tail[end - 1] = bits & 0xff;
  for (let i = 0; i < tail.length; i += BLOCK) compress(h, tail, i, w);

  const out = new Uint8Array(DIGEST);
  for (let i = 0; i < 8; i++) {
    out[i * 4] = (h[i] >>> 24) & 0xff;
    out[i * 4 + 1] = (h[i] >>> 16) & 0xff;
    out[i * 4 + 2] = (h[i] >>> 8) & 0xff;
    out[i * 4 + 3] = h[i] & 0xff;
  }
  return out;
}

export function hmacSha256(key: Uint8Array, message: Uint8Array): Uint8Array {
  const block = new Uint8Array(BLOCK);
  block.set(key.length > BLOCK ? sha256(key) : key);

  const inner = new Uint8Array(BLOCK + message.length);
  const outer = new Uint8Array(BLOCK + DIGEST);
  for (let i = 0; i < BLOCK; i++) {
    inner[i] = block[i] ^ 0x36;
    outer[i] = block[i] ^ 0x5c;
  }
  inner.set(message, BLOCK);
  outer.set(sha256(inner), BLOCK);
  return sha256(outer);
}

/**
 * PBKDF2-HMAC-SHA256, RFC 8018.
 *
 * The iteration count comes from the server, because it is a property of the
 * stored hash rather than a choice this side gets to make. At the 200 000 the
 * server writes by default this takes a fraction of a second here, which is
 * the right trade for a login: slow enough to be worth attacking offline,
 * fast enough that nobody notices.
 */
export function pbkdf2Sha256(
  password: Uint8Array,
  salt: Uint8Array,
  iterations: number,
  length: number,
): Uint8Array {
  const out = new Uint8Array(length);
  const block = new Uint8Array(salt.length + 4);
  block.set(salt);

  for (let index = 1, written = 0; written < length; index++) {
    block[salt.length] = (index >>> 24) & 0xff;
    block[salt.length + 1] = (index >>> 16) & 0xff;
    block[salt.length + 2] = (index >>> 8) & 0xff;
    block[salt.length + 3] = index & 0xff;

    let current = hmacSha256(password, block);
    const accumulated = current.slice();
    for (let i = 1; i < iterations; i++) {
      current = hmacSha256(password, current);
      for (let j = 0; j < DIGEST; j++) accumulated[j] ^= current[j];
    }
    const take = Math.min(DIGEST, length - written);
    out.set(accumulated.subarray(0, take), written);
    written += take;
  }
  return out;
}

export function utf8(text: string): Uint8Array {
  return new TextEncoder().encode(text);
}

export function fromHex(hex: string): Uint8Array {
  const out = new Uint8Array(hex.length >> 1);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16);
  return out;
}

export function toHex(bytes: Uint8Array): string {
  let out = '';
  for (const b of bytes) out += b.toString(16).padStart(2, '0');
  return out;
}
