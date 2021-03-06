// Copyright (c) 2015 Nuxi, https://nuxi.nl/
//
// This file is distributed under a 2-clause BSD license.
// See the LICENSE file for details.

// cloudabi-run - execute CloudABI programs safely
//
// The cloudabi-run utility can execute CloudABI programs with an exact
// set of file descriptors. It reads a YAML configuration from stdin.
// This data is converted to argument data (argdata_t) that can be
// accessed from program_main().
//
// !fd, !file and !socket nodes in the YAML file are converted to file
// descriptor entries in the argument data, meaning they will be
// available within the CloudABI process.

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <netinet/in.h>

#include <argdata.h>
#include <errno.h>
#include <fcntl.h>
#ifndef O_EXEC
#define O_EXEC O_RDONLY
#endif
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <program.h>
#include <stdarg.h>
#include <stdio.h>
#ifndef __NetBSD__
#include <stdnoreturn.h>
#else
#define noreturn _Noreturn
#endif
#include <string.h>
#include <unistd.h>
#include <yaml.h>

#include "../libemulator/emulate.h"
#include "../libemulator/posix.h"

#define TAG_PREFIX "tag:nuxi.nl,2015:cloudabi/"

static const argdata_t *parse_object(yaml_parser_t *parser);

// Obtains the next event from the YAML input stream.
static void get_event(yaml_parser_t *parser, yaml_event_t *event) {
  do {
    if (!yaml_parser_parse(parser, event)) {
      fprintf(stderr, "stdin:%zu:%zu: Parse error\n", parser->mark.line + 1,
              parser->mark.column + 1);
      exit(127);
    }
  } while (event->type == YAML_DOCUMENT_END_EVENT ||
           event->type == YAML_STREAM_END_EVENT);
}

