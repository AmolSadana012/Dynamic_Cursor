# Dynamic Cursor:

Dynamic Cursor is a small background Win32 application that enlarges the real
Windows arrow cursor while the user searches for it with rapid mouse motion.
It recognizes repeated left-right reversals and circular movement globally
through `WH_MOUSE_LL`; it does not draw an overlay.

## Behavior:

- Detection promotes the arrow through 48, 64, 80, and 96 px cursor sizes.
- When movement stops, the size decays one level at a time and returns to the
  saved cursor.
- The cursor is restored when the message loop exits, including hook setup
  failure, Ctrl+C/console shutdown, window close, and normal process shutdown.

## Build:

From a MinGW-w64 terminal:

```text
g++ -std=c++17 -Wall -Wextra -municode -mconsole -static -static-libgcc -static-libstdc++ main.cpp -o DynamicCursor.exe -luser32 -lgdi32
```

## Run:

Run `DynamicCursor.exe` from PowerShell and press Ctrl+C to stop it. The
console control handler posts a shutdown message, which restores the original
arrow cursor before exiting. Avoid forced termination because it bypasses that
cleanup path.
