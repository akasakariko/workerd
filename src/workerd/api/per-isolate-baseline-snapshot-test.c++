// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/per-isolate-bootstrap.h>
#include <workerd/jsg/setup.h>
#include <workerd/tests/test-fixture.h>

#include <embedder_bootstrap_test/embedder_bootstrap_test.capnp.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("baseline snapshot boots shared isolates with different compatibility flags") {
  EmbedderBootstrap embedder(EMBEDDER_BOOTSTRAP_TEST_BUNDLE, "test-embedder/");
  auto baseline = buildBaselineSnapshot(
      TestFixture::getV8System(), kj::atomicRefcounted<jsg::SnapshotArtifact>(), embedder);
  KJ_EXPECT(baseline->blob.data != nullptr && baseline->blob.raw_size > 0);
  KJ_EXPECT(baseline->baselineBootstrapStateIndex != kj::none);
  KJ_EXPECT(baseline->constructorTemplateIndices.empty());
  KJ_EXPECT(baseline->liveWrappables.empty());

  // Isolate 1: TypeScript streams, WeakRef, MessageChannel, spec-compliant attributes, Node compat.
  {
    capnp::MallocMessageBuilder message;
    auto flags = message.initRoot<CompatibilityFlags>();
    flags.setTypeScriptImplementedStreams(true);
    flags.setJsWeakRef(true);
    flags.setExposeGlobalMessageChannel(true);
    flags.setSpecCompliantPropertyAttributes(true);
    flags.setNodeJsCompat(true);
    flags.setNodeJsCompatV2(true);

    TestFixture fixture({
      .featureFlags = flags.asReader(),
      .embedderBootstrap = embedder,
      .snapshotConfig = jsg::SnapshotConfig(jsg::BaselineSnapshot{.artifact = baseline->addRef()}),
    });
    fixture.runInIoContext([](const TestFixture::Environment& env) {
      auto& js = env.js;
      KJ_EXPECT(js.isStartingFromBaselineSnapshot());
      KJ_EXPECT(!js.isStartingFromSnapshot());
      auto tsReadableStream =
          KJ_ASSERT_NONNULL(
              KJ_ASSERT_NONNULL(tryGetBootstrapExport(js, "webstreams/streams"))
                  .tryCast<jsg::JsObject>())
              .get(js, "ReadableStream");
      KJ_EXPECT(js.global().get(js, "ReadableStream").strictEquals(tsReadableStream));
      KJ_EXPECT(!js.global().get(js, "WeakRef").isUndefined());
      KJ_EXPECT(!js.global().get(js, "MessageChannel").isUndefined());
      KJ_EXPECT(!js.global().get(js, "Buffer").isUndefined());
      KJ_EXPECT(js.global().get(js, "testEmbedderGlobal").strictEquals(js.str("installed"_kj)));
    });
  }

  // Isolate 2: same baseline snapshot, but C++ streams, no WeakRef, no MessageChannel, no Node.
  {
    capnp::MallocMessageBuilder message;
    auto flags = message.initRoot<CompatibilityFlags>();
    flags.setTypeScriptImplementedStreams(false);
    flags.setJsWeakRef(false);
    flags.setExposeGlobalMessageChannel(false);
    flags.setNodeJsCompatV2(false);

    TestFixture fixture({
      .featureFlags = flags.asReader(),
      .embedderBootstrap = embedder,
      .snapshotConfig = jsg::SnapshotConfig(jsg::BaselineSnapshot{.artifact = baseline->addRef()}),
    });
    fixture.runInIoContext([](const TestFixture::Environment& env) {
      auto& js = env.js;
      KJ_EXPECT(js.isStartingFromBaselineSnapshot());
      KJ_EXPECT(!js.isStartingFromSnapshot());
      KJ_EXPECT(js.global().get(js, "WeakRef").isUndefined());
      KJ_EXPECT(js.global().get(js, "MessageChannel").isUndefined());
      KJ_EXPECT(js.global().get(js, "Buffer").isUndefined());
      KJ_EXPECT(!js.global().get(js, "ReadableStream").isUndefined());
      // Without typeScriptImplementedStreams, globalThis.ReadableStream stays the C++ class
      // rather than the pre-evaluated TypeScript ReadableStream from the baseline cache.
      auto tsReadableStream =
          KJ_ASSERT_NONNULL(
              KJ_ASSERT_NONNULL(tryGetBootstrapExport(js, "webstreams/streams"))
                  .tryCast<jsg::JsObject>())
              .get(js, "ReadableStream");
      KJ_EXPECT(!js.global().get(js, "ReadableStream").strictEquals(tsReadableStream));
      KJ_EXPECT(!js.global().get(js, "EventSource").isUndefined());
      KJ_EXPECT(js.global().get(js, "testEmbedderGlobal").strictEquals(js.str("installed"_kj)));
    });
  }
}

