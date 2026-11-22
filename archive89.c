/*
 * archive89 - crawl a website (same host only) and save it to disk,
 * with links rewritten so the copy works offline.
 *
 * Build: cc -std=c89 -pedantic -Wall -O2 -o archive89 archive89.c
 * No libraries needed: the HTTP fetcher is an external command (see -h).
 *
 * Resume: progress is journaled to <metadata dir>/.archive89.map after every
 * single step. Run the same command again and it continues where it stopped.
 * Use --fresh to throw the saved progress away.
 */
#define _POSIX_C_SOURCE 200112L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define HSIZE 65536
#define MAXREF 2000
#define MAXBODY (200UL * 1024UL * 1024UL)
#define DEFAULT_AGENT "Mozilla/5.0 (X11; Linux x86_64; rv:140.0) Gecko/20100101 Firefox/140.0"

#define PANEL 7		/* lines pinned at the top of a terminal */
#define BARW 30

#define SAVE    "\033" "7"
#define RESTORE "\033" "8"
#define C_RESET "\033[0m"
#define C_BOLD  "\033[1m"
#define C_DIM   "\033[2m"
#define C_RED   "\033[31m"
#define C_GREEN "\033[32m"
#define C_YELLOW "\033[33m"
#define C_CYAN  "\033[36m"

/* ---------- helpers ---------- */

static void die(const char *msg)
{
	fprintf(stderr, "archive89: %s\n", msg);
	exit(1);
}

static void *xrealloc(void *p, size_t n)
{
	p = realloc(p, n);
	if (!p)
		die("out of memory");
	return p;
}

static char *xstrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = (char *)xrealloc(NULL, n);
	memcpy(p, s, n);
	return p;
}

static void copy_cap(char *dst, const char *src, size_t cap)
{
	size_t n = strlen(src);
	if (n >= cap)
		n = cap - 1;
	memcpy(dst, src, n);
	dst[n] = 0;
}

static int ci_ncmp(const char *a, const char *b, size_t n)
{
	size_t i;
	for (i = 0; i < n; i++) {
		int x = tolower((unsigned char)a[i]);
		int y = tolower((unsigned char)b[i]);
		if (x != y)
			return x - y;
		if (!x)
			return 0;
	}
	return 0;
}

static int ci_contains(const char *hay, const char *needle)
{
	size_t n = strlen(needle);
	for (; *hay; hay++)
		if (ci_ncmp(hay, needle, n) == 0)
			return 1;
	return 0;
}

/* growable, always NUL-terminated buffer */
typedef struct {
	char *p;
	size_t len, cap;
} buf;

static void buf_add(buf *b, const char *s, size_t n)
{
	if (b->len + n + 1 > b->cap) {
		size_t nc = b->cap ? b->cap : 4096;
		while (nc < b->len + n + 1)
			nc *= 2;
		b->p = (char *)xrealloc(b->p, nc);
		b->cap = nc;
	}
	memcpy(b->p + b->len, s, n);
	b->len += n;
	b->p[b->len] = 0;
}

static void buf_adds(buf *b, const char *s)
{
	buf_add(b, s, strlen(s));
}

static void buf_addc(buf *b, char c)
{
	buf_add(b, &c, 1);
}

/* ---------- URLs ---------- */

struct url {
	char scheme[8];
	char host[256];
	char path[2048];
	char query[2048];
	char frag[256];
};

/* remove . and .. segments; result always starts with '/' */
static void norm_path(const char *in, char *out, size_t cap)
{
	size_t o = 0, n;
	const char *p = in, *s;
	int trail = 0;

	out[0] = 0;
	while (*p) {
		while (*p == '/')
			p++;
		s = p;
		while (*p && *p != '/')
			p++;
		n = (size_t)(p - s);
		if (n == 0)
			break;
		if (n == 1 && s[0] == '.') {
			trail = 1;
			continue;
		}
		if (n == 2 && s[0] == '.' && s[1] == '.') {
			while (o > 0 && out[o - 1] != '/')
				o--;
			if (o > 0)
				o--;
			out[o] = 0;
			trail = 1;
			continue;
		}
		trail = 0;
		if (o + n + 2 >= cap)
			break;
		out[o++] = '/';
		memcpy(out + o, s, n);
		o += n;
		out[o] = 0;
	}
	if (in[0] && in[strlen(in) - 1] == '/')
		trail = 1;
	if (o == 0) {
		out[0] = '/';
		out[1] = 0;
	} else if (trail && o + 1 < cap) {
		out[o++] = '/';
		out[o] = 0;
	}
}

static void set_pathquery(struct url *u, const char *r, const struct url *base)
{
	char tmp[2300], joined[4400];
	char *h, *qm;
	const char *bp, *sl;
	size_t n, d;

	n = strlen(r);
	if (n >= sizeof tmp)
		n = sizeof tmp - 1;
	memcpy(tmp, r, n);
	tmp[n] = 0;
	u->frag[0] = 0;
	u->query[0] = 0;

	h = strchr(tmp, '#');
	if (h) {
		*h = 0;
		copy_cap(u->frag, h + 1, sizeof u->frag);
	}
	qm = strchr(tmp, '?');
	if (qm) {
		*qm = 0;
		copy_cap(u->query, qm + 1, sizeof u->query);
	}

	if (tmp[0] == 0) {
		if (base) {
			copy_cap(u->path, base->path, sizeof u->path);
			if (!qm)
				copy_cap(u->query, base->query, sizeof u->query);
		} else {
			strcpy(u->path, "/");
		}
	} else if (tmp[0] == '/') {
		norm_path(tmp, u->path, sizeof u->path);
	} else {
		bp = base ? base->path : "/";
		sl = strrchr(bp, '/');
		d = sl ? (size_t)(sl - bp) + 1 : 0;
		if (d + strlen(tmp) >= sizeof joined) {
			strcpy(u->path, "/");
			return;
		}
		memcpy(joined, bp, d);
		strcpy(joined + d, tmp);
		norm_path(joined, u->path, sizeof u->path);
	}
}

