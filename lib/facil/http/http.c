/*
Copyright: Boaz segev, 2016-2017
License: MIT

Feel free to copy, use and enjoy according to the license provided.
*/

#include "spnlock.inc"

#include "fio_base64.h"
#include "http1.h"
#include "http_internal.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* *****************************************************************************
Small Helpers
***************************************************************************** */
static inline void add_content_length(http_s *r, uintptr_t length) {
  static uint64_t cl_hash = 0;
  if (!cl_hash)
    cl_hash = fiobj_sym_hash("content-length", 14);
  if (!fiobj_hash_get3(r->private_data.out_headers, cl_hash)) {
    static fiobj_s *cl;
    if (!cl)
      cl = HTTP_HEADER_CONTENT_LENGTH;
    fiobj_hash_set(r->private_data.out_headers, cl, fiobj_num_new(length));
  }
}

static fiobj_s *current_date;
static time_t last_date_added;
static spn_lock_i date_lock;
static inline void add_date(http_s *r) {
  static uint64_t date_hash = 0;
  if (!date_hash)
    date_hash = fiobj_sym_hash("date", 4);
  static uint64_t mod_hash = 0;
  if (!mod_hash)
    mod_hash = fiobj_sym_hash("last-modified", 13);

  if (facil_last_tick().tv_sec >= last_date_added + 60) {
    fiobj_s *tmp = fiobj_str_buf(32);
    fiobj_str_resize(
        tmp, http_time2str(fiobj_obj2cstr(tmp).data, facil_last_tick().tv_sec));
    spn_lock(&date_lock);
    if (facil_last_tick().tv_sec >= last_date_added + 60) {
      last_date_added = facil_last_tick().tv_sec;
      fiobj_s *other = current_date;
      current_date = tmp;
      tmp = other;
    }
    spn_unlock(&date_lock);
    fiobj_free(tmp);
  }

  if (!fiobj_hash_get3(r->private_data.out_headers, date_hash)) {
    fiobj_hash_set(r->private_data.out_headers, HTTP_HEADER_DATE,
                   fiobj_dup(current_date));
  }
  if (!fiobj_hash_get3(r->private_data.out_headers, mod_hash)) {
    fiobj_hash_set(r->private_data.out_headers, HTTP_HEADER_LAST_MODIFIED,
                   fiobj_dup(current_date));
  }
}

struct header_writer_s {
  fiobj_s *dest;
  fiobj_s *name;
  fiobj_s *value;
};

static int write_header(fiobj_s *o, void *w_) {
  struct header_writer_s *w = w_;
  if (!o)
    return 0;
  if (o->type == FIOBJ_T_COUPLET) {
    w->name = fiobj_couplet2key(o);
    o = fiobj_couplet2obj(o);
    if (!o)
      return 0;
  }
  if (o->type == FIOBJ_T_ARRAY) {
    fiobj_each1(o, 0, write_header, w);
    return 0;
  }
  fio_cstr_s name = fiobj_obj2cstr(w->name);
  fio_cstr_s str = fiobj_obj2cstr(o);
  if (!str.data)
    return 0;
  fiobj_str_write(w->dest, name.data, name.len);
  fiobj_str_write(w->dest, ":", 1);
  fiobj_str_write(w->dest, str.data, str.len);
  fiobj_str_write(w->dest, "\r\n", 2);
  return 0;
}

static char invalid_cookie_name_char[256];

