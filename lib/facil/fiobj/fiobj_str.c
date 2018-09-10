/*
Copyright: Boaz Segev, 2017-2018
License: MIT
*/

#if defined(__unix__) || defined(__APPLE__) || defined(__linux__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <unistd.h>
#endif

#ifdef _SC_PAGESIZE
#define PAGE_SIZE sysconf(_SC_PAGESIZE)
#else
#define PAGE_SIZE 4096
#endif

#include "fiobject.h"

#include "fio_siphash.h"
#include "fiobj_numbers.h"
#include "fiobj_str.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

#define FIO_OVERRIDE_MALLOC 1
#include "fiobj_mem.h"

#include "fio_str.h"

#ifndef PATH_MAX
#define PATH_MAX PAGE_SIZE
#endif

/* *****************************************************************************
String Type
***************************************************************************** */

typedef struct {
  fiobj_object_header_s head;
  uint64_t hash;
  fio_str_s str;
} fiobj_str_s;

#define obj2str(o) ((fiobj_str_s *)(FIOBJ2PTR(o)))

static inline fio_cstr_s fiobj_str_get_cstr(const FIOBJ o) {
  fio_str_state_s state = fio_str_state(&obj2str(o)->str);
  return (fio_cstr_s){.buffer = state.data, .len = state.len};
}

/* *****************************************************************************
String VTables
***************************************************************************** */

static fio_cstr_s fio_str2str(const FIOBJ o) { return fiobj_str_get_cstr(o); }

static void fiobj_str_dealloc(FIOBJ o, void (*task)(FIOBJ, void *), void *arg) {
  fio_str_free(&obj2str(o)->str);
  fio_free(FIOBJ2PTR(o));
  (void)task;
  (void)arg;
}

static size_t fiobj_str_is_eq(const FIOBJ self, const FIOBJ other) {
  return fio_str_iseq(&obj2str(self)->str, &obj2str(other)->str);
}

static intptr_t fio_str2i(const FIOBJ o) {
  char *pos = fio_str_data(&obj2str(o)->str);
  return fio_atol(&pos);
}
static double fio_str2f(const FIOBJ o) {
  char *pos = fio_str_data(&obj2str(o)->str);
  return fio_atof(&pos);
}

static size_t fio_str2bool(const FIOBJ o) {
  return fio_str_len(&obj2str(o)->str) != 0;
}

uintptr_t fiobject___noop_count(const FIOBJ o);

const fiobj_object_vtable_s FIOBJECT_VTABLE_STRING = {
    .class_name = "String",
    .dealloc = fiobj_str_dealloc,
    .to_i = fio_str2i,
    .to_f = fio_str2f,
    .to_str = fio_str2str,
    .is_eq = fiobj_str_is_eq,
    .is_true = fio_str2bool,
    .count = fiobject___noop_count,
};

/* *****************************************************************************
String API
***************************************************************************** */

/** Creates a buffer String object. Remember to use `fiobj_free`. */
FIOBJ fiobj_str_buf(size_t capa) {
  if (capa)
    capa = capa + 1;
  else
    capa = PAGE_SIZE;

  fiobj_str_s *s = fio_malloc(sizeof(*s));
  if (!s) {
    perror("ERROR: fiobj string couldn't allocate memory");
    exit(errno);
  }
  *s = (fiobj_str_s){
      .head =
          {
              .ref = 1,
              .type = FIOBJ_T_STRING,
          },
      .str = FIO_STR_INIT,
  };
  if (capa) {
    fio_str_capa_assert(&s->str, capa);
  }
  return ((uintptr_t)s | FIOBJECT_STRING_FLAG);
}

/** Creates a String object. Remember to use `fiobj_free`. */
FIOBJ fiobj_str_new(const char *str, size_t len) {
  fiobj_str_s *s = fio_malloc(sizeof(*s));
  if (!s) {
    perror("ERROR: fiobj string couldn't allocate memory");
    exit(errno);
  }
  *s = (fiobj_str_s){
      .head =
          {
              .ref = 1,
              .type = FIOBJ_T_STRING,
          },
      .str = FIO_STR_INIT,
  };
  if (str && len) {
    fio_str_write(&s->str, str, len);
  }
  return ((uintptr_t)s | FIOBJECT_STRING_FLAG);
}

