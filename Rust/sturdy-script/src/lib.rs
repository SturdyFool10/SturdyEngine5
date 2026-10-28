//! Scripts: units of game logic in any language, running side by side in one game.
//!
//! A *script* is a game-logic module whose body is a [`Script`]: something with a lifecycle (start, a tick every update, optional frame
//! contribution, stop), that can be called by name, receives messages and can hand its state to its own replacement when hot-reloaded.
//!
//! * **Rust** (or any compiled language): implement [`Script`] and make a module with [`ScriptModule::native`].
//! * **Interpreted languages** (JavaScript, Lua, Python ...): the application brings the interpreter. Implement [`ScriptLanguage`] — it turns
//!   source text or bytecode into a `Box<dyn Script>` that drives the interpreter — and [`register_language`] it. The engine ships no
//!   interpreter, only this seam. Scripts then load by name or file extension ([`ScriptModule::from_source`], [`engine::load`]).
//!
//! ```ignore
//! use sturdy_script::{Script, ScriptEngine, ScriptLanguage, ScriptSource, Tick};
//!
//! struct LuaScript { /* a lua_State */ }
//! impl Script for LuaScript {
//!     fn update(&mut self, _engine: ScriptEngine, tick: Tick) { /* call the script's `update(dt)` */ }
//! }
//! struct Lua;
//! impl ScriptLanguage for Lua {
//!     fn name(&self) -> String { "lua".into() }
//!     fn extensions(&self) -> Vec<String> { vec![".lua".into()] }
//!     fn load(&self, source: &ScriptSource) -> Result<Box<dyn Script>, String> { /* luaL_loadbuffer ... */ Ok(Box::new(LuaScript {})) }
//! }
//! sturdy_script::register_language(Lua)?;
//! ```
//!
//! **Threading.** Every callback of every script runs with one lock held, so a [`Script`] needs `Send` but not `Sync` and an interpreter that
//! is not thread-safe never sees two threads. `start`, `update`, messages and `stop` run on the thread that drives engine updates;
//! `request_render_frame` may run on a render thread. `update` runs at the update boundary, when nothing else touches the world, so a script
//! may use the whole engine API directly. See `plans/scripting.md`.
//!
//! The engine's functions are declared in [`ffi`] and are not linked here (an executable links the engine library; a plugin leaves them for the loader).

pub use sturdy_net::ffi;

use core::ffi::{c_char, c_void, CStr};
use std::ffi::CString;
use std::panic::{catch_unwind, AssertUnwindSafe};

/// A failed call into the engine.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Error {
    /// The `SturdyResult` code (`ffi::STURDY_ERROR_*`).
    pub code: i32,
    pub message: String,
}

impl core::fmt::Display for Error {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "{} (code {})", self.message, self.code)
    }
}

impl std::error::Error for Error {}

pub type Result<T = ()> = core::result::Result<T, Error>;

fn check(code: i32) -> Result {
    if code == ffi::STURDY_OK {
        return Ok(());
    }
    let pointer = unsafe { ffi::sturdy_last_error_message() };
    let message = if pointer.is_null() { String::new() } else { unsafe { CStr::from_ptr(pointer) }.to_string_lossy().into_owned() };
    Err(Error { code, message })
}

fn cstring(text: &str) -> CString {
    CString::new(text.replace('\0', "")).unwrap_or_default()
}

fn text(pointer: *const c_char) -> String {
    if pointer.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(pointer) }.to_string_lossy().into_owned()
    }
}

fn slice<'a>(data: *const u8, size: u64) -> &'a [u8] {
    if data.is_null() || size == 0 {
        &[]
    } else {
        unsafe { core::slice::from_raw_parts(data, size as usize) }
    }
}

unsafe fn sink(sink: *mut ffi::SturdyBytesSink, data: &[u8]) {
    unsafe { ffi::sturdy_bytes_sink_set(sink, data.as_ptr(), data.len() as u64) };
}

fn guard<T>(what: &str, on_panic: T, body: impl FnOnce() -> T) -> T {
    match catch_unwind(AssertUnwindSafe(body)) {
        Ok(value) => value,
        Err(_) => {
            eprintln!("sturdy-script: panic in {what}; the callback reports failure");
            on_panic
        }
    }
}

