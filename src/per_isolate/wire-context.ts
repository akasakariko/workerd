'use strict';

// Pre-evaluated context-wiring module used by process-wide baseline snapshots.
//
// Evaluated once in buildBaselineSnapshot(): requires all compat-flag-independent
// heavy modules, pre-builds and freezes every PropertyDescriptorMap used to
// install globals, and exports `wireFromBaseline(global)` so restoring a context
// from a baseline snapshot requires zero require() calls, zero descriptor-bag
// allocations, and zero primordials copying.

import type { DomExceptionModule } from './dom-exception';
import type { EventSourceExports } from './eventsource/eventsource';
import type { UrlExports } from './url/url';
import type { UrlPatternExports } from './urlpattern/urlpattern';

const jsPrimordials = require('primordials-js') as typeof primordials;
const {
  jsg: jsgPrimordials,
  ObjectDefineProperties,
  ObjectFreeze,
  ObjectGetPrototypeOf,
  captureJsgGetter,
  uncurryThis,
} = jsPrimordials;

const { defineLazyDataProperty } = utils;

// Pre-evaluate all compat-flag-independent heavy modules once in the baseline
// snapshot so they are cached in the snapshot's require() table and captured
// directly in closures here.
require('serialization');
require('runtime-class');
require('events/event-target-internals');
require('wasm-instantiate-shim');

const domExceptionMod = require('dom-exception') as DomExceptionModule;
const streamsMod = require('webstreams/streams') as Record<string, unknown>;
require('webstreams/cpp_exports');
const digestStreamMod = require('crypto/digest-stream') as {
  DigestStream: unknown;
};
const webfsMod = require('webfs/writable-file-stream') as {
  FileSystemWritableFileStream: unknown;
  createWritable: unknown;
};
const eventSourceMod = require(
  'eventsource/eventsource'
) as EventSourceExports;

const kTsDomException = autogates['ts-dom-exception'] === true;
const kTsUrl = autogates['ts-url'] === true;
const kTsUrlPattern = autogates['ts-url-pattern'] === true;
const kTsStructuredClone = autogates['ts-structured-clone'] === true;

const urlMod = kTsUrl ? (require('url/url') as UrlExports) : undefined;
if (kTsUrl) {
  require('url/cpp_exports');
}
const urlPatternMod = kTsUrlPattern
  ? (require('urlpattern/urlpattern') as UrlPatternExports)
  : undefined;
const structuredCloneMod = kTsStructuredClone
  ? (require('structured-clone') as {
      structuredClone: unknown;
      configure: (global: unknown) => void;
    })
  : undefined;

function dataDesc(value: unknown, enumerable: boolean): PropertyDescriptor {
  return ObjectFreeze({
    __proto__: null,
    configurable: true,
    enumerable,
    writable: true,
    value,
  }) as PropertyDescriptor;
}

function freezeBag(
  entries: Record<string, PropertyDescriptor>
): PropertyDescriptorMap {
  return ObjectFreeze(entries) as unknown as PropertyDescriptorMap;
}

const kDomExceptionProtoDescriptors = kTsDomException
  ? freezeBag({
      __proto__: null as unknown as PropertyDescriptor,
      DOMException: dataDesc(domExceptionMod.DOMException, true),
      QuotaExceededError: dataDesc(domExceptionMod.QuotaExceededError, true),
    })
  : undefined;

const kDomExceptionGlobalDescriptors = kTsDomException
  ? freezeBag({
      __proto__: null as unknown as PropertyDescriptor,
      DOMException: dataDesc(domExceptionMod.DOMException, false),
      QuotaExceededError: dataDesc(domExceptionMod.QuotaExceededError, false),
    })
  : undefined;

const kStreamsDescriptors = freezeBag({
  __proto__: null as unknown as PropertyDescriptor,
  ReadableStream: dataDesc(streamsMod['ReadableStream'], false),
  ReadableStreamDefaultReader: dataDesc(
    streamsMod['ReadableStreamDefaultReader'],
    false
  ),
  ReadableStreamBYOBReader: dataDesc(
    streamsMod['ReadableStreamBYOBReader'],
    false
  ),
  ReadableStreamDefaultController: dataDesc(
    streamsMod['ReadableStreamDefaultController'],
    false
  ),
  ReadableByteStreamController: dataDesc(
    streamsMod['ReadableByteStreamController'],
    false
  ),
  ReadableStreamBYOBRequest: dataDesc(
    streamsMod['ReadableStreamBYOBRequest'],
    false
  ),
  ByteLengthQueuingStrategy: dataDesc(
    streamsMod['ByteLengthQueuingStrategy'],
    false
  ),
  CountQueuingStrategy: dataDesc(streamsMod['CountQueuingStrategy'], false),
  WritableStream: dataDesc(streamsMod['WritableStream'], false),
  WritableStreamDefaultWriter: dataDesc(
    streamsMod['WritableStreamDefaultWriter'],
    false
  ),
  WritableStreamDefaultController: dataDesc(
    streamsMod['WritableStreamDefaultController'],
    false
  ),
  TransformStream: dataDesc(streamsMod['TransformStream'], false),
  TransformStreamDefaultController: dataDesc(
    streamsMod['TransformStreamDefaultController'],
    false
  ),
  IdentityTransformStream: dataDesc(
    streamsMod['IdentityTransformStream'],
    false
  ),
  FixedLengthStream: dataDesc(streamsMod['FixedLengthStream'], false),
  TextEncoderStream: dataDesc(streamsMod['TextEncoderStream'], false),
  TextDecoderStream: dataDesc(streamsMod['TextDecoderStream'], false),
  CompressionStream: dataDesc(streamsMod['CompressionStream'], false),
  DecompressionStream: dataDesc(streamsMod['DecompressionStream'], false),
});

