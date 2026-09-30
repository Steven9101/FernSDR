<script lang="ts">
  /**
   * The spectrum and waterfall, and every gesture that acts on them.
   *
   * The interaction rules are the whole reason this file is as long as it is.
   * On a phone a WebSDR normally fights you: a drag scrolls the page instead of
   * the band, pinch zooms the document, and there is no way to hit a 2 kHz
   * signal with a fingertip. The fixes are all here - the canvas claims its own
   * gestures, drags are frequency-anchored so the signal stays under your
   * finger, and a tap tunes with a tolerance sized for a finger rather than a
   * mouse.
   */
  import { onMount } from 'svelte';
  import { WaterfallDecoder } from '../dsp/waterfall';
  import { WaterfallRenderer, MAX_LINE_WIDTH, type WaterfallLine } from '../render/waterfall-gl';
  import { WaterfallFallbackRenderer } from '../render/waterfall-2d';
  import { drawOverlay, DARK_THEME, MIN_TRACE, RULER_HEIGHT, overlayLayout } from '../render/overlay';
  import { spotNear, type SpotFrequency } from '../state/bandplan';
  import { signalForCarrier } from '../util/cw';
  import { controller, display, tuning, viewport, currentBand } from '../state/store';
  import { editingLayout, layout as listenerLayout, setLayout, SPECTRUM_MAX } from '../state/layout';
  import GripHorizontal from '@lucide/svelte/icons/grip-horizontal';
  import { anchoredViewport, wheelPixels, wheelZoomFactor, zoomViewport } from '../util/viewport';
  import { waterfallLevels } from '../util/waterfall-levels';
  import { resampleLevels } from '../util/spectrum-samples';
  import { spectrumDrag } from '../util/spectrum-gesture';

  /** How far a pointer may move before a tap becomes a drag. */
  const TAP_SLOP_PX = 8;
  /** A finger, rather than a cursor: decided per gesture, not cached at load. */
  function coarsePointer(): boolean {
    return typeof window !== 'undefined' && window.matchMedia?.('(pointer: coarse)').matches === true;
  }
  const SPECTRUM_DECAY_PER_SECOND = 12;

  type Renderer = WaterfallRenderer | WaterfallFallbackRenderer;

  interface Gesture {
    kind: 'none' | 'pan' | 'tune' | 'edge-low' | 'edge-high' | 'pinch';
    startX: number;
    startY: number;
    startLowHz: number;
    startHighHz: number;
    startPassband: [number, number];
    startSignalHz: number;
    scale: number;
    slop: number;
    moved: boolean;
    pointerId: number;
  }

  let container: HTMLDivElement;
  let glCanvas: HTMLCanvasElement;
  let overlay: HTMLCanvasElement;

  let renderer: Renderer | null = null;
  const decoder = new WaterfallDecoder();
  let latestLine: { levels: Float32Array; low: number; high: number } | null = null;
  let peakHold: Float32Array | null = null;
  let lastPeakDecay = performance.now();
  let gesture: Gesture = {
    kind: 'none',
    startX: 0,
    startY: 0,
    startLowHz: 0,
    startHighHz: 0,
    startPassband: [0, 0],
    startSignalHz: 0,
    scale: 1,
    slop: TAP_SLOP_PX,
    moved: false,
    pointerId: -1,
  };
  const pointers = new Map<number, { x: number; y: number }>();
  let pinch: { distance: number; centerHz: number; span: number } | null = null;
  let size = { width: 0, height: 0, dpr: 1 };
  // The display's height in CSS pixels, for what the markup places by it.
  let displayHeight = $state(0);

  /**
   * Where the spectrum, the band plan and the ruler sit. The listener's own
   * spectrum height, set in the edit mode, may take up to 70 % of the
   * display; the page's default stays at a third, as it always was.
   */
  function traceLayout(height: number) {
    const settings = display.value;
    // No spectrum is the Display panel's switch, not a height of 0: a 0
    // stored before that was so falls back to the page's own height.
    const own = listenerLayout.value.spectrum || null;
    return overlayLayout(height, own ?? settings.spectrumHeight, settings.showSpectrum, settings.showBandPlan,
                         own === null ? 0.34 : 0.7);
  }

  // The line under the spectrum, which the edit mode lets the listener drag,
  // and the tallest the display lets it be.
  const splitTop = $derived(editingLayout.value ? traceLayout(displayHeight).spectrumHeight : 0);
  const splitMax = $derived.by(() => {
    if (!editingLayout.value) return 0;
    const settings = display.value;
    return overlayLayout(displayHeight, SPECTRUM_MAX, true, settings.showBandPlan, 0.7).spectrumHeight;
  });

  function setSpectrum(height: number) {
    // A trace under MIN_TRACE pixels is drawn as none (overlayLayout), so a
    // height in between would move nothing: it is either none or the least.
    let next = Math.max(0, Math.min(splitMax || SPECTRUM_MAX, Math.round(height)));
    if (next > 0 && next < MIN_TRACE) next = height > (listenerLayout.value.spectrum ?? splitTop) ? MIN_TRACE : 0;
    // None at all is the trace switched off, which the Display panel shows
    // and undoes; the height it had is kept for when it comes back.
    if (next === 0) {
      if (display.value.showSpectrum) display.value = { ...display.value, showSpectrum: false };
      return;
    }
    // Dragged open while the trace is switched off: it is wanted again.
    if (!display.value.showSpectrum) display.value = { ...display.value, showSpectrum: true };
    setLayout({ spectrum: next });
  }

  function onSplitDown(event: PointerEvent) {
    // Not the display's own drag, which would tune or pan.
    event.stopPropagation();
    const handle = event.currentTarget as HTMLElement;
    handle.setPointerCapture(event.pointerId);
    const top = container.getBoundingClientRect().top;
    const move = (next: PointerEvent) => setSpectrum(next.clientY - top);
    const up = () => {
      handle.removeEventListener('pointermove', move);
      handle.removeEventListener('pointerup', up);
      handle.removeEventListener('pointercancel', up);
    };
    handle.addEventListener('pointermove', move);
    handle.addEventListener('pointerup', up);
    handle.addEventListener('pointercancel', up);
  }

  // The line moves the way the key points: down makes the spectrum taller.
  // Right and left do the same as down and up, for a slider's usual keys.
  function onSplitKey(event: KeyboardEvent) {
    const steps: Record<string, number> = {
      ArrowUp: -10, ArrowLeft: -10, ArrowDown: 10, ArrowRight: 10, PageUp: -50, PageDown: 50,
    };
    const from = display.value.showSpectrum ? (listenerLayout.value.spectrum ?? splitTop) : 0;
    if (event.key in steps) setSpectrum(from + steps[event.key]);
    else if (event.key === 'Home') setSpectrum(0);
    else if (event.key === 'End') setSpectrum(SPECTRUM_MAX);
    else return;
    // Not the page's own arrow keys, which tune and change the volume.
    event.preventDefault();
    event.stopPropagation();
  }
  let lastTap = { time: 0, x: 0 };

  // Read by the render loop on every frame, so a plain variable: nothing on
  // the page is drawn from it but the overlay.
  let activeEdge: 'low' | 'high' | null = null;
  let filling = $state(true);
  let lines = 0;
  let visibleRows = 0;
  let bandId = '';
  let levelCounts: Uint32Array | null = null;
  let visibleLevels = new Float32Array(0);
  let sequence: number | null = null;
  let fallback = $state(false);

  // --- renderer lifecycle ---
  onMount(() => {
    let canvas = glCanvas;
    let current: Renderer;
    const configure = () => {
      current.setPixelRatio(size.dpr);
      current.setPalette(display.value.palette);
      current.setLevels(display.value.floorDb, display.value.ceilingDb);
      current.setReference(currentBand.value?.center ?? 0);
      renderer = current;
    };
    const useFallback = () => {
      const replacement = canvas.cloneNode(false) as HTMLCanvasElement;
      canvas.replaceWith(replacement);
      canvas = replacement;
      glCanvas = replacement;
      current = new WaterfallFallbackRenderer(replacement);
      configure();
      lines = 0;
      filling = true;
      fallback = true;
    };
    try {
      current = new WaterfallRenderer(canvas);
      configure();
    } catch {
      // A canvas that acquired a WebGL context cannot later acquire 2D, even
      // if shader setup failed. Replace it before selecting the fallback.
      useFallback();
    }
    const webglCanvas = canvas;
    const contextLost = (event: Event) => {
      event.preventDefault();
      current.dispose();
      useFallback();
    };
    webglCanvas.addEventListener('webglcontextlost', contextLost);

    return () => {
      webglCanvas.removeEventListener('webglcontextlost', contextLost);
      current.dispose();
      renderer = null;
    };
  });

  // --- incoming lines ---
  onMount(() => {
    controller.setWaterfallSink((packet) => {
      const target = renderer;
      if (!target) return;

      if (sequence !== null && packet.sequence !== ((sequence + 1) & 0xffff)) {
        decoder.reset();
      }
      sequence = packet.sequence;
      const levels = new Float32Array(packet.width);
      if (!decoder.decode(packet.payload, packet.width, levels, packet.zeroRuns, packet.adaptive, packet.stepDb,
        packet.rangeCoded)) {
        decoder.reset();
        return;
      }

      const line: WaterfallLine = {
        lowHz: packet.lowHz,
        highHz: packet.highHz,
        width: packet.width,
        levels,
      };
      target.pushLine(line);
      lines++;
      // A handle for the browser check, which asserts that history stays
      // anchored to frequency across a pan. Nothing in the app reads it.
      (window as unknown as { __fernsdrWaterfall?: unknown }).__fernsdrWaterfall = target;
      // History arrives one line at a time, so a fresh visitor watches it grow
      // down an otherwise empty screen for however long that takes - half a
      // minute at twenty lines a second on a laptop. Saying so beats looking
      // broken. The notice goes when history actually reaches the bottom,
      // which is the only moment at which it stops being true.
      if (visibleRows > 0 && lines >= visibleRows) filling = false;
      const previous = latestLine;
      latestLine = { levels, low: packet.lowHz, high: packet.highHz };

      // Peak hold, which is how a weak intermittent signal - a CW station
      // sending, an FT8 slot - stays findable between transmissions.
      const peak = peakHold;
      if (!peak || peak.length !== levels.length || previous?.low !== packet.lowHz || previous?.high !== packet.highHz) {
        peakHold = Float32Array.from(levels);
      } else {
        for (let i = 0; i < levels.length; i++) {
          if (levels[i] > peak[i]) peak[i] = levels[i];
        }
      }

      if (display.value.autoLevels) {
        if (packet.nativeGrid) {
          const view = viewport.value;
          const low = Math.max(view.lowHz, packet.lowHz), high = Math.min(view.highHz, packet.highHz);
          if (high > low) {
            if (visibleLevels.length !== view.width) visibleLevels = new Float32Array(view.width);
            resampleLevels(levels, packet.lowHz, packet.highHz, low, high, visibleLevels);
            updateAutoLevels(visibleLevels);
          }
        } else updateAutoLevels(levels);
      }
    });
    return () => controller.setWaterfallSink(null);
  });

  function updateAutoLevels(levels: Float32Array) {
    // Percentiles rather than min/max: a single strong carrier must not
    // compress the whole display, and a dead bin must not stretch it.
    const [noise, strong] = waterfallLevels(levels, levelCounts ??= new Uint32Array(301));

    // Where the noise floor lands on the colour ramp decides whether the
    // waterfall looks alive or looks broken. Putting the floor 6 dB under the
    // noise and the ceiling above the strongest carrier sounds right and is
    // wrong: with a 40 dB carrier in the band, the noise ends up in the bottom
    // 7% of the ramp, which in every palette is black. The display then reads
    // as "nothing is being received" when in fact everything is, and the weak
    // signals people are hunting for are invisible.
    //
    // So the floor sits a little further down, and - the part that matters -
    // the ceiling is capped relative to the NOISE rather than tracking the
    // strongest signal upward. One loud broadcaster can no longer compress
    // everything else into the dark end. The noise then sits around 15% of the
    // ramp, where it has visible texture, and the 40 dB above it that actually
    // contains signals gets most of the colour.
    const targetFloor = noise - 8;
    const headroom = Math.min(strong + 6, noise + 60);
    const targetCeiling = Math.max(headroom, targetFloor + 35);

    const current = display.value;
    // Move slowly; a display that jumps whenever a station keys up is worse
    // than one that is slightly mis-set.
    const blend = 0.05;
    const floorDb = current.floorDb + (targetFloor - current.floorDb) * blend;
    const ceilingDb = current.ceilingDb + (targetCeiling - current.ceilingDb) * blend;
    if (Math.abs(floorDb - current.floorDb) > 0.05 || Math.abs(ceilingDb - current.ceilingDb) > 0.05) {
      display.value = { ...current, floorDb, ceilingDb };
    }
  }

  // Paints the display at once; set by the render loop below. Called by the
  // resize as well: setting a canvas's size clears it, and the observer that
  // resizes runs after this frame's animation callbacks, so without a paint
  // of its own the cleared canvas is what the screen shows for a frame. That
  // was the flash when the first click took the audio prompt away.
  let paintNow: (() => void) | null = null;

  // --- sizing ---
  onMount(() => {
    const resize = () => {
      const rect = container.getBoundingClientRect();
      // Cap the pixel ratio: a 3x phone display would otherwise ask the GPU
      // for nine times the fill rate for no visible benefit on a waterfall.
      const dpr = Math.min(window.devicePixelRatio || 1, 2);
      const width = Math.max(1, Math.round(rect.width * dpr));
      const height = Math.max(1, Math.round(rect.height * dpr));
      size = { width, height, dpr };
      displayHeight = height / dpr;
      renderer?.setPixelRatio(dpr);

      for (const canvas of [glCanvas, overlay]) {
        canvas.width = width;
        canvas.height = height;
        canvas.style.width = `${rect.width}px`;
        canvas.style.height = `${rect.height}px`;
      }

      paintNow?.();

      // Ask for as many bins as there are pixels, and no more: extra bins are
      // bandwidth spent on detail the display cannot show.
      controller.setWaterfallWidth(Math.min(MAX_LINE_WIDTH, width));
    };

    // No call of our own first: the observer reports the container's size
    // once, after the page's first layout and before its first paint. Asking
    // here with getBoundingClientRect() forced that layout synchronously in
    // the middle of mounting the page: 17 ms of main-thread time in a profile.
    const observer = new ResizeObserver(resize);
    observer.observe(container);
    window.addEventListener('orientationchange', resize);
    return () => {
      observer.disconnect();
      window.removeEventListener('orientationchange', resize);
    };
  });

  // --- palette and level changes ---
  $effect(() => {
    const settings = display.value;
    if (!renderer) return;
    renderer.setPalette(settings.palette);
    renderer.setLevels(settings.floorDb, settings.ceilingDb);
  });

  $effect(() => {
    const band = currentBand.value;
    if (renderer && band) {
      if (bandId !== band.id) {
        bandId = band.id;
        renderer.clear();
        decoder.reset();
        latestLine = null;
        peakHold = null;
        sequence = null;
        lines = 0;
        filling = true;
      }
      renderer.setReference(band.center);
    }
  });

  // --- render loop ---
  onMount(() => {
    let running = true;
    let paintedRenderer: Renderer | null = null;
    let paintedView: typeof viewport.value | null = null;
    let paintedSettings: typeof display.value | null = null;
    let paintedSpectrum: number | null = null;
    let paintedSize: typeof size | null = null;
    let paintedLine: typeof latestLine = null;
    const paint = () => {
      const target = renderer;
      const view = viewport.value;
      const settings = display.value;
      const current = size;
      const layout = traceLayout(current.height / current.dpr);
      const { spectrumHeight, waterfallTop } = layout;
      visibleRows = Math.max(1, Math.ceil(current.height / current.dpr - waterfallTop));

      // Rows only move when data arrives. Redrawing the same history at the
      // display refresh rate wastes fill rate, especially in Canvas2D. View
      // and size changes still paint on the next animation frame, so dragging
      // and zooming keep the display's full response rate.
      const line = latestLine;
      const spectrum = listenerLayout.value.spectrum;
      if (target && view.highHz > view.lowHz && (target !== paintedRenderer || spectrum !== paintedSpectrum ||
          view !== paintedView || settings !== paintedSettings || current !== paintedSize || line !== paintedLine)) {
        target.render(view.lowHz, view.highHz, waterfallTop * current.dpr);
        paintedRenderer = target;
        paintedView = view;
        paintedSettings = settings;
        paintedSpectrum = spectrum;
        paintedSize = current;
        paintedLine = line;
      }

      // Peak hold bleeds away so it tracks conditions rather than recording
      // every signal since the page loaded.
      const now = performance.now();
      const elapsed = (now - lastPeakDecay) / 1000;
      lastPeakDecay = now;
      const peak = peakHold;
      const latest = latestLine;
      if (peak && latest) {
        const decay = SPECTRUM_DECAY_PER_SECOND * elapsed;
        for (let i = 0; i < peak.length; i++) {
          peak[i] = Math.max(peak[i] - decay, latest.levels[i]);
        }
      }

      const context = overlay.getContext('2d');
      const tune = tuning.value;
      if (context) {
        // Overlay geometry is in CSS pixels so labels and grab handles
        // remain readable at the same size on a high-density phone.
        context.setTransform(current.dpr, 0, 0, current.dpr, 0, 0);
        drawOverlay(context, {
          width: current.width / current.dpr,
          height: current.height / current.dpr,
          dpr: 1,
          viewLowHz: view.lowHz,
          viewHighHz: view.highHz,
          spectrum: latest ? latest.levels : null,
          peakHold: peak,
          spectrumLowHz: latest ? latest.low : view.lowHz,
          spectrumHighHz: latest ? latest.high : view.highHz,
          // The trace never takes more than a third of the display: on a
          // phone in landscape a fixed 120 px would leave no waterfall at
          // all, and the waterfall is the part people are here for.
          spectrumHeight,
          showSpectrum: spectrumHeight > 0,
          floorDb: settings.floorDb,
          ceilingDb: settings.ceilingDb,
          tunedHz: tune.freq,
          markerHz: signalForCarrier(tune.freq, tune.mode, tune.cwPitch),
          passbandLow: tune.low,
          passbandHigh: tune.high,
          showBandPlan: layout.bandPlanHeight > 0,
          activeEdge,
          theme: DARK_THEME,
        });
      }
    };
    paintNow = paint;
    const frame = () => {
      if (!running) return;
      paint();
      requestAnimationFrame(frame);
    };
    const handle = requestAnimationFrame(frame);
    return () => {
      running = false;
      paintNow = null;
      cancelAnimationFrame(handle);
    };
  });

  // --- gestures ---

  function frequencyAt(clientX: number): number {
    const rect = container.getBoundingClientRect();
    const fraction = (clientX - rect.left) / Math.max(rect.width, 1);
    const view = viewport.value;
    return view.lowHz + (view.highHz - view.lowHz) * fraction;
  }

  function pixelsPerHz(): number {
    const rect = container.getBoundingClientRect();
    const view = viewport.value;
    return rect.width / Math.max(view.highHz - view.lowHz, 1);
  }

  function onPointerDown(event: PointerEvent) {
    if (event.pointerType === 'mouse' && event.button !== 0) return;
    container.setPointerCapture(event.pointerId);
    pointers.set(event.pointerId, { x: event.clientX, y: event.clientY });

    if (pointers.size === 2) {
      const [a, b] = [...pointers.values()];
      const view = viewport.value;
      pinch = {
        distance: Math.hypot(a.x - b.x, a.y - b.y),
        centerHz: frequencyAt((a.x + b.x) / 2),
        span: view.highHz - view.lowHz,
      };
      gesture.kind = 'pinch';
      gesture.moved = true;
      activeEdge = null;
      return;
    }
    if (pointers.size > 2) return;

    const view = viewport.value;
    const tune = tuning.value;
    const scale = pixelsPerHz();
    const rect = container.getBoundingClientRect();
    const x = event.clientX - rect.left;
    const lowEdgeX = (tune.freq + tune.low - view.lowHz) * scale;
    const highEdgeX = (tune.freq + tune.high - view.lowHz) * scale;

    const touch = event.pointerType === 'touch' || (!event.pointerType && coarsePointer());
    const markerHz = signalForCarrier(tune.freq, tune.mode, tune.cwPitch);
    const markerX = (markerHz - view.lowHz) * scale;
    const { rulerTop } = traceLayout(rect.height);
    const y = event.clientY - rect.top;
    const kind = spectrumDrag(x, lowEdgeX, highEdgeX, markerX, touch,
      y >= rulerTop && y <= rulerTop + RULER_HEIGHT, event.shiftKey);
    if (kind === 'edge-low') activeEdge = 'low';
    if (kind === 'edge-high') activeEdge = 'high';
    container.style.cursor = kind.startsWith('edge') ? 'ew-resize' : 'grabbing';

    gesture = {
      kind,
      startX: event.clientX,
      startY: event.clientY,
      startLowHz: view.lowHz,
      startHighHz: view.highHz,
      startPassband: [tune.low, tune.high],
      startSignalHz: markerHz,
      scale,
      slop: touch ? (kind === 'pan' ? TAP_SLOP_PX : 3) : 1,
      moved: false,
      pointerId: event.pointerId,
    };
  }

  function onPointerMove(event: PointerEvent) {
    if (!pointers.has(event.pointerId)) {
      if (event.pointerType === 'touch') return;
      const rect = container.getBoundingClientRect();
      const tune = tuning.value;
      const view = viewport.value;
      const scale = rect.width / Math.max(1, view.highHz - view.lowHz);
      const { rulerTop } = traceLayout(rect.height);
      const y = event.clientY - rect.top;
      const kind = spectrumDrag(event.clientX - rect.left,
        (tune.freq + tune.low - view.lowHz) * scale,
        (tune.freq + tune.high - view.lowHz) * scale,
        (signalForCarrier(tune.freq, tune.mode, tune.cwPitch) - view.lowHz) * scale,
        false, y >= rulerTop && y <= rulerTop + RULER_HEIGHT, event.shiftKey);
      container.style.cursor = kind.startsWith('edge') ? 'ew-resize' : kind === 'tune' ? 'grab'
        : markerAt(event, false) ? 'pointer' : 'crosshair';
      return;
    }
    pointers.set(event.pointerId, { x: event.clientX, y: event.clientY });

    if (gesture.kind === 'pinch' && pointers.size >= 2 && pinch) {
      const [a, b] = [...pointers.values()];
      const distance = Math.hypot(a.x - b.x, a.y - b.y);
      const ratio = pinch.distance / Math.max(distance, 1);
      const span = pinch.span * ratio;
      const rect = container.getBoundingClientRect();
      const fraction = ((a.x + b.x) / 2 - rect.left) / Math.max(rect.width, 1);
      const view = anchoredViewport(pinch.centerHz, fraction, span, currentBand.value);
      controller.setViewport(view.lowHz, view.highHz);
      return;
    }

    const dx = event.clientX - gesture.startX;
    if (!gesture.moved && Math.abs(dx) + Math.abs(event.clientY - gesture.startY) >= gesture.slop) {
      gesture.moved = true;
    }
    if (!gesture.moved) return;

    const scale = gesture.scale;

    if (gesture.kind === 'tune') {
      // Preserve the grab offset: touching beside a narrow carrier must not
      // snap it to the finger before the drag has even begun.
      controller.tune(Math.round(gesture.startSignalHz + dx / scale), {}, false);
      return;
    }

    if (gesture.kind === 'pan') {
      // Anchored to the grab point, so the signal you grabbed stays under
      // your finger for the whole drag.
      const deltaHz = -dx / scale;
      controller.setViewport(gesture.startLowHz + deltaHz, gesture.startHighHz + deltaHz);
      return;
    }

    if (gesture.kind === 'edge-low' || gesture.kind === 'edge-high') {
      const deltaHz = dx / scale;
      const [low, high] = gesture.startPassband;
      if (gesture.kind === 'edge-low') {
        controller.setPassband(Math.min(low + deltaHz, high - 50), high, 'low');
      } else {
        controller.setPassband(low, Math.max(high + deltaHz, low + 50), 'high');
      }
    }
  }

  function onPointerUp(event: PointerEvent) {
    if (!pointers.has(event.pointerId)) return;
    pointers.delete(event.pointerId);
    const ended = gesture;
    activeEdge = null;
    container.style.cursor = '';

    if (pointers.size < 2) pinch = null;
    if (pointers.size === 1 && ended.kind === 'pinch') {
      // Lifting one finger continues as a pan from the current view.
      const [pointerId, pointer] = [...pointers.entries()][0];
      const view = viewport.value;
      gesture = { ...ended, kind: 'pan', pointerId, startX: pointer.x, startY: pointer.y,
        startLowHz: view.lowHz, startHighHz: view.highHz, scale: pixelsPerHz(), moved: true };
      return;
    }
    if (pointers.size > 0) return;

    if (event.type === 'pointerup' && (ended.kind === 'pan' || ended.kind === 'tune') && !ended.moved) {
      const now = performance.now();
      const isDoubleTap = now - lastTap.time < 320 &&
                          Math.abs(event.clientX - lastTap.x) < 24;
      lastTap = { time: now, x: event.clientX };

      if (isDoubleTap) {
        zoomAt(event.clientX, 0.5);
      } else {
        const marker = markerAt(event, ended.slop > 1);
        if (marker?.mode) {
          // A band-plan marker: its frequency and the mode it is used in.
          controller.goTo(marker.hz, marker.mode);
        } else {
          // Tune to the tap. Snapping to the nearest 10 Hz keeps the readout
          // tidy without being coarse enough to miss a signal.
          const hz = Math.round(frequencyAt(event.clientX) / 10) * 10;
          controller.tune(hz);
        }
      }
    }

    gesture.kind = 'none';
  }

  /** The band-plan marker under the pointer, when it is on the band-plan strip. */
  function markerAt(event: PointerEvent, touch: boolean): SpotFrequency | null {
    const rect = container.getBoundingClientRect();
    const { spectrumHeight, bandPlanHeight } = traceLayout(rect.height);
    const y = event.clientY - rect.top;
    if (bandPlanHeight === 0 || y < spectrumHeight || y > spectrumHeight + bandPlanHeight) return null;
    const view = viewport.value;
    const hzPerPixel = (view.highHz - view.lowHz) / Math.max(rect.width, 1);
    return spotNear(frequencyAt(event.clientX), (touch ? 14 : 6) * hzPerPixel, view.lowHz, view.highHz);
  }

  function zoomAt(clientX: number, factor: number) {
    const view = viewport.value;
    const rect = container.getBoundingClientRect();
    const fraction = Math.max(0, Math.min(1, (clientX - rect.left) / Math.max(rect.width, 1)));
    const next = zoomViewport(view, fraction, factor, currentBand.value);
    controller.setViewport(next.lowHz, next.highHz);
  }

  function onWheel(event: WheelEvent) {
    event.preventDefault();
    const mode = event.deltaMode;
    const rect = container.getBoundingClientRect();
    const deltaY = wheelPixels(event.deltaY, mode, rect.height);
    const deltaX = wheelPixels(event.deltaX, mode, rect.width);
    if (event.shiftKey || (!event.ctrlKey && Math.abs(deltaX) > Math.abs(deltaY))) {
      const view = viewport.value;
      const pixels = Math.abs(deltaX) > Math.abs(deltaY) ? deltaX : deltaY;
      const delta = (pixels / Math.max(rect.width, 1)) * (view.highHz - view.lowHz);
      controller.setViewport(view.lowHz + delta, view.highHz + delta);
      return;
    }
    if (deltaY === 0) return;
    const factor = wheelZoomFactor(deltaY);
    zoomAt(event.clientX, factor);
  }
