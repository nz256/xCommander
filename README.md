# xCommander (`xc`)

**English** | [Русский](README.ru.md)

A fast, lightweight dual-pane terminal file manager for Linux inspired by FAR Manager and Midnight Commander, written in pure C with `ncurses`. Primarily developed, tested, and used daily on **Arch Linux** with the **Kitty** terminal emulator.

![xCommander Screenshot](docs/screenshot.png)

## Features

- **Dynamic Panels & Built-in `sudo` Privilege Elevation**: Unlimited directory size via dynamic `malloc`/`realloc` panels, plus an automatic red password prompt (`Operation requires privileges elevation`) with session caching on `EACCES`/`EPERM` when saving in the editor (`F4` -> `F2`), copying/moving (`F5`/`F6`), creating folders (`F7`), deleting (`F8`/`Shift+F8`), making links (`Alt+F6`), or changing attributes (`Ctrl+A`).
- **FAR-Style Info Panel (`Ctrl+I`)**: Displays target path, type/symlink/Git branch, unlimited recursive folder size & item count (with live `Scanning the folder` progress popup and `Esc` interrupt), permissions, owner, timestamps, and filesystem mount/free space stats.
- **Dual-Pane Navigation & Real-Time Updates**: Instant directory browsing with `inotify` auto-refresh, custom sorting (`Ctrl+F3`..`Ctrl+F6` with left-aligned header indicator), drive/bookmark menus (`Alt+F1` / `Alt+F2`) with USB safe-unmount, and toggleable 32-color auto file colorization by extension (`Alt+F12`).
- **File Search by Mask & Content (`Alt+F7`)**: Fast recursive search supporting multi-masks (`*.c;*.h`), case-sensitive/insensitive text search with line numbers, and instant actions (`Enter` Go to, `F3` View, `F4` Edit at matched line, `Ctrl+J` Insert path).
- **FAR-Style Temporary Panel (`TmpPanel`)**: Virtual collector panel (`Alt+F1` / `Alt+F2` -> `0`, or `[ Panel ]` from `Alt+F7` search results) holding references to scattered files/folders across directories; add via `F5`, remove via `F8`, jump to real location via `Ctrl+PgUp`.
- **Virtual Archive VFS**: Enter archives seamlessly (`Enter` or `Ctrl+PgDn` on `.zip`, `.jar`, `.apk`, `.whl`, `.epub`, `.docx`, `.tar`, `.tgz`, `.tar.gz`, `.tar.xz`, `.tar.bz2`, `.tar.zst`, `.7z`, `.rar`), view (`F3`), edit with auto-update (`F4`), extract (`F5`), delete inside archives (`F8`), and pack files (`Shift+F1` / `Alt+F5`).
- **Built-in Viewer & IDE-like TUI Editor**: Text/Hex file viewer (`F3`) and full-featured TUI editor (`F4` / `Shift+F4`) with syntax highlighting, `Save As` (`Shift+F2`), cursor position memory per file, line/block duplication (`Ctrl+Shift+D`), moving lines up/down (`Ctrl+Shift+Up/Down`), line/block commenting (`Ctrl+/`), smart auto-indent (`Enter`) & block indent (`Tab` / `Shift+Tab`), full-line copy/cut without selection (`Ctrl+C` / `Ctrl+X`), and FAR-style persistent keyboard macros (`Ctrl+.` to record/stop & assign any hotkey, `Ctrl+M` to view/run/delete).
- **4 Persistent History Journals & Dynamic 12-Key F-Bar**:
  - `Alt+F8` / `Ctrl+H`: Command History (512 items with timestamp & directory; live filter, Run / Insert / Jump / Delete)
  - `Alt+F9`: File History (512 viewed/edited files with saved cursor position; Edit / View / Jump / Delete)
  - `Alt+F10` / `Ctrl+D`: Folder History (512 visited steps; live filter, Go / Other panel / Insert / Delete; plus `Alt+Backspace` / `Alt+Shift+Backspace` Back/Forward)
  - `Alt+F11` / `Ctrl+L`: Multiline Script History (Run / Edit in built-in editor / Delete)
  - Hold `Alt` / `Ctrl` / `Shift` to dynamically switch the 12-button bottom F-bar.
