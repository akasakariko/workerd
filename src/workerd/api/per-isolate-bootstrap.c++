// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "per-isolate-bootstrap.h"

#include <workerd/api/abort-bootstrap.h>
#include <workerd/api/bootstrap-utils.h>
#include <workerd/api/compression.h>
#include <workerd/api/crypto/digest-bootstrap.h>
#include <workerd/api/encoding.h>
#include <workerd/api/event-target-bootstrap.h>
#include <workerd/api/filesystem-bootstrap.h>
#include <workerd/api/request-io.h>
#include <workerd/api/url-bootstrap.h>
#include <workerd/api/urlpattern-bootstrap.h>
#include <workerd/io/compatibility-date.h>
#include <workerd/io/worker-interface.capnp.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/jsvalue.h>
#include <workerd/jsg/ser.h>
#include <workerd/jsg/setup.h>
#include <workerd/jsg/util.h>
#include <workerd/util/autogate.h>
#include <workerd/util/sentry.h>
#include <workerd/util/use-perfetto-categories.h>

#include <per_isolate/per_isolate.capnp.h>

#if KJ_HAS_COMPILER_FEATURE(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#include <sanitizer/lsan_interface.h>
#endif

#include <capnp/dynamic.h>
#include <capnp/schema.h>

namespace workerd {

static_assert(jsg::SERIALIZATION_TAG_JS_SERIALIZABLE ==
    static_cast<uint32_t>(rpc::SerializationTag::JS_SERIALIZABLE));

namespace {

// The script lookup table is the same for every context since PER_ISOLATE_BUNDLE
// is a compile-time constant. We cache it as a process-wide static to avoid
// rebuilding the HashMap on every context creation. This is safe because:
//   - The table is immutable after construction (read-only concurrent access)
//   - Keys (kj::StringPtr) and values (Module::Reader) are non-owning views
//     into the static capnp bundle data which lives for the process lifetime
//   - C++ guarantees thread-safe initialization of function-local statics
const kj::HashMap<kj::StringPtr, jsg::Module::Reader>& getScriptTable() {
  static const auto table = []() {
    jsg::Bundle::Reader bundle = PER_ISOLATE_BUNDLE;
    kj::HashMap<kj::StringPtr, jsg::Module::Reader> t;
    for (auto module: bundle.getModules()) {
      t.insert(module.getName(), module);
    }
    return t;
  }();
  return table;
}

// Parsed compat flag field: maps a JS-visible flag name to the capnp schema field.
// The schema is compiled into the binary, so these are cached per-process.
struct CompatFlagField {
  kj::StringPtr enableFlag;
  capnp::StructSchema::Field field;
};

kj::StaticArrayPtr<const CompatFlagField> getCompatFlagFields() {
  static const auto table = []() {
    auto schema = capnp::Schema::from<CompatibilityFlags>();
    kj::Vector<CompatFlagField> fields;
    for (auto field: schema.getFields()) {
      for (auto annotation: field.getProto().getAnnotations()) {
        if (annotation.getId() == COMPAT_ENABLE_FLAG_ANNOTATION_ID) {
          fields.add(CompatFlagField{
            .enableFlag = annotation.getValue().getText(),
            .field = field,
          });
          break;
        }
      }
    }
    return fields.releaseAsArray();
  }();
  return {table.begin(), table.size()};
}

// Normalize a require() specifier by stripping a leading "./" prefix.
kj::StringPtr normalizeSpecifier(kj::StringPtr specifier) {
  if (specifier.startsWith("./"_kj)) {
    return specifier.slice(2);
  }
  return specifier;
}

// Per-context state for the bootstrap require() mechanism.
// Heap-allocated and stored in the context's BOOTSTRAP_STATE embedder data slot
// so that require() remains callable for lazy evaluation after the initial
// bootstrap completes. The slot holds it as the jsg::BootstrapModules through which
// the deserializer finds classes implemented in the bootstrap scripts.
struct BootstrapState final: public jsg::BootstrapModules {
  BootstrapState(const kj::HashMap<kj::StringPtr, jsg::Module::Reader>& scripts,
      kj::Maybe<const EmbedderBootstrap&> embedder,
      const jsg::ModuleSource* moduleSource)
      : scripts(scripts),
        embedder(embedder),
        moduleSource(moduleSource) {}

  kj::Maybe<jsg::JsValue> tryGetLoadedExports(jsg::Lock& js, kj::StringPtr specifier) override {
    auto normalized = normalizeSpecifier(specifier);
    // Check per-context additions first, then the stashed cache object from a snapshot (avoids
    // copying every stashed entry into `cache` at startup).
    KJ_IF_SOME(cached, cache.find(normalized)) {
      return cached.getHandle(js);
    }
    KJ_IF_SOME(stashed, stashedCache) {
      auto obj = stashed.getHandle(js);
      if (obj.has(js, normalized)) {
        return obj.get(js, normalized);
      }
    }
    return kj::none;
  }

  // Scripts is a const reference to the process-wide script lookup table built
  // from the compiled-in bundle. It is guaranteed to outlive this BootstrapState
  const kj::HashMap<kj::StringPtr, jsg::Module::Reader>& scripts;
  // The embedder's scripts, which the caller of runPerIsolateBootstrap() keeps alive for the
  // lifetime of the context.
  kj::Maybe<const EmbedderBootstrap&> embedder;
  // Where V8 reads script source from, if not the compiled-in bundles. Same lifetime as
  // `embedder`.
  const jsg::ModuleSource* moduleSource;
  kj::HashMap<kj::String, jsg::JsRef<jsg::JsValue>> cache;
  // Stashed require() cache object restored from a baseline or per-worker snapshot, looked up on
  // demand without copying every entry into `cache`.
  kj::Maybe<jsg::JsRef<jsg::JsObject>> stashedCache;
  // Pre-compiled bootstrap script functions restored from a baseline snapshot (specifier -> fn),
  // looked up on demand.
  kj::Maybe<jsg::JsRef<jsg::JsObject>> stashedCompiledScripts;
  // Pre-compiled bootstrap script functions accumulated while building a baseline snapshot.
  kj::HashMap<kj::String, jsg::JsRef<jsg::JsFunction>> compiledScripts;
  // Functions found by tryGetBootstrapFunction(), by module specifier, then name.
  kj::HashMap<kj::String, kj::HashMap<kj::String, jsg::JsRef<jsg::JsFunction>>> functionCache;
  kj::HashSet<kj::String> loading;
  // True while building a baseline snapshot so compileBootstrapScript() uses kEagerCompile.
  bool eagerCompile = false;
  // None during baseline snapshot preload so any accidental compatFlags access fails loudly.
  kj::Maybe<jsg::JsRef<jsg::JsObject>> compatFlagsObj;
  jsg::JsRef<jsg::JsObject> autogatesObj;
  jsg::JsRef<jsg::JsObject> utilsObj;
  // The require() injected into workerd's scripts, which resolves only workerd's scripts.
  jsg::JsRef<jsg::JsFunction> requireFn;
  // The require() injected into the embedder's scripts, which also resolves the embedder's. Set
  // when there is an embedder.
  kj::Maybe<jsg::JsRef<jsg::JsFunction>> embedderRequireFn;
  // The primordials object, loaded before main and injected as a pseudo-global
  // into every script. Provides prototype-pollution-safe references to built-in
  // methods and constructors.
  kj::Maybe<jsg::JsRef<jsg::JsValue>> primordials;
};

#define VALUE_METHOD_MAP(V)                                                                        \
  V(ArrayBuffer)                                                                                   \
  V(ArrayBufferView)                                                                               \
  V(DataView)                                                                                      \
  V(Promise)                                                                                       \
  V(SharedArrayBuffer)                                                                             \
  V(Uint8Array)

#define V(type)                                                                                    \
  static void Is##type(const v8::FunctionCallbackInfo<v8::Value>& args) {                          \
    args.GetReturnValue().Set(args[0]->Is##type());                                                \
  }                                                                                                \
  static bool Is##type##FastApi(v8::Local<v8::Value> unused, v8::Local<v8::Value> value) {         \
    return value->Is##type();                                                                      \
  }                                                                                                \
  static const v8::CFunction fast_is_##type##_ = v8::CFunction::Make(Is##type##FastApi);

VALUE_METHOD_MAP(V)
#undef V

static void IsAnyArrayBuffer(const v8::FunctionCallbackInfo<v8::Value>& args) {
  args.GetReturnValue().Set(args[0]->IsArrayBuffer() || args[0]->IsSharedArrayBuffer());
}

static bool IsAnyArrayBufferFastApi(v8::Local<v8::Value> unused, v8::Local<v8::Value> value) {
  return value->IsArrayBuffer() || value->IsSharedArrayBuffer();
}

static const v8::CFunction fast_is_any_array_buffer_ = v8::CFunction::Make(IsAnyArrayBufferFastApi);

static void MarkPromiseHandled(const v8::FunctionCallbackInfo<v8::Value>& args) {
  if (args[0]->IsPromise()) {
    args[0].As<v8::Promise>()->MarkAsHandled();
  }
}

static void MarkPromiseHandledFastApi(v8::Local<v8::Value> unused, v8::Local<v8::Value> value) {
  if (value->IsPromise()) {
    value.As<v8::Promise>()->MarkAsHandled();
  }
}

static void GetApiSymbol(const v8::FunctionCallbackInfo<v8::Value>& args) {
  // liftKj converts a thrown kj/jsg exception (the validation TypeError below) into a JS
  // exception; without it the raw callback would let it escape and take down the process.
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto name = jsg::JsValue(args[0]);
    auto str = JSG_REQUIRE_NONNULL(
        name.tryCast<jsg::JsString>(), TypeError, "getApiSymbol() expects a string argument");
    return v8::Symbol::ForApi(args.GetIsolate(), str);
  });
}

