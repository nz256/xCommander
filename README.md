# xCommander (`xc`)

**English** | [Русский](README.ru.md)

A fast, lightweight dual-pane terminal file manager for Linux inspired by FAR Manager and Midnight Commander, written in pure C with `ncurses`. Primarily developed, tested, and used daily on **Arch Linux** with the **Kitty** terminal emulator.

![xCommander Screenshot](docs/screenshot.png)

## Features

- **Dual-Pane Navigation & Real-Time Updates**: Instant directory browsing with `inotify` auto-refresh, custom sorting (`Ctrl+F3`..`Ctrl+F6` with left-aligned header indicator), drive/bookmark menus (`Alt+F1` / `Alt+F2`) with USB safe-unmount, and toggleable 32-color auto file colorization by extension (`Alt+F12`).
- **File Search by Mask & Content (`Alt+F7`)**: Fast recursive search supporting multi-masks (`*.c;*.h`), case-sensitive/insensitive text search with line numbers, and instant actions (`Enter` Go to, `F3` View, `F4` Edit at matched line, `Ctrl+J` Insert path).
- **Virtual Archive VFS**: Enter archives seamlessly (`Enter` or `Ctrl+PgDn` on `.zip`, `.jar`, `.apk`, `.whl`, `.epub`, `.docx`, `.tar`, `.tgz`, `.tar.gz`, `.tar.xz`, `.tar.bz2`, `.tar.zst`, `.7z`, `.rar`), view (`F3`), edit with auto-update (`F4`), extract (`F5`), delete inside archives (`F8`), and pack files (`Shift+F1` / `Alt+F5`).
- **Built-in Viewer & IDE-like TUI Editor**: Text/Hex file viewer (`F3`) and full-featured TUI editor (`F4` / `Shift+F4`) with syntax highlighting, `Save As` (`Shift+F2`), cursor position memory per file, line/block duplication (`Ctrl+Shift+D`), moving lines up/down (`Ctrl+Shift+Up/Down`), line/block commenting (`Ctrl+/`), smart auto-indent (`Enter`) & block indent (`Tab` / `Shift+Tab`), full-line copy/cut without selection (`Ctrl+C` / `Ctrl+X`), and FAR-style persistent keyboard macros (`Ctrl+.` to record/stop & assign any hotkey).
- **4 Persistent History Journals & Dynamic 12-Key F-Bar**:
  - `Alt+F8` / `Ctrl+H`: Command History (512 items with timestamp & directory; live filter, Run / Insert / Jump / Delete)
  - `Alt+F9` / `Ctrl+G`: File History (512 viewed/edited files with saved cursor position; Edit / View / Jump / Delete)
  - `Alt+F10` / `Ctrl+D`: Folder History (512 visited steps; live filter, Go / Other panel / Insert / Delete; plus `Alt+Backspace` / `Alt+Shift+Backspace` Back/Forward)
  - `Alt+F11` / `Ctrl+L`: Multiline Script History (Run / Edit in built-in editor / Delete)
  - Hold `Alt` / `Ctrl` / `Shift` to dynamically switch the 12-button bottom F-bar.
- **Inline History Autosuggest & Command-to-Editor Output**: Typing in the command line opens a live prefix-matching history popup from the 1st character (`Up`/`Down` navigate, `Right`/click fill, `Esc` dismiss); press `Shift+Enter` or `Alt+O` to run a command and open its output directly in the built-in `F4` editor.
- **Smart Clipboard & Native Terminal Scrollback**: URI-list file copy/paste (`Ctrl+C` / `Ctrl+X` / `Ctrl+V`), multiline script paste dialog, quick prefix search (`Alt+<char>` with auto EN/RU layout), live-filtered Help (`F1`), and full shell toggle (`Ctrl+O`) with automatic mouse release for native wheel scrolling and text selection.

## Build & Run

```bash
make
./xc
```

### CLI Options

