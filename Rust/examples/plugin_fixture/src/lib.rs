use std::io::Write;

use sturdy_plugin::{sturdy_plugin, Engine, Module, ModuleOptions, Plugin};

fn record(line: String) {
    if let Ok(path) = std::env::var("STURDY_PLUGIN_TEST_LOG") {
        if let Ok(mut file) = std::fs::OpenOptions::new().create(true).append(true).open(path) {
            let _ = writeln!(file, "{line}");
        }
    }
}

struct Logged(&'static str);

impl Module for Logged {
    fn on_engine_initialized(&self, _engine: Engine<'_>) -> bool {
        record(format!("init:{}", self.0));
        true
    }
    fn on_shutdown(&self, _engine: Engine<'_>) {
        record(format!("shutdown:{}", self.0));
    }
}

impl Drop for Logged {
    fn drop(&mut self) {
        record(format!("destroy:{}", self.0));
    }
}

fn unloaded(mut unload: sturdy_plugin::Unload) {
    record(format!("unload state={}", if unload.has_state() { "set" } else { "none" }));
    if let Some(generation) = unload.take_state::<u32>() {
        record(format!("unload generation={generation}"));
    }
}

sturdy_plugin!(describe, unload: unloaded);

fn describe(plugin: &mut Plugin<'_>) -> sturdy_plugin::Result {
    record(format!("load path={}", plugin.path()));
    plugin.set_info("rust", "fixture", "1.0");

    // A generation counter carried across a reload.
    let generation: u32 = match plugin.take_prior_state::<u32>() {
        Some(previous) => {
            record(format!("prior generation={previous}"));
            previous + 1
        }
        None => 1,
    };
    plugin.set_state(generation);

    plugin.add_module("fixture.a", ModuleOptions::overlay_only(), Logged("fixture.a"))?;
    plugin.add_module("fixture.b", ModuleOptions::overlay_only().after(&["fixture.a"]), Logged("fixture.b"))?;
    if std::env::var("STURDY_PLUGIN_TEST_REFUSE").as_deref() == Ok("1") {
        return Err(sturdy_plugin::Error::new(sturdy_plugin::ffi::STURDY_ERROR_CALLBACK_FAILED, "refused by the test"));
    }
    Ok(())
}
