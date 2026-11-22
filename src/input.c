#define _DEFAULT_SOURCE
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#include <wctype.h>
#include <signal.h>
#include <errno.h>
#include "input.h"

int g_mod_ctrl = 0;
int g_mod_alt = 0;
int g_mod_shift = 0;
static volatile sig_atomic_t g_resized = 0;
static void on_sigwinch(int sig) { (void)sig; g_resized = 1; }

static struct termios g_orig_termios;
static int g_has_orig_termios = 0;

void input_set_mouse(int enable) {
	const char *seq = enable ? "\033[?1000;1006h" : "\033[?1000;1006l";
	ssize_t w = write(STDOUT_FILENO, seq, strlen(seq));
	(void)w;
}

void input_enable_raw(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigwinch;
	sigaction(SIGWINCH, &sa, NULL);
	struct termios t;
	if (tcgetattr(STDIN_FILENO, &t) == 0) {
		if (!g_has_orig_termios) {
			g_orig_termios = t;
			g_has_orig_termios = 1;
		}
		t.c_iflag &= ~(unsigned int)(IXON | IXOFF | ICRNL | INLCR | IGNCR | ISTRIP);
		t.c_lflag &= ~(unsigned int)(ICANON | ECHO | ISIG | IEXTEN);
		t.c_cc[VMIN] = 1;
		t.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &t);
	}
	g_mod_ctrl = 0; g_mod_alt = 0; g_mod_shift = 0;
	const char *on = "\033[>27u\033[?1l\033>\033[?1000;1006h\033[?1004h";
	ssize_t w = write(STDOUT_FILENO, on, strlen(on));
	(void)w;
}

void input_restore_sane(void) {
	fflush(stdout);
	const char *off = "\033[?1004l\033[?1000;1006l\033[?1l\033>\033[<99u\033[>4;0m";
	ssize_t w = write(STDOUT_FILENO, off, strlen(off));
	(void)w;
	tcdrain(STDOUT_FILENO);
	usleep(120000);
	while (1) {
		fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
		struct timeval tv = {0, 5000};
		if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0) {
			char drain[256];
			if (read(STDIN_FILENO, drain, sizeof(drain)) <= 0) break;
		} else {
			break;
		}
	}
	if (g_has_orig_termios) {
		struct termios sane = g_orig_termios;
		sane.c_iflag |= (ICRNL | IXON);
		sane.c_iflag &= ~(unsigned int)(INLCR | IGNCR | ISTRIP);
		sane.c_oflag |= (OPOST | ONLCR);
		sane.c_lflag |= (ICANON | ECHO | ECHOE | ECHOK | ISIG | IEXTEN);
		sane.c_cc[VMIN] = 1;
		sane.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &sane);
	}
}

static int wait_byte(int timeout_ms) {
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(STDIN_FILENO, &fds);
	struct timeval tv;
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;
	return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
}

static int read_byte(unsigned char *b) {
	return read(STDIN_FILENO, b, 1) == 1;
}

static wchar_t g_mod_ch = 0;
static int ru_to_en_key(int code) {
	wchar_t lc = (wchar_t)towlower((wint_t)code);
	const wchar_t *ru = L"йцукенгшщзхъфывапролджэячсмитьбюё";
	const char    *en = "qwertyuiop[]asdfghjkl;'zxcvbnm,.`";
	for (int i = 0; ru[i] != L'\0'; i++) {
		if (lc == ru[i]) return (int)(unsigned char)en[i];
	}
	return code;
}

