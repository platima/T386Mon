/*
 * T386MON: how fast Windows 95 runs on tiny386, seen from inside it.
 *
 * One window with three parts:
 *   - Windows' own figures: memory, the swap file, paging and the file
 *     system from Windows 95's performance data (HKEY_DYN_DATA\PerfStats,
 *     the figures System Monitor shows), the display mode, and Windows' own
 *     processor use when it is turned on (F7);
 *   - the emulator's figures from tiny386's information port at 0F1F8h
 *     (perfport.h), when the PC has one: the panel's frames a second (the
 *     pictures that changed; the board draws one every 30 ms changed or
 *     not), how busy the guest processor is, the drawing, the disk and more;
 *   - drawing tests that give frames a second at the current colour depth,
 *     with the mouse pointer parked where each test says.
 * Every few seconds a line, and every test's results, go to a log file
 * (C:\T386MON.LOG unless /log: says otherwise).  The lines are kept in
 * memory and written every 60 seconds, and after the tests: on the board
 * each write to the disk image leads to a flush to the card on the
 * emulator's own core, and none may fall inside a test.
 *
 * What it costs: on the PC harness (2 million guest instructions a second,
 * as on the board), drawing text in Windows at 800x480 takes about half a
 * millisecond of the guest's time a character, so the live view draws only
 * the numbers that changed, each in a box of its own; the slower Windows
 * figures are read every 5 seconds.  Windows' own processor figure is off
 * by default on tiny386: while it is measured, Windows 95 spins instead of
 * halting when it is idle, so the emulator would see the guest always busy.
 *
 * Built by build.sh with gcc for i686-w64-mingw32 and no C library at all:
 * Windows 95 has no MSVCRT.DLL, so this file has its own few helpers and
 * imports only KERNEL32, USER32, GDI32 and ADVAPI32 functions that Windows
 * 95 has (build.sh checks each).  tiny386 runs Windows as a 386 with no
 * floating point unit, so the code is integer only and for the 386
 * (build.sh checks the disassembly for x87 and later instructions).
 */
#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0400
#define _WIN32_WINNT 0x0400
#include <windows.h>
#include <stdarg.h>
#include "perfport.h"

#define APP_NAME    "T386MON"
#define APP_VERSION "1.2"

typedef unsigned long u32;

/* ==== a small runtime: no C library ==== */

void *memset(void *d, int c, size_t n)
{
	void *r = d;
	__asm__ __volatile__("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
	return r;
}

void *memcpy(void *d, const void *s, size_t n)
{
	void *r = d;
	__asm__ __volatile__("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
	return r;
}

void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *dp = d;
	const unsigned char *sp = s;
	if (dp <= sp || dp >= sp + n)
		return memcpy(d, s, n);
	while (n) {
		n--;
		dp[n] = sp[n];
	}
	return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *p = a, *q = b;
	for (; n; n--, p++, q++)
		if (*p != *q)
			return *p < *q ? -1 : 1;
	return 0;
}

static int slen(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	return n;
}

static void scopy(char *d, const char *s, int size)
{
	int i = 0;
	while (s[i] && i < size - 1) {
		d[i] = s[i];
		i++;
	}
	d[i] = 0;
}

static int seq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char) (c + 32) : c;
}

/* 1 if s starts with the word w (any case); *rest is what follows it */
static int word(const char *s, const char *w, const char **rest)
{
	while (*w) {
		if (lower(*s) != *w)
			return 0;
		s++;
		w++;
	}
	if (rest)
		*rest = s;
	return 1;
}

/* 1 if s is the switch w and nothing more */
static int flag(const char *s, const char *w)
{
	const char *r;
	return word(s, w, &r) && (*r == 0 || *r == ' ' || *r == '\t');
}

static u32 atou(const char *s)
{
	u32 v = 0;
	while (*s >= '0' && *s <= '9')
		v = v * 10 + (u32) (*s++ - '0');
	return v;
}

/* a * b / c with a 64-bit middle, 0 for c 0; Windows' own MulDiv, which
   takes and gives signed values, so every figure here is kept below 2^31 */
static u32 muldiv(u32 a, u32 b, u32 c)
{
	int r;
	if (!c)
		return 0;
	r = MulDiv((int) a, (int) b, (int) c);
	return r < 0 ? 0 : (u32) r;
}

/* Text: %s %c %d %u, %x (hex), %G (grouped: 14,336), %T and %H (tenths and
   hundredths: 123 is "12.3" and "1.23"), %%; a width and the flags - and 0 */
static char *utoa_(char *t, u32 v, int base, int group)
{
	char r[24];
	int n = 0, i, o = 0;
	do {
		u32 d = v % (u32) base;
		r[n++] = (char) (d < 10 ? '0' + d : 'a' + d - 10);
		v /= (u32) base;
	} while (v);
	for (i = n - 1; i >= 0; i--) {
		t[o++] = r[i];
		if (group && i && i % 3 == 0)
			t[o++] = ',';
	}
	t[o] = 0;
	return t;
}

static int vfmt(char *out, int size, const char *f, va_list ap)
{
	char *o = out, *end = out + size - 1;
	while (*f) {
		char tmp[40];
		const char *s;
		int n, w = 0, left = 0, zero = 0;
		if (*f != '%') {
			if (o < end)
				*o++ = *f;
			f++;
			continue;
		}
		f++;
		if (*f == '-') {
			left = 1;
			f++;
		}
		if (*f == '0') {
			zero = 1;
			f++;
		}
		while (*f >= '0' && *f <= '9')
			w = w * 10 + (*f++ - '0');
		s = tmp;
		switch (*f) {
		case 's':
			s = va_arg(ap, const char *);
			if (!s)
				s = "";
			break;
		case 'c':
			tmp[0] = (char) va_arg(ap, int);
			tmp[1] = 0;
			break;
		case 'd': {
			int v = va_arg(ap, int);
			if (v < 0) {
				tmp[0] = '-';
				utoa_(tmp + 1, 0u - (u32) v, 10, 0);
			} else
				utoa_(tmp, (u32) v, 10, 0);
			break;
		}
		case 'u':
			utoa_(tmp, va_arg(ap, u32), 10, 0);
			break;
		case 'x':
			utoa_(tmp, va_arg(ap, u32), 16, 0);
			break;
		case 'G':
			utoa_(tmp, va_arg(ap, u32), 10, 1);
			break;
		case 'T':
		case 'H': {
			u32 v = va_arg(ap, u32), div = *f == 'T' ? 10 : 100;
			char *p = utoa_(tmp, v / div, 10, 1);
			p += slen(p);
			*p++ = '.';
			if (div == 100)
				*p++ = (char) ('0' + (v % 100) / 10);
			*p++ = (char) ('0' + v % 10);
			*p = 0;
			break;
		}
		case '%':
			s = "%";
			break;
		default:
			s = "?";
			break;
		}
		if (*f)
			f++;
		n = slen(s);
		if (!left)
			for (; w > n && o < end; w--)
				*o++ = zero ? '0' : ' ';
		for (; *s && o < end; s++)
			*o++ = *s;
		if (left)
			for (; w > n && o < end; w--)
				*o++ = ' ';
	}
	*o = 0;
	return (int) (o - out);
}

static int fmt(char *out, int size, const char *f, ...)
{
	va_list ap;
	int n;
	va_start(ap, f);
	n = vfmt(out, size, f, ap);
	va_end(ap);
	return n;
}

/* ==== the clock: Windows' performance counter (the 8254 at 1.19 MHz on
   Windows 95), or GetTickCount if there is none ==== */
static u32 q_freq;              /* counts a second; 0: GetTickCount (ms) */

static void clock_init(void)
{
	LARGE_INTEGER f;
	if (QueryPerformanceFrequency(&f) && f.HighPart == 0 && f.LowPart >= 1000)
		q_freq = f.LowPart;
}

static u32 qnow(void)
{
	LARGE_INTEGER c;
	if (q_freq && QueryPerformanceCounter(&c))
		return c.LowPart;
	return GetTickCount();
}

/* microseconds in a difference of qnow(); good for half an hour at 1.19 MHz */
static u32 qus(u32 d)
{
	if (!q_freq)
		return d * 1000;
	return muldiv(d, 1000000, q_freq);
}

/* ==== tiny386's information port (perfport.h) ==== */
static int pp_on;               /* the port answered with the magic */
static u32 pp_count;            /* indexes this build knows */
static char pp_fw[33], pp_board[33];
static char pp_why[80] = "not looked for";

static __inline__ void out16(unsigned short port, unsigned short v)
{
	__asm__ __volatile__("outw %w0, %w1" : : "a"(v), "Nd"(port));
}

