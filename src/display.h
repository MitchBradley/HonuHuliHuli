// Character display on the Wemos/LOLIN OLED shield (SSD1306, 64x48, I2C):
// 6 lines of 8 characters with an 8x8 font.  Only the UI task uses it.
#pragma once

#include <string>
#include <vector>

namespace display {

constexpr int COLS = 8;
constexpr int ROWS = 6;

// Returns false (and every later call is a no-op) if there is no display.
bool init();

// Replace the whole screen; rows beyond the list are blank.  Redraws only
// if something changed.
void show(const std::vector<std::string>& rows);

// Big digits (21x32, up to 3) centered across the top 4 text rows, and one
// line of normal text centered on the bottom row.
void show_big(int number, const std::string& bottom);

// s truncated to the width and padded to center it
std::string centered(const std::string& s);

// Split text into display-width rows, breaking after a dot or before a
// slash where possible (for IP addresses and URLs).
std::vector<std::string> wrap(std::string text);

}  // namespace display
