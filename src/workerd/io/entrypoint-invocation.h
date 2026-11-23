// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/frankenvalue.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker.h>

namespace workerd::entrypoint {

// One invocation of a WorkerEntrypoint for an event, which WorkerEntrypoint hands to Rust to
// deliver: the request the event was made for, and what finds the handler that receives it. Rust
// owns it from then on, and operates on it through the calls in `entrypoint/ffi.h`.
struct Invocation {
  kj::Own<IoContext::IncomingRequest> request;
  kj::TaskSet& waitUntilTasks;
  kj::Maybe<kj::String> entrypointName;
  kj::Maybe<Worker::VersionInfo> versionInfo;
  Frankenvalue props;
};

// Starts the scheduled handler of the exported handler that `entrypointName`, `versionInfo` and
// `props` find, for `scheduledTime` and the cron pattern `cron`, in a waitUntil task of `context`:
// ServiceWorkerGlobalScope::startScheduled() runs in IoContext::run(). The task fails with the
// exception the handler throws, or with the KJ exception that startScheduled() throws when there
// is no handler. WorkerEntrypoint::runScheduled() calls it, and so does Rust, through
// `invocation_start_scheduled_in_wait_until()` in `entrypoint/ffi.h`.
void startScheduledInWaitUntil(IoContext& context,
    kj::Date scheduledTime,
    kj::String cron,
    kj::Maybe<kj::String> entrypointName,
    kj::Maybe<Worker::VersionInfo> versionInfo,
    Frankenvalue props);

// What finds the exported handler that an event is delivered to, taken out of an Invocation for
// the function that runs the handler, as a C++ event's lambda captures it.
struct HandlerLookup {
  kj::Maybe<kj::String> entrypointName;
  kj::Maybe<Worker::VersionInfo> versionInfo;
  Frankenvalue props;
};

// The handles of the exported handler that an event is delivered to. Each is undefined if there is
// no handler, as under Service Workers syntax, and `ctx` is also undefined if the handler has none.
struct HandlerHandles {
  v8::Local<v8::Value> handler;
  v8::Local<v8::Value> env;
  v8::Local<v8::Value> ctx;
};

// Finds the exported handler that `lookup` names with Worker::Lock::getExportedHandler(), in
// IoContext::run() for `context`, and returns its handles, in the lock's HandleScope. JavaScript
// that finding it runs, such as a class entrypoint's constructor, can throw, and the exception is
// thrown on. The entrypoint crate runs it as the C++ prelude of a run in a waitUntil task
// (`io_context::RunPrelude`), before Rust holds the lock.
HandlerHandles getHandlerHandles(Worker::Lock& lock, IoContext& context, HandlerLookup lookup);

}  // namespace workerd::entrypoint