/// The engine, valid only inside the callback that received it (do not store it).
#[derive(Clone, Copy)]
pub struct ScriptEngine<'a> {
    raw: ffi::SturdyEngine,
    _scope: core::marker::PhantomData<&'a ()>,
}

impl ScriptEngine<'_> {
    /// The raw handle, for the `ffi::sturdy_*` functions.
    pub fn raw(&self) -> ffi::SturdyEngine {
        self.raw
    }
}

/// One engine update.
#[derive(Debug, Clone, Copy, Default)]
pub struct Tick {
    pub delta_seconds: f64,
    pub elapsed_seconds: f64,
    pub tick: u64,
}

/// A message between scripts (or from the host application).
#[derive(Debug, Clone, Default)]
pub struct Message {
    /// The sending script; empty when the host application sent it.
    pub from: String,
    pub topic: String,
    pub payload: Vec<u8>,
}

/// A frame a script may contribute to, valid only inside [`Script::request_render_frame`]. Configure it with the `ffi::sturdy_frame_*` setters.
pub struct FrameRequest<'a> {
    pub engine: ScriptEngine<'a>,
    pub surface: ffi::SturdySurface,
    pub input: &'a ffi::SturdyFrameInput,
    pub frame: ffi::SturdyFrame,
}

/// A running script. All methods are called with the script lock held, never concurrently with another script's. Every method has a
/// do-nothing default.
pub trait Script: Send + 'static {
    /// Once, when the script starts (as a module of the game). Return an error to abort the load; the message reaches the caller.
    fn start(&mut self, _engine: ScriptEngine<'_>) -> core::result::Result<(), String> {
        Ok(())
    }
    /// Every engine update, at the update boundary, in module start order.
    fn update(&mut self, _engine: ScriptEngine<'_>, _tick: Tick) {}
    /// Contribute to a frame; return `true` to render what you configured. May run on a render thread.
    fn request_render_frame(&mut self, _request: FrameRequest<'_>) -> bool {
        false
    }
    /// Once, when the script is unloaded or replaced, after [`Script::save_state`].
    fn stop(&mut self, _engine: ScriptEngine<'_>) {}
    /// Hot reload: the state to hand to the replacement, if any.
    fn save_state(&mut self) -> Option<Vec<u8>> {
        None
    }
    /// The previous version's state, before `start`. Return `false` to refuse it (the reload fails and the script stays unloaded).
    fn restore_state(&mut self, _state: &[u8]) -> bool {
        true
    }
    /// Calls a function the script exposes.
    fn call(&mut self, function: &str, _args: &[u8]) -> core::result::Result<Vec<u8>, String> {
        Err(format!("no function '{function}'"))
    }
    /// A message sent to this script or published on a topic it subscribed to.
    fn on_message(&mut self, _message: Message) {}
}

/// Where a script comes from.
#[derive(Debug, Clone, Default)]
pub struct ScriptSource {
    /// Unique; the script is a module of this name.
    pub name: String,
    /// The language adapter ("lua", "js"). Empty: chosen from `path`'s extension.
    pub language: String,
    /// A file the adapter may read, and the extension that picks a language.
    pub path: String,
    /// The source text or bytecode. Empty: the adapter loads `path`.
    pub code: Vec<u8>,
    /// Adapter-specific settings.
    pub options: Vec<(String, String)>,
}

impl ScriptSource {
    pub fn new(name: impl Into<String>, language: impl Into<String>, code: impl Into<Vec<u8>>) -> Self {
        Self { name: name.into(), language: language.into(), code: code.into(), ..Self::default() }
    }

    fn lower<'a>(&'a self, strings: &'a mut Vec<CString>, pairs: &'a mut Vec<ffi::SturdyKeyValue>) -> ffi::SturdyScriptSource {
        strings.reserve(3 + 2 * self.options.len());
        let keep = |value: &str, strings: &mut Vec<CString>| {
            strings.push(cstring(value));
            strings.last().map_or(core::ptr::null(), |c| c.as_ptr())
        };
        let name = keep(&self.name, strings);
        let language = keep(&self.language, strings);
        let path = keep(&self.path, strings);
        for (key, value) in &self.options {
            let (key, value) = (keep(key, strings), keep(value, strings));
            pairs.push(ffi::SturdyKeyValue { name: key, value });
        }
        let mut source = ffi::SturdyScriptSource::default();
        source.struct_size = size_of::<ffi::SturdyScriptSource>() as u32;
        source.name = name;
        source.language = language;
        source.path = path;
        source.code = if self.code.is_empty() { core::ptr::null() } else { self.code.as_ptr() };
        source.code_size = self.code.len() as u64;
        source.options = if pairs.is_empty() { core::ptr::null() } else { pairs.as_ptr() };
        source.option_count = pairs.len() as u32;
        source
    }
}

