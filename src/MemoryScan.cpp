//this implementation is based on Windows part of memdig: https://github.com/skeeto/memdig
//gradually remove unnecessary stuff

#include "pch.h"

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>


#define REGION_ITERATOR_DONE    (1UL << 0)
#define REGION_ITERATOR_READ    (1UL << 1)
#define REGION_ITERATOR_WRITE   (1UL << 2)
#define REGION_ITERATOR_EXECUTE (1UL << 3)

#define PROCESS_ITERATOR_DONE   (1UL << 0)

#if 0 // (missing typedefs)
struct process_iterator;
int   process_iterator_init(struct process_iterator *);
int   process_iterator_next(struct process_iterator *);
int   process_iterator_done(struct process_iterator *);
void  process_iterator_destroy(struct process_iterator *);

struct region_iterator;
int   region_iterator_init(struct region_iterator *, os_handle);
int   region_iterator_next(struct region_iterator *);
int   region_iterator_done(struct region_iterator *);
void *region_iterator_memory(struct region_iterator *);
void  region_iterator_destroy(struct region_iterator *);

int         os_write_memory(os_handle, uintptr_t, void *, size_t);
void        os_sleep(double);
os_handle   os_process_open(os_pid);
void        os_process_close(os_handle);
const char *os_last_error(void);

void os_thread_start(struct os_thread *, struct memdig *);
void os_thread_join(struct os_thread *);
void os_mutex_lock(struct os_thread *);
void os_mutex_unlock(struct os_thread *);
#endif

struct memdig;
static void memdig_locker(struct memdig *);


#include <process.h>
#include <windows.h>
#include <tlhelp32.h>

typedef HANDLE os_handle;
typedef DWORD os_pid;

#ifdef PROCESSENTRY32
#undef PROCESSENTRY32
#undef Process32First
#undef Process32Next
#endif

struct process_iterator {
	os_pid pid;
	char *name;
	unsigned long flags;

	// private
	HANDLE snapshot;
	char buf[MAX_PATH];
	PROCESSENTRY32 entry;
};

static int
process_iterator_next(struct process_iterator *i)
{
	if (Process32Next(i->snapshot, &i->entry)) {
		strcpy(i->name, i->entry.szExeFile);
		i->pid = i->entry.th32ProcessID;
		return !(i->flags = 0);
	}
	else {
		return !(i->flags = PROCESS_ITERATOR_DONE);
	}
}

static int
process_iterator_init(struct process_iterator *i)
{
	std::memset(&i->entry, 0, sizeof(i->entry));
	i->entry.dwSize = sizeof(i->entry);
	i->flags = PROCESS_ITERATOR_DONE;
	i->name = i->buf;
	i->snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	PROCESSENTRY32 entry[1] = { {sizeof(entry)} };
	if (Process32First(i->snapshot, entry))
		return process_iterator_next(i);
	else
		return 0;
}

static void
process_iterator_destroy(struct process_iterator *i)
{
	CloseHandle(i->snapshot);
}

struct region_iterator {
	uintptr_t base;
	uintptr_t actualBase;
	size_t size;
	unsigned long flags;

	// private
	void *p;
	HANDLE process;
	void *buf;
	size_t bufsize;
};

static int
region_iterator_next(struct region_iterator *i)
{
	MEMORY_BASIC_INFORMATION info[1];
	for (;;) {
		if (VirtualQueryEx(i->process, i->p, info, sizeof(info))) {
			i->flags = 0;
			i->p = (char *)i->p + info->RegionSize;
			if (info->State == MEM_COMMIT) {
				i->size = info->RegionSize;
				i->base = (uintptr_t)info->AllocationBase;
				i->actualBase = (uintptr_t)info->BaseAddress;
				switch (info->AllocationProtect) {
				case PAGE_EXECUTE:
					i->flags |= REGION_ITERATOR_EXECUTE;
					break;
				case PAGE_EXECUTE_READ:
					i->flags |= REGION_ITERATOR_READ;
					i->flags |= REGION_ITERATOR_EXECUTE;
					break;
				case PAGE_EXECUTE_READWRITE:
					i->flags |= REGION_ITERATOR_READ;
					i->flags |= REGION_ITERATOR_WRITE;
					i->flags |= REGION_ITERATOR_EXECUTE;
					break;
				case PAGE_EXECUTE_WRITECOPY:
					i->flags |= REGION_ITERATOR_READ;
					i->flags |= REGION_ITERATOR_WRITE;
					i->flags |= REGION_ITERATOR_EXECUTE;
					break;
				case PAGE_READWRITE:
					i->flags |= REGION_ITERATOR_READ;
					i->flags |= REGION_ITERATOR_WRITE;
					break;
				case PAGE_READONLY:
					i->flags |= REGION_ITERATOR_READ;
					break;
				case PAGE_WRITECOPY:
					i->flags |= REGION_ITERATOR_READ;
					i->flags |= REGION_ITERATOR_WRITE;
					break;
				}
				break;
			}
		}
		else {
			i->flags = REGION_ITERATOR_DONE;
			break;
		}
	}
	return !(i->flags & REGION_ITERATOR_DONE);
}

static int
region_iterator_init(struct region_iterator *i, os_handle process)
{
	region_iterator null;
	std::memset(&null, 0, sizeof(null));
	null.process = process;
	*i = null;
	return region_iterator_next(i);
}

