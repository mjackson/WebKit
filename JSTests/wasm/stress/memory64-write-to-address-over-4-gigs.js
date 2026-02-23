//@ skip if $addressBits <= 32
//@ runDefaultWasm("-m", "--useWasmMemory64=1", "--useOMGJIT=0")
import { instantiate } from "../wabt-wrapper.js";
import * as assert from "../assert.js";

let wat = `
    (module
        (memory (export "memory") i64 10 50)
        (func (export "write") (param $val i32) (param $addr i64)
            (i32.store (local.get $addr) (local.get $val))
        )
        (func (export "read") (param $addr i64) (result i32)
            (i32.load (local.get $addr))
        )
    )`;

const instance = await instantiate(wat, {}, {reference_types: true});
const {write, read} = instance.exports;

const writeAddr = BigInt(Number.MAX_SAFE_INTEGER + 1);
function test() {
    const outOfBoundsError = [
        WebAssembly.RuntimeError,
        "Out of bounds memory access (evaluating 'func(...args)')",
    ];
    assert.throws(() => write(42, writeAddr), ...outOfBoundsError);
    assert.throws(() => read(writeAddr), ...outOfBoundsError);
}

test();