fn source_from(c: &ffi::SturdyScriptSource) -> ScriptSource {
    let options = if c.options.is_null() {
        Vec::new()
    } else {
        unsafe { core::slice::from_raw_parts(c.options, c.option_count as usize) }.iter().map(|kv| (text(kv.name), text(kv.value))).collect()
    };
    ScriptSource { name: text(c.name), language: text(c.language), path: text(c.path), code: slice(c.code, c.code_size).to_vec(), options }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Scripts as C callbacks
// ------------------------------------------------------------------------------------------------------------------------------

struct Boxed(Box<dyn Script>);

fn engine_of<'a>(raw: ffi::SturdyEngine) -> ScriptEngine<'a> {
    ScriptEngine { raw, _scope: core::marker::PhantomData }
}

extern "C" fn script_start(engine: ffi::SturdyEngine, user_data: *mut c_void, error: *mut ffi::SturdyBytesSink) -> u8 {
    let script = unsafe { &mut *(user_data as *mut Boxed) };
    match catch_unwind(AssertUnwindSafe(|| script.0.start(engine_of(engine)))) {
        Ok(Ok(())) => 1,
        Ok(Err(message)) => {
            unsafe { sink(error, message.as_bytes()) };
            0
        }
        Err(_) => {
            unsafe { sink(error, b"the script panicked in start") };
            0
        }
    }
}

extern "C" fn script_update(engine: ffi::SturdyEngine, tick: *const ffi::SturdyScriptTick, user_data: *mut c_void) {
    let script = unsafe { &mut *(user_data as *mut Boxed) };
    let tick = unsafe { &*tick };
    guard("update", (), || script.0.update(engine_of(engine), Tick { delta_seconds: tick.delta_seconds, elapsed_seconds: tick.elapsed_seconds, tick: tick.tick }));
}

extern "C" fn script_frame(engine: ffi::SturdyEngine, surface: ffi::SturdySurface, input: *const ffi::SturdyFrameInput, frame: ffi::SturdyFrame, user_data: *mut c_void) -> u8 {
    let script = unsafe { &mut *(user_data as *mut Boxed) };
    guard("request_render_frame", false, || script.0.request_render_frame(FrameRequest { engine: engine_of(engine), surface, input: unsafe { &*input }, frame })) as u8
}

extern "C" fn script_stop(engine: ffi::SturdyEngine, user_data: *mut c_void) {
    let script = unsafe { &mut *(user_data as *mut Boxed) };
    guard("stop", (), || script.0.stop(engine_of(engine)));
}

extern "C" fn script_save(user_data: *mut c_void, out: *mut ffi::SturdyBytesSink) -> u8 {
    let script = unsafe { &mut *(user_data as *mut Boxed) };
    match guard("save_state", None, || script.0.save_state()) {
        Some(state) => {
            unsafe { sink(out, &state) };
            1
        }
        None => 0,
    }
}

extern "C" fn script_restore(user_data: *mut c_void, state: *const u8, size: u64) -> u8 {
    let script = unsafe { &mut *(user_data as *mut Boxed) };
    guard("restore_state", false, || script.0.restore_state(slice(state, size))) as u8
}

extern "C" fn script_call(user_data: *mut c_void, function: *const c_char, args: *const u8, args_size: u64, result: *mut ffi::SturdyBytesSink, error: *mut ffi::SturdyBytesSink) -> u8 {
    let script = unsafe { &mut *(user_data as *mut Boxed) };
    let name = text(function);
    match catch_unwind(AssertUnwindSafe(|| script.0.call(&name, slice(args, args_size)))) {
        Ok(Ok(bytes)) => {
            unsafe { sink(result, &bytes) };
            1
        }
        Ok(Err(message)) => {
            unsafe { sink(error, message.as_bytes()) };
            0
        }
        Err(_) => {
            unsafe { sink(error, b"the script panicked") };
            0
        }
    }
}

extern "C" fn script_message(user_data: *mut c_void, message: *const ffi::SturdyScriptMessage) {
    let script = unsafe { &mut *(user_data as *mut Boxed) };
    let message = unsafe { &*message };
    guard("on_message", (), || {
        script.0.on_message(Message { from: text(message.from), topic: text(message.topic), payload: slice(message.payload, message.payload_size).to_vec() })
    });
}

extern "C" fn script_destroy(user_data: *mut c_void) {
    drop(unsafe { Box::from_raw(user_data as *mut Boxed) });
}

fn instance_of(script: Box<dyn Script>) -> ffi::SturdyScriptInstance {
    let mut instance = ffi::SturdyScriptInstance::default();
    instance.struct_size = size_of::<ffi::SturdyScriptInstance>() as u32;
    instance.user_data = Box::into_raw(Box::new(Boxed(script))) as *mut c_void;
    instance.start = Some(script_start);
    instance.update = Some(script_update);
    instance.request_render_frame = Some(script_frame);
    instance.stop = Some(script_stop);
    instance.save_state = Some(script_save);
    instance.restore_state = Some(script_restore);
    instance.call = Some(script_call);
    instance.on_message = Some(script_message);
    instance.destroy = Some(script_destroy);
    instance
}

// ------------------------------------------------------------------------------------------------------------------------------
// Modules
// ------------------------------------------------------------------------------------------------------------------------------

/// Where a script sits in the game.
#[derive(Debug, Clone, Default)]
pub struct ScriptOptions {
    /// Names of modules that must start before this one (and shut down after it).
    pub after: Vec<String>,
    /// The script may supply the scene for a frame (default: it never replaces the scene).
    pub contributes_scene: bool,
}

impl ScriptOptions {
    pub fn after(mut self, names: &[&str]) -> Self {
        self.after = names.iter().map(|n| (*n).to_owned()).collect();
        self
    }
}

/// A script ready to be handed to the engine as a game module: pass [`ScriptModule::into_raw`]'s result to `sturdy_runtime_run_modules` /
/// `sturdy_module_request_load`, or give it to a plugin ([`ScriptModule::take_raw`]). Dropping it unused destroys the script.
pub struct ScriptModule {
    raw: Option<ffi::SturdyGameModule>,
}

impl ScriptModule {
    /// A script built from Rust: no language adapter involved.
    pub fn native(name: &str, options: &ScriptOptions, script: impl Script) -> Result<ScriptModule> {
        let name_c = cstring(name);
        let after: Vec<CString> = options.after.iter().map(|a| cstring(a)).collect();
        let after_ptrs: Vec<*const c_char> = after.iter().map(|a| a.as_ptr()).collect();
        let mut desc = ffi::SturdyNativeScriptDesc::default();
        desc.struct_size = size_of::<ffi::SturdyNativeScriptDesc>() as u32;
        desc.name = name_c.as_ptr();
        desc.instance = instance_of(Box::new(script));
        desc.after = if after_ptrs.is_empty() { core::ptr::null() } else { after_ptrs.as_ptr() };
        desc.after_count = after_ptrs.len() as u32;
        desc.contributes_scene = options.contributes_scene as u8;
        let mut module = ffi::SturdyGameModule::default();
        let code = unsafe { ffi::sturdy_script_module_create_native(&desc, &mut module) };
        if code != ffi::STURDY_OK {
            // Not taken: the instance is still ours to destroy.
            if let Some(destroy) = desc.instance.destroy {
                unsafe { destroy(desc.instance.user_data) };
            }
        }
        check(code)?;
        Ok(ScriptModule { raw: Some(module) })
    }

    /// A script that a registered language adapter loads when the game starts.
    pub fn from_source(source: &ScriptSource, options: &ScriptOptions) -> Result<ScriptModule> {
        let (mut strings, mut pairs) = (Vec::new(), Vec::new());
        let lowered = source.lower(&mut strings, &mut pairs);
        let after: Vec<CString> = options.after.iter().map(|a| cstring(a)).collect();
        let after_ptrs: Vec<*const c_char> = after.iter().map(|a| a.as_ptr()).collect();
        let mut desc = ffi::SturdyScriptModuleDesc::default();
        desc.struct_size = size_of::<ffi::SturdyScriptModuleDesc>() as u32;
        desc.source = lowered;
        desc.after = if after_ptrs.is_empty() { core::ptr::null() } else { after_ptrs.as_ptr() };
        desc.after_count = after_ptrs.len() as u32;
        desc.contributes_scene = options.contributes_scene as u8;
        let mut module = ffi::SturdyGameModule::default();
        check(unsafe { ffi::sturdy_script_module_create(&desc, &mut module) })?;
        Ok(ScriptModule { raw: Some(module) })
    }

    /// Hands the module to whatever consumes it (the engine then owns it and destroys it). The strings inside stay valid until then.
    pub fn into_raw(mut self) -> ffi::SturdyGameModule {
        self.raw.take().unwrap_or_default()
    }

    /// Like [`ScriptModule::into_raw`] without consuming `self`'s ownership semantics for plugin hosts: the module is taken exactly once.
    pub fn take_raw(&mut self) -> Option<ffi::SturdyGameModule> {
        self.raw.take()
    }
}

impl Drop for ScriptModule {
    fn drop(&mut self) {
        if let Some(mut module) = self.raw.take() {
            unsafe { ffi::sturdy_script_module_discard(&mut module) };
        }
    }
}

// ------------------------------------------------------------------------------------------------------------------------------
// Language adapters and host functions
// ------------------------------------------------------------------------------------------------------------------------------

/// Turns source into a running [`Script`]: the seam an application uses to bring an interpreter into the engine.
pub trait ScriptLanguage: Send + Sync + 'static {
    /// The name `ScriptSource::language` refers to ("lua", "js").
    fn name(&self) -> String;
    /// File extensions (with the dot) that select this language.
    fn extensions(&self) -> Vec<String> {
        Vec::new()
    }
    /// Parses or compiles `source`. The script must not start running yet. Called with the script lock held.
    fn load(&self, source: &ScriptSource) -> core::result::Result<Box<dyn Script>, String>;
}