static const void *
region_iterator_memory(struct region_iterator *i)
{
	if (i->bufsize < i->size) {
		free(i->buf);
		i->bufsize = i->size;
		i->buf = malloc(i->bufsize);
	}
	SIZE_T actual;
	void *base = (void *)i->actualBase;
	const BOOL result = ReadProcessMemory(i->process, base, i->buf, i->size, &actual);

	if (actual > 0 && actual < i->size)
		i->size = actual;
	else if (!result)
		return nullptr;
 
	return i->buf;
}

static void
region_iterator_destroy(struct region_iterator *i)
{
	free(i->buf);
	i->buf = NULL;
}

static int
os_write_memory(os_handle target, uintptr_t base, void *buf, size_t bufsize)
{
	SIZE_T actual;
	return WriteProcessMemory(target, (void *)base, buf, bufsize, &actual)
		&& actual == bufsize;
}

static void
os_sleep(double seconds)
{
	DWORD ms = (DWORD)(seconds * 1000);
	Sleep(ms);
}

static os_handle
os_process_open(os_pid id)
{
	DWORD access = PROCESS_VM_READ |
		/*PROCESS_VM_WRITE |
		PROCESS_VM_OPERATION |*/
		PROCESS_QUERY_INFORMATION;
	HANDLE process = OpenProcess(access, 0, id);
	return process;
}

static void
os_process_close(os_handle h)
{
	CloseHandle(h);
}

static char error_buffer[4096];

static const char *
os_last_error(void)
{
	DWORD e = GetLastError();
	SetLastError(0);
	DWORD lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT);
	DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
	FormatMessageA(flags, 0, e, lang, error_buffer, sizeof(error_buffer), 0);
	for (char *p = error_buffer; *p; p++)
		if (*p == '\n')
			*p = 0;
	return error_buffer;
}

struct os_thread {
	HANDLE thread;
	CRITICAL_SECTION mutex;
};

static unsigned __stdcall
os_stub(void *arg)
{
	memdig_locker((memdig*)arg);
	_endthreadex(0);
	return 0;
}

static void
os_thread_start(struct os_thread *t, struct memdig *m)
{
	InitializeCriticalSection(&t->mutex);
	t->thread = (HANDLE)_beginthreadex(0, 0, os_stub, m, 0, 0);
}

static void
os_thread_join(struct os_thread *t)
{
	WaitForSingleObject(t->thread, INFINITE);
	DeleteCriticalSection(&t->mutex);
}

static void
os_mutex_lock(struct os_thread *t)
{
	EnterCriticalSection(&t->mutex);
}

static void
os_mutex_unlock(struct os_thread *t)
{
	LeaveCriticalSection(&t->mutex);
}

static int
process_iterator_done(struct process_iterator *i)
{
	return !!(i->flags & PROCESS_ITERATOR_DONE);
}

static int
region_iterator_done(struct region_iterator *i)
{
	return !!(i->flags & REGION_ITERATOR_DONE);
}

static enum loglevel {
	LOGLEVEL_DEBUG = -2,
	LOGLEVEL_INFO = -1,
	LOGLEVEL_WARNING = 0,
	LOGLEVEL_ERROR = 1,
} loglevel;

#define FATAL(...)                                      \
    do {                                                \
        fprintf(stderr, "memdig: " __VA_ARGS__);        \
        exit(-1);                                       \
    } while (0)

#define LOG_ERROR(...)                                  \
    do {                                                \
        if (loglevel <= LOGLEVEL_ERROR)                 \
            fprintf(stderr, "error: " __VA_ARGS__);     \
        goto fail;                                      \
    } while (0)

#define LOG_WARNING(...)                                \
    do {                                                \
        if (loglevel <= LOGLEVEL_WARNING)               \
            fprintf(stderr, "warning: " __VA_ARGS__);   \
    } while (0)

#define LOG_INFO(...)                                   \
    do {                                                \
        if (loglevel <= LOGLEVEL_INFO)                  \
            fprintf(stderr, "info: " __VA_ARGS__);      \
    } while (0)

#define LOG_DEBUG(...)                                  \
    do {                                                \
        if (loglevel <= LOGLEVEL_DEBUG)                 \
            fprintf(stderr, "debug: " __VA_ARGS__);     \
    } while (0)

enum value_type {
	VALUE_S8,
	VALUE_U8,
	VALUE_S16,
	VALUE_U16,
	VALUE_S32,
	VALUE_U32,
	VALUE_S64,
	VALUE_U64,
	VALUE_F32,
	VALUE_F64,
};

struct value {
	enum value_type type;
	union {
		int8_t s8;
		uint8_t u8;
		int16_t s16;
		uint16_t u16;
		int32_t s32;
		uint32_t u32;
		int64_t s64;
		uint64_t u64;
		float f32;
		double f64;
	} value;
};

#define VALUE_SIZE(v) ("bbcceeiiei"[(v).type] - 'a')

enum value_parse_result {
	VALUE_PARSE_SUCCESS,
	VALUE_PARSE_OVERFLOW,
	VALUE_PARSE_INVALID,
};

