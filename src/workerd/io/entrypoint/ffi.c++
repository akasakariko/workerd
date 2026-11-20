// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "ffi.h"

#include <workerd/io/entrypoint/ffi.rs.h>
#include <workerd/rust/worker/bridge.h>
#include <workerd/util/use-perfetto-categories.h>

namespace workerd::entrypoint {

IoContext::IncomingRequest& invocation_request(Invocation& invocation) {
  return *invocation.request;
}

kj::Own<IoContext::IncomingRequest> invocation_take_request(Invocation& invocation) {
  return kj::mv(invocation.request);
}

kj::TaskSet& invocation_wait_until_tasks(Invocation& invocation) {
  return invocation.waitUntilTasks;
}

void log_drain_failure(kj::Exception exception) {
  KJ_LOG(ERROR, "draining an entrypoint invocation failed", exception);
}

void trace_scheduled_wait_for_finished() {
  TRACE_EVENT("workerd", "WorkerEntrypoint::runScheduled() waitForFinished()");
}

void invocation_start_scheduled_in_wait_until(
    Invocation& invocation, kj::Date scheduledTime, ::rust::Slice<const kj::byte> cron) {
  startScheduledInWaitUntil(invocation.request->getContext(), scheduledTime,
      kj::str(kj::from<kj_rs::Rust>(cron).asChars()), kj::mv(invocation.entrypointName),
      kj::mv(invocation.versionInfo), kj::mv(invocation.props));
}

kj::Promise<WorkerInterface::ScheduledResult> runScheduled(
    kj::Own<Invocation> invocation, kj::Date scheduledTime, kj::StringPtr cron) {
  // Only the Rust path has this span, so a trace shows which path delivered the event.
  TRACE_EVENT("workerd", "entrypoint::runScheduled()");
  auto scheduled =
      scheduled_event_start(kj::mv(invocation), scheduledTime, cron.asBytes().as<kj_rs::Rust>());
  return scheduled_event_finish(kj::mv(scheduled))
      .then([](::workerd::rust::worker::ScheduledResult result) {
    return kj::from<kj_rs::Rust>(result);
  });
}

}  // namespace workerd::entrypoint
