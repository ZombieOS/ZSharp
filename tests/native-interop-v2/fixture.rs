#[path = "../../native/rust/zsharp_ffi.rs"]
mod zsharp_ffi;

use std::collections::BTreeMap;
use std::ffi::{c_char, CStr};
use std::sync::{Mutex, OnceLock};
use zsharp_ffi::*;

#[derive(Default)]
struct Objects {
    next: u64,
    values: BTreeMap<String, String>,
}

fn objects() -> &'static Mutex<Objects> {
    static OBJECTS: OnceLock<Mutex<Objects>> = OnceLock::new();
    OBJECTS.get_or_init(|| Mutex::new(Objects::default()))
}

unsafe fn message(error: *mut c_char, size: usize, text: &str) {
    if !error.is_null() && size > 0 {
        let count = text.len().min(size - 1);
        std::ptr::copy_nonoverlapping(text.as_ptr().cast(), error, count);
        *error.add(count) = 0;
    }
}

unsafe fn run(
    host: *const ZSharpInteropHostV2,
    request: *const ZSharpInteropRequestV2,
) -> Result<String, String> {
    if request.is_null() || (*request).abi_version != 2
        || ((*request).struct_size as usize) < std::mem::size_of::<ZSharpInteropRequestV2>() {
        return Err("unsupported interop request ABI".into());
    }
    let request = &*request;
    let text = |pointer: *const c_char| -> Result<String, String> {
        if pointer.is_null() { return Err("missing request string".into()); }
        CStr::from_ptr(pointer).to_str().map(str::to_owned).map_err(|_| "invalid UTF-8".into())
    };
    let op = text(request.operation)?;
    let target = text(request.target)?;
    let member = text(request.member)?;
    let handle = text(request.handle)?;
    let args = text(request.arguments_json)?;
    if op == "call" && target == "echo" { return Ok(args); }
    if op == "call" && target == "callback" {
        if host.is_null() { return Err("missing Z# callback host".into()); }
        return InteropHost::new(&*host).map_err(str::to_owned)?
            .request("call", "Main:Main:Callback", "", "", &args);
    }
    let mut state = objects().lock().map_err(|_| "object registry poisoned".to_owned())?;
    if op == "new" && target == "Counter" {
        let value = args.strip_prefix('[').and_then(|value| value.strip_suffix(']'))
            .filter(|value| !value.is_empty()).ok_or("Counter requires one wire value")?;
        state.next = state.next.checked_add(1).ok_or("object handle exhausted")?;
        let id = state.next.to_string();
        state.values.insert(id.clone(), value.into());
        return Ok(format!("{{\"$ref\":\"{id}\",\"$type\":\"Counter\"}}"));
    }
    let value = state.values.get(&handle).ok_or("released or unknown object handle")?.clone();
    match (op.as_str(), member.as_str()) {
        ("get", "value") => Ok(value),
        ("set", "value") => {
            let value = args.strip_prefix('[').and_then(|value| value.strip_suffix(']'))
                .filter(|value| !value.is_empty()).ok_or("value requires one wire value")?;
            state.values.insert(handle, value.into());
            Ok("null".into())
        }
        ("invoke", "echo") => Ok(args),
        ("release", _) => { state.values.remove(&handle); Ok("null".into()) },
        _ => Err("unknown Rust object member or operation".into()),
    }
}

#[no_mangle]
pub unsafe extern "C" fn zsharp_rust_call_v2(
    host: *const ZSharpInteropHostV2,
    request: *const ZSharpInteropRequestV2,
    result: *const ZSharpInteropResultV2,
    error: *mut c_char,
    error_size: usize,
) -> i32 {
    match std::panic::catch_unwind(|| run(host, request)) {
        Ok(Ok(json)) => interop_emit(result, &json, error, error_size),
        Ok(Err(failure)) => { message(error, error_size, &failure); 0 },
        Err(_) => { message(error, error_size, "Rust interop panic"); 0 },
    }
}
