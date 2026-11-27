# Interactive TUI Dialogs in `xCommander` (`-D` / `--dialog` & `:dialog`)

## 1. Overview and Architecture

`xCommander` includes a built-in TUI dialog subsystem that lets you build interactive terminal workflows in **two ways**:
1. **Externally (from shell, bash, or Python scripts)** — via the **`xc -D`** (`--dialog`) CLI flag.
2. **Internally (inside a running `xCommander` session)** — via the built-in **`:dialog`** (`:D`) command from the command line, terminal screen (`Ctrl+O`), User Menu (`F2`), command history (`Alt+F8`), and batch execution (`Ctrl+G`).

### Key Architectural Highlights
* **Transparent ANSI Overlay via `/dev/tty`**: without `--bg`, dialogs render directly over the current terminal output without clearing the screen, restoring cursor position and screen content upon exit.
* **Clean `stdout` for Command Substitution (`$(...)`)**: all overlay UI rendering is written directly to `/dev/tty`, while `stdout` receives **only the resulting string** (typed text or chosen menu item). This allows clean capture like `VAR=$(xc -D input ...)` in shell scripts.
* **Native In-Process Execution inside `xc`**: `:dialog` / `:D` draws directly over the dual panels in the current process without spawning a child `xc` instance.
* **Custom Background (`--bg <color>`)**: supports color names (`darkblue`, `black`, `cyan`, etc.), `0..255` palette indices, and 24-bit HEX colors (`#RRGGBB`).

---

## 2. External CLI Mode: `xc -D` / `xc --dialog`

```bash
xc -D [--bg <color>] <subcommand> [arguments...]
```

### 2.1. `msg` — Scrollable Message Box (Default)
Scroll with `Up`/`Down`, `PgUp`/`PgDn`, or mouse wheel. Close with `Enter`, `Esc`, `Space`, `E`, or clicking `{ Enter }`.
```bash
xc -D "Done" "Backup completed successfully.\nNo errors found."
git status -s | xc -D "Git Status" -
xc -D --bg "#1e1e2e" msg "Release" "Build v1.0 finished"
```

### 2.2. `confirm` — Confirmation Prompt (`Yes` / `No`)
Displays `{ Yes }` and `[ No ]`. Navigate with `Left`/`Right`, `Tab`, `Y`/`N`, `Enter`, `Esc`, or mouse click. Exits `0` on **Yes**, `1` on **No** or **Esc**.
```bash
xc -D confirm "Deploy" "Push changes to production?" && git push
```

### 2.3. `input` — UTF-8 Text Input Field
Supports text selection (`Shift+Arrows`, `Ctrl+A`), word navigation (`Ctrl+Left`/`Right`), and system clipboard (`Ctrl+C`, `Ctrl+X`, `Ctrl+V`). Prints entered text to `stdout`; exits `0` (`OK`) or `1` (`Cancel`/`Esc`).
```bash
MSG=$(xc -D input "Git Commit" "Enter commit message:") && git commit -m "$MSG"
BRANCH=$(xc -D input "Branch" "Branch name:" "feat/") && git checkout -b "$BRANCH"
```

### 2.4. `menu` — Interactive Selection Menu
Navigate with `Up`/`Down`, `PgUp`/`PgDn`, `Home`/`End`, mouse wheel, or mouse click. Prints chosen item to `stdout`; exits `0` (`Enter`/click) or `1` (`Esc`).
```bash
ENV=$(xc -D menu "Environment" "dev" "staging" "prod") && echo "Selected: $ENV"
BRANCH=$(git branch --format='%(refname:short)' | xc -D menu "Branch" -) && git checkout "$BRANCH"
```

### 2.5. `form` — Run Dialog from a Spec File (or `stdin`)
The spec file can have any filename or extension (e.g., `deploy.dialog`):
```ini
type=confirm           # msg | confirm | input | menu
title=Deploy Project
bg=#1e1e2e             # optional background color
text=Push build to production server?\nAll services will restart.
default=feat/new       # initial value for type=input
items=dev|staging|prod # pipe-separated items for type=menu
```
Run with `xc -D form deploy.dialog && ./deploy.sh` or `cat deploy.dialog | xc -D form -`.

