#define _FILE_OFFSET_BITS 64	/* sigh, for gnu libc */
#define _BSD_SOURCE
#define _XOPEN_SOURCE 600

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <syslog.h>
#include <stdarg.h>
#include <inttypes.h>
#include <pthread.h>

#include <openssl/sha.h>

/* openbsd defines these in sys/param.h */
#undef roundup
#undef MAX
#undef MIN

#include "dat.h"
#include "fns.h"