static enum value_parse_result
value_parse(struct value *v, const char *arg)
{
	int base = 10;
	int is_signed = 1;
	int is_integer = 1;
	size_t len = strlen(arg);
	const char *suffix = arg + len;
	char digits[] = "0123456789abcdef";

	/* Check prefix to determine base and sign. */
	if (arg[0] == '0' && arg[1] == 'x') {
		is_signed = 0;
		base = 16;
		arg += 2;
		len -= 2;
	}
	else if (arg[0] == '0') {
		is_signed = 0;
		base = 8;
		arg += 1;
		len -= 1;
	}
	digits[base] = 0;

	/* Find the suffix. */
	while (suffix > arg && !strchr(digits, suffix[-1]))
		suffix--;

	/* Check for an integer. */
	for (const char *p = arg; p < suffix; p++) {
		if (p == arg && *p == '-')
			p++;
		if (!strchr(digits, *p))
			is_integer = 0;
	}
	if (is_integer && suffix[0] == 'u') {
		is_signed = 0;
		suffix++;
	}

	/* Parse the in-between. */
	if (is_integer) {
		errno = 0;
		if (is_signed) {
			intmax_t s = strtoimax(arg, 0, base);
			if (errno)
				return VALUE_PARSE_OVERFLOW;
			switch (suffix[0]) {
			case 'o':
				v->type = VALUE_S8;
				if (s > INT8_MAX || s < INT8_MIN)
					return VALUE_PARSE_OVERFLOW;
				v->value.s8 = (int8_t)s;
				return VALUE_PARSE_SUCCESS;
			case 'h':
				v->type = VALUE_S16;
				if (s > INT16_MAX || s < INT16_MIN)
					return VALUE_PARSE_OVERFLOW;
				v->value.s16 = (int16_t)s;
				return VALUE_PARSE_SUCCESS;
			case 0:
				v->type = VALUE_S32;
				if (s > INT32_MAX || s < INT32_MIN)
					return VALUE_PARSE_OVERFLOW;
				v->value.s32 = (int32_t)s;
				return VALUE_PARSE_SUCCESS;
			case 'q':
				v->type = VALUE_S64;
				if (s > INT64_MAX || s < INT64_MIN)
					return VALUE_PARSE_OVERFLOW;
				v->value.s64 = (int64_t)s;
				return VALUE_PARSE_SUCCESS;
			}
		}
		else {
			uintmax_t u = strtoumax(arg, 0, base);
			if (errno)
				return VALUE_PARSE_OVERFLOW;
			switch (suffix[0]) {
			case 'o':
				v->type = VALUE_U8;
				if (u > UINT8_MAX)
					return VALUE_PARSE_OVERFLOW;
				v->value.u8 = (uint8_t)u;
				return VALUE_PARSE_SUCCESS;
			case 'h':
				v->type = VALUE_U16;
				if (u > UINT16_MAX)
					return VALUE_PARSE_OVERFLOW;
				v->value.u16 = (uint16_t)u;
				return VALUE_PARSE_SUCCESS;
			case 0:
				v->type = VALUE_U32;
				if (u > UINT32_MAX)
					return VALUE_PARSE_OVERFLOW;
				v->value.u32 = (uint32_t)u;
				return VALUE_PARSE_SUCCESS;
			case 'q':
				v->type = VALUE_U64;
				if (u > UINT64_MAX)
					return VALUE_PARSE_OVERFLOW;
				v->value.u64 = (uint64_t)u;
				return VALUE_PARSE_SUCCESS;
			}
		}
	}
	else { /* float */
		errno = 0;
		char *end;
		switch (suffix[0]) {
		case 'f':
			v->type = VALUE_F32;
			v->value.f32 = strtof(arg, &end);
			if (end == suffix)
				return VALUE_PARSE_SUCCESS;
			break;
		case 0:
			v->type = VALUE_F64;
			v->value.f64 = strtod(arg, &end);
			if (end == suffix)
				return VALUE_PARSE_SUCCESS;
			break;
		}
	}
	return VALUE_PARSE_INVALID;
}

static void
value_print(char *buf, size_t n, const struct value *v)
{
	switch (v->type) {
	case VALUE_S8: {
		snprintf(buf, n, "%" PRId8 "o", v->value.s8);
	} return;
	case VALUE_U8: {
		snprintf(buf, n, "%" PRIu8 "uo", v->value.u8);
	} return;
	case VALUE_S16: {
		snprintf(buf, n, "%" PRId16 "h", v->value.s16);
	} return;
	case VALUE_U16: {
		snprintf(buf, n, "%" PRIu16 "uh", v->value.u16);
	} return;
	case VALUE_S32: {
		snprintf(buf, n, "%" PRId32 "", v->value.s32);
	} return;
	case VALUE_U32: {
		snprintf(buf, n, "%" PRIu32 "u", v->value.u32);
	} return;
	case VALUE_S64: {
		snprintf(buf, n, "%" PRId64 "q", v->value.s64);
	} return;
	case VALUE_U64: {
		snprintf(buf, n, "%" PRIu64 "uq", v->value.u64);
	} return;
	case VALUE_F32: {
		snprintf(buf, n, "%.9gf", v->value.f32);
	} return;
	case VALUE_F64: {
		snprintf(buf, n, "%.17g", v->value.f64);
	} return;
	}
	abort();
}