// Creates the native digest object backing the TypeScript DigestStream. Unlike
// its neighbors this allocates and can throw (an unrecognized algorithm name
// raises a DOMNotSupportedError), so it is registered as a plain method rather
// than a fast-API call, and needs liftKj to turn a thrown kj::Exception into a
// JS exception.
static void CreateDigestContext(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args.GetIsolate(), [&] {
    auto& js = jsg::Lock::from(args.GetIsolate());
    js.withinHandleScope([&] {
      auto name = jsg::JsValue(args[0]);
      auto str = JSG_REQUIRE_NONNULL(name.tryCast<jsg::JsString>(), TypeError,
          "createDigestContext() expects a string argument");
      // The caller has already reduced the option bag to a boolean, so this is
      // ToBoolean on an actual boolean and cannot run user code.
      auto toWellFormed = api::ToWellFormed(args[1]->BooleanValue(args.GetIsolate()));
      args.GetReturnValue().Set(
          v8::Local<v8::Value>(api::createDigestContext(js, str.toString(js), toWellFormed)));
    });
  });
}

// Creates the native write context backing the TypeScript
// FileSystemWritableFileStream. Like CreateDigestContext this allocates and can
// throw -- a DOMException when the file cannot be opened, a TypeError when the
// receiver is not a FileSystemFileHandle -- so it is a plain method rather than a
// fast-API call, and needs liftKj.
static void CreateFileSystemWriteContext(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args.GetIsolate(), [&] {
    auto& js = jsg::Lock::from(args.GetIsolate());
    js.withinHandleScope([&] {
      // The caller has already reduced the option bag to a boolean, so this is
      // ToBoolean on an actual boolean and cannot run user code.
      auto keepExistingData = api::KeepExistingData(args[1]->BooleanValue(args.GetIsolate()));
      args.GetReturnValue().Set(v8::Local<v8::Value>(
          api::createFileSystemWriteContext(js, jsg::JsValue(args[0]), keepExistingData)));
    });
  });
}

// Registers a DOM abort algorithm on an AbortSignal (see abort-bootstrap.h). Plain
// method: it allocates and can throw.
static void AddAbortAlgorithm(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args.GetIsolate(), [&] {
    auto& js = jsg::Lock::from(args.GetIsolate());
    js.withinHandleScope([&] {
      args.GetReturnValue().Set(v8::Local<v8::Value>(
          api::addAbortAlgorithmForBootstrap(js, jsg::JsValue(args[0]), jsg::JsValue(args[1]))));
    });
  });
}

// The EventTarget seam (see event-target-bootstrap.h). Plain methods: they allocate, can throw,
// and some call back into JavaScript.
static void SetListenerCountHook(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args.GetIsolate(), [&] {
    auto& js = jsg::Lock::from(args.GetIsolate());
    js.withinHandleScope([&] {
      api::EventTargetBootstrap::setListenerCountHook(
          js, jsg::JsValue(args[0]), jsg::JsValue(args[1]), jsg::JsValue(args[2]));
    });
  });
}

static void GetEventHandlerAttribute(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args.GetIsolate(), [&] {
    auto& js = jsg::Lock::from(args.GetIsolate());
    js.withinHandleScope([&] {
      args.GetReturnValue().Set(
          v8::Local<v8::Value>(api::EventTargetBootstrap::getEventHandlerAttribute(
              js, jsg::JsValue(args[0]), jsg::JsValue(args[1]))));
    });
  });
}

static void SetEventHandlerAttribute(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args.GetIsolate(), [&] {
    auto& js = jsg::Lock::from(args.GetIsolate());
    js.withinHandleScope([&] {
      api::EventTargetBootstrap::setEventHandlerAttribute(
          js, jsg::JsValue(args[0]), jsg::JsValue(args[1]), jsg::JsValue(args[2]));
    });
  });
}

static void DispatchEvent(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args.GetIsolate(), [&] {
    auto& js = jsg::Lock::from(args.GetIsolate());
    js.withinHandleScope([&] {
      // The caller passes an actual boolean, so this is ToBoolean on a boolean and cannot run
      // user code.
      auto report = api::ReportListenerExceptions(args[2]->BooleanValue(args.GetIsolate()));
      args.GetReturnValue().Set(v8::Local<v8::Value>(api::EventTargetBootstrap::dispatchEvent(
          js, jsg::JsValue(args[0]), jsg::JsValue(args[1]), report)));
    });
  });
}

static void CreateTrustedMessageEvent(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args.GetIsolate(), [&] {
    auto& js = jsg::Lock::from(args.GetIsolate());
    js.withinHandleScope([&] {
      args.GetReturnValue().Set(
          v8::Local<v8::Value>(api::EventTargetBootstrap::createTrustedMessageEvent(js,
              jsg::JsValue(args[0]), jsg::JsValue(args[1]), jsg::JsValue(args[2]),
              jsg::JsValue(args[3]), jsg::JsValue(args[4]))));
    });
  });
}

static void CreateTrustedEvent(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return v8::Local<v8::Value>(
        api::EventTargetBootstrap::createTrustedEvent(js, jsg::JsValue(args[0])));
  });
}

static void CreateTrustedOpenEvent(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return v8::Local<v8::Value>(api::EventTargetBootstrap::createTrustedOpenEvent(js));
  });
}

static void CreateTrustedErrorEvent(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return v8::Local<v8::Value>(
        api::EventTargetBootstrap::createTrustedErrorEvent(js, jsg::JsValue(args[0])));
  });
}

// The request-bound I/O handle (see request-io.h) and the helpers of bootstrap-utils.h. Plain
// methods: they allocate and can throw.
static void CreateRequestIo(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return v8::Local<v8::Value>(api::RequestIo::createForBootstrap(js));
  });
}

static void IsFetcher(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return js.boolean(api::BootstrapUtils::isFetcher(js, jsg::JsValue(args[0])));
  });
}

static void ParseUrl(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return v8::Local<v8::Value>(api::BootstrapUtils::parseUrl(js, jsg::JsValue(args[0])));
  });
}

static void UrlParse(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return v8::Local<v8::Value>(api::UrlBootstrap::parse(
        js, jsg::JsValue(args[0]), jsg::JsValue(args[1]), jsg::JsValue(args[2])));
  });
}

static void UrlCanParse(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return js.boolean(
        api::UrlBootstrap::canParse(js, jsg::JsValue(args[0]), jsg::JsValue(args[1])));
  });
}

static void UrlUpdate(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return v8::Local<v8::Value>(api::UrlBootstrap::update(js, jsg::JsValue(args[0]),
        jsg::JsValue(args[1]), jsg::JsValue(args[2]), jsg::JsValue(args[3])));
  });
}

static void UrlOrigin(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return v8::Local<v8::Value>(api::UrlBootstrap::origin(js, jsg::JsValue(args[0])));
  });
}

static void CreateLegacyUrl(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return v8::Local<v8::Value>(
        api::UrlBootstrap::createLegacyUrl(js, jsg::JsValue(args[0]), jsg::JsValue(args[1])));
  });
}

static void TryCreateLegacyUrl(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return v8::Local<v8::Value>(
        api::UrlBootstrap::tryCreateLegacyUrl(js, jsg::JsValue(args[0]), jsg::JsValue(args[1])));
  });
}

static void CreateLegacySearchParams(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return v8::Local<v8::Value>(api::UrlBootstrap::createLegacySearchParams(js));
  });
}

static void UrlPatternCompile(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return v8::Local<v8::Value>(api::UrlPatternBootstrap::compile(
        js, jsg::JsValue(args[0]), jsg::JsValue(args[1]), jsg::JsValue(args[2])));
  });
}

static void UrlPatternProcessInit(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return v8::Local<v8::Value>(api::UrlPatternBootstrap::processInit(js, jsg::JsValue(args[0])));
  });
}

static void LegacyUrlPatternCompile(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return v8::Local<v8::Value>(api::UrlPatternBootstrap::compileLegacy(
        js, jsg::JsValue(args[0]), jsg::JsValue(args[1]), jsg::JsValue(args[2])));
  });
}

static void LegacyUrlPatternProcessInit(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return v8::Local<v8::Value>(
        api::UrlPatternBootstrap::processInitLegacy(js, jsg::JsValue(args[0])));
  });
}

static void MimeTypeEssence(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return v8::Local<v8::Value>(api::BootstrapUtils::mimeTypeEssence(js, jsg::JsValue(args[0])));
  });
}

static void ReadableStreamState(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    return v8::Local<v8::Value>(
        api::BootstrapUtils::readableStreamState(js, jsg::JsValue(args[0])));
  });
}