static __inline__ u32 in32(unsigned short port)
{
	u32 v;
	__asm__ __volatile__("inl %w1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static u32 pp_get(unsigned i)
{
	out16(PP_PORT_INDEX, (unsigned short) i);
	return in32(PP_PORT_DATA);
}

/* the indexes read each time, as ranges */
static const unsigned char pp_ranges[][2] = {
	{ PP_I_VALID0, PP_I_PORT_IO }, { PP_I_INSNS_LO, PP_I_SECONDS },
	{ PP_I_PICTURES, PP_I_CHANGED_1S }, { PP_I_MODE_KIND, PP_I_SCREEN_HEIGHT },
	{ PP_I_GUEST_MEM_KB, PP_I_IRAM_MIN_FREE_KB }, { PP_I_DISK_READS, PP_I_DISK_ACTIVITY },
	{ PP_I_TIMER_OWED_MAX, PP_I_TIMER_CATCHUP }, { PP_I_KEY_EVENTS, PP_I_MOUSE_PACKETS },
	{ PP_I_LOG_HELD, PP_I_CPU_MHZ },
};

static void pp_read(u32 *v)
{
	unsigned r, i;
	memset(v, 0, PP_COUNT * sizeof(u32));
	out16(PP_PORT_CMD, PP_CMD_SNAPSHOT);
	for (r = 0; r < sizeof pp_ranges / sizeof pp_ranges[0]; r++)
		for (i = pp_ranges[r][0]; i <= pp_ranges[r][1]; i++)
			if (i < pp_count)
				v[i] = pp_get(i);
}

static void pp_text(char *out, unsigned first)
{
	unsigned i;
	for (i = 0; i < PP_TEXT_INDEXES; i++) {
		u32 w = pp_get(first + i);
		out[i * 4] = (char) w;
		out[i * 4 + 1] = (char) (w >> 8);
		out[i * 4 + 2] = (char) (w >> 16);
		out[i * 4 + 3] = (char) (w >> 24);
	}
	out[32] = 0;
	for (i = 0; out[i]; i++)
		if ((unsigned char) out[i] < 32 || (unsigned char) out[i] > 126)
			out[i] = '?';
}

static void pp_detect(void)
{
	u32 v;
	if (!(GetVersion() & 0x80000000UL)) {
		/* Windows NT: a program's own in and out would stop it */
		scopy(pp_why, "not looked for: Windows NT does not let a program reach ports",
		      sizeof pp_why);
		return;
	}
	v = pp_get(PP_I_MAGIC);
	if (v != PP_MAGIC) {
		fmt(pp_why, sizeof pp_why, "not running on tiny386 (no information port: %x at F1FCh)", v);
		return;
	}
	v = pp_get(PP_I_VERSION);
	if ((v >> 16) != PP_VERSION_MAJOR) {
		fmt(pp_why, sizeof pp_why, "tiny386's information port is version %u.%u, "
		    "this monitor knows %u", v >> 16, v & 0xffff, PP_VERSION_MAJOR);
		return;
	}
	pp_count = pp_get(PP_I_COUNT);
	if (pp_count > PP_COUNT)
		pp_count = PP_COUNT;
	out16(PP_PORT_CMD, PP_CMD_SNAPSHOT);
	if (pp_count >= PP_I_BOARD + PP_TEXT_INDEXES) {
		pp_text(pp_fw, PP_I_FIRMWARE);
		pp_text(pp_board, PP_I_BOARD);
	}
	pp_on = 1;
	fmt(pp_why, sizeof pp_why, "information port %u.%u", v >> 16, v & 0xffff);
}

/* Settling before a test: 1 once the guest has been halted at least half
   of a whole second that began after the settling did, and no disk has
   been read or written for 2.5 s (the board flushes the card 2 s after the
   last write, on the emulator's core; Windows' own writes between the
   tests put such a flush inside a test on the PC harness).  start: note
   where the emulator's seconds and the disks are now; busy: the disks were
   in use during what came before (a_before: their count at its end).  A
   snapshot and three values a call. */
#define DISK_QUIET_MS 2500
static u32 pp_quiet_s0, pp_quiet_a, pp_quiet_t;
static int pp_quiet(int start, int busy, u32 a_before)
{
	u32 s, h, a, now = qnow();
	out16(PP_PORT_CMD, PP_CMD_SNAPSHOT);
	s = pp_get(PP_I_SECONDS);
	a = pp_get(PP_I_DISK_ACTIVITY);
	if (start) {
		pp_quiet_s0 = s;
		pp_quiet_a = a;
		/* disk activity just before: as if it had been now */
		pp_quiet_t = busy || a != a_before ? now :
			now - (q_freq ? muldiv(DISK_QUIET_MS + 1000, q_freq, 1000) : DISK_QUIET_MS + 1000);
		return 0;
	}
	if (a != pp_quiet_a) {
		pp_quiet_a = a;
		pp_quiet_t = now;
	}
	h = pp_get(PP_I_HALT_PCT);
	return s - pp_quiet_s0 >= 2 && h >= 50 && qus(now - pp_quiet_t) / 1000 >= DISK_QUIET_MS;
}

/* ==== Windows 95's performance data (HKEY_DYN_DATA\PerfStats) ==== */
enum {
	S_CPU, S_THREADS, S_VMS,
	S_PF, S_PIN, S_POUT, S_DISC,            /* counts: a rate from two reads */
	S_FREE, S_LOCKED, S_COMMIT, S_SWAPF, S_SWAPUSE, S_CACHE, S_CACHEMIN, S_CACHEMAX,
	S_FSR, S_FSW, S_FSBR, S_FSBW,           /* counts */
	S_DIRTY,
	S_N
};
static const char *const stat_name[S_N] = {
	"KERNEL\\CPUUsage", "KERNEL\\Threads", "KERNEL\\VMs",
	"VMM\\cPageFaults", "VMM\\cPageIns", "VMM\\cPageOuts", "VMM\\cDiscards",
	"VMM\\cpgFree", "VMM\\cpgLocked", "VMM\\cpgCommit", "VMM\\cpgSwapfile",
	"VMM\\cpgSwapfileInUse", "VMM\\cpgDiskcache", "VMM\\cpgDiskcacheMin",
	"VMM\\cpgDiskcacheMac",
	"VFAT\\ReadsSec", "VFAT\\WritesSec", "VFAT\\BReadsSec", "VFAT\\BWritesSec",
	"VFAT\\DirtyData",
};
/* counts (System Monitor's "Differentiate"): a rate is their difference */
static const char stat_count[S_N] = { 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0 };
/* when each is read: 1 every second (the processor, when it is on), 2 once
   at the start (the disk cache's limits), 0 every 5 seconds; each read
   costs about a millisecond of the guest's time on the PC harness */
static const char stat_fast[S_N] = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 2, 0, 0, 0, 0, 0 };
static HKEY stat_key;
static int stat_on, stat_ok[S_N], wcpu_on;
static u32 st_val[S_N], st_rate[S_N], st_t[S_N];
static char st_seen[S_N];

static void stat_query(const char *key, int from, int to)
{
	HKEY k;
	int i;
	if (RegOpenKeyExA(HKEY_DYN_DATA, key, 0, KEY_READ, &k) != ERROR_SUCCESS)
		return;
	for (i = from; i <= to; i++) {
		BYTE b[16];
		DWORD t, n = sizeof b;
		RegQueryValueExA(k, stat_name[i], NULL, &t, b, &n);
	}
	RegCloseKey(k);
}

static void stats_start(void)
{
	int i;
	stat_query("PerfStats\\StartStat", S_CPU + 1, S_N - 1);   /* not the processor */
	if (RegOpenKeyExA(HKEY_DYN_DATA, "PerfStats\\StatData", 0, KEY_READ, &stat_key) != ERROR_SUCCESS)
		return;
	for (i = 0; i < S_N; i++) {
		BYTE b[16];
		DWORD t, n = sizeof b;
		stat_ok[i] = RegQueryValueExA(stat_key, stat_name[i], NULL, &t, b, &n) == ERROR_SUCCESS
			&& n >= 4;
		stat_on |= stat_ok[i];
	}
}

/* all: 0 the processor only (when it is on), 1 every 5 seconds' set,
   2 everything */
static void stats_read(int all)
{
	int i;
	u32 now = qnow();
	if (!stat_on)
		return;
	for (i = 0; i < S_N; i++) {
		DWORD t, n = 4, x = 0;
		if (!stat_ok[i] || (i == S_CPU && !wcpu_on))
			continue;
		if (stat_fast[i] == 0 && all < 1)
			continue;
		if (stat_fast[i] == 2 && all < 2)
			continue;
		if (RegQueryValueExA(stat_key, stat_name[i], NULL, &t, (BYTE *) &x, &n) != ERROR_SUCCESS)
			continue;
		if (stat_count[i]) {
			if (st_seen[i])
				st_rate[i] = muldiv(x - st_val[i], 1000000, qus(now - st_t[i]));
			st_t[i] = now;
			st_seen[i] = 1;
		}
		st_val[i] = x;
	}
}

/* Windows' own processor figure on or off.  While it is on, Windows 95
   spins in its idle loop instead of halting (seen on the PC harness: the
   guest halted 0% of the time with it on, 60 to 90% with it off) */
static HWND g_wcpu_btn;
static void wcpu_set(int on)
{
	if (on && !(stat_on && stat_ok[S_CPU]))
		on = 0;
	if (on != wcpu_on && stat_on && stat_ok[S_CPU]) {
		stat_query(on ? "PerfStats\\StartStat" : "PerfStats\\StopStat", S_CPU, S_CPU);
		wcpu_on = on;
		if (on)
			stats_read(0);          /* its figure is for the time since the last read */
	}
	st_val[S_CPU] = 0;
	if (g_wcpu_btn)
		SendMessageA(g_wcpu_btn, BM_SETCHECK, wcpu_on ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void stats_stop(void)
{
	if (stat_key)
		RegCloseKey(stat_key);
	if (stat_on)
		stat_query("PerfStats\\StopStat", wcpu_on ? S_CPU : S_CPU + 1, S_N - 1);
}

/* ==== a sample ==== */
typedef struct {
	u32 q;                  /* qnow() */
	u32 tick;               /* GetTickCount() */
	u32 st[S_N];            /* Windows' figures, as last read */
	MEMORYSTATUS ms;
	int pp;                 /* v[] holds a snapshot */
	u32 v[PP_COUNT];
} Sample;

/* end: 0 takes the clock last (a measurement starts after the reads),
   1 first (it ends before them); all: every Windows figure */
static void take(Sample *s, int end, int all)
{
	if (end)
		s->q = qnow();
	s->tick = GetTickCount();
	stats_read(all);
	memcpy(s->st, st_val, sizeof s->st);
	s->ms.dwLength = sizeof s->ms;
	GlobalMemoryStatus(&s->ms);
	s->pp = pp_on;
	if (pp_on)
		pp_read(s->v);
	if (!end)
		s->q = qnow();
}

#define VALID(s, i) ((s)->pp && (((s)->v[PP_I_VALID0 + ((i) >> 5)] >> ((i) & 31)) & 1))
#define DPP(a, b, i) ((b)->v[i] - (a)->v[i])

/* a count a second from two samples, by the emulator's clock */
static u32 pp_rate(const Sample *a, const Sample *b, int i, u32 scale)
{
	return muldiv(DPP(a, b, i), 1000000 * scale, DPP(a, b, PP_I_CLOCK_US));
}

/* the display mode */
static int scr_w, scr_h, scr_bpp;

static void mode_read(void)
{
	HDC dc = GetDC(NULL);
	scr_w = GetDeviceCaps(dc, HORZRES);
	scr_h = GetDeviceCaps(dc, VERTRES);
	scr_bpp = GetDeviceCaps(dc, BITSPIXEL) * GetDeviceCaps(dc, PLANES);
	ReleaseDC(NULL, dc);
}

static const char *depth_name(int bpp)
{
	switch (bpp) {
	case 1: return "2 colours";
	case 4: return "16 colours";
	case 8: return "256 colours";
	case 15: return "High Colour (15 bit)";
	case 16: return "High Colour (16 bit)";
	case 24: return "True Colour (24 bit)";
	case 32: return "True Colour (32 bit)";
	}
	return "an unusual depth";
}

/* ==== settings ==== */
static HINSTANCE g_inst;
static HWND g_wnd;
static char log_path[MAX_PATH] = "C:\\T386MON.LOG";
static u32 opt_every = 5;       /* seconds between log lines; 0: none */
static u32 opt_write = 60;      /* seconds between writes of the log; 0: at once */
static u32 opt_secs = 4;        /* seconds a test */
static int opt_bench, opt_quit, opt_compact, opt_top;
static int opt_wcpu = -1;       /* Windows' own processor figure: -1 on unless on tiny386 */
static int opt_debug, opt_nostats, opt_noport;
static u32 dbg_chars, dbg_calls;        /* text drawn: characters, calls */

/* ==== the log ==== */
static char log_err[MAX_PATH + 40];
static char log_last[12] = "none yet";

static void now_text(char *out, int size, int with_date)
{
	SYSTEMTIME t;
	GetLocalTime(&t);
	if (with_date)
		fmt(out, size, "%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay,
		    t.wHour, t.wMinute, t.wSecond);
	else
		fmt(out, size, "%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond);
}

/* The lines wait in memory and go to the file every opt_write seconds,
   after the tests, at Log now and at the end: on the board each write to
   the disk image is followed, 2 s later, by a flush to the card on the
   emulator's own core, which holds the guest up meanwhile. */
#define LOGBUF 16384
static char g_lbuf[LOGBUF];
static int g_llen;
static u32 g_lsince;                    /* GetTickCount() of the oldest line waiting */
static char log_written[12] = "not yet";
static int log_hold;                    /* the tests run: nothing is written */

static void log_flush(void)
{
	DWORD n;
	HANDLE h;
	if (!g_llen)
		return;
	h = CreateFileA(log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		fmt(log_err, sizeof log_err, "cannot write %s (error %u)", log_path, GetLastError());
		g_llen = 0;
		return;
	}
	SetFilePointer(h, 0, NULL, FILE_END);
	if (!WriteFile(h, g_lbuf, (DWORD) g_llen, &n, NULL) || n != (DWORD) g_llen)
		fmt(log_err, sizeof log_err, "cannot write %s (error %u)", log_path, GetLastError());
	else
		log_err[0] = 0;
	CloseHandle(h);
	g_llen = 0;
	now_text(log_written, sizeof log_written, 0);
}

static void log_put(const char *text)
{
	int n = slen(text);
	if (g_llen + n > LOGBUF)
		log_flush();
	if (n > LOGBUF)
		n = LOGBUF;
	if (!g_llen)
		g_lsince = GetTickCount();
	memcpy(g_lbuf + g_llen, text, (size_t) n);
	g_llen += n;
	now_text(log_last, sizeof log_last, 0);
	if (!opt_write && !log_hold)
		log_flush();
}

/* once a second: the lines written if they have waited long enough */
static void log_write_due(void)
{
	if (g_llen && !log_hold && GetTickCount() - g_lsince >= opt_write * 1000)
		log_flush();
}

/* ==== the live view: lines of fields ====
   A line's template is text with fields: {N} a number right-aligned in N
   digits' width, {-N} text left-aligned in N digits' width, {*} text to the
   end of the line.  Each second only the fields whose text changed are
   drawn again, each in its own box: drawing text is what costs (above). */
#define MAXF 8
#define FV 112
typedef struct {
	short off, len, x;
} Run;
typedef struct {
	const char *tmpl;
	int nf, nr, laid, y, h, x0, width, cellw;
	HFONT *font;
	char fv[MAXF][FV], fs[MAXF][FV];
	short fx[MAXF], fw[MAXF];
	char fl[MAXF];
	Run run[MAXF + 1];
} Line;

enum {
	L_HEAD,
	L_SWIN, L_WCPU, L_WMEM, L_WSWAP, L_WPAGE, L_WFS, L_WDISP,
	L_SEMU, L_EID, L_ECPU, L_EPIC, L_EDRAW, L_EVBE, L_EMODE, L_EMEM, L_EBMEM, L_EDISK,
	L_EDERR, L_ETIME, L_EINPUT, L_EBOARD,
	L_SMON, L_MCOST, L_MLOG,
	L_N
};
static const struct {
	char kind;              /* H headline, S section, L a line */
	const char *label;
} LD[L_N] = {
	{ 'H', "" },
	{ 'S', "Windows" },
	{ 'L', "Processor use" },
	{ 'L', "Memory" },
	{ 'L', "Swap file, cache" },
	{ 'L', "Paging" },
	{ 'L', "File system" },
	{ 'L', "Display" },
	{ 'S', "tiny386, from its information port" },
	{ 'L', "Emulator" },
	{ 'L', "Guest processor" },
	{ 'L', "Panel pictures" },
	{ 'L', "Board's drawing" },
	{ 'L', "VBE drawing" },
	{ 'L', "Mode" },
	{ 'L', "PC memory" },
	{ 'L', "Board memory" },
	{ 'L', "Disk" },
	{ 'L', "Disk trouble" },
	{ 'L', "Timer" },
	{ 'L', "Keyboard, mouse" },
	{ 'L', "Board" },
	{ 'S', "This monitor" },
	{ 'L', "Its own cost" },
	{ 'L', "Log" },
};
static Line LN[L_N], CL[2];             /* the live view; the compact view */

/* the templates */
static const char T_STR[] = "{*}";
static const char T_HEAD_W[] = "{3} fps     guest {3}% busy     {5} M instr/s     Windows {3}%";
static const char T_HEAD_R[] = "{3} fps     guest {3}% busy     {5} M instr/s     {6} rows/s";
static const char T_HEAD_NOPP[] = "Windows {3}% busy     {8} KB free";
static const char T_HEAD_NOPP2[] = "{8} KB free";
static const char T_WCPU_ON[] = "{3}% busy (Windows' own figure); {4} threads, {3} VMs";
static const char T_WCPU_OFF[] = "not measured (F7: Windows then never halts); {4} threads, {3} VMs";
static const char T_WMEM[] = "{6} KB in all, {6} KB free, {3}% in use; {6} KB allocated";
static const char T_WSWAP[] = "swap file {6} KB, {6} KB in use; disk cache {6} KB, from {6} to {6}";
static const char T_WPAGE[] = "{4} page faults/s, {3} page-ins/s, {3} page-outs/s, {3} discards/s; {6} KB locked";
static const char T_WFS[] = "{4} reads/s, {4} writes/s, {5} KB/s read, {5} KB/s written; {5} KB unwritten";
static const char T_ECPU[] = "{4} M instr/s running, {4} overall; halted {3}% of the last second";
static const char T_EPIC_D[] = "{3} changed/s of {3} drawn; {5} ms each, {5} at most, {5} ever";
static const char T_EPIC[] = "{3} changed/s of {3} drawn";
static const char T_EDRAW[] = "{3}% of its core's time; {6} rows/s to the panel in {4} bands/s";
static const char T_EVBE[] = "{6} rows/s converted, {6} blocks/s, {3} whole pictures/s; blocks {-3}";
static const char T_EMEM[] = "{6} KB for the PC, {5} KB of video memory";
static const char T_EBMEM[] = "PSRAM {6} KB free, block {6}; internal {4} KB free, block {4}, least {4}";
static const char T_EDISK[] = "{4} reads/s, {4} writes/s, {5} KB/s; wait {5} ms average, {6} at most; {4} flushes";
static const char T_EDERR[] = "{4} waits over a tick, {4} refused at first, {4} recovered, {4} told to the PC";
static const char T_ETIME[] = "{6} ticks dropped, {4} in the last second; at most {4} owed at once";
static const char T_EINPUT[] = "{4} keys/s, {4} mouse reports/s, {4} mouse packets/s";
static const char T_EBOARD[] = "{4} MHz, up {5} min; status {-8}; log {6} lines held back, {6} dropped";
static const char T_MCOST[] = "{5} ms a second, {5}% of the time; {5} port ins and outs/s; snapshot {4} us";
static const char T_MCOST_NOPP[] = "{5} ms a second, {5}% of the time";
static const char T_MLOG[] = "{-20} a line every {3} s, to the disk every {3} s; last {-8}";
static const char T_MLOG0[] = "{-20} a line every {3} s, to the disk at once; last {-8}";
static const char T_C0[] = "{3} fps   guest {3}% busy";
static const char T_C1[] = "{5} M instr/s   {6} rows/s";
static const char T_C0_NOPP[] = "Windows {3}% busy";
static const char T_C1_NOPP[] = "{8} KB free";

static void line_set(Line *l, const char *tmpl)
{
	if (l->tmpl != tmpl) {
		l->tmpl = tmpl;
		l->laid = 0;
	}
}

static void fset(Line *l, int f, const char *form, ...)
{
	va_list ap;
	if (f >= MAXF)
		return;
	va_start(ap, form);
	vfmt(l->fv[f], FV, form, ap);
	va_end(ap);
}

/* one number into a field: plain, grouped (14,336), tenths (12.3) and
   hundredths (1.23); quicker than fset(), which matters once a second */
static void fu(Line *l, int f, u32 v)
{
	utoa_(l->fv[f], v, 10, 0);
}

static void fg(Line *l, int f, u32 v)
{
	utoa_(l->fv[f], v, 10, 1);
}

static void fdec(Line *l, int f, u32 v, u32 div)
{
	char *p = utoa_(l->fv[f], v / div, 10, 1);
	p += slen(p);
	*p++ = '.';
	if (div == 100)
		*p++ = (char) ('0' + (v % 100) / 10);
	*p++ = (char) ('0' + v % 10);
	*p = 0;
}

static void ft(Line *l, int f, u32 v)
{
	fdec(l, f, v, 10);
}

static void fh(Line *l, int f, u32 v)
{
	fdec(l, f, v, 100);
}

/* a line as text: its template with the fields' values in */
static int line_render(const Line *l, char *out, int size)
{
	const char *p = l->tmpl ? l->tmpl : "";
	int o = 0, f = 0;
	while (*p && o < size - 1) {
		if (*p == '{') {
			const char *s = f < MAXF ? l->fv[f] : "";
			while (*p && *p != '}')
				p++;
			if (*p)
				p++;
			while (*s && o < size - 1)
				out[o++] = *s++;
			f++;
		} else
			out[o++] = *p++;
	}
	out[o] = 0;
	return o;
}

static u32 g_ticks, g_cost_us[10];
static int g_cost_i;
static Sample g_s[2];           /* the last two samples, g_s[g_cur] the newer */
static int g_cur;
static Sample g_lbase;          /* the log's interval */
static Sample g_dbase;          /* the live view's interval, with /refresh:N */
static u32 opt_refresh = 1;     /* the live view drawn every this many seconds */
static u32 g_lcpu_sum, g_lcpu_n, g_lcost_us;

/* the panel's frames a second: the pictures of the last whole second that
   changed (port 1.1 on), into field f; "-" from an older port.  The board
   draws a picture every 30 ms whether or not anything changed, so the
   pictures drawn a second are no frame rate. */
static void f_fps(Line *l, int f, const Sample *b)
{
	u32 c = b->v[PP_I_CHANGED_1S];
	if (!VALID(b, PP_I_CHANGED_1S)) {
		scopy(l->fv[f], "-", FV);
		return;
	}
	if (c > b->v[PP_I_PICTURES_1S])
		c = b->v[PP_I_PICTURES_1S];     /* the two may be a picture apart */
	fu(l, f, c);
}

static void build_live(const Sample *a, const Sample *b)
{
	u32 x, y;
	Line *l;

	/* the headline, and the compact view */
	if (b->pp) {
		line_set(&LN[L_HEAD], wcpu_on ? T_HEAD_W : T_HEAD_R);
		f_fps(&LN[L_HEAD], 0, b);
		fu(&LN[L_HEAD], 1, 100 - b->v[PP_I_HALT_PCT]);
		fh(&LN[L_HEAD], 2, b->v[PP_I_KIPS] / 10);
		if (wcpu_on)
			fu(&LN[L_HEAD], 3, st_val[S_CPU]);
		else
			fg(&LN[L_HEAD], 3, pp_rate(a, b, PP_I_FLUSH_ROWS, 1));
		line_set(&CL[0], T_C0);
		line_set(&CL[1], T_C1);
		memcpy(CL[0].fv[0], LN[L_HEAD].fv[0], FV);
		memcpy(CL[0].fv[1], LN[L_HEAD].fv[1], FV);
		memcpy(CL[1].fv[0], LN[L_HEAD].fv[2], FV);
		fg(&CL[1], 1, pp_rate(a, b, PP_I_FLUSH_ROWS, 1));
	} else if (wcpu_on) {
		line_set(&LN[L_HEAD], T_HEAD_NOPP);
		fu(&LN[L_HEAD], 0, st_val[S_CPU]);
		fg(&LN[L_HEAD], 1, b->ms.dwAvailPhys / 1024);
		line_set(&CL[0], T_C0_NOPP);
		line_set(&CL[1], T_C1_NOPP);
		memcpy(CL[0].fv[0], LN[L_HEAD].fv[0], FV);
		memcpy(CL[1].fv[0], LN[L_HEAD].fv[1], FV);
	} else {
		line_set(&LN[L_HEAD], T_HEAD_NOPP2);
		fg(&LN[L_HEAD], 0, b->ms.dwAvailPhys / 1024);
		line_set(&CL[0], T_STR);
		line_set(&CL[1], T_C1_NOPP);
		scopy(CL[0].fv[0], "not on tiny386", FV);
		memcpy(CL[1].fv[0], LN[L_HEAD].fv[0], FV);
	}

	/* Windows (the VMM's memory figures are in bytes) */
	l = &LN[L_WCPU];
	if (stat_on) {
		line_set(l, wcpu_on ? T_WCPU_ON : T_WCPU_OFF);
		if (wcpu_on)
			fu(l, 0, st_val[S_CPU]);
		fu(l, wcpu_on ? 1 : 0, st_val[S_THREADS]);
		fu(l, wcpu_on ? 2 : 1, st_val[S_VMS]);
		l = &LN[L_WSWAP];
		line_set(l, T_WSWAP);
		fg(l, 0, st_val[S_SWAPF] / 1024);
		fg(l, 1, st_val[S_SWAPUSE] / 1024);
		fg(l, 2, st_val[S_CACHE] / 1024);
		fg(l, 3, st_val[S_CACHEMIN] / 1024);
		fg(l, 4, st_val[S_CACHEMAX] / 1024);
		l = &LN[L_WPAGE];
		line_set(l, T_WPAGE);
		fu(l, 0, st_rate[S_PF]);
		fu(l, 1, st_rate[S_PIN]);
		fu(l, 2, st_rate[S_POUT]);
		fu(l, 3, st_rate[S_DISC]);
		fg(l, 4, st_val[S_LOCKED] / 1024);
		l = &LN[L_WFS];
		line_set(l, T_WFS);
		fu(l, 0, st_rate[S_FSR]);
		fu(l, 1, st_rate[S_FSW]);
		fg(l, 2, st_rate[S_FSBR] / 1024);
		fg(l, 3, st_rate[S_FSBW] / 1024);
		fg(l, 4, st_val[S_DIRTY] / 1024);
	} else {
		line_set(l, T_STR);
		fset(l, 0, "not known: Windows gives no performance data here");
		line_set(&LN[L_WSWAP], T_STR);
		line_set(&LN[L_WPAGE], T_STR);
		line_set(&LN[L_WFS], T_STR);
		fset(&LN[L_WSWAP], 0, "-");
		fset(&LN[L_WPAGE], 0, "-");
		fset(&LN[L_WFS], 0, "-");
	}
	l = &LN[L_WMEM];
	line_set(l, T_WMEM);
	fg(l, 0, b->ms.dwTotalPhys / 1024);
	fg(l, 1, b->ms.dwAvailPhys / 1024);
	fu(l, 2, b->ms.dwMemoryLoad);
	fg(l, 3, stat_ok[S_COMMIT] ? st_val[S_COMMIT] / 1024 :
	     (b->ms.dwTotalPageFile - b->ms.dwAvailPageFile) / 1024);
	line_set(&LN[L_WDISP], T_STR);
	fset(&LN[L_WDISP], 0, "%u x %u in %s", scr_w, scr_h, depth_name(scr_bpp));

	/* the emulator */
	if (!b->pp) {
		for (x = L_EID; x <= L_EBOARD; x++) {
			line_set(&LN[x], T_STR);
			LN[x].fv[0][0] = 0;
		}
		fset(&LN[L_EID], 0, "%s", pp_why);
	} else {
		u32 clk = DPP(a, b, PP_I_CLOCK_US);
		line_set(&LN[L_EID], T_STR);
		line_set(&LN[L_EMODE], T_STR);
		fset(&LN[L_EID], 0, "%s on %s (%s)", pp_fw, pp_board,
		     b->v[PP_I_SOURCE] == PP_SOURCE_BOARD ? "the board" :
		     b->v[PP_I_SOURCE] == PP_SOURCE_HARNESS ? "the PC harness, virtual time" : "unknown");
		l = &LN[L_ECPU];
		line_set(l, T_ECPU);
		fh(l, 0, b->v[PP_I_KIPS] / 10);
		fh(l, 1, muldiv(DPP(a, b, PP_I_INSNS_LO), 100, clk));
		fu(l, 2, b->v[PP_I_HALT_PCT]);
		l = &LN[L_EPIC];
		if (VALID(b, PP_I_DRAW_US)) {
			line_set(l, T_EPIC_D);
			ft(l, 2, b->v[PP_I_DRAW_AVG_US_1S] / 100);
			ft(l, 3, b->v[PP_I_DRAW_MAX_US_1S] / 100);
			ft(l, 4, b->v[PP_I_DRAW_MAX_US] / 100);
		} else
			line_set(l, T_EPIC);
		f_fps(l, 0, b);
		fu(l, 1, b->v[PP_I_PICTURES_1S]);
		l = &LN[L_EDRAW];
		line_set(l, T_EDRAW);
		fu(l, 0, muldiv(DPP(a, b, PP_I_DRAW_US), 100, clk));
		fg(l, 1, pp_rate(a, b, PP_I_FLUSH_ROWS, 1));
		fu(l, 2, pp_rate(a, b, PP_I_FLUSHES, 1));
		l = &LN[L_EVBE];
		line_set(l, T_EVBE);
		fg(l, 0, pp_rate(a, b, PP_I_VBE_ROWS, 1));
		fg(l, 1, pp_rate(a, b, PP_I_VBE_BLOCKS, 1));
		fu(l, 2, pp_rate(a, b, PP_I_VBE_WHOLE, 1));
		fset(l, 3, "%s", b->v[PP_I_VBE_BLOCKS_ON] ? "on" : "off");
		x = b->v[PP_I_MODE_WIDTH];
		y = b->v[PP_I_MODE_HEIGHT];
		l = &LN[L_EMODE];
		switch (b->v[PP_I_MODE_KIND]) {
		case PP_MODE_TEXT:
			fset(l, 0, "text, %u x %u", x, y);
			break;
		case PP_MODE_VGA:
			fset(l, 0, "VGA %u x %u, %u bits (BIOS mode %x); the screen %u x %u", x, y,
			     b->v[PP_I_MODE_BPP], b->v[PP_I_BIOS_MODE], b->v[PP_I_SCREEN_WIDTH],
			     b->v[PP_I_SCREEN_HEIGHT]);
			break;
		case PP_MODE_VBE:
			fset(l, 0, "VBE %u x %u, %u bits, %G bytes a line; the screen %u x %u%s", x, y,
			     b->v[PP_I_MODE_BPP], b->v[PP_I_MODE_LINE_BYTES], b->v[PP_I_SCREEN_WIDTH],
			     b->v[PP_I_SCREEN_HEIGHT], x > b->v[PP_I_SCREEN_WIDTH] ||
			     y > b->v[PP_I_SCREEN_HEIGHT] ? ", shown shrunk" : "");
			break;
		default:
			fset(l, 0, "blank");
			break;
		}
		l = &LN[L_EMEM];
		line_set(l, T_EMEM);
		fg(l, 0, b->v[PP_I_GUEST_MEM_KB]);
		fg(l, 1, b->v[PP_I_VGA_MEM_KB]);
		l = &LN[L_EBMEM];
		if (VALID(b, PP_I_PSRAM_FREE_KB)) {
			line_set(l, T_EBMEM);
			fg(l, 0, b->v[PP_I_PSRAM_FREE_KB]);
			fg(l, 1, b->v[PP_I_PSRAM_LARGEST_KB]);
			fg(l, 2, b->v[PP_I_IRAM_FREE_KB]);
			fg(l, 3, b->v[PP_I_IRAM_LARGEST_KB]);
			fg(l, 4, b->v[PP_I_IRAM_MIN_FREE_KB]);
		} else {
			line_set(l, T_STR);
			fset(l, 0, "not known here");
		}
		x = DPP(a, b, PP_I_DISK_READS) + DPP(a, b, PP_I_DISK_WRITES);
		l = &LN[L_EDISK];
		line_set(l, T_EDISK);
		fu(l, 0, pp_rate(a, b, PP_I_DISK_READS, 1));
		fu(l, 1, pp_rate(a, b, PP_I_DISK_WRITES, 1));
		fg(l, 2, muldiv((DPP(a, b, PP_I_DISK_READ_SECTORS) +
					 DPP(a, b, PP_I_DISK_WRITE_SECTORS)) / 2, 1000000, clk));
		ft(l, 3, x ? DPP(a, b, PP_I_DISK_BUSY_US) / x / 100 : 0);
		ft(l, 4, b->v[PP_I_DISK_LONGEST_US] / 100);
		fg(l, 5, b->v[PP_I_DISK_FLUSHES]);
		l = &LN[L_EDERR];
		line_set(l, T_EDERR);
		fu(l, 0, b->v[PP_I_DISK_OVER_TICK]);
		fu(l, 1, b->v[PP_I_DISK_ERRORS]);
		fu(l, 2, b->v[PP_I_DISK_RECOVERED]);
		fu(l, 3, b->v[PP_I_DISK_REFUSED]);
		l = &LN[L_ETIME];
		line_set(l, T_ETIME);
		fg(l, 0, b->v[PP_I_TIMER_DROPPED]);
		fu(l, 1, DPP(a, b, PP_I_TIMER_DROPPED));
		fu(l, 2, b->v[PP_I_TIMER_OWED_MAX]);
		l = &LN[L_EINPUT];
		line_set(l, T_EINPUT);
		fu(l, 0, pp_rate(a, b, PP_I_KEY_EVENTS, 1));
		fu(l, 1, pp_rate(a, b, PP_I_MOUSE_REPORTS, 1));
		fu(l, 2, pp_rate(a, b, PP_I_MOUSE_PACKETS, 1));
		l = &LN[L_EBOARD];
		if (VALID(b, PP_I_CPU_MHZ)) {
			line_set(l, T_EBOARD);
			fu(l, 0, b->v[PP_I_CPU_MHZ]);
			fg(l, 1, b->v[PP_I_UPTIME_MS] / 60000);
			fset(l, 2, "%x", b->v[PP_I_STATUS]);
			fg(l, 3, b->v[PP_I_LOG_HELD]);
			fg(l, 4, b->v[PP_I_LOG_DROPPED]);
		} else {
			line_set(l, T_STR);
			fset(l, 0, "not known here");
		}
	}

	/* this monitor */
	for (x = 0, y = 0; y < 10; y++)
		x += g_cost_us[y];
	y = g_ticks < 10 ? g_ticks : 10;
	x = y ? x / y : 0;                      /* us a second */
	l = &LN[L_MCOST];
	line_set(l, b->pp ? T_MCOST : T_MCOST_NOPP);
	ft(l, 0, x / 100);
	fh(l, 1, x / 100);
	if (b->pp) {
		fu(l, 2, pp_rate(a, b, PP_I_PORT_IO, 1));
		fu(l, 3, b->v[PP_I_SNAP_US]);
	}
	l = &LN[L_MLOG];
	if (log_err[0]) {
		line_set(l, T_STR);
		fset(l, 0, "%s", log_err);
	} else if (opt_every) {
		line_set(l, opt_write ? T_MLOG : T_MLOG0);
		fset(l, 0, "%s", log_path);
		fu(l, 1, opt_every);
		if (opt_write) {
			fu(l, 2, opt_write);
			fset(l, 3, "%s", log_written);
		} else
			fset(l, 2, "%s", log_written);
	} else {
		line_set(l, T_STR);
		fset(l, 0, "%s: only the tests and Log now (/every:0)", log_path);
	}
}

/* the log's line every opt_every seconds: averages over the interval */
static const char log_cols[] =
	"time     up_s wcpu gbusy mips mips_all  fps refr/s draw_ms max_ms bload rows/s vbe_rows/s "
	"whole/s free_kb swap_kb pf/s fs_rd/s fs_wr/s fs_rkb/s fs_wkb/s dk_rd/s dk_wr/s "
	"dk_kb/s dk_wait_ms flush tdrop keys/s mouse/s cost_ms\r\n";

static void log_line(const Sample *a, const Sample *b)
{
	char line[400], t[12], cpu[8];
	int n;
	u32 us = qus(b->q - a->q), x;
	now_text(t, sizeof t, 0);
	if (wcpu_on && g_lcpu_n)
		fmt(cpu, sizeof cpu, "%u", g_lcpu_sum / g_lcpu_n);
	else
		scopy(cpu, "-", sizeof cpu);
	n = fmt(line, sizeof line, "%s %5u %4s", t, b->tick / 1000, cpu);
	if (b->pp && a->pp) {
		u32 clk = DPP(a, b, PP_I_CLOCK_US), run = DPP(a, b, PP_I_RUN_MS),
		    halt = DPP(a, b, PP_I_HALT_MS), pics = DPP(a, b, PP_I_PICTURES);
		char fps[12];
		if (VALID(b, PP_I_CHANGED))
			fmt(fps, sizeof fps, "%T", pp_rate(a, b, PP_I_CHANGED, 10));
		else
			scopy(fps, "-", sizeof fps);
		n += fmt(line + n, sizeof line - n,
			 " %5u %4H %8H %4s %6T %7T %6T %5u %6u %10u %7u",
			 run + halt ? muldiv(run, 100, run + halt) : 0,
			 run ? muldiv(DPP(a, b, PP_I_INSNS_LO), 1, run * 10) : 0,
			 muldiv(DPP(a, b, PP_I_INSNS_LO), 100, clk), fps,
			 pp_rate(a, b, PP_I_PICTURES, 10),
			 pics ? DPP(a, b, PP_I_DRAW_US) / pics / 100 : 0,
			 b->v[PP_I_DRAW_MAX_US_1S] / 100,
			 muldiv(DPP(a, b, PP_I_DRAW_US), 100, clk),
			 pp_rate(a, b, PP_I_FLUSH_ROWS, 1), pp_rate(a, b, PP_I_VBE_ROWS, 1),
			 pp_rate(a, b, PP_I_VBE_WHOLE, 1));
	} else
		n += fmt(line + n, sizeof line - n, " %5s %4s %8s %4s %6s %7s %6s %5s %6s %10s %7s",
			 "-", "-", "-", "-", "-", "-", "-", "-", "-", "-", "-");
	n += fmt(line + n, sizeof line - n, " %7u %7u %4u %7u %7u %8u %8u",
		 b->ms.dwAvailPhys / 1024, b->st[S_SWAPUSE] / 1024,
		 muldiv(b->st[S_PF] - a->st[S_PF], 1000000, us),
		 muldiv(b->st[S_FSR] - a->st[S_FSR], 1000000, us),
		 muldiv(b->st[S_FSW] - a->st[S_FSW], 1000000, us),
		 muldiv((b->st[S_FSBR] - a->st[S_FSBR]) / 1024, 1000000, us),
		 muldiv((b->st[S_FSBW] - a->st[S_FSBW]) / 1024, 1000000, us));
	if (b->pp && a->pp) {
		u32 clk = DPP(a, b, PP_I_CLOCK_US);
		x = DPP(a, b, PP_I_DISK_READS) + DPP(a, b, PP_I_DISK_WRITES);
		n += fmt(line + n, sizeof line - n, " %7u %7u %7u %10T %5u %5u %6u %7u",
			 pp_rate(a, b, PP_I_DISK_READS, 1), pp_rate(a, b, PP_I_DISK_WRITES, 1),
			 muldiv((DPP(a, b, PP_I_DISK_READ_SECTORS) + DPP(a, b, PP_I_DISK_WRITE_SECTORS)) / 2,
				1000000, clk),
			 x ? DPP(a, b, PP_I_DISK_BUSY_US) / x / 100 : 0, DPP(a, b, PP_I_DISK_FLUSHES),
			 DPP(a, b, PP_I_TIMER_DROPPED), pp_rate(a, b, PP_I_KEY_EVENTS, 1),
			 pp_rate(a, b, PP_I_MOUSE_REPORTS, 1));
	} else
		n += fmt(line + n, sizeof line - n, " %7s %7s %7s %10s %5s %5s %6s %7s",
			 "-", "-", "-", "-", "-", "-", "-", "-");
	fmt(line + n, sizeof line - n, " %7T\r\n", muldiv(g_lcost_us, 10, us ? us / 1000 : 1));
	log_put(line);
}

static void log_start(void)
{
	char line[1000], when[24];
	OSVERSIONINFOA ov;
	int n;
	now_text(when, sizeof when, 1);
	ov.dwOSVersionInfoSize = sizeof ov;
	if (!GetVersionExA(&ov))
		memset(&ov, 0, sizeof ov);
	n = fmt(line, sizeof line, "\r\n# %s %s started %s; Windows %u.%u build %u%s; up %u s\r\n",
		APP_NAME, APP_VERSION, when, ov.dwMajorVersion, ov.dwMinorVersion,
		ov.dwBuildNumber & 0xffff, ov.szCSDVersion, GetTickCount() / 1000);
	n += fmt(line + n, sizeof line - n, "# screen %u x %u in %s; memory %G KB; %s%s%s%s%s\r\n",
		 scr_w, scr_h, depth_name(scr_bpp), g_s[g_cur].ms.dwTotalPhys / 1024, pp_why,
		 pp_on ? ": " : "", pp_fw, pp_on ? " on " : "", pp_board);
	n += fmt(line + n, sizeof line - n, "# Windows' own processor figure %s\r\n",
		 wcpu_on ? "on (Windows then never halts)" : "off (F7 turns it on)");
	if (opt_every)
		n += fmt(line + n, sizeof line - n, "# a line every %u s, each the average over its "
			 "interval, written to the disk every %u s (README.md says what each column is)"
			 "\r\n%s", opt_every, opt_write, log_cols);
	log_put(line);
}

/* "Log now": the live view as it stands */
static void log_view(void)
{
	char line[300], when[24];
	int i, n;
	now_text(when, sizeof when, 1);
	fmt(line, sizeof line, "# the live view at %s\r\n", when);
	log_put(line);
	for (i = 0; i < L_N; i++) {
		if (LD[i].kind == 'S')
			n = fmt(line, sizeof line, "#   [%s]", LD[i].label);
		else
			n = fmt(line, sizeof line, "#   %s%s", LD[i].label, LD[i].kind == 'L' ? ": " : "");
		n += line_render(&LN[i], line + n, sizeof line - n - 2);
		fmt(line + n, sizeof line - n, "\r\n");
		log_put(line);
	}
}

/* ==== the drawing tests ==== */
/* The mouse pointer is parked for the tests: at the test window's centre
   (on the drawing), off to its left for "text, pointer aside", and hidden
   for "text, no pointer".  At 800x480 with VMDisp9x where it is changes a
   drawing call's cost by more than twice (README.md 7.1). */
enum { T_IDLE, T_FILL, T_SCROLL, T_TEXT, T_TEXTASIDE, T_TEXTNP, T_SPRITE, T_FULL, T_DIB, T_N };
static const char *const t_key[T_N] = {
	"idle", "fill", "scroll", "text", "text-aside", "text-nopointer", "sprite", "full-copy",
	"full-picture"
};
static const char *const t_title[T_N] = {
	"Idle", "Fill", "Scroll", "Text", "Text, pointer aside", "Text, no pointer", "Sprite",
	"Full copy", "Full picture"
};
#define STAGE_W 400
#define STAGE_H 300
#define SPRITE 48
#define SETTLE_CAP_MS 10000     /* the most a test waits for the guest to quieten */

typedef struct {
	int done, stopped, skipped;
	char name[32];
	int w, h;                       /* the area drawn */
	u32 frames, us, pix;            /* pixels a frame */
	u32 wcpu;
	int wcpu_ok, pp;
	u32 clk, ins, run, halt, pics, draw, rows, vrows, whole, drop;
	u32 changed;                    /* pictures that changed (port 1.1) */
	int changed_ok;
	u32 disk, flushes;              /* disk reads and writes; flushes to the card */
	u32 settle_ms;                  /* waited before it */
	int settle_cap;                 /* the guest never quietened */
	POINT ptr;                      /* the pointer at the end */
	int moved;                      /* the mouse moved during it */
	int clockdiff;                  /* the two clocks differed by over 1%, or ticks dropped */
} Result;
static Result R[T_N];

static struct {
	int on, stop, test, phase;      /* phase 0: settling, 1: measuring */
	u32 q0, frames;
	int quiet;                      /* settling until the guest is quiet */
	POINT ptr_saved, home, aside, want;
	Sample s0, s1;
	HWND stage;
	HDC dc;
	int w, h;                       /* the stage */
	HBRUSH br[8], bg;
	HDC mimg, mmask, mfull;
	HBITMAP bimg, bmask, bfull, oimg, omask, ofull;
	BYTE *bits;
	BITMAPINFO *bmi;
	RGBQUAD pal[256];
	int sx, sy, dx, dy, lh;
	char when[24];
	int bpp, sw, sh, wcpu;
	int wcpu_was;                   /* Windows' own processor figure was on */
	int nopointer;                  /* the mouse pointer hidden for the test */
} B;

static const COLORREF t_colour[8] = {
	RGB(255, 0, 0), RGB(0, 255, 0), RGB(0, 0, 255), RGB(255, 255, 0),
	RGB(0, 255, 255), RGB(255, 0, 255), RGB(255, 255, 255), RGB(128, 128, 128)
};

static LRESULT CALLBACK stage_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
	PAINTSTRUCT ps;
	switch (m) {
	case WM_PAINT:
		BeginPaint(w, &ps);
		PatBlt(ps.hdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
		       ps.rcPaint.bottom - ps.rcPaint.top, BLACKNESS);
		EndPaint(w, &ps);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	}
	return DefWindowProcA(w, m, wp, lp);
}

static void stage_open(int x, int y, int w, int h)
{
	B.stage = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, "T386MONStage", "",
				  WS_POPUP, x, y, w, h, g_wnd, NULL, g_inst, NULL);
	if (!B.stage)
		return;
	ShowWindow(B.stage, SW_SHOWNA);
	UpdateWindow(B.stage);
	B.dc = GetDC(B.stage);
	B.w = w;
	B.h = h;
}

static void stage_close(void)
{
	if (B.dc)
		ReleaseDC(B.stage, B.dc);
	if (B.stage)
		DestroyWindow(B.stage);
	B.dc = NULL;
	B.stage = NULL;
}

static void mem_free(HDC *dc, HBITMAP *bm, HBITMAP old)
{
	if (*dc) {
		SelectObject(*dc, old);
		DeleteDC(*dc);
	}
	if (*bm)
		DeleteObject(*bm);
	*dc = NULL;
	*bm = NULL;
}

static void pointer_show(void)
{
	if (B.nopointer) {
		ShowCursor(TRUE);
		B.nopointer = 0;
	}
}

static void bench_free(void)
{
	int i;
	pointer_show();
	mem_free(&B.mimg, &B.bimg, B.oimg);
	mem_free(&B.mmask, &B.bmask, B.omask);
	mem_free(&B.mfull, &B.bfull, B.ofull);
	if (B.bits)
		HeapFree(GetProcessHeap(), 0, B.bits);
	if (B.bmi)
		HeapFree(GetProcessHeap(), 0, B.bmi);
	B.bits = NULL;
	B.bmi = NULL;
	stage_close();
	for (i = 0; i < 8; i++) {
		if (B.br[i])
			DeleteObject(B.br[i]);
		B.br[i] = NULL;
	}
	if (B.bg)
		DeleteObject(B.bg);
	B.bg = NULL;
}

/* a rainbow, integer only */
static BYTE tri(int k)
{
	k &= 255;
	return (BYTE) (k < 128 ? k * 2 : (255 - k) * 2);
}

static void text_frame(u32 n)
{
	char buf[96];
	int y, i, len;
	RECT rc;
	SetTextColor(B.dc, t_colour[(n + 6) & 7]);
	SetBkColor(B.dc, (n & 1) ? RGB(0, 0, 128) : RGB(0, 0, 0));
	for (y = 0, i = 0; y < B.h; y += B.lh, i++) {
		rc.left = 0;
		rc.top = y;
		rc.right = B.w;
		rc.bottom = y + B.lh;
		len = fmt(buf, sizeof buf, "Frame %u, line %u: the quick brown fox jumps over the lazy dog",
			  n, i);
		ExtTextOutA(B.dc, 2, y, ETO_OPAQUE | ETO_CLIPPED, &rc, buf, (UINT) len, NULL);
	}
}

/* what a test needs, made before its settling time; 0 if it cannot run */
static int test_prep(int t)
{
	int i, x, y;
	HGDIOBJ old;
	TEXTMETRICA tm;
	switch (t) {
	case T_FILL:
		for (i = 0; i < 8; i++)
			B.br[i] = CreateSolidBrush(t_colour[i]);
		B.bg = CreateSolidBrush(RGB(0, 0, 128));
		x = (B.sw - STAGE_W) / 2;
		y = (B.sh - STAGE_H) / 2;
		stage_open(x < 0 ? 0 : x, y < 0 ? 0 : y, STAGE_W, STAGE_H);
		if (!B.dc)
			return 0;
		SelectObject(B.dc, GetStockObject(DEFAULT_GUI_FONT));
		GetTextMetricsA(B.dc, &tm);
		B.lh = tm.tmHeight > 0 ? tm.tmHeight : 13;
		fmt(R[t].name, sizeof R[t].name, "Fill %ux%u", B.w, B.h);
		R[t].pix = (u32) B.w * (u32) B.h;
		return 1;
	case T_SCROLL:
		if (!B.dc)
			return 0;
		text_frame(0);
		fmt(R[t].name, sizeof R[t].name, "Scroll %ux%u", B.w, B.h);
		R[t].pix = (u32) B.w * (u32) B.h;
		return 1;
	case T_TEXT:
	case T_TEXTASIDE:
	case T_TEXTNP:
		if (!B.dc)
			return 0;
		fmt(R[t].name, sizeof R[t].name, t == T_TEXT ? "Text %ux%u" : t == T_TEXTASIDE ?
		    "Text %ux%u, pointer aside" : "Text %ux%u, no pointer", B.w, B.h);
		R[t].pix = (u32) B.w * (u32) B.h;
		return 1;
	case T_SPRITE:
		if (!B.dc)
			return 0;
		B.mimg = CreateCompatibleDC(B.dc);
		B.mmask = CreateCompatibleDC(B.dc);
		B.bimg = CreateCompatibleBitmap(B.dc, SPRITE, SPRITE);
		B.bmask = CreateBitmap(SPRITE, SPRITE, 1, 1, NULL);
		if (!B.mimg || !B.mmask || !B.bimg || !B.bmask)
			return 0;
		B.oimg = SelectObject(B.mimg, B.bimg);
		B.omask = SelectObject(B.mmask, B.bmask);
		/* the picture: rings on black; the mask: white (kept) round them */
		PatBlt(B.mimg, 0, 0, SPRITE, SPRITE, BLACKNESS);
		PatBlt(B.mmask, 0, 0, SPRITE, SPRITE, WHITENESS);
		SelectObject(B.mmask, GetStockObject(BLACK_BRUSH));
		SelectObject(B.mmask, GetStockObject(BLACK_PEN));
		Ellipse(B.mmask, 0, 0, SPRITE, SPRITE);
		SelectObject(B.mimg, GetStockObject(NULL_PEN));
		old = SelectObject(B.mimg, B.br[3]);
		Ellipse(B.mimg, 0, 0, SPRITE, SPRITE);
		SelectObject(B.mimg, B.br[0]);
		Ellipse(B.mimg, 10, 10, SPRITE - 10, SPRITE - 10);
		SelectObject(B.mimg, B.br[6]);
		Ellipse(B.mimg, 18, 14, 26, 22);
		SelectObject(B.mimg, old);
		SelectObject(B.dc, B.bg);
		PatBlt(B.dc, 0, 0, B.w, B.h, PATCOPY);
		B.sx = 0;
		B.sy = 0;
		B.dx = 4;
		B.dy = 3;
		fmt(R[t].name, sizeof R[t].name, "Sprite %ux%u", SPRITE, SPRITE);
		R[t].pix = SPRITE * SPRITE;
		return 1;
	case T_FULL:
		mem_free(&B.mimg, &B.bimg, B.oimg);
		mem_free(&B.mmask, &B.bmask, B.omask);
		stage_close();
		stage_open(0, 0, B.sw, B.sh);
		if (!B.dc)
			return 0;
		fmt(R[t].name, sizeof R[t].name, "Full copy %ux%u", B.w, B.h);
		R[t].pix = (u32) B.w * (u32) B.h;
		B.mfull = CreateCompatibleDC(B.dc);
		B.bfull = CreateCompatibleBitmap(B.dc, B.w + 64, B.h);
		if (!B.mfull || !B.bfull)
			return 0;
		B.ofull = SelectObject(B.mfull, B.bfull);
		/* bars 8 pixels wide in 8 colours: moved 8 pixels a frame, every
		   pixel changes */
		for (x = 0; x < B.w + 64; x += 8) {
			SelectObject(B.mfull, B.br[(x / 8) & 7]);
			PatBlt(B.mfull, x, 0, 8, B.h, PATCOPY);
		}
		return 1;
	case T_DIB:
		mem_free(&B.mfull, &B.bfull, B.ofull);
		if (!B.dc)
			return 0;
		fmt(R[t].name, sizeof R[t].name, "Full picture %ux%u", B.w, B.h);
		R[t].pix = (u32) B.w * (u32) B.h;
		B.bmi = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
				  sizeof(BITMAPINFOHEADER) + 256 * sizeof(RGBQUAD));
		B.bits = HeapAlloc(GetProcessHeap(), 0, (DWORD) ((B.w + 3) & ~3) * (DWORD) B.h);
		if (!B.bmi || !B.bits)
			return 0;
		B.bmi->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		B.bmi->bmiHeader.biWidth = B.w;
		B.bmi->bmiHeader.biHeight = B.h;
		B.bmi->bmiHeader.biPlanes = 1;
		B.bmi->bmiHeader.biBitCount = 8;
		B.bmi->bmiHeader.biCompression = BI_RGB;
		B.bmi->bmiHeader.biClrUsed = 256;
		/* squares of 8 pixels: each row made once and copied to the 7
		   below it */
		i = (B.w + 3) & ~3;
		for (y = 0; y < B.h; y++) {
			BYTE *row = B.bits + (DWORD) y * (DWORD) i;
			if (y & 7)
				memcpy(row, row - i, (size_t) i);
			else
				for (x = 0; x < i; x++)
					row[x] = (BYTE) ((x >> 3) + (y >> 3));
		}
		for (i = 0; i < 256; i++) {
			B.pal[i].rgbRed = tri(i);
			B.pal[i].rgbGreen = tri(i + 85);
			B.pal[i].rgbBlue = tri(i + 170);
			B.pal[i].rgbReserved = 0;
		}
		return 1;
	}
	return 1;
}

/* one frame of test t */
static void test_frame(int t, u32 n)
{
	int i;
	switch (t) {
	case T_FILL:
		SelectObject(B.dc, B.br[n & 7]);
		PatBlt(B.dc, 0, 0, B.w, B.h, PATCOPY);
		break;
	case T_SCROLL:
		BitBlt(B.dc, 0, 0, B.w, B.h - 4, B.dc, 0, 4, SRCCOPY);
		SelectObject(B.dc, B.br[(n >> 2) & 7]);
		PatBlt(B.dc, 0, B.h - 4, B.w, 4, PATCOPY);
		break;
	case T_TEXT:
	case T_TEXTASIDE:
	case T_TEXTNP:
		text_frame(n);
		break;
	case T_SPRITE:
		SelectObject(B.dc, B.bg);
		PatBlt(B.dc, B.sx, B.sy, SPRITE, SPRITE, PATCOPY);
		B.sx += B.dx;
		B.sy += B.dy;
		if (B.sx < 0 || B.sx > B.w - SPRITE) {
			B.dx = -B.dx;
			B.sx += 2 * B.dx;
		}
		if (B.sy < 0 || B.sy > B.h - SPRITE) {
			B.dy = -B.dy;
			B.sy += 2 * B.dy;
		}
		SetBkColor(B.dc, RGB(255, 255, 255));
		SetTextColor(B.dc, RGB(0, 0, 0));
		BitBlt(B.dc, B.sx, B.sy, SPRITE, SPRITE, B.mmask, 0, 0, SRCAND);
		BitBlt(B.dc, B.sx, B.sy, SPRITE, SPRITE, B.mimg, 0, 0, SRCPAINT);
		break;
	case T_FULL:
		BitBlt(B.dc, 0, 0, B.w, B.h, B.mfull, (int) (n & 7) * 8, 0, SRCCOPY);
		break;
	case T_DIB:
		for (i = 0; i < 256; i++)
			B.bmi->bmiColors[i] = B.pal[(i + (int) n * 8) & 255];
		SetDIBitsToDevice(B.dc, 0, 0, (DWORD) B.w, (DWORD) B.h, 0, 0, 0, (UINT) B.h, B.bits,
				  B.bmi, DIB_RGB_COLORS);
		break;
	}
	GdiFlush();
}

/* the least wait before a test; with the port it then waits on until the
   guest is quiet (pp_quiet()), at most SETTLE_CAP_MS in all.  Without it
   the waits are fixed, and longer after the pointer moves aside (the
   port's settling took up to 2 s there on the PC harness) */
static u32 settle_ms(int t)
{
	if (B.quiet)
		return 1000;
	return t == T_IDLE || t == T_TEXTASIDE ? 2500 : t == T_FILL || t == T_FULL ? 2000 : 1000;
}

static void bench_result(int t)
{
	Result *r = &R[t];
	const Sample *a = &B.s0, *b = &B.s1;
	r->done = 1;
	r->stopped = B.stop;
	r->frames = B.frames;
	r->us = qus(b->q - a->q);
	r->wcpu = b->st[S_CPU];
	r->wcpu_ok = wcpu_on;
	r->pp = a->pp && b->pp;
	GetCursorPos(&r->ptr);
	r->moved = r->ptr.x != B.want.x || r->ptr.y != B.want.y;
	if (r->pp) {
		r->clk = DPP(a, b, PP_I_CLOCK_US);
		r->ins = DPP(a, b, PP_I_INSNS_LO);
		r->run = DPP(a, b, PP_I_RUN_MS);
		r->halt = DPP(a, b, PP_I_HALT_MS);
		r->pics = DPP(a, b, PP_I_PICTURES);
		r->draw = DPP(a, b, PP_I_DRAW_US);
		r->rows = DPP(a, b, PP_I_FLUSH_ROWS);
		r->vrows = DPP(a, b, PP_I_VBE_ROWS);
		r->whole = DPP(a, b, PP_I_VBE_WHOLE);
		r->drop = DPP(a, b, PP_I_TIMER_DROPPED);
		r->changed_ok = VALID(b, PP_I_CHANGED);
		r->changed = DPP(a, b, PP_I_CHANGED);
		r->disk = DPP(a, b, PP_I_DISK_READS) + DPP(a, b, PP_I_DISK_WRITES);
		r->flushes = DPP(a, b, PP_I_DISK_FLUSHES);
		if (DPP(a, b, PP_I_MOUSE_REPORTS))
			r->moved = 1;
		/* Windows' clock (the 8254's ticks) falls behind when the board
		   drops ticks; the emulator's is the board's own */
		r->clockdiff = r->drop || (r->clk > r->us ? r->clk - r->us : r->us - r->clk) > r->us / 100;
	}
}

/* the figures of a result, as the table and the log show them; the time
   is the emulator's clock where there is one (real time on the board),
   else Windows' */
#define R_T(r)      ((r)->pp && (r)->clk ? (r)->clk : (r)->us)                  /* us */
#define R_FPS(r)    (R_T(r) ? muldiv((r)->frames, 100000000, R_T(r)) : 0)       /* x100 */
#define R_MSF(r)    ((r)->frames ? R_T(r) / (r)->frames / 100 : 0)             /* x10 */
#define R_MPIX(r)   (R_T(r) ? muldiv((r)->frames * (r)->pix, 100, R_T(r)) : 0)  /* x100 */
#define R_CHG(r)    ((r)->clk ? muldiv((r)->changed, 10000000, (r)->clk) : 0)  /* x10 */
#define R_BUSY(r)   ((r)->run + (r)->halt ? muldiv((r)->run, 100, (r)->run + (r)->halt) : 0)
#define R_MIPS(r)   ((r)->run ? muldiv((r)->ins, 1, (r)->run * 10) : 0)         /* x100 */
#define R_PICS(r)   ((r)->clk ? muldiv((r)->pics, 10000000, (r)->clk) : 0)     /* x10 */
#define R_DRAWMS(r) ((r)->pics ? (r)->draw / (r)->pics / 100 : 0)              /* x10 */
#define R_BLOAD(r)  ((r)->clk ? muldiv((r)->draw, 100, (r)->clk) : 0)
#define R_ROWS(r)   ((r)->clk ? muldiv((r)->rows, 1000000, (r)->clk) : 0)
#define R_VROWS(r)  ((r)->clk ? muldiv((r)->vrows, 1000000, (r)->clk) : 0)

static void log_result(int t)
{
	char line[300], name[24], size[16], cpu[8];
	const Result *r = &R[t];
	int n;
	fmt(name, sizeof name, "%s%s", t_key[t], r->stopped ? "(stopped)" : "");
	if (r->skipped) {
		fmt(line, sizeof line, "%-15s could not run (out of memory?)\r\n", name);
		log_put(line);
		return;
	}
	if (t == T_IDLE)
		scopy(size, "-", sizeof size);
	else
		fmt(size, sizeof size, "%ux%u", r->w, r->h);
	n = fmt(line, sizeof line, "%-15s %-7s %7H", name, size, r->us / 10000);
	if (r->pp)
		n += fmt(line + n, sizeof line - n, " %7H", r->clk / 10000);
	else
		n += fmt(line + n, sizeof line - n, " %7s", "-");
	if (t == T_IDLE)
		n += fmt(line + n, sizeof line - n, " %6s %8s %6s %7s", "-", "-", "-", "-");
	else
		n += fmt(line + n, sizeof line - n, " %6u %8H %6T %7H", r->frames, R_FPS(r), R_MSF(r),
			 R_MPIX(r));
	if (r->wcpu_ok)
		fmt(cpu, sizeof cpu, "%u", r->wcpu);
	else
		scopy(cpu, "-", sizeof cpu);
	n += fmt(line + n, sizeof line - n, " %4s", cpu);
	if (r->pp) {
		char fps[12];
		if (r->changed_ok)
			fmt(fps, sizeof fps, "%T", R_CHG(r));
		else
			scopy(fps, "-", sizeof fps);
		n += fmt(line + n, sizeof line - n, " %5u %5H %5s %6T %7T %5u %6u %8u %5u %5u %5u %5u",
			 R_BUSY(r), R_MIPS(r), fps, R_PICS(r), R_DRAWMS(r), R_BLOAD(r), R_ROWS(r),
			 R_VROWS(r), r->whole, r->drop, r->disk, r->flushes);
	} else
		n += fmt(line + n, sizeof line - n, " %5s %5s %5s %6s %7s %5s %6s %8s %5s %5s %5s %5s",
			 "-", "-", "-", "-", "-", "-", "-", "-", "-", "-", "-", "-");
	fmt(line + n, sizeof line - n, " %5T%s %u,%u%s%s\r\n", r->settle_ms / 100,
	    r->settle_cap ? "+" : " ", (u32) r->ptr.x, (u32) r->ptr.y,
	    r->moved ? " moved" : "", r->clockdiff ? " clocks" : "");
	log_put(line);
}

static int g_view;              /* 0 the live view, 1 the tests */
static int g_compact;
static void layout(void);
static void repaint_all(void);
static void view_set(int v);
static void compact_set(int on);
static void live_restart(void);
static char g_status[160] = "F5 runs all the tests; Esc stops them.";

/* the tests' view drawn now, while the next test settles, so that no
   drawing of this window falls in a test's measured time */
static void bench_show(void)
{
	if (g_view == 1 && !g_compact)
		layout();
	repaint_all();
	UpdateWindow(g_wnd);
}

/* the pointer to p, for the next test */
static void ptr_place(POINT p)
{
	SetCursorPos(p.x, p.y);
	GetCursorPos(&B.want);          /* where Windows put it */
}

/* the next test's settling starts */
static void settle_start(void)
{
	B.phase = 0;
	B.q0 = qnow();
	if (!B.quiet)
		return;
	/* the disks as the test before left them, or the live view's last second */
	if (B.test == T_IDLE)
		pp_quiet(1, DPP(&g_s[g_cur ^ 1], &g_s[g_cur], PP_I_DISK_ACTIVITY) != 0,
			 g_s[g_cur].v[PP_I_DISK_ACTIVITY]);
	else
		pp_quiet(1, DPP(&B.s0, &B.s1, PP_I_DISK_ACTIVITY) != 0, B.s1.v[PP_I_DISK_ACTIVITY]);
}

static void bench_start(void)
{
	char line[1000];
	int n;
	if (B.on)
		return;
	compact_set(0);
	memset(&B, 0, sizeof B);
	memset(R, 0, sizeof R);
	mode_read();
	B.on = 1;
	log_hold = 1;                   /* the log waits in memory until bench_end() */
	B.bpp = scr_bpp;
	/* Windows' own processor figure off for the tests: while it is on
	   Windows never halts, so the guest is never quiet before a test, and
	   on the PC harness it slowed the text test with the pointer aside by
	   a fifth (1.12 frames a second against 1.39) */
	B.wcpu_was = wcpu_on;
	if (wcpu_on)
		wcpu_set(0);
	B.wcpu = wcpu_on;
	B.sw = GetSystemMetrics(SM_CXSCREEN);
	B.sh = GetSystemMetrics(SM_CYSCREEN);
	B.quiet = VALID(&g_s[g_cur], PP_I_SECONDS) && VALID(&g_s[g_cur], PP_I_HALT_PCT);
	/* the pointer: at the test window's centre, which is the screen's;
	   aside: half way from the screen's left edge to the test window */
	GetCursorPos(&B.ptr_saved);
	B.home.x = B.sw / 2;
	B.home.y = B.sh / 2;
	B.aside.x = (B.sw - STAGE_W) / 4;
	B.aside.y = B.sh / 2;
	if (B.aside.x < 8)
		B.aside.x = 8;
	now_text(B.when, sizeof B.when, 1);
	scopy(R[T_IDLE].name, "Idle", sizeof R[T_IDLE].name);
	B.test = T_IDLE;
	KillTimer(g_wnd, 1);
	fmt(g_status, sizeof g_status, "Test 1 of %u: Idle. Leave the mouse alone; Esc stops.", T_N);
	g_view = 1;
	bench_show();
	ptr_place(B.home);
	n = fmt(line, sizeof line, "# tests at %s: %u x %u in %s, %u s each; Windows' own processor "
		"figure %s\r\n", B.when, scr_w, scr_h, depth_name(scr_bpp), opt_secs,
		B.wcpu_was ? "off for the tests (on before and after them)" : "off");
	n += fmt(line + n, sizeof line - n, "# the pointer parked at %d,%d (on the tests' drawing), "
		 "at %d,%d for text-aside, hidden for text-nopointer; it was at %d,%d\r\n",
		 (int) B.want.x, (int) B.want.y, (int) B.aside.x, (int) B.aside.y,
		 (int) B.ptr_saved.x, (int) B.ptr_saved.y);
	n += fmt(line + n, sizeof line - n, "# frames/s, ms/fr and Mpix/s by %s; settle: seconds "
		 "waited before the test%s\r\n", pp_on ? "the emulator's clock (emu_secs)" :
		 "Windows' clock (secs)", B.quiet ? ", until the guest was at least half halted for "
		 "a whole second and the disks idle for 2.5 s (+: not in 10 s)" : "");
	fmt(line + n, sizeof line - n,
	    "# test          size       secs emu_secs frames frames/s  ms/fr  Mpix/s wcpu gbusy  "
	    "mips   fps refr/s draw_ms bload rows/s vbe_rows whole tdrop dk_rw flush settle "
	    "pointer\r\n");
	log_put(line);
	settle_start();
}

static void bench_end(void)
{
	char line[900];
	int n, t, marks = 0;
	bench_free();
	B.on = 0;
	SetCursorPos(B.ptr_saved.x, B.ptr_saved.y);
	n = fmt(line, sizeof line, "# summary:");
	for (t = T_FILL; t < T_N; t++)
		if (R[t].done && !R[t].skipped)
			n += fmt(line + n, sizeof line - n, " %s %H,", t_key[t], R_FPS(&R[t]));
	n += fmt(line + n, sizeof line - n, " frames a second");
	if (R[T_IDLE].done && R[T_IDLE].pp) {
		n += fmt(line + n, sizeof line - n, "; idle: guest %u%% busy", R_BUSY(&R[T_IDLE]));
		if (R[T_IDLE].changed_ok)
			n += fmt(line + n, sizeof line - n, ", panel %T fps", R_CHG(&R[T_IDLE]));
		n += fmt(line + n, sizeof line - n, ", %T pictures drawn a second", R_PICS(&R[T_IDLE]));
	}
	if (R[T_IDLE].done && R[T_IDLE].wcpu_ok)
		n += fmt(line + n, sizeof line - n, "; idle: Windows %u%%", R[T_IDLE].wcpu);
	n += fmt(line + n, sizeof line - n, "%s\r\n", B.stop ? " (stopped with Esc)" : "");
	for (t = 0; t < T_N; t++)
		if (R[t].done && !R[t].skipped && (R[t].moved || R[t].clockdiff)) {
			if (!marks++)
				n += fmt(line + n, sizeof line - n, "# note:");
			n += fmt(line + n, sizeof line - n, " %s%s%s;", t_key[t],
				 R[t].moved ? ", the mouse moved" : "",
				 R[t].clockdiff ? ", the two clocks differed" : "");
		}
	if (marks)
		fmt(line + n, sizeof line - n, "\r\n");
	log_put(line);
	log_hold = 0;
	log_flush();                    /* the tests' lines, all at once */
	scopy(g_status, B.stop ? "Tests stopped." : "Tests done; the results are in the log too.",
	      sizeof g_status);
	wcpu_set(B.wcpu_was);           /* as before the tests, and the box with it */
	live_restart();
	SetTimer(g_wnd, 1, 1000, NULL);
	bench_show();
	if (opt_quit)
		PostMessageA(g_wnd, WM_CLOSE, 0, 0);
}

static void bench_step(void)
{
	u32 el = qus(qnow() - B.q0) / 1000;     /* ms in this phase */
	if (B.phase == 0) {
		u32 want = settle_ms(B.test);
		if (el < want && !B.stop) {
			MsgWaitForMultipleObjects(0, NULL, FALSE, want - el > 100 ? 100 : want - el,
						  QS_ALLINPUT);
			return;
		}
		/* then until the guest has been quiet for a second (Windows may
		   still be drawing what an earlier test covered) and the disks
		   for 2.5 s (so that the card's flush is done), at most
		   SETTLE_CAP_MS; polled every 200 ms */
		if (B.quiet && !B.stop && el < SETTLE_CAP_MS && !pp_quiet(0, 0, 0)) {
			MsgWaitForMultipleObjects(0, NULL, FALSE, 200, QS_ALLINPUT);
			return;
		}
		if (B.stop) {
			bench_end();
			return;
		}
		R[B.test].settle_ms = el;
		R[B.test].settle_cap = B.quiet && el >= SETTLE_CAP_MS;
		if (B.test == T_TEXTNP) {
			/* Windows' software pointer is taken off and put back
			   round every drawing call; this test shows that cost */
			ShowCursor(FALSE);
			B.nopointer = 1;
		}
		take(&B.s0, 0, 0);
		B.frames = 0;
		B.phase = 1;
		B.q0 = B.s0.q;
		return;
	}
	if (B.test == T_IDLE) {
		if (el < opt_secs * 1000 && !B.stop) {
			u32 left = opt_secs * 1000 - el;
			MsgWaitForMultipleObjects(0, NULL, FALSE, left > 100 ? 100 : left, QS_ALLINPUT);
			return;
		}
	} else {
		test_frame(B.test, B.frames);
		B.frames++;
		if (!B.stop && qus(qnow() - B.q0) < opt_secs * 1000000)
			return;
	}
	take(&B.s1, 1, 0);
	pointer_show();
	bench_result(B.test);
	log_result(B.test);
	for (;;) {
		B.test++;
		if (B.test >= T_N || B.stop) {
			bench_end();
			return;
		}
		scopy(R[B.test].name, t_title[B.test], sizeof R[B.test].name);
		if (test_prep(B.test)) {
			R[B.test].w = B.test == T_SPRITE ? SPRITE : B.w;
			R[B.test].h = B.test == T_SPRITE ? SPRITE : B.h;
			break;
		}
		R[B.test].done = R[B.test].skipped = 1;
		log_result(B.test);
	}
	fmt(g_status, sizeof g_status, "Test %u of %u: %s. Leave the mouse alone; Esc stops.",
	    B.test + 1, T_N, R[B.test].name);
	bench_show();
	ptr_place(B.test == T_TEXTASIDE ? B.aside : B.home);
	settle_start();
}

/* ==== the window ==== */
#define ID_LIVE    101
#define ID_TESTS   102
#define ID_RUN     103
#define ID_LOGNOW  104
#define ID_COMPACT 105
#define ID_TOP     106
#define ID_WCPU    107
#define ID_STOP    108
#define N_BTN      7
#define BTN_H      22
static HWND g_btn[N_BTN];
static const struct { int id; const char *text; int w; } BTN[N_BTN] = {
	{ ID_LIVE, "&Live", 46 }, { ID_TESTS, "&Tests", 50 }, { ID_RUN, "&Run all tests", 88 },
	{ ID_LOGNOW, "Log &now", 62 }, { ID_COMPACT, "&Compact", 64 }, { ID_TOP, "On to&p", 66 },
	{ ID_WCPU, "&Windows CPU", 96 },
};
static RECT g_full_rect;
static HFONT f_txt, f_bold, f_big, f_cmp, f_tab, f_tabb;
static int lh_txt, lh_big, lh_tab, g_vx, g_cw, g_ch, g_cellw;

/* How this window is drawn.  With VMDisp9x at 800x480 every drawing call
   to the screen costs about 20 ms of the guest's time on the PC harness,
   most of it Windows' software mouse pointer taken off and put back round
   the call; into memory a call costs about 1 ms.  There the window is
   drawn into a copy in memory and the changed part put on the screen with
   one call a second.  With the standard VGA driver a call costs about 1 ms
   and a copy from memory to its planes is slow, so it draws directly.
   draw_pick() measures which at the start. */
static int g_buf = -1;          /* 1: through g_mdc; 0: directly; -1: not yet known */
static HDC g_mdc;
static HBITMAP g_mbm, g_mold;
static int g_mw, g_mh, g_valid;
static RECT g_dirty;
static int g_ndirty;
static u32 cal_text_us, cal_copy_us;

static void dirty_add(int x0, int y0, int x1, int y1)
{
	if (!g_buf || x1 <= x0 || y1 <= y0)
		return;
	if (!g_ndirty) {
		g_dirty.left = x0;
		g_dirty.top = y0;
		g_dirty.right = x1;
		g_dirty.bottom = y1;
	} else {
		if (x0 < g_dirty.left) g_dirty.left = x0;
		if (y0 < g_dirty.top) g_dirty.top = y0;
		if (x1 > g_dirty.right) g_dirty.right = x1;
		if (y1 > g_dirty.bottom) g_dirty.bottom = y1;
	}
	g_ndirty++;
}

/* the copy in memory, the client area's size */
static void buf_make(void)
{
	HDC dc;
	if (g_buf != 1 || (g_mdc && g_mw == g_cw && g_mh == g_ch))
		return;
	if (g_mdc) {
		SelectObject(g_mdc, g_mold);
		DeleteObject(g_mbm);
		DeleteDC(g_mdc);
		g_mdc = NULL;
	}
	dc = GetDC(g_wnd);
	g_mdc = CreateCompatibleDC(dc);
	g_mbm = CreateCompatibleBitmap(dc, g_cw > 0 ? g_cw : 1, g_ch > 0 ? g_ch : 1);
	ReleaseDC(g_wnd, dc);
	if (!g_mdc || !g_mbm) {
		/* no memory for it: draw directly */
		if (g_mbm)
			DeleteObject(g_mbm);
		if (g_mdc)
			DeleteDC(g_mdc);
		g_mdc = NULL;
		g_mbm = NULL;
		g_buf = 0;
		return;
	}
	g_mold = SelectObject(g_mdc, g_mbm);
	g_mw = g_cw;
	g_mh = g_ch;
	g_valid = 0;
}

/* the changed part of the copy onto the screen: one call */
static void buf_flush(void)
{
	HDC dc;
	if (g_buf != 1 || !g_ndirty || !g_mdc)
		return;
	dc = GetDC(g_wnd);
	BitBlt(dc, g_dirty.left, g_dirty.top, g_dirty.right - g_dirty.left,
	       g_dirty.bottom - g_dirty.top, g_mdc, g_dirty.left, g_dirty.top, SRCCOPY);
	ReleaseDC(g_wnd, dc);
	g_ndirty = 0;
	dbg_calls++;
}

/* everything drawn again at the next WM_PAINT */
static void repaint_all(void)
{
	g_valid = 0;
	InvalidateRect(g_wnd, NULL, g_buf != 1);
}

static HFONT font(const char *face, int h, int bold)
{
	return CreateFontA(-h, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
			   OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
			   VARIABLE_PITCH | FF_SWISS, face);
}

static void font_free(HFONT *f)
{
	if (*f)
		DeleteObject(*f);
	*f = NULL;
}

/* A font still selected into a device context must not be deleted: on
   Windows 95 DeleteObject then says it succeeded, but the font stays in
   GDI's small heap.  1.1's font sizing did so on every try, and Windows'
   System Resources fell from 97% to 57% free over six runs of its tests
   on the PC harness; with this before each font_free(), and before a DC
   goes back, they stay at 97%.  So every DC that held one of these fonts
   gets a stock font first. */
static void font_off(HDC dc)
{
	if (dc)
		SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
}

static int text_wn(HDC dc, const char *s, int n)
{
	SIZE z;
	if (n <= 0 || !GetTextExtentPoint32A(dc, s, n, &z))
		return 0;
	return z.cx;
}

static int text_w(HDC dc, const char *s)
{
	return text_wn(dc, s, slen(s));
}

static int font_h(HDC dc)
{
	TEXTMETRICA tm;
	GetTextMetricsA(dc, &tm);
	return tm.tmHeight + 1;
}

/* the widest digit: a field of N is N of these */
static int cell_w(HDC dc)
{
	int i, w = 1;
	for (i = 0; i < 10; i++) {
		char c = (char) ('0' + i);
		int k = text_wn(dc, &c, 1);
		if (k > w)
			w = k;
	}
	return w;
}

static int top_y(void)
{
	return g_compact ? 0 : BTN_H + 6;
}

/* where each piece of a line goes; the dc has the line's font */
static void line_layout(HDC dc, Line *l, int x0, int right, int cellw)
{
	const char *p = l->tmpl ? l->tmpl : "", *s = p;
	int x = x0, n = 0, r = 0, need = 0;
	for (;;) {
		if (*p == '{' || !*p) {
			if (p > s && r <= MAXF) {
				int k = text_wn(dc, s, (int) (p - s));
				l->run[r].off = (short) (s - l->tmpl);
				l->run[r].len = (short) (p - s);
				l->run[r].x = (short) x;
				x += k;
				need += k;
				r++;
			}
			if (!*p)
				break;
			p++;
			if (n < MAXF) {
				int left = 0, w, k;
				if (*p == '-') {
					left = 1;
					p++;
				}
				if (*p == '*') {
					/* to the end of the line, at least as wide as its
					   text; only the text counts for fitting */
					k = text_w(dc, l->fv[n]);
					w = right - x > k ? right - x : k;
					left = 1;
				} else
					k = w = (int) atou(p) * cellw;
				l->fx[n] = (short) x;
				l->fw[n] = (short) w;
				l->fl[n] = (char) left;
				x += w;
				need += k;
				n++;
			}
			while (*p && *p != '}')
				p++;
			if (*p)
				p++;
			s = p;
		} else
			p++;
	}
	l->nf = n;
	l->nr = r;
	l->x0 = x0;
	l->width = need;
	l->cellw = cellw;
	l->laid = 1;
}

static void put_field(HDC dc, Line *l, int f, int right)
{
	RECT rc;
	const char *s = l->fv[f];
	rc.left = l->fx[f];
	rc.top = l->y;
	rc.right = l->fx[f] + l->fw[f];
	if (rc.right > right)
		rc.right = right;
	rc.bottom = l->y + l->h;
	SetTextAlign(dc, l->fl[f] ? TA_LEFT | TA_TOP : TA_RIGHT | TA_TOP);
	ExtTextOutA(dc, l->fl[f] ? rc.left : l->fx[f] + l->fw[f] - 1, l->y, ETO_OPAQUE | ETO_CLIPPED,
		    &rc, s, (UINT) slen(s), NULL);
	SetTextAlign(dc, TA_LEFT | TA_TOP);
	scopy(l->fs[f], s, FV);
	dirty_add(rc.left, rc.top, rc.right, rc.bottom);
	dbg_chars += (u32) slen(s);
	dbg_calls++;
}

/* the whole line: its rectangle cleared, its text, its fields */
static void line_draw(HDC dc, Line *l, int right)
{
	RECT rc;
	int i;
	rc.left = l->x0;
	rc.top = l->y;
	rc.right = right;
	rc.bottom = l->y + l->h;
	ExtTextOutA(dc, l->x0, l->y, ETO_OPAQUE, &rc, "", 0, NULL);
	dirty_add(rc.left, rc.top, rc.right, rc.bottom);
	for (i = 0; i < l->nr; i++) {
		ExtTextOutA(dc, l->run[i].x, l->y, 0, NULL, l->tmpl + l->run[i].off,
			    (UINT) l->run[i].len, NULL);
		dbg_chars += (u32) l->run[i].len;
		dbg_calls++;
	}
	for (i = 0; i < l->nf; i++)
		put_field(dc, l, i, right);
}

/* each second: the fields that changed; a line whose template changed is
   laid out and drawn whole */
static void line_update(HDC dc, Line *l, int right)
{
	int i;
	if (!l->laid) {
		line_layout(dc, l, l->x0, right, l->cellw ? l->cellw : g_cellw);
		line_draw(dc, l, right);
		return;
	}
	for (i = 0; i < l->nf; i++)
		if (!seq(l->fv[i], l->fs[i]))
			put_field(dc, l, i, right);
}

static void dc_colours(HDC dc, COLORREF c)
{
	SetTextColor(dc, c);
	SetBkColor(dc, GetSysColor(COLOR_WINDOW));
	SetBkMode(dc, OPAQUE);
}

/* the largest fonts that fit the window */
static const int cand[] = { 36, 30, 26, 22, 19, 16, 14, 13, 12, 11, 10 };
#define NCAND ((int) (sizeof cand / sizeof cand[0]))

static void layout_live(HDC dc)
{
	int c, i, labw, need_h, nsec = 0, nline = 0, ok = 0, maxw;
	for (i = 0; i < L_N; i++) {
		if (LD[i].kind == 'S')
			nsec++;
		else if (LD[i].kind == 'L')
			nline++;
	}
	for (c = 0; c < NCAND && !ok; c++) {
		int y;
		font_off(dc);
		font_free(&f_txt);
		font_free(&f_bold);
		font_free(&f_big);
		f_txt = font("MS Sans Serif", cand[c], 0);
		f_bold = font("MS Sans Serif", cand[c], 1);
		f_big = font("Arial", cand[c] * 4 / 3, 1);
		SelectObject(dc, f_big);
		lh_big = font_h(dc);
		g_cellw = cell_w(dc);
		LN[L_HEAD].x0 = 8;
		line_layout(dc, &LN[L_HEAD], 8, g_cw, g_cellw);
		maxw = 8 + LN[L_HEAD].width;
		SelectObject(dc, f_txt);
		lh_txt = font_h(dc);
		g_cellw = cell_w(dc);
		labw = 0;
		for (i = 0; i < L_N; i++)
			if (LD[i].kind == 'L' && text_w(dc, LD[i].label) > labw)
				labw = text_w(dc, LD[i].label);
		g_vx = 12 + labw + 12;
		for (i = 0; i < L_N; i++) {
			if (LD[i].kind != 'L')
				continue;
			line_layout(dc, &LN[i], g_vx, g_cw, g_cellw);
			if (g_vx + LN[i].width > maxw)
				maxw = g_vx + LN[i].width;
		}
		need_h = top_y() + 4 + lh_big + 4 + (nsec + nline) * lh_txt + 4;
		ok = need_h <= g_ch && maxw + 4 <= g_cw;
		if (c == NCAND - 1)
			ok = 1;
		if (ok) {
			/* the lines' places, for drawing */
			y = top_y() + 4;
			for (i = 0; i < L_N; i++) {
				LN[i].y = y;
				LN[i].h = LD[i].kind == 'H' ? lh_big : lh_txt;
				LN[i].font = LD[i].kind == 'H' ? &f_big : &f_txt;
				y += LD[i].kind == 'H' ? lh_big + 4 : lh_txt;
			}
		}
	}
	/* the headline was laid out with the big font's digits */
	SelectObject(dc, f_big);
	line_layout(dc, &LN[L_HEAD], 8, g_cw, cell_w(dc));
	SelectObject(dc, f_txt);
}

static void layout_compact(HDC dc)
{
	int c, i, ok = 0, cw;
	for (c = 0; c < NCAND && !ok; c++) {
		font_off(dc);
		font_free(&f_cmp);
		f_cmp = font("Arial", cand[c], 1);
		SelectObject(dc, f_cmp);
		lh_big = font_h(dc);
		cw = cell_w(dc);
		ok = 2 * lh_big + 6 <= g_ch;
		for (i = 0; i < 2; i++) {
			line_layout(dc, &CL[i], 6, g_cw, cw);
			CL[i].y = 3 + i * lh_big;
			CL[i].h = lh_big;
			if (6 + CL[i].width > g_cw)
				ok = 0;
		}
		if (c == NCAND - 1)
			ok = 1;
	}
}

/* the tests' table */
#define NCOL 8
static const char *const col_head[NCOL] = {
	"Test", "Frames/s", "ms/frame", "Mpixels/s", "Guest busy", "Panel fps", "Board ms/pic",
	"Windows"
};
#define CELL 32
static char cell[T_N][NCOL][CELL];
static int col_x[NCOL + 1];
/* the columns shown: Mpixels/s only in the log (a photograph wants large
   text), Windows' figure only if it was on for the tests */
static int col_n, col_i[NCOL];

static void cols_pick(void)
{
	/* without the port, Mpixels/s in place of the emulator's three */
	static const int base[] = { 0, 1, 2, 4, 5, 6 }, noport[] = { 0, 1, 2, 3 };
	int i, k = pp_on ? 6 : 4;
	for (col_n = 0; col_n < k; col_n++)
		col_i[col_n] = pp_on ? base[col_n] : noport[col_n];
	if (B.wcpu)
		col_i[col_n++] = 7;
	for (i = col_n; i < NCOL; i++)
		col_i[i] = 0;
}

static void cells_make(void)
{
	int t;
	for (t = 0; t < T_N; t++) {
		Result *r = &R[t];
		int c;
		for (c = 0; c < NCOL; c++)
			cell[t][c][0] = 0;
		scopy(cell[t][0], t_title[t], CELL);  /* the sizes are on a line below */
		if (!r->done) {
			if (B.on && B.test == t)
				scopy(cell[t][1], "running", CELL);
			continue;
		}
		if (r->skipped) {
			scopy(cell[t][1], "could not run", CELL);
			continue;
		}
		/* marks: * stopped with Esc, m the mouse moved, c the two clocks
		   differed by more than 1% (or timer ticks were dropped) */
		if (t != T_IDLE)
			fmt(cell[t][1], CELL, "%H%s%s%s", R_FPS(r), r->stopped ? "*" : "",
			    r->moved ? " m" : "", r->clockdiff ? " c" : "");
		else
			fmt(cell[t][1], CELL, "-%s%s%s", r->stopped ? "*" : "", r->moved ? " m" : "",
			    r->clockdiff ? " c" : "");
		if (t != T_IDLE) {
			fmt(cell[t][2], CELL, "%T", R_MSF(r));
			fmt(cell[t][3], CELL, "%H", R_MPIX(r));
		}
		if (r->pp) {
			fmt(cell[t][4], CELL, "%u%%", R_BUSY(r));
			if (r->changed_ok)
				fmt(cell[t][5], CELL, "%T", R_CHG(r));
			else
				scopy(cell[t][5], "-", CELL);
			fmt(cell[t][6], CELL, "%T", R_DRAWMS(r));
		}
		if (r->wcpu_ok)
			fmt(cell[t][7], CELL, "%u%%", r->wcpu);
		else
			scopy(cell[t][7], "-", CELL);
	}
}

static int tests_lines(void)
{
	return 2 + 1 + T_N + 6;         /* title, status, header, rows, notes */
}

/* the notes under the table */
#define NNOTE 5
static char g_note[NNOTE][200];
static HFONT f_note;
static int lh_note;

static void notes_make(void)
{
	int t, m = 0, c = 0, s = 0;
	for (t = 0; t < T_N; t++) {
		m |= R[t].done && R[t].moved;
		c |= R[t].done && R[t].clockdiff;
		s |= R[t].done && R[t].stopped;
	}
	if (pp_on)
		scopy(g_note[0], "Frames/s: what Windows drew in a second, by the emulator's clock. "
		      "Guest busy: of the emulated processor's time.", sizeof g_note[0]);
	else
		fmt(g_note[0], sizeof g_note[0], "Frames/s: what Windows drew in a second, by its own "
		    "clock. Mpixels/s: pixels drawn a second.%s", B.wcpu ? " Windows: its own "
		    "processor figure." : "");
	if (pp_on)
		scopy(g_note[1], "Panel fps: pictures on the panel that changed, a second. Board "
		      "ms/pic: the board's time a picture.", sizeof g_note[1]);
	else
		scopy(g_note[1], "Not running on tiny386: no emulator figures.", sizeof g_note[1]);
	if (R[T_FILL].w)
		fmt(g_note[2], sizeof g_note[2], "Fill, scroll and text: a %ux%u window; sprite %ux%u; "
		    "full: the whole screen, %ux%u.", R[T_FILL].w, R[T_FILL].h, SPRITE, SPRITE,
		    B.sw, B.sh);
	else
		fmt(g_note[2], sizeof g_note[2], "Fill, scroll and text: a %ux%u window; sprite %ux%u; "
		    "full: the whole screen.", STAGE_W, STAGE_H, SPRITE, SPRITE);
	if (R[T_IDLE].done)
		fmt(g_note[3], sizeof g_note[3], "The pointer at %d,%d on the drawing; aside: at %d,%d; "
		    "no pointer: hidden.%s%s%s", (int) B.home.x, (int) B.home.y, (int) B.aside.x,
		    (int) B.aside.y, s ? " *: stopped." : "", m ? " m: the mouse moved." : "",
		    c ? " c: the two clocks differed." : "");
	else
		scopy(g_note[3], "The tests park the mouse pointer on their drawing, and put it back.",
		      sizeof g_note[3]);
	if (pp_on)
		fmt(g_note[4], sizeof g_note[4], "tiny386 %s on %s (%s).", pp_fw, pp_board, pp_why);
	else
		g_note[4][0] = 0;
}

static void layout_tests(HDC dc)
{
	int c, i, t, x, h = 10, wmax, k;
	cells_make();
	cols_pick();
	for (c = 0; c < NCAND + 3; c++) {
		int w[NCOL];
		h = c < 3 ? 56 - c * 8 : cand[c - 3];
		font_off(dc);
		font_free(&f_tab);
		font_free(&f_tabb);
		f_tab = font("Arial", h, 0);
		f_tabb = font("Arial", h, 1);
		SelectObject(dc, f_tabb);
		lh_tab = font_h(dc);
		for (i = 0; i < col_n; i++)
			w[i] = text_w(dc, col_head[col_i[i]]);
		SelectObject(dc, f_tab);
		for (t = 0; t < T_N; t++)
			for (i = 0; i < col_n; i++) {
				int k = text_w(dc, cell[t][col_i[i]]);
				if (k > w[i])
					w[i] = k;
			}
		x = 8;
		for (i = 0; i < col_n; i++) {
			col_x[i] = x;
			x += w[i] + h * 2 / 3;
		}
		col_x[col_n] = x;
		if (x <= g_cw && top_y() + 4 + tests_lines() * lh_tab <= g_ch)
			break;
	}
	/* the notes in the table's font, or the largest smaller one in which
	   the widest fits (they were cut off at the window's edge) */
	notes_make();
	for (k = 0;;) {
		font_off(dc);
		font_free(&f_note);
		f_note = font("Arial", h, 0);
		SelectObject(dc, f_note);
		for (wmax = 0, i = 0; i < NNOTE; i++)
			if (text_w(dc, g_note[i]) > wmax)
				wmax = text_w(dc, g_note[i]);
		if (8 + wmax + 4 <= g_cw)
			break;
		while (k < NCAND && cand[k] >= h)
			k++;
		if (k >= NCAND)
			break;
		h = cand[k];
	}
	lh_note = font_h(dc);
	SelectObject(dc, f_tab);
}

static void layout(void)
{
	HDC dc = GetDC(g_wnd);
	RECT rc;
	GetClientRect(g_wnd, &rc);
	g_cw = rc.right;
	g_ch = rc.bottom;
	font_off(g_mdc);                /* the copy in memory may hold a font about to go */
	if (g_compact)
		layout_compact(dc);
	else if (g_view == 0)
		layout_live(dc);
	else
		layout_tests(dc);
	font_off(dc);                   /* none of ours left in a DC given back */
	ReleaseDC(g_wnd, dc);
	buf_make();
	g_valid = 0;
}

/* Which way to draw (g_buf): five short texts drawn on the screen, and
   five copies of the same size from memory, each timed alone; called once
   the window is on the screen, before its first picture.  The least time
   of each counts: a moving mouse pointer is drawn at interrupt time and
   only ever adds to a call's time.  (The average of three, as before, read
   4.4 ms for text with the standard VGA driver and the mouse moving, and
   chose the copy, which there costs ten times as much.) */
#define CAL_N 5
static int opt_draw = -1;       /* /direct 0, /buffer 1 */
static void draw_pick(void)
{
	HDC dc = GetDC(g_wnd), m = CreateCompatibleDC(dc);
	HBITMAP b = CreateCompatibleBitmap(dc, 120, 16), o;
	RECT rc;
	u32 q, t;
	int i;
	rc.left = 8;
	rc.top = g_ch > 40 ? g_ch - 20 : 20;
	rc.right = 128;
	rc.bottom = rc.top + 16;
	if (m && b) {
		o = SelectObject(m, b);
		PatBlt(m, 0, 0, 120, 16, WHITENESS);
		SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
		cal_text_us = cal_copy_us = 0xFFFFFFFFUL;
		for (i = 0; i < CAL_N; i++) {
			q = qnow();
			ExtTextOutA(dc, rc.left, rc.top, ETO_OPAQUE, &rc, "1234", 4, NULL);
			t = qus(qnow() - q);
			if (t < cal_text_us)
				cal_text_us = t;
		}
		for (i = 0; i < CAL_N; i++) {
			q = qnow();
			BitBlt(dc, rc.left, rc.top, 120, 16, m, 0, 0, SRCCOPY);
			t = qus(qnow() - q);
			if (t < cal_copy_us)
				cal_copy_us = t;
		}
		SelectObject(m, o);
	}
	if (b)
		DeleteObject(b);
	if (m)
		DeleteDC(m);
	ReleaseDC(g_wnd, dc);
	g_buf = cal_text_us > 4000 && cal_copy_us < 3 * cal_text_us;
	if (opt_draw >= 0)
		g_buf = opt_draw;
}

static void put(HDC dc, HFONT f, COLORREF c, int x, int y, int right, int h, const char *s)
{
	RECT rc;
	rc.left = x;
	rc.top = y;
	rc.right = right;
	rc.bottom = y + h;
	SelectObject(dc, f);
	dc_colours(dc, c);
	ExtTextOutA(dc, x, y, ETO_OPAQUE | ETO_CLIPPED, &rc, s, (UINT) slen(s), NULL);
	dirty_add(rc.left, rc.top, rc.right, rc.bottom);
	dbg_chars += (u32) slen(s);
	dbg_calls++;
}

#define NAVY  RGB(0, 0, 128)

static void paint_live(HDC dc)
{
	int i;
	COLORREF tx = GetSysColor(COLOR_WINDOWTEXT);
	for (i = 0; i < L_N; i++) {
		Line *l = &LN[i];
		switch (LD[i].kind) {
		case 'H':
			SelectObject(dc, f_big);
			dc_colours(dc, NAVY);
			if (!l->laid)
				line_layout(dc, l, 8, g_cw, cell_w(dc));
			line_draw(dc, l, g_cw);
			break;
		case 'S':
			put(dc, f_bold, NAVY, 6, l->y, g_cw, lh_txt, LD[i].label);
			break;
		default:
			put(dc, f_txt, tx, 12, l->y, g_vx, lh_txt, LD[i].label);
			if (!l->laid)
				line_layout(dc, l, g_vx, g_cw, g_cellw);
			line_draw(dc, l, g_cw);
			break;
		}
	}
}

static void paint_compact(HDC dc)
{
	int i;
	SelectObject(dc, f_cmp);
	dc_colours(dc, NAVY);
	for (i = 0; i < 2; i++) {
		if (!CL[i].laid)
			line_layout(dc, &CL[i], 6, g_cw, cell_w(dc));
		line_draw(dc, &CL[i], g_cw);
	}
}

static void paint_tests(HDC dc)
{
	int y = top_y() + 4, t, i;
	char title[200];
	COLORREF tx = GetSysColor(COLOR_WINDOWTEXT);
	cells_make();
	if (R[T_IDLE].done || B.on)
		fmt(title, sizeof title, "Tests at %s: %u x %u in %s, %u s each", B.when, scr_w, scr_h,
		    depth_name(B.bpp), opt_secs);
	else
		scopy(title, "No tests yet: Run all tests (F5) takes a minute or two; Esc stops them.",
		      sizeof title);
	put(dc, f_tabb, NAVY, 8, y, g_cw, lh_tab, title);
	y += lh_tab;
	put(dc, f_tab, tx, 8, y, g_cw, lh_tab, g_status);
	y += lh_tab + lh_tab / 2;
	for (i = 0; i < col_n; i++)
		put(dc, f_tabb, NAVY, col_x[i], y, i + 1 < col_n ? col_x[i + 1] : g_cw, lh_tab,
		    col_head[col_i[i]]);
	y += lh_tab;
	for (t = 0; t < T_N; t++) {
		for (i = 0; i < col_n; i++)
			put(dc, f_tab, tx, col_x[i], y, i + 1 < col_n ? col_x[i + 1] : g_cw, lh_tab,
			    cell[t][col_i[i]]);
		y += lh_tab;
	}
	y += lh_tab / 2;
	if (!f_note)
		return;
	for (i = 0; i < NNOTE; i++) {
		put(dc, f_note, tx, 8, y, g_cw, lh_note, g_note[i]);
		y += lh_note;
	}
	SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));     /* f_note free to go */
}