static void
value_read(struct value *v, enum value_type t, const void *p)
{
	v->type = t;
	switch (v->type) {
	case VALUE_S8: {
		memcpy(&v->value.s8, p, sizeof(v->value.s8));
	} return;
	case VALUE_U8: {
		memcpy(&v->value.u8, p, sizeof(v->value.u8));
	} return;
	case VALUE_S16: {
		memcpy(&v->value.s16, p, sizeof(v->value.s16));
	} return;
	case VALUE_U16: {
		memcpy(&v->value.u16, p, sizeof(v->value.u16));
	} return;
	case VALUE_S32: {
		memcpy(&v->value.s32, p, sizeof(v->value.s32));
	} return;
	case VALUE_U32: {
		memcpy(&v->value.u32, p, sizeof(v->value.u32));
	} return;
	case VALUE_S64: {
		memcpy(&v->value.s64, p, sizeof(v->value.s64));
	} return;
	case VALUE_U64: {
		memcpy(&v->value.u64, p, sizeof(v->value.u64));
	} return;
	case VALUE_F32: {
		memcpy(&v->value.f32, p, sizeof(v->value.f32));
	} return;
	case VALUE_F64: {
		memcpy(&v->value.f64, p, sizeof(v->value.f64));
	} return;
	}
	abort();
}

#define VALUE_COMPARE(a, b) ((a) < (b) ? -1 : (b) < (a) ? 1 : 0)

static int
value_compare(const struct value *a, const struct value *b)
{
	if (a->type != b->type)
		return a->type - b->type;
	switch (a->type) {
	case VALUE_S8:
		return VALUE_COMPARE(a->value.s8, b->value.s8);
	case VALUE_U8:
		return VALUE_COMPARE(a->value.u8, b->value.u8);
	case VALUE_S16:
		return VALUE_COMPARE(a->value.s16, b->value.s16);
	case VALUE_U16:
		return VALUE_COMPARE(a->value.u16, b->value.u16);
	case VALUE_S32:
		return VALUE_COMPARE(a->value.s32, b->value.s32);
	case VALUE_U32:
		return VALUE_COMPARE(a->value.u32, b->value.u32);
	case VALUE_S64:
		return VALUE_COMPARE(a->value.s64, b->value.s64);
	case VALUE_U64:
		return VALUE_COMPARE(a->value.u64, b->value.u64);
	case VALUE_F32:
		return VALUE_COMPARE(a->value.f32, b->value.f32);
	case VALUE_F64:
		return VALUE_COMPARE(a->value.f64, b->value.f64);
	}
	abort();
}

struct watchlist {
	os_handle process;
	size_t count;
	size_t size;
	struct wtf {
		uintptr_t addr;
		struct value prev;
	} *list;
};

static void
watchlist_init(struct watchlist *s, os_handle process)
{
	s->process = process;
	s->size = 4096;
	s->count = 0;
	s->list = (watchlist::wtf*)malloc(s->size * sizeof(s->list[0]));
}

static void
watchlist_push(struct watchlist *s, uintptr_t a, const struct value *v)
{
	if (s->count == s->size) {
		s->size *= 2;
		s->list = (watchlist::wtf*)realloc(s->list, s->size * sizeof(s->list[0]));
	}
	s->list[s->count].addr = a;
	s->list[s->count].prev = *v;
	s->count++;
}

static void
watchlist_free(struct watchlist *s)
{
	free(s->list);
	s->list = NULL;
}

static void
watchlist_clear(struct watchlist *s)
{
	s->count = 0;
}

/* Memory scanning */

enum scan_op {
	SCAN_OP_EQ,
	SCAN_OP_LT,
	SCAN_OP_GT,
	SCAN_OP_LTEG,
	SCAN_OP_GTEQ,
};

static int
scan_op_parse(const char *s, enum scan_op *op)
{
	static const struct {
		char name[3];
		enum scan_op op;
	} table[] = {
		{"=", SCAN_OP_EQ},
		{"<", SCAN_OP_LT},
		{">", SCAN_OP_GT},
		{"<=", SCAN_OP_LTEG},
		{">=", SCAN_OP_GTEQ},
	};
	for (unsigned i = 0; i < sizeof(table) / sizeof(table[0]); i++)
		if (strcmp(table[i].name, s) == 0) {
			*op = table[i].op;
			return 1;
		}
	return 0;
}

static int
scan(struct watchlist *wl, struct value *v, enum scan_op op)
{
	unsigned value_size = VALUE_SIZE(*v);
	enum value_type type = v->type;
	watchlist_clear(wl);
	struct region_iterator it[1];
	region_iterator_init(it, wl->process);
	for (; !region_iterator_done(it); region_iterator_next(it)) {
		const char *buf;
		if ((buf = (const char*)region_iterator_memory(it))) {
			size_t count = it->size / value_size;
			for (size_t i = 0; i < count; i++) {
				struct value read;
				value_read(&read, type, buf + i * value_size);
				int cmp = value_compare(&read, v);
				int pass = 0;
				switch (op) {
				case SCAN_OP_EQ:
					pass = cmp == 0;
					break;
				case SCAN_OP_LT:
					pass = cmp < 0;
					break;
				case SCAN_OP_GT:
					pass = cmp > 0;
					break;
				case SCAN_OP_LTEG:
					pass = cmp <= 0;
					break;
				case SCAN_OP_GTEQ:
					pass = cmp >= 0;
					break;
				}
				if (pass) {
					uintptr_t addr = it->base + i * value_size;
					watchlist_push(wl, addr, &read);
				}
			}
		}
		else {
			LOG_DEBUG("memory read failed [0x%016" PRIxPTR "]: %s\n",
				it->base, os_last_error());
		}
	}
	region_iterator_destroy(it);
	return 1;
}