const kDigestStreamDescriptors = freezeBag({
  __proto__: null as unknown as PropertyDescriptor,
  DigestStream: dataDesc(digestStreamMod.DigestStream, true),
});

const kFileSystemWritableDescriptors = freezeBag({
  __proto__: null as unknown as PropertyDescriptor,
  FileSystemWritableFileStream: dataDesc(
    webfsMod.FileSystemWritableFileStream,
    false
  ),
});

const kCreateWritableDescriptors = freezeBag({
  __proto__: null as unknown as PropertyDescriptor,
  createWritable: dataDesc(webfsMod.createWritable, true),
});

const kDrainingReaderDescriptors = freezeBag({
  __proto__: null as unknown as PropertyDescriptor,
  ReadableStreamDrainingReader: dataDesc(
    streamsMod['ReadableStreamDrainingReader'],
    false
  ),
});

const kEventSourceProtoDescriptors = freezeBag({
  __proto__: null as unknown as PropertyDescriptor,
  EventSource: dataDesc(eventSourceMod.EventSource, true),
});

const kEventSourceGlobalDescriptors = freezeBag({
  __proto__: null as unknown as PropertyDescriptor,
  EventSource: dataDesc(eventSourceMod.EventSource, false),
});

const kUrlDescriptors =
  urlMod !== undefined
    ? freezeBag({
        __proto__: null as unknown as PropertyDescriptor,
        URL: dataDesc(urlMod.URL, false),
        URLSearchParams: dataDesc(urlMod.URLSearchParams, false),
      })
    : undefined;

const kUrlPatternDescriptors =
  urlPatternMod !== undefined
    ? freezeBag({
        __proto__: null as unknown as PropertyDescriptor,
        URLPattern: dataDesc(urlPatternMod.URLPattern, false),
      })
    : undefined;

const kStructuredCloneDescriptors =
  structuredCloneMod !== undefined
    ? freezeBag({
        __proto__: null as unknown as PropertyDescriptor,
        structuredClone: dataDesc(structuredCloneMod.structuredClone, true),
      })
    : undefined;

const kMessageChannelSpecifier = 'messagechannel/messagechannel';

function initJsgPrimordials(global: typeof globalThis): unknown {
  const mutableJsg = jsgPrimordials as unknown as Record<string, unknown>;
  const AbortControllerCtor = global.AbortController;
  const AbortSignalCtor = global.AbortSignal;
  const EventTargetCtor = global.EventTarget;
  const EventTargetPrototype = EventTargetCtor.prototype;
  const TextDecoderCtor = global.TextDecoder;
  const TextEncoderCtor = global.TextEncoder;

  mutableJsg['AbortController'] = AbortControllerCtor;
  mutableJsg['AbortSignal'] = AbortSignalCtor;
  mutableJsg['EventTarget'] = EventTargetCtor;
  mutableJsg['TextDecoder'] = TextDecoderCtor;
  mutableJsg['TextEncoder'] = TextEncoderCtor;
  mutableJsg['BlobPrototypeSlice'] = uncurryThis(global.Blob.prototype.slice);
  mutableJsg['EventTargetPrototype'] = EventTargetPrototype;
  mutableJsg['EventTargetAddEventListener'] = uncurryThis(
    EventTargetPrototype.addEventListener
  );
  mutableJsg['EventTargetRemoveEventListener'] = uncurryThis(
    EventTargetPrototype.removeEventListener
  );
  mutableJsg['AbortControllerCtor'] = AbortControllerCtor;
  mutableJsg['AbortControllerAbort'] = uncurryThis(
    AbortControllerCtor.prototype.abort
  );
  mutableJsg['AbortControllerSignalGet'] = captureJsgGetter(
    AbortControllerCtor.prototype,
    'signal'
  );
  mutableJsg['AbortSignalAbortedGet'] = captureJsgGetter(
    AbortSignalCtor.prototype,
    'aborted'
  );
  mutableJsg['AbortSignalReasonGet'] = captureJsgGetter(
    AbortSignalCtor.prototype,
    'reason'
  );
  mutableJsg['TextDecoderEncodingGet'] = captureJsgGetter(
    TextDecoderCtor.prototype,
    'encoding'
  );
  mutableJsg['TextDecoderFatalGet'] = captureJsgGetter(
    TextDecoderCtor.prototype,
    'fatal'
  );
  mutableJsg['TextDecoderIgnoreBOMGet'] = captureJsgGetter(
    TextDecoderCtor.prototype,
    'ignoreBOM'
  );
  mutableJsg['TextEncoderEncode'] = uncurryThis(
    TextEncoderCtor.prototype.encode
  );
  mutableJsg['TextDecoderDecode'] = uncurryThis(
    TextDecoderCtor.prototype.decode
  );
  ObjectFreeze(jsgPrimordials);
  return jsgPrimordials;
}

