// Services the HLE layer needs from the Android/SDL platform layer.
#pragma once

#include <string>

#include "hle/common.h"

namespace platform {

// Logs crashes (with game-relative addresses) to logcat and crash.txt.
void install_crash_handler();

// Pixel size of a CAEAGLLayer's drawable.
void drawable_size(id layer, int* w, int* h);

// Make the game's GL context current on the calling thread (or release it).
void gl_make_current(bool current);

// Show the game's drawable texture (portrait w*h, iOS orientation) on screen.
void present_frame(unsigned tex, int w, int h);

// Runs the main loop: SDL events, run loop timers, layout. Does not return
// until the app quits.
void run_main_loop();

void open_url(const std::string& url);

// Soft keyboard / text input for UITextField.
void start_text_input(const std::u16string& initial, const std::u16string& placeholder, bool secure);
void stop_text_input();

// Gallery picker for UIImagePickerController; result goes to uikit::image_picked.
void pick_image();

}  // namespace platform
