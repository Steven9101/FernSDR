/**
 * WebGL2 waterfall.
 *
 * Each incoming line is one texSubImage2D into a ring-buffer texture, and the
 * whole display is one quad: adding a line is O(1) regardless of how much
 * history is on screen, which is what makes this smooth on a phone.
 *
 * The part worth explaining is the frequency anchoring. Every line arrives
 * carrying the span it covers, and that span is stored per row in a second
 * texture. The fragment shader maps each screen pixel to a frequency and then
 * asks each row where that frequency sat *in that row*. Pan or zoom, and the
 * history stays locked to frequency instead of shearing sideways - the thing
 * that makes most waterfalls unusable to drag around.
 */
import { buildPaletteTexture, type PaletteId } from './palettes';

const HISTORY_ROWS = 1024;
export const MAX_LINE_WIDTH = 2048;

const VERTEX_SHADER = `#version 300 es
in vec2 aPosition;
out vec2 vUv;
void main() {
  vUv = vec2(aPosition.x * 0.5 + 0.5, 0.5 - aPosition.y * 0.5);
  gl_Position = vec4(aPosition, 0.0, 1.0);
}`;

const FRAGMENT_SHADER = `#version 300 es
precision highp float;

in vec2 vUv;
out vec4 fragColor;

uniform sampler2D uLevels;    // R16F: decoded dB values, one row per line
uniform sampler2D uRowMeta;   // RGBA32F: (low offset Hz, span Hz, columns used) per row
uniform sampler2D uPalette;   // RGBA8 colour ramp

uniform float uViewLow;       // view low edge, Hz relative to the reference
uniform float uViewSpan;
uniform float uFloorDb;
uniform float uCeilingDb;
uniform float uWriteRow;      // next row to be written
uniform float uRowsVisible;
uniform vec3  uEmptyColor;

void main() {
  float frequency = uViewLow + uViewSpan * vUv.x;

  // Newest line at the top. Walk back through the ring from the write head.
  float age = floor(vUv.y * uRowsVisible);
  float row = mod(uWriteRow - 1.0 - age + ${HISTORY_ROWS}.0 * 2.0, ${HISTORY_ROWS}.0);
  float rowV = (row + 0.5) / ${HISTORY_ROWS}.0;

  vec3 meta = texture(uRowMeta, vec2(0.5, rowV)).rgb;
  float rowLow = meta.r;
  float rowSpan = meta.g;      // the span this line actually covered
  float rowColumns = meta.b;   // how many texture columns hold its data

  if (rowSpan <= 0.0 || rowColumns <= 0.0) {
    fragColor = vec4(uEmptyColor, 1.0);
    return;
  }

  // Where this frequency sits within the line, 0 to 1 across what it covered.
  float u = (frequency - rowLow) / rowSpan;
  if (u < 0.0 || u > 1.0) {
    // This line never covered this frequency: the user has panned or zoomed
    // beyond what that line held. Showing nothing is honest; stretching the
    // edge pixel across the gap would invent signals that were never received.
    fragColor = vec4(uEmptyColor, 1.0);
    return;
  }

  // Then into the texture, which is wider than the line. A line of 1024 bins
  // occupies half of a 2048-column row and the rest is zeros. Mapping u
  // straight onto the texture - as this did - made every row claim data across
  // TWICE the frequency range it held, and drew the padding as a noise floor.
  // Panning left blanked correctly because u went negative; panning right
  // walked into the padding and rendered it as though it were spectrum.
  // Bin i of the line covers [i, i+1) of u, so its centre is at u=(i+0.5)/n and
  // its texel centre is at i+0.5 - which is exactly u*n. Adding a half-texel on
  // top of that, as this did, shifted every row half a bin to the right.
  float texel = clamp(u * rowColumns, 0.5, rowColumns - 0.5);
  float db = texture(uLevels, vec2(texel / ${MAX_LINE_WIDTH}.0, rowV)).r;
  float level = clamp((db - uFloorDb) / max(uCeilingDb - uFloorDb, 1.0), 0.0, 1.0);
  fragColor = vec4(texture(uPalette, vec2(level, 0.5)).rgb, 1.0);
}`;

export interface WaterfallLine {
  lowHz: number;
  highHz: number;
  width: number;
  levels: Float32Array;
}

export class WaterfallRenderer {
  private gl: WebGL2RenderingContext;
  private program: WebGLProgram;
  private vao: WebGLVertexArrayObject;
  private levelsTexture: WebGLTexture;
  private metaTexture: WebGLTexture;
  private paletteTexture: WebGLTexture;
  private uniforms: Record<string, WebGLUniformLocation | null> = {};