static void render(HDC dc)
{
	if (g_compact)
		paint_compact(dc);
	else if (g_view == 0)
		paint_live(dc);
	else
		paint_tests(dc);
}

static void paint(void)
{
	PAINTSTRUCT ps;
	HDC dc = BeginPaint(g_wnd, &ps);
	if (g_buf == 1 && g_mdc) {
		if (!g_valid) {
			RECT rc;
			rc.left = rc.top = 0;
			rc.right = g_mw;
			rc.bottom = g_mh;
			FillRect(g_mdc, &rc, (HBRUSH) (COLOR_WINDOW + 1));
			render(g_mdc);
			g_valid = 1;
		}
		g_ndirty = 0;
		BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left,
		       ps.rcPaint.bottom - ps.rcPaint.top, g_mdc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
	} else {
		render(dc);
		font_off(dc);
	}
	EndPaint(g_wnd, &ps);
}

static int line_changed(const Line *l)
{
	int i;
	if (!l->laid)
		return 1;
	for (i = 0; i < l->nf; i++)
		if (!seq(l->fv[i], l->fs[i]))
			return 1;
	return 0;
}

/* each second: only the fields that changed are drawn again, and with the
   copy in memory, put on the screen with one call */
static void update_shown(void)
{
	HDC dc;
	int i, sel = -1;
	if (IsIconic(g_wnd) || (!g_compact && g_view != 0))
		return;
	if (g_buf == 1 && (!g_mdc || !g_valid))
		return;                 /* the next WM_PAINT draws it all */
	dc = g_buf == 1 ? g_mdc : GetDC(g_wnd);
	if (g_compact) {
		SelectObject(dc, f_cmp);
		dc_colours(dc, NAVY);
		for (i = 0; i < 2; i++)
			line_update(dc, &CL[i], g_cw);
	} else {
		COLORREF tx = GetSysColor(COLOR_WINDOWTEXT);
		for (i = 0; i < L_N; i++) {
			int k = LD[i].kind == 'H';
			if (LD[i].kind == 'S' || !line_changed(&LN[i]))
				continue;
			if (sel != k) {
				SelectObject(dc, *LN[i].font);
				dc_colours(dc, k ? NAVY : tx);
				sel = k;
			}
			if (k && !LN[i].laid) {
				line_layout(dc, &LN[i], 8, g_cw, cell_w(dc));
				line_draw(dc, &LN[i], g_cw);
			} else
				line_update(dc, &LN[i], g_cw);
		}
	}
	if (g_buf == 1)
		buf_flush();
	else {
		font_off(dc);
		ReleaseDC(g_wnd, dc);
	}
}

