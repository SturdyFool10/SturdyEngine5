//! Desktop services that belong to the process rather than to one window: file dialogs, message boxes, the screen saver
//! and a tray icon. They work the same whichever window provider (SDL3 or GLFW) the program's windows come from.
//!
//! Dialogs and trays are asynchronous: a file dialog gives you a [`PendingFileDialog`] to poll, and a [`Tray`] collects
//! menu clicks for [`Tray::take_clicks`]. Call [`pump_events`] once per frame if no window provider is pumping SDL for
//! you (the engine does this when a tray exists, but the call is cheap and harmless).

use sturdy_sys::window::ffi;

use crate::Error;

/// Delivers tray clicks and other desktop events. Cheap when no tray exists. Call from the main thread.
pub fn pump_events() {
    ffi::desktop_pump_events();
}

/// Allows or inhibits the screen saver / display blanking. Games turn it off for their lifetime.
pub fn set_screensaver_enabled(enabled: bool) -> Result<(), Error> {
    let s = ffi::desktop_set_screensaver_enabled(enabled);
    if s.ok { Ok(()) } else { Err(Error::new(s.error)) }
}

/// Whether the system may blank the screen right now.
pub fn screensaver_enabled() -> Result<bool, Error> {
    let mut enabled = false;
    let s = ffi::desktop_screensaver_enabled(&mut enabled);
    if s.ok { Ok(enabled) } else { Err(Error::new(s.error)) }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum MessageBoxKind {
    #[default]
    Information = 0,
    Warning,
    Error,
}

/// A modal message box. See [`message_box`].
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct MessageBox {
    pub kind: MessageBoxKind,
    pub title: String,
    pub message: String,
    /// Button labels, left to right; empty gives a single "OK".
    pub buttons: Vec<String>,
    /// The button Enter chooses (an index into `buttons`), or -1.
    pub default_button: i32,
    /// The button Escape chooses, or -1.
    pub escape_button: i32,
}

/// Shows the box and blocks until it is dismissed; returns the index of the pressed button (or -1 when closed another
/// way). Call it from the main thread.
pub fn message_box(options: &MessageBox) -> Result<i32, Error> {
    let r = ffi::desktop_message_box(
        options.kind as u8,
        &options.title,
        &options.message,
        &options.buttons,
        options.default_button,
        options.escape_button,
    );
    if r.ok { Ok(r.button) } else { Err(Error::new(r.error)) }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum FileDialogKind {
    #[default]
    OpenFile = 0,
    SaveFile,
    OpenFolder,
}

/// A file-type filter: a display name and the extensions it matches.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FileFilter {
    pub name: String,
    /// Extensions without the dot, separated by `;` (`"png;jpg;jpeg"`), or `"*"` for everything.
    pub pattern: String,
}

/// A file or folder dialog. See [`FileDialog::open`].
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct FileDialog {
    pub kind: FileDialogKind,
    pub title: String,
    /// Labels of the confirm / cancel buttons; empty uses the system's.
    pub accept_label: String,
    pub cancel_label: String,
    /// A folder, or a file path to preselect.
    pub default_location: String,
    pub filters: Vec<FileFilter>,
    pub allow_multiple: bool,
}

/// How a closed dialog ended.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FileDialogResult {
    Chosen { paths: Vec<std::path::PathBuf>, filter_index: Option<usize> },
    Cancelled,
    Failed(String),
}

/// A dialog that is open. Poll it from your frame loop.
#[derive(Debug)]
pub struct PendingFileDialog {
    ticket: u64,
}

impl FileDialog {
    /// Opens the dialog and returns at once. Fails when the system cannot show one.
    pub fn open(&self) -> Result<PendingFileDialog, Error> {
        let names: Vec<String> = self.filters.iter().map(|f| f.name.clone()).collect();
        let patterns: Vec<String> = self.filters.iter().map(|f| f.pattern.clone()).collect();
        let mut error = String::new();
        let ticket = ffi::desktop_file_dialog_start(
            self.kind as u8,
            &self.title,
            &self.accept_label,
            &self.cancel_label,
            &self.default_location,
            &names,
            &patterns,
            self.allow_multiple,
            &mut error,
        );
        if ticket == 0 { Err(Error::new(error)) } else { Ok(PendingFileDialog { ticket }) }
    }
}

impl FileDialog {
    /// Opens the dialog as a child of the window `window_id` (a surface's `window_id`): it stays above it and is attached
    /// to it by the window system. Like [`open`](Self::open) it returns at once.
    pub fn open_for(&self, engine: &mut crate::Engine<'_>, window_id: u64) -> Result<PendingFileDialog, Error> {
        let names: Vec<String> = self.filters.iter().map(|f| f.name.clone()).collect();
        let patterns: Vec<String> = self.filters.iter().map(|f| f.pattern.clone()).collect();
        let mut error = String::new();
        let ticket = ffi::desktop_file_dialog_start_for(
            engine.view_mut(),
            window_id,
            self.kind as u8,
            &self.title,
            &self.accept_label,
            &self.cancel_label,
            &self.default_location,
            &names,
            &patterns,
            self.allow_multiple,
            &mut error,
        );
        if ticket == 0 { Err(Error::new(error)) } else { Ok(PendingFileDialog { ticket }) }
    }
}

