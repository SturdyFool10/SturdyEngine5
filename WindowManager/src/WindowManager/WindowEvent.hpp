#pragma once

#include <Foundation/Foundation.hpp>

#include <WindowManager/Keyboard.hpp>
#include <WindowManager/WindowGeometry.hpp>

namespace SFT::WindowManager {

    struct WindowResize {
        WindowExtent previous = {};
        WindowExtent current = {};
        WindowExtent framebuffer = {};
        bool framebuffer_changed = false;
    };

    enum class WindowEventKind {
        CloseRequested,
        Moved,
        Resized,
        FramebufferResized,
        FocusGained,
        FocusLost,
        MouseEntered,
        MouseLeft,
        KeyPressed,
        KeyReleased,
        TextInput,
        TextEditing,
        MouseMoved,
        MouseButtonPressed,
        MouseButtonReleased,
        MouseWheel,
        MouseLocked,
        MouseUnlocked,
        /// The window's content scale (see `Window::content_scale()`) changed, e.g. it moved to a
        /// monitor with a different UI scale. The new value is in `WindowEvent::content_scale`.
        ContentScaleChanged,

        /// The window was maximized / minimized / restored from either (by the user, the OS, or a request).
        Maximized,
        Minimized,
        Restored,
        /// Drag and drop onto the window. `DropBegin` starts a drag over the window; `DropPosition` tracks the pointer
        /// (`WindowEvent::position`, window coordinates); each `DropFile`/`DropText` is one dropped item, whose path or
        /// text is taken from `take_dropped_items` (events carry no strings); `DropComplete` ends the drop (or the drag
        /// left the window).
        DropBegin,
        DropPosition,
        DropFile,
        DropText,
        DropComplete,
        /// The OS light/dark preference changed; read it with `Window::system_theme()`.
        SystemThemeChanged,
        /// The window became visible / hidden (shown, hidden, or the OS mapped/unmapped it).
        Shown,
        Hidden,
        /// The OS asks for a redraw of the window's contents (uncovered, restored, ...).
        Exposed,
        EnteredFullscreen,
        LeftFullscreen,
        /// A monitor was connected, disconnected, or changed its mode/arrangement; re-read `Window::displays()`.
        DisplayAdded,
        DisplayRemoved,
        DisplayChanged,
        /// The clipboard contents changed (by any program).
        ClipboardUpdated,
        /// Fingers on a touch screen or pad (`WindowEvent::touch`).
        TouchDown,
        TouchUp,
        TouchMotion,
        TouchCanceled,
        /// A pen (`WindowEvent::pen`): hover enters/leaves range, the tip touches/lifts, it moves, a barrel button
        /// changes, or one axis (pressure, tilt, ...) changes.
        PenProximityIn,
        PenProximityOut,
        PenDown,
        PenUp,
        PenMotion,
        PenButtonDown,
        PenButtonUp,
        PenAxis,
    };

    struct WindowKeyboardEvent {
                                                                                               
        i32 key = 0;
        i32 scancode = 0;
        u32 modifiers = 0;
        bool repeat = false;

                                                                                                  
                                                                                
        KeyboardKey key_code = KeyboardKey::Unknown;
    };

    struct WindowTextInputEvent {
        char utf8[32] = {};
    };

                                                                                                  
                                                                                                    
                                                                                                 
                                                                                                    
                                                                                                  
                                                                                                       
                                                                                                    
                                                                                                     
                                                                                                    
                                                                                           
                                                                                                  
                                                                                                      
                                                                                              
                                                                                
       
                                                                                                   
                                                                                                      
                                                                                                      
    struct WindowTextEditingEvent {
        char utf8[512] = {};
                                                                                                     
                                                                                                      
                                                                                                     
                                                                                                        
        i32 cursor = 0;
        i32 selection_length = 0;
    };

    struct WindowMouseMoveEvent {
        f32 x = 0.0F;
        f32 y = 0.0F;
        f32 delta_x = 0.0F;
        f32 delta_y = 0.0F;
        u32 buttons = 0;
    };

                                                                                               
                                                                                                    
                                                                                                    
                                                                                                        
                                                                                                      
                                                                                                 
                                                                                                     
                                                                                               
    enum class MouseButton : u8 {
        Unknown,
        Left,
        Middle,
        Right,
        Extra1,
        Extra2,
        Extra3,
        Extra4,
        Extra5,
        Extra6,
        Extra7,
        Extra8,
        Extra9,
        Extra10,
        Extra11,
        Extra12,
    };

    struct WindowMouseButtonEvent {
                                                                                                
                                                           
        u8 button = 0;
        u8 clicks = 1;
        f32 x = 0.0F;
        f32 y = 0.0F;

                                                                                               
        MouseButton button_code = MouseButton::Unknown;
    };

    struct WindowMouseWheelEvent {
        f32 x = 0.0F;
        f32 y = 0.0F;
        f32 mouse_x = 0.0F;
        f32 mouse_y = 0.0F;
    };

    /// One finger. Positions are in window coordinates.
    struct WindowTouchEvent {
        u64 device = 0;
        u64 finger = 0;
        f32 x = 0.0f;
        f32 y = 0.0f;
        f32 dx = 0.0f;
        f32 dy = 0.0f;
        /// 0 to 1.
        f32 pressure = 0.0f;
    };

    /// `WindowPenEvent::axis`, in SDL's order.
    enum class PenAxis : u8 {
        Pressure,
        XTilt,
        YTilt,
        Distance,
        Rotation,
        Slider,
        TangentialPressure,
    };

    /// One pen. Positions are in window coordinates; which fields matter depends on the event kind.
    struct WindowPenEvent {
        u32 pen = 0;
        f32 x = 0.0f;
        f32 y = 0.0f;
        /// `PenAxis` events: the axis and its new value (pressure 0..1, tilt in degrees, ...).
        PenAxis axis = PenAxis::Pressure;
        f32 value = 0.0f;
        /// `PenButtonDown/Up`: the barrel button (1-based).
        u8 button = 0;
        /// `PenDown/Up`: the eraser end is in use.
        bool eraser = false;
    };

    struct WindowEvent {
        WindowEventKind kind = WindowEventKind::CloseRequested;

                                                                                             
                                                                                                    
                                                                                               
                                                                                                       
                                                                                                       
                                                                                                        
                                                                                                  
        u64 timestamp_ns = 0;

        WindowPosition position = {};
        WindowResize resize = {};
        WindowKeyboardEvent keyboard = {};
        WindowTextInputEvent text = {};
        WindowTextEditingEvent editing = {};
        WindowMouseMoveEvent mouse_move = {};
        WindowMouseButtonEvent mouse_button = {};
        WindowMouseWheelEvent mouse_wheel = {};
        WindowTouchEvent touch = {};
        WindowPenEvent pen = {};
        f32 content_scale = 1.0F;
    };

} // namespace SFT::WindowManager