// Structured-clones a MessagePort message as postMessage() does: serializes it (which can throw
// a DataCloneError, and runs any getters on it), rejects a SharedArrayBuffer anywhere in it, and
// returns the deserialized copy.
static void CloneMessageData(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    jsg::Serializer ser(js);
    ser.write(js, jsg::JsValue(args[0]));
    auto released = ser.release();
    JSG_REQUIRE(released.sharedArrayBuffers.size() == 0, TypeError,
        "SharedArrayBuffer is unsupported with MessagePort");
    jsg::Deserializer deserializer(js, released);
    return v8::Local<v8::Value>(deserializer.readValue(js));
  });
}

// Calls the function argument, with no arguments and an undefined receiver, as a reaction to an
// already-resolved promise, as jsg::Promise::then() on js.resolvedPromise() does. V8's
// Promise::Then() involves no species lookup, so scheduling runs no user code. If the function
// throws, the reaction's promise rejects, and the rejection is reported as unhandled.
static void QueueReaction(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    auto fn = JSG_REQUIRE_NONNULL(jsg::JsValue(args[0]).tryCast<jsg::JsFunction>(), TypeError,
        "queueReaction() expects a function");
    auto context = js.v8Context();
    auto resolver = jsg::check(v8::Promise::Resolver::New(context));
    jsg::check(resolver->Resolve(context, js.undefined()));
    // The reaction's promise is dropped deliberately; see above.
    std::ignore = jsg::check(resolver->GetPromise()->Then(context, v8::Local<v8::Function>(fn)));
    return v8::Local<v8::Value>(js.undefined());
  });
}

// The getter of a property defined by DefineLazyDataProperty(): requires the bootstrap module
// named by the callback's data and returns its export of the property's name. V8 then replaces
// the property with a data property holding the result, so a read that returns runs this once.
static void LazyDataPropertyGetter(
    v8::Local<v8::Name> name, const v8::PropertyCallbackInfo<v8::Value>& info) {
  jsg::liftKj(info, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(info.GetIsolate());
    auto specifier = jsg::JsString(info.DataV2().As<v8::Value>().As<v8::String>()).toString(js);
    auto exports =
        KJ_ASSERT_NONNULL(requireBootstrapExport(js, specifier).tryCast<jsg::JsObject>());
    return exports.get(js, jsg::JsValue(name));
  });
}

// Defines the property `key` (a string or symbol) of `target` as a writable, configurable data
// property, enumerable if `enumerable` is true, whose value is the `key` export of the bootstrap
// module `specifier`, which is required when the property is first read; reading its descriptor
// counts. Until then nothing is loaded, and nothing but the timing of the load distinguishes it
// from an ordinary data property: it is V8's lazy data property, as JSG_LAZY_INSTANCE_PROPERTY
// defines. A read that throws leaves the property lazy, so the next read tries again.
static void DefineLazyDataProperty(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    auto target = JSG_REQUIRE_NONNULL(jsg::JsValue(args[0]).tryCast<jsg::JsObject>(), TypeError,
        "defineLazyDataProperty() expects an object");
    JSG_REQUIRE(args[1]->IsName(), TypeError, "defineLazyDataProperty() expects a property key");
    JSG_REQUIRE(
        args[2]->IsString(), TypeError, "defineLazyDataProperty() expects a module specifier");
    auto attributes = args[3]->IsTrue() ? v8::PropertyAttribute::None : v8::DontEnum;
    // A property still lazy when a zygote takes its snapshot keeps this getter, and its data, in
    // the heap. V8 serializes an accessor's data into the isolate snapshot, which is shared by
    // every context restored from it and so cannot hold a function of this one: the data is the
    // specifier, and the getter requires the module in the context it is read in.
    jsg::isolateRegisterExternalReference(
        js.v8Isolate, reinterpret_cast<intptr_t>(&LazyDataPropertyGetter));
    KJ_ASSERT(jsg::check(v8::Local<v8::Object>(target)->SetLazyDataProperty(
        js.v8Context(), args[1].As<v8::Name>(), &LazyDataPropertyGetter, args[2], attributes)));
    return js.v8Undefined();
  });
}

// The exact UTF-8 byte length of a string as TextEncoder encodes it: a lone
// surrogate counts the 3 bytes of U+FFFD. V8 computes it with simdutf. A plain
// method rather than a fast-API call, because V8 flattens the string first,
// which can allocate.
static void Utf8Length(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    auto str = JSG_REQUIRE_NONNULL(
        jsg::JsValue(args[0]).tryCast<jsg::JsString>(), TypeError, "utf8Length() expects a string");
    return js.num(static_cast<double>(str.utf8Length(js)));
  });
}

// TextEncoder.encode() for the TypeScript TextEncoderStream: see api::encodeUtf8Chunk.
static void EncodeUtf8(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    auto str = JSG_REQUIRE_NONNULL(
        jsg::JsValue(args[0]).tryCast<jsg::JsString>(), TypeError, "encodeUtf8() expects a string");
    return api::encodeUtf8Chunk(js, str);
  });
}

// The V8 private symbol of the given name (v8::Private::ForApi), as a value the bootstrap
// scripts use as a property key: `object[key]` reads or writes the slot that
// jsg::JsObject::getPrivate() and setPrivate() reach with that name, without a call into C++
// (v8::Object::GetPrivate() is that same property read). A private key is never inherited,
// reflected or seen by Proxy traps, and only code holding the key can name it. The API does
// not type a private symbol as a v8::Value, so it is handed over as a template property, as
// Node.js hands over its own.
static void GetPrivateSymbol(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    auto name = JSG_REQUIRE_NONNULL(jsg::JsValue(args[0]).tryCast<jsg::JsString>(), TypeError,
        "getPrivateSymbol() expects a string argument");
    v8::Local<v8::String> slot = js.strIntern("key"_kj);
    auto holder = v8::ObjectTemplate::New(js.v8Isolate);
    holder->Set(slot, v8::Private::ForApi(js.v8Isolate, name));
    return jsg::check(jsg::check(holder->NewInstance(js.v8Context()))->Get(js.v8Context(), slot));
  });
}

// The structured clone of a value, backing the TypeScript structuredClone(). The
// caller has already converted the options, so the second argument is undefined
// or a dense array it built itself, whose elements are objects to transfer;
// reading them cannot run user code. Serialization can run user code (getters)
// and throws a DataCloneError for an uncloneable value. Plain method: it
// allocates and can throw.
static void StructuredClone(const v8::FunctionCallbackInfo<v8::Value>& args) {
  jsg::liftKj(args, [&]() -> v8::Local<v8::Value> {
    auto& js = jsg::Lock::from(args.GetIsolate());
    kj::Maybe<kj::Array<jsg::JsValue>> transfer;
    KJ_IF_SOME(list, jsg::JsValue(args[1]).tryCast<jsg::JsArray>()) {
      auto builder = kj::heapArrayBuilder<jsg::JsValue>(list.size());
      for (auto i: kj::zeroTo(list.size())) {
        builder.add(list.get(js, i));
      }
      transfer = builder.finish();
    }
    return jsg::JsValue(args[0]).structuredClone(js, kj::mv(transfer));
  });
}

static const v8::CFunction fast_mark_promise_handled_ =
    v8::CFunction::Make(MarkPromiseHandledFastApi);

v8::Local<v8::Value> makeMethod(jsg::Lock& js,
    v8::FunctionCallback callback,
    v8::SideEffectType sideEffect,
    const v8::CFunction* c_function = nullptr) {
  jsg::isolateRegisterExternalReference(js.v8Isolate, reinterpret_cast<intptr_t>(callback));
  if (c_function != nullptr) {
    jsg::isolateRegisterExternalReference(js.v8Isolate, reinterpret_cast<intptr_t>(c_function));
  }
  return jsg::check(v8::FunctionTemplate::New(js.v8Isolate, callback, v8::Local<v8::Value>(),
      v8::Local<v8::Signature>(), 0, v8::ConstructorBehavior::kThrow, sideEffect, c_function)
                        ->GetFunction(js.v8Context()));
}

v8::Local<v8::Value> getFastMethodNoSideEffect(
    jsg::Lock& js, v8::FunctionCallback callback, const v8::CFunction* c_function) {
  return makeMethod(js, callback, v8::SideEffectType::kHasNoSideEffect, c_function);
}

v8::Local<v8::Value> getFastMethod(
    jsg::Lock& js, v8::FunctionCallback callback, const v8::CFunction* c_function) {
  return makeMethod(js, callback, v8::SideEffectType::kHasSideEffect, c_function);
}

v8::Local<v8::Value> getMethod(jsg::Lock& js, v8::FunctionCallback callback) {
  return makeMethod(js, callback, v8::SideEffectType::kHasSideEffect);
}