struct Adapter {
    language: Box<dyn ScriptLanguage>,
}

extern "C" fn language_load(user_data: *mut c_void, source: *const ffi::SturdyScriptSource, out_instance: *mut ffi::SturdyScriptInstance, error: *mut ffi::SturdyBytesSink) -> u8 {
    let adapter = unsafe { &*(user_data as *const Adapter) };
    let source = source_from(unsafe { &*source });
    match catch_unwind(AssertUnwindSafe(|| adapter.language.load(&source))) {
        Ok(Ok(script)) => {
            unsafe { *out_instance = instance_of(script) };
            1
        }
        Ok(Err(message)) => {
            unsafe { sink(error, message.as_bytes()) };
            0
        }
        Err(_) => {
            unsafe { sink(error, b"the language adapter panicked while loading") };
            0
        }
    }
}

extern "C" fn language_destroy(user_data: *mut c_void) {
    drop(unsafe { Box::from_raw(user_data as *mut Adapter) });
}

/// Registers a language adapter for the whole process (before the runtime starts, or while it runs). An error when a language of that name exists.
pub fn register_language(language: impl ScriptLanguage) -> Result {
    let name = cstring(&language.name());
    let extensions: Vec<CString> = language.extensions().iter().map(|e| cstring(e)).collect();
    let extension_ptrs: Vec<*const c_char> = extensions.iter().map(|e| e.as_ptr()).collect();
    let user_data = Box::into_raw(Box::new(Adapter { language: Box::new(language) })) as *mut c_void;
    let mut table = ffi::SturdyScriptLanguage::default();
    table.struct_size = size_of::<ffi::SturdyScriptLanguage>() as u32;
    table.name = name.as_ptr();
    table.extensions = if extension_ptrs.is_empty() { core::ptr::null() } else { extension_ptrs.as_ptr() };
    table.extension_count = extension_ptrs.len() as u32;
    table.user_data = user_data;
    table.load = Some(language_load);
    table.destroy = Some(language_destroy);
    let code = unsafe { ffi::sturdy_script_language_register(&table) };
    if code != ffi::STURDY_OK {
        drop(unsafe { Box::from_raw(user_data as *mut Adapter) }); // not taken
    }
    check(code)
}

