/**
 * Ambient declarations for the AudioWorklet global scope.
 *
 * TypeScript's DOM library does not describe the worklet scope, because it is
 * a different global than the window. These are the pieces nac-worklet.ts uses.
 */

declare const sampleRate: number;
declare const currentTime: number;
declare const currentFrame: number;

interface AudioWorkletProcessor {
  readonly port: MessagePort;
}

declare const AudioWorkletProcessor: {
  prototype: AudioWorkletProcessor;
  new (options?: AudioWorkletNodeOptions): AudioWorkletProcessor;
};

declare function registerProcessor(
  name: string,
  processorCtor: new (options?: AudioWorkletNodeOptions) => AudioWorkletProcessor & {
    process(
      inputs: Float32Array[][],
      outputs: Float32Array[][],
      parameters: Record<string, Float32Array>,
    ): boolean;
  },
): void;