typedef void(*watchlist_visitor)(uintptr_t, const struct value *, void *);

static void
watchlist_visit(struct watchlist *wl, watchlist_visitor f, void *arg)
{
	struct region_iterator it[1];
	region_iterator_init(it, wl->process);
	size_t n = 0;
	for (; !region_iterator_done(it) && n < wl->count; region_iterator_next(it)) {
		const char *buf = NULL;
		uintptr_t base = it->base;
		uintptr_t tail = base + it->size;
		while (n < wl->count && wl->list[n].addr < base)
			n++;
		while (n < wl->count && wl->list[n].addr >= base && wl->list[n].addr < tail) {
			if (!buf)
				buf = (const char*)region_iterator_memory(it);
			uintptr_t addr = wl->list[n].addr;
			enum value_type type = wl->list[n].prev.type;
			size_t offset = wl->list[n].addr - base;
			if (buf) {
				struct value value;
				value_read(&value, type, buf + offset);
				f(addr, &value, arg);
			}
			else {
				f(addr, NULL, arg);
			}
			n++;
		}
	}
	region_iterator_destroy(it);
}

struct narrow_visitor_state {
	struct watchlist *wl;
	struct value target;
	enum scan_op op;
};

static void
narrow_visitor(uintptr_t addr, const struct value *v, void *arg)
{
	char buf[64];
	value_print(buf, sizeof(buf), v);
	struct narrow_visitor_state *s = (narrow_visitor_state *)arg;
	int cmp = value_compare(v, &s->target);
	int pass = 0;
	switch (s->op) {
	case SCAN_OP_EQ:
		pass = cmp == 0;
		break;
	case SCAN_OP_LT:
		pass = cmp < 0;
		break;
	case SCAN_OP_GT:
		pass = cmp > 0;
		break;
	case SCAN_OP_LTEG:
		pass = cmp <= 0;
		break;
	case SCAN_OP_GTEQ:
		pass = cmp >= 0;
		break;
	}
	if (pass)
		watchlist_push(s->wl, addr, v);
}

static int
narrow(struct watchlist *wl, enum scan_op op, struct value *v)
{
	struct watchlist out[1];
	watchlist_init(out, wl->process);
	struct narrow_visitor_state state;
	state.wl = out;
	state.target = *v;
	state.op = op;

	watchlist_visit(wl, narrow_visitor, &state);
	watchlist_free(wl);
	*wl = *out;
	return 1;
}

static void
display_memory_regions(os_handle target)
{
	struct region_iterator it[1];
	region_iterator_init(it, target);
	for (; !region_iterator_done(it); region_iterator_next(it)) {
		char protect[4] = {
			it->flags & REGION_ITERATOR_READ ? 'R' : ' ',
			it->flags & REGION_ITERATOR_WRITE ? 'W' : ' ',
			it->flags & REGION_ITERATOR_EXECUTE ? 'X' : ' ',
		};
		uintptr_t tail = it->base + it->size;
		printf("%s 0x%016" PRIxPTR " 0x%016" PRIxPTR " %10zu bytes\n",
			protect, it->base, tail, it->size);
	}
	region_iterator_destroy(it);
}

enum find_result {
	FIND_SUCCESS,
	FIND_FAILURE,
	FIND_AMBIGUOUS,
};

static os_pid
process_find(const char *pattern, os_pid *pid)
{
	struct process_iterator it[1];
	process_iterator_init(it);
	*pid = 0;
	os_pid target_pid = 0;
	if (pattern[0] == ':')
		target_pid = strtol(pattern + 1, NULL, 10);
	for (; !process_iterator_done(it); process_iterator_next(it)) {
		if (target_pid == it->pid || strstr(it->name, pattern)) {
			if (*pid)
				return FIND_AMBIGUOUS;
			*pid = it->pid;
		}
	}
	process_iterator_destroy(it);
	if (!*pid)
		return FIND_FAILURE;
	return FIND_SUCCESS;
}

/* Command processing */

enum command {
	COMMAND_AMBIGUOUS = -2,
	COMMAND_UNKNOWN = -1,
	COMMAND_ATTACH = 0,
	COMMAND_MEMORY,
	COMMAND_FIND,
	COMMAND_NARROW,
	COMMAND_PUSH,
	COMMAND_LIST,
	COMMAND_SET,
	COMMAND_LOCK,
	COMMAND_WAIT,
	COMMAND_HELP,
	COMMAND_QUIT,
};


struct memdig {
	os_pid id;
	os_handle target;
	struct os_thread thread;
	enum value_type last_type;
	struct watchlist active;
	struct watchlist locked;
	int running;
};

static void
memdig_locker(struct memdig *m)
{
	for (;;) {
		os_sleep(0.1);
		os_mutex_lock(&m->thread);
		if (!m->running) {
			os_mutex_unlock(&m->thread);
			break;
		}
		if (m->target)
			for (size_t i = 0; i < m->locked.count; i++) {
				uintptr_t addr = m->locked.list[i].addr;
				struct value *value = &m->locked.list[i].prev;
				unsigned size = VALUE_SIZE(*value);
				os_write_memory(m->target, addr, &value->value, size);
			}
		os_mutex_unlock(&m->thread);
	}
}