- **Inline History Autosuggest & Command-to-Editor Output**: Typing in the command line opens a live prefix-matching history popup from the 1st character (`Up`/`Down` navigate, `Right`/click fill, `Esc` dismiss); press `Shift+Enter` to run a command and open its output directly in the built-in `F4` editor.
- **Progress Dialogs with Speed, ETA & Cancel Confirmation**: Live two-bar progress window (`F5` Copy, `F6` Move, `F8` Trash, `Shift+F8` Permanent Delete) showing `MB/s`, `ETA`, and interactive `Esc` abort confirmation.
- **Smart Clipboard & Native Terminal Scrollback**: URI-list file copy/paste (`Ctrl+C` / `Ctrl+X` / `Ctrl+V`), multiline script paste dialog, quick prefix search (`Alt+<char>` with auto EN/RU layout), live-filtered Help (`F1`), and full shell toggle (`Ctrl+O`) with automatic mouse release for native wheel scrolling and text selection.

## Build & Run

```bash
make
./xc
```

### CLI Options

- `xc` — launch dual-pane file manager in current directory
- `xc -e [LINE[:COL]] [FILE]` / `--edit` — open file (or `-` for `stdin`) directly in built-in TUI editor (full guide: [docs/EDITOR.md](docs/EDITOR.md))
- `xc -v [LINE] [FILE]` / `--view` — open file (or `-` for `stdin`) directly in built-in viewer
- `xc -D` / `--dialog [--bg COLOR] <subcommand> ...` — show TUI dialogs (transparent `/dev/tty` overlay by default, or colored background via `--bg` name / `0..255` / `#RRGGBB`):
  - `msg [TITLE] TEXT|FILE|-` — scrollable message box
  - `confirm [TITLE] TEXT|FILE|-` — `[ Yes ]` / `[ No ]` confirmation (exits `0` on Yes, `1` on No/Esc)
  - `input [TITLE] PROMPT [DEFAULT]` — UTF-8 input field with selection & clipboard (prints result to `stdout`, exits `0`/`1`)
  - `menu TITLE ITEM1 ITEM2...` or `menu TITLE -` — interactive selection menu from args or `stdin` (prints choice to `stdout`)
  - `form FILE|-` — run dialog from key-value spec (`type=msg|confirm|input|menu`, `title=`, `bg=`, `text=`, `default=`, `items=a|b|c`)
  - Built-in `:dialog` / `:D` inside `xc` (command line, `Ctrl+O`, `F2` User Menu, `Ctrl+G`), including `$(:dialog ...)` inline substitution and `&&` chains without spawning a child process
  - Full guide and examples: [docs/DIALOGS.md](docs/DIALOGS.md)
- `xc -V` / `--version` — print version and build number
- `xc -h` / `--help` — show command-line help

## Key Bindings