- `xc` — launch dual-pane file manager in current directory
- `xc -e [LINE[:COL]] [FILE]` — open file directly in built-in TUI editor
- `xc -v` — print version and build number
- `xc -h` — show command-line help

## Key Bindings

| Key | Action |
| :--- | :--- |
| `Tab` | Switch active panel (Left / Right) |
| `Enter` | Open directory, enter archive, or run executable |
| `Ctrl+PgUp` / `Ctrl+PgDn` | Go to parent (`..`) / enter directory or any archive |
| `Ctrl+U` | Swap left and right panels with cursor |
| `Ctrl+R` | Force reload both panels |
| `Ctrl+F3`..`Ctrl+F6` | Sort active panel by Name, Extension, Time, Size (repeat to reverse) |
| `Alt+F1` / `Alt+F2` | Drive & bookmarks menu for Left / Right panel (`Ins`/`+` add, `F8`/`Del` remove/unmount) |
| `Insert` / `Shift+Arrows` | Select / deselect files, ranges, or whole words in editor |
| `+` / `-` / `*` | Select by mask / Deselect by mask / Invert selection |
| `Alt+<char>` | Quick search by filename prefix (auto EN/RU layout) |
| `F1` | Open Help guide with live search filter |
| `F2` | Open User Menu (`~/.config/xcommander/menu`) |
| `F3` | Built-in Text / Hex viewer (`F2` Wrap, `F4` Hex, `F7` Find) |
| `F4` / `Shift+F4` | Edit file (remembers cursor pos) / Create new file (`Shift+F2`: Save As in editor) |
| `Ctrl+Shift+D` / `Ctrl+/` | Duplicate line/block / Toggle line or block comment in editor |
| `Ctrl+Shift+Up` / `Down` | Move current line or selected block up / down in editor |
| `Tab` / `Shift+Tab` | Indent / unindent selected block (or current line) in editor |
| `Ctrl+.` | Record / stop keyboard macro in editor and assign any hotkey (persistent) |
| `F5` / `F6` | Copy (or extract from archive) / Move or rename (supports relative paths) |
| `Shift+F5` / `Shift+F6` | Copy / Rename single file in-place (prefills filename only; group unchanged) |
| `Shift+F1` / `Alt+F5` | Pack selected items to archive (`.zip`, `.tar.gz`, `.tar.xz`, `.tar.zst`, `.7z`) |
| `F7` / `F8` | Create directory / Delete selected items (works inside `.zip`/`.7z` archives) |
| `Alt+F7` | Find files by mask & content (`Enter` Go to, `F3` View, `F4` Edit at line, `Ctrl+J` Insert) |
| `Alt+F8` / `Ctrl+H` | Open Command History (filter, Run / Insert / Jump / Delete) |
| `Alt+F9` / `Ctrl+G` | Open File History with saved cursor position (Edit / View / Jump / Delete) |
| `Alt+F10` / `Ctrl+D` | Open Folder History (filter, Go / Other / Insert / Delete) |
| `Alt+F11` / `Ctrl+L` | Open Multiline Script History (Run / Edit / Delete) |
| `Alt+F12` | Toggle 32-color auto file colorization by extension (persistent) |
| `Alt+Backspace` / `Alt+Shift+Backspace` | Step Back / Forward through visited folder history |
| `Alt+[` / `Alt+]` | Insert Left / Right panel path into command line |
| `Alt+Enter` / `Ctrl+J` | Insert filename under cursor into command line |
| `Shift+Enter` / `Alt+O` | Run entered command and open output directly in `F4` editor (when cmd is non-empty) |
| `Up` / `Down` / `Right` (in cmd) | Navigate & accept inline command history popup (`Esc` dismisses popup) |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | Smart copy / cut / paste (files, selection, or whole line in editor) |
| `Ctrl+O` | Toggle full terminal mode / panels (with native mouse wheel scroll & selection) |
| `F10` / `Ctrl+Q` | Save state & histories and quit |