static char invalid_cookie_value_char[256];
/* *****************************************************************************
The Request / Response type and functions
***************************************************************************** */
static const char hex_chars[] = {'0', '1', '2', '3', '4', '5', '6', '7',
                                 '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};

/**
 * Sets a response header, taking ownership of the value object, but NOT the
 * name object (so name objects could be reused in future responses).
 *
 * Returns -1 on error and 0 on success.
 */
int http_set_header(http_s *r, fiobj_s *name, fiobj_s *value) {
  if (!r || !name || !r->private_data.out_headers)
    return -1;
  set_header_add(r->private_data.out_headers, name, value);
  return 0;
}
/**
 * Sets a response header, taking ownership of the value object, but NOT the
 * name object (so name objects could be reused in future responses).
 *
 * Returns -1 on error and 0 on success.
 */
int http_set_header2(http_s *r, fio_cstr_s n, fio_cstr_s v) {
  if (!r || !n.data || !n.length || (v.data && !v.length) ||
      !r->private_data.out_headers)
    return -1;
  fiobj_s *tmp = fiobj_sym_new(n.data, n.length);
  int ret = http_set_header(r, tmp, fiobj_str_new(v.data, v.length));
  fiobj_free(tmp);
  return ret;
}
/**
 * Sets a response cookie, taking ownership of the value object, but NOT the
 * name object (so name objects could be reused in future responses).
 *
 * Returns -1 on error and 0 on success.
 */
#undef http_set_cookie
int http_set_cookie(http_s *h, http_cookie_args_s cookie) {
#if DEBUG
  HTTP_ASSERT(h, "Can't set cookie for NULL HTTP handler!");
#endif
  if (!h || !h->private_data.out_headers || cookie.name_len >= 32768 ||
      cookie.value_len >= 131072)
    return -1;

  /* write name and value while auto-correcting encoding issues */
  size_t capa = cookie.name_len + cookie.value_len + 128;
  size_t len = 0;
  fiobj_s *c = fiobj_str_buf(capa);
  fio_cstr_s t = fiobj_obj2cstr(c);
  if (cookie.name) {
    if (cookie.name_len) {
      size_t tmp = 0;
      while (tmp < cookie.name_len) {
        if (invalid_cookie_name_char[(uint8_t)cookie.name[tmp]]) {
          fprintf(stderr,
                  "WARNING: illigal char 0x%.2x in cookie name (in %s)\n"
                  "         automatic %% encoding applied\n",
                  cookie.name[tmp], cookie.name);
          t.data[len++] = '%';
          t.data[len++] = hex_chars[(cookie.name[tmp] >> 4) & 0x0F];
          t.data[len++] = hex_chars[cookie.name[tmp] & 0x0F];
        } else {
          t.data[len++] = cookie.name[tmp];
        }
        tmp += 1;
        if (capa <= len + 3) {
          capa += 32;
          fiobj_str_capa_assert(c, capa);
          t = fiobj_obj2cstr(c);
        }
      }
    } else {
      size_t tmp = 0;
      while (cookie.name[tmp]) {
        if (invalid_cookie_name_char[(uint8_t)cookie.name[tmp]]) {
          fprintf(stderr,
                  "WARNING: illigal char 0x%.2x in cookie name (in %s)\n"
                  "         automatic %% encoding applied\n",
                  cookie.name[tmp], cookie.name);
          t.data[len++] = '%';
          t.data[len++] = hex_chars[(cookie.name[tmp] >> 4) & 0x0F];
          t.data[len++] = hex_chars[cookie.name[tmp] & 0x0F];
        } else {
          t.data[len++] = cookie.name[tmp];
        }
        tmp += 1;
        if (capa <= len + 4) {
          capa += 32;
          fiobj_str_capa_assert(c, capa);
          t = fiobj_obj2cstr(c);
        }
      }
    }
  }
  t.data[len++] = '=';
  if (cookie.value) {
    if (cookie.value_len) {
      size_t tmp = 0;
      while (tmp < cookie.value_len) {
        if (invalid_cookie_value_char[(uint8_t)cookie.value[tmp]]) {
          fprintf(stderr,
                  "WARNING: illigal char 0x%.2x in cookie value (in %s)\n"
                  "         automatic %% encoding applied\n",
                  cookie.value[tmp], cookie.name);
          t.data[len++] = '%';
          t.data[len++] = hex_chars[(cookie.value[tmp] >> 4) & 0x0F];
          t.data[len++] = hex_chars[cookie.value[tmp] & 0x0F];
        } else {
          t.data[len++] = cookie.value[tmp];
        }
        tmp += 1;
        if (capa <= len + 3) {
          capa += 32;
          fiobj_str_capa_assert(c, capa);
          t = fiobj_obj2cstr(c);
        }
      }
    } else {
      size_t tmp = 0;
      while (cookie.value[tmp]) {
        if (invalid_cookie_value_char[(uint8_t)cookie.value[tmp]]) {
          fprintf(stderr,
                  "WARNING: illigal char 0x%.2x in cookie value (in %s)\n"
                  "         automatic %% encoding applied\n",
                  cookie.value[tmp], cookie.name);
          t.data[len++] = '%';
          t.data[len++] = hex_chars[(cookie.value[tmp] >> 4) & 0x0F];
          t.data[len++] = hex_chars[cookie.value[tmp] & 0x0F];
        } else {
          t.data[len++] = cookie.value[tmp];
        }
        tmp += 1;
        if (capa <= len + 3) {
          capa += 32;
          fiobj_str_capa_assert(c, capa);
          t = fiobj_obj2cstr(c);
        }
      }
    }
  } else
    cookie.max_age = -1;
  t.data[len++] = ';';
  if (capa <= len + 40) {
    capa = len + 40;
    fiobj_str_capa_assert(c, capa);
    t = fiobj_obj2cstr(c);
  }
  memcpy(t.data + len, "Max-Age=", 8);
  len += 8;
  len += fio_ltoa(t.data + len, cookie.max_age, 10);
  t.data[len++] = ';';
  fiobj_str_resize(c, len);

  if (cookie.domain && cookie.domain_len) {
    fiobj_str_write(c, "domain=", 7);
    fiobj_str_write(c, cookie.domain, cookie.domain_len);
    fiobj_str_write(c, ";", 1);
  }
  if (cookie.path && cookie.path_len) {
    fiobj_str_write(c, "path=", 5);
    fiobj_str_write(c, cookie.path, cookie.path_len);
    fiobj_str_write(c, ";", 1);
  }
  if (cookie.http_only) {
    fiobj_str_write(c, "HttpOnly;", 9);
  }
  if (cookie.secure) {
    fiobj_str_write(c, "secure;", 7);
  }
  static fiobj_s *sym = NULL;
  if (!sym)
    sym = HTTP_HEADER_SET_COOKIE;
  set_header_add(h->private_data.out_headers, sym, c);
  return 0;
}

#define http_set_cookie(http__req__, ...)                                      \
  http_set_cookie((http__req__), (http_cookie_args_s){__VA_ARGS__})
/**
 * Sends the response headers and body.
 *
 * Returns -1 on error and 0 on success.
 *
 * AFTER THIS FUNCTION IS CALLED, THE `http_s` OBJECT IS NO LONGER VALID.
 */
int http_send_body(http_s *r, void *data, uintptr_t length) {
  if (!r->private_data.out_headers)
    return -1;
  add_content_length(r, length);
  add_date(r);
  int ret = ((http_protocol_s *)r->private_data.owner)
                ->vtable->http_send_body(r, data, length);
  return ret;
}
/**
 * Sends the response headers and the specified file (the response's body).
 *
 * Returns -1 on error and 0 on success.
 *
 * AFTER THIS FUNCTION IS CALLED, THE `http_s` OBJECT IS NO LONGER VALID.
 */
int http_sendfile(http_s *r, int fd, uintptr_t length, uintptr_t offset) {
  if (!r->private_data.out_headers) {
    close(fd);
    return -1;
  };
  add_content_length(r, length);
  add_date(r);
  int ret = ((http_protocol_s *)r->private_data.owner)
                ->vtable->http_sendfile(r, fd, length, offset);
  return ret;
}
/**
 * Sends the response headers and the specified file (the response's body).
 *
 * Returns -1 eton error and 0 on success.
 *
 * AFTER THIS FUNCTION IS CALLED, THE `http_s` OBJECT IS NO LONGER VALID.
 */
int http_sendfile2(http_s *r, fiobj_s *filename) {
  if (!r->private_data.out_headers)
    return -1;
  struct stat file_data = {.st_size = 0};
  static uint64_t accept_enc_hash = 0;
  if (!accept_enc_hash)
    accept_enc_hash = fiobj_sym_hash("accept-encoding", 15);
  static uint64_t range_hash = 0;
  if (!range_hash)
    range_hash = fiobj_sym_hash("range", 5);

  int file = -1;
  uint8_t is_gz = 0;

  fio_cstr_s s = fiobj_obj2cstr(filename);
  {
    fiobj_s *tmp = fiobj_hash_get3(r->headers, accept_enc_hash);
    if (!tmp)
      goto no_gzip_support;
    fio_cstr_s ac_str = fiobj_obj2cstr(tmp);
    if (!strstr(ac_str.data, "gzip"))
      goto no_gzip_support;
    if (s.data[s.len - 2] != '.' || s.data[s.len - 2] != 'g' ||
        s.data[s.len - 1] != 'z') {
      fiobj_str_write(filename, ".gz", 3);
      fio_cstr_s s = fiobj_obj2cstr(filename);
      if (!stat(s.data, &file_data) &&
          (S_ISREG(file_data.st_mode) || S_ISLNK(file_data.st_mode))) {
        is_gz = 1;
        goto found_file;
      }
      fiobj_str_resize(filename, s.len - 3);
    }
  }
no_gzip_support:
  if (stat(s.data, &file_data) ||
      !(S_ISREG(file_data.st_mode) || S_ISLNK(file_data.st_mode)))
    return -1;
found_file:
  /* set last-modified */
  {
    fiobj_s *tmp = fiobj_str_buf(32);
    fiobj_str_resize(
        tmp, http_time2str(fiobj_obj2cstr(tmp).data, file_data.st_mtime));
    http_set_header(r, HTTP_HEADER_LAST_MODIFIED, tmp);
  }
  /* set cache-control */
  http_set_header(r, HTTP_HEADER_CACHE_CONTROL, fiobj_dup(HTTP_HVALUE_MAX_AGE));
  /* set & test etag */
  uint64_t etag = (uint64_t)file_data.st_size;
  etag ^= (uint64_t)file_data.st_mtime;
  etag = fiobj_sym_hash(&etag, sizeof(uint64_t));
  fiobj_s *etag_str = fiobj_str_buf(32);
  fiobj_str_resize(etag_str,
                   fio_base64_encode(fiobj_obj2cstr(etag_str).data,
                                     (void *)&etag, sizeof(uint64_t)));
  /* set */
  http_set_header(r, HTTP_HEADER_ETAG, etag_str);
  /* test */
  {
    static uint64_t none_match_hash = 0;
    if (!none_match_hash)
      none_match_hash = fiobj_sym_hash("if-none-match", 13);
    fiobj_s *tmp2 = fiobj_hash_get3(r->headers, none_match_hash);
    if (tmp2 && fiobj_iseq(tmp2, etag_str)) {
      r->status = 304;
      http_finish(r);
      return 0;
    }
  }
  /* handle range requests */
  int64_t offset = 0;
  int64_t length = file_data.st_size;
  {
    static uint64_t ifrange_hash = 0;
    if (!ifrange_hash)
      ifrange_hash = fiobj_sym_hash("if-range", 8);
    fiobj_s *tmp = fiobj_hash_get3(r->headers, ifrange_hash);
    if (tmp && fiobj_iseq(tmp, etag_str)) {
      fiobj_hash_delete3(r->headers, range_hash);
    } else {
      tmp = fiobj_hash_get3(r->headers, range_hash);
      if (tmp) {
        /* range ahead... */
        if (tmp->type == FIOBJ_T_ARRAY)
          tmp = fiobj_ary_index(tmp, 0);
        fio_cstr_s range = fiobj_obj2cstr(tmp);
        if (!range.data || memcmp("bytes=", range.data, 6))
          goto open_file;
        char *pos = range.data + 6;
        int64_t start_at = 0, end_at = 0;
        start_at = fio_atol(&pos);
        if (start_at >= file_data.st_size)
          goto open_file;
        if (start_at >= 0) {
          pos++;
          end_at = fio_atol(&pos);
          if (end_at <= 0)
            goto open_file;
        }
        /* we ignore multimple ranges, only responding with the first range. */
        if (start_at < 0) {
          if (0 - start_at < file_data.st_size) {
            offset = file_data.st_size - start_at;
            length = 0 - start_at;
          }
        } else if (end_at) {
          offset = start_at;
          length = end_at - start_at + 1;
          if (length + start_at > file_data.st_size || length <= 0)
            length = length - start_at;
        } else {
          offset = start_at;
          length = length - start_at;
        }
        r->status = 206;

        http_set_header(r, HTTP_HEADER_CONTENT_RANGE,
                        fiobj_strprintf("bytes %lu-%lu/%lu",
                                        (unsigned long)start_at,
                                        (unsigned long)(start_at + length - 1),
                                        (unsigned long)file_data.st_size));
        http_set_header(r, HTTP_HEADER_ACCEPT_RANGES,
                        fiobj_dup(HTTP_HVALUE_BYTES));
      }
    }
  }
open_file:
  s = fiobj_obj2cstr(filename);
  file = open(s.data, O_RDONLY);
  if (file == -1) {
    fprintf(stderr, "ERROR: Couldn't open file %s!\n", s.data);
    http_send_error(r, 500);
    return 0;
  }
  {
    fiobj_s *tmp = NULL;
    uintptr_t pos = 0;
    if (is_gz) {
      http_set_header(r, HTTP_HEADER_CONTENT_ENCODING,
                      fiobj_dup(HTTP_HVALUE_GZIP));

      pos = s.len - 4;
      while (pos && s.data[pos] != '.')
        pos--;
      pos++; /* assuming, but that's fine. */
      tmp = http_mimetype_find(s.data + pos, s.len - pos - 4);

    } else {
      pos = s.len - 1;
      while (pos && s.data[pos] != '.')
        pos--;
      pos++; /* assuming, but that's fine. */
      tmp = http_mimetype_find(s.data + pos, s.len - pos);
    }
    if (tmp)
      http_set_header(r, HTTP_HEADER_CONTENT_TYPE, tmp);
  }
  return http_sendfile(r, file, length, offset);
}

/**
 * Sends an HTTP error response.
 *
 * Returns -1 on error and 0 on success.
 *
 * AFTER THIS FUNCTION IS CALLED, THE `http_s` OBJECT IS NO LONGER VALID.
 *
 * The `uuid` argument is optional and will be used only if the `http_s`
 * argument is set to NULL.
 */
int http_send_error(http_s *r, size_t error) {
  if (!r || !error || !r->private_data.out_headers)
    return -1;
  r->status = error;
  fiobj_s *fname = fiobj_str_new(
      ((http_protocol_s *)r->private_data.owner)->settings->public_folder,
      ((http_protocol_s *)r->private_data.owner)
          ->settings->public_folder_length);
  fiobj_str_write2(fname, "/%lu.html", (unsigned long)error);
  if (http_sendfile2(r, fname)) {
    http_set_header(r, HTTP_HEADER_CONTENT_TYPE, http_mimetype_find("txt", 3));
    fiobj_str_resize(fname, 0);
    fio_cstr_s t = http_status2str(error);
    http_send_body(r, t.data, t.len);
  }
  fiobj_free(fname);
  return 0;
}

/**
 * Sends the response headers and starts streaming. Use `http_defer` to
 * continue straming.
 *
 * Returns -1 on error and 0 on success.
 */
int http_stream(http_s *r, void *data, uintptr_t length) {
  return ((http_protocol_s *)r->private_data.owner)
      ->vtable->http_stream(r, data, length);
}
/**
 * Sends the response headers for a header only response.
 *
 * AFTER THIS FUNCTION IS CALLED, THE `http_s` OBJECT IS NO LONGER VALID.
 */
void http_finish(http_s *r) {
  ((http_protocol_s *)r->private_data.owner)->vtable->http_finish(r);
}
/**
 * Pushes a data response when supported (HTTP/2 only).
 *
 * Returns -1 on error and 0 on success.
 */
int http_push_data(http_s *r, void *data, uintptr_t length,
                   fiobj_s *mime_type) {
  return ((http_protocol_s *)r->private_data.owner)
      ->vtable->http_push_data(r, data, length, mime_type);
}
/**
 * Pushes a file response when supported (HTTP/2 only).
 *
 * If `mime_type` is NULL, an attempt at automatic detection using
 * `filename` will be made.
 *
 * Returns -1 on error and 0 on success.
 */
int http_push_file(http_s *h, fiobj_s *filename, fiobj_s *mime_type) {
  return ((http_protocol_s *)h->private_data.owner)
      ->vtable->http_push_file(h, filename, mime_type);
}

/**
 * Defers the request / response handling for later.
 *
 * Returns -1 on error and 0 on success.
 */
int http_defer(http_s *h, void (*task)(http_s *h),
               void (*fallback)(http_s *h)) {
  return ((http_protocol_s *)h->private_data.owner)
      ->vtable->http_defer(h, task, fallback);
}

/**
 * Upgrades an HTTP/1.1 connection to a Websocket connection.
 */
#undef http_upgrade2ws
void http_upgrade2ws(websocket_settings_s args) {
  if (!args.http || !args.http->headers) {
    fprintf(stderr,
            "ERROR: `http_upgrade2ws` requires a valid `http_s` handle.");
    return;
  }
  ((http_protocol_s *)args.http->private_data.owner)
      ->vtable->http2websocket(&args);
}

/* *****************************************************************************
Listening to HTTP connections
*****************************************************************************
*/

static void http_on_open(intptr_t uuid, void *set) {
  static __thread ssize_t capa = 0;
  if (!capa)
    capa = sock_max_capacity();
  facil_set_timeout(uuid, ((http_settings_s *)set)->timeout);
  if (sock_uuid2fd(uuid) + HTTP_BUSY_UNLESS_HAS_FDS >= capa) {
    fprintf(stderr, "WARNING: HTTP server at capacity\n");
    http_send_error2(uuid, 503, set);
    sock_close(uuid);
    return;
  }
  protocol_s *pr = http1_new(uuid, set, NULL, 0);
  if (!pr)
    sock_close(uuid);
  facil_attach(uuid, pr);
}

static void http_on_finish(intptr_t uuid, void *set) {
  http_settings_s *settings = set;

  if (settings->on_finish)
    settings->on_finish(settings);

  free((void *)settings->public_folder);
  free(settings);
  (void)uuid;
}

/**
 * Listens to HTTP connections at the specified `port`.
 *
 * Leave as NULL to ignore IP binding.
 *
 * Returns -1 on error and 0 on success.
 */
#undef http_listen
int http_listen(const char *port, const char *binding,
                struct http_settings_s arg_settings) {
  if (arg_settings.on_request == NULL) {
    fprintf(stderr, "ERROR: http_listen requires the .on_request parameter "
                    "to be set\n");
    kill(0, SIGINT), exit(11);
  }

  http_settings_s *settings = malloc(sizeof(*settings));
  *settings = arg_settings;

  if (!settings->max_body_size)
    settings->max_body_size = HTTP_DEFAULT_BODY_LIMIT;
  if (!settings->timeout)
    settings->timeout = 5;
  if (!settings->ws_max_msg_size)
    settings->ws_max_msg_size = 262144; /** defaults to ~250KB */
  if (!settings->ws_timeout)
    settings->ws_timeout = 40; /* defaults to 40 seconds */

  if (settings->public_folder) {
    settings->public_folder_length = strlen(settings->public_folder);
    if (settings->public_folder[0] == '~' &&
        settings->public_folder[1] == '/' && getenv("HOME")) {
      char *home = getenv("HOME");
      size_t home_len = strlen(home);
      char *tmp = malloc(settings->public_folder_length + home_len + 1);
      memcpy(tmp, home, home_len);
      if (home[home_len - 1] == '/')
        --home_len;
      memcpy(tmp + home_len, settings->public_folder + 1,
             settings->public_folder_length); // copy also the NULL
      settings->public_folder = tmp;
      settings->public_folder_length = strlen(settings->public_folder);
    } else {
      settings->public_folder = malloc(settings->public_folder_length + 1);
      memcpy((void *)settings->public_folder, arg_settings.public_folder,
             settings->public_folder_length);
      ((uint8_t *)settings->public_folder)[settings->public_folder_length] = 0;
    }
  }

  return facil_listen(.port = port, .address = binding,
                      .on_finish = http_on_finish, .on_open = http_on_open,
                      .udata = settings);
}
/** Listens to HTTP connections at the specified `port` and `binding`. */
#define http_listen(port, binding, ...)                                        \
  http_listen((port), (binding), (struct http_settings_s)(__VA_ARGS__))

/**
 * Returns the settings used to setup the connection.
 *
 * Returns NULL on error (i.e., connection was lost).
 */
struct http_settings_s *http_settings(http_s *r) {
  return ((http_protocol_s *)r->private_data.owner)->settings;
}

/* *****************************************************************************
TODO: HTTP client mode
*****************************************************************************
*/

/* *****************************************************************************
HTTP Helper functions that could be used globally
*****************************************************************************
*/

/**
 * Returns a String object representing the unparsed HTTP request (HTTP
 * version is capped at HTTP/1.1). Mostly usable for proxy usage and
 * debugging.
 */
fiobj_s *http_req2str(http_s *h) {
  if (!h->headers)
    return NULL;

  struct header_writer_s w;
  w.dest = fiobj_str_buf(4096);

  fiobj_str_join(w.dest, h->method);
  fiobj_str_write(w.dest, " ", 1);
  fiobj_str_join(w.dest, h->path);
  if (h->query) {
    fiobj_str_write(w.dest, "?", 1);
    fiobj_str_join(w.dest, h->query);
  }
  {
    fio_cstr_s t = fiobj_obj2cstr(h->version);
    if (t.len < 6 || t.data[5] != '1')
      fiobj_str_write(w.dest, " HTTP/1.1\r\n", 10);
    else {
      fiobj_str_write(w.dest, " ", 1);
      fiobj_str_join(w.dest, h->version);
      fiobj_str_write(w.dest, "\r\n", 2);
    }
  }

  fiobj_each1(h->headers, 0, write_header, &w);
  fiobj_str_write(w.dest, "\r\n", 2);
  if (h->body) {
    fiobj_io_seek(h->body, 0);
    fio_cstr_s t = fiobj_io_read(h->body, 0);
    fiobj_str_write(w.dest, t.data, t.len);
  }
  return w.dest;
}

void http_write_log(http_s *h) {
  fiobj_s *l = fiobj_str_buf(128);
  static uint64_t cl_hash = 0;
  if (!cl_hash)
    cl_hash = fiobj_sym_hash("content-length", 14);

  intptr_t bytes_sent =
      fiobj_obj2num(fiobj_hash_get3(h->private_data.out_headers, cl_hash));

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &end);
  start = facil_last_tick();

  fio_cstr_s buff = fiobj_obj2cstr(l);

  // TODO Guess IP address from headers (forwarded) where possible
  sock_peer_addr_s addrinfo = sock_peer_addr(
      sock_uuid2fd(((http_protocol_s *)h->private_data.owner)->uuid));
  if (addrinfo.addrlen) {
    if (inet_ntop(
            addrinfo.addr->sa_family,
            addrinfo.addr->sa_family == AF_INET
                ? (void *)&((struct sockaddr_in *)addrinfo.addr)->sin_addr
                : (void *)&((struct sockaddr_in6 *)addrinfo.addr)->sin6_addr,
            buff.data, 128))
      buff.len = strlen(buff.data);
  }
  if (buff.len == 0) {
    memcpy(buff.data, "[unknown]", 9);
    buff.len = 9;
  }
  memcpy(buff.data + buff.len, " - - [", 6);
  buff.len += 6;
  fiobj_str_resize(l, buff.len);
  {
    fiobj_s *date;
    spn_lock(&date_lock);
    date = fiobj_dup(current_date);
    spn_unlock(&date_lock);
    fiobj_str_join(l, current_date);
    fiobj_free(date);
  }
  fiobj_str_write(l, "] \"", 3);
  fiobj_str_join(l, h->method);
  fiobj_str_write(l, " ", 1);
  fiobj_str_join(l, h->path);
  fiobj_str_write(l, " ", 1);
  fiobj_str_join(l, h->version);
  fiobj_str_write(l, "\" ", 2);
  if (bytes_sent > 0)
    fiobj_str_write2(l, "%lu %luB ", (unsigned long)h->status, bytes_sent);
  else
    fiobj_str_write2(l, "%lu -- ", (unsigned long)h->status);

  bytes_sent = ((end.tv_sec - start.tv_sec) * 1000) +
               ((end.tv_nsec - start.tv_nsec) / 1000000);
  fiobj_str_write2(l, "%lums\r\n", (unsigned long)bytes_sent);
  buff = fiobj_obj2cstr(l);

  fwrite(buff.data, 1, buff.len, stderr);
}