/* /calib: what each kind of drawing call costs here, into the log, with the
   mouse pointer where it is, far from the drawing, on it, and hidden */
static int opt_calib;
static void calib_pass(const char *how);
static void calib(void)
{
	POINT p;
	RECT wr;
	GetCursorPos(&p);
	calib_pass("the pointer where it was");
	SetCursorPos(GetSystemMetrics(SM_CXSCREEN) - 1, 0);
	calib_pass("the pointer at the top right corner");
	GetWindowRect(g_wnd, &wr);
	SetCursorPos(wr.left + 40, wr.bottom - 14);
	calib_pass("the pointer on the drawing");
	ShowCursor(FALSE);
	calib_pass("the pointer hidden");
	ShowCursor(TRUE);
	SetCursorPos(p.x, p.y);
	log_flush();
}

static void calib_pass(const char *how)
{
	static const char s80[] = "0123456789 the quick brown fox jumps over the lazy dog 0123456789 ABCDEFGHIJKLM";
	char line[200];
	int k, n, dx[80];
	HDC dc, mdc;
	HBITMAP bm, ob;
	RECT rc;
	u32 q, t[16];
	const char *name[16] = {
		"GetDC and ReleaseDC", "SelectObject (a font)", "SetTextColor", "ExtTextOut 1 char",
		"ExtTextOut 10 chars", "ExtTextOut 80 chars", "ExtTextOut 80 chars with spacing",
		"TextOut 10 chars", "PatBlt 60x14", "GetTextExtentPoint32 10 chars", "SetTextAlign",
		"BitBlt 600x14 from memory", "ExtTextOut 80 chars into memory", "GdiFlush", ""
	};
	memset(t, 0, sizeof t);
	for (k = 0; k < 80; k++)
		dx[k] = 6;
	dc = GetDC(g_wnd);
	mdc = CreateCompatibleDC(dc);
	bm = CreateCompatibleBitmap(dc, 600, 14);
	ob = SelectObject(mdc, bm);
	SelectObject(mdc, f_txt);
	SelectObject(dc, f_txt);
	rc.left = 10;
	rc.right = 610;
	rc.top = g_ch - 20;
	rc.bottom = g_ch - 6;
	for (n = 0; n < 10; n++) {
		HDC d2;
		q = qnow(); d2 = GetDC(g_wnd); ReleaseDC(g_wnd, d2); t[0] += qus(qnow() - q);
		q = qnow(); SelectObject(dc, (n & 1) ? f_bold : f_txt); t[1] += qus(qnow() - q);
		SelectObject(dc, f_txt);
		q = qnow(); SetTextColor(dc, (n & 1) ? RGB(0, 0, 0) : RGB(0, 0, 128)); t[2] += qus(qnow() - q);
		q = qnow(); ExtTextOutA(dc, rc.left, rc.top, ETO_OPAQUE, &rc, s80, 1, NULL); t[3] += qus(qnow() - q);
		q = qnow(); ExtTextOutA(dc, rc.left, rc.top, ETO_OPAQUE, &rc, s80, 10, NULL); t[4] += qus(qnow() - q);
		q = qnow(); ExtTextOutA(dc, rc.left, rc.top, ETO_OPAQUE, &rc, s80, 80, NULL); t[5] += qus(qnow() - q);
		q = qnow(); ExtTextOutA(dc, rc.left, rc.top, ETO_OPAQUE, &rc, s80, 80, dx); t[6] += qus(qnow() - q);
		q = qnow(); TextOutA(dc, rc.left, rc.top, s80, 10); t[7] += qus(qnow() - q);
		q = qnow(); PatBlt(dc, rc.left, rc.top, 60, 14, WHITENESS); t[8] += qus(qnow() - q);
		{
			SIZE z;
			q = qnow(); GetTextExtentPoint32A(dc, s80, 10, &z); t[9] += qus(qnow() - q);
		}
		q = qnow(); SetTextAlign(dc, (n & 1) ? TA_RIGHT : TA_LEFT); t[10] += qus(qnow() - q);
		SetTextAlign(dc, TA_LEFT);
		q = qnow(); BitBlt(dc, rc.left, rc.top, 600, 14, mdc, 0, 0, SRCCOPY); t[11] += qus(qnow() - q);
		q = qnow(); ExtTextOutA(mdc, 0, 0, ETO_OPAQUE, NULL, s80, 80, NULL); t[12] += qus(qnow() - q);
		q = qnow(); GdiFlush(); t[13] += qus(qnow() - q);
	}
	SelectObject(mdc, ob);
	DeleteObject(bm);
	DeleteDC(mdc);
	font_off(dc);
	ReleaseDC(g_wnd, dc);
	repaint_all();
	fmt(line, sizeof line, "# calib, %s:\r\n", how);
	log_put(line);
	for (k = 0; name[k][0]; k++) {
		fmt(line, sizeof line, "# calib: %-34s %7u us a call\r\n", name[k], t[k] / 10);
		log_put(line);
	}
}

