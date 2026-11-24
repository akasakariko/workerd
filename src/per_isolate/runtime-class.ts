'use strict';

// Helpers that make a bootstrap class look as a JSG resource type does.

const { ObjectDefineProperties, ObjectFreeze, SymbolFor, SymbolToStringTag } =
  primordials;

const { getApiSymbol } = utils;

const kInternalClass = SymbolFor('cloudflare:internal-class');
const kResourceTypeInspect = getApiSymbol('kResourceTypeInspect');
const kNoInspectProperties = ObjectFreeze({ __proto__: null });

function setNameAndLength(fn: unknown, name: string, length: number): void {
  ObjectDefineProperties(fn, {
    __proto__: null,
    name: { __proto__: null, value: name },
    length: { __proto__: null, value: length },
  });
}

// Installs the compat-flag-independent runtime-class markers on `proto`:
// the internal-class marker and util.inspect()'s formatting hook.
function markAsRuntimeClassBase(proto: object): void {
  ObjectDefineProperties(proto, {
    __proto__: null,
    [kInternalClass]: {
      __proto__: null,
      value: kInternalClass,
      writable: false,
      enumerable: false,
      configurable: false,
    },
    [kResourceTypeInspect]: {
      __proto__: null,
      value: kNoInspectProperties,
      writable: false,
      enumerable: false,
      configurable: true,
    },
  });
}

// Installs [Symbol.toStringTag] on `proto` when `set_tostring_tag` is enabled.
function applyToStringTag(
  proto: object,
  name: string,
  writable: boolean
): void {
  if (compatFlags['set_tostring_tag'] === true) {
    ObjectDefineProperties(proto, {
      __proto__: null,
      [SymbolToStringTag]: {
        __proto__: null,
        value: name,
        writable,
        enumerable: false,
        configurable: true,
      },
    });
  }
}

// What JSG puts on every resource type's prototype, in JSG's order: under
// set_tostring_tag, the tag; the marker tools use to recognize runtime
// classes; and util.inspect()'s formatting hook, which formats an instance
// from its accessors.
function markAsRuntimeClass(proto: object, name: string): void {
  applyToStringTag(proto, name, true);
  markAsRuntimeClassBase(proto);
}

export type RuntimeClassHelpers = {
  markAsRuntimeClass: typeof markAsRuntimeClass;
  markAsRuntimeClassBase: typeof markAsRuntimeClassBase;
  applyToStringTag: typeof applyToStringTag;
  setNameAndLength: typeof setNameAndLength;
};

module.exports = {
  markAsRuntimeClass,
  markAsRuntimeClassBase,
  applyToStringTag,
  setNameAndLength,
};
