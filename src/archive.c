#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "archive.h"
#include "panel.h"
#include "ui.h"

typedef enum { ARC_NONE = 0, ARC_ZIP, ARC_TAR, ARC_7Z } ArchiveType;

static ArchiveType detect_archive_type(const char *fn) {
	if (!fn) return ARC_NONE;
	const char *dot = strrchr(fn, '.');
	if (!dot) return ARC_NONE;
	if (!strcasecmp(dot,".zip")||!strcasecmp(dot,".jar")||!strcasecmp(dot,".apk")||
	    !strcasecmp(dot,".whl")||!strcasecmp(dot,".epub")||!strcasecmp(dot,".docx")||
	    !strcasecmp(dot,".xlsx")||!strcasecmp(dot,".pptx")) return ARC_ZIP;
	if (!strcasecmp(dot,".tar")||!strcasecmp(dot,".tgz")||!strcasecmp(dot,".txz")||
	    !strcasecmp(dot,".tbz2")||!strcasecmp(dot,".tzst")) return ARC_TAR;
	size_t l = strlen(fn);
	if ((l>7&&!strcasecmp(fn+l-7,".tar.gz"))||(l>7&&!strcasecmp(fn+l-7,".tar.xz"))||
	    (l>8&&!strcasecmp(fn+l-8,".tar.bz2"))||(l>8&&!strcasecmp(fn+l-8,".tar.zst"))) return ARC_TAR;
	if (!strcasecmp(dot,".7z")||!strcasecmp(dot,".rar")) return ARC_7Z;
	return ARC_NONE;
}

int archive_is_supported(const char *fn) { return detect_archive_type(fn) != ARC_NONE; }

static void shell_quote(const char *in, char *out, size_t sz) {
	size_t j = 0;
	if (sz < 3) { if (sz > 0) out[0] = '\0'; return; }
	out[j++] = '\'';
	for (size_t i = 0; in[i]; i++) {
		if (in[i] == '\'') {
			if (j + 4 >= sz - 1) break;
			out[j++]='\''; out[j++]='\\'; out[j++]='\''; out[j++]='\'';
		} else { if (j + 1 >= sz - 1) break; out[j++] = in[i]; }
	}
	out[j++] = '\''; out[j] = '\0';
}

static void zip_escape_pattern(const char *in, char *out, size_t sz) {
	size_t j = 0;
	for (size_t i = 0; in[i] && j + 3 < sz; i++) {
		if (in[i]=='['||in[i]==']'||in[i]=='*'||in[i]=='?'||in[i]=='\\') out[j++]='\\';
		out[j++] = in[i];
	}
	out[j] = '\0';
}

static void panel_add_entry_dedup(Panel *p, const char *name, int is_dir, off_t size, time_t mt, mode_t mode) {
	if (!name || !*name || !strcmp(name,".") || !strcmp(name,"..")) return;
	for (int i = 0; i < p->count; i++) {
		if (!strcmp(p->files[i].name, name)) {
			if (is_dir && !p->files[i].is_dir) { p->files[i].is_dir = 1; p->files[i].mode = S_IFDIR|0755; }
			if (!is_dir && size > 0) p->files[i].size = size;
			if (mt > 0) p->files[i].mtime = mt;
			return;
		}
	}
	if (!panel_ensure_capacity(p, p->count + 1)) return;
	FileEntry *fe = &p->files[p->count++];
	memset(fe, 0, sizeof(*fe));
	snprintf(fe->name, MAX_FILENAME, "%s", name);
	fe->is_dir = is_dir ? 1 : 0; fe->size = is_dir ? 0 : size; fe->mtime = mt;
	fe->mode = is_dir ? (S_IFDIR|0755) : (mode ? mode : (S_IFREG|0644));
}