/**
A faster (yet less localized) alternative to `gmtime_r`.

See the libc `gmtime_r` documentation for details.

Falls back to `gmtime_r` for dates before epoch.
*/
struct tm *http_gmtime(time_t timer, struct tm *tmbuf) {
  // static char* DAYS[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri",
  // "Sat"}; static char * Months = {  "Jan", "Feb", "Mar", "Apr", "May",
  // "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  static const uint8_t month_len[] = {
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, // nonleap year
      31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31  // leap year
  };
  if (timer < 0)
    return gmtime_r(&timer, tmbuf);
  ssize_t a, b;
  tmbuf->tm_gmtoff = 0;
  tmbuf->tm_zone = "UTC";
  tmbuf->tm_isdst = 0;
  tmbuf->tm_year = 70; // tm_year == The number of years since 1900
  tmbuf->tm_mon = 0;
  // for seconds up to weekdays, we build up, as small values clean up
  // larger values.
  a = (ssize_t)timer;
  b = a / 60;
  tmbuf->tm_sec = a - (b * 60);
  a = b / 60;
  tmbuf->tm_min = b - (a * 60);
  b = a / 24;
  tmbuf->tm_hour = a - (b * 24);
  // day of epoch was a thursday. Add + 4 so sunday == 0...
  tmbuf->tm_wday = (b + 4) % 7;