  private writeRow = 0;
  private referenceHz = 0;
  private paletteId: PaletteId | null = null;
  private metaScratch = new Float32Array(4);
  /**
   * A CPU mirror of what each row was told to hold.
   *
   * Costs 12 kB and exists because the alternative - inferring whether history
   * is anchored to frequency by cross-correlating screenshots - is a way to
   * spend an afternoon proving two different things.
   */
  private metaMirror = new Float32Array(3 * HISTORY_ROWS);

  private pixelRatio = 1;
  private floorDb = -115;
  private ceilingDb = -35;
  private emptyColor: [number, number, number] = [0.03, 0.04, 0.06];

  constructor(private readonly canvas: HTMLCanvasElement) {
    const gl = canvas.getContext('webgl2', {
      alpha: false,
      antialias: false,
      depth: false,
      // The waterfall is redrawn every frame anyway; not preserving the buffer
      // lets the driver take the cheaper path.
      preserveDrawingBuffer: false,
      powerPreference: 'low-power',
    });
    if (!gl) throw new Error('WebGL2 is not available');
    this.gl = gl;

    this.program = this.link(VERTEX_SHADER, FRAGMENT_SHADER);
    gl.useProgram(this.program);
    for (const name of [
      'uLevels', 'uRowMeta', 'uPalette', 'uViewLow', 'uViewSpan', 'uFloorDb', 'uCeilingDb',
      'uWriteRow', 'uRowsVisible', 'uEmptyColor',
    ]) {
      this.uniforms[name] = gl.getUniformLocation(this.program, name);
    }

    const vao = gl.createVertexArray();
    if (!vao) throw new Error('could not create a vertex array');
    this.vao = vao;
    gl.bindVertexArray(vao);
    const buffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
    const location = gl.getAttribLocation(this.program, 'aPosition');
    gl.enableVertexAttribArray(location);
    gl.vertexAttribPointer(location, 2, gl.FLOAT, false, 0, 0);

    // LINEAR horizontally smooths the display when zoomed in; vertically it
    // costs nothing, because row coordinates are computed at exact texel
    // centres.
    this.levelsTexture = this.createTexture(gl.LINEAR);
    gl.bindTexture(gl.TEXTURE_2D, this.levelsTexture);
    // Half floats represent every integral codec level exactly and are
    // filterable in core WebGL2. Clipping levels into an 8-bit display range
    // before interpolation changes slopes between native FFT bins, sometimes
    // by tens of dB. This texture uses 4 MiB, with no extra CPU history copy.
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.R16F, MAX_LINE_WIDTH, HISTORY_ROWS, 0, gl.RED,
                  gl.FLOAT, null);