static void add_archive_rel_path(Panel *p, const char *full, int is_d, off_t sz, time_t mt) {
	char clean[MAX_PATH * 2];
	snprintf(clean, sizeof(clean), "%s", full);
	while (clean[0] == '.' && clean[1] == '/') memmove(clean, clean + 2, strlen(clean + 2) + 1);
	while (clean[0] == '/') memmove(clean, clean + 1, strlen(clean + 1) + 1);
	size_t cl = strlen(clean);
	while (cl > 0 && clean[cl - 1] == '/') { clean[--cl] = '\0'; is_d = 1; }
	if (!cl) return;
	const char *rel = clean;
	size_t dl = strlen(p->archive_dir);
	if (dl > 0) { if (strncmp(clean, p->archive_dir, dl) || clean[dl] != '/') return; rel = clean + dl + 1; }
	if (!*rel) return;
	const char *sl = strchr(rel, '/');
	if (sl) {
		size_t seg = (size_t)(sl - rel);
		if (seg >= MAX_FILENAME) seg = MAX_FILENAME - 1;
		char dn[MAX_FILENAME]; memcpy(dn, rel, seg); dn[seg] = '\0';
		panel_add_entry_dedup(p, dn, 1, 0, mt, S_IFDIR|0755);
	} else panel_add_entry_dedup(p, rel, is_d, sz, mt, is_d ? (S_IFDIR|0755) : (S_IFREG|0644));
}

static time_t parse_dt(const char *s, int is_zip) {
	struct tm t; memset(&t, 0, sizeof(t));
	int y=0,m=0,d=0,hh=0,mm=0,ss=0;
	int ok = is_zip ? (sscanf(s,"%4d%2d%2d.%2d%2d%2d",&y,&m,&d,&hh,&mm,&ss)==6)
	                : (sscanf(s,"%d-%d-%d %d:%d:%d",&y,&m,&d,&hh,&mm,&ss)>=5);
	if (!ok) return 0;
	t.tm_year=y-1900; t.tm_mon=m-1; t.tm_mday=d; t.tm_hour=hh; t.tm_min=mm; t.tm_sec=ss; t.tm_isdst=-1;
	return mktime(&t);
}

static int arc_entry_cmp(const void *a, const void *b) {
	const FileEntry *fa = (const FileEntry *)a;
	const FileEntry *fb = (const FileEntry *)b;
	if (strcmp(fa->name, "..") == 0) return -1;
	if (strcmp(fb->name, "..") == 0) return 1;
	if (fa->is_dir != fb->is_dir) return fb->is_dir - fa->is_dir;
	return strcasecmp(fa->name, fb->name);
}