// tmp == number of days since epoch
#define DAYS_PER_400_YEARS ((400 * 365) + 97)
  while (b >= DAYS_PER_400_YEARS) {
    tmbuf->tm_year += 400;
    b -= DAYS_PER_400_YEARS;
  }
#undef DAYS_PER_400_YEARS
#define DAYS_PER_100_YEARS ((100 * 365) + 24)
  while (b >= DAYS_PER_100_YEARS) {
    tmbuf->tm_year += 100;
    b -= DAYS_PER_100_YEARS;
    if (((tmbuf->tm_year / 100) & 3) ==
        0) // leap century divisable by 400 => add leap
      --b;
  }
#undef DAYS_PER_100_YEARS
#define DAYS_PER_32_YEARS ((32 * 365) + 8)
  while (b >= DAYS_PER_32_YEARS) {
    tmbuf->tm_year += 32;
    b -= DAYS_PER_32_YEARS;
  }
#undef DAYS_PER_32_YEARS
#define DAYS_PER_8_YEARS ((8 * 365) + 2)
  while (b >= DAYS_PER_8_YEARS) {
    tmbuf->tm_year += 8;
    b -= DAYS_PER_8_YEARS;
  }
#undef DAYS_PER_8_YEARS
#define DAYS_PER_4_YEARS ((4 * 365) + 1)
  while (b >= DAYS_PER_4_YEARS) {
    tmbuf->tm_year += 4;
    b -= DAYS_PER_4_YEARS;
  }
