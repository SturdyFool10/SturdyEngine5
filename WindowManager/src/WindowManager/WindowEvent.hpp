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
    };

} // namespace SFT::WindowManager
