#[path = "zsharp_ffi.rs"]
mod zsharp_ffi;

use std::cell::RefCell;
use std::ffi::{c_char, CStr, CString};
use zsharp_ffi::*;

thread_local! {
    static LAST_TEXT: RefCell<CString> = RefCell::new(CString::new("").unwrap());
}

#[no_mangle]
pub unsafe extern "C" fn zsharp_rust_call_v1(
    abi_version: u32,
    function: *const c_char,
    arguments: *const ZSharpRustValue,
    argument_count: usize,
    result: *mut ZSharpRustValue,
    _error: *mut c_char,
    _error_size: usize,
) -> i32 {
    if abi_version != ZSHARP_RUST_ABI_VERSION || function.is_null() || result.is_null() {
        return 0;
    }
    let name = match CStr::from_ptr(function).to_str() {
        Ok(value) => value,
        Err(_) => return 0,
    };
    let args = if argument_count == 0 {
        &[][..]
    } else if arguments.is_null() {
        return 0;
    } else {
        std::slice::from_raw_parts(arguments, argument_count)
    };
    match name {
        "greeting" if args.len() == 1 && args[0].value_type == ZSHARP_RUST_TEXT => {
            let person = CStr::from_ptr(args[0].text).to_string_lossy();
            let greeting = CString::new(format!("Hello from Rust, {person}!"));
            let Ok(greeting) = greeting else { return 0 };
            LAST_TEXT.with(|last| {
                *last.borrow_mut() = greeting;
                (*result).value_type = ZSHARP_RUST_TEXT;
                (*result).text = last.borrow().as_ptr();
            });
            1
        }
        "add" if args.len() == 2 => {
            (*result).value_type = ZSHARP_RUST_NUMBER;
            (*result).number = args[0].number + args[1].number;
            1
        }
        "ready" if args.is_empty() => {
            (*result).value_type = ZSHARP_RUST_STATUS;
            (*result).number = 1.0;
            1
        }
        _ => 0,
    }
}
