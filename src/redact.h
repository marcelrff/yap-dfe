#pragma once

#include <cstddef>

// Removes the text under some rectangles of one page, for real: the glyphs
// are deleted from the page's content, not covered. Images, line art and
// annotations stay. Uses MuPDF's redaction.
//
// `rects` holds `count` rectangles as x, y, width, height, normalised to the
// page as displayed (rotation applied, origin top-left). `page` is 0-based.
// On failure, returns false with a message in `error`.
bool redactText(const char *in, const char *password, int page, const double *rects, int count, const char *out, char *error,
                std::size_t errorSize);
