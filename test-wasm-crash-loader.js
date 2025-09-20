// Load and test the WebAssembly module in JSC
// Run with: ./WebKitBuild/Debug/bin/jsc test-wasm-crash-loader.js

print("Loading WebAssembly module for crash reproduction test...");

// Read the wasm binary file
function loadWasmFile(path) {
    // In JSC shell, we need to read binary data differently
    // We'll embed the compiled wasm bytes directly
    
    // This is the actual WASM binary compiled from test-wasm-ipint-crash.wat
    // We'll use a simpler test module for now
    const wasmModule = new WebAssembly.Module(new Uint8Array([
        // WASM Magic number and version
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        
        // Type section (1 type)
        0x01, 0x07, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,  // (func (param i32) (result i32))
        
        // Function section (1 function)
        0x03, 0x02, 0x01, 0x00,
        
        // Table section (1 table with 2 funcref)
        0x04, 0x05, 0x01, 0x70, 0x00, 0x02,
        
        // Memory section (1 page)
        0x05, 0x03, 0x01, 0x00, 0x01,
        
        // Export section (export "test")
        0x07, 0x08, 0x01, 0x04, 0x74, 0x65, 0x73, 0x74, 0x00, 0x00,
        
        // Element section (initialize table with function 0)
        0x09, 0x09, 0x01, 0x00, 0x41, 0x00, 0x0b, 0x02, 0x00, 0x00,
        
        // Code section
        0x0a, 0x2b, 0x01, 0x29, 0x01, 0x01, 0x7f,
        // Function body with recursive call and indirect call
        0x20, 0x00,        // local.get 0
        0x45,              // i32.eqz
        0x04, 0x40,        // if
        0x41, 0x01,        // i32.const 1
        0x0f,              // return
        0x0b,              // end
        // Store to memory to trigger potential bounds issues
        0x20, 0x00,        // local.get 0
        0x41, 0x04,        // i32.const 4
        0x6c,              // i32.mul
        0x21, 0x01,        // local.set 1
        0x20, 0x01,        // local.get 1
        0x20, 0x00,        // local.get 0
        0x36, 0x02, 0x00,  // i32.store offset=0
        // Indirect call through table
        0x20, 0x00,        // local.get 0
        0x41, 0x01,        // i32.const 1
        0x6b,              // i32.sub
        0x41, 0x00,        // i32.const 0 (table index)
        0x11, 0x00, 0x00,  // call_indirect type 0
        0x0b               // end
    ]));
    
    return wasmModule;
}

function runTests() {
    try {
        print("\n=== Creating WebAssembly module ===");
        const module = loadWasmFile();
        
        print("=== Instantiating module ===");
        const instance = new WebAssembly.Instance(module);
        
        print("=== Running tests ===");
        
        // Test 1: Small recursion depth
        print("\nTest 1: Small recursion (depth=10)");
        try {
            const result = instance.exports.test(10);
            print(`  Result: ${result}`);
        } catch (e) {
            print(`  Error: ${e}`);
        }
        
        // Test 2: Medium recursion depth
        print("\nTest 2: Medium recursion (depth=100)");
        try {
            const result = instance.exports.test(100);
            print(`  Result: ${result}`);
        } catch (e) {
            print(`  Error: ${e}`);
        }
        
        // Test 3: Deep recursion (potential stack overflow)
        print("\nTest 3: Deep recursion (depth=1000)");
        try {
            const result = instance.exports.test(1000);
            print(`  Result: ${result}`);
        } catch (e) {
            print(`  Error: ${e}`);
        }
        
        // Test 4: Very deep recursion
        print("\nTest 4: Very deep recursion (depth=10000)");
        try {
            const result = instance.exports.test(10000);
            print(`  Result: ${result}`);
        } catch (e) {
            print(`  Error: ${e}`);
        }
        
        // Test 5: Rapid repeated calls
        print("\nTest 5: Rapid repeated calls (1000 iterations)");
        let successCount = 0;
        for (let i = 0; i < 1000; i++) {
            try {
                instance.exports.test(50);
                successCount++;
            } catch (e) {
                print(`  Failed at iteration ${i}: ${e}`);
                break;
            }
        }
        print(`  Completed ${successCount}/1000 calls`);
        
        print("\n=== All tests completed ===");
        
    } catch (e) {
        print(`\nFATAL ERROR: ${e}`);
        if (e.stack) {
            print("Stack trace:");
            print(e.stack);
        }
    }
}

// Check for WebAssembly support
if (typeof WebAssembly === 'undefined') {
    print("ERROR: WebAssembly is not available in this environment");
} else {
    print("WebAssembly is available");
    print("Starting tests...");
    runTests();
}