/* parse "http(s)://host/..." ; returns 1 on success */
static int parse_abs(const char *s, struct url *u)
{
	const char *p;
	size_t n, i;

	while (isspace((unsigned char)*s))
		s++;
	if (ci_ncmp(s, "https://", 8) == 0) {
		strcpy(u->scheme, "https");
		p = s + 8;
	} else if (ci_ncmp(s, "http://", 7) == 0) {
		strcpy(u->scheme, "http");
		p = s + 7;
	} else {
		return 0;
	}
	n = strcspn(p, "/?#");
	if (n == 0 || n >= sizeof u->host)
		return 0;
	for (i = 0; i < n; i++)
		u->host[i] = (char)tolower((unsigned char)p[i]);
	u->host[n] = 0;
	set_pathquery(u, p + n, NULL);
	return 1;
}

static int resolve(const struct url *base, const char *ref, struct url *u)
{
	const char *p = ref;
	char tmp[2300];
	size_t i;

	while (isspace((unsigned char)*p))
		p++;
	if (*p == 0 || *p == '#')
		return 0;

	if (isalpha((unsigned char)p[0])) {
		i = 0;
		while (isalnum((unsigned char)p[i]) || p[i] == '+' ||
		       p[i] == '-' || p[i] == '.')
			i++;
		if (p[i] == ':')
			return parse_abs(p, u);
	}
	if (p[0] == '/' && p[1] == '/') {
		if (strlen(p) + 8 >= sizeof tmp)
			return 0;
		sprintf(tmp, "%s:%s", base->scheme, p);
		return parse_abs(tmp, u);
	}
	strcpy(u->scheme, base->scheme);
	strcpy(u->host, base->host);
	set_pathquery(u, p, base);
	return 1;
}

static void url_full(const struct url *u, char *out)
{
	sprintf(out, "%s://%s%s%s%s", u->scheme, u->host, u->path,
		u->query[0] ? "?" : "", u->query);
}

/* URL -> path relative to the output root */
static void local_path(const struct url *u, char *out, size_t cap)
{
	char tmp[4400], suffix[16];
	const char *last, *dot;
	unsigned long h = 5381UL;
	const char *q;
	size_t n, stem;

	copy_cap(tmp, u->path + 1, sizeof tmp);
	n = strlen(tmp);
	if (n == 0 || tmp[n - 1] == '/') {
		strcat(tmp, "index.html");
	} else {
		last = strrchr(tmp, '/');
		last = last ? last + 1 : tmp;
		if (!strchr(last, '.'))
			strcat(tmp, "/index.html");
	}
	if (u->query[0]) {
		for (q = u->query; *q; q++)
			h = ((h * 33UL) + (unsigned char)*q) & 0xffffffffUL;
		sprintf(suffix, "_%08lx", h);
		last = strrchr(tmp, '/');
		last = last ? last + 1 : tmp;
		dot = strrchr(last, '.');
		stem = dot ? (size_t)(dot - tmp) : strlen(tmp);
		if (stem + strlen(suffix) + strlen(tmp + stem) + 1 < cap) {
			memcpy(out, tmp, stem);
			strcpy(out + stem, suffix);
			strcat(out, tmp + stem);
			return;
		}
	}
	copy_cap(out, tmp, cap);
}

/* does the last path segment have an extension that is in list? */
static int ext_in(const struct url *u, const char **list)
{
	const char *last = strrchr(u->path, '/');
	const char *dot;
	size_t n;
	int k;

	last = last ? last + 1 : u->path;
	dot = strrchr(last, '.');
	if (!dot)
		return 0;
	dot++;
	n = strlen(dot);
	for (k = 0; list[k]; k++)
		if (strlen(list[k]) == n && ci_ncmp(dot, list[k], n) == 0)
			return 1;
	return 0;
}

static int has_ext(const struct url *u)
{
	const char *last = strrchr(u->path, '/');
	last = last ? last + 1 : u->path;
	return strrchr(last, '.') != NULL;
}

static const char *html_exts[] = { "html", "htm", "xhtml", "shtml", NULL };
static const char *css_exts[] = { "css", NULL };
static const char *page_exts[] = { "html", "htm", "xhtml", "shtml", "php",
	"asp", "aspx", "jsp", "cgi", "css", NULL };

/* pages worth parsing for links (decided before fetching, by name only) */
static int is_crawlable(const struct url *u)
{
	return !has_ext(u) || ext_in(u, page_exts);
}

static int looks_html(const buf *b)
{
	char head[1025];
	size_t n = b->len < 1024 ? b->len : 1024, i;

	memcpy(head, b->p, n);
	head[n] = 0;
	for (i = 0; i < n; i++)
		if (head[i] == 0)
			return 0;
	return ci_contains(head, "<!doctype html") || ci_contains(head, "<html") ||
	       ci_contains(head, "<head") || ci_contains(head, "<body") ||
	       ci_contains(head, "<a ");
}

/* relative path from file 'from' to file 'to' (both root-relative) */
static void relpath(const char *from, const char *to, char *out)
{
	size_t i = 0, last = 0;
	const char *p;

	while (from[i] && to[i] && from[i] == to[i]) {
		if (from[i] == '/')
			last = i + 1;
		i++;
	}
	out[0] = 0;
	for (p = from + last; *p; p++)
		if (*p == '/')
			strcat(out, "../");
	strcat(out, to + last);
}

