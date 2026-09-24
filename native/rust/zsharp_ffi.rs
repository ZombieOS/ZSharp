//! The C ABI shared by Z# Rust cdylib modules and the ZVM.
use std::ffi::{c_char, c_int};

pub const ZSHARP_RUST_ABI_VERSION: u32 = 1;
pub const ZSHARP_RUST_NULL: c_int = 0;
pub const ZSHARP_RUST_NUMBER: c_int = 1;
pub const ZSHARP_RUST_TEXT: c_int = 2;
pub const ZSHARP_RUST_STATUS: c_int = 3;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ZSharpRustValue {
    pub value_type: c_int,
    pub number: f64,
    pub text: *const c_char,
}

impl Default for ZSharpRustValue {
    fn default() -> Self {
        Self {
            value_type: ZSHARP_RUST_NULL,
            number: 0.0,
            text: std::ptr::null(),
        }
    }
}