/* the tick: once a second */
static void tick(void)
{
	u32 q0 = qnow(), c, q1, q2, q3, q4, q5, q6;
	Sample *a = &g_s[g_cur], *b;
	int log_due;
	g_cur ^= 1;
	b = &g_s[g_cur];
	dbg_chars = dbg_calls = 0;
	log_due = opt_every && qus(q0 - g_lbase.q) / 1000 + 500 >= opt_every * 1000;
	/* Windows' slower figures with each log line, or every 5 s */
	stats_read(log_due || ((!opt_every || opt_every > 5) && g_ticks % 5 == 0));
	memcpy(b->st, st_val, sizeof b->st);
	q1 = qnow();
	b->ms.dwLength = sizeof b->ms;
	GlobalMemoryStatus(&b->ms);
	q2 = qnow();
	b->tick = GetTickCount();
	b->pp = pp_on;
	if (pp_on)
		pp_read(b->v);
	b->q = q3 = qnow();
	if (++g_ticks % 5 == 0)
		mode_read();
	q4 = q5 = qnow();
	if ((g_compact || g_view == 0) && g_ticks % opt_refresh == 0) {
		/* the view's figures are over its own interval (/refresh:N) */
		build_live(opt_refresh > 1 ? &g_dbase : a, b);
		q4 = qnow();
		update_shown();
		q5 = qnow();
		memcpy(&g_dbase, b, sizeof g_dbase);
	}
	g_lcpu_sum += st_val[S_CPU];
	g_lcpu_n++;
	if (log_due) {
		log_line(&g_lbase, b);
		memcpy(&g_lbase, b, sizeof g_lbase);
		g_lcpu_sum = g_lcpu_n = 0;
		g_lcost_us = 0;
	}
	log_write_due();
	q6 = qnow();
	if (opt_debug) {
		char line[200];
		fmt(line, sizeof line, "# tick %u: stats %u us, memory %u, port %u, text %u, "
		    "drawing %u (%u characters in %u calls), log %u\r\n", g_ticks, qus(q1 - q0),
		    qus(q2 - q1), qus(q3 - q2), qus(q4 - q3), qus(q5 - q4), dbg_chars, dbg_calls,
		    qus(q6 - q5));
		log_put(line);
	}
	c = qus(qnow() - q0);
	g_cost_us[g_cost_i] = c;
	g_cost_i = (g_cost_i + 1) % 10;
	g_lcost_us += c;
}

