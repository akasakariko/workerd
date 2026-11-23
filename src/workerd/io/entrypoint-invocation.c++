// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "entrypoint-invocation.h"

#include <workerd/api/global-scope.h>
#include <workerd/util/use-perfetto-categories.h>

namespace workerd::entrypoint {

void startScheduledInWaitUntil(IoContext& context,
    kj::Date scheduledTime,
    kj::String cron,
    kj::Maybe<kj::String> entrypointName,
    kj::Maybe<Worker::VersionInfo> versionInfo,
    Frankenvalue props) {
  // Scheduled handlers run entirely in waitUntil() tasks.
  context.addWaitUntil(
      context.run([scheduledTime, cron = kj::mv(cron), entrypointName = kj::mv(entrypointName),
                      versionInfo = kj::mv(versionInfo),
                      props = kj::mv(props)](Worker::Lock& lock, IoContext& context) mutable {
    TRACE_EVENT("workerd", "WorkerEntrypoint::runScheduled() run");
    auto traceScope = context.makeRequestTraceScope(lock);

    lock.getGlobalScope().startScheduled(scheduledTime, cron, lock,
        lock.getExportedHandler(
            entrypointName.map([](kj::String& name) -> kj::StringPtr { return name; }),
            kj::mv(versionInfo), kj::mv(props), context.getActor()));
  }));
}

HandlerHandles getHandlerHandles(Worker::Lock& lock, IoContext& context, HandlerLookup lookup) {
  jsg::Lock& js = lock;
  HandlerHandles handles{
    .handler = js.v8Undefined(),
    .env = js.v8Undefined(),
    .ctx = js.v8Undefined(),
  };
  KJ_IF_SOME(h,
      lock.getExportedHandler(
          lookup.entrypointName.map([](kj::String& name) -> kj::StringPtr { return name; }),
          kj::mv(lookup.versionInfo), kj::mv(lookup.props), context.getActor())) {
    // The handles stay valid in the lock's HandleScope once the ExportedHandler is gone.
    handles.handler = h->self.getHandle(js);
    handles.env = h->env.getHandle(js);
    KJ_IF_SOME(c, h->getCtx()) {
      handles.ctx = lock.getWorker().getIsolate().getApi().wrapExecutionContext(js, kj::mv(c));
    }
  }
  return handles;
}

}  // namespace workerd::entrypoint
