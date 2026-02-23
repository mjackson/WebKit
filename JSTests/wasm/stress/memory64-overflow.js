//@ skip if $addressBits <= 32
//@ runDefaultWasm("-m", "--useWasmMemory64=1", "--useOMGJIT=0")
import { instantiate } from "../wabt-wrapper.js";
import * as assert from "../assert.js";

async function test() {
  const wat = `
  (module
      (memory i64 1)
      (func (export "load") (param $addr i64) (result i32)
          local.get $addr
          i32.load offset=1
      )
      (func (export "store") (param $addr i64)
          local.get $addr
          i32.const 0
          i32.store offset=1
      )
  )
  `;

  const instance = await instantiate(wat, {}, { threads: false });
  const { load, store } = instance.exports;

  const outOfBoundsError = [
    WebAssembly.RuntimeError,
    "Out of bounds memory access (evaluating 'func(...args)')",
  ];
  // 0xFFFFFFFFFFFFFFFF + offset=1 wraps to 0x0, which is in bounds —
  // without overflow detection this would incorrectly succeed.
  assert.throws(() => load(0xffffffffffffffffn), ...outOfBoundsError);
  assert.throws(() => store(0xffffffffffffffffn), ...outOfBoundsError);

  // 0xFFFFFFFFFFFFFFFE + offset=1 + size-1=3 wraps to 2, also falsely in bounds.
  assert.throws(() => load(0xfffffffffffffffen), ...outOfBoundsError);
  assert.throws(() => store(0xfffffffffffffffen), ...outOfBoundsError);
}

await assert.asyncTest(test());