static KeyAction map_modified_code(int code, int mod) {
	g_mod_ch = 0;
	int m = (mod > 0) ? (mod - 1) : 0;
	int has_shift = (m & 1) ? 1 : 0;
	int has_alt   = (m & 2) ? 1 : 0;
	int has_ctrl  = (m & 4) ? 1 : 0;
	if ((has_ctrl || has_alt) && code >= 1024) {
		code = ru_to_en_key(code);
	}


	if (code == 13 || code == 10) {
		if (has_ctrl || has_alt) return ACT_INSERT_NAME;
		if (has_shift) return ACT_SHIFT_ENTER;
		return ACT_ENTER;
	}
	if (code == 91 && (has_ctrl || has_alt)) return ACT_INSERT_LEFT_PATH;
	if ((code == 93 || code == 125 || code == 1098 || code == 1066) && has_shift && (has_ctrl || has_alt)) return ACT_SELECT_MATCHING_BLOCK;
	if (code == 93 && (has_ctrl || has_alt)) return ACT_INSERT_RIGHT_PATH;

	if (has_ctrl && has_shift && !has_alt) {
		if (code == 'd' || code == 'D' || code == 1074 || code == 1042) return ACT_CTRL_SHIFT_D;
	}
	if (has_alt && has_shift && !has_ctrl) {
		if (code == 'n' || code == 'N' || code == 1090 || code == 1058) return ACT_ALT_SHIFT_N;
	}
	if (has_ctrl && !has_alt && !has_shift && code == '.') return ACT_MACRO_REC;
	if (has_ctrl && !has_alt && (code == '/' || code == '?' || code == 31)) return ACT_TOGGLE_COMMENT;
	if (has_ctrl && !has_alt && !has_shift) {
		if (code == 'a' || code == 'A') return ACT_CTRL_A;
		if (code == 'b' || code == 'B') return ACT_CTRL_B;
		if (code == 'c' || code == 'C') return ACT_CTRL_C;
		if (code == 'd' || code == 'D' || code == 1074 || code == 1042) return ACT_CTRL_D;
		if (code == 'e' || code == 'E') return ACT_CTRL_E;
		if (code == 'f' || code == 'F') return ACT_CTRL_F;
		if (code == 'g' || code == 'G' || code == 1087 || code == 1055) return ACT_CTRL_G;
		if (code == 'h' || code == 'H' || code == 1088 || code == 1056) return ACT_CTRL_H;
		if (code == 'i' || code == 'I' || code == 1096 || code == 1064) return ACT_CTRL_I;
		if (code == 'j' || code == 'J') return ACT_INSERT_NAME;
		if (code == 'l' || code == 'L' || code == 1076 || code == 1044) return ACT_CTRL_L;
		if (code == 'o' || code == 'O') return ACT_CTRL_O;
		if (code == 'q' || code == 'Q') return ACT_CTRL_Q;
		if (code == 'r' || code == 'R') return ACT_CTRL_R;
		if (code == 'u' || code == 'U') return ACT_CTRL_U;
		if (code == 'v' || code == 'V') return ACT_CTRL_V;
		if (code == 'x' || code == 'X') return ACT_CTRL_X;
		if (code == 'y' || code == 'Y') return ACT_CTRL_Y;
		if (code == 'z' || code == 'Z') return ACT_CTRL_Z;
	}
	if (code == 14) {
		if (has_ctrl) return ACT_CTRL_F4;
		if (has_shift) return ACT_SHIFT_F4;
		return ACT_F4;
	}
	if (has_alt && !has_ctrl && code >= 32) {
		g_mod_ch = (wchar_t)code;
		return ACT_ALT_CHAR;
	}
	if (has_ctrl && !has_alt && code >= 32 && code < 57344) {
		g_mod_ch = (wchar_t)towlower((wint_t)code);
		return ACT_CTRL_CHAR;
	}
	return ACT_NONE;
}