// Creates an object with methods for performing fast type checks on JS values.
// Because we are not fully bootstrapped at this point, we don't want to rely
// on jsg::Object and the type wrapper system, etc. Instead, just use a plain
// object with some properties set. (Members implemented in the api layer, like
// newCompressionCodec, keep their knowledge there and are wired here by name;
// their callbacks may use the type wrapper at CALL time -- the isolate is fully
// set up by then -- through jsg::Lock's type-handler lookup.)
jsg::JsRef<jsg::JsObject> createUtilsObject(jsg::Lock& js) {
  static constexpr std::string_view names[] = {
#define V(Name) "is" #Name,
    // NOLINTNEXTLINE(bugprone-suspicious-missing-comma)
    VALUE_METHOD_MAP(V)
#undef V
        "isAnyArrayBuffer",
    "markPromiseHandled",
    "getApiSymbol",
    "newCompressionCodec",
    "createDigestContext",
    "createFileSystemWriteContext",
    "addAbortAlgorithm",
    "utf8Length",
    "encodeUtf8",
    "getPrivateSymbol",
    "setListenerCountHook",
    "getEventHandlerAttribute",
    "setEventHandlerAttribute",
    "dispatchEvent",
    "createTrustedMessageEvent",
    "createTrustedEvent",
    "cloneMessageData",
    "queueReaction",
    "defineLazyDataProperty",
    "createTrustedOpenEvent",
    "createTrustedErrorEvent",
    "createRequestIo",
    "isFetcher",
    "parseUrl",
    "urlParse",
    "urlCanParse",
    "urlUpdate",
    "urlOrigin",
    "createLegacyUrl",
    "tryCreateLegacyUrl",
    "createLegacySearchParams",
    "urlPatternCompile",
    "urlPatternProcessInit",
    "legacyUrlPatternCompile",
    "legacyUrlPatternProcessInit",
    "mimeTypeEssence",
    "readableStreamState",
    "DOMExceptionBase",
    "structuredClone",
  };
  auto tmpl = v8::DictionaryTemplate::New(js.v8Isolate, names);
  v8::MaybeLocal<v8::Value> values[] = {
#define V(Name) getFastMethodNoSideEffect(js, Is##Name, &fast_is_##Name##_),
    VALUE_METHOD_MAP(V)
#undef V
        getFastMethodNoSideEffect(js, IsAnyArrayBuffer, &fast_is_any_array_buffer_),
    getFastMethod(js, MarkPromiseHandled, &fast_mark_promise_handled_),
    getMethod(js, GetApiSymbol),
    getMethod(js, api::newCompressionCodecCallback),
    getMethod(js, CreateDigestContext),
    getMethod(js, CreateFileSystemWriteContext),
    getMethod(js, AddAbortAlgorithm),
    getMethod(js, Utf8Length),
    getMethod(js, EncodeUtf8),
    getMethod(js, GetPrivateSymbol),
    getMethod(js, SetListenerCountHook),
    getMethod(js, GetEventHandlerAttribute),
    getMethod(js, SetEventHandlerAttribute),
    getMethod(js, DispatchEvent),
    getMethod(js, CreateTrustedMessageEvent),
    getMethod(js, CreateTrustedEvent),
    getMethod(js, CloneMessageData),
    getMethod(js, QueueReaction),
    getMethod(js, DefineLazyDataProperty),
    getMethod(js, CreateTrustedOpenEvent),
    getMethod(js, CreateTrustedErrorEvent),
    getMethod(js, CreateRequestIo),
    getMethod(js, IsFetcher),
    getMethod(js, ParseUrl),
    getMethod(js, UrlParse),
    getMethod(js, UrlCanParse),
    getMethod(js, UrlUpdate),
    getMethod(js, UrlOrigin),
    getMethod(js, CreateLegacyUrl),
    getMethod(js, TryCreateLegacyUrl),
    getMethod(js, CreateLegacySearchParams),
    getMethod(js, UrlPatternCompile),
    getMethod(js, UrlPatternProcessInit),
    getMethod(js, LegacyUrlPatternCompile),
    getMethod(js, LegacyUrlPatternProcessInit),
    getMethod(js, MimeTypeEssence),
    getMethod(js, ReadableStreamState),
    v8::Local<v8::Value>(jsg::getBootstrapDomExceptionBase(js)),
    getMethod(js, StructuredClone),
  };

  static_assert(kj::arrayPtr(names).size() == kj::arrayPtr(values).size());

  return jsg::JsObject(tmpl->NewInstance(js.v8Context(), values)).addRef(js);
}

// The pseudo-global variables that scripts access directly (e.g. `require`, `compatFlags`,
// `primordials`). Each script is compiled as a function taking them as parameters, in this
// order. gen-compile-cache (src/rust/gen-compile-cache/cxx-bridge.c++) compiles the scripts
// with the same parameter list, which must match for V8 to accept the cache.
//
// They are parameters rather than the properties of a context extension object because an
// extension object makes V8 resolve every free variable in the script (`undefined` included)
// with a dynamic lookup slot: a runtime call that walks the context chain on every access,
// even in optimized code.
constexpr kj::StringPtr PSEUDO_GLOBAL_NAMES[] = {
  "require"_kj,
  "compatFlags"_kj,
  "autogates"_kj,
  "module"_kj,
  "exports"_kj,
  "primordials"_kj,
  "utils"_kj,
};
constexpr size_t PSEUDO_GLOBAL_COUNT = kj::size(PSEUDO_GLOBAL_NAMES);

// The values of PSEUDO_GLOBAL_NAMES for a script, in the same order.
v8::LocalVector<v8::Value> pseudoGlobalValues(jsg::Lock& js,
    BootstrapState& state,
    const jsg::JsObject& moduleObj,
    const jsg::JsObject& exportsObj,
    kj::Maybe<jsg::JsFunction> require) {
  v8::LocalVector<v8::Value> values(js.v8Isolate,
      {
        js.v8Undefined(),  // require
        js.v8Undefined(),  // compatFlags
        v8::Local<v8::Value>(state.autogatesObj.getHandle(js)),
        v8::Local<v8::Value>(moduleObj),
        v8::Local<v8::Value>(exportsObj),
        js.v8Undefined(),  // primordials
        v8::Local<v8::Value>(state.utilsObj.getHandle(js)),
      });
  KJ_ASSERT(values.size() == PSEUDO_GLOBAL_COUNT);

  KJ_IF_SOME(fn, require) {
    values[0] = v8::Local<v8::Value>(fn);
  }
  KJ_IF_SOME(compatFlags, state.compatFlagsObj) {
    values[1] = v8::Local<v8::Value>(compatFlags.getHandle(js));
  }
  KJ_IF_SOME(primordials, state.primordials) {
    values[5] = v8::Local<v8::Value>(primordials.getHandle(js));
  }
  return values;
}

// Retrieve the BootstrapState from the given context's embedder data, if it has one. Only
// runPerIsolateBootstrap() stores a jsg::BootstrapModules in the slot of a context it runs in,
// since this downcasts what the slot holds; a test supplies modules of its own with
// setBootstrapExportForTest().
kj::Maybe<BootstrapState&> tryGetBootstrapState(v8::Local<v8::Context> context) {
  KJ_IF_SOME(modules,
      jsg::getAlignedPointerFromEmbedderData<jsg::BootstrapModules>(
          context, jsg::ContextPointerSlot::BOOTSTRAP_STATE)) {
    return kj::downcast<BootstrapState>(modules);
  }
  return kj::none;
}

// Retrieve the BootstrapState from the current context's embedder data.
BootstrapState& getBootstrapState(jsg::Lock& js) {
  return KJ_REQUIRE_NONNULL(
      tryGetBootstrapState(js.v8Context()), "per-isolate bootstrap state not initialized");
}