#undef DAYS_PER_4_YEARS
  while (b >= 365) {
    tmbuf->tm_year += 1;
    b -= 365;
    if ((tmbuf->tm_year & 3) == 0) { // leap year
      if (b > 0) {
        --b;
        continue;
      } else {
        b += 365;
        --tmbuf->tm_year;
        break;
      }
    }
  }
  b++; /* day 1 of the year is 1, not 0. */
  tmbuf->tm_yday = b;
  if ((tmbuf->tm_year & 3) == 1) {
    // regular year
    for (size_t i = 0; i < 12; i++) {
      if (b <= month_len[i])
        break;
      b -= month_len[i];
      ++tmbuf->tm_mon;
    }
  } else {
    // leap year
    for (size_t i = 12; i < 24; i++) {
      if (b <= month_len[i])
        break;
      b -= month_len[i];
      ++tmbuf->tm_mon;
    }
  }
  tmbuf->tm_mday = b;
  return tmbuf;
}

static const char *DAY_NAMES[] = {"Sun", "Mon", "Tue", "Wed",
                                  "Thu", "Fri", "Sat"};
static const char *MONTH_NAMES[] = {"Jan ", "Feb ", "Mar ", "Apr ",
                                    "May ", "Jun ", "Jul ", "Aug ",
                                    "Sep ", "Oct ", "Nov ", "Dec "};