int archive_load_dir(Panel *p) {
	if (!p || p->archive_file[0] == '\0') return 0;
	ArchiveType at = detect_archive_type(p->archive_file);
	if (at == ARC_NONE) at = ARC_ZIP;
	char saved[MAX_FILENAME] = "";
	if (p->parent_name[0]) { snprintf(saved, sizeof(saved), "%s", p->parent_name); p->parent_name[0] = '\0'; }
	else if (p->count > 0 && p->selected >= 0 && p->selected < p->count) snprintf(saved, sizeof(saved), "%s", p->files[p->selected].name);

	p->count = 1; memset(&p->files[0], 0, sizeof(FileEntry));
	strcpy(p->files[0].name, ".."); p->files[0].is_dir = 1; p->files[0].mode = S_IFDIR|0755;

	char qarc[MAX_PATH*2], cmd[MAX_PATH*2+64];
	shell_quote(p->archive_file, qarc, sizeof(qarc));
	if (at == ARC_ZIP) snprintf(cmd, sizeof(cmd), "unzip -Z -T %s 2>/dev/null", qarc);
	else if (at == ARC_TAR) snprintf(cmd, sizeof(cmd), "tar --full-time -tvf %s 2>/dev/null", qarc);
	else snprintf(cmd, sizeof(cmd), "7z l -slt %s 2>/dev/null", qarc);

	FILE *fp = popen(cmd, "r");
	if (!fp) return 0;
	char line[MAX_PATH*2+256];
	if (at == ARC_ZIP) {
		while (fgets(line, sizeof(line), fp)) {
			size_t l = strlen(line); while (l>0 && (line[l-1]=='\n'||line[l-1]=='\r')) line[--l]='\0';
			if (l < 30 || (line[0]!='-' && line[0]!='d' && line[0]!='l')) continue;
			char a[32],b[16],d[16],e[16],f[16],dt[32]; long long sz=0; int pos=0;
			if (sscanf(line,"%31s %15s %15s %lld %15s %15s %31s %n",a,b,d,&sz,e,f,dt,&pos)>=7 && pos>0)
				add_archive_rel_path(p, line+pos, line[0]=='d', (off_t)sz, parse_dt(dt,1));
		}
	} else if (at == ARC_TAR) {
		while (fgets(line, sizeof(line), fp)) {
			size_t l = strlen(line); while (l>0 && (line[l-1]=='\n'||line[l-1]=='\r')) line[--l]='\0';
			if (l < 30 || (line[0]!='-' && line[0]!='d' && line[0]!='l' && line[0]!='h')) continue;
			char a[32],o[64],ds[32],ts[32],dt[64]; long long sz=0; int pos=0;
			if (sscanf(line,"%31s %63s %lld %31s %31s %n",a,o,&sz,ds,ts,&pos)>=5 && pos>0) {
				char *fn = line+pos, *ar = strstr(fn," -> "); if (ar) *ar='\0';
				snprintf(dt, sizeof(dt), "%s %s", ds, ts);
				add_archive_rel_path(p, fn, line[0]=='d', (off_t)sz, parse_dt(dt,0));
			}
		}
	} else {
		char cp[MAX_PATH*2]=""; long long cz=0; int cd=0, in=0; time_t cm=0;
		while (fgets(line, sizeof(line), fp)) {
			size_t l = strlen(line); while (l>0 && (line[l-1]=='\n'||line[l-1]=='\r')) line[--l]='\0';
			if (!strcmp(line,"----------")) { in=1; continue; }
			if (!in) continue;
			if (!l) { if (cp[0]) add_archive_rel_path(p,cp,cd,(off_t)cz,cm); cp[0]='\0'; cz=0; cd=0; cm=0; continue; }
			if (!strncmp(line,"Path = ",7)) snprintf(cp,sizeof(cp),"%s",line+7);
			else if (!strncmp(line,"Size = ",7)) cz=atoll(line+7);
			else if (!strncmp(line,"Folder = +",10)||!strncmp(line,"Attributes = D",14)) cd=1;
			else if (!strncmp(line,"Modified = ",11)) cm=parse_dt(line+11,0);
		}
		if (cp[0]) add_archive_rel_path(p,cp,cd,(off_t)cz,cm);
	}
	pclose(fp);
	if (p->count > 2) qsort(&p->files[1], (size_t)(p->count - 1), sizeof(FileEntry), arc_entry_cmp);
	p->selected = 0;
	if (saved[0]) for (int i=0; i<p->count; i++) if (!strcmp(p->files[i].name, saved)) { p->selected=i; break; }
	return 1;
}