/* a new start for the live figures after the tests */
static void live_restart(void)
{
	take(&g_s[g_cur], 0, 1);
	memcpy(&g_lbase, &g_s[g_cur], sizeof g_lbase);
	memcpy(&g_dbase, &g_s[g_cur], sizeof g_dbase);
	g_lcpu_sum = g_lcpu_n = 0;
	g_lcost_us = 0;
	build_live(&g_s[g_cur], &g_s[g_cur]);
}

static void view_set(int v)
{
	g_view = v;
	if (!g_compact) {
		if (v == 0)
			build_live(&g_s[g_cur ^ 1], &g_s[g_cur]);
		layout();
		repaint_all();
	}
}

static void top_set(int on)
{
	opt_top = on;
	SendMessageA(g_btn[5], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
	SetWindowPos(g_wnd, on || g_compact ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
		     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void compact_set(int on)
{
	int i;
	if (on == g_compact)
		return;
	if (on) {
		RECT wr;
		GetWindowRect(g_wnd, &g_full_rect);
		g_compact = 1;
		for (i = 0; i < N_BTN; i++)
			ShowWindow(g_btn[i], SW_HIDE);
		SetWindowTextA(g_wnd, APP_NAME " (double-click: full view)");
		build_live(&g_s[g_cur ^ 1], &g_s[g_cur]);
		wr.left = 0;
		wr.top = 0;
		wr.right = 300;
		wr.bottom = 44;
		AdjustWindowRectEx(&wr, WS_OVERLAPPEDWINDOW, FALSE, 0);
		SetWindowPos(g_wnd, HWND_TOPMOST, g_full_rect.left, g_full_rect.top,
			     wr.right - wr.left, wr.bottom - wr.top, SWP_NOACTIVATE);
	} else {
		g_compact = 0;
		for (i = 0; i < N_BTN; i++)
			ShowWindow(g_btn[i], SW_SHOW);
		SetWindowTextA(g_wnd, APP_NAME " " APP_VERSION ": Windows 95 on tiny386");
		SetWindowPos(g_wnd, opt_top ? HWND_TOPMOST : HWND_NOTOPMOST, g_full_rect.left,
			     g_full_rect.top, g_full_rect.right - g_full_rect.left,
			     g_full_rect.bottom - g_full_rect.top, SWP_NOACTIVATE);
	}
	layout();
	repaint_all();
}

static void buttons_make(HWND w)
{
	int i, x = 4;
	for (i = 0; i < N_BTN; i++) {
		int box = BTN[i].id == ID_TOP || BTN[i].id == ID_WCPU;
		g_btn[i] = CreateWindowExA(0, "BUTTON", BTN[i].text, WS_CHILD | WS_VISIBLE |
					   (box ? BS_AUTOCHECKBOX : BS_PUSHBUTTON),
					   x, 3, BTN[i].w, BTN_H, w, (HMENU) (INT_PTR) BTN[i].id, g_inst, NULL);
		SendMessageA(g_btn[i], WM_SETFONT, (WPARAM) GetStockObject(DEFAULT_GUI_FONT), 0);
		x += BTN[i].w + 4;
	}
	g_wcpu_btn = g_btn[6];
}

static void command(int id)
{
	switch (id) {
	case ID_LIVE:
		compact_set(0);
		view_set(0);
		break;
	case ID_TESTS:
		compact_set(0);
		view_set(1);
		break;
	case ID_RUN:
		bench_start();
		break;
	case ID_STOP:
		if (B.on)
			B.stop = 1;
		break;
	case ID_LOGNOW:
		if (B.on)
			break;
		build_live(&g_s[g_cur ^ 1], &g_s[g_cur]);
		log_view();
		log_flush();            /* Log now writes at once, with what waited */
		build_live(&g_s[g_cur ^ 1], &g_s[g_cur]);
		update_shown();
		break;
	case ID_COMPACT:
		compact_set(!g_compact);
		break;
	case ID_TOP:
		top_set(SendMessageA(g_btn[5], BM_GETCHECK, 0, 0) == BST_CHECKED);
		break;
	case ID_WCPU:
		/* the box (a click) or F7 (a key): a key turns it over */
		if (B.on)
			break;
		wcpu_set(SendMessageA(g_wcpu_btn, BM_GETCHECK, 0, 0) == BST_CHECKED);
		{
			char line[120];
			fmt(line, sizeof line, "# Windows' own processor figure %s\r\n", wcpu_on ?
			    "on (Windows then never halts)" : "off");
			log_put(line);
		}
		build_live(&g_s[g_cur ^ 1], &g_s[g_cur]);
		update_shown();
		break;
	case ID_WCPU + 100:
		SendMessageA(g_wcpu_btn, BM_SETCHECK, wcpu_on ? BST_UNCHECKED : BST_CHECKED, 0);
		command(ID_WCPU);
		return;
	}
	if (id != ID_TOP && id != ID_WCPU && GetFocus() != g_wnd && !g_compact)
		SetFocus(g_wnd);
}

/* the last line, and everything waiting, to the log; once */
static void the_end(const char *why)
{
	static int done;
	char line[160], when[24];
	if (done)
		return;
	done = 1;
	log_hold = 0;
	stats_stop();
	now_text(when, sizeof when, 1);
	fmt(line, sizeof line, "# %s stopped %s%s%s", APP_NAME, when, why ? " (" : "", why ? why : "");
	if (why)
		fmt(line + slen(line), sizeof line - slen(line), ")");
	fmt(line + slen(line), sizeof line - slen(line), "\r\n");
	log_put(line);
	log_flush();
}

static LRESULT CALLBACK main_proc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
	switch (m) {
	case WM_CREATE:
		g_wnd = w;
		buttons_make(w);
		return 0;
	case WM_SIZE:
		if (wp != SIZE_MINIMIZED) {
			layout();
			repaint_all();
		}
		return 0;
	case WM_PAINT:
		paint();
		return 0;
	case WM_ERASEBKGND:
		if (g_buf == 1)
			return 1;               /* the copy covers it all */
		break;
	case WM_TIMER:
		if (wp == 1 && opt_calib == 1) {
			opt_calib = 2;
			calib();
		}
		if (wp == 1 && !B.on)
			tick();
		else if (wp == 2) {
			KillTimer(w, 2);
			bench_start();
		}
		return 0;
	case WM_COMMAND:
		command(LOWORD(wp));
		return 0;
	case WM_LBUTTONDBLCLK:
		if (g_compact)
			compact_set(0);
		return 0;
	case WM_DISPLAYCHANGE:
		mode_read();
		g_mw = -1;              /* a copy in memory of the new depth */
		layout();
		repaint_all();
		return 0;
	case WM_GETMINMAXINFO:
		if (!g_compact) {
			((MINMAXINFO *) lp)->ptMinTrackSize.x = 300;
			((MINMAXINFO *) lp)->ptMinTrackSize.y = 160;
		}
		return 0;
	case WM_CLOSE:
		if (B.on) {
			B.stop = 1;
			bench_free();
			B.on = 0;
			log_hold = 0;
			SetCursorPos(B.ptr_saved.x, B.ptr_saved.y);
		}
		DestroyWindow(w);
		return 0;
	case WM_ENDSESSION:
		/* Windows shuts down with this window open: the waiting lines
		   are written now, as the process may not run on */
		if (wp)
			the_end("Windows shutting down");
		return 0;
	case WM_DESTROY:
		KillTimer(w, 1);
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(w, m, wp, lp);
}

/* ==== the start ==== */
static void args(const char *s)
{
	const char *r;
	/* past the program's own name */
	if (*s == '"') {
		s++;
		while (*s && *s != '"')
			s++;
		if (*s)
			s++;
	} else
		while (*s && *s != ' ' && *s != '\t')
			s++;
	for (;;) {
		while (*s == ' ' || *s == '\t')
			s++;
		if (!*s)
			break;
		if (*s == '/' || *s == '-') {
			s++;
			if (flag(s, "bench") || flag(s, "b"))
				opt_bench = 1;
			else if (flag(s, "quit") || flag(s, "q"))
				opt_quit = 1;
			else if (flag(s, "compact"))
				opt_compact = 1;
			else if (flag(s, "top"))
				opt_top = 1;
			else if (flag(s, "nolog"))
				opt_every = 0;
			else if (flag(s, "wcpu"))
				opt_wcpu = 1;
			else if (flag(s, "nowcpu"))
				opt_wcpu = 0;
			else if (flag(s, "debug"))
				opt_debug = 1;
			else if (flag(s, "calib"))
				opt_calib = 1;
			else if (flag(s, "direct"))
				opt_draw = 0;
			else if (flag(s, "buffer"))
				opt_draw = 1;
			else if (flag(s, "nostats"))
				opt_nostats = 1;
			else if (flag(s, "noport"))
				opt_noport = 1;
			else if (word(s, "every:", &r))
				opt_every = atou(r);
			else if (word(s, "write:", &r)) {
				opt_write = atou(r);
				if (opt_write > 3600)
					opt_write = 3600;
			}
			else if (word(s, "refresh:", &r)) {
				opt_refresh = atou(r);
				if (opt_refresh < 1)
					opt_refresh = 1;
				if (opt_refresh > 60)
					opt_refresh = 60;
			}
			else if (word(s, "secs:", &r)) {
				opt_secs = atou(r);
				if (opt_secs < 1)
					opt_secs = 1;
				if (opt_secs > 60)
					opt_secs = 60;
			} else if (word(s, "log:", &r)) {
				char *d = log_path;
				while (*r && *r != ' ' && *r != '\t' && d < log_path + MAX_PATH - 1)
					*d++ = *r++;
				*d = 0;
			} else if (*s == '?' || flag(s, "help")) {
				MessageBoxA(NULL,
					    "T386MON [/bench] [/quit] [/secs:N] [/every:N] [/write:N] [/nolog] "
					    "[/log:PATH] [/refresh:N] [/compact] [/top] [/wcpu] [/nowcpu]\n\n"
					    "/bench  run all the tests 3 s after the start\n"
					    "/quit  close after the tests\n"
					    "/secs:N  seconds a test (4)\n"
					    "/every:N  a log line every N seconds (5; 0 none)\n"
					    "/write:N  the lines written to the disk every N seconds "
					    "(60; 0 at once; never during the tests)\n"
					    "/nolog  no log lines every few seconds\n"
					    "/log:PATH  the log file (C:\\T386MON.LOG)\n"
					    "/refresh:N  the live view drawn every N seconds (1)\n"
					    "/compact  start in the small view\n"
					    "/top  stay on top of other windows\n"
					    "/wcpu, /nowcpu  Windows' own processor figure on or off "
					    "(off on tiny386: while it is on, Windows never halts)\n\n"
					    "Keys: F2 live view, F3 tests, F4 compact, F5 run all tests, "
					    "F6 log now, F7 Windows' processor figure, Esc stops the tests.",
					    APP_NAME " " APP_VERSION, MB_OK | MB_ICONINFORMATION);
				ExitProcess(0);
			}
		}
		while (*s && *s != ' ' && *s != '\t')
			s++;
	}
}

static const ACCEL accels[] = {
	{ FVIRTKEY, VK_F2, ID_LIVE }, { FVIRTKEY, VK_F3, ID_TESTS }, { FVIRTKEY, VK_F4, ID_COMPACT },
	{ FVIRTKEY, VK_F5, ID_RUN }, { FVIRTKEY, VK_F6, ID_LOGNOW }, { FVIRTKEY, VK_F7, ID_WCPU + 100 },
	{ FVIRTKEY, VK_ESCAPE, ID_STOP },
	{ FVIRTKEY | FALT, 'L', ID_LIVE }, { FVIRTKEY | FALT, 'T', ID_TESTS },
	{ FVIRTKEY | FALT, 'R', ID_RUN }, { FVIRTKEY | FALT, 'N', ID_LOGNOW },
	{ FVIRTKEY | FALT, 'C', ID_COMPACT }, { FVIRTKEY | FALT, 'W', ID_WCPU + 100 },
};

void entry(void)
{
	WNDCLASSEXA wc;
	MSG msg;
	HACCEL acc;
	RECT wa;
	int w, h;
	u32 i;

	g_inst = GetModuleHandleA(NULL);
	args(GetCommandLineA());
	clock_init();
	if (!opt_noport)
		pp_detect();
	else
		scopy(pp_why, "not looked for (/noport)", sizeof pp_why);
	if (!opt_nostats)
		stats_start();
	wcpu_set(opt_wcpu < 0 ? !pp_on : opt_wcpu);
	mode_read();
	stats_read(2);
	take(&g_s[0], 0, 1);
	memcpy(&g_s[1], &g_s[0], sizeof g_s[0]);
	memcpy(&g_lbase, &g_s[0], sizeof g_lbase);
	memcpy(&g_dbase, &g_s[0], sizeof g_dbase);
	for (i = 0; i < L_N; i++)
		LN[i].tmpl = T_STR;
	build_live(&g_s[0], &g_s[0]);
	log_start();

	memset(&wc, 0, sizeof wc);
	wc.cbSize = sizeof wc;
	wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
	wc.lpfnWndProc = main_proc;
	wc.hInstance = g_inst;
	wc.hIcon = LoadIconA(g_inst, MAKEINTRESOURCEA(1));
	wc.hIconSm = (HICON) LoadImageA(g_inst, MAKEINTRESOURCEA(1), IMAGE_ICON, 16, 16, 0);
	wc.hCursor = LoadCursorA(NULL, (LPCSTR) IDC_ARROW);
	wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
	wc.lpszClassName = "T386MON";
	RegisterClassExA(&wc);
	wc.style = 0;
	wc.lpfnWndProc = stage_proc;
	wc.hIcon = NULL;
	wc.hIconSm = NULL;
	wc.hbrBackground = NULL;
	wc.lpszClassName = "T386MONStage";
	RegisterClassExA(&wc);

	if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
		wa.left = wa.top = 0;
		wa.right = GetSystemMetrics(SM_CXSCREEN);
		wa.bottom = GetSystemMetrics(SM_CYSCREEN);
	}
	w = wa.right - wa.left;
	h = wa.bottom - wa.top;
	if (w > 680)
		w = 680;
	if (h > 452)
		h = 452;
	g_full_rect.left = wa.left;
	g_full_rect.top = wa.top;
	g_full_rect.right = wa.left + w;
	g_full_rect.bottom = wa.top + h;
	CreateWindowExA(0, "T386MON", APP_NAME " " APP_VERSION ": Windows 95 on tiny386",
			WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, wa.left, wa.top, w, h, NULL, NULL, g_inst,
			NULL);
	if (!g_wnd)
		ExitProcess(1);
	wcpu_set(wcpu_on);
	if (!stat_on || !stat_ok[S_CPU])
		EnableWindow(g_wcpu_btn, FALSE);
	ShowWindow(g_wnd, SW_SHOWNORMAL);
	draw_pick();
	layout();
	repaint_all();
	{
		char line[200];
		fmt(line, sizeof line, "# drawing: a short text on the screen takes %T ms, a copy of "
		    "the same size from memory %T ms (the least of %u each): %s\r\n", cal_text_us / 100,
		    cal_copy_us / 100, CAL_N,
		    g_buf == 1 ? "drawn in memory, then copied once a second" : "drawn directly");
		log_put(line);
		log_flush();            /* the start's lines at once; then every opt_write s */
	}
	UpdateWindow(g_wnd);
	if (opt_top)
		top_set(1);
	if (opt_compact)
		compact_set(1);
	acc = CreateAcceleratorTableA((LPACCEL) accels, sizeof accels / sizeof accels[0]);
	SetTimer(g_wnd, 1, 1000, NULL);
	if (opt_bench)
		SetTimer(g_wnd, 2, 3000, NULL);

	for (;;) {
		if (B.on) {
			while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
				if (msg.message == WM_QUIT)
					goto done;
				if (!acc || !TranslateAcceleratorA(g_wnd, acc, &msg)) {
					TranslateMessage(&msg);
					DispatchMessageA(&msg);
				}
			}
			if (B.on)
				bench_step();
		} else {
			if (GetMessageA(&msg, NULL, 0, 0) <= 0)
				break;
			if (!acc || !TranslateAcceleratorA(g_wnd, acc, &msg)) {
				TranslateMessage(&msg);
				DispatchMessageA(&msg);
			}
		}
	}
done:
	the_end(NULL);
	if (acc)
		DestroyAcceleratorTable(acc);
	ExitProcess(0);
}