/* ---------- state ---------- */

#define ST_PENDING 0
#define ST_DONE    1
#define ST_FAILED  2

struct entry {
	char *url;
	char *lp;	/* path relative to output / metadata root */
	int state;
	int crawl;	/* parse for links before the download phase */
};

struct node {
	char *key;
	size_t idx;
	struct node *next;
};

static struct entry *ents;
static size_t nent, cent;
static struct node *table[HSIZE];

static char g_host[256];
static char g_outdir[2048];
static char g_metadir[2048];
static char g_jpath[4400];
static FILE *g_jf;
static const char *g_fetcher;
static char g_default_fetcher[1400];
static int g_every = 10;
static double g_timeout = 3.0;
static double g_wait = 10.0;
static double g_rate_wait = 60.0;
static int g_rate_retries = 10;
static unsigned long g_max_files;
static unsigned long g_reqs;
static unsigned long g_ok;	/* done (including earlier runs) */
static unsigned long g_fail;	/* failed in this run */
static unsigned long g_sess;	/* requests finished in this run */
static unsigned long g_bytes;	/* downloaded in this run */
static unsigned long g_resumed;	/* done entries loaded from the journal */
static time_t g_t0;
static int g_tty, g_color;
static const char *g_phase = "";
static char g_note[64];
static char g_now[2300];

static unsigned long hash_str(const char *s)
{
	unsigned long h = 5381UL;
	while (*s)
		h = ((h * 33UL) + (unsigned char)*s++) & 0xffffffffUL;
	return h;
}

/* returns entry index or -1 */
static long seen_get(const char *key)
{
	unsigned long h = hash_str(key) % HSIZE;
	struct node *n;

	for (n = table[h]; n; n = n->next)
		if (strcmp(n->key, key) == 0)
			return (long)n->idx;
	return -1;
}

static void seen_add(const char *key, size_t idx)
{
	unsigned long h = hash_str(key) % HSIZE;
	struct node *n;

	n = (struct node *)xrealloc(NULL, sizeof *n);
	n->key = xstrdup(key);
	n->idx = idx;
	n->next = table[h];
	table[h] = n;
}

/* ---------- filesystem ---------- */