    // NEAREST, for two independent reasons. RG32F is not a filterable format
    // in core WebGL2, so a LINEAR filter leaves the texture incomplete and
    // every sample reads as zero - which silently renders the entire waterfall
    // as "no data". And interpolating between two rows' frequency spans would
    // be meaningless even if it were allowed.
    this.metaTexture = this.createTexture(gl.NEAREST);
    gl.bindTexture(gl.TEXTURE_2D, this.metaTexture);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, 1, HISTORY_ROWS, 0, gl.RGBA, gl.FLOAT, null);

    this.paletteTexture = this.createTexture(gl.LINEAR);
    this.setPalette('aurora');

    gl.uniform1i(this.uniforms.uLevels, 0);
    gl.uniform1i(this.uniforms.uRowMeta, 1);
    gl.uniform1i(this.uniforms.uPalette, 2);
  }

  private createTexture(filter: number): WebGLTexture {
    const gl = this.gl;
    const texture = gl.createTexture();
    if (!texture) throw new Error('could not create a texture');
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return texture;
  }

  private link(vertexSource: string, fragmentSource: string): WebGLProgram {
    const gl = this.gl;
    const compile = (type: number, source: string): WebGLShader => {
      const shader = gl.createShader(type);
      if (!shader) throw new Error('could not create a shader');
      gl.shaderSource(shader, source);
      gl.compileShader(shader);
      if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) {
        throw new Error(`shader failed to compile: ${gl.getShaderInfoLog(shader)}`);
      }
      return shader;
    };
    const program = gl.createProgram();
    if (!program) throw new Error('could not create a program');
    gl.attachShader(program, compile(gl.VERTEX_SHADER, vertexSource));
    gl.attachShader(program, compile(gl.FRAGMENT_SHADER, fragmentSource));
    gl.linkProgram(program);
    if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
      throw new Error(`program failed to link: ${gl.getProgramInfoLog(program)}`);
    }
    return program;
  }

  setPalette(id: PaletteId): void {
    if (id === this.paletteId) return;
    this.paletteId = id;
    const gl = this.gl;
    gl.bindTexture(gl.TEXTURE_2D, this.paletteTexture);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, 256, 1, 0, gl.RGBA, gl.UNSIGNED_BYTE,
                  buildPaletteTexture(id));
  }

  /**
   * The device pixel ratio, so one received line occupies one CSS pixel row
   * rather than one device pixel row. Without this the waterfall appears to
   * scroll at half speed on a 2x display and a third on a 3x one - the same
   * receiver would look sluggish on a phone and fine on a laptop.
   */
  setPixelRatio(ratio: number): void {
    this.pixelRatio = Math.max(1, ratio);
  }

  setLevels(floorDb: number, ceilingDb: number): void {
    this.floorDb = floorDb;
    this.ceilingDb = ceilingDb;
  }

  setEmptyColor(r: number, g: number, b: number): void {
    this.emptyColor = [r, g, b];
  }

  /**
   * Frequencies are held as offsets from a reference, because a 32-bit float
   * cannot resolve 1 Hz at 30 MHz - it would quantise the display to about
   * 2 Hz steps and make fine tuning visibly jump.
   */
  setReference(hz: number): void {
    if (Math.abs(hz - this.referenceHz) < 1) return;
    this.referenceHz = hz;
    this.clear();
  }

  clear(): void {
    const gl = this.gl;
    const empty = new Float32Array(4 * HISTORY_ROWS);
    gl.bindTexture(gl.TEXTURE_2D, this.metaTexture);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, 1, HISTORY_ROWS, gl.RGBA, gl.FLOAT, empty);
    this.writeRow = 0;
  }

  pushLine(line: WaterfallLine): void {
    const gl = this.gl;
    const width = Math.min(line.width, MAX_LINE_WIDTH);

    gl.bindTexture(gl.TEXTURE_2D, this.levelsTexture);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, this.writeRow, width, 1, gl.RED, gl.FLOAT, line.levels, 0);

    // The line's real span, and how much of the texture row it occupies. Both
    // are needed: the span says which frequencies this row covers, and the
    // column count says where its data stops and the padding begins.
    this.metaScratch[0] = line.lowHz - this.referenceHz;
    this.metaScratch[1] = line.highHz - line.lowHz;
    this.metaScratch[2] = width;
    this.metaScratch[3] = 0;
    this.metaMirror[this.writeRow * 3] = line.lowHz;
    this.metaMirror[this.writeRow * 3 + 1] = line.highHz;
    this.metaMirror[this.writeRow * 3 + 2] = width;

    gl.bindTexture(gl.TEXTURE_2D, this.metaTexture);
    gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, this.writeRow, 1, 1, gl.RGBA, gl.FLOAT, this.metaScratch,
                     0);

    this.writeRow = (this.writeRow + 1) % HISTORY_ROWS;
  }

  render(viewLowHz: number, viewHighHz: number, top = 0): void {
    const gl = this.gl;
    const width = this.canvas.width;
    const height = Math.max(0, this.canvas.height - Math.round(top));
    if (width === 0 || height === 0) return;

    gl.clearColor(...this.emptyColor, 1);
    gl.clear(gl.COLOR_BUFFER_BIT);
    gl.viewport(0, 0, width, height);
    gl.useProgram(this.program);
    gl.bindVertexArray(this.vao);

    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.levelsTexture);
    gl.activeTexture(gl.TEXTURE1);
    gl.bindTexture(gl.TEXTURE_2D, this.metaTexture);
    gl.activeTexture(gl.TEXTURE2);
    gl.bindTexture(gl.TEXTURE_2D, this.paletteTexture);

    gl.uniform1f(this.uniforms.uViewLow, viewLowHz - this.referenceHz);
    gl.uniform1f(this.uniforms.uViewSpan, viewHighHz - viewLowHz);
    gl.uniform1f(this.uniforms.uFloorDb, this.floorDb);
    gl.uniform1f(this.uniforms.uCeilingDb, this.ceilingDb);
    gl.uniform1f(this.uniforms.uWriteRow, this.writeRow);
    gl.uniform1f(this.uniforms.uRowsVisible, Math.min(HISTORY_ROWS, height / this.pixelRatio));
    gl.uniform3f(this.uniforms.uEmptyColor, ...this.emptyColor);

    gl.drawArrays(gl.TRIANGLES, 0, 3);
  }

  /** What the row `age` lines back was written with, for tests. */
  rowMeta(age: number): { lowHz: number; highHz: number; width: number } {
    const row = ((this.writeRow - 1 - age) % HISTORY_ROWS + HISTORY_ROWS) % HISTORY_ROWS;
    return {
      lowHz: this.metaMirror[row * 3],
      highHz: this.metaMirror[row * 3 + 1],
      width: this.metaMirror[row * 3 + 2],
    };
  }

  dispose(): void {
    const gl = this.gl;
    gl.deleteTexture(this.levelsTexture);
    gl.deleteTexture(this.metaTexture);
    gl.deleteTexture(this.paletteTexture);
    gl.deleteProgram(this.program);
    gl.deleteVertexArray(this.vao);
  }
}