// Modules whose functions are invoked during per-context startup wiring (plus embedder scripts).
// Compiling these eagerly in buildBaselineSnapshot() eliminates CompileLazy on isolate startup
// without bloating TrustedSpace in the startup snapshot with hundreds of cold runtime-API methods
// (such as webstreams/* method bodies) that are never called during isolate creation.
bool shouldEagerCompileInBaseline(kj::StringPtr normalized, bool isEmbedderScript) {
  if (isEmbedderScript) return true;
  return normalized == "wire-context"_kj || normalized == "primordials-js"_kj ||
      normalized == "wasm-instantiate-shim"_kj || normalized == "dom-exception"_kj ||
      normalized == "url/url"_kj || normalized == "urlpattern/urlpattern"_kj ||
      normalized == "structured-clone"_kj || normalized == "eventsource/eventsource"_kj ||
      normalized == "runtime-class"_kj || normalized == "events/event-target-internals"_kj;
}
// When `eagerCompile` is true (used once per process inside buildBaselineSnapshot()), every inner
// function is compiled eagerly into bytecode so FunctionCodeHandling::kKeep eliminates CompileLazy
// on restored isolates.
jsg::JsFunction compileBootstrapScript(jsg::Lock& js,
    kj::StringPtr normalized,
    jsg::Module::Reader script,
    const jsg::ModuleSource* moduleSource,
    bool eagerCompile = false) {
  auto source = script.getSrc();
  auto& observer = jsg::IsolateBase::from(js.v8Isolate).getObserver();
  // Without a ModuleSource, V8 gets the script's bytes as a one-byte (Latin-1) string
  // without transcoding, so the text a script produces at runtime must be ASCII: a
  // non-ASCII character in a string literal reads as its UTF-8 bytes. A ModuleSource
  // transcodes, which for ASCII source is the identity.
  v8::Local<v8::String> sourceStr;
  if (moduleSource != nullptr) {
    sourceStr = jsg::newExternalString(js, moduleSource->get(source.asChars()));
  } else {
#if KJ_HAS_COMPILER_FEATURE(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
    // Under LSAN, use a copied string to avoid false-positive leak reports.
    // The ExternString resource is properly owned by V8 (freed via Dispose()
    // on GC), but LSAN can't trace through V8's heap to see it.
    sourceStr = js.str(source);
#else
    // Use strExtern to avoid copying: the source data lives in the static
    // capnp bundle for the lifetime of the process.
    sourceStr = js.strExtern(source.asChars());
#endif
  }
  auto originName = kj::str("workerd:per-isolate/", normalized);
  v8::ScriptOrigin origin(js.strIntern(originName));

  // Use the compile cache from the bundle if available and compatible. Static compile caches are
  // generated against V8's embedded default snapshot, so skip them in isolates booted from a
  // custom snapshot (where CompatibilityCheck would reject them) and during eager baseline
  // compilation (kEagerCompile and kConsumeCodeCache are mutually exclusive in V8).
  v8::ScriptCompiler::CachedData* cachedData = nullptr;
  auto options = eagerCompile ? v8::ScriptCompiler::kEagerCompile
                              : v8::ScriptCompiler::kNoCompileOptions;
  auto compileCache = script.getCompileCache();
  if (!eagerCompile && !js.isStartingFromSnapshot() && !js.isStartingFromBaselineSnapshot() &&
      compileCache.size() > 0 && compileCache.begin() != nullptr) {
    // V8 takes ownership of the CachedData instance but not the underlying
    // buffer (which lives in the static capnp bundle data).
    cachedData = new v8::ScriptCompiler::CachedData(
        compileCache.begin(), compileCache.size(), v8::ScriptCompiler::CachedData::BufferNotOwned);
    auto check = cachedData->CompatibilityCheck(js.v8Isolate);
    if (check != v8::ScriptCompiler::CachedData::kSuccess) {
      LOG_WARNING_ONCE("NOSENTRY per-isolate bootstrap compile cache failed its "
                       "compatibility check; scripts will be compiled from source",
          normalized, static_cast<int>(check));
      observer.onCompileCacheRejected(js.v8Isolate);
      delete cachedData;
      cachedData = nullptr;
    }
  }

  // Source takes ownership of cachedData. Do not use cachedData after this.
  v8::ScriptCompiler::Source compilerSource(sourceStr, origin, cachedData);

  auto maybeCached = compilerSource.GetCachedData();
  if (maybeCached != nullptr && !maybeCached->rejected) {
    options = v8::ScriptCompiler::kConsumeCodeCache;
  }

  v8::Local<v8::String> params[PSEUDO_GLOBAL_COUNT];
  for (auto i: kj::indices(params)) {
    params[i] = js.strIntern(PSEUDO_GLOBAL_NAMES[i]);
  }
  auto fn = jsg::JsFunction(jsg::check(v8::ScriptCompiler::CompileFunction(
      js.v8Context(), &compilerSource, PSEUDO_GLOBAL_COUNT, params, 0, nullptr, options)));

  if (options == v8::ScriptCompiler::kConsumeCodeCache) {
    if (compilerSource.GetCachedData()->rejected) {
      LOG_WARNING_ONCE("NOSENTRY per-isolate bootstrap compile cache was rejected by V8 "
                       "while compiling; scripts will be compiled from source",
          normalized);
      observer.onCompileCacheRejected(js.v8Isolate);
    } else {
      observer.onCompileCacheFound(js.v8Isolate);
    }
  }

  return fn;
}

// Which scripts a require() function resolves: workerd's only, or the embedder's as well.
enum class RequireScope { WORKERD, EMBEDDER };

// The implementation of both require() functions injected into bootstrap scripts.
void requireImpl(const v8::FunctionCallbackInfo<v8::Value>& info, RequireScope scope) {
  jsg::liftKj(info.GetIsolate(), [&] {
    auto& js = jsg::Lock::from(info.GetIsolate());
    auto& state = getBootstrapState(js);

    js.withinHandleScope([&] {
      KJ_REQUIRE(info.Length() >= 1 && info[0]->IsString(), "require() expects a string argument");

      auto specifierUtf8 = jsg::JsValue(info[0]).toString(js);
      auto normalized = normalizeSpecifier(specifierUtf8);

      // workerd's scripts cannot see the embedder's, even once they are cached.
      bool isEmbedderScript = false;
      KJ_IF_SOME(embedder, state.embedder) {
        isEmbedderScript = embedder.isInNamespace(normalized);
      }
      KJ_REQUIRE(!isEmbedderScript || scope == RequireScope::EMBEDDER,
          "per-isolate bootstrap: workerd's scripts cannot require the embedder's", normalized);

      // Check cache (both per-context additions and any snapshot-stashed cache object).
      KJ_IF_SOME(cached, state.tryGetLoadedExports(js, normalized)) {
        info.GetReturnValue().Set(v8::Local<v8::Value>(cached));
        return;
      }

      // Cycle detection.
      KJ_REQUIRE(!state.loading.contains(normalized),
          "circular dependency in per-isolate bootstrap", normalized);

      // Look up the script in its bundle, and the require() it gets: the embedder's scripts
      // resolve everything, workerd's only workerd's, and primordials-js nothing.
      kj::Maybe<jsg::Module::Reader> maybeScript;
      kj::Maybe<jsg::JsFunction> scriptRequire;
      if (isEmbedderScript) {
        maybeScript = KJ_ASSERT_NONNULL(state.embedder).tryGetScript(normalized);
        scriptRequire = KJ_ASSERT_NONNULL(state.embedderRequireFn).getHandle(js);
      } else {
        KJ_IF_SOME(found, state.scripts.find(normalized)) {
          maybeScript = found;
        }
        if (normalized != "primordials-js") {
          scriptRequire = state.requireFn.getHandle(js);
        }
      }
      auto script =
          KJ_REQUIRE_NONNULL(maybeScript, "per-isolate bootstrap: unknown script", normalized);

      // Mark as loading.
      state.loading.insert(kj::str(normalized));
      KJ_DEFER(state.loading.eraseMatch(normalized));

      // Create the module and exports objects for this script.
      auto moduleObj = js.obj();
      auto exportsObj = js.obj();
      moduleObj.set(js, "exports"_kj, exportsObj);

      auto args = pseudoGlobalValues(js, state, moduleObj, exportsObj, scriptRequire);

      // Use the pre-compiled function from the baseline snapshot when available, otherwise compile
      // the script on demand.
      kj::Maybe<jsg::JsFunction> maybeFn;
      KJ_IF_SOME(precompiled, state.compiledScripts.find(normalized)) {
        maybeFn = precompiled.getHandle(js);
      } else KJ_IF_SOME(stashedCompiled, state.stashedCompiledScripts) {
        auto obj = stashedCompiled.getHandle(js);
        if (obj.has(js, normalized)) {
          maybeFn = KJ_REQUIRE_NONNULL(JSG_TRY_CAST_FUNCTION(obj.get(js, normalized)),
              "baseline bootstrap compiled script entry is not a function");
        }
      }
      auto fn = maybeFn.orDefault([&]() {
        bool eager = state.eagerCompile &&
            shouldEagerCompileInBaseline(normalized, isEmbedderScript);
        return compileBootstrapScript(js, normalized, script, state.moduleSource, eager);
      });

      // Execute the script.
      fn.call(js, js.undefined(), args);

      // Read back module.exports (script may have reassigned it).
      auto result = moduleObj.get(js, "exports"_kj);

      // Cache the result.
      state.cache.insert(kj::str(normalized), result.addRef(js));

      info.GetReturnValue().Set(v8::Local<v8::Value>(result));
    });
  });
}

// The V8 callback backing the require() function injected into workerd's bootstrap scripts.
void requireCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  requireImpl(info, RequireScope::WORKERD);
}

// The V8 callback backing the require() function injected into the embedder's bootstrap scripts.
void embedderRequireCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
  requireImpl(info, RequireScope::EMBEDDER);
}

jsg::JsRef<jsg::JsFunction> newRequireFunction(jsg::Lock& js, v8::FunctionCallback callback) {
  // No v8::External needed: the callback reads the state from the context embedder slot.
  jsg::isolateRegisterExternalReference(js.v8Isolate, reinterpret_cast<intptr_t>(callback));
  return jsg::JsFunction(jsg::check(v8::Function::New(js.v8Context(), callback))).addRef(js);
}

// Populates `obj` with { flagName: true } entries for enabled compat flags.
// Disabled flags are omitted — scripts check with `if ('flag_name' in compatFlags)`
// or `compatFlags['flag_name'] === true`.
void populateCompatFlagsObject(
    jsg::Lock& js, jsg::JsObject obj, CompatibilityFlags::Reader flags) {
  auto dynamicFlags = capnp::toDynamic(flags);
  auto v8Obj = v8::Local<v8::Object>(obj);
  auto context = js.v8Context();
  auto v8True = v8::True(js.v8Isolate);

  for (auto& entry: getCompatFlagFields()) {
    if (dynamicFlags.get(entry.field).as<bool>()) {
      jsg::check(v8Obj->CreateDataProperty(
          context, v8::Local<v8::String>(js.strIntern(entry.enableFlag)), v8True));
    }
  }
}

jsg::JsRef<jsg::JsObject> buildCompatFlagsObject(jsg::Lock& js, CompatibilityFlags::Reader flags) {
  auto obj = js.objNoProto();
  populateCompatFlagsObject(js, obj, flags);
  return obj.addRef(js);
}