int archive_extract_one(const char *arc, const char *inner, char *out, size_t sz) {
	if (!arc || !inner || !out || !sz) return 0;
	char td[] = "/tmp/xc_arc_XXXXXX";
	if (!mkdtemp(td)) return 0;
	const char *b = strrchr(inner, '/'); b = b ? (b + 1) : inner;
	if (!*b) b = "file.tmp";
	snprintf(out, sz, "%s/%s", td, b);

	ArchiveType at = detect_archive_type(arc);
	if (at == ARC_NONE) at = ARC_ZIP;
	char qa[MAX_PATH*2], qo[MAX_PATH*2], cmd[MAX_PATH*6+128];
	shell_quote(arc, qa, sizeof(qa));
	shell_quote(out, qo, sizeof(qo));

	if (at == ARC_ZIP) {
		char esc[MAX_PATH*2], qi[MAX_PATH*4];
		zip_escape_pattern(inner, esc, sizeof(esc));
		shell_quote(esc, qi, sizeof(qi));
		snprintf(cmd, sizeof(cmd), "unzip -p %s %s > %s 2>/dev/null", qa, qi, qo);
	} else if (at == ARC_TAR) {
		char qi[MAX_PATH*2]; shell_quote(inner, qi, sizeof(qi));
		snprintf(cmd, sizeof(cmd), "tar -xOf %s %s > %s 2>/dev/null", qa, qi, qo);
	} else {
		char qi[MAX_PATH*2]; shell_quote(inner, qi, sizeof(qi));
		snprintf(cmd, sizeof(cmd), "7z x -so %s %s > %s 2>/dev/null", qa, qi, qo);
	}
	struct stat st;
	if (system(cmd) == 0 && stat(out, &st) == 0) return 1;
	unlink(out); rmdir(td);
	return 0;
}

int archive_extract_items(Panel *src, Panel *dst) {
	if (!src || !dst || !src->archive_file[0] || src->count <= 0) return 0;
	if (dst->archive_file[0]) { ui_message("Extract", "Cannot extract directly into another archive yet"); return 0; }
	int sc = 0, single = -1;
	for (int i = 0; i < src->count; i++) if (src->files[i].is_selected && strcmp(src->files[i].name, "..")) sc++;
	if (!sc) {
		if (src->selected < 0 || src->selected >= src->count || !strcmp(src->files[src->selected].name, "..")) return 0;
		single = src->selected;
	}
	char msg[MAX_PATH+128];
	if (sc) snprintf(msg, sizeof(msg), "Extract %d selected item(s) to %s?", sc, dst->current_path);
	else snprintf(msg, sizeof(msg), "Extract \"%s\" to %s?", src->files[single].name, dst->current_path);
	if (!ui_confirm("Extract from Archive", msg)) return 0;

	char sd[] = "/tmp/xc_ext_XXXXXX";
	if (!mkdtemp(sd)) return 0;
	ArchiveType at = detect_archive_type(src->archive_file);
	if (at == ARC_NONE) at = ARC_ZIP;
	char qa[MAX_PATH*2], qs[MAX_PATH*2];
	shell_quote(src->archive_file, qa, sizeof(qa));
	shell_quote(sd, qs, sizeof(qs));

	int ok = 0;
	for (int i = 0; i < src->count; i++) {
		if (sc) { if (!src->files[i].is_selected || !strcmp(src->files[i].name, "..")) continue; }
		else if (i != single) continue;
		char in[MAX_PATH*2], cmd[MAX_PATH*8+128];
		if (src->archive_dir[0]) snprintf(in, sizeof(in), "%s/%s", src->archive_dir, src->files[i].name);
		else snprintf(in, sizeof(in), "%s", src->files[i].name);

		if (at == ARC_ZIP) {
			char esc[MAX_PATH*2], qi[MAX_PATH*4], qis[MAX_PATH*4];
			zip_escape_pattern(in, esc, sizeof(esc));
			shell_quote(esc, qi, sizeof(qi));
			if (src->files[i].is_dir) {
				char es[MAX_PATH*2+4]; snprintf(es, sizeof(es), "%s/*", esc);
				shell_quote(es, qis, sizeof(qis));
				snprintf(cmd, sizeof(cmd), "unzip -oq %s %s %s -d %s >/dev/null 2>&1", qa, qi, qis, qs);
			} else snprintf(cmd, sizeof(cmd), "unzip -oq %s %s -d %s >/dev/null 2>&1", qa, qi, qs);
		} else if (at == ARC_TAR) {
			char qi[MAX_PATH*2]; shell_quote(in, qi, sizeof(qi));
			snprintf(cmd, sizeof(cmd), "tar -xf %s -C %s %s >/dev/null 2>&1", qa, qs, qi);
		} else {
			char qi[MAX_PATH*2]; shell_quote(in, qi, sizeof(qi));
			snprintf(cmd, sizeof(cmd), "7z x -y -o%s %s %s >/dev/null 2>&1", qs, qa, qi);
		}
		if (system(cmd) == 0) ok = 1;
		src->files[i].is_selected = 0;
	}

	char mf[MAX_PATH*2], qf[MAX_PATH*4], qd[MAX_PATH*2], mc[MAX_PATH*6+128], rc[MAX_PATH*2+32];
	if (src->archive_dir[0]) snprintf(mf, sizeof(mf), "%s/%s", sd, src->archive_dir);
	else snprintf(mf, sizeof(mf), "%s", sd);
	shell_quote(mf, qf, sizeof(qf));
	shell_quote(dst->current_path, qd, sizeof(qd));
	snprintf(mc, sizeof(mc), "cp -a %s/. %s/ 2>/dev/null", qf, qd);
	if (system(mc) == 0) ok = 1;
	snprintf(rc, sizeof(rc), "rm -rf %s", qs);
	(void)system(rc);
	return ok;
}