/// Forgets a language. Scripts already running keep working until they are unloaded.
pub fn unregister_language(name: &str) -> Result {
    check(unsafe { ffi::sturdy_script_language_unregister(cstring(name).as_ptr()) })
}

/// Names of the registered languages.
pub fn languages() -> Vec<String> {
    let mut count = 0u32;
    unsafe { ffi::sturdy_script_language_count(&mut count) };
    (0..count)
        .filter_map(|index| {
            let mut length = 0u64;
            if unsafe { ffi::sturdy_script_language_name(index, core::ptr::null_mut(), 0, &mut length) } != ffi::STURDY_OK {
                return None;
            }
            let mut buffer = vec![0u8; length as usize];
            unsafe { ffi::sturdy_script_language_name(index, buffer.as_mut_ptr() as *mut c_char, buffer.len() as u64, &mut length) };
            if buffer.last() == Some(&0) {
                buffer.pop();
            }
            Some(String::from_utf8_lossy(&buffer).into_owned())
        })
        .collect()
}

type HostFn = Box<dyn Fn(&[u8]) -> core::result::Result<Vec<u8>, String> + Send + Sync + 'static>;

extern "C" fn host_trampoline(user_data: *mut c_void, args: *const u8, args_size: u64, result: *mut ffi::SturdyBytesSink, error: *mut ffi::SturdyBytesSink) -> u8 {
    let function = unsafe { &*(user_data as *const HostFn) };
    match catch_unwind(AssertUnwindSafe(|| function(slice(args, args_size)))) {
        Ok(Ok(bytes)) => {
            unsafe { sink(result, &bytes) };
            1
        }
        Ok(Err(message)) => {
            unsafe { sink(error, message.as_bytes()) };
            0
        }
        Err(_) => {
            unsafe { sink(error, b"the host function panicked") };
            0
        }
    }
}