impl PendingFileDialog {
    /// The outcome once the user has closed the dialog; `None` while it is still open. Returns `Some` exactly once.
    pub fn poll(&self) -> Option<FileDialogResult> {
        let outcome = ffi::desktop_file_dialog_take(self.ticket);
        if !outcome.ready {
            return None;
        }
        Some(if outcome.failed {
            FileDialogResult::Failed(outcome.error)
        } else if outcome.cancelled {
            FileDialogResult::Cancelled
        } else {
            FileDialogResult::Chosen {
                paths: outcome.paths.into_iter().map(std::path::PathBuf::from).collect(),
                filter_index: usize::try_from(outcome.filter_index).ok(),
            }
        })
    }
}

/// One row of a tray menu.
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct TrayEntry {
    /// Reported by [`Tray::take_clicks`] when the row is clicked; unique within the menu.
    pub id: u32,
    pub label: String,
    /// A separator line; every other field is ignored.
    pub separator: bool,
    /// Shows a check mark that toggles when clicked.
    pub checkable: bool,
    pub checked: bool,
    pub enabled: bool,
    /// Non-empty makes this row open a submenu (and the row itself is not clickable).
    pub submenu: Vec<TrayEntry>,
}

impl TrayEntry {
    pub fn item(id: u32, label: impl Into<String>) -> Self {
        Self { id, label: label.into(), enabled: true, ..Self::default() }
    }

    pub fn separator() -> Self {
        Self { separator: true, enabled: true, ..Self::default() }
    }
}

fn flatten(entries: &[TrayEntry], parent: i32, out: &mut Vec<ffi::TrayEntryInfo>) {
    for entry in entries {
        let index = out.len() as i32;
        out.push(ffi::TrayEntryInfo {
            id: entry.id,
            label: entry.label.clone(),
            separator: entry.separator,
            checkable: entry.checkable,
            checked: entry.checked,
            enabled: entry.enabled,
            parent,
        });
        flatten(&entry.submenu, index, out);
    }
}

/// A notification-area / menu-bar icon with a menu. Dropping it removes the icon.
#[derive(Debug)]
pub struct Tray {
    handle: u64,
}

impl Tray {
    /// Creates a tray icon from tightly packed RGBA8 pixels (`width * height * 4` bytes). Fails when the system has no
    /// tray (most Linux desktops need a StatusNotifier host).
    pub fn create(width: u32, height: u32, rgba: &[u8], tooltip: &str, entries: &[TrayEntry]) -> Result<Self, Error> {
        let mut rows = Vec::new();
        flatten(entries, -1, &mut rows);
        let r = ffi::tray_create(width, height, rgba, tooltip, &rows);
        if r.ok { Ok(Self { handle: r.handle }) } else { Err(Error::new(r.error)) }
    }

    pub fn set_icon(&mut self, width: u32, height: u32, rgba: &[u8]) -> Result<(), Error> {
        let s = ffi::tray_set_icon(self.handle, width, height, rgba);
        if s.ok { Ok(()) } else { Err(Error::new(s.error)) }
    }

    pub fn set_tooltip(&mut self, tooltip: &str) -> Result<(), Error> {
        let s = ffi::tray_set_tooltip(self.handle, tooltip);
        if s.ok { Ok(()) } else { Err(Error::new(s.error)) }
    }

    /// Replaces the whole menu.
    pub fn set_menu(&mut self, entries: &[TrayEntry]) -> Result<(), Error> {
        let mut rows = Vec::new();
        flatten(entries, -1, &mut rows);
        let s = ffi::tray_set_menu(self.handle, &rows);
        if s.ok { Ok(()) } else { Err(Error::new(s.error)) }
    }

    /// The ids of the rows clicked since the last call. Clicks arrive while events are pumped ([`pump_events`]).
    pub fn take_clicks(&mut self) -> Vec<u32> {
        ffi::tray_take_clicks(self.handle)
    }
}

impl Drop for Tray {
    fn drop(&mut self) {
        let _ = ffi::tray_destroy(self.handle);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn menus_flatten_with_parent_indices() {
        let menu = [
            TrayEntry::item(1, "Open"),
            TrayEntry { submenu: vec![TrayEntry::item(10, "Low"), TrayEntry::item(11, "High")], enabled: true, label: "Quality".into(), ..TrayEntry::default() },
            TrayEntry::separator(),
            TrayEntry::item(2, "Quit"),
        ];
        let mut rows = Vec::new();
        flatten(&menu, -1, &mut rows);
        assert_eq!(rows.len(), 6);
        assert_eq!(rows.iter().map(|r| r.parent).collect::<Vec<_>>(), vec![-1, -1, 1, 1, -1, -1]);
        assert_eq!(rows[2].label, "Low");
        assert!(rows[4].separator);
    }
}
