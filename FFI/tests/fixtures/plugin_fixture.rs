//! The same plugin as `PluginFixture.c`, written in Rust against nothing but the layout of `Sturdy.h`: no engine crate, no linking
//! to the engine. It logs to the file named by STURDY_PLUGIN_TEST_LOG so `PluginFfiTest` can run its identical checks against a
//! library built by a different toolchain. (A real Rust plugin would use the `sturdy-sys` bindings instead of redeclaring structs.)
//!
//! Build: rustc --crate-type cdylib -C panic=abort -O plugin_fixture.rs -o libplugin_fixture.so

#![allow(private_interfaces)]

use std::ffi::{c_char, c_void, CStr, CString};
use std::io::Write;
use std::ptr;

type Bool = u8;

#[repr(C)]
#[derive(Clone, Copy)]
struct Engine {
    token: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct GameLogic {
    struct_size: u32,
    reserved: u32,
    user_data: *mut c_void,
    on_engine_initialized: Option<extern "C" fn(Engine, *mut c_void) -> Bool>,
    request_render_frame: Option<extern "C" fn(Engine, u64, *const c_void, u64, *mut c_void) -> Bool>,
    on_shutdown: Option<extern "C" fn(Engine, *mut c_void)>,
    destroy: Option<extern "C" fn(*mut c_void)>,
}

#[repr(C)]
struct GameModule {
    struct_size: u32,
    after_count: u32,
    name: *const c_char,
    after: *const *const c_char,
    contributes_scene: Bool,
    reserved: [u8; 7],
    logic: GameLogic,
}

#[repr(C)]
struct PluginHost {
    struct_size: u32,
    abi_major: u32,
    abi_minor: u32,
    reserved: u32,
    context: *mut c_void,
    path: *const c_char,
    prior_state: *mut c_void,
    add_module: extern "C" fn(*mut c_void, *const GameModule) -> i32,
    set_info: extern "C" fn(*mut c_void, *const c_char, *const c_char, *const c_char) -> i32,
    set_state: extern "C" fn(*mut c_void, *mut c_void),
}

const STURDY_OK: i32 = 0;
const STURDY_ERROR_INTERNAL: i32 = 9;
const STURDY_ERROR_CALLBACK_FAILED: i32 = 6;
const STURDY_ERROR_INVALID_ARGUMENT: i32 = 1;
const PLUGIN_UNLOAD: i32 = 2;

fn record(line: String) {
    if let Ok(path) = std::env::var("STURDY_PLUGIN_TEST_LOG") {
        if let Ok(mut file) = std::fs::OpenOptions::new().create(true).append(true).open(path) {
            let _ = writeln!(file, "{line}");
        }
    }
}

fn name_of(user_data: *mut c_void) -> String {
    unsafe { CStr::from_ptr(user_data as *const c_char) }.to_string_lossy().into_owned()
}

extern "C" fn on_initialized(_: Engine, user_data: *mut c_void) -> Bool {
    record(format!("init:{}", name_of(user_data)));
    1
}

extern "C" fn on_shutdown(_: Engine, user_data: *mut c_void) {
    record(format!("shutdown:{}", name_of(user_data)));
}

extern "C" fn on_destroy(user_data: *mut c_void) {
    record(format!("destroy:{}", name_of(user_data)));
}

fn add(host: &PluginHost, name: &'static CStr, dependency: Option<&'static CStr>, with_callbacks: bool) -> bool {
    let after = [dependency.map_or(ptr::null(), |d| d.as_ptr())];
    let module = GameModule {
        struct_size: std::mem::size_of::<GameModule>() as u32,
        after_count: dependency.is_some() as u32,
        name: name.as_ptr(),
        after: if dependency.is_some() { after.as_ptr() } else { ptr::null() },
        contributes_scene: 0,
        reserved: [0; 7],
        logic: GameLogic {
            struct_size: std::mem::size_of::<GameLogic>() as u32,
            reserved: 0,
            user_data: name.as_ptr() as *mut c_void,
            on_engine_initialized: if with_callbacks { Some(on_initialized) } else { None },
            request_render_frame: None,
            on_shutdown: if with_callbacks { Some(on_shutdown) } else { None },
            destroy: Some(on_destroy),
        },
    };
    (host.add_module)(host.context, &module) == STURDY_OK
}

#[no_mangle]
pub extern "C" fn sturdy_plugin_entry(host: *const PluginHost, reason: i32, state: *mut c_void) -> i32 {
    let host = unsafe { &*host };
    if reason == PLUGIN_UNLOAD {
        record(format!("unload state={}", if state.is_null() { "none" } else { "set" }));
        if !state.is_null() {
            let generation = unsafe { Box::from_raw(state as *mut i32) };
            record(format!("unload generation={generation}"));
        }
        return STURDY_OK;
    }

    let path = unsafe { CStr::from_ptr(host.path) }.to_string_lossy().into_owned();
    record(format!("load path={path}"));

    let language = CString::new("rust").unwrap();
    let plugin_name = CString::new("fixture").unwrap();
    (host.set_info)(host.context, language.as_ptr(), plugin_name.as_ptr(), ptr::null());

    // Generation counter carried across a reload through prior_state (plain data: an i32 in a Box).
    let generation = if host.prior_state.is_null() {
        1
    } else {
        let previous = unsafe { Box::from_raw(host.prior_state as *mut i32) };
        record(format!("prior generation={previous}"));
        *previous + 1
    };
    (host.set_state)(host.context, Box::into_raw(Box::new(generation)) as *mut c_void);

    if !add(host, c"fixture.a", None, true) || !add(host, c"fixture.b", Some(c"fixture.a"), false) {
        return STURDY_ERROR_INTERNAL;
    }
    if (host.add_module)(host.context, ptr::null()) != STURDY_ERROR_INVALID_ARGUMENT {
        return STURDY_ERROR_INTERNAL;
    }
    if std::env::var("STURDY_PLUGIN_TEST_REFUSE").as_deref() == Ok("1") {
        return STURDY_ERROR_CALLBACK_FAILED;
    }
    STURDY_OK
}
