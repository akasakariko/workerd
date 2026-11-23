# io/entrypoint/

## OVERVIEW

The `entrypoint` crate: the events of a C++ `WorkerEntrypoint` (`io/worker-entrypoint.{h,c++}`) that Rust delivers. `WorkerEntrypoint` hands Rust an `entrypoint::Invocation` (`io/entrypoint-invocation.h`): the `IoContext::IncomingRequest` the event was made for, the `waitUntil` task set, and what finds the exported handler (entrypoint name, version info, props). Rust reports the event to the tracer, delivers it, starts the handler and finishes the request. The calls on the request are `io_context`'s bindings of `IoContext::IncomingRequest` (`io-context/ffi.h`), and `kj::TaskSet` is `src/rust/kj`'s; this crate's `ffi.h` only reaches into its own `Invocation`, starts the scheduled handler and finds the exported handler (see "Porting C++ to Rust" in `io/AGENTS.md`).

Two events are delivered this way:

- **The scheduled event** (`WorkerEntrypoint::runScheduled()`, `scheduled.rs`), behind the `RUST_ENTRYPOINT_SCHEDULED` autogate. Its JavaScript half, `ServiceWorkerGlobalScope::startScheduled()` with the `ScheduledController` it creates, stays in C++: both paths start it with `entrypoint::startScheduledInWaitUntil()` (`io/entrypoint-invocation.{h,c++}`).
- **The test event** (`WorkerEntrypoint::test()`, `test.rs`), with no autogate, since only test runners deliver it. Its handler runs in Rust: `Delivered::run_in_wait_until()` runs a Rust function in a waitUntil task with `io_context::incoming_request_run_in_wait_until()` (C++ `context.addWaitUntil(context.run(...))`), on the handles that `entrypoint::getHandlerHandles()` finds in the run's C++ prelude (`io_context::RunPrelude`), and the test-api crate calls the handler with a Rust `TestController`.

## KEY FILES

| File           | Role                                                                                                                                                  |
| -------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| `lib.rs`       | `Invocation` → `Delivered` → `Running`: each step consumes the value it is called on, so each happens once and in order. Dropping a delivered event drains it, even after a failed step |
| `scheduled.rs` | The scheduled event's steps, in the order `WorkerEntrypoint::runScheduled()` takes them in C++                                                        |
| `test.rs`      | The test event's steps, and the function that calls the handler's `test()`                                                                            |
| `ffi.rs`       | The cxx bridge, and the Rust functions C++ calls (`scheduled_event_*`, `test_event_*`)                                                                |
| `ffi.h/c++`    | Access to the `Invocation`'s request, waitUntil tasks and handler, the scheduled handler's start, and `runScheduled()` and `runTest()`, which `worker-entrypoint.c++` calls |

## CONVENTIONS

- **C++ is called when a step is called**, not when a future it returns is first polled. A step that waits returns a future for the wait only, so the request is in the state the step leaves it in once the step returns, as in C++. `runScheduled()` and `runTest()` keep this across the boundary by splitting into a synchronous start and an async finish, so that an exception before the wait is thrown from them, as in C++.
- **No panic reaches kj-rs.** The cxx bridge turns a panic in an `extern "Rust"` function into a KJ exception (`src/rust/cxx/README.md`), but kj-rs polls and drops Rust futures through `extern "C"` functions, where a panic aborts the process. Futures handed to C++ are wrapped in `io_context::CatchPanics`.
- **No C++ JSG in `ffi.c++` / `ffi.h`.** They are in `CPP_SHIMS` (`src/rust/jsg/BUILD.bazel`), with no allowlist entries, so `//src/rust/jsg:cpp-jsg-allowlist-test` fails on any use. Code that needs C++ JSG belongs in `src/workerd/io` or `src/workerd/api`, in a function that the C++ path calls too, so that the two paths can't drift apart (as `startScheduledInWaitUntil()` is). No C++ JSG runs while Rust holds the isolate lock either: C++ work that a run does before it calls Rust, such as `getHandlerHandles()` finding the exported handler, which can construct a class, is the run's `io_context::RunPrelude`, which `io-context` runs in `IoContext::run()` before it enters Rust. An exception it throws, as from a throwing constructor, propagates as from the C++ lambda.
- **A function run in a waitUntil task fails with an `io_context::RunError`**: a JavaScript error, which `IoContext::run()` logs as uncaught, or a KJ exception, which it gets unchanged, as from C++ code. A missing `test()` fails with a KJ exception for a JavaScript `Error` (`jsg.Error: ...`), as `JSG_REQUIRE` does in C++, so that it is logged once, not also as thrown by JavaScript.
- **Perfetto spans are emitted from C++.** Rust has no perfetto bindings yet, so a step that should have a span gets it in its C++ call. Both events keep the span names of their C++ paths; `entrypoint::runScheduled()` marks the scheduled event's Rust path. `server/tests/scheduled:scheduled-perfetto-test` and `server/tests/test-event:test-event-perfetto-test` check them.
- **The scheduled event's C++ path stays until the autogate is removed.** Keep the two paths' behaviour identical; `api/tests:scheduled-test` (its `@all-autogates` variant takes the Rust path) and `server/tests/scheduled` check both.

## NOT YET PORTED

- `bench()`: `workerd bench`'s event, once `BenchController` is in Rust.
- `ScheduledController` and `startScheduled()` in Rust, which would make the scheduled event's JavaScript half Rust as well.
- A Rust `WorkerEntrypoint` that implements `worker::Interface`, handling the ported events and delegating the rest to the C++ object. The per-event bridge entry points (`scheduled_event_*`, `test_event_*`) then become methods of that interface, which `RustWorkerInterface` already bridges.
- Perfetto spans from Rust, once Rust has perfetto bindings.