</script>

<!-- The view is in the DOM, so a test can assert on where the display
     actually is rather than inferring it from pixels. Cross-correlating a
     screenshot against a ruler of evenly spaced ticks locks onto the wrong
     tick as happily as the right one, which cost an hour of chasing a bug
     that was not there.

     Claiming every gesture (touch-action: none) is what stops the page
     scrolling and the browser zooming when the user drags across the band.

     The display is operated by pointer and wheel, and by the page's
     keyboard shortcuts, which its label names; it is focusable so those
     reach it. -->
<!-- svelte-ignore a11y_no_noninteractive_tabindex -->
<div
  bind:this={container}
  class="spectrum"
  data-view-low={Math.round(viewport.value.lowHz)}
  data-view-high={Math.round(viewport.value.highHz)}
  data-trace-height={Math.round(traceLayout(displayHeight).spectrumHeight)}
  data-renderer={fallback ? '2d' : 'webgl'}
  style:touch-action="none"
  onpointerdown={onPointerDown}
  onpointermove={onPointerMove}
  onpointerup={onPointerUp}
  onpointercancel={onPointerUp}
  onlostpointercapture={onPointerUp}
  onwheel={onWheel}
  tabindex="0"
  role="region"
  aria-label="Drag the filter or frequency scale to tune. Drag filter edges to resize. Drag the waterfall background to pan; Shift-drag always pans. Pinch or scroll to zoom, tap to tune. Arrow keys tune; X zooms in and Z zooms out."
>
  <canvas bind:this={glCanvas} class="spectrum__waterfall"></canvas>
  <canvas bind:this={overlay} class="spectrum__overlay"></canvas>
  {#if filling}
    <div class="spectrum__filling">
      Building history
    </div>
  {/if}
  {#if editingLayout.value}
    <div
      class="spectrum__split"
      style:top="max({splitTop}px, var(--split-floor))"
      role="slider"
      tabindex="0"
      aria-label="Height of the spectrum"
      aria-valuemin={0}
      aria-valuemax={Math.round(splitMax)}
      aria-valuenow={Math.round(splitTop)}
      aria-valuetext={splitTop > 0 ? `${Math.round(splitTop)} pixels; arrow down makes it taller` : 'No spectrum; arrow down shows it'}
      onpointerdown={onSplitDown}
      onkeydown={onSplitKey}
    >
      <span class="spectrum__split-grip"><GripHorizontal size={16} aria-hidden="true" /> Drag to set the spectrum's height</span>
    </div>
  {/if}
</div>
