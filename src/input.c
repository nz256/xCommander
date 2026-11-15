#define _DEFAULT_SOURCE
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>
#include <signal.h>
#include <errno.h>
#include "input.h"

static volatile sig_atomic_t g_resized = 0;
static void on_sigwinch(int sig) { (void)sig; g_resized = 1; }

void input_enable_raw(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_sigwinch;
	sigaction(SIGWINCH, &sa, NULL);
	struct termios t;
	if (tcgetattr(STDIN_FILENO, &t) == 0) {
		t.c_iflag &= ~(unsigned int)(IXON | IXOFF | ICRNL | INLCR | IGNCR | ISTRIP);
		t.c_lflag &= ~(unsigned int)(ICANON | ECHO | ISIG | IEXTEN);
		t.c_cc[VMIN] = 1;
		t.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &t);
	}
	printf("\033[>4;2m\033[?1l\033>");
	fflush(stdout);
}

void input_restore_sane(void) {
	printf("\033[>4;0m");
	fflush(stdout);
	system("stty sane");
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
static KeyAction map_modified_code(int code, int mod) {
	g_mod_ch = 0;
	int has_shift = (mod == 2 || mod == 4 || mod == 6 || mod == 8);
	int has_alt   = (mod == 3 || mod == 4 || mod == 7 || mod == 8);
	int has_ctrl  = (mod == 5 || mod == 6 || mod == 7 || mod == 8);

	if (code == 13 || code == 10) {
		return (has_ctrl || has_alt) ? ACT_INSERT_NAME : ACT_ENTER;
	}
	if (code == 91 && (has_ctrl || has_alt)) return ACT_INSERT_LEFT_PATH;
	if (code == 93 && (has_ctrl || has_alt)) return ACT_INSERT_RIGHT_PATH;

	if (has_ctrl && !has_alt && !has_shift) {
		if (code == 'a' || code == 'A') return ACT_CTRL_A;
		if (code == 'b' || code == 'B') return ACT_CTRL_B;
		if (code == 'c' || code == 'C') return ACT_CTRL_C;
		if (code == 'e' || code == 'E') return ACT_CTRL_E;
		if (code == 'f' || code == 'F') return ACT_CTRL_F;
		if (code == 'j' || code == 'J') return ACT_INSERT_NAME;
		if (code == 'o' || code == 'O') return ACT_CTRL_O;
		if (code == 'q' || code == 'Q') return ACT_CTRL_Q;
		if (code == 'u' || code == 'U') return ACT_CTRL_U;
		if (code == 'v' || code == 'V') return ACT_CTRL_V;
		if (code == 'x' || code == 'X') return ACT_CTRL_X;
		if (code == 'y' || code == 'Y') return ACT_CTRL_Y;
	}
	if (has_alt && !has_ctrl && code >= 32) {
		g_mod_ch = (wchar_t)code;
		return ACT_ALT_CHAR;
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

	if (seq[len - 1] == 'u') {
		int code = 0, mod = 1;
		if (sscanf(seq, "%d;%du", &code, &mod) == 2) {
			return map_modified_code(code, mod);
		}
	}

	char final = seq[len - 1];
	int p1 = 1, mod = 1;
	if (len == 1) {
		mod = 1;
	} else if (sscanf(seq, "%d;%d", &p1, &mod) != 2) {
		sscanf(seq, "%d", &p1);
	}

	if (final == 'A') return (mod == 2) ? ACT_SHIFT_UP : ACT_UP;
	if (final == 'B') return (mod == 2) ? ACT_SHIFT_DOWN : ACT_DOWN;
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
	if (final == 'H') return ACT_HOME;
	if (final == 'F') return ACT_END;
	if (final == 'P') return ACT_F1;
	if (final == 'Q') return ACT_F2;
	if (final == 'R') return ACT_F3;
	if (final == 'S') return (mod == 2) ? ACT_SHIFT_F4 : ACT_F4;

	if (final == '~') {
		if (p1 == 1 || p1 == 7) return ACT_HOME;
		if (p1 == 4 || p1 == 8) return ACT_END;
		if (p1 == 2) return (mod == 2) ? ACT_SHIFT_INSERT : ACT_INSERT;
		if (p1 == 3) return ACT_DELETE;
		if (p1 == 5) return (mod == 5) ? ACT_CTRL_PGUP : ACT_PGUP;
		if (p1 == 6) return (mod == 5) ? ACT_CTRL_PGDN : ACT_PGDN;
		if (p1 == 11) return ACT_F1;
		if (p1 == 12) return ACT_F2;
		if (p1 == 13) return ACT_F3;
		if (p1 == 14) return (mod == 2) ? ACT_SHIFT_F4 : ACT_F4;
		if (p1 == 15) return ACT_F5;
		if (p1 == 17) return ACT_F6;
		if (p1 == 18) return (mod == 2) ? ACT_SHIFT_F7 : ACT_F7;
		if (p1 == 19) return ACT_F8;
		if (p1 == 20) return ACT_F9;
		if (p1 == 21) return ACT_F10;
		if (p1 == 26) return ACT_SHIFT_F4;
	}
	return ACT_NONE;
}

InputEvent input_read(void) {
	InputEvent ev = {ACT_NONE, 0};
	if (g_resized) { g_resized = 0; ev.action = ACT_RESIZE; return ev; }
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
			if (!wait_byte(5)) { ev.action = ACT_INSERT_LEFT_PATH; return ev; }
			char seq[32] = {0};
			int idx = 0;
			unsigned char c = 0;
			if (read_byte(&c)) {
				if (c == 91) {
					if (wait_byte(10) && read_byte(&c)) {
						if (c == 'A') ev.action = ACT_F1;
						else if (c == 'B') ev.action = ACT_F2;
						else if (c == 'C') ev.action = ACT_F3;
						else if (c == 'D') ev.action = ACT_F4;
						else if (c == 'E') ev.action = ACT_F5;
					}
					return ev;
				}
				seq[idx++] = (char)c;
				while (idx < 30 && !(c >= 0x40 && c <= 0x7E)) {
					if (!wait_byte(10) || !read_byte(&c)) break;
					seq[idx++] = (char)c;
				}
				seq[idx] = '\0';
				ev.action = parse_csi(seq); if (ev.action == ACT_ALT_CHAR) ev.ch = g_mod_ch;
				return ev;
			}
		} else if (b2 == 'O') {
			if (wait_byte(10)) {
				unsigned char b3 = 0;
				if (read_byte(&b3)) {
					if (b3 == 'A') ev.action = ACT_UP;
					else if (b3 == 'B') ev.action = ACT_DOWN;
					else if (b3 == 'C') ev.action = ACT_RIGHT;
					else if (b3 == 'D') ev.action = ACT_LEFT;
					else if (b3 == 'P') ev.action = ACT_F1;
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
	if (b == 21) { ev.action = ACT_CTRL_U; return ev; }
	if (b == 22) { ev.action = ACT_CTRL_V; return ev; }
	if (b == 24) { ev.action = ACT_CTRL_X; return ev; }
	if (b == 25) { ev.action = ACT_CTRL_Y; return ev; }
	if (b == 29) { ev.action = ACT_INSERT_RIGHT_PATH; return ev; }

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