static int mkdirs(const char *dir)
{
	char tmp[4400];
	size_t i, len = strlen(dir);

	if (len == 0 || len >= sizeof tmp)
		return -1;
	strcpy(tmp, dir);
	for (i = 1; i < len; i++) {
		if (tmp[i] == '/') {
			tmp[i] = 0;
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
				return -1;
			tmp[i] = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
		return -1;
	return 0;
}

static int save_file(const char *root, const char *lp, const char *data, size_t n)
{
	char full[4400];
	char *slash;
	FILE *f;
	int ok;

	if (strlen(root) + strlen(lp) + 2 > sizeof full)
		return -1;
	sprintf(full, "%s/%s", root, lp);
	slash = strrchr(full, '/');
	*slash = 0;
	if (mkdirs(full) != 0)
		return -1;
	*slash = '/';
	f = fopen(full, "wb");
	if (!f)
		return -1;
	ok = (n == 0 || fwrite(data, 1, n, f) == n);
	if (fclose(f) != 0)
		ok = 0;
	return ok ? 0 : -1;
}

/*
 * Metadata tree: same layout as the website. Empty file = not downloaded.
 * Non-empty file = downloaded; its content is the absolute path of the copy.
 */
static void meta_write(const char *lp, const char *content)
{
	if (save_file(g_metadir, lp, content, strlen(content)) != 0)
		fprintf(stderr, "WARN cannot write metadata for %s\n", lp);
}

/* ---------- journal (resume) ---------- */

/*
 * .archive89.map, one line per event, flushed immediately:
 *   P <url> <local path>    planned
 *   D <local path>          downloaded and saved
 *   F <local path>          failed (retried on the next run)
 */
static void jwrite(char tag, const char *a, const char *b)
{
	if (!g_jf)
		return;
	if (b)
		fprintf(g_jf, "%c %s %s\n", tag, a, b);
	else
		fprintf(g_jf, "%c %s\n", tag, a);
	fflush(g_jf);
}

/* rewrite the journal compactly (atomic via rename) and reopen for append */
static void write_plan(void)
{
	char tmp[4500];
	FILE *f;
	size_t i;

	if (g_jf) {
		fclose(g_jf);
		g_jf = NULL;
	}
	sprintf(tmp, "%s.tmp", g_jpath);
	f = fopen(tmp, "w");
	if (f) {
		fprintf(f, "# archive89 journal: P planned, D done, F failed\n");
		for (i = 0; i < nent; i++) {
			fprintf(f, "P %s %s\n", ents[i].url, ents[i].lp);
			if (ents[i].state == ST_DONE)
				fprintf(f, "D %s\n", ents[i].lp);
		}
		if (fclose(f) == 0)
			rename(tmp, g_jpath);
		else
			remove(tmp);
	}
	g_jf = fopen(g_jpath, "a");
}

static void add_entry(const char *full, const char *lp, int crawl)
{
	if (nent == cent) {
		cent = cent ? cent * 2 : 256;
		ents = (struct entry *)xrealloc(ents, cent * sizeof *ents);
	}
	seen_add(lp, nent);
	ents[nent].url = xstrdup(full);
	ents[nent].lp = xstrdup(lp);
	ents[nent].state = ST_PENDING;
	ents[nent].crawl = crawl;
	nent++;
}

/* replay the journal; returns number of entries loaded */
static size_t journal_load(void)
{
	char line[8192], full[4400];
	struct stat st;
	struct url u;
	FILE *f;
	char *sp, *p;
	size_t n, i;
	long k;

	f = fopen(g_jpath, "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof line, f)) {
		n = strlen(line);
		while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r'))
			line[--n] = 0;
		if (n < 3 || line[1] != ' ')
			continue;
		p = line + 2;
		if (line[0] == 'P') {
			sp = strchr(p, ' ');
			if (!sp)
				continue;
			*sp = 0;
			if (!parse_abs(p, &u))
				continue;
			if (nent == 0 && strcmp(u.host, g_host) != 0) {
				fclose(f);
				die("saved progress belongs to another site (use --fresh)");
			}
			if (seen_get(sp + 1) >= 0)
				continue;
			add_entry(p, sp + 1, is_crawlable(&u));
		} else if (line[0] == 'D') {
			k = seen_get(p);
			if (k >= 0)
				ents[k].state = ST_DONE;
		}
	}
	fclose(f);
	/* a "done" file that is gone from disk must be fetched again */
	for (i = 0; i < nent; i++) {
		if (ents[i].state != ST_DONE)
			continue;
		sprintf(full, "%s/%s", g_outdir, ents[i].lp);
		if (stat(full, &st) != 0)
			ents[i].state = ST_PENDING;
		else
			g_ok++;
	}
	g_resumed = g_ok;
	return nent;
}

/* returns 1 if the file is (now) part of the plan, 0 if over ARCHIVE89_MAX_FILES */
static int enqueue(const struct url *u)
{
	char lp[4400], full[4400];

	local_path(u, lp, sizeof lp);
	if (seen_get(lp) >= 0)
		return 1;
	if (g_max_files > 0 && (unsigned long)nent >= g_max_files)
		return 0;
	url_full(u, full);
	add_entry(full, lp, is_crawlable(u));
	meta_write(lp, "");
	jwrite('P', full, lp);
	return 1;
}

/* ---------- rewriting ---------- */

/* resolve ref; if same host: queue it and append the local relative path */
static int handle_ref(const char *ref, size_t n, const struct url *base,
		      const char *lp, buf *out)
{
	char tmp[2300], tp[4400], rel[8192];
	struct url u;
	size_t i, o = 0;

	if (n == 0 || n >= MAXREF)
		return 0;
	for (i = 0; i < n && o < sizeof tmp - 1; i++) {
		if (ref[i] == '&' && i + 5 <= n && strncmp(ref + i, "&amp;", 5) == 0) {
			tmp[o++] = '&';
			i += 4;
		} else {
			tmp[o++] = ref[i];
		}
	}
	while (o > 0 && isspace((unsigned char)tmp[o - 1]))
		o--;
	tmp[o] = 0;

	if (!resolve(base, tmp, &u))
		return 0;
	if (strcmp(u.host, g_host) != 0)
		return 0;
	if (!enqueue(&u))
		return 0;
	local_path(&u, tp, sizeof tp);
	relpath(lp, tp, rel);
	buf_adds(out, rel);
	if (u.frag[0]) {
		buf_addc(out, '#');
		buf_adds(out, u.frag);
	}
	return 1;
}

static int attr_at(const char *s, size_t i, size_t n, size_t *vs, char *q)
{
	static const char *names[] = { "href", "src", "poster", "data-src" };
	int k;
	size_t j, l;

	if (i > 0 && !isspace((unsigned char)s[i - 1]) &&
	    s[i - 1] != '"' && s[i - 1] != '\'')
		return 0;
	for (k = 0; k < 4; k++) {
		l = strlen(names[k]);
		if (i + l >= n || ci_ncmp(s + i, names[k], l) != 0)
			continue;
		j = i + l;
		while (j < n && isspace((unsigned char)s[j]))
			j++;
		if (j >= n || s[j] != '=')
			continue;
		j++;
		while (j < n && isspace((unsigned char)s[j]))
			j++;
		if (j >= n || (s[j] != '"' && s[j] != '\''))
			continue;
		*q = s[j];
		*vs = j + 1;
		return 1;
	}
	return 0;
}

static void rewrite_html(const char *s, size_t n, const struct url *base,
			 const char *lp, buf *out)
{
	size_t i = 0, vs, ve;
	char q;

	while (i < n) {
		if (attr_at(s, i, n, &vs, &q)) {
			buf_add(out, s + i, vs - i);
			ve = vs;
			while (ve < n && s[ve] != q)
				ve++;
			if (ve >= n) {
				buf_add(out, s + vs, n - vs);
				return;
			}
			if (!handle_ref(s + vs, ve - vs, base, lp, out))
				buf_add(out, s + vs, ve - vs);
			i = ve;
		} else {
			buf_addc(out, s[i]);
			i++;
		}
	}
}

static void rewrite_css(const char *s, size_t n, const struct url *base,
			const char *lp, buf *out)
{
	size_t i = 0, j, vs;
	char q;

	while (i < n) {
		if (i + 4 <= n && ci_ncmp(s + i, "url(", 4) == 0) {
			buf_add(out, s + i, 4);
			j = i + 4;
			while (j < n && isspace((unsigned char)s[j]))
				buf_addc(out, s[j++]);
			q = 0;
			if (j < n && (s[j] == '"' || s[j] == '\'')) {
				q = s[j];
				buf_addc(out, q);
				j++;
			}
			vs = j;
			if (q) {
				while (j < n && s[j] != q)
					j++;
			} else {
				while (j < n && s[j] != ')' &&
				       !isspace((unsigned char)s[j]))
					j++;
			}
			if (!handle_ref(s + vs, j - vs, base, lp, out))
				buf_add(out, s + vs, j - vs);
			i = j;
		} else {
			buf_addc(out, s[i]);
			i++;
		}
	}
}

/* ---------- fetcher ---------- */

#define FETCH_OK    0
#define FETCH_FAIL  (-1)
#define FETCH_RATE  1

/*
 * Runs:  sh -c '<ARCHIVE89_FETCHER> "$1"' archive89 <url>
 * (the ` "$1"` is only appended if the command does not use $1 itself).
 * The fetcher writes the response body to stdout and exits:
 *   0 = ok, 75 = too many requests (caller waits and retries), else failed.
 * Returns FETCH_OK, FETCH_RATE or FETCH_FAIL.
 */
static int run_fetcher(const char *url, buf *b)
{
	int fd[2], status, tooBig = 0;
	pid_t pid;
	char chunk[8192], *script;
	ssize_t n;

	script = (char *)xrealloc(NULL, strlen(g_fetcher) + 8);
	if (strstr(g_fetcher, "$1"))
		strcpy(script, g_fetcher);
	else
		sprintf(script, "%s \"$1\"", g_fetcher);

	if (pipe(fd) != 0) {
		free(script);
		return -1;
	}
	fflush(stdout);
	fflush(stderr);
	pid = fork();
	if (pid < 0) {
		close(fd[0]);
		close(fd[1]);
		free(script);
		return -1;
	}
	if (pid == 0) {
		close(fd[0]);
		if (dup2(fd[1], 1) < 0)
			_exit(126);
		close(fd[1]);
		execl("/bin/sh", "sh", "-c", script, "archive89", url, (char *)NULL);
		_exit(127);
	}
	close(fd[1]);
	free(script);
	for (;;) {
		n = read(fd[0], chunk, sizeof chunk);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;
		buf_add(b, chunk, (size_t)n);
		if (b->len > MAXBODY) {
			tooBig = 1;
			kill(pid, SIGKILL);
			break;
		}
	}
	close(fd[0]);
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	if (tooBig)
		return FETCH_FAIL;
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return FETCH_OK;
	if (WIFEXITED(status) && WEXITSTATUS(status) == 75)
		return FETCH_RATE;
	return FETCH_FAIL;
}

static void pause_for(double s)
{
	struct timespec ts;

	if (s <= 0)
		return;
	ts.tv_sec = (time_t)s;
	ts.tv_nsec = (long)((s - (double)ts.tv_sec) * 1e9);
	while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
		;
}

/* ---------- status panel (pinned to the top of a terminal) ---------- */

static const char *col(const char *c)
{
	return g_color ? c : "";
}

static int percent_of(unsigned long n)
{
	unsigned long p;

	if (nent == 0)
		return 0;
	p = (n * 100UL) / (unsigned long)nent;
	return p > 100UL ? 100 : (int)p;
}

static int term_width(void)
{
	struct winsize ws;

	if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 20)
		return (int)ws.ws_col;
	return 80;
}

static void fmt_bytes(unsigned long n, char *out)
{
	if (n < 1024UL)
		sprintf(out, "%lu B", n);
	else if (n < 1024UL * 1024UL)
		sprintf(out, "%.1f KB", (double)n / 1024.0);
	else if (n < 1024UL * 1024UL * 1024UL)
		sprintf(out, "%.1f MB", (double)n / (1024.0 * 1024.0));
	else
		sprintf(out, "%.2f GB", (double)n / (1024.0 * 1024.0 * 1024.0));
}

static void fmt_time(long s, char *out)
{
	if (s < 0)
		s = 0;
	sprintf(out, "%02ld:%02ld:%02ld", s / 3600L, (s / 60L) % 60L, s % 60L);
}

static void draw_bar(void)
{
	static const char dashes[] =
"------------------------------------------------------------"
"------------------------------------------------------------";
	char bar[BARW + 1], by[32], el[16], eta[16], now[2300];
	unsigned long fin = g_ok + g_fail, pend;
	long el_s;
	double rate;
	int pct, fill, k, w, room;

	if (!g_tty)
		return;
	w = term_width();
	pct = percent_of(fin);
	fill = pct * BARW / 100;
	for (k = 0; k < BARW; k++)
		bar[k] = k < fill ? '#' : '-';
	bar[BARW] = 0;
	pend = (unsigned long)nent > fin ? (unsigned long)nent - fin : 0;
	el_s = (long)(time(NULL) - g_t0);
	rate = el_s > 0 ? (double)g_sess / (double)el_s : 0.0;
	fmt_time(el_s, el);
	if (rate > 0.0 && pend > 0)
		fmt_time((long)((double)pend / rate), eta);
	else
		strcpy(eta, "--:--:--");
	fmt_bytes(g_bytes, by);
	room = w - 8;
	if (room < 4)
		room = 4;
	copy_cap(now, g_now, sizeof now);
	if ((int)strlen(now) > room)
		now[room] = 0;

	printf(SAVE);
	printf("\033[1;1H\033[2K %s%sarchive89%s  %s  %s%s%s",
	       col(C_BOLD), col(C_CYAN), col(C_RESET), g_host,
	       col(C_DIM), g_phase, col(C_RESET));
	printf("\033[2;1H\033[2K  [%s%s%s] %3d%%  %lu / %lu files",
	       col(C_GREEN), bar, col(C_RESET), pct, fin, (unsigned long)nent);
	printf("\033[3;1H\033[2K  %sok %lu%s  %sfailed %lu%s  pending %lu"
	       "  (resumed %lu)",
	       col(C_GREEN), g_ok, col(C_RESET),
	       g_fail ? col(C_RED) : "", g_fail, col(C_RESET), pend, g_resumed);
	printf("\033[4;1H\033[2K  elapsed %s  eta %s  %.1f req/s  data %s",
	       el, eta, rate, by);
	printf("\033[5;1H\033[2K  now: %s", now);
	printf("\033[6;1H\033[2K  %s%s%s", col(C_YELLOW), g_note, col(C_RESET));
	k = w - 1;
	if (k > 120)
		k = 120;
	printf("\033[7;1H\033[2K%s%.*s%s", col(C_DIM), k, dashes, col(C_RESET));
	printf(RESTORE);
	fflush(stdout);
}

static void tty_reset(void)
{
	if (g_tty) {
		printf("\033[r\033[999;1H\n");
		fflush(stdout);
	}
}

static void on_signal(int sig)
{
	ssize_t r;

	(void)sig;
	r = write(1, "\033[r\033[999;1H\n", 12);
	(void)r;
	_exit(130);
}

static void tty_setup(void)
{
	int k;

	g_tty = isatty(1);
	if (!g_tty)
		return;
	g_color = getenv("NO_COLOR") == NULL;
	for (k = 0; k < PANEL; k++)
		printf("\n");
	printf("\033[%d;r\033[%d;1H", PANEL + 1, PANEL + 1);
	fflush(stdout);
	atexit(tty_reset);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
}

/* sleep with a visible countdown in the panel */
static void wait_secs(double s, const char *label)
{
	double step;

	while (s > 0) {
		sprintf(g_note, "%s %ds", label, (int)(s + 0.999));
		draw_bar();
		step = s > 1.0 ? 1.0 : s;
		pause_for(step);
		s -= step;
	}
	g_note[0] = 0;
	draw_bar();
}

/* ---------- crawl ---------- */

static void mark_fail(size_t idx)
{
	ents[idx].state = ST_FAILED;
	g_fail++;
	jwrite('F', ents[idx].lp, NULL);
}

static void process_one(size_t idx)
{
	char *url = ents[idx].url;
	char *lp = ents[idx].lp;
	char loc[4400];
	struct url u;
	buf body, out;
	int html, rc, tries = 0;

	if (g_every > 0 && g_reqs > 0 && g_reqs % (unsigned long)g_every == 0) {
		printf("%sPAUSE%s %.1fs (after %lu requests)\n",
		       col(C_CYAN), col(C_RESET), g_timeout, g_reqs);
		fflush(stdout);
		wait_secs(g_timeout, "pause");
	}
	g_reqs++;

	memset(&body, 0, sizeof body);
	memset(&out, 0, sizeof out);
	if (!parse_abs(url, &u)) {
		mark_fail(idx);
		return;
	}
	for (;;) {
		rc = run_fetcher(url, &body);
		if (rc != FETCH_RATE)
			break;
		free(body.p);
		memset(&body, 0, sizeof body);
		if (++tries > g_rate_retries) {
			fprintf(stderr, "[%3d%%] %sFAIL%s %s (still rate limited, gave up)\n",
				percent_of(g_ok + g_fail + 1), col(C_RED), col(C_RESET), url);
			mark_fail(idx);
			return;
		}
		printf("%sRATE%s too many requests; waiting %.0fs, retry %d/%d: %s\n",
		       col(C_YELLOW), col(C_RESET), g_rate_wait, tries,
		       g_rate_retries, url);
		fflush(stdout);
		wait_secs(g_rate_wait, "rate limited, retry in");
	}
	if (rc != FETCH_OK) {
		fprintf(stderr, "[%3d%%] %sFAIL%s %s\n", percent_of(g_ok + g_fail + 1),
			col(C_RED), col(C_RESET), url);
		mark_fail(idx);
		free(body.p);
		return;
	}
	if (!body.p)
		buf_add(&body, "", 0);

	if (ext_in(&u, css_exts)) {
		rewrite_css(body.p, body.len, &u, lp, &out);
	} else {
		html = ext_in(&u, html_exts) || looks_html(&body);
		if (html)
			rewrite_html(body.p, body.len, &u, lp, &out);
		else
			buf_add(&out, body.p, body.len);
	}
	if (!out.p)
		buf_add(&out, "", 0);

	if (save_file(g_outdir, lp, out.p, out.len) == 0) {
		ents[idx].state = ST_DONE;
		g_ok++;
		g_bytes += (unsigned long)body.len;
		sprintf(loc, "%s/%s", g_outdir, lp);
		meta_write(lp, loc);
		jwrite('D', lp, NULL);
		printf("[%3d%%] %sOK%s   %s -> %s\n", percent_of(g_ok + g_fail),
		       col(C_GREEN), col(C_RESET), url, lp);
	} else {
		fprintf(stderr, "[%3d%%] %sFAIL%s cannot write %s\n",
			percent_of(g_ok + g_fail + 1), col(C_RED), col(C_RESET), lp);
		mark_fail(idx);
	}
	free(body.p);
	free(out.p);
}

static void process(size_t idx)
{
	copy_cap(g_now, ents[idx].url, sizeof g_now);
	draw_bar();
	process_one(idx);
	g_sess++;
	draw_bar();
}

/* ---------- main ---------- */

static void usage(FILE *f)
{
	fputs(
"usage: archive89 <url> [options]\n"
"       archive89 --url <url> [options]\n"
"\n"
"Crawls a website (same host only) and saves it for offline viewing.\n"
"Default output directory: ~/<domain_name>\n"
"Interrupted runs resume automatically when started again with the same\n"
"url and output directory.\n"
"\n"
"options:\n"
"  --url <url>         site to archive\n"
"  --output <dir>      save into <dir> instead of ~/<domain_name>\n"
"  --fresh             ignore saved progress and start over\n"
"  --tar.gz, --tar     also create <dir>.tar.gz\n", f);
	fputs(
"  -h, --help          show this help\n", f);
	fputs(
"\n"
"environment (all optional):\n"
"  ARCHIVE89_FETCHER      fetch command. The URL is appended as the last\n"
"                         argument (or use $1 in your own script). Body goes\n"
"                         to stdout. Exit 0 = ok, 75 = too many requests\n"
"                         (wait, retry), anything else = failed.\n"
"                         default: a curl wrapper (429 -> exit 75)\n"
"                         e.g. wget -qO- -U \"$ARCHIVE89_AGENT\"\n"
"", f);
	fputs(
"  ARCHIVE89_AGENT        user agent (default: Firefox 140 on Linux)\n"
"  ARCHIVE89_REFERER      Referer sent by the default fetcher\n"
"  ARCHIVE89_COOKIE       cookie string or file for the default fetcher\n"
"  ARCHIVE89_REQ_TIMEOUT  per-request timeout in seconds (default 60)\n", f);
	fputs(
"                         (AGENT, REFERER, COOKIE, REQ_TIMEOUT are exported\n"
"                         to the fetcher)\n"
"  ARCHIVE89_RATE_WAIT    seconds to wait when rate limited (default 60)\n"
"  ARCHIVE89_RATE_RETRIES retries per URL when rate limited (default 10)\n"
"", f);
	fputs(
"  ARCHIVE89_WAIT         wait after the crawl, before downloads (default 10)\n"
"  ARCHIVE89_EVERY        pause after every N requests (default 10, 0 = off)\n"
"  ARCHIVE89_TIMEOUT      length of that pause in seconds (default 3)\n"
"  ARCHIVE89_MAX_FILES    stop adding new files after N (default 0 = no limit)\n"
"  ARCHIVE89_OUTPUT       same as --output\n"
"  ARCHIVE89_TAR          1 = same as --tar.gz\n"
"  ARCHIVE89_METADATA     metadata directory (default <output dir>.metadata)\n", f);
	fputs(
"  NO_COLOR               set to disable colors\n", f);
	fputs(
"\n"
"Phase 1 crawls the site and maps every planned request (metadata tree +\n"
".archive89.map, a journal updated after every step). After ARCHIVE89_WAIT,\n"
"phase 2 downloads what is missing. In the metadata tree an empty file =\n"
"not downloaded, otherwise it holds the path of the saved copy. On a\n"
"terminal a status panel (progress, counts, speed, ETA) stays pinned to the\n"
"top. Failed files are retried on the next run.\n", f);
}

static void env_default(const char *name, const char *value)
{
	const char *e = getenv(name);

	if (!e || !*e)
		setenv(name, value, 1);
}

/* default fetcher: curl, reports HTTP 429 as exit code 75 */
static void build_default_fetcher(void)
{
	char *d = g_default_fetcher;

	d[0] = 0;
	strcat(d, "t=$(mktemp) || exit 1; ");
	strcat(d, "c=$(curl -sSL --compressed --max-time \"$ARCHIVE89_REQ_TIMEOUT\" "
		  "-A \"$ARCHIVE89_AGENT\" ");
	strcat(d, "${ARCHIVE89_REFERER:+-e \"$ARCHIVE89_REFERER\"} "
		  "${ARCHIVE89_COOKIE:+-b \"$ARCHIVE89_COOKIE\"} ");
	strcat(d, "-o \"$t\" -w '%{http_code}' \"$1\"); r=$?; ");
	strcat(d, "if [ $r -ne 0 ]; then rm -f \"$t\"; exit 1; fi; ");
	strcat(d, "case \"$c\" in 2*) cat \"$t\"; rm -f \"$t\"; exit 0;; "
		  "429) rm -f \"$t\"; exit 75;; esac; ");
	strcat(d, "rm -f \"$t\"; exit 22");
	g_fetcher = d;
}

static void expand_home(const char *in, char *out, size_t cap)
{
	const char *home = getenv("HOME");
	size_t n;

	if (in[0] == '~' && (in[1] == '/' || in[1] == 0) && home) {
		if (strlen(home) + strlen(in) >= cap)
			die("path too long");
		sprintf(out, "%s%s", home, in + 1);
	} else {
		if (strlen(in) >= cap)
			die("path too long");
		strcpy(out, in);
	}
	n = strlen(out);
	while (n > 1 && out[n - 1] == '/')
		out[--n] = 0;
}

static void make_abs(char *p, size_t cap)
{
	char cwd[2048], tmp[4400];

	if (p[0] == '/')
		return;
	if (!getcwd(cwd, sizeof cwd))
		die("cannot get current directory");
	if (strlen(cwd) + strlen(p) + 2 > sizeof tmp || strlen(cwd) + strlen(p) + 2 > cap)
		die("path too long");
	sprintf(tmp, "%s/%s", cwd, p);
	strcpy(p, tmp);
}

static int make_targz(const char *dir)
{
	char archive[2100], parent[2100];
	const char *base, *slash;
	pid_t pid;
	int status;

	if (strlen(dir) + 8 >= sizeof archive)
		return -1;
	sprintf(archive, "%s.tar.gz", dir);
	slash = strrchr(dir, '/');
	if (!slash) {
		strcpy(parent, ".");
		base = dir;
	} else if (slash == dir) {
		strcpy(parent, "/");
		base = dir + 1;
	} else {
		memcpy(parent, dir, (size_t)(slash - dir));
		parent[slash - dir] = 0;
		base = slash + 1;
	}
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execlp("tar", "tar", "-czf", archive, "-C", parent, base,
		       (char *)NULL);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	printf("tar: %s\n", archive);
	return 0;
}

int main(int argc, char **argv)
{
	const char *urlarg = NULL, *outarg = NULL, *e;
	char starturl[2300], hostdir[300];
	struct url start;
	size_t i, pending = 0, loaded;
	int a, tar = 0, fresh = 0;

	for (a = 1; a < argc; a++) {
		const char *arg = argv[a];
		if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
			usage(stdout);
			return 0;
		} else if (!strcmp(arg, "--url")) {
			if (++a >= argc) {
				usage(stderr);
				return 1;
			}
			urlarg = argv[a];
		} else if (!strcmp(arg, "--output") || !strcmp(arg, "-o")) {
			if (++a >= argc) {
				usage(stderr);
				return 1;
			}
			outarg = argv[a];
		} else if (!strcmp(arg, "--tar.gz") || !strcmp(arg, "--tar")) {
			tar = 1;
		} else if (!strcmp(arg, "--fresh")) {
			fresh = 1;
		} else if (arg[0] == '-') {
			fprintf(stderr, "archive89: unknown option %s\n", arg);
			usage(stderr);
			return 1;
		} else {
			urlarg = arg;
		}
	}
	if (!urlarg) {
		usage(stderr);
		return 1;
	}

	build_default_fetcher();
	e = getenv("ARCHIVE89_FETCHER");
	if (e && *e)
		g_fetcher = e;
	env_default("ARCHIVE89_AGENT", DEFAULT_AGENT);
	env_default("ARCHIVE89_REQ_TIMEOUT", "60");
	e = getenv("ARCHIVE89_RATE_WAIT");
	if (e && *e)
		g_rate_wait = atof(e);
	e = getenv("ARCHIVE89_RATE_RETRIES");
	if (e && *e)
		g_rate_retries = atoi(e);
	e = getenv("ARCHIVE89_MAX_FILES");
	if (e && *e)
		g_max_files = (unsigned long)atol(e);
	e = getenv("ARCHIVE89_OUTPUT");
	if (!outarg && e && *e)
		outarg = e;
	e = getenv("ARCHIVE89_TAR");
	if (e && *e && *e != '0')
		tar = 1;
	e = getenv("ARCHIVE89_EVERY");
	if (e && *e)
		g_every = atoi(e);
	e = getenv("ARCHIVE89_WAIT");
	if (e && *e)
		g_wait = atof(e);
	e = getenv("ARCHIVE89_TIMEOUT");
	if (e && *e)
		g_timeout = atof(e);

	if (strlen(urlarg) >= MAXREF)
		die("url too long");
	if (ci_ncmp(urlarg, "http://", 7) == 0 || ci_ncmp(urlarg, "https://", 8) == 0)
		strcpy(starturl, urlarg);
	else
		sprintf(starturl, "https://%s", urlarg);
	if (!parse_abs(starturl, &start))
		die("invalid url");
	strcpy(g_host, start.host);

	if (outarg) {
		expand_home(outarg, g_outdir, sizeof g_outdir);
	} else {
		const char *home = getenv("HOME");
		char *c;
		copy_cap(hostdir, g_host, sizeof hostdir);
		for (c = hostdir; *c; c++)
			if (*c == ':')
				*c = '_';
		if (!home)
			home = ".";
		if (strlen(home) + strlen(hostdir) + 2 >= sizeof g_outdir)
			die("output path too long");
		sprintf(g_outdir, "%s/%s", home, hostdir);
	}
	make_abs(g_outdir, sizeof g_outdir);

	e = getenv("ARCHIVE89_METADATA");
	if (e && *e) {
		expand_home(e, g_metadir, sizeof g_metadir);
	} else {
		if (strlen(g_outdir) + 10 >= sizeof g_metadir)
			die("path too long");
		sprintf(g_metadir, "%s.metadata", g_outdir);
	}
	make_abs(g_metadir, sizeof g_metadir);

	if (mkdirs(g_outdir) != 0)
		die("cannot create output directory");
	if (mkdirs(g_metadir) != 0)
		die("cannot create metadata directory");

	setvbuf(stdout, NULL, _IOLBF, 0);

	/* resume: replay the journal unless --fresh */
	sprintf(g_jpath, "%s/.archive89.map", g_metadir);
	if (fresh)
		remove(g_jpath);
	loaded = journal_load();
	write_plan();
	if (loaded > 0)
		printf("resuming: %lu planned, %lu already downloaded, %lu to do\n",
		       (unsigned long)loaded, g_resumed,
		       (unsigned long)loaded - g_resumed);

	g_t0 = time(NULL);
	tty_setup();
	enqueue(&start);

	g_phase = "phase 1/2: crawl";
	printf("== phase 1: crawl and map ==\n");
	for (i = 0; i < nent; i++)
		if (ents[i].state == ST_PENDING && ents[i].crawl)
			process(i);

	for (i = 0; i < nent; i++)
		if (ents[i].state == ST_PENDING)
			pending++;
	write_plan();
	printf("map: %lu planned requests, %lu still to download (%s)\n",
	       (unsigned long)nent, (unsigned long)pending, g_jpath);

	g_phase = "phase 2/2: download";
	wait_secs(g_wait, "starting in");
	printf("== phase 2: download ==\n");
	for (i = 0; i < nent; i++)
		if (ents[i].state == ST_PENDING)
			process(i);
	write_plan();
	g_phase = "finished";
	g_now[0] = 0;
	draw_bar();

	printf("done: %lu of %lu files downloaded (%lu failed, rerun to retry) -> %s\n",
	       g_ok, (unsigned long)nent, g_fail, g_outdir);
	printf("metadata: %s\n", g_metadir);

	if (tar && make_targz(g_outdir) != 0) {
		fprintf(stderr, "archive89: tar failed\n");
		return 1;
	}
	return 0;
}