extern "C" fn host_destroy(user_data: *mut c_void) {
    drop(unsafe { Box::from_raw(user_data as *mut HostFn) });
}

/// Registers a function every script can call by name, over byte strings. How a script reaches it is the adapter's business.
pub fn register_host_function(name: &str, function: impl Fn(&[u8]) -> core::result::Result<Vec<u8>, String> + Send + Sync + 'static) -> Result {
    let user_data = Box::into_raw(Box::new(Box::new(function) as HostFn)) as *mut c_void;
    let code = unsafe { ffi::sturdy_script_register_host_function(cstring(name).as_ptr(), Some(host_trampoline), user_data, Some(host_destroy)) };
    if code != ffi::STURDY_OK {
        drop(unsafe { Box::from_raw(user_data as *mut HostFn) });
    }
    check(code)
}

pub fn unregister_host_function(name: &str) -> Result {
    check(unsafe { ffi::sturdy_script_unregister_host_function(cstring(name).as_ptr()) })
}

fn finished(code: i32, op: ffi::SturdyAsyncOp) -> core::result::Result<Vec<u8>, String> {
    if code != ffi::STURDY_OK {
        return Err(check(code).unwrap_err().message);
    }
    let mut status = 0;
    unsafe { ffi::sturdy_op_status(op, &mut status) };
    let result = if status == ffi::STURDY_OP_OK {
        let mut length = 0u64;
        unsafe { ffi::sturdy_op_bytes(op, core::ptr::null_mut(), 0, &mut length) };
        let mut bytes = vec![0u8; length as usize];
        if !bytes.is_empty() {
            unsafe { ffi::sturdy_op_bytes(op, bytes.as_mut_ptr(), bytes.len() as u64, &mut length) };
        }
        Ok(bytes)
    } else {
        let mut length = 0u64;
        unsafe { ffi::sturdy_op_error_message(op, core::ptr::null_mut(), 0, &mut length) };
        let mut buffer = vec![0u8; length as usize];
        unsafe { ffi::sturdy_op_error_message(op, buffer.as_mut_ptr() as *mut c_char, buffer.len() as u64, &mut length) };
        if buffer.last() == Some(&0) {
            buffer.pop();
        }
        Err(String::from_utf8_lossy(&buffer).into_owned())
    };
    unsafe { ffi::sturdy_op_release(op) };
    result
}

