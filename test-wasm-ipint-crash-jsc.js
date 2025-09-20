// JSC Shell test case for WebAssembly IPInt crash
// Run with: jsc --useWebAssemblyIPInt=true test-wasm-ipint-crash-jsc.js

print("Testing WebAssembly IPInt crash reproduction...");

// WebAssembly binary (compiled from test-wasm-ipint-crash.wat)
// This is the actual binary content of the wasm module we created
const wasmBytes = new Uint8Array([
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // WASM magic and version
    // The rest would be the compiled WASM bytecode
    // For now, let's create a simpler inline test
]);

// Simpler inline WebAssembly module for testing
const simpleWasmModule = `
(module
  (type $t0 (func (param i32) (result i32)))
  (table 2 funcref)
  (memory 1)
  
  ;; Recursive function that causes deep stack
  (func $recurse (type $t0) (param $n i32) (result i32)
    (if (result i32)
      (i32.eqz (local.get $n))
      (then (i32.const 1))
      (else
        ;; Recursive call
        (i32.add
          (call $recurse (i32.sub (local.get $n) (i32.const 1)))
          (i32.const 1)
        )
      )
    )
  )
  
  ;; Function that uses indirect calls
  (func $indirect_recurse (type $t0) (param $n i32) (result i32)
    (if (result i32)
      (i32.eqz (local.get $n))
      (then (i32.const 1))
      (else
        ;; Indirect call through table
        (call_indirect (type $t0)
          (i32.sub (local.get $n) (i32.const 1))
          (i32.const 0)  ;; table index
        )
      )
    )
  )
  
  ;; Test function
  (func $test (export "test") (param $depth i32) (result i32)
    (call $recurse (local.get $depth))
  )
  
  ;; Indirect test function
  (func $test_indirect (export "test_indirect") (param $depth i32) (result i32)
    (call $indirect_recurse (local.get $depth))
  )
  
  ;; Initialize table
  (elem (i32.const 0) $recurse $indirect_recurse)
)
`;

// Helper to convert WAT to binary (simplified - would need full implementation)
function createSimpleWasmBinary() {
    // This is a minimal WASM module that just exports a function
    // that can trigger the issue
    return new Uint8Array([
        // Magic number
        0x00, 0x61, 0x73, 0x6d,
        // Version
        0x01, 0x00, 0x00, 0x00,
        // Type section
        0x01, 0x07, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        // Function section  
        0x03, 0x02, 0x01, 0x00,
        // Export section
        0x07, 0x08, 0x01, 0x04, 0x74, 0x65, 0x73, 0x74, 0x00, 0x00,
        // Code section
        0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x41, 0x01, 0x6a, 0x0b
    ]);
}

function testBasicWasm() {
    print("\n=== Basic WASM Test ===");
    try {
        const binary = createSimpleWasmBinary();
        const module = new WebAssembly.Module(binary);
        const instance = new WebAssembly.Instance(module);
        
        if (instance.exports.test) {
            const result = instance.exports.test(5);
            print(`Basic test result: ${result}`);
        }
    } catch (e) {
        print(`Basic test error: ${e}`);
        if (e.stack) print(e.stack);
    }
}

// Test with deep recursion
function testDeepRecursion() {
    print("\n=== Deep Recursion Test ===");
    
    // Create a module with deep recursion
    const recursiveWasm = new Uint8Array([
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        // Type section: (func (param i32) (result i32))
        0x01, 0x07, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        // Import section: empty
        // Function section: 1 function of type 0
        0x03, 0x02, 0x01, 0x00,
        // Table section: empty
        // Memory section: 1 page
        0x05, 0x03, 0x01, 0x00, 0x01,
        // Global section: empty
        // Export section: export "test"
        0x07, 0x08, 0x01, 0x04, 0x74, 0x65, 0x73, 0x74, 0x00, 0x00,
        // Code section: recursive function
        0x0a, 0x17, 0x01, 0x15, 0x00,
        // if (param == 0) return 1
        0x20, 0x00,        // local.get 0
        0x45,              // i32.eqz
        0x04, 0x40,        // if
        0x41, 0x01,        // i32.const 1
        0x0f,              // return
        0x0b,              // end
        // else recurse(param - 1) + 1
        0x20, 0x00,        // local.get 0
        0x41, 0x01,        // i32.const 1
        0x6b,              // i32.sub
        0x10, 0x00,        // call 0 (self)
        0x41, 0x01,        // i32.const 1
        0x6a,              // i32.add
        0x0b               // end
    ]);
    
    try {
        const module = new WebAssembly.Module(recursiveWasm);
        const instance = new WebAssembly.Instance(module);
        
        // Test with increasing depths
        for (let depth of [10, 100, 1000, 10000]) {
            try {
                print(`Testing recursion depth ${depth}...`);
                const result = instance.exports.test(depth);
                print(`  Success: ${result}`);
            } catch (e) {
                print(`  Failed at depth ${depth}: ${e}`);
                break;
            }
        }
    } catch (e) {
        print(`Deep recursion test error: ${e}`);
    }
}

// Test with indirect calls
function testIndirectCalls() {
    print("\n=== Indirect Call Test ===");
    
    // WASM module with table and indirect calls
    const indirectWasm = new Uint8Array([
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        // Type section: (func (param i32) (result i32))
        0x01, 0x07, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
        // Import section: empty
        // Function section: 2 functions of type 0
        0x03, 0x03, 0x02, 0x00, 0x00,
        // Table section: 1 table, 2 funcref
        0x04, 0x05, 0x01, 0x70, 0x00, 0x02,
        // Memory section: 1 page
        0x05, 0x03, 0x01, 0x00, 0x01,
        // Export section
        0x07, 0x08, 0x01, 0x04, 0x74, 0x65, 0x73, 0x74, 0x00, 0x00,
        // Element section: initialize table
        0x09, 0x07, 0x01, 0x00, 0x41, 0x00, 0x0b, 0x01, 0x00,
        // Code section
        0x0a, 0x1a, 0x02,
        // Function 0: calls indirect
        0x0c, 0x00,
        0x20, 0x00,        // local.get 0
        0x41, 0x00,        // i32.const 0 (table index)
        0x11, 0x00, 0x00,  // call_indirect type 0
        0x0b,              // end
        // Function 1: simple return
        0x09, 0x00,
        0x20, 0x00,        // local.get 0
        0x41, 0x2a,        // i32.const 42
        0x6a,              // i32.add
        0x0b               // end
    ]);
    
    try {
        const module = new WebAssembly.Module(indirectWasm);
        const instance = new WebAssembly.Instance(module);
        
        print("Testing indirect calls...");
        for (let i = 0; i < 100; i++) {
            const result = instance.exports.test(i);
            if (i % 25 === 0) {
                print(`  Iteration ${i}: ${result}`);
            }
        }
        print("Indirect call test completed");
    } catch (e) {
        print(`Indirect call test error: ${e}`);
    }
}

// Main test execution
function runTests() {
    print("Starting WebAssembly IPInt tests...");
    print("Platform: JSC");
    
    if (typeof WebAssembly === 'undefined') {
        print("ERROR: WebAssembly is not available");
        return;
    }
    
    testBasicWasm();
    testDeepRecursion();
    testIndirectCalls();
    
    print("\n=== Tests Complete ===");
}

// Run the tests
try {
    runTests();
} catch (e) {
    print(`Uncaught error: ${e}`);
    if (e.stack) print(e.stack);
}