// Build a JS object with { gateName: true } entries for enabled autogates.
// Disabled autogates are omitted.
jsg::JsRef<jsg::JsObject> buildAutogatesObject(jsg::Lock& js) {
  auto obj = js.obj();

  for (util::AutogateKey key: util::getAutogateKeys()) {
    if (util::Autogate::isEnabled(key)) {
      auto name = kj::str(key);
      obj.set(js, name, js.boolean(true));
    }
  }

  return obj.addRef(js);
}

// Heap-allocates the per-context state and stores it in the BOOTSTRAP_STATE pointer slot. The
// slot holds a raw pointer, so this uses `new` rather than kj::heap; cleanupPerIsolateBootstrap()
// frees it.
BootstrapState* allocateBootstrapState(jsg::Lock& js,
    const kj::HashMap<kj::StringPtr, jsg::Module::Reader>& scripts,
    kj::Maybe<const EmbedderBootstrap&> embedder,
    const jsg::ModuleSource* moduleSource) {
  auto* state = new BootstrapState(scripts, embedder, moduleSource);
#if KJ_HAS_COMPILER_FEATURE(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
  // Tell LSAN to ignore this allocation. The pointer is stored exclusively
  // in V8's embedder data (BOOTSTRAP_STATE slot), which LSAN can't trace.
  // The allocation is properly freed by cleanupPerIsolateBootstrap() when
  // the context is disposed. Instances of BootstrapState are specific to
  // each v8::Context and cannot be shared, and since we might create multiple
  // contexts over a Worker::Isolate lifetime, it's not all that efficient to
  // store them in a field in Worker::Isolate::Impl.
  __lsan_ignore_object(state);
#endif
  jsg::setAlignedPointerInEmbedderData(js.v8Context(), jsg::ContextPointerSlot::BOOTSTRAP_STATE,
      static_cast<jsg::BootstrapModules*>(state));
  return state;
}

// Property names of the object a zygote or baseline builder pins into the startup snapshot
// (SnapshotArtifact::bootstrapStateIndex or baselineBootstrapStateIndex): the bootstrap's
// JS-held state.
constexpr kj::StringPtr STASH_REQUIRE = "require"_kj;
// Present only when the snapshot ran with an embedder bootstrap.
constexpr kj::StringPtr STASH_EMBEDDER_REQUIRE = "embedderRequire"_kj;
constexpr kj::StringPtr STASH_COMPAT_FLAGS = "compatFlags"_kj;
constexpr kj::StringPtr STASH_AUTOGATES = "autogates"_kj;
constexpr kj::StringPtr STASH_UTILS = "utils"_kj;
constexpr kj::StringPtr STASH_PRIMORDIALS = "primordials"_kj;
// A null-prototype object mapping each normalized specifier in the require() cache to its
// module.exports.
constexpr kj::StringPtr STASH_CACHE = "cache"_kj;
// Baseline snapshot only: a null-prototype object mapping each normalized specifier to its
// pre-compiled v8::Function (used for embedder scripts).
constexpr kj::StringPtr STASH_COMPILED_SCRIPTS = "compiledScripts"_kj;
// Baseline snapshot only: the pre-evaluated wireFromBaseline(global) function from wire-context.
constexpr kj::StringPtr STASH_WIRE_FN = "wireFn"_kj;

// Returns the state a zygote pinned into the snapshot `context` was deserialized from. V8 hands
// a pinned value out exactly once, so the restore cannot run twice. Only valid in an isolate
// started from a per-worker snapshot.
kj::Maybe<jsg::JsObject> tryTakeSnapshotBootstrapState(
    jsg::Lock& js, v8::Local<v8::Context> context) {
  auto& isolateBase = jsg::IsolateBase::from(js.v8Isolate);
  KJ_IF_SOME(idx, isolateBase.finalizedSnapshotArtifact().bootstrapStateIndex) {
    return jsg::JsObject(isolateBase.extractOnceFromSnapshot(context, idx));
  }
  return kj::none;
}

// Returns the baseline bootstrap state pinned into the baseline snapshot `context` was
// deserialized from. Valid in both BaselineSnapshot isolates and zygotes chaining off a baseline.
kj::Maybe<jsg::JsObject> tryTakeBaselineBootstrapState(
    jsg::Lock& js, v8::Local<v8::Context> context) {
  auto& isolateBase = jsg::IsolateBase::from(js.v8Isolate);
  KJ_IF_SOME(idx, isolateBase.baselineSnapshotArtifact().baselineBootstrapStateIndex) {
    return jsg::JsObject(isolateBase.extractOnceFromSnapshot(context, idx));
  }
  return kj::none;
}

// Rebuilds the C++ view of the bootstrap from the state a zygote pinned into its snapshot.
// Nothing is loading. The embedder is the restoring isolate's, which must be the zygote's: its
// scripts' exports are in the cache under their prefixed specifiers, and its require() is
// restored only when the state has an embedder.
void restoreBootstrapState(jsg::Lock& js, BootstrapState* state, jsg::JsObject bag) {
  auto takeObject = [&](kj::StringPtr name) -> jsg::JsRef<jsg::JsObject> {
    auto maybeObj = JSG_TRY_CAST_OBJECT(bag.get(js, name));
    auto& obj = KJ_REQUIRE_NONNULL(maybeObj, "snapshot bootstrap state is missing an object", name);
    return obj.addRef(js);
  };
  state->compatFlagsObj = takeObject(STASH_COMPAT_FLAGS);
  state->autogatesObj = takeObject(STASH_AUTOGATES);
  state->utilsObj = takeObject(STASH_UTILS);
  {
    auto maybeFn = JSG_TRY_CAST_FUNCTION(bag.get(js, STASH_REQUIRE));
    auto& fn = KJ_REQUIRE_NONNULL(maybeFn, "snapshot bootstrap state is missing require()");
    state->requireFn = fn.addRef(js);
  }
  if (state->embedder != kj::none) {
    auto maybeFn = JSG_TRY_CAST_FUNCTION(bag.get(js, STASH_EMBEDDER_REQUIRE));
    auto& fn =
        KJ_REQUIRE_NONNULL(maybeFn, "snapshot bootstrap state is missing the embedder's require()");
    state->embedderRequireFn = fn.addRef(js);
  }
  state->primordials = bag.get(js, STASH_PRIMORDIALS).addRef(js);
  state->stashedCache = takeObject(STASH_CACHE);
}

// Runs the per-context wiring phase: loads `primordials` (extending pure-JS primordials with this
// context's JSG globals), runs `main`, and runs the embedder entry point if present.
void runPerContextBootstrapWiring(jsg::Lock& js, BootstrapState* state) {
  // Load primordials first — this must happen before any other script so that
  // built-in prototype methods are captured before anything could pollute them.
  // The result is cached in state and passed as the `primordials` parameter to every
  // subsequent script.
  {
    TRACE_EVENT("workerd", "PerIsolateBootrap::primordials");
    auto result =
        state->requireFn.getHandle(js).call(js, js.undefined(), js.strIntern("primordials"_kj));
    state->primordials = result.addRef(js);
  }

  {
    TRACE_EVENT("workerd", "PerIsolateBootstrap::main");
    // Run the entry point. This synchronously executes main.js, which may
    // require() other scripts. All execution is synchronous.
    state->requireFn.getHandle(js).call(js, js.undefined(), js.strIntern("main"_kj));
  }

  KJ_IF_SOME(e, state->embedder) {
    TRACE_EVENT("workerd", "PerIsolateBootstrap::embedderMain");
    // The embedder's entry point runs once workerd's globals are installed, and before the
    // WeakRef globals are removed, so that its scripts see the same environment as workerd's.
    KJ_ASSERT_NONNULL(state->embedderRequireFn)
        .getHandle(js)
        .call(js, js.undefined(), js.strIntern(e.getEntryPoint()));
  }
}

// Rebuilds the flag-independent bootstrap state from a process-wide baseline snapshot, populates
// the shared compatFlags object in place for this context's flags, and runs the per-context wiring
// phase.
void bootstrapFromBaselineSnapshot(
    jsg::Lock& js, BootstrapState* state, jsg::JsObject bag, CompatibilityFlags::Reader flags) {
  auto takeObject = [&](kj::StringPtr name) -> jsg::JsRef<jsg::JsObject> {
    auto maybeObj = JSG_TRY_CAST_OBJECT(bag.get(js, name));
    auto& obj = KJ_REQUIRE_NONNULL(maybeObj, "baseline bootstrap state is missing an object", name);
    return obj.addRef(js);
  };
  // Populate the same compatFlags object that pre-evaluated baseline modules closed over.
  auto compatFlags = takeObject(STASH_COMPAT_FLAGS);
  populateCompatFlagsObject(js, compatFlags.getHandle(js), flags);
  state->compatFlagsObj = kj::mv(compatFlags);
  state->autogatesObj = takeObject(STASH_AUTOGATES);
  state->utilsObj = takeObject(STASH_UTILS);
  {
    auto maybeFn = JSG_TRY_CAST_FUNCTION(bag.get(js, STASH_REQUIRE));
    auto& fn = KJ_REQUIRE_NONNULL(maybeFn, "baseline bootstrap state is missing require()");
    state->requireFn = fn.addRef(js);
  }
  if (state->embedder != kj::none) {
    KJ_IF_SOME(fn, JSG_TRY_CAST_FUNCTION(bag.get(js, STASH_EMBEDDER_REQUIRE))) {
      state->embedderRequireFn = fn.addRef(js);
    } else {
      state->embedderRequireFn = newRequireFunction(js, embedderRequireCallback);
    }
  }

  state->stashedCache = takeObject(STASH_CACHE);
  state->stashedCompiledScripts = takeObject(STASH_COMPILED_SCRIPTS);

  {
    TRACE_EVENT("workerd", "PerIsolateBootstrap::wireFromBaseline");
    auto maybeWireFn = JSG_TRY_CAST_FUNCTION(bag.get(js, STASH_WIRE_FN));
    auto& wireFn =
        KJ_REQUIRE_NONNULL(maybeWireFn, "baseline bootstrap state is missing wireFromBaseline()");
    auto primed = wireFn.call(js, js.undefined(), js.global());
    state->primordials = primed.addRef(js);
    state->cache.insert(kj::str("primordials"), primed.addRef(js));
  }

  KJ_IF_SOME(e, state->embedder) {
    TRACE_EVENT("workerd", "PerIsolateBootstrap::embedderMain");
    KJ_ASSERT_NONNULL(state->embedderRequireFn)
        .getHandle(js)
        .call(js, js.undefined(), js.strIntern(e.getEntryPoint()));
  }
}

