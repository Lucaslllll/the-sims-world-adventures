// CoreGraphics HLE: bitmap contexts, images, paths and text drawing.
#pragma once

#include <string>

#include "hle/common.h"

namespace cg {

void init();  // define CF-style classes (CGImage, CGContext, CGColor...)

// CGImage
id image_from_file(const std::string& path);  // +1, nullptr on failure
id image_from_rgba(const uint8_t* rgba, int w, int h);  // +1, premultiplied RGBA
int image_width(id img);
int image_height(id img);

// UIGraphics context stack
id current_context();
void push_context(id ctx);
void pop_context();
id begin_image_context(double w, double h, double scale);  // pushes
id image_from_current_context();  // +1 CGImage
void end_image_context();

// Drawing into a context (UIKit helpers)
void draw_image(id ctx, id cgimage, CGRect rect);

// Text (UIFont described by size/bold)
struct FontDesc {
    double size;
    bool bold;
};
CGSize measure_text(const std::u16string& s, FontDesc f);
void draw_text(id ctx, const std::u16string& s, CGPoint origin, FontDesc f);

}  // namespace cg