int archive_update_one(const char *arc, const char *inner, const char *src_file) {
	if (!arc || !inner || !src_file) return 0;
	ArchiveType at = detect_archive_type(arc);
	if (at == ARC_NONE) at = ARC_ZIP;
	if (at == ARC_TAR) {
		ui_message("Archive Edit", "Updating files inside compressed TAR archives is not supported");
		return 0;
	}

	char sd[] = "/tmp/xc_upd_XXXXXX";
	if (!mkdtemp(sd)) return 0;

	char dest_in_stage[MAX_PATH * 2];
	snprintf(dest_in_stage, sizeof(dest_in_stage), "%s/%s", sd, inner);
	char *sl = strrchr(dest_in_stage, '/');
	if (sl) {
		*sl = '\0';
		char qdir[MAX_PATH * 4], mkcmd[MAX_PATH * 4 + 32];
		shell_quote(dest_in_stage, qdir, sizeof(qdir));
		snprintf(mkcmd, sizeof(mkcmd), "mkdir -p %s", qdir);
		(void)system(mkcmd);
		*sl = '/';
	}

	char qsrc[MAX_PATH * 2], qdst[MAX_PATH * 4], cpcmd[MAX_PATH * 6 + 32];
	shell_quote(src_file, qsrc, sizeof(qsrc));
	shell_quote(dest_in_stage, qdst, sizeof(qdst));
	snprintf(cpcmd, sizeof(cpcmd), "cp -f %s %s", qsrc, qdst);
	if (system(cpcmd) != 0) {
		char qs[MAX_PATH], rmcmd[MAX_PATH + 32];
		shell_quote(sd, qs, sizeof(qs));
		snprintf(rmcmd, sizeof(rmcmd), "rm -rf %s", qs);
		(void)system(rmcmd);
		return 0;
	}

	char qa[MAX_PATH * 2], qs[MAX_PATH * 2], qi[MAX_PATH * 2], cmd[MAX_PATH * 6 + 128];
	shell_quote(arc, qa, sizeof(qa));
	shell_quote(sd, qs, sizeof(qs));
	shell_quote(inner, qi, sizeof(qi));

	if (at == ARC_ZIP) {
		snprintf(cmd, sizeof(cmd), "cd %s && zip -q %s %s >/dev/null 2>&1", qs, qa, qi);
	} else {
		snprintf(cmd, sizeof(cmd), "cd %s && 7z u -y %s %s >/dev/null 2>&1", qs, qa, qi);
	}
	int ok = (system(cmd) == 0) ? 1 : 0;

	char rmcmd[MAX_PATH * 2 + 32];
	snprintf(rmcmd, sizeof(rmcmd), "rm -rf %s", qs);
	(void)system(rmcmd);
	return ok;
}