KJ_TEST("ephemeral zygote chains from baseline snapshot to produce a worker snapshot") {
  auto baseline = buildBaselineSnapshot(
      TestFixture::getV8System(), kj::atomicRefcounted<jsg::SnapshotArtifact>());

  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setTypeScriptImplementedStreams(true);
  flags.setExposeGlobalMessageChannel(true);

  constexpr kj::StringPtr workerSource = R"(
    const token = Math.random();
    let counter = 0;
    export default {
      async fetch() {
        counter++;
        const rs = new ReadableStream({
          start(c) { c.enqueue('ok'); c.close(); }
        });
        const reader = rs.getReader();
        const { value } = await reader.read();
        const mc = new MessageChannel();
        mc.port1.close();
        mc.port2.close();
        return new Response(`${token}:${counter}:${value}`);
      }
    };
  )"_kj;

  // Step 1: Build a per-worker snapshot in a zygote chained off the baseline snapshot.
  kj::Own<jsg::SnapshotArtifact> workerArtifact;
  {
    TestFixture zygote({
      .featureFlags = flags.asReader(),
      .mainModuleSource = workerSource,
      .snapshotConfig = jsg::SnapshotConfig(jsg::MutableSnapshot{
        .artifact = kj::atomicRefcounted<jsg::SnapshotArtifact>(),
        .base = baseline->addRef(),
      }),
    });
    zygote.enterWorkerLockSynchronously([&](Worker::Lock& lock) {
      auto& isolateBase = jsg::IsolateBase::from(lock.getIsolate());
      KJ_EXPECT(isolateBase.isPreparingSnapshot());
      KJ_EXPECT(isolateBase.isStartingFromBaselineSnapshot());
      workerArtifact = isolateBase.extractSnapshotArtifact();
    });
  }
  KJ_EXPECT(workerArtifact->blob.data != nullptr && workerArtifact->blob.raw_size > 0);
  KJ_EXPECT(workerArtifact->bootstrapStateIndex != kj::none);

  // Step 2: Boot two independent request isolates from the worker snapshot. Both must observe the
  // same top-level `token` and counter == 1 (isolated heaps, no top-level re-evaluation).
  auto runRestored = [&]() -> kj::String {
    TestFixture restored({
      .featureFlags = flags.asReader(),
      .mainModuleSource = workerSource,
      .snapshotConfig =
          jsg::SnapshotConfig(jsg::FinalizedSnapshot{.artifact = workerArtifact->addRef()}),
    });
    restored.enterWorkerLockSynchronously([](Worker::Lock& lock) {
      auto& isolateBase = jsg::IsolateBase::from(lock.getIsolate());
      KJ_EXPECT(isolateBase.isStartingFromSnapshot());
      KJ_EXPECT(!isolateBase.isStartingFromBaselineSnapshot());
    });
    auto response = restored.runRequest(kj::HttpMethod::GET, "http://example.com/", "");
    KJ_EXPECT(response.statusCode == 200);
    return kj::mv(response.body);
  };

  auto first = runRestored();
  auto second = runRestored();
  KJ_EXPECT(first == second, first, second);
  KJ_EXPECT(first.endsWith(":1:ok"_kj), first);
}

}  // namespace
}  // namespace workerd
