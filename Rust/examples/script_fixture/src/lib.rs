//! Self-test for `sturdy-script`: a Rust script, a registered language adapter and a host function, against the real engine.
//!
//! The host (FFI/tests/ScriptRustFixtureTest.cpp) owns a running game and drives it: `setup`, some engine updates, `probe`, an update, `verify`,
//! `unload`, an update, `finish`. Each export returns the number of failed checks and appends their descriptions to `message`.

use std::sync::Mutex;

use sturdy_script::*;

static LOG: Mutex<Vec<String>> = Mutex::new(Vec::new());
static FAILURES: Mutex<Vec<String>> = Mutex::new(Vec::new());

fn log(entry: impl Into<String>) {
    LOG.lock().unwrap().push(entry.into());
}

fn count(entry: &str) -> usize {
    LOG.lock().unwrap().iter().filter(|e| e.as_str() == entry).count()
}

fn check(ok: bool, what: &str) {
    if !ok {
        FAILURES.lock().unwrap().push(what.to_string());
    }
}

/// Logs its lifecycle under `label`; remembers a counter that survives a reload through save/restore.
struct Counter {
    label: String,
    total: u32,
}

impl Script for Counter {
    fn start(&mut self, _engine: ScriptEngine<'_>) -> core::result::Result<(), String> {
        log(format!("{} start", self.label));
        Ok(())
    }
    fn update(&mut self, _engine: ScriptEngine<'_>, _tick: Tick) {
        self.total += 1;
        log(format!("{} update", self.label));
    }
    fn stop(&mut self, _engine: ScriptEngine<'_>) {
        log(format!("{} stop", self.label));
    }
    fn save_state(&mut self) -> Option<Vec<u8>> {
        Some(self.total.to_le_bytes().to_vec())
    }
    fn restore_state(&mut self, state: &[u8]) -> bool {
        match <[u8; 4]>::try_from(state) {
            Ok(bytes) => {
                self.total = u32::from_le_bytes(bytes);
                log(format!("{} restored {}", self.label, self.total));
                true
            }
            Err(_) => false,
        }
    }
    fn call(&mut self, function: &str, args: &[u8]) -> core::result::Result<Vec<u8>, String> {
        match function {
            "echo" => Ok(args.to_vec()),
            "total" => Ok(self.total.to_string().into_bytes()),
            "host_double" => call_host_function("double", args),
            _ => Err(format!("no function '{function}'")),
        }
    }
    fn on_message(&mut self, message: Message) {
        log(format!("{} message {}:{}:{}", self.label, message.from, message.topic, String::from_utf8_lossy(&message.payload)));
    }
}

/// A language whose "source" is the label the script logs under; a source of `broken` fails to load.
struct Mini;

impl ScriptLanguage for Mini {
    fn name(&self) -> String {
        "rustmini".into()
    }
    fn extensions(&self) -> Vec<String> {
        vec![".rmini".into()]
    }
    fn load(&self, source: &ScriptSource) -> core::result::Result<Box<dyn Script>, String> {
        let label = String::from_utf8_lossy(&source.code).into_owned();
        if label == "broken" {
            return Err("syntax error in broken".into());
        }
        Ok(Box::new(Counter { label, total: 0 }))
    }
}

fn finish_checks(message: *mut u8, capacity: usize) -> i32 {
    let failures: Vec<String> = FAILURES.lock().unwrap().drain(..).collect();
    if capacity > 0 && !message.is_null() {
        let text = failures.join("; ");
        let bytes = text.as_bytes();
        let n = bytes.len().min(capacity - 1);
        unsafe {
            core::ptr::copy_nonoverlapping(bytes.as_ptr(), message, n);
            *message.add(n) = 0;
        }
    }
    failures.len() as i32
}

fn engine_of<'a>(engine: ffi::SturdyEngine) -> ScriptEngine<'a> {
    engine::from_raw(engine)
}