static KeyAction parse_csi(const char *seq) {
	int len = (int)strlen(seq);
	if (len == 0) return ACT_ESC;

	if (strncmp(seq, "27;", 3) == 0 && seq[len - 1] == '~') {
		int mod = 1, code = 0;
		if (sscanf(seq + 3, "%d;%d~", &mod, &code) == 2) {
			return map_modified_code(code, mod);
		}
	}

	if (strcmp(seq, "I") == 0 || strcmp(seq, "O") == 0) {
		if (g_mod_ctrl || g_mod_alt || g_mod_shift) {
			g_mod_ctrl = 0; g_mod_alt = 0; g_mod_shift = 0;
			return ACT_MOD_CHANGE;
		}
		return ACT_NONE;
	}

	if (seq[len - 1] == 'u') {
		int code = 0, mod = 1, ev_type = 1, assoc_ch = 0;
		char *semi1 = strchr(seq, ';');
		code = atoi(seq);
		if (semi1) {
			mod = atoi(semi1 + 1);
			char *colon = strchr(semi1 + 1, ':');
			char *semi2 = strchr(semi1 + 1, ';');
			if (colon && (!semi2 || colon < semi2)) ev_type = atoi(colon + 1);
			if (semi2) assoc_ch = atoi(semi2 + 1);
		} else {
			char *colon = strchr(seq, ':');
			if (colon) ev_type = atoi(colon + 1);
		}
		if (mod < 1) mod = 1;

		/* Отслеживание отдельных клавиш-модификаторов Kitty Protocol */
		if (code == 57441 || code == 57447) { g_mod_shift = (ev_type != 3); return ACT_MOD_CHANGE; }
		if (code == 57442 || code == 57448) { g_mod_ctrl  = (ev_type != 3); return ACT_MOD_CHANGE; }
		if (code == 57443 || code == 57449) { g_mod_alt   = (ev_type != 3); return ACT_MOD_CHANGE; }

		/* Игнорируем отпускание (release = 3) для обычных клавиш */
		if (ev_type == 3) return ACT_NONE;

		/* Синхронизируем флаги модификаторов с текущим нажатием */
		int m = mod - 1;
		g_mod_shift = (m & 1) ? 1 : 0;
		g_mod_alt   = (m & 2) ? 1 : 0;
		g_mod_ctrl  = (m & 4) ? 1 : 0;

		if (code == 9) return g_mod_shift ? ACT_SHIFT_TAB : ACT_TAB;
		if (code == 13 || code == 10) { if (g_mod_ctrl || g_mod_alt) return ACT_INSERT_NAME; if (g_mod_shift) return ACT_SHIFT_ENTER; return ACT_ENTER; }
		if (code == 27) return ACT_ESC;
		if (code == 57375 && ((mod == 3) || g_mod_alt)) return ACT_ALT_F12;
		if (code >= 57371 && code <= 57382) {
			int f_idx = code - 57370;
			if (f_idx == 1) return ((mod == 3) || g_mod_alt) ? ACT_ALT_F1 : (((mod == 2) || g_mod_shift) ? ACT_SHIFT_F1 : ACT_F1);
			if (f_idx == 2) return ((mod == 3) || g_mod_alt) ? ACT_ALT_F2 : (((mod == 2) || g_mod_shift) ? ACT_SHIFT_F2 : ACT_F2);
			if (f_idx == 8) return ((mod == 3) || g_mod_alt) ? ACT_ALT_F8 : (((mod == 2) || g_mod_shift) ? ACT_SHIFT_F8 : (((mod == 5) || g_mod_ctrl) ? ACT_CTRL_F8 : ACT_F8));
			if (f_idx == 3) return (mod == 5) ? ACT_CTRL_F3 : ACT_F3;
			if (f_idx == 4) return ((mod == 5) || g_mod_ctrl) ? ACT_CTRL_F4 : (((mod == 2) || g_mod_shift) ? ACT_SHIFT_F4 : ACT_F4);
			if (f_idx == 5) return ((mod == 5) || g_mod_ctrl) ? ACT_CTRL_F5 : (((mod == 3) || g_mod_alt) ? ACT_ALT_F5 : (((mod == 2) || g_mod_shift) ? ACT_SHIFT_F5 : ACT_F5));
			if (f_idx == 6) return ((mod == 5) || g_mod_ctrl) ? ACT_CTRL_F6 : (((mod == 3) || g_mod_alt) ? ACT_ALT_F6 : (((mod == 2) || g_mod_shift) ? ACT_SHIFT_F6 : ACT_F6));
			if (f_idx == 7) return ACT_F7;
			if (f_idx == 8) return ACT_F8;
			if (f_idx == 9) return ((mod == 3) || g_mod_alt) ? ACT_ALT_F9 : ACT_F9;
			if (f_idx == 10) return ((mod == 3) || g_mod_alt) ? ACT_ALT_F10 : ACT_F10;
			if (f_idx == 11) return ((mod == 3) || g_mod_alt) ? ACT_ALT_F11 : ACT_F11;
			if (f_idx == 12) return ((mod == 3) || g_mod_alt) ? ACT_ALT_F12 : ACT_F12;
		}
		
		if (code == 127 || code == 8) { if ((mod == 4) || (g_mod_alt && g_mod_shift)) return ACT_ALT_SHIFT_BACKSPACE; if ((mod == 3) || g_mod_alt) return ACT_ALT_BACKSPACE; return ACT_BACKSPACE; }

		if (mod > 1) {
			KeyAction ma = map_modified_code(code, mod);
			if (ma != ACT_NONE) return ma;
		}
		if (code >= 32 && code < 57344 && !g_mod_ctrl && !g_mod_alt) {
			wchar_t wc = (assoc_ch >= 32 && assoc_ch < 57344) ? (wchar_t)assoc_ch : (wchar_t)code;
			if (assoc_ch == 0) {
				int caps = ((mod - 1) & 64) ? 1 : 0;
				if (g_mod_shift ^ caps) wc = towupper(wc);
			}
			g_mod_ch = wc;
			return ACT_CHAR;
		}
		return ACT_NONE;
	}

	char final = seq[len - 1];
	int p1 = 1, mod = 1;
	if (len == 1) {
		mod = 1;
	} else {
		int ev_type = 1;
		if (sscanf(seq, "%d;%d:%d", &p1, &mod, &ev_type) == 3 && ev_type == 3) return ACT_NONE;
		else if (sscanf(seq, "%d;%d", &p1, &mod) != 2) sscanf(seq, "%d", &p1);
	}

	mod = ((mod > 0 ? (mod - 1) : 0) & 7) + 1;
	if (final == 'Z') return ACT_SHIFT_TAB;
	if (final == 'A') { if (mod == 6) return ACT_MOVE_LINES_UP; return (mod == 2) ? ACT_SHIFT_UP : ACT_UP; }
	if (final == 'B') { if (mod == 6) return ACT_MOVE_LINES_DOWN; return (mod == 2) ? ACT_SHIFT_DOWN : ACT_DOWN; }
	if (final == 'C') {
		if (mod == 2) return ACT_SHIFT_RIGHT;
		if (mod == 5) return ACT_CTRL_RIGHT;
		if (mod == 6 || mod == 4) return ACT_WORD_SEL_RIGHT;
		return ACT_RIGHT;
	}
	if (final == 'D') {
		if (mod == 2) return ACT_SHIFT_LEFT;
		if (mod == 5) return ACT_CTRL_LEFT;
		if (mod == 6 || mod == 4) return ACT_WORD_SEL_LEFT;
		return ACT_LEFT;
	}
	if (final == 'H') { if (mod == 2) return ACT_SHIFT_HOME; if (mod == 4) return ACT_ALT_SHIFT_HOME; if (mod == 5) return ACT_CTRL_HOME; if (mod == 6) return ACT_CTRL_SHIFT_HOME; return ACT_HOME; }
	if (final == 'F') { if (mod == 2) return ACT_SHIFT_END; if (mod == 4) return ACT_ALT_SHIFT_END; if (mod == 5) return ACT_CTRL_END; if (mod == 6) return ACT_CTRL_SHIFT_END; return ACT_END; }
	if (final == 'P') return ((mod == 3) || g_mod_alt) ? ACT_ALT_F1 : (((mod == 2) || g_mod_shift) ? ACT_SHIFT_F1 : ACT_F1);
	if (final == 'Q') return ((mod == 3) || g_mod_alt) ? ACT_ALT_F2 : (((mod == 2) || g_mod_shift) ? ACT_SHIFT_F2 : ACT_F2);
	if (final == 'R') return ACT_F3;
	if (final == 'S') return ((mod == 5) || g_mod_ctrl) ? ACT_CTRL_F4 : (((mod == 2) || g_mod_shift) ? ACT_SHIFT_F4 : ACT_F4);

	if (final == '~') {
		if (p1 == 1 || p1 == 7) { if (mod == 2) return ACT_SHIFT_HOME; if (mod == 4) return ACT_ALT_SHIFT_HOME; if (mod == 5) return ACT_CTRL_HOME; if (mod == 6) return ACT_CTRL_SHIFT_HOME; return ACT_HOME; }
		if (p1 == 4 || p1 == 8) { if (mod == 2) return ACT_SHIFT_END; if (mod == 4) return ACT_ALT_SHIFT_END; if (mod == 5) return ACT_CTRL_END; if (mod == 6) return ACT_CTRL_SHIFT_END; return ACT_END; }
		if (p1 == 2) return (mod == 2) ? ACT_SHIFT_INSERT : ACT_INSERT;
		if (p1 == 3) return ACT_DELETE;
		if (p1 == 5) return (mod == 5) ? ACT_CTRL_PGUP : ACT_PGUP;
		if (p1 == 6) return (mod == 5) ? ACT_CTRL_PGDN : ACT_PGDN;
		if (p1 == 11) return ((mod == 3) || g_mod_alt) ? ACT_ALT_F1 : (((mod == 2) || g_mod_shift) ? ACT_SHIFT_F1 : ACT_F1);
		if (p1 == 12) return ((mod == 3) || g_mod_alt) ? ACT_ALT_F2 : (((mod == 2) || g_mod_shift) ? ACT_SHIFT_F2 : ACT_F2);
		if (p1 == 13) return (mod == 5) ? ACT_CTRL_F3 : ACT_F3;
		if (p1 == 14) return ((mod == 5) || g_mod_ctrl) ? ACT_CTRL_F4 : (((mod == 2) || g_mod_shift) ? ACT_SHIFT_F4 : ACT_F4);
		if (p1 == 15) return ((mod == 5) || g_mod_ctrl) ? ACT_CTRL_F5 : (((mod == 3) || g_mod_alt) ? ACT_ALT_F5 : (((mod == 2) || g_mod_shift) ? ACT_SHIFT_F5 : ACT_F5));
		if (p1 == 17) return ((mod == 5) || g_mod_ctrl) ? ACT_CTRL_F6 : (((mod == 3) || g_mod_alt) ? ACT_ALT_F6 : (((mod == 2) || g_mod_shift) ? ACT_SHIFT_F6 : ACT_F6));
		if (p1 == 18) { if ((mod == 3) || g_mod_alt) return ACT_ALT_F7; if ((mod == 2) || g_mod_shift) return ACT_SHIFT_F7; if ((mod == 5) || g_mod_ctrl) return ACT_CTRL_F7; return ACT_F7; }
		if (p1 == 19) return ((mod == 3) || g_mod_alt) ? ACT_ALT_F8 : (((mod == 2) || g_mod_shift) ? ACT_SHIFT_F8 : (((mod == 5) || g_mod_ctrl) ? ACT_CTRL_F8 : ACT_F8));
		if (p1 == 20) return ((mod == 3) || g_mod_alt) ? ACT_ALT_F9 : ACT_F9;
		if (p1 == 21) return ((mod == 3) || g_mod_alt) ? ACT_ALT_F10 : ACT_F10;
		if (p1 == 23) return ((mod == 3) || g_mod_alt) ? ACT_ALT_F11 : ACT_F11;
		if (p1 == 24) return ((mod == 3) || g_mod_alt) ? ACT_ALT_F12 : ACT_F12;
		if (p1 == 26) return ACT_SHIFT_F4;
	}
	return ACT_NONE;
}