/**
 * Creates a String object. Remember to use `fiobj_free`.
 *
 * It's possible to wrap a previosly allocated memory block in a FIOBJ String
 * object, as long as it was allocated using `fio_malloc`.
 *
 * The ownership of the memory indicated by `str` will "move" to the object and
 * will be freed (using `fio_free`) once the object's reference count drops to
 * zero.
 */
FIOBJ fiobj_str_move(char *str, size_t len, size_t capacity) {
  fiobj_str_s *s = fio_malloc(sizeof(*s));
  if (!s) {
    perror("ERROR: fiobj string couldn't allocate memory");
    exit(errno);
  }
  *s = (fiobj_str_s){
      .head =
          {
              .ref = 1,
              .type = FIOBJ_T_STRING,
          },
      .str = FIO_STR_INIT_EXISTING(str, len, capacity),
  };
  return ((uintptr_t)s | FIOBJECT_STRING_FLAG);
}

/** Creates a String object using a printf like interface. */
__attribute__((format(printf, 1, 0))) FIOBJ fiobj_strvprintf(const char *format,
                                                             va_list argv) {
  FIOBJ str = FIOBJ_INVALID;
  va_list argv_cpy;
  va_copy(argv_cpy, argv);
  int len = vsnprintf(NULL, 0, format, argv_cpy);
  va_end(argv_cpy);
  if (len < 0)
    return str;
  str = fiobj_str_new("", 0);
  if (len == 0)
    return str;
  fio_str_state_s state = fio_str_resize(&obj2str(str)->str, len);
  vsnprintf(state.data, len + 1, format, argv);
  return str;
}
__attribute__((format(printf, 1, 2))) FIOBJ fiobj_strprintf(const char *format,
                                                            ...) {
  va_list argv;
  va_start(argv, format);
  FIOBJ str = fiobj_strvprintf(format, argv);
  va_end(argv);
  return str;
}

/**
 * Returns a thread-static temporary string. Avoid calling `fiobj_dup` or
 * `fiobj_free`.
 */
FIOBJ fiobj_str_tmp(void) {
  static __thread fiobj_str_s tmp = {
      .head =
          {
              .ref = ((~(uint32_t)0) >> 4),
              .type = FIOBJ_T_STRING,
          },
      .str = {.small = 1},
  };
  tmp.str.frozen = 0;
  return ((uintptr_t)&tmp | FIOBJECT_STRING_FLAG);
}

/** Dumps the `filename` file's contents into a new String. If `limit == 0`,
 * than the data will be read until EOF.
 *
 * If the file can't be located, opened or read, or if `start_at` is beyond
 * the EOF position, NULL is returned.
 *
 * Remember to use `fiobj_free`.
 */
FIOBJ fiobj_str_readfile(const char *filename, intptr_t start_at,
                         intptr_t limit) {
#if defined(__unix__) || defined(__linux__) || defined(__APPLE__)
  /* POSIX implementations. */
  fiobj_str_s *s = fio_malloc(sizeof(*s));
  if (!s) {
    perror("ERROR: fiobj string couldn't allocate memory");
    exit(errno);
  }
  *s = (fiobj_str_s){
      .head =
          {
              .ref = 1,
              .type = FIOBJ_T_STRING,
          },
      .str = FIO_STR_INIT,
  };
  fio_str_state_s state = fio_str_fread(&s->str, filename, start_at, limit);
  if (!state.data) {
    free(s);
    return FIOBJ_INVALID;
  }
  return ((uintptr_t)s | FIOBJECT_STRING_FLAG);

#else
  /* TODO: consider adding non POSIX implementations. */
  return FIOBJ_INVALID;
#endif
}

/** Prevents the String object from being changed. */
void fiobj_str_freeze(FIOBJ str) {
  if (FIOBJ_TYPE_IS(str, FIOBJ_T_STRING))
    fio_str_freeze(&obj2str(str)->str);
}

