#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

/* this library requires the request object and extends it. */
#include "http-request.h"
#include "http-objpool.h"
#include "http-status.h"
#include "http-mime-types.h"
#include "lib-server.h"

/* defined in the request header, and used here:
HTTP_HEAD_MAX_SIZE
*/

/**
The struct HttpResponse type will contain all the data required for handling the
response.

The response object and it's API are NOT thread-safe (it is assumed that no two
threads handle the same response at the same time).
*/
struct HttpResponse {
  /**
  The body's response length.

  If this isn't set manually, the first call to
  `HttpResponse.write_body` (and friends) will set the length to the length
  being written (which might be less then the total data sent, if the sending is
  fragmented).
  */
  size_t content_length;
  /**
  The HTTP date for the response (in seconds since epoche).

  Defaults to now (approximately, not exactly, uses cached data).

  The date will be automatically formatted to match the HTTP protocol
  specifications. It is better to avoid setting the "Date" header manualy.
  */
  time_t date;
  /**
  The actual header buffer - do not edit directly.

  The extra 248 bytes are for the status line and variable headers, such as the
  date, content-length and connection status, that are requireed by some clients
  and aren't always meaningful for a case-by-case consideration.
  */
  char header_buffer[HTTP_HEAD_MAX_SIZE + 248];
  /**
  The response status
  */
  int status;
  /**
  Metadata about the response's state - don't edit this data (except the opaque
  data, if needed).
  */
  struct {
    /**
    an HttpResponse class object identifier, used to validate that the response
    object pointer is actually pointing to a response object (only validated
    before storing the object in the pool or freeing the object's memory).
    */
    void* classUUID;
    /**
    The server through which the response will be sent.
    */
    server_pt server;
    /**
    The socket's fd, for sending the response.
    */
    int fd;
    /**
    A pointer to the header's writing position.
    */
    char* headers_pos;
    /**
    Set to true once the headers were sent.
    */
    unsigned headers_sent : 1;
    /**
    Reserved for future use.
    */
    unsigned date_written : 1;
    /**
    Set to true when the "Connection" header is written to the buffer.
    */
    unsigned connection_written : 1;
    /**
    Reserved for future use.
    */
    unsigned rsrv : 4;
    /**
    An opaque user data flag.
    */
    unsigned opaque : 1;

  } metadata;
};

