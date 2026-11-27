# Built-in TUI Editor in `xCommander` (`F4` / `xc -e`)

## 1. Launch Modes

The built-in full-screen text editor works both inside the file manager and as a standalone command-line editor:

* **From `xCommander` panels**:
  * `F4` — open the file under cursor (if inside an archive such as `.zip`, `.tar.*`, or `.7z`, the file is extracted automatically and updated back into the archive upon save).
  * `Shift+F4` — prompt for a filename, create a new file, and open it in the editor.
  * `Alt+F9` — File History (512 entries preserving exact line and column cursor positions).
  * `Shift+Enter` on the command line — execute a command and open its output directly in the editor.
* **From the terminal and scripts (`xc -e` / `xc --edit`)**:
  ```bash
  xc -e file.c                 # open file
  xc -e 42 file.c              # open file and jump to line 42
  xc -e 42:15 file.c           # open file at line 42, column 15
  git diff | xc -e -           # open stdin output in a temporary editor buffer
  ```

---

## 2. Syntax Highlighting and Display Modes

The editor automatically detects the language by file extension and highlights keywords, strings, numbers, comments (`//`, `/* ... */`, `#`), and preprocessor directives:

| Language Profile | Supported Extensions | Highlights |
| :--- | :--- | :--- |
| **C / C++** | `.c`, `.h`, `.cpp`, `.hpp`, `.cc` | C/C++ keywords, `#include`/`#define`, `//` and multi-line `/* ... */` comments |
| **Python** | `.py`, `.pyw` | Python 3 keywords, strings, numbers, `#` comments |
| **Shell** | `.sh`, `.bash`, `.zsh` | `if`/`then`/`fi`, `for`, `case`, built-ins, `#` comments |
| **Config** | `.conf`, `.ini`, `.cfg`, `.toml`, `.yaml`, `.yml`, `.env` | Status keywords (`TRUE`/`FALSE`, `YES`/`NO`, `TODO`, `FIXME`, `ERROR`), `#` comments |

* **`F4`** (inside editor) — toggle syntax highlighting on/off.
* **`F5`** — toggle visible whitespace characters (spaces `·`, tabs `→`, line endings).
* **`Insert`** — toggle Insert (`INS`) / Overwrite (`OVR`) mode.
* **`F9`** — toggle auto-indent on `Enter` (`Ind:On` / `Ind:Of`).

---

## 3. Navigation, Matching Brackets & Text Selection

### Cursor Movement & Mouse
* **Arrows / `Home` / `End`** — move by character/line, or jump to start/end of current line.
* **`Ctrl+Left` / `Ctrl+Right`** — jump cursor by words.
* **`Ctrl+Home` / `Ctrl+End`** — jump to start/end of the entire file.
* **`PageUp` / `PageDown`** — scroll one page up/down.
* **`Alt+F8`** — jump to `LINE[:COL]`.
* **`Ctrl+]`** — jump to matching bracket or quote (`()`, `[]`, `{}`, `""`, `''`).
* **Mouse** — click to position cursor, wheel to scroll, click any `F1..F12` button on the bottom bar.

### Block Selection & System Clipboard
* **`Shift+Arrows` / `Shift+Home` / `Shift+End`** — select characters, lines, or to start/end of line.
* **`Ctrl+Shift+Arrows`** (or `Alt+Shift+Arrows`) — select whole words.
* **`Ctrl+Shift+Home` / `Ctrl+Shift+End`** — select from cursor to start/end of the entire file.
* **`F3`** — toggle `MARK` selection mode (moving the cursor extends selection without holding `Shift`).
* **`Alt+Shift+]`** — select inside matching brackets/quotes (press again to expand and include the brackets/quotes themselves; picks the innermost enclosing pair).
* **`Ctrl+A`** — select the entire file.
* **`Ctrl+C` / `Ctrl+X` / `Ctrl+V` (`Shift+Ins`)** — copy, cut (block or current line), and paste (supports multi-line paste via X11/Wayland clipboard).

---

## 4. Line & Block Editing and Undo / Redo

* **`Tab` / `Shift+Tab`** — insert tab or indent / unindent selected block.
* **`Ctrl+Shift+D`** — duplicate current line or selected block.
* **`Ctrl+/`** — toggle line or block comment (automatically uses `//` or `#` depending on language).
* **`Ctrl+Shift+Up` / `Ctrl+Shift+Down`** — move current line or selected block up / down.
* **`Ctrl+Y`** — delete the entire current line.
* **`Alt+Backspace`** — delete word to the left of the cursor.
* **`Ctrl+Z` / `Ctrl+R`** — Undo / Redo changes (up to **64 steps**).

---

## 5. Search & Interactive Replace (including RegExp)