| Key | Action |
| :--- | :--- |
| `Tab` | Switch active panel (Left / Right) |
| `Enter` | Open directory, enter archive, or run executable |
| `Ctrl+PgUp` / `Ctrl+PgDn` | Go to parent (`..`) / enter directory or any archive |
| `Ctrl+U` | Swap left and right panels with cursor |
| `Ctrl+R` | Force reload both panels |
| `Ctrl+F3`..`Ctrl+F6` | Sort active panel by Name, Extension, Time, Size (repeat to reverse) |
| `Alt+F1` / `Alt+F2` | Drive, bookmarks & `TmpPanel` (`0`) menu (`Ins`/`+` add, `F8`/`Del` remove/unmount) |
| `Insert` / `Shift+Arrows` | Select / deselect files, ranges, or whole words in editor |
| `Ctrl+Shift+Home` / `End` | Select from cursor to start / end of entire file in editor |
| `+` / `-` / `*` | Select by mask / Deselect by mask / Invert selection |
| `Alt+<char>` | Quick search by filename prefix (auto EN/RU layout) |
| `F1` | Open Help guide with live search filter |
| `F2` / `F9` | Open User Menu (`~/.config/xcommander/menu`) / Top Configuration Menu |
| `F9` -> `Commands` | **File Associations** (`~/.config/xcommander/assoc`): bind commands to masks (`mask \| desc \| enter \| f3 \| f4`) for `Enter` & optional `F3`/`F4` (FAR macros `!.!` & `:dialog`) |
| `F3` | View file (Text / Hex) or calculate folder(s) / current dir (`..`) size (`Esc` cancels) |
| `F4` / `Shift+F4` | Edit file (remembers cursor pos) / Create new file (`Shift+F2`: Save As in editor) |
| `Ctrl+Shift+D` / `Ctrl+/` | Duplicate line/block / Toggle line or block comment in editor |
| `Ctrl+Shift+Up` / `Down` | Move current line or selected block up / down in editor |
| `Tab` / `Shift+Tab` | Indent / unindent selected block (or current line) in editor |
| `Ctrl+.` / `Ctrl+M` | Record/stop keyboard macro (assign hotkey) / Open Macro Manager in editor |
| `Ctrl+O` (in editor) | Toggle terminal output screen to view previous command output (like FAR) |
| `Alt+F8` (in editor) | Go to line (or `line:col`) dialog |
| `F5` / `F6` | Copy (or extract from archive) / Move or rename (supports relative paths) |
| `Alt+F6` | Make Symbolic (`ln -s`) or Hard (`ln`) link (supports `sudo` elevation) |
| `Shift+F5` / `Shift+F6` | Copy / Rename single file in-place (prefills filename only; group unchanged) |
| `Shift+F1` / `Alt+F5` | Pack selected items to archive (`.zip`, `.tar.gz`, `.tar.xz`, `.tar.zst`, `.7z`) |
| `F7` / `F8` / `Shift+F8` | Create directory / Move to system Trash (or delete in archive) / Permanent delete |
| `Ctrl+A` | File attributes, permissions, owner & timestamps dialog (`chmod`, `chown`, `mtime`/`atime`, recursive) |
| `Ctrl+G` | Apply shell command to each selected file/dir (`!.!`, `!~`, `!.`, `!:`, `!/` FAR macros, persistent last command) |
| `Ctrl+I` | Toggle FAR-style Info Panel (recursive folder stats with `Esc` cancel, permissions, disk info) |
| `Alt+F7` | Find files by mask & content (`Go to`, `View`, `Edit`, `Insert`, `Panel` -> `TmpPanel`) |
| `Alt+F8` / `Ctrl+H` | Open Command History (filter, Run / Insert / Jump / Delete) |
| `Alt+F9` | Open File History with saved cursor position (Edit / View / Jump / Delete) |
| `Alt+F10` / `Ctrl+D` | Open Folder History (filter, Go / Other / Insert / Delete) |
| `Alt+F11` / `Ctrl+L` | Open Multiline Script History (Run / Edit / Delete) |
| `Alt+F12` | Toggle 32-color auto file colorization by extension (persistent) |
| `Alt+Backspace` / `Alt+Shift+Backspace` | Step Back / Forward through visited folder history |
| `Alt+[` / `Alt+]` | Insert Left / Right panel path into command line |
| `Ctrl+Enter` / `Alt+Enter` / `Ctrl+J` | Insert filename under cursor into command line (auto-prepends `./` for executables on empty line) |
| `Shift+Enter` | Run entered command and open output directly in `F4` editor (when cmd is non-empty) |
| `Up` / `Down` / `Right` (in cmd) | Navigate & accept inline command history popup (`Esc` dismisses popup) |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | Smart copy / cut / paste (files, selection, or whole line in editor) |
| `Alt+Shift+N` | Copy filename(s) under cursor or selection as plain text to clipboard |
| `Ctrl+O` | Toggle full terminal mode / panels (with native mouse wheel scroll & selection) |
| `F10` / `Ctrl+Q` | Save state & histories and quit |
