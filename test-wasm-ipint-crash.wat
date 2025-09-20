;; WebAssembly Text Format test case to reproduce IPInt crash
;; This test attempts to reproduce the crashes seen in Bun with tesseract.js/scribe.js-ocr
;; The crashes appear to happen during deep call stacks with indirect calls

(module
  ;; Define a table for indirect function calls
  (table $funcs 10 funcref)
  
  ;; Define memory with 1 page initially, growable
  (memory $mem 1 100)
  
  ;; Type for recursive functions
  (type $recursive_func (func (param i32) (result i32)))
  
  ;; Function that does memory operations and recursive calls
  (func $recursive_mem_op (type $recursive_func) (param $depth i32) (result i32)
    (local $addr i32)
    (local $result i32)
    
    ;; Base case - return when depth is 0
    (if (i32.eqz (local.get $depth))
      (then (return (i32.const 42)))
    )
    
    ;; Calculate memory address based on depth
    (local.set $addr 
      (i32.mul 
        (local.get $depth)
        (i32.const 4)
      )
    )
    
    ;; Bounds check and memory operation
    (if (i32.lt_u (local.get $addr) (i32.const 65536))
      (then
        ;; Store value in memory
        (i32.store (local.get $addr) (local.get $depth))
        
        ;; Load it back
        (local.set $result (i32.load (local.get $addr)))
      )
    )
    
    ;; Recursive call with decremented depth
    (call $recursive_mem_op 
      (i32.sub (local.get $depth) (i32.const 1))
    )
    
    ;; Add current result
    (i32.add (local.get $result))
  )
  
  ;; Function that uses indirect calls
  (func $indirect_caller (type $recursive_func) (param $depth i32) (result i32)
    (local $func_idx i32)
    
    ;; Base case
    (if (i32.eqz (local.get $depth))
      (then (return (i32.const 100)))
    )
    
    ;; Calculate function index (mod table size)
    (local.set $func_idx
      (i32.rem_u (local.get $depth) (i32.const 3))
    )
    
    ;; Indirect call through table
    (call_indirect (type $recursive_func)
      (i32.sub (local.get $depth) (i32.const 1))
      (local.get $func_idx)
    )
  )
  
  ;; Another recursive function for variety
  (func $recursive_alt (type $recursive_func) (param $depth i32) (result i32)
    (if (result i32)
      (i32.eqz (local.get $depth))
      (then (i32.const 200))
      (else
        ;; Mix of direct and indirect calls
        (if (result i32)
          (i32.rem_u (local.get $depth) (i32.const 2))
          (then
            (call $recursive_mem_op 
              (i32.sub (local.get $depth) (i32.const 1))
            )
          )
          (else
            (call_indirect (type $recursive_func)
              (i32.sub (local.get $depth) (i32.const 1))
              (i32.const 0)
            )
          )
        )
      )
    )
  )
  
  ;; Main stress test function
  (func $stress_test (export "stress_test") (param $iterations i32) (result i32)
    (local $i i32)
    (local $sum i32)
    
    ;; Loop through iterations
    (loop $loop
      ;; Call with deep recursion
      (local.set $sum
        (i32.add
          (local.get $sum)
          (call $indirect_caller (i32.const 1000))
        )
      )
      
      ;; Increment counter
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      
      ;; Continue if not done
      (br_if $loop
        (i32.lt_u (local.get $i) (local.get $iterations))
      )
    )
    
    (local.get $sum)
  )
  
  ;; Export a simple test function
  (func $simple_test (export "simple_test") (result i32)
    (call $recursive_mem_op (i32.const 100))
  )
  
  ;; Initialize the table with function references
  (elem (i32.const 0) $recursive_mem_op $indirect_caller $recursive_alt)
)