/** Confirms the requested capacity is available and allocates as required. */
size_t fiobj_str_capa_assert(FIOBJ str, size_t size) {

  assert(FIOBJ_TYPE_IS(str, FIOBJ_T_STRING));
  if (obj2str(str)->str.frozen)
    return 0;
  fio_str_state_s state = fio_str_capa_assert(&obj2str(str)->str, size);
  return state.capa;
}

/** Return's a String's capacity, if any. */
size_t fiobj_str_capa(FIOBJ str) {
  assert(FIOBJ_TYPE_IS(str, FIOBJ_T_STRING));
  return fio_str_capa(&obj2str(str)->str);
}

/** Resizes a String object, allocating more memory if required. */
void fiobj_str_resize(FIOBJ str, size_t size) {
  assert(FIOBJ_TYPE_IS(str, FIOBJ_T_STRING));
  fio_str_resize(&obj2str(str)->str, size);
  obj2str(str)->hash = 0;
  return;
}

/** Deallocates any unnecessary memory (if supported by OS). */
void fiobj_str_minimize(FIOBJ str) {
  assert(FIOBJ_TYPE_IS(str, FIOBJ_T_STRING));
  fio_str_compact(&obj2str(str)->str);
  return;
}

/** Empties a String's data. */
void fiobj_str_clear(FIOBJ str) {
  assert(FIOBJ_TYPE_IS(str, FIOBJ_T_STRING));
  fio_str_resize(&obj2str(str)->str, 0);
  obj2str(str)->hash = 0;
}

/**
 * Writes data at the end of the string, resizing the string as required.
 * Returns the new length of the String
 */
size_t fiobj_str_write(FIOBJ dest, const char *data, size_t len) {
  assert(FIOBJ_TYPE_IS(dest, FIOBJ_T_STRING));
  if (obj2str(dest)->str.frozen)
    return 0;
  obj2str(dest)->hash = 0;
  return fio_str_write(&obj2str(dest)->str, data, len).len;
}
/**
 * Writes data at the end of the string, resizing the string as required.
 * Returns the new length of the String
 */
size_t fiobj_str_write2(FIOBJ dest, const char *format, ...) {
  assert(FIOBJ_TYPE_IS(dest, FIOBJ_T_STRING));
  if (obj2str(dest)->str.frozen)
    return 0;
  obj2str(dest)->hash = 0;
  va_list argv;
  va_start(argv, format);
  fio_str_state_s state = fio_str_vprintf(&obj2str(dest)->str, format, argv);
  va_end(argv);
  return state.len;
}
/**
 * Writes data at the end of the string, resizing the string as required.
 * Returns the new length of the String
 */
size_t fiobj_str_join(FIOBJ dest, FIOBJ obj) {
  assert(FIOBJ_TYPE_IS(dest, FIOBJ_T_STRING));
  if (obj2str(dest)->str.frozen)
    return 0;
  obj2str(dest)->hash = 0;
  fio_cstr_s o = fiobj_obj2cstr(obj);
  if (o.len == 0)
    return fio_str_len(&obj2str(dest)->str);
  return fio_str_write(&obj2str(dest)->str, o.data, o.len).len;
}

/**
 * Calculates a String's SipHash value for use as a HashMap key.
 */
uint64_t fiobj_str_hash(FIOBJ o) {
  assert(FIOBJ_TYPE_IS(o, FIOBJ_T_STRING));
  // if (obj2str(o)->is_small) {
  //   return fio_siphash(STR_INTENAL_STR(o), STR_INTENAL_LEN(o));
  // } else
  if (obj2str(o)->hash) {
    return obj2str(o)->hash;
  }
  fio_str_state_s state = fio_str_state(&obj2str(o)->str);
  obj2str(o)->hash = fio_siphash(state.data, state.len);
  return obj2str(o)->hash;
}

/* *****************************************************************************
Tests
***************************************************************************** */