#[unsafe(no_mangle)]
pub extern "C" fn sturdy_script_fixture_setup(engine: ffi::SturdyEngine, message: *mut u8, capacity: usize) -> i32 {
    let engine = engine_of(engine);
    check(register_language(Mini).is_ok(), "register the language");
    check(register_language(Mini).is_err(), "a duplicate language is refused");
    check(languages().iter().any(|l| l == "rustmini"), "the language is listed");
    check(register_host_function("double", |args| Ok([args, args].concat())).is_ok(), "register a host function");
    check(call_host_function("double", b"ab").as_deref() == Ok(&b"abab"[..]), "call a host function");
    check(call_host_function("nope", b"").is_err(), "an unknown host function fails");

    let native = ScriptModule::native("rs_native", &ScriptOptions::default(), Counter { label: "native".into(), total: 0 });
    match native {
        Ok(module) => {
            let raw = module.into_raw();
            check(unsafe { ffi::sturdy_module_request_load(engine.raw(), &raw) } == ffi::STURDY_OK, "load the native script");
        }
        Err(error) => check(false, &format!("create the native script: {error}")),
    }
    let source = ScriptSource::new("rs_lang", "rustmini", b"lang".to_vec());
    check(engine::load(engine, &source, &ScriptOptions::default().after(&["rs_native"])).is_ok(), "load through the adapter");
    finish_checks(message, capacity)
}

#[unsafe(no_mangle)]
pub extern "C" fn sturdy_script_fixture_probe(engine: ffi::SturdyEngine, message: *mut u8, capacity: usize) -> i32 {
    let engine = engine_of(engine);
    check(count("native start") == 1 && count("lang start") == 1, "both scripts started once");
    check(count("native update") >= 2 && count("lang update") >= 2, "both scripts ticked");
    let names = engine::names(engine);
    check(names.contains(&"rs_native".to_string()) && names.contains(&"rs_lang".to_string()), "both scripts are listed");
    check(engine::call(engine, "rs_native", "echo", b"hi").as_deref() == Ok(&b"hi"[..]), "call a Rust script");
    check(engine::call(engine, "rs_lang", "host_double", b"xy").as_deref() == Ok(&b"xyxy"[..]), "a script calls a host function");
    check(engine::call(engine, "rs_lang", "missing", b"").is_err(), "an unknown function fails");
    check(engine::call(engine, "ghost", "echo", b"").is_err(), "an unknown script fails");
    check(engine::info(engine, "rs_lang").is_some_and(|i| i.language == "rustmini" && i.generation == 1 && i.healthy), "info");
    check(engine::send(engine, "", "rs_lang", "greet", b"hello").is_ok(), "send a message");
    check(engine::subscribe(engine, "rs_native", "news").is_ok(), "subscribe");
    check(engine::publish(engine, "rs_lang", "news", b"extra") == Ok(1), "publish reaches one subscriber");
    // A broken replacement leaves the running version alone and reports the error at once.
    check(engine::reload(engine, &ScriptSource::new("rs_lang", "rustmini", b"broken".to_vec())).is_err(), "a broken reload is refused");
    check(engine::reload_native(engine, "rs_native", Counter { label: "native2".into(), total: 0 }).is_ok(), "reload a Rust script");
    finish_checks(message, capacity)
}

#[unsafe(no_mangle)]
pub extern "C" fn sturdy_script_fixture_verify(engine: ffi::SturdyEngine, message: *mut u8, capacity: usize) -> i32 {
    let engine = engine_of(engine);
    check(count("lang message :greet:hello") == 1, "the direct message arrived");
    // Queued in the same step as the reload, so whichever version is live at the boundary receives it.
    check(count("native message rs_lang:news:extra") + count("native2 message rs_lang:news:extra") == 1, "the published message arrived");
    check(count("native stop") == 1 && count("native2 start") == 1, "the old version stopped and the new one started");
    check(LOG.lock().unwrap().iter().any(|e| e.starts_with("native2 restored ")), "state moved to the new version");
    check(engine::info(engine, "rs_native").is_some_and(|i| i.generation == 2), "generation advanced");
    check(engine::unload(engine, "rs_lang").is_ok(), "unload");
    finish_checks(message, capacity)
}

#[unsafe(no_mangle)]
pub extern "C" fn sturdy_script_fixture_finish(engine: ffi::SturdyEngine, message: *mut u8, capacity: usize) -> i32 {
    let engine = engine_of(engine);
    check(count("lang stop") == 1, "the unloaded script stopped");
    check(!engine::names(engine).contains(&"rs_lang".to_string()), "it is gone");
    check(unregister_host_function("double").is_ok(), "unregister the host function");
    check(unregister_language("rustmini").is_ok(), "unregister the language");
    finish_checks(message, capacity)
}
