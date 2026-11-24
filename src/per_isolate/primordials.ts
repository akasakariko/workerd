// This is the FIRST script executed during per-isolate bootstrap - it runs
// before main.ts and before any other bootstrap script. Its exports are
// cached and injected as the `primordials` pseudo-global into every
// subsequent bootstrap script's scope.
//
// Pure ECMAScript captures live in `primordials-js` (which is pre-evaluated in
// process-wide baseline snapshots). This module extends them with the JSG
// runtime built-ins from the context's own global scope, whose templates
// depend on the Worker's compatibility flags.

const jsPrimordials = require('primordials-js') as {
  readonly jsg: Record<string, unknown>;
  readonly ObjectFreeze: typeof Object.freeze;
  readonly uncurryThis: typeof primordials.uncurryThis;
  readonly captureJsgGetter: typeof primordials.captureJsgGetter;
  readonly [key: string]: unknown;
};

const {
  jsg: jsgPrimordials,
  ObjectFreeze,
  uncurryThis,
  captureJsgGetter,
} = jsPrimordials;

const AbortControllerCtor = globalThis.AbortController;
const AbortControllerAbort = uncurryThis(
  AbortControllerCtor.prototype.abort
) as (controller: AbortController, reason?: unknown) => void;
const AbortControllerSignalGet = captureJsgGetter<
  (controller: AbortController) => AbortSignal
>(AbortControllerCtor.prototype, 'signal');

const AbortSignalCtor = globalThis.AbortSignal;
const AbortSignalAbortedGet = captureJsgGetter<
  (signal: AbortSignal) => boolean
>(AbortSignalCtor.prototype, 'aborted');
const AbortSignalReasonGet = captureJsgGetter<(signal: AbortSignal) => unknown>(
  AbortSignalCtor.prototype,
  'reason'
);

const EventTargetCtor = globalThis.EventTarget;
const EventTargetPrototype = EventTargetCtor.prototype;
const EventTargetAddEventListener = uncurryThis(
  EventTargetCtor.prototype.addEventListener
) as (target: object, type: string, listener: () => void) => void;
const EventTargetRemoveEventListener = uncurryThis(
  EventTargetCtor.prototype.removeEventListener
) as (target: object, type: string, listener: () => void) => void;

// Throws for any receiver but a Blob (a File included): a brand check.
const BlobPrototypeSlice = uncurryThis(globalThis.Blob.prototype.slice) as (
  blob: Blob,
  start?: number,
  end?: number
) => Blob;

const TextDecoderCtor = globalThis.TextDecoder;
const TextEncoderCtor = globalThis.TextEncoder;

const TextEncoderEncode = uncurryThis(TextEncoderCtor.prototype.encode) as (
  encoder: TextEncoder,
  input: string
) => Uint8Array;

const TextDecoderDecode = uncurryThis(TextDecoderCtor.prototype.decode) as (
  decoder: TextDecoder,
  input?: BufferSource,
  options?: { stream?: boolean }
) => string;

// TextDecoder readonly properties use the JSG layout trap: prototype
// accessors under modern compat, own data properties under old dates.
const TextDecoderEncodingGet = captureJsgGetter<
  (decoder: TextDecoder) => string
>(TextDecoderCtor.prototype, 'encoding');
const TextDecoderFatalGet = captureJsgGetter<(decoder: TextDecoder) => boolean>(
  TextDecoderCtor.prototype,
  'fatal'
);
const TextDecoderIgnoreBOMGet = captureJsgGetter<
  (decoder: TextDecoder) => boolean
>(TextDecoderCtor.prototype, 'ignoreBOM');

// Populate and freeze the live JSG primordials container shared with modules
// pre-evaluated in a process-wide baseline snapshot. Its prototype is already
// `jsPrimordials` (frozen, null-terminated), so exporting `jsgPrimordials`
// exposes both the JSG and pure-JS primordials without copying 140+ properties.
jsgPrimordials['AbortController'] = AbortControllerCtor;
jsgPrimordials['AbortSignal'] = AbortSignalCtor;
jsgPrimordials['EventTarget'] = EventTargetCtor;
jsgPrimordials['TextDecoder'] = TextDecoderCtor;
jsgPrimordials['TextEncoder'] = TextEncoderCtor;
jsgPrimordials['BlobPrototypeSlice'] = BlobPrototypeSlice;
jsgPrimordials['EventTargetPrototype'] = EventTargetPrototype;
jsgPrimordials['EventTargetAddEventListener'] = EventTargetAddEventListener;
jsgPrimordials['EventTargetRemoveEventListener'] =
  EventTargetRemoveEventListener;
jsgPrimordials['AbortControllerCtor'] = AbortControllerCtor;
jsgPrimordials['AbortControllerAbort'] = AbortControllerAbort;
jsgPrimordials['AbortControllerSignalGet'] = AbortControllerSignalGet;
jsgPrimordials['AbortSignalAbortedGet'] = AbortSignalAbortedGet;
jsgPrimordials['AbortSignalReasonGet'] = AbortSignalReasonGet;
jsgPrimordials['TextDecoderEncodingGet'] = TextDecoderEncodingGet;
jsgPrimordials['TextDecoderFatalGet'] = TextDecoderFatalGet;
jsgPrimordials['TextDecoderIgnoreBOMGet'] = TextDecoderIgnoreBOMGet;
jsgPrimordials['TextEncoderEncode'] = TextEncoderEncode;
jsgPrimordials['TextDecoderDecode'] = TextDecoderDecode;
ObjectFreeze(jsgPrimordials);

module.exports = jsgPrimordials;