static void
memdig_init(struct memdig *m)
{
	std::memset(m, 0, sizeof(*m));
	m->last_type = VALUE_S32;
	m->running = 1;
	os_thread_start(&m->thread, m);
}

static void
list_visitor(uintptr_t addr, const struct value *v, void *file)
{
	char buf[64] = "???";
	if (v)
		value_print(buf, sizeof(buf), v);
	//fprintf(file, "0x%016" PRIxPTR " %s\n", addr, buf);
}

enum memdig_result {
	MEMDIG_RESULT_ERROR = -1,
	MEMDIG_RESULT_OK = 0,
	MEMDIG_RESULT_QUIT = 1,
};

static enum memdig_result
memdig_exec(struct memdig *m, int argc, char **argv)
{
	if (argc == 0)
		return MEMDIG_RESULT_OK;
	char *verb = argv[0];
	enum command command = COMMAND_AMBIGUOUS; //no parse needed so just removed compile errors
	switch (command) {
	case COMMAND_AMBIGUOUS: {
		LOG_ERROR("ambiguous command '%s'\n", verb);
	} break;
	case COMMAND_UNKNOWN: {
		LOG_ERROR("unknown command '%s'\n", verb);
	} break;
	case COMMAND_ATTACH: {
		if (argc == 1) {
			if (m->target)
				printf("attached to %ld\n", (long)m->id);
			else
				printf("not attached to a process\n");
			return MEMDIG_RESULT_OK;
		}
		else if (argc != 2)
			LOG_ERROR("wrong number of arguments\n");
		if (m->target) {
			os_mutex_lock(&m->thread);
			watchlist_free(&m->active);
			watchlist_free(&m->locked);
			os_process_close(m->target);
			m->target = 0;
			os_mutex_unlock(&m->thread);
		}
		char *pattern = argv[1];
		switch (process_find(pattern, &m->id)) {
		case FIND_FAILURE:
			LOG_ERROR("no process found for '%s'\n", pattern);
			break;
		case FIND_AMBIGUOUS:
			LOG_ERROR("ambiguous target '%s'\n", pattern);
			break;
		case FIND_SUCCESS:
			os_mutex_lock(&m->thread);
			if (!(m->target = os_process_open(m->id))) {
				if (!m->target)
					LOG_ERROR("open process %ld failed: %s\n",
					(long)m->id, os_last_error());
				m->id = 0;
			}
			else {
				watchlist_init(&m->active, m->target);
				watchlist_init(&m->locked, m->target);
				printf("attached to %ld\n", (long)m->id);
			}
			os_mutex_unlock(&m->thread);
			break;
		}
	} break;
	case COMMAND_MEMORY: {
		if (!m->target)
			LOG_ERROR("no process attached\n");
		display_memory_regions(m->target);
	} break;
	case COMMAND_FIND: {
		if (!m->target)
			LOG_ERROR("no process attached\n");
		if (argc < 2 || argc > 3)
			LOG_ERROR("wrong number of arguments\n");
		enum scan_op op = SCAN_OP_EQ;
		const char *arg = argv[1];
		if (argc == 3) {
			if (!scan_op_parse(argv[1], &op))
				LOG_ERROR("invalid operator '%s'\n", argv[1]);
			arg = argv[2];
		}
		struct value value;
		enum value_parse_result r = value_parse(&value, arg);
		switch (r) {
		case VALUE_PARSE_OVERFLOW: {
			LOG_ERROR("overflow '%s'\n", arg);
		} break;
		case VALUE_PARSE_INVALID: {
			LOG_ERROR("invalid value '%s'\n", arg);
		} break;
		case VALUE_PARSE_SUCCESS: {
			char buf[64];
			value_print(buf, sizeof(buf), &value);
			LOG_INFO("finding %s\n", buf);
		} break;
		}
		m->last_type = value.type;
		if (!scan(&m->active, &value, op))
			LOG_ERROR("scan failure'\n");
		else
			printf("%zu values found\n", m->active.count);
	} break;
	case COMMAND_NARROW: {
		if (!m->target)
			LOG_ERROR("no process attached\n");
		if (argc < 2 || argc > 3)
			LOG_ERROR("wrong number of arguments\n");
		enum scan_op op = SCAN_OP_EQ;
		const char *arg = argv[1];
		if (argc == 3) {
			if (!scan_op_parse(argv[1], &op))
				LOG_ERROR("invalid operator '%s'\n", argv[1]);
			arg = argv[2];
		}
		struct value value;
		enum value_parse_result r = value_parse(&value, arg);
		switch (r) {
		case VALUE_PARSE_OVERFLOW: {
			LOG_ERROR("overflow '%s'\n", arg);
		} break;
		case VALUE_PARSE_INVALID: {
			LOG_ERROR("invalid value '%s'\n", arg);
		} break;
		case VALUE_PARSE_SUCCESS: {
			char buf[64];
			value_print(buf, sizeof(buf), &value);
			LOG_INFO("narrowing to %s\n", buf);
		} break;
		}

		if (!narrow(&m->active, op, &value))
			LOG_ERROR("scan failure'\n");
		else
			printf("%zu values found\n", m->active.count);
	} break;
	case COMMAND_PUSH: {
		if (!m->target)
			LOG_ERROR("no process attached\n");
		if (argc != 2)
			LOG_ERROR("wrong number of arguments");
		char *addrs = argv[1];
		if (strncmp(addrs, "0x", 2) != 0)
			LOG_ERROR("unknown address format '%s'\n", addrs);
		uintptr_t addr = (uintptr_t)strtoull(addrs + 2, NULL, 16);
		struct value value;
		value.type = m->last_type;
		value.value.u64 = 0;
		watchlist_push(&m->active, addr, &value);
	} break;
	case COMMAND_LIST: {
		char arg = 'a';
		if (argc > 1)
			arg = argv[1][0];
		switch (arg) {
		case 'a': {
			if (!m->target)
				LOG_ERROR("no process attached\n");
			watchlist_visit(&m->active, list_visitor, stdout);
		} break;
		case 'p': {
			struct process_iterator i[1];
			process_iterator_init(i);
			for (; !process_iterator_done(i); process_iterator_next(i))
				printf("%8ld %s\n", (long)i->pid, i->name);
			process_iterator_destroy(i);
		} break;
		case 'l': {
			if (!m->target)
				LOG_ERROR("no process attached\n");
			watchlist_visit(&m->locked, list_visitor, 0);
		} break;
		default: {
			LOG_ERROR("unknown list type '%s'\n", argv[1]);
		} break;
		}
	} break;
	case COMMAND_SET: {
		if (!m->target)
			LOG_ERROR("no process attached\n");
		if (argc != 2)
			LOG_ERROR("wrong number of arguments");
		const char *arg = argv[1];
		struct value value;
		enum value_parse_result r = value_parse(&value, arg);
		switch (r) {
		case VALUE_PARSE_OVERFLOW: {
			LOG_ERROR("overflow '%s'\n", arg);
		} break;
		case VALUE_PARSE_INVALID: {
			LOG_ERROR("invalid value '%s'\n", arg);
		} break;
		case VALUE_PARSE_SUCCESS: {
			char buf[64];
			value_print(buf, sizeof(buf), &value);
			LOG_INFO("setting to %s\n", buf);
		} break;
		}
		m->last_type = value.type;
		size_t set_count = 0;
		for (size_t i = 0; i < m->active.count; i++) {
			uintptr_t addr = m->active.list[i].addr;
			unsigned size = VALUE_SIZE(value);
			if (!os_write_memory(m->target, addr, &value.value, size))
				LOG_WARNING("write memory failed: %s\n",
					os_last_error());
			else
				set_count++;
		}
		printf("%zu values set\n", set_count);
	} break;
	case COMMAND_LOCK: {
		if (!m->target)
			LOG_ERROR("no process attached\n");
		if (argc > 2)
			LOG_ERROR("wrong number of arguments");
		struct value value;
		int have_value = 0;
		if (argc == 2) {
			have_value = 1;
			const char *arg = argv[1];
			enum value_parse_result r = value_parse(&value, arg);
			switch (r) {
			case VALUE_PARSE_OVERFLOW: {
				LOG_ERROR("overflow '%s'\n", arg);
			} break;
			case VALUE_PARSE_INVALID: {
				LOG_ERROR("invalid value '%s'\n", arg);
			} break;
			case VALUE_PARSE_SUCCESS: {
				char buf[64];
				value_print(buf, sizeof(buf), &value);
				LOG_INFO("setting to %s\n", buf);
			} break;
			}
			m->last_type = value.type;
		}
		os_mutex_lock(&m->thread);
		for (size_t i = 0; i < m->active.count; i++) {
			uintptr_t addr = m->active.list[i].addr;
			struct value *prev = &m->active.list[i].prev;
			watchlist_push(&m->locked, addr, have_value ? &value : prev);
		}
		os_mutex_unlock(&m->thread);
	} break;
	case COMMAND_WAIT: {
		if (argc != 2)
			LOG_ERROR("wrong number of arguments");
		os_sleep(atof(argv[1]));
	} break;
	case COMMAND_QUIT: {
		return MEMDIG_RESULT_QUIT;
	} break;
	}
	return MEMDIG_RESULT_OK;
fail:
	return MEMDIG_RESULT_ERROR;
}