* **`F7`** — search text with options:
  * **`[ ] Case sensitive`** — case-sensitive matching (case-insensitive by default, including Unicode/Cyrillic);
  * **`[ ] RegExp`** — enable the built-in multi-line UTF-8 regular expression engine.
* **`Shift+F7`** — find next match.
* **`Ctrl+F7`** — interactive Find & Replace with live substitution preview and per-match confirmation:
  * **`Replace`** — replace current match and jump to next;
  * **`All`** — replace all remaining matches in the file (undone in a single `Ctrl+Z`);
  * **`Skip`** — skip current match;
  * **`Cancel` (`Esc`)** — stop replacing.

### 5.1. Regular Expression Syntax in "Search" (`RegExp`)

| Construct | Meaning | Example |
| :--- | :--- | :--- |
| `.` | Any character except newline `\n` | `a.c` (`abc`, `a1c`) |
| `\d` / `\D` | Digit `0..9` / non-digit | `\d+` (integer number) |
| `\w` / `\W` | Letter (incl. Unicode), digit, or `_` / non-word | `\w+` (identifier or word) |
| `\s` / `\S` | Space or tab `\t` / non-whitespace | `\s+` (spaces/tabs run) |
| `^` / `$` | Start of line / end of line | `^#.*$` (comment line) |
| `\n`, `\t` | Newline (multi-line search!) and tab | `\n\n+` (consecutive blank lines) |
| `*`, `+`, `?` | Greedy quantifiers: `0+`, `1+`, `0..1` | `".*"` (first `"` to last `"`) |
| `*?`, `+?`, `??` | Lazy quantifiers (stop at first match) | `".*?"` (each quoted string separately) |
| `(...)` | Capture group (up to **9** groups per pattern) | `(\w+)\s*=\s*(.+)` |
| `\.`, `\(`, `\)` | Escape metacharacters (`.`, `(`, `)`, `*`, `+`, `?`, `^`, `$`, `\`) | `func\((.*?)\)` |

### 5.2. Escape Sequences & Capture Groups in "Replace"

| Token | Inserts on Replacement |
| :--- | :--- |
| `\0` | Entire matched text |
| `\1` .. `\9` | Text captured by the corresponding `(...)` group |
| `\t` | Tab character |
| `\n` | Newline (splits a single match into multiple lines!) |
| `\\` | Literal backslash `\` |

### 5.3. Practical Examples for `Ctrl+F7` (with `RegExp` checked)

1. **Swap left and right sides of an assignment (`a = b` -> `b = a`)**:
   * Search: `^(\s*)(\w+)\s*=\s*(.+)$`
   * Replace: `\1\3 = \2`
2. **Trim trailing spaces and tabs from all lines**:
   * Search: `\s+$`
   * Replace: *(leave empty)*
3. **Collapse multiple consecutive blank lines into a single blank line**:
   * Search: `\n\n+`
   * Replace: `\n\n`
4. **Wrap every number in square brackets (`42` -> `[42]`)**:
   * Search: `\d+`
   * Replace: `[\0]`
5. **Convert 4 leading whitespace characters into a single tab** (or search `^` alone to indent every line with `\t`):
   * Search: `^\s\s\s\s`
   * Replace: `\t`
6. **Split a comma-separated list into indented lines**:
   * Search: `,\s*`
   * Replace: `,\n\t`

---

## 6. Macro Recording & Management (`Ctrl+.` / `Ctrl+M`)

The editor includes a built-in keyboard macro recorder:
* **`Ctrl+.`** — start / stop recording key sequences (navigation, selection, text input, deletion, commenting, etc.). When stopping, assign a name and hotkey (`Ctrl+<key>` or `Alt+<key>`).
* **`Ctrl+M`** — open the Macro Manager to inspect recorded steps (`<C-Left>`, `<Sel-Word-R>`, `<Comment>`, `<Dup>`, etc.), run a macro (`Enter`), or delete it (`F8` / `Del`).

---

## 7. Saving, Auto-`sudo`, Terminal Output & Help

* **`F2`** — save file. If write access is denied (`EACCES` / `EPERM`, e.g., `/etc/...`), the editor automatically displays a red **`sudo`** password prompt and saves through the privileged helper.
* **`Shift+F2`** — Save As (save under a new filename and re-detect syntax highlighting).
* **`Ctrl+O`** — toggle terminal output screen (like FAR Manager) and return to the editor with `Ctrl+O`.
* **`Hold Ctrl / Shift / Alt`** — dynamic `F1..F12` bar highlights available shortcuts for the held modifier.
* **`F1`** — open the built-in interactive Editor Help with live key filtering.
* **`Esc` / `F10` / `Ctrl+Q`** — exit editor (`[ Yes ] / [ No ] / [ Cancel ]` prompt if modified).