int archive_delete_items(Panel *src) {
	if (!src || !src->archive_file[0] || src->count <= 0) return 0;
	ArchiveType at = detect_archive_type(src->archive_file);
	if (at == ARC_NONE) at = ARC_ZIP;
	if (at == ARC_TAR) {
		ui_message("Archive Delete", "Deleting items inside compressed TAR archives is not supported");
		return 0;
	}

	int sc = 0, single = -1;
	for (int i = 0; i < src->count; i++) if (src->files[i].is_selected && strcmp(src->files[i].name, "..")) sc++;
	if (!sc) {
		if (src->selected < 0 || src->selected >= src->count || !strcmp(src->files[src->selected].name, "..")) return 0;
		single = src->selected;
	}

	char msg[MAX_FILENAME + 96];
	if (sc) snprintf(msg, sizeof(msg), "Delete %d selected item(s) from archive?", sc);
	else snprintf(msg, sizeof(msg), "Delete \"%s\" from archive?", src->files[single].name);
	if (!ui_confirm("Archive Delete", msg)) return 0;

	char qa[MAX_PATH * 2];
	shell_quote(src->archive_file, qa, sizeof(qa));

	int ok = 0;
	for (int i = 0; i < src->count; i++) {
		if (sc) { if (!src->files[i].is_selected || !strcmp(src->files[i].name, "..")) continue; }
		else if (i != single) continue;

		char in[MAX_PATH * 2], cmd[MAX_PATH * 6 + 128];
		if (src->archive_dir[0]) snprintf(in, sizeof(in), "%s/%s", src->archive_dir, src->files[i].name);
		else snprintf(in, sizeof(in), "%s", src->files[i].name);

		if (at == ARC_ZIP) {
			char esc[MAX_PATH * 2], qi[MAX_PATH * 4];
			zip_escape_pattern(in, esc, sizeof(esc));
			if (src->files[i].is_dir) {
				char es[MAX_PATH * 2 + 4], qis[MAX_PATH * 4];
				snprintf(es, sizeof(es), "%s/*", esc);
				shell_quote(esc, qi, sizeof(qi));
				shell_quote(es, qis, sizeof(qis));
				snprintf(cmd, sizeof(cmd), "zip -dq %s %s %s >/dev/null 2>&1", qa, qi, qis);
			} else {
				shell_quote(esc, qi, sizeof(qi));
				snprintf(cmd, sizeof(cmd), "zip -dq %s %s >/dev/null 2>&1", qa, qi);
			}
		} else {
			char qi[MAX_PATH * 2];
			shell_quote(in, qi, sizeof(qi));
			snprintf(cmd, sizeof(cmd), "7z d -y %s %s >/dev/null 2>&1", qa, qi);
		}
		if (system(cmd) == 0) ok = 1;
		src->files[i].is_selected = 0;
	}
	return ok;
}