---

## 3. Built-in Mode inside `xCommander`: `:dialog` and `:D`

Invoke dialogs from the command line, `Ctrl+O` terminal screen, User Menu (**`F2`**), command history (**`Alt+F8`**), or batch apply (**`Ctrl+G`**) without spawning a child `xc` process:
```text
:dialog "Title" "Message text"
:D docs/DIALOGS.md
:dialog confirm "Question" "Continue execution?"
:dialog input "Title" "Prompt" "Default"
:dialog menu "Choose" "Item 1" "Item 2" "Item 3"
:dialog form /path/to/file.dialog
```

### Inline `$(:dialog ...)` Substitution, `&&` Chains & `$XC_DIALOG_OUT`
```ini
c:	Git Commit | MSG=\$(:dialog input "Git Commit" "Message:") && git commit -m "$MSG"
d:	Deploy Confirm | :dialog confirm "Deploy" "Push changes?" && git push
t:	Test Chain | VAL=\((:dialog input "Input" "Name:" "test") && :dialog "Result" "Entered: \)VAL"
```
* `$(:dialog input ...)` and `$(:dialog menu ...)` expand in-place over the panels; pressing `Esc` or `Cancel` aborts the entire chain.
* Variable assignments (`VAR=value`) in `&&` chains are stored in the process environment and expanded as `$VAR` / `${VAR}` in subsequent commands.
* Standalone `:dialog input ...` or `:dialog menu ...` also exports the result to **`$XC_DIALOG_OUT`** for subsequent commands (e.g. `:dialog "Result" "Selected: $XC_DIALOG_OUT"`).
* In `Ctrl+G`, FAR-style macros are supported: `:dialog "File" "Name: !.!\nDir: !/"`.

---

## 4. `.dialog` Script Files (Compact One-Line & `key=value` Formats)

Any `.dialog` file can be run directly via **`Enter`** in `xCommander` panels (configured in `~/.config/xcommander/assoc`), via **`:dialog file.dialog`**, or from the terminal via **`xc -D file.dialog`**.

### 4.1. Compact One-Line Scripting Syntax
Each line is a step (`msg`, `confirm`, `input`, `menu`, `if`/`else`/`endif`, or `!` shell command). Pressing `Cancel` or `Esc` at any prompt aborts the remaining steps:
```ini
confirm "Build Check" "Script works!\nStart build?" bg=#1e1e2e
input "Release" "Archive name:" "backup_test" -> NAME
menu "Format" "tar.gz|zip|7z" -> EXT
msg "Summary" "Creating NAME.{EXT}"
! echo "Done: NAME.{EXT}"
```

### 4.2. Built-in Panel Context Variables
Inside `.dialog` scripts, panel context variables are automatically populated:
* `${FILE}` — filename or directory under cursor
* `${NAME}` — filename without extension
* `${EXT}` — file extension (without dot)
* `${DIR}` — active panel directory path
* `${OTHER_DIR}` — passive panel directory path
* `${SELECTED}` — space-separated quoted marked filenames (or `"${FILE}"` if none marked)
* `${COUNT}` — number of marked items (or `1`)

### 4.3. Conditional Branching (`if` / `else` / `endif`)
Supports nested `if ${VAR} == "..."` (or `=`), `if ${VAR} != "..."`, `else`, and `endif` (or `fi`):
```ini
menu "Action for \${FILE}" "Archive|Permissions|Delete" -> ACT
if \${ACT} == "Archive"
    input "Archive" "Output filename:" "\${NAME}.tar.gz" -> ARCH
    ! tar -czf "ARCH" {SELECTED}
else
    if \${ACT} == "Permissions"
        ! ls -ld \${SELECTED}
    else
        confirm "Delete" "Delete \${FILE}?"
        ! rm -rf "\${FILE}"
    endif
endif
```
