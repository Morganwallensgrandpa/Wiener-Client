#pragma once

namespace fc::core {

// Brings the client online: logging, module catalog and the frame hook. Called
// once from the injected thread.
void start();
void stop();

// Called every frame from the swap hook (render thread). Drives the JNI
// bridge, module ticks and keybind polling.
void on_render_frame();

// Runs the enabled modules' per-frame drawing. Must be called inside ImGui's
// frame, before the menu paints, so module visuals end up underneath the menu.
void draw_modules(float w, float h);

// JNIEnv* for the calling thread, or null before the JVM is reachable.
void* env();

} // namespace fc::core