// Bootstraps a context built from scratch: creates require(), loads primordials and runs main.
void bootstrapFromScripts(jsg::Lock& js, BootstrapState* state, CompatibilityFlags::Reader flags) {
  // Build the compat flags and autogates objects.
  state->compatFlagsObj = buildCompatFlagsObject(js, flags);
  state->autogatesObj = buildAutogatesObject(js);
  state->utilsObj = createUtilsObject(js);

  state->requireFn = newRequireFunction(js, requireCallback);
  if (state->embedder != kj::none) {
    state->embedderRequireFn = newRequireFunction(js, embedderRequireCallback);
  }

  runPerContextBootstrapWiring(js, state);
}

// Populates `context` (a plain V8 context inside `buildBaselineSnapshot()`) with the
// compat-flag-independent bootstrap state, pins it into the snapshot artifact, and releases the
// C++ BootstrapState.
void populateBaselineBootstrapContext(jsg::Lock& js,
    v8::Local<v8::Context> context,
    const kj::HashMap<kj::StringPtr, jsg::Module::Reader>& scripts,
    kj::Maybe<const EmbedderBootstrap&> embedder,
    const jsg::ModuleSource* moduleSource) {
  JSG_TRY(js) {
    auto* state = allocateBootstrapState(js, scripts, embedder, moduleSource);
    KJ_DEFER(cleanupPerIsolateBootstrap(js, context));
    // Compile every bootstrap script eagerly so FunctionCodeHandling::kKeep bakes all inner
    // function bytecode into the baseline snapshot, eliminating CompileLazy on restored isolates.
    state->eagerCompile = true;

    // Empty mutable null-prototype compatFlags object closed over by pre-evaluated modules and
    // populated in place per context on restore.
    state->compatFlagsObj = js.objNoProto().addRef(js);
    state->autogatesObj = buildAutogatesObject(js);
    state->utilsObj = createUtilsObject(js);
    state->requireFn = newRequireFunction(js, requireCallback);
    if (embedder != kj::none) {
      state->embedderRequireFn = newRequireFunction(js, embedderRequireCallback);
    }

    auto requireModule = [&](kj::StringPtr specifier) {
      return state->requireFn.getHandle(js).call(js, js.undefined(), js.strIntern(specifier));
    };

    // Pre-evaluate pure-JS primordials first, then `wire-context`, which transitively requires and
    // pre-evaluates every compat-flag-independent bootstrap module (serialization, runtime-class,
    // events/event-target-internals, wasm-instantiate-shim, dom-exception, webstreams/*,
    // DigestStream, FileSystemWritableFileStream, EventSource, and autogate-enabled url/*,
    // urlpattern, structured-clone) and pre-builds all frozen PropertyDescriptorMaps.
    state->primordials = requireModule("primordials-js"_kj).addRef(js);
    auto wireExports = KJ_REQUIRE_NONNULL(
        JSG_TRY_CAST_OBJECT(requireModule("wire-context"_kj)),
        "wire-context module did not export an object");
    auto wireFn = KJ_REQUIRE_NONNULL(
        JSG_TRY_CAST_FUNCTION(wireExports.get(js, "wireFromBaseline"_kj)),
        "wire-context module is missing wireFromBaseline()");

    // Pre-compile embedder scripts eagerly so they run without parsing on restore. Lazy /
    // test-only workerd scripts (e.g. messagechannel, testing/cloneable-test-box) are intentionally
    // left out to keep the baseline startup heap small and are compiled on demand if ever used.
    KJ_IF_SOME(e, embedder) {
      for (auto& entry: e.getScripts()) {
        if (state->cache.find(entry.key) != kj::none) continue;
        state->compiledScripts.insert(kj::str(entry.key),
            compileBootstrapScript(js, entry.key, entry.value, moduleSource, true).addRef(js));
      }
    }

    KJ_REQUIRE(state->loading.size() == 0, "require() still in progress at baseline snapshot");
    auto cache = js.objNoProto();
    for (auto& entry: state->cache) {
      cache.set(js, entry.key, entry.value.getHandle(js));
    }
    // Mark `main` as loaded so embedder scripts calling require('main') do not re-run it.
    cache.set(js, "main"_kj, js.obj());
    auto compiled = js.objNoProto();
    for (auto& entry: state->compiledScripts) {
      compiled.set(js, entry.key, entry.value.getHandle(js));
    }

    auto bag = js.objNoProto();
    bag.set(js, STASH_REQUIRE, state->requireFn.getHandle(js));
    KJ_IF_SOME(embedderRequire, state->embedderRequireFn) {
      bag.set(js, STASH_EMBEDDER_REQUIRE, embedderRequire.getHandle(js));
    }
    bag.set(js, STASH_COMPAT_FLAGS, KJ_ASSERT_NONNULL(state->compatFlagsObj).getHandle(js));
    bag.set(js, STASH_AUTOGATES, state->autogatesObj.getHandle(js));
    bag.set(js, STASH_UTILS, state->utilsObj.getHandle(js));
    bag.set(js, STASH_CACHE, cache);
    bag.set(js, STASH_COMPILED_SCRIPTS, compiled);
    bag.set(js, STASH_WIRE_FN, wireFn);

    auto& isolateBase = jsg::IsolateBase::from(js.v8Isolate);
    isolateBase.mutableSnapshotArtifact().baselineBootstrapStateIndex =
        isolateBase.pinIntoSnapshot(context, v8::Local<v8::Object>(bag));
  }
  JSG_CATCH(exception) {
    kj::throwException(js.exceptionToKj(kj::mv(exception)));
  }
}

// Minimal throwaway isolate type used once per process to build the baseline snapshot. It has no
// ServiceWorkerGlobalScope and materializes no JSG resource templates.
JSG_DECLARE_ISOLATE_TYPE(BaselineSnapshotIsolate);

}  // namespace

EmbedderBootstrap::EmbedderBootstrap(jsg::Bundle::Reader bundle, kj::StringPtr prefix)
    : prefix(kj::str(prefix)),
      entryPoint(kj::str(prefix, "main")) {
  KJ_REQUIRE(prefix.size() > 1 && prefix.endsWith("/") && !prefix.startsWith("./"),
      "embedder bootstrap prefix must be a non-empty path ending with '/'", prefix);
  for (auto& entry: getScriptTable()) {
    KJ_REQUIRE(!entry.key.startsWith(prefix),
        "embedder bootstrap prefix overlaps workerd's bootstrap scripts", prefix, entry.key);
  }
  for (auto module: bundle.getModules()) {
    auto name = module.getName();
    KJ_REQUIRE(
        name.startsWith(prefix), "embedder bootstrap script is not under its prefix", name, prefix);
    scripts.insert(name, module);
  }
  KJ_REQUIRE(scripts.find(entryPoint) != kj::none,
      "embedder bootstrap bundle is missing its entry point", entryPoint);
}

kj::Maybe<jsg::Module::Reader> EmbedderBootstrap::tryGetScript(kj::StringPtr specifier) const {
  KJ_IF_SOME(script, scripts.find(specifier)) {
    return script;
  }
  return kj::none;
}