extern int g_inotify_fd;

static int poll_stdin_or_inotify(void) {
	while (1) {
		if (g_resized) return 0;
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);
		int maxfd = STDIN_FILENO;
		if (g_inotify_fd >= 0) {
			FD_SET(g_inotify_fd, &fds);
			if (g_inotify_fd > maxfd) maxfd = g_inotify_fd;
		}
		int r = select(maxfd + 1, &fds, NULL, NULL, NULL);
		if (r < 0) {
			if (errno == EINTR) return 0;
			return 1;
		}
		if (FD_ISSET(STDIN_FILENO, &fds)) return 1;
		if (g_inotify_fd >= 0 && FD_ISSET(g_inotify_fd, &fds)) {
			char buf[4096];
			while (read(g_inotify_fd, buf, sizeof(buf)) > 0) {}
			for (int step = 0; step < 3; step++) {
				fd_set dfds;
				FD_ZERO(&dfds);
				FD_SET(STDIN_FILENO, &dfds);
				FD_SET(g_inotify_fd, &dfds);
				struct timeval tv = {0, 100000};
				int dr = select(maxfd + 1, &dfds, NULL, NULL, &tv);
				if (dr <= 0) break;
				if (FD_ISSET(STDIN_FILENO, &dfds)) return 1;
				if (FD_ISSET(g_inotify_fd, &dfds)) {
					while (read(g_inotify_fd, buf, sizeof(buf)) > 0) {}
				}
			}
			return 2;
		}
	}
}

