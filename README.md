# xCommander (`xc`)

A fast, lightweight, FAR Manager-inspired dual-panel TUI file manager and text editor for Linux, written in C11 and `ncursesw`.

> **Note:** Built primarily for personal daily use on **Arch Linux** with **Kitty** as the primary terminal emulator (leveraging the Kitty Keyboard Protocol for dynamic `Ctrl`/`Alt`/`Shift` function key bars and full modifier support across layouts), while remaining compatible with other modern Linux terminals (`xterm`, `konsole`, `xfce4-terminal`).

## Key Features

- **Dual-Panel File Management:** Full UTF-8 support, color-coded directories/executables/selections, detailed status bar, persistent panel state, and instant panel swap (`Ctrl+U`).
- **Drive & Mount Menu (`Alt+F1` / `Alt+F2`):** Quick switching between `$HOME`, `/`, mounted partitions (`/media`, `/run/media`, `/mnt`) with free space indicators, custom bookmarks (`~/.config/xcommander/bookmarks`), and safe USB unmount (`F8` / `Delete` via `udisksctl`).
- **Built-in TUI Text Editor (`F4` / `Shift+F4`):**
  - Modular syntax highlighting (`C/C++`, `Python`, `Shell`, `Config`) toggled with `F4`.
  - Matching bracket & quote highlighting, enclosing block jump (`Ctrl+]`), and block selection (`Alt+Shift+]`).
  - 64-step `Undo` / `Redo` (`Ctrl+Z` / `Ctrl+R`), interactive FAR-style replace (`Ctrl+F7`), search (`F7` / `Shift+F7`), visible whitespace (`F5`), and auto-indent (`F9`).
- **Built-in Text & Hex Viewer (`F3`):** Line wrapping (`F2`), Hex dump mode (`F4`), and search (`F7` / `Shift+F7`).
- **Smart Clipboard & Multiline Script Runner:** Copies files as `text/uri-list` (`Ctrl+C` / `Ctrl+X`), previews and executes multiline shell scripts (`Ctrl+V`) with automatic logging to Multiline History (`Alt+H`).
- **User Menu (`F2`) & Full Mouse Support:** Hierarchical user menus with hotkeys (`~/.config/xcommander/menu`), dynamic `F1`–`F10` modifier bar (`Ctrl`/`Alt`/`Shift`), and full mouse click/wheel support across all windows.

## Build & Install

```bash
make
sudo make install
```

## License

This project is licensed under the **BSD 3-Clause License** — see the [LICENSE](LICENSE) file for details.