/// Calls a host function (what an adapter does on a script's behalf).
pub fn call_host_function(name: &str, args: &[u8]) -> core::result::Result<Vec<u8>, String> {
    let mut op = ffi::SturdyAsyncOp::default();
    let code = unsafe { ffi::sturdy_script_call_host_function(cstring(name).as_ptr(), args.as_ptr(), args.len() as u64, &mut op) };
    finished(code, op)
}

// ------------------------------------------------------------------------------------------------------------------------------
// Working with the running game's scripts
// ------------------------------------------------------------------------------------------------------------------------------

/// What `info` reports about a loaded script.
#[derive(Debug, Clone, Default)]
pub struct ScriptInfo {
    pub language: String,
    pub path: String,
    /// 1 for the first version loaded under this name, +1 for each reload.
    pub generation: u64,
    pub healthy: bool,
    pub failure: String,
}

/// Operations on the scripts of a running engine. Take the [`ScriptEngine`] a callback received.
pub mod engine {
    use super::*;

    /// Adds a script that a registered language adapter loads, at the next update boundary.
    pub fn load(engine: ScriptEngine<'_>, source: &ScriptSource, options: &ScriptOptions) -> Result {
        let (mut strings, mut pairs) = (Vec::new(), Vec::new());
        let lowered = source.lower(&mut strings, &mut pairs);
        let after: Vec<CString> = options.after.iter().map(|a| cstring(a)).collect();
        let after_ptrs: Vec<*const c_char> = after.iter().map(|a| a.as_ptr()).collect();
        let mut desc = ffi::SturdyScriptModuleDesc::default();
        desc.struct_size = size_of::<ffi::SturdyScriptModuleDesc>() as u32;
        desc.source = lowered;
        desc.after = if after_ptrs.is_empty() { core::ptr::null() } else { after_ptrs.as_ptr() };
        desc.after_count = after_ptrs.len() as u32;
        desc.contributes_scene = options.contributes_scene as u8;
        check(unsafe { ffi::sturdy_script_load(engine.raw, &desc) })
    }

    /// Hot-reloads a script: the new version loads first (a parse error leaves the old one running and is returned here), then the old one's state
    /// moves to it at the next update boundary.
    pub fn reload(engine: ScriptEngine<'_>, source: &ScriptSource) -> Result {
        let (mut strings, mut pairs) = (Vec::new(), Vec::new());
        let lowered = source.lower(&mut strings, &mut pairs);
        check(unsafe { ffi::sturdy_script_reload(engine.raw, &lowered) })
    }

    /// Hot-reloads a script with a Rust implementation.
    pub fn reload_native(engine: ScriptEngine<'_>, name: &str, script: impl Script) -> Result {
        let instance = instance_of(Box::new(script));
        let code = unsafe { ffi::sturdy_script_reload_native(engine.raw, cstring(name).as_ptr(), &instance) };
        if code != ffi::STURDY_OK {
            if let Some(destroy) = instance.destroy {
                unsafe { destroy(instance.user_data) };
            }
        }
        check(code)
    }

    pub fn unload(engine: ScriptEngine<'_>, name: &str) -> Result {
        check(unsafe { ffi::sturdy_script_unload(engine.raw, cstring(name).as_ptr()) })
    }

