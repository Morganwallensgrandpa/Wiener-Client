# Wiener-Client
## 1. Repo Description (the "About" field)

GitHub allows 350 characters. This is 179:

```
Fully custom, GPU-rendered client for Minecraft 1.21.11 (Fabric) — a C++ DLL injected into the running game, ImGui overlay on OpenGL, modules over JNI. Single-player only. v1.0.0.
```

Shorter alternative (105 characters, if you want a one-line teaser):

```
C++ utility client for Minecraft 1.21.11 (Fabric): ImGui overlay, MinHook injection, JNI modules. v1.0.0.
```

## 2. Suggested topics (About → gear icon → Topics)

```
minecraft  fabric  minecraft-client  imgui  minhook  jni  cpp20  dll-injection  opengl  windows
```

---

## 3. Long description (repo body / pinned release / README top)

```markdown
# Wiener Client v1.0.0

A fully custom, GPU-rendered utility client for **Minecraft 1.21.11 (Fabric)**.
Not a Fabric mod and not a resource pack: it is a **card in your own process** —
a C++ DLL injected into the running game, drawing an ImGui overlay on a hook of
the frame the game just finished.

**Wiener Client v1.0.0 · Minecraft 1.21.11 · Fabric · single player only.**

- **Overlay** — Dear ImGui on an OpenGL 3 back end, styled from scratch. Frosted
  glass, animated accents, soft glows; nothing blocky.
- **Injection** — MinHook inside the client, a plain
  `CreateRemoteThread` + `LoadLibraryW` loader outside it.
- **Modules** — toggled from an in-game menu and driven through JNI with one
  consistent bridge, so a module cannot drift from the way the others talk to
  the game.

Built against, and only against, **1.21.11**. This is a pinned build, not a
maintained one: it is not expected to follow the game to the next version.
```

---

## 4. Usage + injection text (a ready "Using it" section)

This can replace or extend the existing `## Using it` section in README.md.

```markdown
## Using Wiener Client v1.0.0 (Minecraft 1.21.11, Fabric)

### What you need

- **Minecraft 1.21.11, Fabric loader**, 64-bit Java.
- The two files from `build/bin/`, **kept side by side**:
  `Wiener Client.exe` (the injector) and `Wiener Client.dll` (the client).
- Windows. Run the injector **as administrator** if the game is running
  elevated — otherwise `OpenProcess` is denied and the injector says so.

### Injecting

1. Start **Minecraft 1.21.11 (Fabric)** and get into the game. Windowed is
   easiest; the injector has to be able to see the window.
2. Run **`Wiener Client.exe`** from the same folder as the DLL.
3. Wait for the green dot: the injector rescans once a second and shows
   **"Minecraft detected"** with the window title, pid and `javaw.exe`.
   It identifies the game as a **Java process** whose window is a `GLFW*`
   window or is titled `Minecraft <version>` — which is what stops the
   *Minecraft Launcher* from being mistaken for the game.
4. Press the big **INJECT** button. It animates through *INJECTING* and lands on
   a green checkmark with **"injected"**; the status line then reads
   **"injected — press RIGHT SHIFT in game"**.
5. In game, press **RIGHT SHIFT** to open the menu. Clicking modules in the
   sidebar toggles them; while the menu is open the client hands the mouse back
   to Windows so the pointer is free, and closing it hands the grab back to the
   game.

There is no launcher integration and no mod loader step — the DLL is written
into the already-running game process, so **inject after the game is up**, and
**reinject after every rebuild**: a loaded DLL cannot be replaced under a
running process.

### Where the logs are

Both halves keep a `.log` in the directory they were started from:

| File | Written by | Location |
|------|-----------|----------|
| `Wiener Client Injector.log` | the injector | next to the exe (so, `build/bin/` if you launch it there) |
| `Wiener Client.log` | the injected DLL | the game's working directory — i.e. the game's process, not the exe's folder |

The client logs its own file name and timestamp at boot
(`module Wiener Client.dll, written 2026-10-12 ...`), so "is the game running
the build I just made?" is one line in the log rather than a guessing game.

### If it does not work

| What you see | What it means |
|---|---|
| **"Waiting for Minecraft"**, no green dot | No matching window yet. The game must be running **and** its window visible; the launcher alone will never match, on purpose. |
| **"DLL not found next to the injector"** | `Wiener Client.dll` is not beside `Wiener Client.exe`. Keep the pair together. |
| **"OpenProcess denied (run as administrator?)"** | The game is elevated (or otherwise protected). Relaunch the injector as administrator. |
| **"target rejected the library"** | `LoadLibraryW` returned 0 inside the game. Almost always a bitness mismatch — the client is **x64**, so the game must be running 64-bit Java. |
| Button shows **RETRY** | The click failed; the exact reason is in `Wiener Client Injector.log`. |
| Menu opens but nothing is clickable | The GLFW cursor hooks did not install. The client log warns about this explicitly: `glfw.dll not loaded; the cursor will stay grabbed while the menu is open`. |
| Linker: `Permission denied` on `build/bin/Wiener Client.dll` | The game is still running with the DLL loaded. Close it, then rebuild. |

### Renaming it

The name lives in exactly two places that must agree: `NAME` in the Makefile and
`fc::kClientDllFile` in `src/common/brand.h`. Everything the user *reads* — the
menu header, the footer, the watermark, the injector's window title and both log
file names — comes from that header, so there is no third copy to keep in step.
Rename one without the other and the injector reports "DLL not found next to the
injector".
```

---
