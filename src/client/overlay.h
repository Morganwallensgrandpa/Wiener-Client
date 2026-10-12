#pragma once
#include <windows.h>

namespace fc::overlay {

// Called from the swap hook the first time it runs, with the device context the
// game is about to present. Builds the ImGui context against the game's own GL
// context and subclasses the game's window procedure for input.
//
// Returns false if the frame is not ready yet; the caller simply tries again on
// the next frame.
bool init(HDC dc);

// Called on every SwapBuffers, before the real one: blurs the frame the game
// just finished (while the menu is visible) and draws the menu/HUD over it.
void frame(HDC dc);

void shutdown();
bool initialized();

} // namespace fc::overlay