// Fast per-context wiring invoked directly from C++ when booting a context from
// a process-wide baseline snapshot. Returns the frozen primordials object.
function wireFromBaseline(global: typeof globalThis): unknown {
  const primed = initJsgPrimordials(global);
  const globalPrototype = ObjectGetPrototypeOf(global) as object;
  const specCompliant = compatFlags['spec_compliant_property_attributes'] === true;

  domExceptionMod.configure(global);
  if (kDomExceptionProtoDescriptors !== undefined) {
    ObjectDefineProperties(globalPrototype, kDomExceptionProtoDescriptors);
    if (specCompliant && kDomExceptionGlobalDescriptors !== undefined) {
      ObjectDefineProperties(global, kDomExceptionGlobalDescriptors);
    }
  }

  if (compatFlags['typescript_implemented_streams'] === true) {
    ObjectDefineProperties(global, kStreamsDescriptors);
    const { Crypto } = global as unknown as { Crypto: { prototype: object } };
    ObjectDefineProperties(Crypto.prototype, kDigestStreamDescriptors);

    const { FileSystemFileHandle, FileSystemWritableFileStream: NativeStream } =
      global as unknown as {
        FileSystemFileHandle?: { prototype: object };
        FileSystemWritableFileStream?: unknown;
      };
    if (FileSystemFileHandle !== undefined && NativeStream !== undefined) {
      ObjectDefineProperties(global, kFileSystemWritableDescriptors);
      ObjectDefineProperties(
        FileSystemFileHandle.prototype,
        kCreateWritableDescriptors
      );
    }

    if (compatFlags['expose_draining_reader'] === true) {
      ObjectDefineProperties(global, kDrainingReaderDescriptors);
    }
  }

  if (compatFlags['expose_global_message_channel'] === true) {
    defineLazyDataProperty(
      globalPrototype,
      'MessageChannel',
      kMessageChannelSpecifier,
      true
    );
    defineLazyDataProperty(
      globalPrototype,
      'MessagePort',
      kMessageChannelSpecifier,
      true
    );
    if (specCompliant) {
      defineLazyDataProperty(
        global,
        'MessageChannel',
        kMessageChannelSpecifier,
        false
      );
      defineLazyDataProperty(
        global,
        'MessagePort',
        kMessageChannelSpecifier,
        false
      );
    }
  }

  eventSourceMod.configure();
  ObjectDefineProperties(globalPrototype, kEventSourceProtoDescriptors);
  if (specCompliant) {
    ObjectDefineProperties(global, kEventSourceGlobalDescriptors);
  }

  if (urlMod !== undefined && kUrlDescriptors !== undefined) {
    urlMod.configure();
    ObjectDefineProperties(global, kUrlDescriptors);
  }

  if (urlPatternMod !== undefined && kUrlPatternDescriptors !== undefined) {
    urlPatternMod.configure();
    ObjectDefineProperties(global, kUrlPatternDescriptors);
  }

  if (
    structuredCloneMod !== undefined &&
    kStructuredCloneDescriptors !== undefined
  ) {
    structuredCloneMod.configure(global);
    ObjectDefineProperties(globalPrototype, kStructuredCloneDescriptors);
  }

  if (compatFlags['expose_serialization_test_class'] === true) {
    const { CloneableTestBox, CloneableTestError } = require(
      'testing/cloneable-test-box'
    ) as { CloneableTestBox: unknown; CloneableTestError: unknown };
    ObjectDefineProperties(global, {
      CloneableTestBox: dataDesc(CloneableTestBox, false),
      CloneableTestError: dataDesc(CloneableTestError, false),
    });
  }

  return primed;
}

export type WireContextExports = {
  wireFromBaseline: typeof wireFromBaseline;
};

module.exports = ObjectFreeze({
  wireFromBaseline,
});
