// UIKit / QuartzCore HLE.
#pragma once

#include <vector>

#include "hle/common.h"

namespace uikit {

void init();  // define classes (after foundation::init)

// Touch input from the platform, in portrait view points of the key window.
enum Phase { kBegan = 0, kMoved = 1, kStationary = 2, kEnded = 3, kCancelled = 4 };
struct TouchEvent {
    long long pointer_id;
    Phase phase;
    CGPoint location;
};
void dispatch_touches(const std::vector<TouchEvent>& events);

// Calls -layoutSubviews on views that need layout.
void layout_pass();

// Lifecycle notifications to the app delegate.
void app_will_resign_active();
void app_did_enter_background();
void app_will_enter_foreground();
void app_did_become_active();
void app_will_terminate();
void app_memory_warning();

// Accelerometer sample in g units (iOS axes).
void accelerometer_update(double x, double y, double z);

// Text input events for the focused UITextField.
void text_input_insert(const std::u16string& s);
void text_input_backspace();
void text_input_return();
// Result of the platform text dialog (accepted=false keeps the old text).
void text_input_finish(const std::u16string& text, bool accepted);
bool text_input_active();

// Result of platform::pick_image (cgimage == nil when cancelled).
void image_picked(id cgimage);

// CALayer helpers
CGRect layer_bounds(id layer);
double layer_contents_scale(id layer);

id ui_color(double r, double g, double b, double a);  // autoreleased UIColor
void color_components(id color, double out[4]);

}  // namespace uikit
