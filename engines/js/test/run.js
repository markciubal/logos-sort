// Test runner — verifies the seven sort invariants across many inputs.

const { runTests } = require('./property-tests-harness.js');
const { sort, sortInplace } = require('..');

let failed = 0;

console.log('logos-sort: property tests\n');

console.log('== sort (buffered) ==');
const r1 = runTests(sort, { trials: 30, sizes: [10, 100, 1000] });
if (r1.totalFailures > 0) failed++;

console.log('\n== sortInplace ==');
const r2 = runTests(sortInplace, { trials: 30, sizes: [10, 100, 1000] });
if (r2.totalFailures > 0) failed++;

console.log('\n== argSort ==');
const { runArgsortTests } = require('./argsort-tests.js');
const { argSort } = require('..');
const r3 = runArgsortTests(argSort, {});
if (r3.totalFailures > 0) failed++;

console.log('\n== perf-cliff guard ==');
const { runPerfGuards } = require('./perf-guard.js');
const r4 = runPerfGuards(require('..').sort);
if (r4.totalFailures > 0) failed++;

if (failed > 0) {
  console.error('\n✗ ' + failed + ' module(s) failed property tests');
  process.exit(1);
}

console.log('\n✓ all property tests pass');
process.exit(0);