    /// Names of the loaded scripts, in start order.
    pub fn names(engine: ScriptEngine<'_>) -> Vec<String> {
        let mut count = 0u32;
        unsafe { ffi::sturdy_script_count(engine.raw, &mut count) };
        (0..count)
            .filter_map(|index| {
                let mut length = 0u64;
                if unsafe { ffi::sturdy_script_name(engine.raw, index, core::ptr::null_mut(), 0, &mut length) } != ffi::STURDY_OK {
                    return None;
                }
                let mut buffer = vec![0u8; length as usize];
                unsafe { ffi::sturdy_script_name(engine.raw, index, buffer.as_mut_ptr() as *mut c_char, buffer.len() as u64, &mut length) };
                if buffer.last() == Some(&0) {
                    buffer.pop();
                }
                Some(String::from_utf8_lossy(&buffer).into_owned())
            })
            .collect()
    }

    pub fn info(engine: ScriptEngine<'_>, name: &str) -> Option<ScriptInfo> {
        let mut op = ffi::SturdyAsyncOp::default();
        if unsafe { ffi::sturdy_script_info(engine.raw, cstring(name).as_ptr(), &mut op) } != ffi::STURDY_OK {
            return None;
        }
        let field = |key: &str| -> String {
            let key = cstring(key);
            let mut length = 0u64;
            if unsafe { ffi::sturdy_op_field(op, key.as_ptr(), core::ptr::null_mut(), 0, &mut length) } != ffi::STURDY_OK {
                return String::new();
            }
            let mut buffer = vec![0u8; length as usize];
            unsafe { ffi::sturdy_op_field(op, key.as_ptr(), buffer.as_mut_ptr() as *mut c_char, buffer.len() as u64, &mut length) };
            if buffer.last() == Some(&0) {
                buffer.pop();
            }
            String::from_utf8_lossy(&buffer).into_owned()
        };
        let info = ScriptInfo { language: field("language"), path: field("path"), generation: field("generation").parse().unwrap_or(0), healthy: field("healthy") == "1", failure: field("failure") };
        unsafe { ffi::sturdy_op_release(op) };
        Some(info)
    }

    /// Calls a function of a script now. Safe from any thread (it waits for the script lock).
    pub fn call(engine: ScriptEngine<'_>, script: &str, function: &str, args: &[u8]) -> core::result::Result<Vec<u8>, String> {
        let mut op = ffi::SturdyAsyncOp::default();
        let code = unsafe { ffi::sturdy_script_call(engine.raw, cstring(script).as_ptr(), cstring(function).as_ptr(), args.as_ptr(), args.len() as u64, &mut op) };
        finished(code, op)
    }

    /// Queues a message for `to`, delivered at the next update boundary. `from` empty: the host application.
    pub fn send(engine: ScriptEngine<'_>, from: &str, to: &str, topic: &str, payload: &[u8]) -> Result {
        let from = (!from.is_empty()).then(|| cstring(from));
        check(unsafe { ffi::sturdy_script_send(engine.raw, from.as_ref().map_or(core::ptr::null(), |f| f.as_ptr()), cstring(to).as_ptr(), cstring(topic).as_ptr(), payload.as_ptr(), payload.len() as u64) })
    }

    /// Queues a message for every script subscribed to `topic` except `from`; returns how many will get it.
    pub fn publish(engine: ScriptEngine<'_>, from: &str, topic: &str, payload: &[u8]) -> Result<u32> {
        let from = (!from.is_empty()).then(|| cstring(from));
        let mut receivers = 0u32;
        check(unsafe {
            ffi::sturdy_script_publish(engine.raw, from.as_ref().map_or(core::ptr::null(), |f| f.as_ptr()), cstring(topic).as_ptr(), payload.as_ptr(), payload.len() as u64, &mut receivers)
        })?;
        Ok(receivers)
    }

    pub fn subscribe(engine: ScriptEngine<'_>, script: &str, topic: &str) -> Result {
        check(unsafe { ffi::sturdy_script_subscribe(engine.raw, cstring(script).as_ptr(), cstring(topic).as_ptr()) })
    }

    pub fn unsubscribe(engine: ScriptEngine<'_>, script: &str, topic: &str) -> Result {
        check(unsafe { ffi::sturdy_script_unsubscribe(engine.raw, cstring(script).as_ptr(), cstring(topic).as_ptr()) })
    }

    /// Wraps a raw handle (for tests and for hosts that hold one).
    pub fn from_raw<'a>(raw: ffi::SturdyEngine) -> ScriptEngine<'a> {
        engine_of(raw)
    }
}