/**
The HttpResponse library
========================

This library helps us to write HTTP valid responses, even when we do not know
the internals of the HTTP protocol.

The response object allows us to easily update the response status (all
responses start with the default 200 "OK" status code), write headers and cookie
data to the header buffer and send the response's body.

The response object also allows us to easily update the body size and send body
data or open files (which will be automatically closed once sending is done).

Before using any response object (usually performed before the server starts),
it is important to inialize the response object pool:

    HttpResponse.create_pool()

To destroy the pool (usually after the server is done), use:

    HttpResponse.destroy_pool()

As example flow for the response could be:

     // get an HttpRequest object
     struct HttpRequest * response = HttpResponse.new(request);
     // ... write headers and body, i.e.
     HttpResponse.write_header_cstr(response, "X-Data", "my data");
     HttpResponse.write_body(response, "Hello World!\r\n", 14);
     // release the object
     HttpResponse.destroy(response);


--
Thread-safety:

The response object and it's API are NOT thread-safe (it is assumed that no two
threads handle the same response at the same time).

Initializing and destroying the request object pool is NOT thread-safe.

---
Misc notes:
The response header's buffer size is limited and too many headers will fail the
response.

The response object allows us to easily update the response status (all
responses start with the default 200 "OK" status code), write headers and write
cookie data to the header buffer.

The response object also allows us to easily update the body size and send body
data or open files (which will be automatically closed once sending is done).

The response does NOT support chuncked encoding.

The following is the response API container, use:

     struct HttpRequest * response = HttpResponse.new(request);


---
Performance:

A note about using this library with the HTTP/1 protocol family (if this library
supports HTTP/2, in the future, the use of the response object will be required,
as it wouldn't be possible to handle the response manually):

Since this library safeguards against certain mistakes and manages an
internal header buffer, it comes at a performance cost (it adds a layer of data
copying to the headers).

This cost is mitigated by the optional use of a response object pool, so that it
actually saves us from using `malloc` for the headers - for some cases this is
faster.

In my performance tests, the greatest issue is this: spliting the headers from
the body means that the socket's buffer is under-utilized on the first call to
`send`, while sending the headers. While other operations incure minor costs,
this is the actual reason for degraded performance when using this library.

The order of performance should be considered as follows:

1. Destructive: Overwriting the request's header buffer with both the response
headers and the response data (small responses). Sending the data through the
socket using the `Server.write` function.

2. Using malloc to allocate enough memory for both the response's headers AND
it's body.  Sending the data through the socket using the `Server.write_move`
function.

3. Using the HttpResponse object to send the response.

Network issues and response properties might influence the order of performant
solutions.
*/
struct ___HttpResponse_class___ {
  /**
  Destroys the response object pool. This function ISN'T thread-safe.
  */
  void (*destroy_pool)(void);
  /**
  Creates the response object pool (unless it already exists). This function
  ISN'T thread-safe.
  */
  void (*create_pool)(void);
  /**
  Creates a new response object or recycles a response object from the response
  pool.

  returns NULL on failuer, or a pointer to a valid response object.
  */
  struct HttpResponse* (*new)(struct HttpRequest*);
  /**
  Destroys the response object or places it in the response pool for recycling.
  */
  void (*destroy)(struct HttpResponse*);
  /**
  The pool limit property (defaults to 64) sets the limit of the pool storage,
  making sure that excess memory used is cleared rather then recycled.
  */
  int pool_limit;
  /**
  Clears the HttpResponse object, linking it with an HttpRequest object (which
  will be used to set the server's pointer and socket fd).
  */
  void (*reset)(struct HttpResponse*, struct HttpRequest*);
  /** Gets a response status, as a string */
  char* (*status_str)(struct HttpResponse*);
  /**
  Writes a header to the response. This function writes only the requested
  number of bytes from the header value and can be used even when the header
  value doesn't contain a NULL terminating byte.

  If the header buffer is full or the headers were already sent (new headers
  cannot be sent), the function will return -1.

  On success, the function returns 0.
  */
  int (*write_header)(struct HttpResponse*,
                      const char* header,
                      const char* value,
                      size_t value_len);
  /**
  Writes a header to the response.

  This is equivelent to writing:

       HttpResponse.write_header(* response, header, value, strlen(value));

       If the header buffer is full or the headers were already sent (new
  headers
       cannot be sent), the function will return -1.

       On success, the function returns 0.
  */
  int (*write_header2)(struct HttpResponse*,
                       const char* header,
                       const char* value);
  /**
  Prints a string directly to the header's buffer, appending the header
  seperator (the new line marker '\r\n' should NOT be printed to the headers
  buffer).

  If the header buffer is full or the headers were already sent (new headers
  cannot be sent), the function will return -1.

  On success, the function returns 0.
  */
  int (*printf)(struct HttpResponse*, const char* format, ...);
  /**
  Sends the headers (if they weren't previously sent).

  If the connection was already closed, the function will return -1. On success,
  the function returns 0.
  */
  int (*send)(struct HttpResponse*);
  /**
  Sends the headers (if they weren't previously sent) and writes the data to the
  underlying socket.

  The body will be copied to the server's outgoing buffer.

  If the connection was already closed, the function will return -1. On success,
  the function returns 0.
  */
  int (*write_body)(struct HttpResponse*, const char* body, size_t length);
  /**
  Sends the headers (if they weren't previously sent) and writes the data to the
  underlying socket.

  The server's outgoing buffer will take ownership of the body and free it's
  memory using `free` once the data was sent.

  If the connection was already closed, the function will return -1. On success,
  the function returns 0.
  */
  int (*write_body_move)(struct HttpResponse*, const char* body, size_t length);
  /**
  Sends the headers (if they weren't previously sent) and writes the data to the
  underlying socket.

  The server's outgoing buffer will take ownership of the body and free it's
  memory using `free` once the data was sent.

  If the connection was already closed, the function will return -1. On success,
  the function returns 0.
  */
  int (*sendfile)(struct HttpResponse*, FILE* pf, size_t length);
  /**
  Closes the connection.
  */
  void (*close)(struct HttpResponse*);
} HttpResponse;

/* end include guard */
#endif /* HTTP_RESPONSE_H */