#if DEBUG
void fiobj_test_string(void) {
  fprintf(stderr, "=== Testing Strings\n");
  fprintf(stderr, "* Internal String Capacity %u \n",
          (unsigned int)FIO_STR_SMALL_CAPA);
#define TEST_ASSERT(cond, ...)                                                 \
  if (!(cond)) {                                                               \
    fprintf(stderr, "* " __VA_ARGS__);                                         \
    fprintf(stderr, "Testing failed.\n");                                      \
    exit(-1);                                                                  \
  }
#define STR_EQ(o, str)                                                         \
  TEST_ASSERT((fiobj_str_getlen(o) == strlen(str) &&                           \
               !memcmp(fiobj_str_mem_addr(o), str, strlen(str))),              \
              "String not equal to " str)
  FIOBJ o = fiobj_str_new("Hello", 5);
  TEST_ASSERT(FIOBJ_TYPE_IS(o, FIOBJ_T_STRING), "Small String isn't string!\n");
  TEST_ASSERT(obj2str(o)->str.small, "Hello isn't small\n");
  fiobj_str_write(o, " World", 6);
  TEST_ASSERT(FIOBJ_TYPE_IS(o, FIOBJ_T_STRING),
              "Hello World String isn't string!\n");
  TEST_ASSERT(obj2str(o)->str.small, "Hello World isn't small\n");
  TEST_ASSERT(fiobj_obj2cstr(o).len == 11,
              "Invalid small string length (%u != 11)!\n",
              (unsigned int)fiobj_obj2cstr(o).len)
  fiobj_str_write(o, " World, you crazy longer sleep loving person :-)", 48);
  TEST_ASSERT(!obj2str(o)->str.small, "Crazier shouldn't be small\n");
  fiobj_free(o);

  o = fiobj_str_new(
      "hello my dear friend, I hope that your are well and happy.", 58);
  TEST_ASSERT(FIOBJ_TYPE_IS(o, FIOBJ_T_STRING), "Long String isn't string!\n");
  TEST_ASSERT(!obj2str(o)->str.small,
              "Long String is small! (capa: %lu, len: %lu)\n",
              fio_str_capa(&obj2str(o)->str), fio_str_len(&obj2str(o)->str));
  TEST_ASSERT(fiobj_obj2cstr(o).len == 58,
              "Invalid long string length (%lu != 58)!\n",
              fiobj_obj2cstr(o).len)
  uint64_t hash = fiobj_str_hash(o);
  TEST_ASSERT(!obj2str(o)->str.frozen, "String forzen when only hashing!\n");
  fiobj_str_freeze(o);
  TEST_ASSERT(obj2str(o)->str.frozen, "String not forzen!\n");
  fiobj_str_write(o, " World", 6);
  TEST_ASSERT(hash == fiobj_str_hash(o),
              "String hash changed after hashing - not frozen?\n");
  TEST_ASSERT(fiobj_obj2cstr(o).len == 58,
              "String was edited after hashing - not frozen!\n (%lu): %s",
              (unsigned long)fiobj_obj2cstr(o).len, fiobj_obj2cstr(o).data);
  fiobj_free(o);

  o = fiobj_strprintf("%u", 42);
  TEST_ASSERT(fio_str_len(&obj2str(o)->str) == 2,
              "fiobj_strprintf length error.\n");
  TEST_ASSERT(fiobj_obj2num(o), "fiobj_strprintf integer error.\n");
  TEST_ASSERT(!memcmp(fiobj_obj2cstr(o).data, "42", 2),
              "fiobj_strprintf string error.\n");
  fiobj_free(o);

  o = fiobj_str_buf(4);
  for (int i = 0; i < 16000; ++i) {
    fiobj_str_write(o, "a", 1);
  }
  TEST_ASSERT(fio_str_len(&obj2str(o)->str) == 16000,
              "16K fiobj_str_write not 16K.\n");
  TEST_ASSERT(fio_str_capa(&obj2str(o)->str) >= 16000,
              "16K fiobj_str_write capa not enough.\n");
  fiobj_free(o);

  o = fiobj_str_readfile(__FILE__, 0, 0);
  TEST_ASSERT(!memcmp(fiobj_obj2cstr(o).data, "/*", 2),
              "`fiobj_str_readfile` error, start of file doesn't match:\n%s",
              fiobj_obj2cstr(o).data);
  fiobj_free(o);

  fprintf(stderr, "* passed.\n");
}
#endif