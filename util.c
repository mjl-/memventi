#include "memventi.h"


int debugflag = 0;


void
sha1(uchar *score, uchar *data, uint len)
{
	SHA1(data, len, score);
}

void *
lockedmalloc(ulong len)
{
	void *p, *alignedp;
	long pagesize;
	static int mlockwarn = 0;

	pagesize = sysconf(_SC_PAGESIZE);
	if(pagesize == -1)
		errsyslog(1, "sysconf pagesize");
	len = roundup(len, pagesize);
	p = malloc(len+pagesize-1);
	if(p == nil)
		return nil;
	alignedp = (void*)(((uintptr_t)p + pagesize - 1)&~(pagesize-1));
	if(mlock(alignedp, len) != 0 && mlockwarn == 0) {
		syslog(LOG_WARNING, "mlock failed on memory of len=%lu", len);
		mlockwarn++;
	}
	debug(LOG_DEBUG, "lockedmalloc, %lu bytes allocated", len);
	return alignedp;
}

void
errsyslog(int eval, const char *fmt, ...)
{
	va_list ap;
	char msg[256];
	int len;
	char *p;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);

	len = strlen(msg);
	if(len+1 < sizeof msg) {
		p = msg+len;
		snprintf(p, sizeof msg-len, ": %s", strerror(errno));
	}
	syslog(LOG_ERR, "%s", msg);
	exit(eval);
}


void
errxsyslog(int eval, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsyslog(LOG_ERR, fmt, ap);
	va_end(ap);
	exit(eval);
}


void
debug(int level, char *fmt, ...)
{

	va_list ap;
	char errmsg[256];

	if(!debugflag)
		return;

	va_start(ap, fmt);
	vsnprintf(errmsg, sizeof errmsg, fmt, ap);
	fprintf(stderr, "debug: %s\n", errmsg);
	va_end(ap);
}


uvlong
filesize(int fd)
{
	struct stat sb;
	if(fstat(fd, &sb) < 0)
		errsyslog(1, "fstat fd=%d", fd);
	return sb.st_size;
}


uvlong
roundup(uvlong n, uint round)
{
	assert((round & (round-1)) == 0);
	return (n+round-1)&~(round-1);
}


char *
dheaderfmt(DHeader *dh)
{
	static char buf[3][128];
	static int i = 0;
	char *p;

	p = buf[i];
	i = (i+1) % nelem(buf);

	snprintf(p, sizeof buf[0], "score=%s type=%d size=%d", scorestr(dh->score), (int)dh->type, (int)dh->size);

	return p;
}


static char
hex(uchar c)
{
	if(c <= 9)
		return '0'+c;
	if(c <= 0xf)
		return 'a'+c-0xa;
	errx(1, "wrong value (%u)", (uint)c);
}


char *
scorestr(uchar *score)
{
	static char bufs[3][2*Scoresize+1];
	static int index = 0;
	int i;
	char *s;

	s = bufs[index];
	index = (index+1) % 3;

	for(i = 0; i < Scoresize; i++) {
		s[2*i] = hex(score[i]>>4);
		s[2*i+1] = hex(score[i]&0xf);
	}
	s[2*Scoresize] = 0;

	return s;
}


void *
emalloc(ulong len)
{
	void *p;
	p = malloc(len);
	if(p == nil)
		errsyslog(1, "malloc");
	return p;
}


void *
trymalloc(ulong len)
{
	void *p;
	int i;
	for(i = 0; i < 10; i++) {
		p = malloc(len);
		if(p != nil)
			return p;
		sleep(1);
	}
	return nil;
}


void *
erealloc(void *p, ulong len)
{
	p = realloc(p, len);
	if(p == nil)
		errsyslog(1, "realloc");
	return p;
}


ssize_t
preadn(int fd, void *buf, size_t count, off_t offset)
{
	ssize_t have, r;

	have = 0;
	while(count > have) {
		r = pread(fd, buf, count-have, offset+have);
		if(r < 0)
			return r;
		if(r == 0)
			break;
		have += r;
	}
	return have;
}


ssize_t
writen(int fd, char *buf, size_t len)
{
	int n, r;

	n = len;
	while(n > 0) {
		r = write(fd, buf, n);
		if(r < 0)
			return r;
		n -= r;
		buf += r;
	}
	return len;
}


uvlong
msec(void)
{
	struct timeval tv;
	gettimeofday(&tv, nil);
	return tv.tv_sec * 1000 + tv.tv_usec/1000;
}


int
lockinit(Lock *l)
{
	return pthread_mutex_init(&l->lock, nil) == 0;
}

void
lock(Lock *l)
{
	pthread_mutex_lock(&l->lock);
}

void
unlock(Lock *l)
{
	pthread_mutex_unlock(&l->lock);
}


int
rwlockinit(RWLock *l)
{
	return pthread_rwlock_init(&l->rwlock, nil) == 0;
}

void
rlock(RWLock *l)
{
	pthread_rwlock_rdlock(&l->rwlock);
}

void
wlock(RWLock *l)
{
	pthread_rwlock_wrlock(&l->rwlock);
}

void
runlock(RWLock *l)
{
	pthread_rwlock_unlock(&l->rwlock);
}

void
wunlock(RWLock *l)
{
	runlock(l);
}