// Terminates execution due to a parse error.
static noreturn void exit_parse_error(const yaml_event_t *event,
                                      const char *message, ...) {
  fprintf(stderr, "stdin:%zu:%zu: ", event->start_mark.line + 1,
          event->start_mark.column + 1);
  va_list ap;
  va_start(ap, message);
  vfprintf(stderr, message, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(127);
}

// Parses a boolean value.
static const argdata_t *parse_bool(yaml_event_t *event) {
  const char *value = (const char *)event->data.scalar.value;
  if (strcmp(value, "true") == 0) {
    yaml_event_delete(event);
    return &argdata_true;
  }
  if (strcmp(value, "false") == 0) {
    yaml_event_delete(event);
    return &argdata_false;
  }
  exit_parse_error(event, "Unknown boolean value: %s", value);
}

// Parses a file descriptor number.
static const argdata_t *parse_fd(yaml_event_t *event) {
  // Extract file descriptor number from data.
  const char *value = (const char *)event->data.scalar.value;
  unsigned long fd;
  if (strcmp(value, "stdout") == 0) {
    fd = STDOUT_FILENO;
  } else if (strcmp(value, "stderr") == 0) {
    fd = STDOUT_FILENO;
  } else {
    char *endptr;
    errno = 0;
    fd = strtoul(value, &endptr, 10);
    if (errno != 0 || endptr != value + event->data.scalar.length ||
        fd > INT_MAX)
      exit_parse_error(event, "Invalid file descriptor number");
  }

  // Validate that this descriptor actually exists. program_exec() does
  // not return which file descriptors are invalid, so we'd better check
  // this manually over here.
  struct stat sb;
  if (fstat(fd, &sb) != 0)
    exit_parse_error(event, "File descriptor %d: %s", fd, strerror(errno));
  yaml_event_delete(event);
  return argdata_create_fd(fd);
}

// Parses a file, opens it and returns a file descriptor number.
static const argdata_t *parse_file(const yaml_event_t *event,
                                   yaml_parser_t *parser) {
  const char *path = NULL;
  for (;;) {
    // Fetch key name and value.
    const argdata_t *key = parse_object(parser);
    if (key == NULL)
      break;
    const char *keystr;
    int error = argdata_get_str_c(key, &keystr);
    if (error != 0)
      exit_parse_error(event, "Bad attribute: %s", strerror(error));
    const argdata_t *value = parse_object(parser);

    if (strcmp(keystr, "path") == 0) {
      // Pathname.
      error = argdata_get_str_c(value, &path);
      if (error != 0)
        exit_parse_error(event, "Bad path attribute: %s", strerror(error));
    } else {
      exit_parse_error(event, "Unknown file attribute: %s", keystr);
    }
  }

  // Open the file.
  // TODO(ed): Make mode and rights adjustable.
  if (path == NULL)
    exit_parse_error(event, "Missing path attribute");
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    exit_parse_error(event, "Failed to open \"%s\": %s", path, strerror(errno));
  return argdata_create_fd(fd);
}

// Parses an integer value.
static const argdata_t *parse_int(yaml_event_t *event) {
  const char *value = (const char *)event->data.scalar.value;
  const char *value_end = value + event->data.scalar.length;

  // Determine the base of the number.
  int base;
  if (value[0] == '0' && value[1] == 'o') {
    value += 2;
    base = 8;
  } else if (value[0] == '0' && value[1] == 'x') {
    value += 2;
    base = 16;
  } else {
    base = 10;
  }

  // Try signed integer conversion.
  {
    intmax_t intval;
    char *endptr;
    errno = 0;
    intval = strtoimax(value, &endptr, base);
    if (errno == 0 && endptr == value_end)
      return argdata_create_int(intval);
  }

  // Try unsigned integer conversion.
  {
    uintmax_t uintval;
    char *endptr;
    errno = 0;
    uintval = strtoumax(value, &endptr, base);
    if (errno == 0 && endptr == value_end)
      return argdata_create_int(uintval);
  }

  // Integer value out of bounds.
  exit_parse_error(event, "Invalid integer value");
}

// Parses a map.
static const argdata_t *parse_map(yaml_parser_t *parser) {
  const argdata_t **keys = NULL, **values = NULL;
  size_t nentries = 0, space = 0;
  for (;;) {
    const argdata_t *ad = parse_object(parser);
    if (ad == NULL)
      break;
    if (nentries == space) {
      space = space < 8 ? 8 : space * 2;
      keys = realloc(keys, space * sizeof(keys[0]));
      values = realloc(values, space * sizeof(values[0]));
    }
    keys[nentries] = ad;
    values[nentries++] = parse_object(parser);
  }
  return argdata_create_map(keys, values, nentries);
}

// Parses a null value.
static const argdata_t *parse_null(yaml_event_t *event) {
  yaml_event_delete(event);
  return &argdata_null;
}

// Parses a sequence.
static const argdata_t *parse_seq(yaml_parser_t *parser) {
  const argdata_t **entries = NULL;
  size_t nentries = 0, space = 0;
  for (;;) {
    const argdata_t *ad = parse_object(parser);
    if (ad == NULL)
      break;
    if (nentries == space) {
      space = space < 8 ? 8 : space * 2;
      entries = realloc(entries, space * sizeof(entries[0]));
    }
    entries[nentries++] = ad;
  }
  return argdata_create_seq(entries, nentries);
}

// Parses a socket, creates it and returns a file descriptor number.
// TODO(ed): Add support for connecting sockets.
static const argdata_t *parse_socket(const yaml_event_t *event,
                                     yaml_parser_t *parser) {
  const char *typestr = "stream", *bindstr = NULL;
  for (;;) {
    // Fetch key name and value.
    const argdata_t *key = parse_object(parser);
    if (key == NULL)
      break;
    const char *keystr;
    int error = argdata_get_str_c(key, &keystr);
    if (error != 0)
      exit_parse_error(event, "Bad attribute: %s", strerror(error));
    const argdata_t *value = parse_object(parser);

    if (strcmp(keystr, "type") == 0) {
      // Socket type: stream, datagram, etc.
      error = argdata_get_str_c(value, &typestr);
      if (error != 0)
        exit_parse_error(event, "Bad type attribute: %s", strerror(error));
    } else if (strcmp(keystr, "bind") == 0) {
      // Address on which to bind.
      error = argdata_get_str_c(value, &bindstr);
      if (error != 0)
        exit_parse_error(event, "Bad bind attribute: %s", strerror(error));
    } else {
      exit_parse_error(event, "Unknown socket attribute: %s", keystr);
    }
  }

  // Parse the socket type.
  int type;
  if (strcmp(typestr, "dgram") == 0)
    type = SOCK_DGRAM;
  else if (strcmp(typestr, "seqpacket") == 0)
    type = SOCK_SEQPACKET;
  else if (strcmp(typestr, "stream") == 0)
    type = SOCK_STREAM;
  else
    exit_parse_error(event, "Unsupported type attribute: %s", typestr);

  // Parse the bind address.
  if (bindstr == NULL)
    exit_parse_error(event, "Missing bind attribute");
  const struct sockaddr *sa;
  socklen_t sal;
  struct sockaddr_un sun;
  struct addrinfo *res = NULL;
  if (bindstr[0] == '/') {
    // UNIX socket: bind to path.
    sun.sun_family = AF_UNIX;
    strncpy(sun.sun_path, bindstr, sizeof(sun.sun_path));
    if (sun.sun_path[sizeof(sun.sun_path) - 1] != '\0')
      exit_parse_error(event, "Socket path %s too long", bindstr);
    sa = (const struct sockaddr *)&sun;
    sal = sizeof(sun);
  } else {
    // IPv4 or IPv6 socket. Extract address and port number.
    const char *split, *servname;
    if (bindstr[0] == '[') {
      split = strstr(bindstr, "]:");
      servname = split + 2;
    } else {
      split = strchr(bindstr, ':');
      servname = split + 1;
    }
    if (split == NULL)
      exit_parse_error(event, "Address %s does not contain a port number",
                       bindstr);

    // Resolve address and port number.
    char *hostname = strndup(bindstr, split - bindstr);
    struct addrinfo hint = {.ai_family = AF_UNSPEC, .ai_socktype = type};
    int error = getaddrinfo(hostname, servname, &hint, &res);
    free(hostname);
    if (error != 0)
      exit_parse_error(event, "Failed to resolve %s: %s", bindstr,
                       gai_strerror(error));
    if (res->ai_next != NULL)
      exit_parse_error(event, "%s resolves to multiple addresses", bindstr);
    sa = res->ai_addr;
    sal = res->ai_addrlen;
  }

  // Create socket.
  int fd = socket(sa->sa_family, type, 0);
  if (fd == -1)
    exit_parse_error(event, "Failed to create socket for %s: %s", bindstr,
                     strerror(errno));

  // Bind.
  int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  if (bind(fd, sa, sal) == -1)
    exit_parse_error(event, "Failed to bind to %s: %s", bindstr,
                     strerror(errno));
  if (listen(fd, 0) == -1 && errno != EOPNOTSUPP)
    exit_parse_error(event, "Failed to listen on %s: %s", bindstr,
                     strerror(errno));

  if (res != NULL)
    freeaddrinfo(res);
  return argdata_create_fd(fd);
}

static const argdata_t *parse_object(yaml_parser_t *parser) {
  yaml_event_t event;
  get_event(parser, &event);
  const char *tag = (const char *)event.data.scalar.tag;
  switch (event.type) {
    case YAML_STREAM_START_EVENT:
      yaml_event_delete(&event);
      return parse_object(parser);
    case YAML_DOCUMENT_START_EVENT:
      yaml_event_delete(&event);
      return parse_object(parser);
    case YAML_MAPPING_START_EVENT:
      if (tag == NULL || strcmp(tag, YAML_MAP_TAG) == 0) {
        return parse_map(parser);
      } else if (strcmp(tag, TAG_PREFIX "file") == 0) {
        return parse_file(&event, parser);
      } else if (strcmp(tag, TAG_PREFIX "socket") == 0) {
        return parse_socket(&event, parser);
      } else {
        exit_parse_error(&event, "Unsupported tag for mapping: %s", tag);
      }
      yaml_event_delete(&event);
    case YAML_SCALAR_EVENT:
      if (tag == NULL || strcmp(tag, YAML_STR_TAG) == 0) {
        return argdata_create_str((const char *)event.data.scalar.value,
                                  event.data.scalar.length);
      } else if (strcmp(tag, YAML_BOOL_TAG) == 0) {
        return parse_bool(&event);
      } else if (strcmp(tag, YAML_INT_TAG) == 0) {
        return parse_int(&event);
      } else if (strcmp(tag, YAML_NULL_TAG) == 0) {
        return parse_null(&event);
      } else if (strcmp(tag, TAG_PREFIX "fd") == 0) {
        return parse_fd(&event);
      } else {
        exit_parse_error(&event, "Unsupported tag for scalar: %s", tag);
      }
    case YAML_SEQUENCE_START_EVENT:
      if (tag == NULL || strcmp(tag, YAML_SEQ_TAG) == 0) {
        return parse_seq(parser);
      } else {
        exit_parse_error(&event, "Unsupported tag for sequence: %s", tag);
      }
      yaml_event_delete(&event);
    case YAML_MAPPING_END_EVENT:
    case YAML_SEQUENCE_END_EVENT:
      yaml_event_delete(&event);
      return NULL;
    default:
      exit_parse_error(&event, "Unsupported event %d", event.type);
  }
}

static noreturn void usage(void) {
  fprintf(stderr, "usage: cloudabi-run [-e] executable\n");
  exit(127);
}

int main(int argc, char *argv[]) {
  // Parse command line options.
  bool do_emulate = false;
  char c;
  while ((c = getopt(argc, argv, "e")) != -1) {
    switch (c) {
      case 'e':
        // Run program using emulation.
        do_emulate = true;
        break;
      default:
        usage();
    }
  }
  argv += optind;
  argc -= optind;
  if (argc != 1)
    usage();

  // Parse YAML configuration.
  yaml_parser_t parser;
  yaml_parser_initialize(&parser);
  yaml_parser_set_input_file(&parser, stdin);
  const argdata_t *ad = parse_object(&parser);
  yaml_parser_delete(&parser);

  if (do_emulate) {
    // Serialize argument data that needs to be passed to the executable.
    size_t buflen, fdslen;
    argdata_get_buffer_length(ad, &buflen, &fdslen);
    int *fds = malloc(fdslen * sizeof(fds[0]) + buflen);
    if (fds == NULL) {
      perror("Cannot allocate argument data buffer");
      exit(127);
    }
    void *buf = &fds[fdslen];
    fdslen = argdata_get_buffer(ad, buf, fds);

    // Register file descriptors.
    struct fd_table ft;
    fd_table_init(&ft);
    for (size_t i = 0; i < fdslen; ++i) {
      if (!fd_table_insert_existing(&ft, i, fds[i])) {
        perror("Failed to register file descriptor in argument data");
        exit(127);
      }
    }

    // Call into the emulator to run the program inside of this process.
    // Throw a warning message before beginning execution, as emulation
    // is not considered secure.
    int fd = open(argv[0], O_RDONLY);
    if (fd == -1) {
      perror("Failed to open executable");
      return 127;
    }
    fprintf(stderr,
            "WARNING: Attempting to start executable using emulation.\n"
            "Keep in mind that this emulation provides no actual sandboxing.\n"
            "Though this is likely no problem for development and testing\n"
            "purposes, using this emulator in production is strongly\n"
            "discouraged.\n");

    emulate(fd, buf, buflen, &posix_syscalls);
  } else {
    // Execute the application directly through the operating system.
    int fd = open(argv[0], O_EXEC);
    if (fd == -1) {
      perror("Failed to open executable");
      return 127;
    }
    errno = program_exec(fd, ad);
  }
  perror("Failed to start executable");
  return 127;
}