void runPerIsolateBootstrap(jsg::Lock& js,
    CompatibilityFlags::Reader flags,
    kj::Maybe<const EmbedderBootstrap&> embedder,
    const jsg::ModuleSource* moduleSource) {
  auto context = js.v8Context();

  // Get (or lazily build) the process-wide script lookup table.
  auto& scripts = getScriptTable();

  // Verify required scripts exist.
  KJ_REQUIRE(scripts.find("primordials-js"_kj) != kj::none,
      "per-isolate bootstrap bundle is missing 'primordials-js' script");
  KJ_REQUIRE(scripts.find("primordials"_kj) != kj::none,
      "per-isolate bootstrap bundle is missing 'primordials' script");
  KJ_REQUIRE(scripts.find("main"_kj) != kj::none,
      "per-isolate bootstrap bundle is missing 'main' entry point");

  JSG_TRY(js) {
    if (js.isStartingFromSnapshot()) {
      // A context deserialized from a per-worker startup snapshot. The scripts' effects came with
      // it: the globals main installed, every module's exports, the closures holding require(),
      // primordials and utils.
      auto maybeBag = tryTakeSnapshotBootstrapState(js, context);
      auto& bag = KJ_REQUIRE_NONNULL(maybeBag,
          "context deserialized from a startup snapshot carries no per-isolate bootstrap state");
      TRACE_EVENT("workerd", "PerIsolateBootstrap::restore");
      restoreBootstrapState(js, allocateBootstrapState(js, scripts, embedder, moduleSource), bag);
    } else if (js.isStartingFromBaselineSnapshot()) {
      // A context deserialized from the process-wide baseline snapshot (shared isolate or
      // ephemeral zygote). Restore the pre-compiled scripts and pure-JS modules, then wire the
      // compat-flag-dependent globals for this context.
      auto maybeBag = tryTakeBaselineBootstrapState(js, context);
      auto& bag = KJ_REQUIRE_NONNULL(maybeBag,
          "context deserialized from a baseline snapshot carries no baseline bootstrap state");
      TRACE_EVENT("workerd", "PerIsolateBootstrap::restoreBaseline");
      bootstrapFromBaselineSnapshot(
          js, allocateBootstrapState(js, scripts, embedder, moduleSource), bag, flags);
    } else {
      bootstrapFromScripts(js, allocateBootstrapState(js, scripts, embedder, moduleSource), flags);
    }

    if (!flags.getJsWeakRef()) {
      jsg::deleteWeakRefGlobals(js.v8Isolate, context);
    }
  }
  JSG_CATCH(exception) {
    kj::throwException(js.exceptionToKj(kj::mv(exception)));
  }
}

void cleanupPerIsolateBootstrap(jsg::Lock& js, v8::Local<v8::Context> context) {
  KJ_IF_SOME(state, tryGetBootstrapState(context)) {
    delete &state;
    jsg::setAlignedPointerInEmbedderData(
        context, jsg::ContextPointerSlot::BOOTSTRAP_STATE, nullptr);
  }
}

void stashPerIsolateBootstrapForSnapshot(jsg::Lock& js, v8::Local<v8::Context> context) {
  KJ_REQUIRE(js.isPreparingSnapshot());
  KJ_IF_SOME(state, tryGetBootstrapState(context)) {
    // The zygote's top-level evaluation has left its context scope; creating the objects and
    // setting their properties needs the context entered.
    JSG_WITHIN_CONTEXT_SCOPE(js, context, [&](jsg::Lock& js) {
      KJ_REQUIRE(state.loading.size() == 0, "require() still in progress at snapshot time");
      // The cache is read now rather than when the bootstrap finished: a lazy require() during
      // the zygote's top-level evaluation grows it after main returned. Reuse any stashed cache
      // object from a baseline snapshot so its entries do not need to be copied.
      auto cache = state.stashedCache.map([&](auto& s) { return s.getHandle(js); })
                       .orDefault([&]() { return js.objNoProto(); });
      for (auto& entry: state.cache) {
        cache.set(js, entry.key, entry.value.getHandle(js));
      }
      auto bag = js.objNoProto();
      bag.set(js, STASH_REQUIRE, state.requireFn.getHandle(js));
      if (state.embedderRequireFn != kj::none) {
        bag.set(
            js, STASH_EMBEDDER_REQUIRE, KJ_ASSERT_NONNULL(state.embedderRequireFn).getHandle(js));
      }
      bag.set(js, STASH_COMPAT_FLAGS,
          KJ_REQUIRE_NONNULL(state.compatFlagsObj, "bootstrap has no compatFlags object")
              .getHandle(js));
      bag.set(js, STASH_AUTOGATES, state.autogatesObj.getHandle(js));
      bag.set(js, STASH_UTILS, state.utilsObj.getHandle(js));
      bag.set(js, STASH_PRIMORDIALS,
          KJ_REQUIRE_NONNULL(state.primordials, "bootstrap did not load primordials")
              .getHandle(js));
      bag.set(js, STASH_CACHE, cache);
      auto& isolateBase = jsg::IsolateBase::from(js.v8Isolate);
      isolateBase.mutableSnapshotArtifact().bootstrapStateIndex =
          isolateBase.pinIntoSnapshot(context, v8::Local<v8::Object>(bag));
    });
  }
  // CreateBlob rejects live global handles: release the C++ state as for any context being
  // disposed.
  cleanupPerIsolateBootstrap(js, context);
}

kj::Own<jsg::SnapshotArtifact> buildBaselineSnapshot(jsg::V8System& v8System,
    kj::Own<jsg::SnapshotArtifact> artifact,
    kj::Maybe<const EmbedderBootstrap&> embedder,
    const jsg::ModuleSource* moduleSource) {
  auto& scripts = getScriptTable();
  KJ_REQUIRE(scripts.find("primordials-js"_kj) != kj::none,
      "per-isolate bootstrap bundle is missing 'primordials-js' script");
  KJ_REQUIRE(scripts.find("primordials"_kj) != kj::none,
      "per-isolate bootstrap bundle is missing 'primordials' script");
  KJ_REQUIRE(scripts.find("main"_kj) != kj::none,
      "per-isolate bootstrap bundle is missing 'main' entry point");

  kj::Own<jsg::SnapshotArtifact> result;
  {
    BaselineSnapshotIsolate isolate(v8System, jsg::newIsolateGroup(), nullptr,
        kj::heap<jsg::IsolateObserver>(), jsg::defaultExternalStringAllocator(),
        v8::Isolate::CreateParams{}, /*instantiateTypeWrapper=*/true,
        jsg::SnapshotConfig(jsg::MutableSnapshot{.artifact = kj::mv(artifact)}));

    isolate.runInLockScope([&](BaselineSnapshotIsolate::Lock& lock) {
      auto& isolateBase = jsg::IsolateBase::from(lock.v8Isolate);
      uint32_t badTagFieldsCount = 0;
      jsg::WrappablesSerializeMetadata wrappablesMetadata;
      v8::Global<v8::Context> contextGlobal;

      lock.withinHandleScope([&] {
        v8::Local<v8::Context> context = v8::Context::New(lock.v8Isolate);
        jsg::setAlignedPointerInEmbedderData(
            context, jsg::ContextPointerSlot::MAX_POINTER_SLOT, nullptr);
        isolateBase.setSnapshotDefaultContext(context, &badTagFieldsCount, &wrappablesMetadata);

        JSG_WITHIN_CONTEXT_SCOPE(lock, context, [&](jsg::Lock& js) {
          populateBaselineBootstrapContext(js, context, scripts, embedder, moduleSource);
        });

        contextGlobal.Reset(lock.v8Isolate, context);
      });

      isolateBase.createSnapshotBlob(kj::mv(contextGlobal), {}, &badTagFieldsCount,
          &wrappablesMetadata, {}, v8::SnapshotCreator::FunctionCodeHandling::kKeep);
      result = isolateBase.extractSnapshotArtifact();
      // BaselineSnapshot isolates do not restore constructor templates from the baseline; clear
      // the throwaway isolate's placeholder entries so the artifact carries none.
      result->constructorTemplateIndices.clear();
    });
  }
  return result;
}

kj::Maybe<jsg::JsValue> tryGetBootstrapExport(jsg::Lock& js, kj::StringPtr specifier) {
  return getBootstrapState(js).tryGetLoadedExports(js, specifier);
}

jsg::JsValue requireBootstrapExport(jsg::Lock& js, kj::StringPtr specifier) {
  auto& state = getBootstrapState(js);
  KJ_IF_SOME(exports, state.tryGetLoadedExports(js, specifier)) {
    return exports;
  }
  return state.requireFn.getHandle(js).call(js, js.undefined(), js.strIntern(specifier));
}

kj::Maybe<jsg::JsFunction> tryGetBootstrapFunction(
    jsg::Lock& js, kj::StringPtr specifier, kj::StringPtr name) {
  auto& state = getBootstrapState(js);
  auto normalized = normalizeSpecifier(specifier);

  KJ_IF_SOME(functions, state.functionCache.find(normalized)) {
    KJ_IF_SOME(cached, functions.find(name)) {
      return cached.getHandle(js);
    }
  }

  KJ_IF_SOME(cached, state.tryGetLoadedExports(js, normalized)) {
    KJ_IF_SOME(obj, cached.tryCast<jsg::JsObject>()) {
      KJ_IF_SOME(func, obj.get(js, name).tryCast<jsg::JsFunction>()) {
        auto& functions = state.functionCache.findOrCreate(normalized,
            [&]() -> decltype(state.functionCache)::Entry { return {kj::str(normalized), {}}; });
        functions.insert(kj::str(name), func.addRef(js));
        return func;
      }
    }
  }
  return kj::none;
}

void setBootstrapExportForTest(jsg::Lock& js, kj::StringPtr specifier, jsg::JsValue exports) {
  auto& state = getBootstrapState(js);
  auto normalized = normalizeSpecifier(specifier);
  state.functionCache.erase(normalized);
  state.cache.upsert(kj::str(normalized), exports.addRef(js));
}

}  // namespace workerd