static const char *GMT_STR = "GMT";

size_t http_date2str(char *target, struct tm *tmbuf) {
  char *pos = target;
  uint16_t tmp;
  *(uint32_t *)pos = *((uint32_t *)DAY_NAMES[tmbuf->tm_wday]);
  pos[3] = ',';
  pos[4] = ' ';
  pos += 5;
  if (tmbuf->tm_mday < 10) {
    *pos = '0' + tmbuf->tm_mday;
    ++pos;
  } else {
    tmp = tmbuf->tm_mday / 10;
    pos[0] = '0' + tmp;
    pos[1] = '0' + (tmbuf->tm_mday - (tmp * 10));
    pos += 2;
  }
  *(pos++) = ' ';
  *(uint32_t *)pos = *((uint32_t *)MONTH_NAMES[tmbuf->tm_mon]);
  pos += 4;
  // write year.
  pos += fio_ltoa(pos, tmbuf->tm_year + 1900, 10);
  *(pos++) = ' ';
  tmp = tmbuf->tm_hour / 10;
  pos[0] = '0' + tmp;
  pos[1] = '0' + (tmbuf->tm_hour - (tmp * 10));
  pos[2] = ':';
  tmp = tmbuf->tm_min / 10;
  pos[3] = '0' + tmp;
  pos[4] = '0' + (tmbuf->tm_min - (tmp * 10));
  pos[5] = ':';
  tmp = tmbuf->tm_sec / 10;
  pos[6] = '0' + tmp;
  pos[7] = '0' + (tmbuf->tm_sec - (tmp * 10));
  pos += 8;
  pos[0] = ' ';
  *((uint32_t *)(pos + 1)) = *((uint32_t *)GMT_STR);
  pos += 4;
  return pos - target;
}

size_t http_date2rfc2822(char *target, struct tm *tmbuf) {
  char *pos = target;
  uint16_t tmp;
  *(uint32_t *)pos = *((uint32_t *)DAY_NAMES[tmbuf->tm_wday]);
  pos[3] = ',';
  pos[4] = ' ';
  pos += 5;
  if (tmbuf->tm_mday < 10) {
    *pos = '0' + tmbuf->tm_mday;
    ++pos;
  } else {
    tmp = tmbuf->tm_mday / 10;
    pos[0] = '0' + tmp;
    pos[1] = '0' + (tmbuf->tm_mday - (tmp * 10));
    pos += 2;
  }
  *(pos++) = '-';
  *(uint32_t *)pos = *((uint32_t *)MONTH_NAMES[tmbuf->tm_mon]);
  pos += 3;
  *(pos++) = '-';
  // write year.
  pos += fio_ltoa(pos, tmbuf->tm_year + 1900, 10);
  *(pos++) = ' ';
  tmp = tmbuf->tm_hour / 10;
  pos[0] = '0' + tmp;
  pos[1] = '0' + (tmbuf->tm_hour - (tmp * 10));
  pos[2] = ':';
  tmp = tmbuf->tm_min / 10;
  pos[3] = '0' + tmp;
  pos[4] = '0' + (tmbuf->tm_min - (tmp * 10));
  pos[5] = ':';
  tmp = tmbuf->tm_sec / 10;
  pos[6] = '0' + tmp;
  pos[7] = '0' + (tmbuf->tm_sec - (tmp * 10));
  pos += 8;
  pos[0] = ' ';
  *((uint32_t *)(pos + 1)) = *((uint32_t *)GMT_STR);
  pos += 4;
  return pos - target;
}