static void
memdig_free(struct memdig *m)
{
	os_mutex_lock(&m->thread);
	if (m->target) {
		watchlist_free(&m->active);
		watchlist_free(&m->locked);
		os_process_close(m->target);
		m->target = 0;
	}
	m->running = 0;
	os_mutex_unlock(&m->thread);
	os_thread_join(&m->thread);
}

#define PROMPT(f)                               \
    do {                                        \
        fputs("> ", f);                         \
        fflush(f);                              \
    } while(0)

/*int
main(int argc, char **argv)
{
	int result = 0;
	struct memdig memdig[1];
	memdig_init(memdig);

	char *xargv[16];
	int xargc = 0;
	int i = 1;
	while (i < argc && argv[i][0] == '-') {
		xargc = 1;
		xargv[0] = argv[i++];
		while (xargv[0][0] == '-' && xargv[0][0] != 0)
			xargv[0]++;
		while (i < argc && argv[i][0] != '-' && xargc < 15)
			xargv[xargc++] = argv[i++];
		xargv[xargc] = NULL;
		enum memdig_result r = memdig_exec(memdig, xargc, xargv);
		if (r == MEMDIG_RESULT_ERROR)
			result = -1;
		if (r != MEMDIG_RESULT_OK)
			goto quit;
		if (strcmp(xargv[0], "help") == 0)
			goto quit;
		if (strcmp(xargv[0], "version") == 0)
			goto quit;
	}

	char line[4096];
	PROMPT(stdout);
	const char *delim = " \n\t";
	while (fgets(line, sizeof(line), stdin)) {
		xargc = 0;
		xargv[0] = strtok(line, delim);
		if (xargv[0]) {
			do
				xargc++;
			while (xargc < 15 && (xargv[xargc] = strtok(NULL, delim)));
			xargv[xargc] = NULL;
		}
		if (memdig_exec(memdig, xargc, xargv) == MEMDIG_RESULT_QUIT)
			goto quit;
		PROMPT(stdout);
	}

quit:
	memdig_free(memdig);
	return result;
}*/

