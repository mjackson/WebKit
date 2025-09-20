// Test case to reproduce WebAssembly IPInt crash
// This simulates the deep recursion and indirect calls that trigger the crash in Bun

const fs = require('fs');

async function runTest() {
    console.log("Loading WebAssembly module...");
    
    try {
        // Read the wasm file
        const wasmBuffer = fs.readFileSync('./test-wasm-ipint-crash.wasm');
        
        // Compile and instantiate the module
        const wasmModule = await WebAssembly.compile(wasmBuffer);
        const instance = await WebAssembly.instantiate(wasmModule);
        
        console.log("Module loaded successfully");
        console.log("Available exports:", Object.keys(instance.exports));
        
        // Test 1: Simple recursive test
        console.log("\n=== Test 1: Simple recursive call ===");
        try {
            const result1 = instance.exports.simple_test();
            console.log(`Simple test result: ${result1}`);
        } catch (e) {
            console.error("Simple test failed:", e);
        }
        
        // Test 2: Stress test with increasing iterations
        console.log("\n=== Test 2: Stress test with indirect calls ===");
        for (let iterations of [1, 10, 100, 1000]) {
            console.log(`Running stress test with ${iterations} iterations...`);
            try {
                const result = instance.exports.stress_test(iterations);
                console.log(`  Result: ${result}`);
            } catch (e) {
                console.error(`  Failed at ${iterations} iterations:`, e);
                break;
            }
        }
        
        // Test 3: Rapid repeated calls (simulating the OCR workload)
        console.log("\n=== Test 3: Rapid repeated calls ===");
        const rapidCalls = 10000;
        console.log(`Making ${rapidCalls} rapid calls...`);
        let successCount = 0;
        for (let i = 0; i < rapidCalls; i++) {
            try {
                instance.exports.simple_test();
                successCount++;
                if (i % 1000 === 0) {
                    console.log(`  Progress: ${i}/${rapidCalls}`);
                }
            } catch (e) {
                console.error(`  Failed at call ${i}:`, e);
                break;
            }
        }
        console.log(`  Completed ${successCount}/${rapidCalls} calls`);
        
    } catch (error) {
        console.error("Test failed with error:", error);
        console.error("Stack trace:", error.stack);
    }
}

// For JSC shell compatibility
if (typeof WebAssembly === 'undefined') {
    console.error("WebAssembly is not available in this environment");
    console.log("This test requires WebAssembly support");
} else {
    runTest().catch(console.error);
}