size_t http_date2rfc2109(char *target, struct tm *tmbuf) {
  char *pos = target;
  uint16_t tmp;
  *(uint32_t *)pos = *((uint32_t *)DAY_NAMES[tmbuf->tm_wday]);
  pos[3] = ',';
  pos[4] = ' ';
  pos += 5;
  if (tmbuf->tm_mday < 10) {
    *pos = '0' + tmbuf->tm_mday;
    ++pos;
  } else {
    tmp = tmbuf->tm_mday / 10;
    pos[0] = '0' + tmp;
    pos[1] = '0' + (tmbuf->tm_mday - (tmp * 10));
    pos += 2;
  }
  *(pos++) = ' ';
  *(uint32_t *)pos = *((uint32_t *)MONTH_NAMES[tmbuf->tm_mon]);
  pos += 4;
  // write year.
  pos += fio_ltoa(pos, tmbuf->tm_year + 1900, 10);
  *(pos++) = ' ';
  tmp = tmbuf->tm_hour / 10;
  pos[0] = '0' + tmp;
  pos[1] = '0' + (tmbuf->tm_hour - (tmp * 10));
  pos[2] = ':';
  tmp = tmbuf->tm_min / 10;
  pos[3] = '0' + tmp;
  pos[4] = '0' + (tmbuf->tm_min - (tmp * 10));
  pos[5] = ':';
  tmp = tmbuf->tm_sec / 10;
  pos[6] = '0' + tmp;
  pos[7] = '0' + (tmbuf->tm_sec - (tmp * 10));
  pos += 8;
  *pos++ = ' ';
  *pos++ = '-';
  *pos++ = '0';
  *pos++ = '0';
  *pos++ = '0';
  *pos++ = '0';
  *pos = 0;
  return pos - target;
}

/**
 * Prints Unix time to a HTTP time formatted string.
 *
 * This variation implements chached results for faster processeing, at the
 * price of a less accurate string.
 */
size_t http_time2str(char *target, const time_t t) {
  /* pre-print time every 1 or 2 seconds or so. */
  static __thread time_t cached_tick;
  static __thread char cached_httpdate[48];
  static __thread size_t chached_len;
  time_t last_tick = facil_last_tick().tv_sec;
  if ((t | 7) < last_tick) {
    /* this is a custom time, not "now", pass through */
    struct tm tm;
    http_gmtime(t, &tm);
    return http_date2str(target, &tm);
  }
  if (last_tick > cached_tick) {
    struct tm tm;
    cached_tick = last_tick | 1;
    http_gmtime(last_tick, &tm);
    chached_len = http_date2str(cached_httpdate, &tm);
  }
  memcpy(target, cached_httpdate, chached_len);
  return chached_len;
}

/* Credit to Jonathan Leffler for the idea of a unified conditional */
#define hex_val(c)                                                             \
  (((c) >= '0' && (c) <= '9')                                                  \
       ? ((c)-48)                                                              \
       : (((c) >= 'a' && (c) <= 'f') || ((c) >= 'A' && (c) <= 'F'))            \
             ? (((c) | 32) - 87)                                               \
             : ({                                                              \
                 return -1;                                                    \
                 0;                                                            \
               }))
static inline int hex2byte(uint8_t *dest, const uint8_t *source) {
  if (source[0] >= '0' && source[0] <= '9')
    *dest = (source[0] - '0');
  else if ((source[0] >= 'a' && source[0] <= 'f') ||
           (source[0] >= 'A' && source[0] <= 'F'))
    *dest = (source[0] | 32) - 87;
  else
    return -1;
  *dest <<= 4;
  if (source[1] >= '0' && source[1] <= '9')
    *dest |= (source[1] - '0');
  else if ((source[1] >= 'a' && source[1] <= 'f') ||
           (source[1] >= 'A' && source[1] <= 'F'))
    *dest |= (source[1] | 32) - 87;
  else
    return -1;
  return 0;
}

ssize_t http_decode_url(char *dest, const char *url_data, size_t length) {
  char *pos = dest;
  const char *end = url_data + length;
  while (url_data < end) {
    if (*url_data == '+') {
      // decode space
      *(pos++) = ' ';
      ++url_data;
    } else if (*url_data == '%') {
      // decode hex value
      // this is a percent encoded value.
      if (hex2byte((uint8_t *)pos, (uint8_t *)&url_data[1]))
        return -1;
      pos++;
      url_data += 3;
    } else
      *(pos++) = *(url_data++);
  }
  *pos = 0;
  return pos - dest;
}

ssize_t http_decode_url_unsafe(char *dest, const char *url_data) {
  char *pos = dest;
  while (*url_data) {
    if (*url_data == '+') {
      // decode space
      *(pos++) = ' ';
      ++url_data;
    } else if (*url_data == '%') {
      // decode hex value
      // this is a percent encoded value.
      if (hex2byte((uint8_t *)pos, (uint8_t *)&url_data[1]))
        return -1;
      pos++;
      url_data += 3;
    } else
      *(pos++) = *(url_data++);
  }
  *pos = 0;
  return pos - dest;
}

ssize_t http_decode_path(char *dest, const char *url_data, size_t length) {
  char *pos = dest;
  const char *end = url_data + length;
  while (url_data < end) {
    if (*url_data == '%') {
      // decode hex value
      // this is a percent encoded value.
      if (hex2byte((uint8_t *)pos, (uint8_t *)&url_data[1]))
        return -1;
      pos++;
      url_data += 3;
    } else
      *(pos++) = *(url_data++);
  }
  *pos = 0;
  return pos - dest;
}

ssize_t http_decode_path_unsafe(char *dest, const char *url_data) {
  char *pos = dest;
  while (*url_data) {
    if (*url_data == '%') {
      // decode hex value
      // this is a percent encoded value.
      if (hex2byte((uint8_t *)pos, (uint8_t *)&url_data[1]))
        return -1;
      pos++;
      url_data += 3;
    } else
      *(pos++) = *(url_data++);
  }
  *pos = 0;
  return pos - dest;
}

/* *****************************************************************************
Lookup Tables / functions
***************************************************************************** */
#include "fio_hashmap.h"

static fio_hash_s mime_types;

/** Registers a Mime-Type to be associated with the file extension. */
void http_mimetype_register(char *file_ext, size_t file_ext_len,
                            fiobj_s *mime_type_str) {
  if (!mime_types.map)
    fio_hash_new(&mime_types);
  uintptr_t hash = fiobj_sym_hash(file_ext, file_ext_len);
  fiobj_s *old = fio_hash_insert(&mime_types, hash, mime_type_str);
  fiobj_free(old);
}

/**
 * Finds the mime-type associated with the file extension.
 *  Remember to call `fiobj_free`.
 */
fiobj_s *http_mimetype_find(char *file_ext, size_t file_ext_len) {
  if (!mime_types.map)
    return NULL;
  uintptr_t hash = fiobj_sym_hash(file_ext, file_ext_len);
  return fiobj_dup(fio_hash_find(&mime_types, hash));
}

