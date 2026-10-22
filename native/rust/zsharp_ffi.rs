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

// The v2 ABI is additive; none of the v1 constants or layouts above change.
pub const ZSHARP_INTEROP_ABI_V2: u32 = 2;
pub const ZSHARP_INTEROP_MAX_JSON_BYTES: usize = 1024 * 1024;

#[repr(C)]
pub struct ZSharpInteropRequestV2 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub operation: *const c_char,
    pub target: *const c_char,
    pub member: *const c_char,
    pub handle: *const c_char,
    pub arguments_json: *const c_char,
}

pub type ZSharpInteropWriteV2 = unsafe extern "C" fn(
    context: *mut std::ffi::c_void,
    json: *const c_char,
    length: usize,
    error: *mut c_char,
    error_size: usize,
) -> c_int;

#[repr(C)]
pub struct ZSharpInteropResultV2 {
    pub context: *mut std::ffi::c_void,
    pub write: Option<ZSharpInteropWriteV2>,
}

pub type ZSharpInteropHostInvokeV2 = unsafe extern "C" fn(
    context: *mut std::ffi::c_void,
    request: *const ZSharpInteropRequestV2,
    result: *const ZSharpInteropResultV2,
    error: *mut c_char,
    error_size: usize,
) -> c_int;

#[repr(C)]
pub struct ZSharpInteropHostV2 {
    pub abi_version: u32,
    pub struct_size: u32,
    pub context: *mut std::ffi::c_void,
    pub invoke: Option<ZSharpInteropHostInvokeV2>,
}

pub type ZSharpInteropCallV2 = unsafe extern "C" fn(
    host: *const ZSharpInteropHostV2,
    request: *const ZSharpInteropRequestV2,
    result: *const ZSharpInteropResultV2,
    error: *mut c_char,
    error_size: usize,
) -> c_int;

/// Copies a complete UTF-8 wire JSON value into the caller's synchronous sink.
/// The buffer can be dropped as soon as this returns. Never retry a failed
/// sink write: the native operation may already have changed state.
///
/// # Safety
/// `result` must be the live sink borrowed by the current entry, on its thread.
pub unsafe fn interop_emit(
    result: *const ZSharpInteropResultV2,
    json: &str,
    error: *mut c_char,
    error_size: usize,
) -> c_int {
    if result.is_null() || json.len() > ZSHARP_INTEROP_MAX_JSON_BYTES {
        return 0;
    }
    match (*result).write {
        Some(write) => write((*result).context, json.as_ptr().cast(), json.len(), error, error_size),
        None => 0,
    }
}

/// A synchronous, thread-bound borrowed ZVM host. The lifetime is deliberately
/// tied to the entry's host reference; !Send/!Sync prevents moving it to a
/// worker or retaining it in an asynchronous task through safe Rust.
pub struct InteropHost<'call> {
    host: &'call ZSharpInteropHostV2,
    _thread_bound: std::marker::PhantomData<std::rc::Rc<()>>,
}

impl<'call> InteropHost<'call> {
    pub fn new(host: &'call ZSharpInteropHostV2) -> Result<Self, &'static str> {
        if host.abi_version != ZSHARP_INTEROP_ABI_V2
            || (host.struct_size as usize) < std::mem::size_of::<ZSharpInteropHostV2>()
            || host.invoke.is_none()
        {
            return Err("unsupported Z# interop host ABI");
        }
        Ok(Self { host, _thread_bound: std::marker::PhantomData })
    }

    /// Invokes call/get/set/invoke synchronously. Numbers remain exact tagged
    /// JSON text. The host validates object handles and callback visibility.
    pub fn request(
        &self,
        operation: &str,
        target: &str,
        member: &str,
        handle: &str,
        arguments_json: &str,
    ) -> Result<String, String> {
        use std::ffi::{CStr, CString};
        let strings: Result<Vec<_>, _> = [operation, target, member, handle, arguments_json]
            .iter().map(|value| CString::new(*value)).collect();
        let strings = strings.map_err(|_| "interop strings cannot contain NUL".to_owned())?;
        if arguments_json.len() > ZSHARP_INTEROP_MAX_JSON_BYTES {
            return Err("interop JSON exceeds 1 MiB".to_owned());
        }
        let request = ZSharpInteropRequestV2 {
            abi_version: ZSHARP_INTEROP_ABI_V2,
            struct_size: std::mem::size_of::<ZSharpInteropRequestV2>() as u32,
            operation: strings[0].as_ptr(),
            target: strings[1].as_ptr(),
            member: strings[2].as_ptr(),
            handle: strings[3].as_ptr(),
            arguments_json: strings[4].as_ptr(),
        };
        let mut captured: Option<String> = None;
        unsafe extern "C" fn collect(
            context: *mut std::ffi::c_void,
            json: *const c_char,
            length: usize,
            _error: *mut c_char,
            _error_size: usize,
        ) -> c_int {
            if context.is_null() || json.is_null() || length > ZSHARP_INTEROP_MAX_JSON_BYTES {
                return 0;
            }
            let captured = &mut *context.cast::<Option<String>>();
            if captured.is_some() { return 0; }
            let bytes = std::slice::from_raw_parts(json.cast::<u8>(), length);
            let Ok(text) = std::str::from_utf8(bytes) else { return 0 };
            *captured = Some(text.to_owned());
            1
        }
        let result = ZSharpInteropResultV2 {
            context: (&mut captured as *mut Option<String>).cast(),
            write: Some(collect),
        };
        let mut error = [0 as c_char; 1024];
        let ok = unsafe {
            self.host.invoke.unwrap()(self.host.context, &request, &result, error.as_mut_ptr(), error.len())
        };
        if ok == 0 {
            // A hostile native host must not make the error read unbounded.
            error[1023] = 0;
            let message = unsafe { CStr::from_ptr(error.as_ptr()) }.to_string_lossy();
            return Err(if message.is_empty() { "Z# host request failed".to_owned() } else { message.into_owned() });
        }
        captured.ok_or_else(|| "Z# host returned no result".to_owned())
    }
}
