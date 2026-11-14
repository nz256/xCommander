# xCommander (`xc`)

A minimalist, high-speed two-panel TUI file manager for Linux written in C using `ncursesw`.

![xCommander Screenshot](docs/screenshot.png)

## Features
* **FAR Manager-style interface:** Classic two-panel layout with dual-column view and color highlighting for directories, executables, and selected items.
* **Batch file operations:** Select multiple items (`Shift+Arrows`, `Insert`) and perform batch copy (`F5`), move/rename (`F6`), make folder (`F7`), or delete (`F8`).
* **Smart clipboard integration:** Copy, cut, and paste both files and text (`Ctrl+C`, `Ctrl+X`, `Ctrl+V`, `Ctrl+Y`, `Shift+Insert`) synced with the system clipboard (`wl-clipboard`, `xclip`, `xsel`).
* **Built-in command line & terminal mode:** Execute shell commands directly, toggle full terminal mode (`Ctrl+O`), browse persistent command history (`Ctrl+E` / `Ctrl+X`), and edit lines with word jumping (`Ctrl+Arrows`) and text selection (`Shift+Arrows`, `Ctrl+Shift+Arrows`).
* **Viewer & editor:** Built-in help (`F1`), file viewer (`F3` via `less`), editor (`F4` via `$EDITOR`), and quick file creation (`Shift+F4`).

## Build & Install

```bash
make
sudo make install
```