InputEvent input_read(void) {
	struct termios cur_t;
	if (tcgetattr(STDIN_FILENO, &cur_t) == 0 && (cur_t.c_lflag & (ICANON | ECHO))) {
		input_enable_raw();
	}
	InputEvent ev = {ACT_NONE, 0, -1, -1};
	if (g_resized) { g_resized = 0; ev.action = ACT_RESIZE; return ev; }
	int pr = poll_stdin_or_inotify();
	if (pr == 0) {
		if (g_resized) { g_resized = 0; ev.action = ACT_RESIZE; }
		return ev;
	}
	if (pr == 2) {
		ev.action = ACT_DIR_CHANGED;
		return ev;
	}
	unsigned char b = 0;
	if (!read_byte(&b)) {
		if (g_resized) { g_resized = 0; ev.action = ACT_RESIZE; }
		return ev;
	}

	if (b == 27) {
		if (!wait_byte(20)) { ev.action = ACT_ESC; return ev; }
		unsigned char b2 = 0;
		if (!read_byte(&b2)) { ev.action = ACT_ESC; return ev; }

		if (b2 == 91) {
			if (!wait_byte(35)) { ev.action = ACT_INSERT_LEFT_PATH; return ev; }
			char seq[64] = {0};
			int idx = 0;
			unsigned char c = 0;
			if (read_byte(&c)) {
				if (c == 91) {
					if (wait_byte(35) && read_byte(&c)) {
						if (c == 'A') ev.action = ACT_F1;
						else if (c == 'B') ev.action = ACT_F2;
						else if (c == 'C') ev.action = ACT_F3;
						else if (c == 'D') ev.action = ACT_F4;
						else if (c == 'E') ev.action = ACT_F5;
					}
					return ev;
				}
				seq[idx++] = (char)c;
				while (idx < 62 && !(c >= 0x40 && c <= 0x7E)) {
					if (!wait_byte(35) || !read_byte(&c)) break;
					seq[idx++] = (char)c;
				}
				seq[idx] = '\0';
				if (seq[0] == '<') {
					int btn = 0, mx = 0, my = 0;
					char kind = seq[idx - 1];
					if (sscanf(seq + 1, "%d;%d;%d", &btn, &mx, &my) == 3 && kind == 'M') {
						ev.mouse_x = mx - 1;
						ev.mouse_y = my - 1;
						int base_btn = btn & 3;
						if (btn & 64) {
							ev.action = (base_btn == 0) ? ACT_MOUSE_WHEEL_UP : ACT_MOUSE_WHEEL_DOWN;
						} else if (base_btn == 0) {
							ev.action = ACT_MOUSE_LEFT;
						} else if (base_btn == 2) {
							ev.action = ACT_MOUSE_RIGHT;
						}
					}
					return ev;
				}
				ev.action = parse_csi(seq); if (ev.action == ACT_ALT_CHAR || ev.action == ACT_CTRL_CHAR || ev.action == ACT_CHAR) ev.ch = g_mod_ch;
				return ev;
			}
		} else if (b2 == 127 || b2 == 8) { ev.action = ACT_ALT_BACKSPACE; return ev; }
			if (b2 == 'O') {
			if (wait_byte(10)) {
				unsigned char b3 = 0;
				if (read_byte(&b3)) {
					if (b3 == 'A') ev.action = ACT_UP;
					else if (b3 == 'B') ev.action = ACT_DOWN;
					else if (b3 == 'C') ev.action = ACT_RIGHT;
					else if (b3 == 'D') ev.action = ACT_LEFT;
					else if (b3 == 'P') ev.action = g_mod_alt ? ACT_ALT_F1 : (g_mod_shift ? ACT_SHIFT_F1 : ACT_F1);
					else if (b3 == 'Q') ev.action = ACT_F2;
					else if (b3 == 'R') ev.action = ACT_F3;
					else if (b3 == 'S') ev.action = ACT_F4;
					else if (b3 == 'H') ev.action = ACT_HOME;
					else if (b3 == 'F') ev.action = ACT_END;
				}
			}
			return ev;
		} else if (b2 == 93) {
			ev.action = ACT_INSERT_RIGHT_PATH;
			return ev;
		} else if (b2 == 13 || b2 == 10) {
			ev.action = ACT_INSERT_NAME;
			return ev;
		} else if (b2 == 27 && wait_byte(10)) {
			unsigned char b3 = 0;
			if (read_byte(&b3) && b3 == 'O' && wait_byte(10)) {
				unsigned char b4 = 0;
				if (read_byte(&b4)) {
					if (b4 == 'P') { ev.action = ACT_ALT_F1; return ev; }
					if (b4 == 'Q') { ev.action = ACT_ALT_F2; return ev; }
				}
			}
			return ev;
		} else if (b2 >= 32) {
			char utf[8] = {0};
			int expected = 1;
			if ((b2 & 0xE0) == 0xC0) expected = 2;
			else if ((b2 & 0xF0) == 0xE0) expected = 3;
			else if ((b2 & 0xF8) == 0xF0) expected = 4;
			utf[0] = (char)b2;
			for (int i = 1; i < expected; i++) {
				unsigned char nb = 0;
				if (wait_byte(10) && read_byte(&nb)) utf[i] = (char)nb;
			}
			wchar_t wc = 0;
			if (mbtowc(&wc, utf, (size_t)expected) > 0) {
				ev.action = ACT_ALT_CHAR;
				ev.ch = wc;
			}
			return ev;
		}
		return ev;
	}

	if (b == 1)  { ev.action = ACT_CTRL_A; return ev; }
	if (b == 2)  { ev.action = ACT_CTRL_B; return ev; }
	if (b == 3)  { ev.action = ACT_CTRL_C; return ev; }
	if (b == 5)  { ev.action = ACT_CTRL_E; return ev; }
	if (b == 6)  { ev.action = ACT_CTRL_F; return ev; }
	if (b == 8 || b == 127) { ev.action = ACT_BACKSPACE; return ev; }
	if (b == 9)  { ev.action = ACT_TAB; return ev; }
	if (b == 10) { ev.action = ACT_INSERT_NAME; return ev; }
	if (b == 13) { ev.action = ACT_ENTER; return ev; }
	if (b == 15) { ev.action = ACT_CTRL_O; return ev; }
	if (b == 17) { ev.action = ACT_CTRL_Q; return ev; }
	if (b == 18) { ev.action = ACT_CTRL_R; return ev; }
	if (b == 21) { ev.action = ACT_CTRL_U; return ev; }
	if (b == 22) { ev.action = ACT_CTRL_V; return ev; }
	if (b == 24) { ev.action = ACT_CTRL_X; return ev; }
	if (b == 25) { ev.action = ACT_CTRL_Y; return ev; }
	if (b == 26) { ev.action = ACT_CTRL_Z; return ev; }
	if (b == 29) { ev.action = ACT_INSERT_RIGHT_PATH; return ev; }
	if (b == 31) { ev.action = ACT_TOGGLE_COMMENT; return ev; }

	if (b >= 32) {
		char utf[8] = {0};
		int expected = 1;
		if ((b & 0xE0) == 0xC0) expected = 2;
		else if ((b & 0xF0) == 0xE0) expected = 3;
		else if ((b & 0xF8) == 0xF0) expected = 4;

		utf[0] = (char)b;
		for (int i = 1; i < expected; i++) {
			unsigned char nb = 0;
			if (wait_byte(10) && read_byte(&nb)) utf[i] = (char)nb;
		}
		wchar_t wc = 0;
		if (mbtowc(&wc, utf, (size_t)expected) > 0) {
			ev.action = ACT_CHAR;
			ev.ch = wc;
		}
	}
	return ev;
}
