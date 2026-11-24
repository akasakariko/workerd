// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Runs with the user_timing_detail_standard compatibility flag.
import { performance, PerformanceMark } from 'node:perf_hooks';
import {
  deepStrictEqual,
  notStrictEqual,
  strictEqual,
  throws,
} from 'node:assert';

export const markDetailIsCloned = {
  test() {
    performance.clearMarks();
    const detail = { foo: 1, bar: { 1: 2 } };

    const mark = performance.mark('m', { detail });
    const ctorMark = new PerformanceMark('c', { detail });
    detail.foo = 2;

    for (const entry of [mark, ctorMark]) {
      notStrictEqual(entry.detail, detail);
      deepStrictEqual(entry.detail, { foo: 1, bar: { 1: 2 } });
      strictEqual(entry.detail, entry.detail);
    }
    performance.clearMarks();
  },
};

export const measureDetailIsCloned = {
  test() {
    performance.clearMeasures();
    const detail = { foo: 1 };

    const measure = performance.measure('m', { start: 0, detail });
    detail.foo = 2;

    notStrictEqual(measure.detail, detail);
    deepStrictEqual(measure.detail, { foo: 1 });
    strictEqual(measure.detail, measure.detail);
    performance.clearMeasures();
  },
};

export const primitiveDetailRoundTrips = {
  test() {
    for (const detail of [null, 0, 'text', true]) {
      strictEqual(performance.mark('m', { detail }).detail, detail);
      strictEqual(
        performance.measure('m', { start: 0, detail }).detail,
        detail
      );
    }
    strictEqual(performance.mark('m', { detail: undefined }).detail, null);
    performance.clearMarks();
    performance.clearMeasures();
  },
};

export const uncloneableDetailThrows = {
  test() {
    performance.clearMarks();
    performance.clearMeasures();

    for (const detail of [Symbol(), { s: Symbol() }, () => {}, { fn() {} }]) {
      const isDataCloneError = (err) => err.name === 'DataCloneError';
      throws(() => performance.mark('bad', { detail }), isDataCloneError);
      throws(() => new PerformanceMark('bad', { detail }), isDataCloneError);
      throws(
        () => performance.measure('bad', { start: 0, detail }),
        isDataCloneError
      );
    }

    strictEqual(performance.getEntriesByName('bad').length, 0);
  },
};
