// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// `into_send()` makes a C++ promise's future `Send`, but not `Sync`: a `&SendKjFuture` would let
// another thread use the promise.
use kj_rs::KjFuture;

#[cxx::bridge(namespace = "kj_rs_compile_test")]
mod ffi {
    unsafe extern "C++" {
        include!("kj-rs-compile-test.h");
        async fn promise() -> i32;
    }
}

fn require_send<T: Send>(_: T) {}

pub fn send() {
    require_send(ffi::promise().into_send());
}

pub fn not_sync() {
    let future = ffi::promise().into_send();
    require_send(&future); //~ ERROR cannot be shared between threads safely

    //~| ERROR cannot be shared between threads safely
}