static memdig _instance;
static bool _instanceInited;
static std::string _processFilename;
bool AttachToCloseCombat()
{
	if (_instanceInited)
		return true; //already initialized

	bool retval = false;

	if (process_find("CC3.exe", &_instance.id) != FIND_SUCCESS)
	{
		std::cerr << "Failure to locate CC3.exe\n";
		return retval;
	}

	_instanceInited = true;
	_instance.last_type = VALUE_S32;
	_instance.running = 1;
	os_thread_start(&_instance.thread, &_instance);
	os_mutex_lock(&_instance.thread);

	_instance.target = os_process_open(_instance.id);
	if (!_instance.target)
	{
		_instance.id = 0;
		std::cerr << "Failed to open CC3.exe process: " << os_last_error() << std::endl;

		//TODO: call DetachFromCloseCombat here for cleanup when I can verify it works without crashing
		//low prio since we exit app anyway if this happens
	}
	else
	{
		char filename[MAX_PATH + 1];
		const DWORD filenameLength = GetModuleFileNameExA(_instance.target, nullptr, filename, MAX_PATH + 1);
		if (filenameLength == 0)
		{
			std::cerr << "Failed to get filename of CC3.exe: " << os_last_error() << std::endl;
		}
		else
		{
			retval = true;
			_processFilename = filename;
		}

		watchlist_init(&_instance.active, _instance.target);
		watchlist_init(&_instance.locked, _instance.target);
		std::cout << "Attached to CC3.exe\n";
	}
	os_mutex_unlock(&_instance.thread);

	return retval;
}

const std::string& GetAttachedFilename()
{
	return _processFilename;
}

std::string GetAttachedPathPrefix()
{
	auto offset = _processFilename.find_last_of('\\');
	if (offset == std::string::npos)
	{
		std::cerr << "Warning: Unexpected lack of folder path separator in " << _processFilename << std::endl;
		return ".\\";
	}

	return _processFilename.substr(0, offset + 1);
}

void DetachFromCloseCombat()
{
	if (!_instanceInited)
		return;

	os_mutex_lock(&_instance.thread);
	if (_instance.target)
	{
		watchlist_free(&_instance.active);
		watchlist_free(&_instance.locked);
		os_process_close(_instance.target);
		_instance.target = 0;
	}
	_instance.running = 0;
	os_mutex_unlock(&_instance.thread);
	os_thread_join(&_instance.thread);
}

static uint32_t FindSharedOffset(const char* magic)
{
	uint32_t sharedOffset = 0;
	const std::size_t len = std::strlen(magic);

	region_iterator it[1];
	region_iterator_init(it, _instance.target);

	for (; !region_iterator_done(it); region_iterator_next(it))
	{
		if (it->flags & REGION_ITERATOR_EXECUTE)
		{
			continue; //not interested in program code
		}

		const char* const buf = (const char*)region_iterator_memory(it);
		if (buf)
		{
			std::size_t offset = 0;
			std::size_t remaining = it->size;
			while (remaining >= len)
			{
				if (std::memcmp(buf + offset, magic, len) == 0)
				{
					sharedOffset = static_cast<uint32_t>(it->actualBase + offset);
					goto End;
				}

				offset += 1;
				remaining -= 1;
			}
			
		}
	}
	End:
	region_iterator_destroy(it);

	if (sharedOffset == 0)
	{
		std::cerr << "Unable to find shared offset " << magic << std::endl;
	}

	return sharedOffset;
}


void DumpMemory(std::ostream& binaryStream, bool segmented)
{
	const uint32_t sharedOffset = FindSharedOffset("Close Combat: Cross of Iron");

	region_iterator it[1];
	region_iterator_init(it, _instance.target);

	for (; !region_iterator_done(it); region_iterator_next(it))
	{
		if (it->flags & REGION_ITERATOR_EXECUTE)
		{
			continue; //not interested in program code
		}

		const char* buf = (const char*)region_iterator_memory(it);
		if (buf)
		{
			if (segmented)
			{
				const uint32_t base = it->actualBase - sharedOffset; //fine, reader can interpret as signed
				const uint32_t size = it->size;
				binaryStream.write(reinterpret_cast<const char*>(&base), 4);
				binaryStream.write(reinterpret_cast<const char*>(&size), 4);
			}
			
			binaryStream.write(buf, it->size);
		}
		else
		{
			std::cerr << "CC3.exe memory read failed: " << os_last_error() << std::endl;
		}
	}
	region_iterator_destroy(it);
}