int archive_create_from_panel(Panel *src) {
	if (!src || src->archive_file[0] != '\0' || src->count <= 0) return 0;

	int sc = 0, single = -1;
	for (int i = 0; i < src->count; i++) if (src->files[i].is_selected && strcmp(src->files[i].name, "..")) sc++;
	if (!sc) {
		if (src->selected < 0 || src->selected >= src->count || !strcmp(src->files[src->selected].name, "..")) return 0;
		single = src->selected;
	}

	char arc_name[MAX_FILENAME] = "";
	if (sc == 1) {
		for (int i = 0; i < src->count; i++) if (src->files[i].is_selected) { snprintf(arc_name, sizeof(arc_name), "%s.zip", src->files[i].name); break; }
	} else if (sc > 1) {
		const char *bn = strrchr(src->current_path, '/');
		bn = (bn && *(bn + 1)) ? (bn + 1) : "archive";
		snprintf(arc_name, sizeof(arc_name), "%s.zip", bn);
	} else {
		snprintf(arc_name, sizeof(arc_name), "%s.zip", src->files[single].name);
	}

	if (!ui_input("Pack to Archive (.zip/.tar.gz/.tar.xz/.7z)", "Archive name:", arc_name, sizeof(arc_name)) || arc_name[0] == '\0') {
		return 0;
	}

	char full_arc[MAX_PATH * 2];
	if (arc_name[0] == '/') snprintf(full_arc, sizeof(full_arc), "%s", arc_name);
	else if (!strcmp(src->current_path, "/")) snprintf(full_arc, sizeof(full_arc), "/%s", arc_name);
	else snprintf(full_arc, sizeof(full_arc), "%s/%s", src->current_path, arc_name);

	ArchiveType at = detect_archive_type(full_arc);
	if (at == ARC_NONE) at = ARC_ZIP;

	char list_file[] = "/tmp/xc_pack_XXXXXX";
	int lfd = mkstemp(list_file);
	if (lfd < 0) return 0;
	FILE *lf = fdopen(lfd, "w");
	if (!lf) { close(lfd); unlink(list_file); return 0; }

	for (int i = 0; i < src->count; i++) {
		if (sc) { if (!src->files[i].is_selected || !strcmp(src->files[i].name, "..")) continue; }
		else if (i != single) continue;
		fprintf(lf, "%s\n", src->files[i].name);
		src->files[i].is_selected = 0;
	}
	fclose(lf);

	char qdir[MAX_PATH * 2], qarc[MAX_PATH * 4], qlist[64], cmd[MAX_PATH * 8 + 128];
	shell_quote(src->current_path, qdir, sizeof(qdir));
	shell_quote(full_arc, qarc, sizeof(qarc));
	shell_quote(list_file, qlist, sizeof(qlist));

	if (at == ARC_TAR) {
		const char *zflag = "";
		size_t fl = strlen(full_arc);
		if ((fl > 4 && !strcasecmp(full_arc + fl - 4, ".tgz")) || (fl > 7 && !strcasecmp(full_arc + fl - 7, ".tar.gz"))) zflag = "z";
		else if ((fl > 4 && !strcasecmp(full_arc + fl - 4, ".txz")) || (fl > 7 && !strcasecmp(full_arc + fl - 7, ".tar.xz"))) zflag = "J";
		else if ((fl > 5 && !strcasecmp(full_arc + fl - 5, ".tbz2")) || (fl > 8 && !strcasecmp(full_arc + fl - 8, ".tar.bz2"))) zflag = "j";
		else if ((fl > 5 && !strcasecmp(full_arc + fl - 5, ".tzst")) || (fl > 8 && !strcasecmp(full_arc + fl - 8, ".tar.zst"))) zflag = "--zstd ";
		if (zflag[0] == '-') snprintf(cmd, sizeof(cmd), "cd %s && tar %s-cf %s -T %s >/dev/null 2>&1", qdir, zflag, qarc, qlist);
		else snprintf(cmd, sizeof(cmd), "cd %s && tar -c%sf %s -T %s >/dev/null 2>&1", qdir, zflag, qarc, qlist);
	} else if (at == ARC_7Z) {
		snprintf(cmd, sizeof(cmd), "cd %s && 7z a -y %s @%s >/dev/null 2>&1", qdir, qarc, qlist);
	} else {
		snprintf(cmd, sizeof(cmd), "cd %s && cat %s | zip -rq %s -@ >/dev/null 2>&1", qdir, qlist, qarc);
	}

	int ok = (system(cmd) == 0) ? 1 : 0;
	unlink(list_file);
	if (ok) {
		const char *bn = strrchr(full_arc, '/');
		bn = bn ? (bn + 1) : full_arc;
		snprintf(src->parent_name, MAX_FILENAME, "%s", bn);
	} else {
		ui_message("Pack to Archive", "Failed to create archive (check zip/tar/7z)");
	}
	return ok;
}