/** Clears the Mime-Type registry (it will be emoty afterthis call). */
void http_mimetype_clear(void) {
  if (!mime_types.map)
    return;
  FIO_HASH_FOR_LOOP(&mime_types, obj) { fiobj_free((void *)obj->obj); }
  fio_hash_free(&mime_types);
  last_date_added = 0;
  fiobj_free(current_date);
}

/**
* Create with Ruby using:

a = []
256.times {|i| a[i] = 1;}
('a'.ord..'z'.ord).each {|i| a[i] = 0;}
('A'.ord..'Z'.ord).each {|i| a[i] = 0;}
('0'.ord..'9'.ord).each {|i| a[i] = 0;}
"!#$%&'*+-.^_`|~".bytes.each {|i| a[i] = 0;}
p a; nil
"!#$%&'()*+-./:<=>?@[]^_`{|}~".bytes.each {|i| a[i] = 0;} # for values
p a; nil
*/
static char invalid_cookie_name_char[256] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 0, 1,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

static char invalid_cookie_value_char[256] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};

// clang-format off
#define HTTP_SET_STATUS_STR(status, str) [status-100] = { .buffer = (str), .length = (sizeof(str) - 1) }
// clang-format on

/** Returns the status as a C string struct */
fio_cstr_s http_status2str(uintptr_t status) {
  static const fio_cstr_s status2str[] = {
      HTTP_SET_STATUS_STR(100, "Continue"),
      HTTP_SET_STATUS_STR(101, "Switching Protocols"),
      HTTP_SET_STATUS_STR(102, "Processing"),
      HTTP_SET_STATUS_STR(200, "OK"),
      HTTP_SET_STATUS_STR(201, "Created"),
      HTTP_SET_STATUS_STR(202, "Accepted"),
      HTTP_SET_STATUS_STR(203, "Non-Authoritative Information"),
      HTTP_SET_STATUS_STR(204, "No Content"),
      HTTP_SET_STATUS_STR(205, "Reset Content"),
      HTTP_SET_STATUS_STR(206, "Partial Content"),
      HTTP_SET_STATUS_STR(207, "Multi-Status"),
      HTTP_SET_STATUS_STR(208, "Already Reported"),
      HTTP_SET_STATUS_STR(226, "IM Used"),
      HTTP_SET_STATUS_STR(300, "Multiple Choices"),
      HTTP_SET_STATUS_STR(301, "Moved Permanently"),
      HTTP_SET_STATUS_STR(302, "Found"),
      HTTP_SET_STATUS_STR(303, "See Other"),
      HTTP_SET_STATUS_STR(304, "Not Modified"),
      HTTP_SET_STATUS_STR(305, "Use Proxy"),
      HTTP_SET_STATUS_STR(306, "(Unused), "),
      HTTP_SET_STATUS_STR(307, "Temporary Redirect"),
      HTTP_SET_STATUS_STR(308, "Permanent Redirect"),
      HTTP_SET_STATUS_STR(400, "Bad Request"),
      HTTP_SET_STATUS_STR(403, "Forbidden"),
      HTTP_SET_STATUS_STR(404, "Not Found"),
      HTTP_SET_STATUS_STR(401, "Unauthorized"),
      HTTP_SET_STATUS_STR(402, "Payment Required"),
      HTTP_SET_STATUS_STR(405, "Method Not Allowed"),
      HTTP_SET_STATUS_STR(406, "Not Acceptable"),
      HTTP_SET_STATUS_STR(407, "Proxy Authentication Required"),
      HTTP_SET_STATUS_STR(408, "Request Timeout"),
      HTTP_SET_STATUS_STR(409, "Conflict"),
      HTTP_SET_STATUS_STR(410, "Gone"),
      HTTP_SET_STATUS_STR(411, "Length Required"),
      HTTP_SET_STATUS_STR(412, "Precondition Failed"),
      HTTP_SET_STATUS_STR(413, "Payload Too Large"),
      HTTP_SET_STATUS_STR(414, "URI Too Long"),
      HTTP_SET_STATUS_STR(415, "Unsupported Media Type"),
      HTTP_SET_STATUS_STR(416, "Range Not Satisfiable"),
      HTTP_SET_STATUS_STR(417, "Expectation Failed"),
      HTTP_SET_STATUS_STR(421, "Misdirected Request"),
      HTTP_SET_STATUS_STR(422, "Unprocessable Entity"),
      HTTP_SET_STATUS_STR(423, "Locked"),
      HTTP_SET_STATUS_STR(424, "Failed Dependency"),
      HTTP_SET_STATUS_STR(425, "Unassigned"),
      HTTP_SET_STATUS_STR(426, "Upgrade Required"),
      HTTP_SET_STATUS_STR(427, "Unassigned"),
      HTTP_SET_STATUS_STR(428, "Precondition Required"),
      HTTP_SET_STATUS_STR(429, "Too Many Requests"),
      HTTP_SET_STATUS_STR(430, "Unassigned"),
      HTTP_SET_STATUS_STR(431, "Request Header Fields Too Large"),
      HTTP_SET_STATUS_STR(500, "Internal Server Error"),
      HTTP_SET_STATUS_STR(501, "Not Implemented"),
      HTTP_SET_STATUS_STR(502, "Bad Gateway"),
      HTTP_SET_STATUS_STR(503, "Service Unavailable"),
      HTTP_SET_STATUS_STR(504, "Gateway Timeout"),
      HTTP_SET_STATUS_STR(505, "HTTP Version Not Supported"),
      HTTP_SET_STATUS_STR(506, "Variant Also Negotiates"),
      HTTP_SET_STATUS_STR(507, "Insufficient Storage"),
      HTTP_SET_STATUS_STR(508, "Loop Detected"),
      HTTP_SET_STATUS_STR(509, "Unassigned"),
      HTTP_SET_STATUS_STR(510, "Not Extended"),
      HTTP_SET_STATUS_STR(511, "Network Authentication Required"),
  };
  fio_cstr_s ret = (fio_cstr_s){.length = 0, .buffer = NULL};
  if (status >= 100 && status < sizeof(status2str) / sizeof(status2str[0]))
    ret = status2str[status - 100];
  if (!ret.buffer)
    ret = status2str[400];
  return ret;
}
#undef HTTP_SET_STATUS_STR