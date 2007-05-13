#include "memventi.h"


typedef struct Args Args;
typedef struct Chain Chain;
typedef struct Netaddr Netaddr;

enum {
	Listenmax	= 32,
	Addressesmax	= 16,
	Stacksize	= 32*1024,
};

enum {
	Srunning,	/* running normally */
	Sdegraded,	/* a write error has occured, disallow writes */
	Sclosing,	/* shutting down, stop handling requests */
};


struct Args {
	int fd;
	int allowwrite;
	uchar *buf;
};

struct Chain {
	Chain *next;
	uchar *data;
	uchar n;
} __attribute__((__packed__));

struct Netaddr {
	char *host;
	char *port;
};

static int fflag;
static int vflag;

static int datafd;
static int indexfd;
static uvlong datafilesize;
static uvlong indexfilesize;

static char *datafile = "data";
static char *indexfile = "index";

static Chain *heads;
static ulong nheads;
static uchar *mem;
static uchar *memend;
static uvlong nblocks;

static int headscorewidth;
static int entryscorewidth;
static int addrwidth;
static uvlong endaddr;
static int mementrysize;
static uint initheadlen;

static char *defaultport= "17034";

static RWLock htlock[256];
static Lock disklock;
static Lock statelock;
static int state;

static pthread_t readlistenthread[Listenmax];
static pthread_t writelistenthread[Listenmax];
static pthread_t syncprocthread;
static int nreadlistens, nwritelistens;

static uvlong nlookups;
static uvlong diskhisto[Addressesmax];


static uchar zeroscore[Scoresize] = {
	0xda, 0x39, 0xa3, 0xee, 0x5e, 0x6b, 0x4b, 0xd, 0x32, 0x55,
	0xbf, 0xef, 0x95, 0x60, 0x18, 0x90, 0xaf, 0xd8, 0x7, 0x9
};


static uvlong
getaddr(Chain *c, int i)
{
	return getuvlong(c->data, 8*c->n+(entryscorewidth+addrwidth)*i+entryscorewidth, addrwidth);
}


static int
isscore(uchar *score, Chain *c, int i)
{
	uvlong n1, n2;

	n1 = getuvlong(score, headscorewidth, entryscorewidth);
	n2 = getuvlong(c->data, c->n*8+(entryscorewidth+addrwidth)*i, entryscorewidth);
	return n1 == n2;
}


static int
istype(uchar type, Chain *c, int i)
{
	return c->data[i] == type;
}


static int
isend(Chain *c, int i)
{
	return getaddr(c, i) == endaddr;
}


static void
disklookuphisto(void)
{
	int i;

	printf("disk lookup histogram:\n");
	printf("count   frequency:\n");
	for(i = 0; i < nelem(diskhisto); i++) {
		if(diskhisto[i] != 0)
			printf("%7d  %llu\n", i, diskhisto[i]);
	}
	printf("total memory lookups: %llu\n", nlookups);
}


static ulong
headcount(Chain *c)
{
	ulong n;
	int i;
	uchar *p;

	n = 0;
	for(; c->next != nil; c = c->next)
		n += c->n;

	p = c->data;
	for(i = 0; i < c->n; i++)
		if(isend(c, i))
			break;
		else
			n++;
	return n;
}


static void
headhisto(void)
{
	ulong *freqs;
	ulong i, j;
	ulong lastindex;
	ulong index;

	freqs = emalloc(sizeof freqs[0]);
	lastindex = 0;
	freqs[0] = 0;
	for(i = 0; i < nelem(htlock); i++)
		rlock(&htlock[i]);
	for(i = 0; i < nheads; i++) {
		index = headcount(&heads[i]);
		if(index > lastindex)
			freqs = erealloc(freqs, sizeof freqs[0] * (index+1));
		for(j = lastindex+1; j <= index; j++)
			freqs[j] = 0;
		freqs[index] += 1;
		if(index > lastindex)
			lastindex = index;
	}
	for(i = 0; i < nelem(htlock); i++)
		runlock(&htlock[i]);

	printf("head length histogram:\n");
	printf("count    frequency\n");
	for(i = 0; i <= lastindex; i++) {
		if(freqs[i] != 0)
			printf("%7lu  %10lu\n", i, freqs[i]);
	}
	printf("nblocks: %llu\n", nblocks);
	free(freqs);
}


static int
lookup(uchar *score, uchar type, uvlong *addr)
{
	ulong index;
	Chain *c;
	int i, n;
	uvlong entryaddr;

	nlookups++;

	index = getuvlong(score, 0, headscorewidth);

	n = 0;
	for(c = &heads[index]; c != nil; c = c->next) {
		for(i = 0; i < c->n; i++) {
			entryaddr = getaddr(c, i);
			if(entryaddr == endaddr)
				break;
			if(!istype(type, c, i) || !isscore(score, c, i))
				continue;
			if(n >= Addressesmax)
				return -1;
			addr[n++] = entryaddr;
		}
	}
	return n;
}


static uchar *
bufalloc(int n)
{
	uchar *p;

	if(n == 0)
		return nil;

	if(mem == nil || mem+n >= memend) {
		mem = lockedmalloc(Bufallocsize);
		if(mem == nil)
			return nil;
		memend = mem+Bufallocsize;
		memset(mem, (uchar)0xff, Bufallocsize);
	}
	p = mem;
	mem += n;
	return p;
}


static Chain *
chainalloc(int n)
{
	static Chain *b = nil;
	static Chain *e = nil;
	Chain *newb;
	uchar *p;

	if(b == nil || b >= e) {
		newb = lockedmalloc(sizeof newb[0] * Chainallocn);
		if(newb == nil)
			return nil;
		b = newb;
		e = &b[Chainallocn];
	}
	n = MAX(n, Chainentriesmin);
	p = bufalloc(roundup(n*mementrysize, 8)/8);
	if(p == nil)
		return nil;
	b->data = p;
	b->next = nil;
	b->n = n;
	return b++;
}


static void
putentry(Chain *c, int i, uchar *score, uchar type, uvlong addr)
{
	int bitoffset;
	uvlong n;

	c->data[i] = type;
	bitoffset = 8*c->n+(entryscorewidth+addrwidth)*i;
	n = getuvlong(score, headscorewidth, entryscorewidth);
	putuvlong(c->data, n, bitoffset, entryscorewidth);
	putuvlong(c->data, addr, bitoffset+entryscorewidth, addrwidth);
}


static int
insert(uchar *score, uchar type, uvlong addr)
{
	ulong index;
	int i;
	Chain *c;
	int headlen;
	int nalloc;

	index = getuvlong(score, 0, headscorewidth);

	c = &heads[index];
	while(c->next != nil)
		c = c->next;

	for(i = 0; i < c->n; i++) {
		if(!isend(c, i))
			continue;
		putentry(c, i, score, type, addr);
		return 1;
	}

	if(c == &heads[index] && c->n == 0) {
		c->data = bufalloc(roundup(Chainentriesmin*mementrysize, 8)/8);
		if(c->data == nil)
			return 0;
		c->n = Chainentriesmin;
	} else {
		headlen = headcount(&heads[index]);
		nalloc = MIN(255, MAX(0, (int)initheadlen - headlen));
		nalloc = 0;
		c->next = chainalloc(nalloc);
		if(c->next == nil)
			return 0;
		c = c->next;
	}
	putentry(c, 0, score, type, addr);
	return 1;
}


static uvlong
disklookup(uvlong *addr, int naddr, uchar *score, uchar type, int readdata, uchar *data, DHeader *dh, char **errmsg)
{
	int i;
	char *err;
	uchar diskscore[Scoresize];
	uvlong offset;
	int n;
	int want;

	diskhisto[naddr] += 1;

	want = Diskdheadersize;
	if(readdata)
		want += 8*1024;

	*errmsg = nil;
	for(i = 0; i < naddr; i++) {
		offset = addr[i];
		n = preadn(datafd, data, want, offset);
		if(n <= 0) {
			*errmsg = "error reading header";
			syslog(LOG_WARNING, "disklookup: error reading header for block at offset=%llu, score=%s type=%d: %s",
				offset, scorestr(score), (int)type, (n < 0) ? strerror(errno) : "end of file");
			continue;
		}
		if(n < Diskdheadersize) {
			*errmsg = "short read for header";
			syslog(LOG_WARNING, "disklookup: short read for header for block at offset=%llu, have=%d, score=%s type=%d",
				offset, n, scorestr(score), (int)type);
			continue;
		}

		err = unpackdheader(data, dh);
		if(err != nil) {
			*errmsg = err;
			syslog(LOG_WARNING, "disklookup: unpacking header for block at offset=%llu, score=%s type=%d: %s",
				offset, scorestr(score), (int)type, *errmsg);
			continue;
		}

		if(memcmp(score, dh->score, Scoresize) != 0 || dh->type != type)
			continue;

		if(readdata) {
			if(dh->size > n-Diskdheadersize) {
				n = preadn(datafd, data, dh->size, offset+Diskdheadersize);
				if(n <= 0) {
					*errmsg = "disklookup: error reading data";
					syslog(LOG_WARNING, "error reading data for block at offset=%llu, score=%s type=%d: %s",
						offset, scorestr(score), (int)type, (n < 0) ? strerror(errno) : "end of file");
				}
				if(n != dh->size) {
					*errmsg = "disklookup: short read for data";
					syslog(LOG_WARNING, "short read for data for block at offset=%llu, have=%d, score=%s type=%d",
						offset, n, scorestr(score), (int)type);
				}
			} else {
				memmove(data, data+Diskdheadersize, dh->size);
			}
			sha1(diskscore, data, dh->size);
			if(memcmp(diskscore, score, Scoresize) != 0) {
				*errmsg = "score on disk invalid";
				syslog(LOG_ALERT, "disklookup: datafile %s has wrong score (has %s, claims %s) in block at offset=%llu size=%d type=%d",
					datafile, scorestr(diskscore), scorestr(dh->score), offset, (int)dh->size, (int)dh->type);
				return ~0ULL;
			}
		}
		return offset;
	}
	return ~0ULL;
}


static char *
indexstore(IHeader *ih)
{
	uchar ihbuf[Diskiheadersize];
	int n;
	static char errmsg[512];

	packiheader(ihbuf, ih);
	n = pwrite(indexfd, ihbuf, sizeof ihbuf, indexfilesize);
	if(n <= 0) {
		snprintf(errmsg, sizeof errmsg,
			"indexstore: writing header to indexfile %s at offset=%llu for datafile block at offset=%llu: %s",
			indexfile, indexfilesize, ih->offset, (n < 0) ? strerror(errno) : "end of file");
		syslog(LOG_ALERT, "%s", errmsg);
		return errmsg;
	}
	if(n != sizeof ihbuf) {
		snprintf(errmsg, sizeof errmsg,
			"indexstore: short write for header to indexfile %s at offset=%llu for datafile block at offset=%llu, "
			"dangling bytes at end of datafile %s",
			indexfile, indexfilesize, ih->offset, datafile);
		syslog(LOG_ALERT, "%s", errmsg);
		return errmsg;
	}
	return nil;
}


static uvlong
store(DHeader *dh, uchar *data)
{
	uchar buf[Diskdheadersize];
	uvlong offset;
	int n;
	IHeader ih;
	char *errmsg;

	packdheader(buf, dh);
	offset = datafilesize;

	debug(LOG_DEBUG, "writing data, offset=%llu size=%d", offset, (int)dh->size);

	n = pwrite(datafd, buf, sizeof buf, offset);
	if(n <= 0) {
		syslog(LOG_ALERT, "store: writing header to datafile %s, block at offset=%llu, %s: %s",
			datafile, offset, dheaderfmt(dh), (n < 0) ? strerror(errno) : "end of file");
		return ~0ULL;
	}
	if(n != sizeof buf) {
		syslog(LOG_ALERT, "store: short write for header, %d dangling bytes at end of datafile %s, block at offset=%llu, %s",
			n, datafile, offset, dheaderfmt(dh));
		return ~0ULL;
	}

	n = pwrite(datafd, data, dh->size, offset+Diskdheadersize);
	if(n <= 0) {
		syslog(LOG_ALERT, "store: writing data to datafile %s, block at offset=%llu, %s: %s",
			datafile, offset, dheaderfmt(dh), (n < 0) ? strerror(errno) : "end of file");
		return ~0ULL;
	}
	if(n != dh->size) {
		syslog(LOG_ALERT, "store: short write for data, %d dangling bytes at end of datafile %s, "
			"block at offset=%llu, %s, header for partly written block remains at end of file!",
			n+Diskdheadersize, datafile, offset, dheaderfmt(dh));
		return ~0ULL;
	}

	datafilesize += dh->size+Diskdheadersize;

	toiheader(&ih, dh, offset);
	errmsg = indexstore(&ih);
	if(errmsg != nil)
		return ~0ULL;

	nblocks++;
	return offset;
}


static int
safe_lookup(uchar *score, uchar type, uvlong *addr)
{
	int n;
	RWLock *htl;

	htl = &htlock[GET8(score)];
	rlock(htl);
	n = lookup(score, type, addr);
	runlock(htl);
	return n;
}


static void
safe_sync(void)
{
	lock(&disklock);
	fsync(datafd);
	fsync(indexfd);
	unlock(&disklock);
}


static void
stateset(int s)
{
	lock(&statelock);
	state = s;
	unlock(&statelock);
}

static int
stateget(void)
{
	int s;
	lock(&statelock);
	s = state;
	unlock(&statelock);
	return s;
}


static char *
readiheader(uvlong offset, IHeader *ih)
{
	uchar ihbuf[Diskiheadersize];
	int n;

	n = preadn(indexfd, ihbuf, sizeof ihbuf, offset);
	if(n < 0)
		return strerror(errno);
	if(n == 0)
		return "end of file";
	if(n != sizeof ihbuf)
		return "short read";
	unpackiheader(ihbuf, ih);
	return nil;
}


static char *
readblock(uvlong offset, DHeader *dh, uchar *data)
{
	int n;
	uchar dhbuf[Diskdheadersize];
	static char errmsg[128];
	char *msg;

	if(offset+Diskdheadersize > datafilesize)
		return "offset+size lies outside datafile";

	n = preadn(datafd, dhbuf, sizeof dhbuf, offset);
	if(n < 0) {
		snprintf(errmsg, sizeof errmsg, "error reading header: %s", strerror(errno));
		return errmsg;
	}
	if(n == 0)
		return "end of file while reading header";
	if(n != sizeof dhbuf)
		return "short read on header";

	msg = unpackdheader(dhbuf, dh);
	if(msg != nil) {
		snprintf(errmsg, sizeof errmsg, "parsing header: %s", msg);
		return errmsg;
	}

	n = preadn(datafd, data, dh->size, offset+Diskdheadersize);
	if(n < 0) {
		snprintf(errmsg, sizeof errmsg, "error reading data: %s", strerror(errno));
		return errmsg;
	}
	if(n == 0)
		return "end of file while reading data";
	if(n != dh->size)
		return "short read on data";

	return nil;
}


static void
init(void)
{
	uvlong end;
	uvlong ioffset, doffset;
	IHeader ih;
	DHeader dh;
	int n;
	int i;
	char *errmsg;
	uchar data[Datamax];
	uchar score[Scoresize];
	uchar ihbuf[Diskiheadersize];
	uvlong origiblocks;
	uvlong len;
	uvlong off;
	uvlong nindexadded;
	uvlong start, totalstart;
	uvlong dataread;

	totalstart = msec();

	datafd = open(datafile, O_RDWR|O_CREAT|O_APPEND, 0600);
	if(datafd < 0)
		errsyslog(1, "opening datafile %s", datafile);
	datafilesize = filesize(datafd);
	indexfd = open(indexfile, O_RDWR|O_CREAT|O_APPEND, 0600);
	if(indexfd < 0)
		errsyslog(1, "opening indexfile %s", indexfile);
	indexfilesize = filesize(indexfd);

	if(indexfilesize % Diskiheadersize != 0)
		errxsyslog(1, "indexfile size not multiple of index header size (%d)", (int)Diskiheadersize);

	/* check if last index entry is valid, if any */
	doffset = 0;
	if(indexfilesize > 0) {
		ioffset = indexfilesize-Diskiheadersize;
		errmsg = readiheader(ioffset, &ih);
		if(errmsg != nil)
			errxsyslog(1, "reading last header from index at offset=%llu: %s", ioffset, errmsg);

		if(ih.offset > datafilesize)
			errxsyslog(1, "last header at offset=%llu in index point past end of datafile at block at offset=%llu",
				ioffset, ih.offset);

		errmsg = readblock(ih.offset, &dh, data);
		if(errmsg != nil)
			errxsyslog(1, "error reading disk block at offset=%llu that indexfile at offset=%llu claims is the last: %s",
				ih.offset, ioffset, errmsg);

		sha1(score, data, dh.size);
		if(memcmp(score, dh.score, Scoresize) != 0)
			errxsyslog(1, "invalid score for block at offset=%llu in datafile, has %s, claims %s",
				ih.offset, scorestr(score), scorestr(dh.score));

		if(memcmp(ih.indexscore, dh.score, Indexscoresize) != 0)
			errxsyslog(1, "score in indexfile at offset=%llu does not match score in datafile at block at offset=%llu",
				ioffset, ih.offset);

		if(ih.type != dh.type)
			errxsyslog(1, "type in indexfile at offset=%llu does not match score in datafile at block at offset=%llu",
				ioffset, ih.offset);
		doffset = ih.offset+Diskdheadersize+dh.size;
	}

	origiblocks = indexfilesize / Diskiheadersize;

	/* read remaining datafile blocks (that are not in indexfile) and add to indexfile */
	dataread = 0;
	ioffset = indexfilesize;
	nindexadded = 0;
	start = msec();
	while(doffset < datafilesize) {
		errmsg = readblock(doffset, &dh, data);
		if(errmsg != nil)
			errxsyslog(1, "error reading block at offset=%llu (for adding to index): %s",
				doffset, errmsg);
		sha1(score, data, dh.size);
		if(memcmp(score, dh.score, Scoresize) != 0)
			errxsyslog(1, "invalid score for block at offset=%llu in datafile, has %s, claims %s (for adding to index)",
				doffset, scorestr(score), scorestr(dh.score));
		toiheader(&ih, &dh, doffset);
		errmsg = indexstore(&ih);
		if(errmsg != nil)
			errxsyslog(1, "could not store newly read datafile block at offset=%llu to indexfile at offset=%llu, %s",
				doffset, ioffset, dheaderfmt(&dh));
		indexfilesize += Diskiheadersize;
		ioffset += Diskiheadersize;
		doffset += Diskdheadersize+dh.size;

		dataread += Diskdheadersize+dh.size;
		nindexadded++;
	}
	syslog(LOG_NOTICE, "added %llu entries from datafile (%llu bytes in datafile) to indexfile, in %.3fs",
		nindexadded, dataread, (msec()-start)/1000.0);
	nblocks = indexfilesize / Diskiheadersize;

	nheads = 1<<headscorewidth;
	len = nheads * sizeof heads[0];
	heads = lockedmalloc(len);
	if(heads == nil)
		errsyslog(1, "malloc for initial heads, %llu bytes", len);
	debug(LOG_DEBUG, "%llu bytes allocated for heads", len);

	initheadlen = (nheads > 0) ? nblocks / nheads : 0;
	len = nheads * roundup(initheadlen*mementrysize, 8)/8;
	mem = malloc(len);
	if(mem == nil)
		errsyslog(1, "no memory for initial index entries");
	memset(mem, 0xff, len);
	memend = mem+len;
	debug(LOG_DEBUG, "%llu bytes allocated for initial entries in index", len);

	for(i = 0; i < nheads; i++) {
		heads[i].n = 0;
		heads[i].next = nil;
		heads[i].data = nil;
	}

	end = indexfilesize;
	off = 0;
	start = msec();
	while(off < end) {
		n = pread(indexfd, ihbuf, sizeof ihbuf, off);
		if(n <= 0)
			errxsyslog(1, "error reading indexfile offset=%llu", off);
		if(n != sizeof ihbuf)
			errxsyslog(1, "short read for indexfile offset=%llu, have=%d want=%d", off,
				(int)n, (int)sizeof ihbuf);
			
		unpackiheader(ihbuf, &ih);
		if(!insert(ih.indexscore, ih.type, ih.offset))
			errxsyslog(1, "error inserting in memory for indexfile offset=%llu", off);
		off += sizeof ihbuf;
	}
	syslog(LOG_NOTICE, "init done, %llu bytes for heads, read %llu bytes in %.3fs from index, entire startup in %.3fs",
		len, indexfilesize, (msec()-start)/1000.0, (msec()-totalstart)/1000.0);

	for(i = 0; i < nelem(diskhisto); i++)
		diskhisto[i] = 0;

	if(!lockinit(&statelock))
		errxsyslog(1, "init statelock");
	if(!lockinit(&disklock))
		errxsyslog(1, "init disklock");
	for(i = 0; i < nelem(htlock); i++)
		if(!rwlockinit(&htlock[i]))
			errxsyslog(1, "init hash table lock");
}


static void *
connproc(void *p)
{
	int fd;
	FILE *f;
	Vmsg in, out;
	char buf[128];
	char *l;
	char handshake[] = "venti-02-simple\n";
	DHeader dh;
	int ok, okhdr;
	int len;
	int allowwrite;
	Args *args;
	int n;
	uvlong addr;
	uvlong addrs[Addressesmax];
	char *errmsg;
	RWLock *htl;
	uchar *databuf;

	args = (Args *)p;
	fd = args->fd;
	allowwrite = args->allowwrite;
	databuf = args->buf;
	free(p);

	in.data = nil;

	debug(LOG_DEBUG, "connproc: started, fd %d", fd);

	f = fdopen(fd, "r");
	if(f == nil) {
		debug(LOG_DEBUG, "connproc: fdopen on fd %d: %s", fd, strerror(errno));
		goto done;
	}

	if(write(fd, handshake, strlen(handshake)) != strlen(handshake)) {
		debug(LOG_DEBUG, "error writing protocol handshake: %s", strerror(errno));
		goto done;
	}

	l = fgets(buf, sizeof buf, f);
	if(l == nil || strncmp(l, "venti-02", 8) != 0) {
		debug(LOG_DEBUG, "error reading protocol handshake or wrong protocol version: %s",
			ferror(f) ? strerror(errno) : "eof");
		goto done;
	}
	if(l != nil && (len = strlen(l)) > 0 && l[len-1] == '\n')
		l[len-1] = '\0';
	debug(LOG_DEBUG, "connproc: have handshake version %s", l);

	if(readvmsg(f, &in, databuf) == 0) {
		debug(LOG_DEBUG, "error reading hello msg");
		goto done;
	}
	if(in.op != Thello) {
		debug(LOG_DEBUG, "first message not hello");
		goto done;
	}
	debug(LOG_DEBUG, "connproc: have hello message");
	out.op = in.op+1;
	out.tag = in.tag;
	if(writevmsg(fd, &out, databuf) == 0) {
		debug(LOG_DEBUG, "error writing hello venti reponse");
		goto done;
	}
	debug(LOG_DEBUG, "connproc: hello response written");

	out.data = nil;
	for(;;) {
		free(in.data);
		in.data = nil;
		errmsg = nil;

		if(readvmsg(f, &in, databuf) == 0)
			goto done;

		if(stateget() == Sclosing) {
			if(in.op == Tgoodbye)
				goto done;
			out.op = Rerror;
			out.msg = "venti shutting down";
			if(writevmsg(fd, &out, databuf) == 0)
				debug(LOG_DEBUG, "error writing venti shutdown message");
			goto done;
		}

		out.op = in.op+1;
		out.tag = in.tag;
		debug(LOG_DEBUG, "connproc: read message");

		switch(in.op) {
		case Thello:
			syslog(LOG_INFO, "read Thello after handshake");
			goto done;
			break;
		case Tread:
			debug(LOG_DEBUG, "request: op=read score=%s type=%d", scorestr(in.score), (int)in.type);
			if(memcmp(in.score, zeroscore, Scoresize) == 0) {
				out.data = nil;
				out.dsize = 0;
				break;
			}

			n = safe_lookup(in.score, in.type, addrs);
			if(n == 0) {
				out.op = Rerror;
				out.msg = "no such score/type";
				break;
			}
			if(n == -1) {
				out.op = Rerror;
				out.msg = "internal error (too many partial matches)";
				break;
			}
			addr = disklookup(addrs, n, in.score, in.type, 1, databuf, &dh, &errmsg);
			if(addr == ~0ULL) {
				out.op = Rerror;
				out.msg = "error retrieving data";
				break;
			}

			if(dh.size > in.count) {
				out.op = Rerror;
				out.msg = "data larger than requested";
			} else {
				out.data = trymalloc(dh.size);
				if(out.data == nil) {
					out.op = Rerror;
					out.msg = "out of memory";
					syslog(LOG_WARNING, "connproc: out of memory for read of size %u", (uint)dh.size);
					break;
				}
				memcpy(out.data, databuf, dh.size);
				out.dsize = dh.size;
			}
			break;
		case Twrite:
			if(!allowwrite) {
				out.op = Rerror;
				out.msg = "no write access";
				break;
			}
			if(stateget() == Sdegraded) {
				out.op = Rerror;
				out.msg = "cannot write";
				break;
			}

			if(in.dsize == 0) {
				memcpy(out.score, zeroscore, Scoresize);
				break;
			}

			sha1(out.score, in.data, in.dsize);
			debug(LOG_DEBUG, "request: op=write score=%s type=%d size=%d",
				scorestr(out.score), (int)in.type, (int)in.dsize);

			htl = &htlock[GET8(out.score)];
			wlock(htl);
			n = lookup(out.score, in.type, addrs);
			if(n == -1) {
				out.op = Rerror;
				out.msg = "internal error (too many partial matches)";
				wunlock(htl);
				break;
			}
			if(n > 0) {
				addr = disklookup(addrs, n, out.score, in.type, 0, databuf, &dh, &errmsg);
				if(addr != ~0ULL) {
					wunlock(htl);
					break;
				}
				if(errmsg != nil) {
					wunlock(htl);
					out.op = Rerror;
					out.msg = "internal error (could not confirm score presence)";
					break;
				}
			}
			okhdr = -1;
			memcpy(dh.score, out.score, Scoresize);
			dh.type = in.type;
			dh.size = in.dsize;

			if(datafilesize+Diskdheadersize+dh.size >= endaddr) {
				wunlock(htl);
				out.op = Rerror;
				out.msg = "data file is full";
				break;
			}

			lock(&disklock);
			addr = store(&dh, in.data);
			unlock(&disklock);

			ok = addr != ~0ULL;
			if(ok)
				okhdr = insert(out.score, in.type, addr);
			wunlock(htl);

			if(!ok) {
				stateset(Sdegraded);
				out.op = Rerror;
				out.msg = "error writing block";
				syslog(LOG_WARNING, "connproc: error writing data, degraded to read-only mode");
				break;
			}
			if(okhdr == 0) {
				stateset(Sdegraded);
				out.op = Rerror;
				out.msg = "out of memory";
				syslog(LOG_WARNING, "connproc: out of memory for storing index entry, "
					"data file was written, degraded to read-only mode");
				break;
			}
			break;
		case Tsync:
			if(allowwrite)
				safe_sync();
			break;
		case Tgoodbye:
			if(allowwrite)
				safe_sync();
			goto done;
		default:
			syslog(LOG_NOTICE, "invalid op %d", in.op);
			goto done;
			break;
		}

		debug(LOG_DEBUG, "connproc: have response for request");
		if(writevmsg(fd, &out, databuf) == 0) {
			debug(LOG_DEBUG, "error writing venti response");
			free(out.data);
			out.data = nil;
			goto done;
		}
		free(out.data);
		out.data = nil;
		debug(LOG_DEBUG, "connproc: response for request written");
	}

done:
	free(in.data);
	free(databuf);
	if(f != nil)
		fclose(f);
	close(fd);
	debug(LOG_DEBUG, "connproc: done");
	return nil;
}


static void *
listenproc(void *p)
{
	struct sockaddr_storage addr;
	socklen_t len;
	pthread_t *thread;
	Args *args;
	int fd;
	int listenfd, allowwrite;
	pthread_attr_t attrs;

	args = (Args *)p;
	listenfd = args->fd;
	allowwrite = args->allowwrite;
	free(p);

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, nil);

	syslog(LOG_NOTICE, "listenproc: accepting connections...");
	for(;;) {
		fd = accept(listenfd, (struct sockaddr *)&addr, &len);
		if(fd < 0)
			continue;

		thread = nil;
		args = malloc(sizeof args[0]);
		if(args == nil)
			goto error;
		args->fd = fd;
		args->allowwrite = allowwrite;
		args->buf = malloc(Datamax+8);
		if(args->buf == nil)
			goto error;

		thread = malloc(sizeof thread[0]);
		if(thread == nil)
			goto error;
		if(pthread_attr_init(&attrs) != 0
			|| pthread_attr_setstacksize(&attrs, Stacksize) != 0
			|| pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED) != 0)
			goto error;
		if(pthread_create(thread, &attrs, connproc, args) != 0)
			goto error;
		pthread_attr_destroy(&attrs);
		free(thread);
		continue;

	error:
		if(args != nil)
			free(args->buf);
		free(args);
		free(thread);
		syslog(LOG_WARNING, "listenproc: could not create process: %s", strerror(errno));
	}
	return nil;
}


static void *
syncproc(void *p)
{
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, nil);
	for(;;) {
		sleep(10);
		safe_sync();
	}
}


static void *
signalproc(void *p)
{
	int sig;
	sigset_t mask;
	int i;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGUSR1);
	sigaddset(&mask, SIGUSR2);

	for(;;) {
		sig = 0;
		if(sigwait(&mask, &sig) != 0)
			syslog(LOG_NOTICE, "sigwait returned an error, sig=%d: %s", sig, strerror(sig));

		switch(sig) {
		case SIGUSR1:
			headhisto();
			break;
		case SIGUSR2:
			disklookuphisto();
			break;
		case SIGINT:
		case SIGTERM:
			stateset(Sclosing);
			syslog(LOG_INFO, "closing down");
			for(i = 0; i < nreadlistens; i++)
				pthread_cancel(readlistenthread[i]);
			for(i = 0; i < nwritelistens; i++)
				pthread_cancel(writelistenthread[i]);

			for(i = 0; i < nelem(htlock); i++)
				wlock(&htlock[i]);
			lock(&disklock);
			pthread_cancel(syncprocthread);
			fsync(datafd);
			fsync(indexfd);
			syslog(LOG_NOTICE, "data and index flushed, exiting...");
			exit(0);
			break;
		default:
			syslog(LOG_NOTICE, "unexpected signal (%d)", sig);
		}
	}
}


int
dobind(Netaddr *netaddr)
{
	struct sockaddr bound;
	socklen_t boundlen;
	int gaierr;
	struct addrinfo *addrs;
        struct addrinfo localhints = { AI_PASSIVE, PF_INET, SOCK_STREAM, 0, 0, NULL, NULL, NULL };
	int listenfd;

	listenfd = socket(PF_INET, SOCK_STREAM, 0);
	if(listenfd < 0)
		errsyslog(1, "socket");

	gaierr = getaddrinfo(netaddr->host, netaddr->port, &localhints, &addrs);
	if(gaierr)
		errxsyslog(1, "getaddrinfo: %s", gai_strerror(gaierr));

	boundlen = addrs->ai_addrlen;
	memcpy(&bound, addrs->ai_addr, boundlen);
	freeaddrinfo(addrs);

	if(bind(listenfd, &bound, boundlen) != 0)
		errsyslog(1, "bind");

	if(listen(listenfd, 1) != 0)
		errsyslog(1, "listen");

	return listenfd;
}


void
startlisten(pthread_t *thread, int listenfd,  int allowwrite)
{
	Args *args;
	pthread_attr_t attrs;

	args = emalloc(sizeof args[0]);
	args->fd = listenfd;
	args->allowwrite = allowwrite;
	if(pthread_attr_init(&attrs) != 0
		|| pthread_attr_setstacksize(&attrs, Stacksize) != 0)
		errsyslog(1, "error setting stacksize for listenproc");
	if(pthread_create(thread, &attrs, listenproc, args) != 0)
		errsyslog(1, "error creating listenproc");
	pthread_attr_destroy(&attrs);
}


static void
usage(void)
{
	fprintf(stderr, "usage: memventi [-fvD] [-r host!port] [-w host!port] [-i indexfile] [-d datafile] headscorewidth entryscorewidth addrwidth\n");
	exit(1);
}


int
main(int argc, char *argv[])
{
	int ch;
	sigset_t mask;
	Netaddr readaddrs[Listenmax];
	Netaddr writeaddrs[Listenmax];
	Netaddr *netaddr;
	int readfds[Listenmax];
	int writefds[Listenmax];
	int i;
	pthread_attr_t attrs;

	fflag = 0;
	vflag = 0;
	nreadlistens = nwritelistens = 0;
	while((ch = getopt(argc, argv, "Dfvd:i:r:w:")) != -1) {
		switch(ch) {
		case 'D':
			debugflag = 1;
			break;
		case 'd':
			datafile = optarg;
			break;
		case 'f':
			fflag = 1;
			break;
		case 'i':
			indexfile = optarg;
			break;
		case 'r':
			if(nreadlistens == nelem(readaddrs))
				errxsyslog(1, "too many read-only hosts specified");
			netaddr = &readaddrs[nreadlistens++];
			netaddr->port = strrchr(optarg, '!');
			if(netaddr->port != nil)
				*netaddr->port++ = '\0';
			else
				netaddr->port = defaultport;
			netaddr->host = optarg;
			break;
		case 'w':
			if(nreadlistens == nelem(writeaddrs))
				errxsyslog(1, "too many read/write hosts specified");
			netaddr = &writeaddrs[nwritelistens++];
			netaddr->port = strrchr(optarg, '!');
			if(netaddr->port != nil)
				*netaddr->port++ = '\0';
			else
				netaddr->port = defaultport;
			netaddr->host = optarg;
			break;
		case 'v':
			vflag = 1;
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if(argc != 3)
		usage();

	headscorewidth = atoi(argv[0]);
	entryscorewidth = atoi(argv[1]);
	addrwidth = atoi(argv[2]);
	if(headscorewidth <= 0 || entryscorewidth <= 0 || addrwidth <= 0)
		usage();
	if(headscorewidth + entryscorewidth > Indexscoresize*8)
		errxsyslog(1, "too many bits in head and per entry, maximum is %d", Indexscoresize*8);
	endaddr = (1ULL<<addrwidth)-1;
	mementrysize = 8+entryscorewidth+addrwidth;

	if(nreadlistens == 0 && nwritelistens == 0) {
		writeaddrs[0].host = "localhost";
		writeaddrs[0].port = defaultport;
		nwritelistens++;
	}

	openlog("memventi", LOG_CONS|(fflag ? LOG_PERROR : 0), LOG_DAEMON);
	setlogmask(LOG_UPTO(vflag ? LOG_DEBUG : LOG_NOTICE));

	for(i = 0; i < nreadlistens; i++)
		readfds[i] = dobind(&readaddrs[i]);
	for(i = 0; i < nwritelistens; i++)
		writefds[i] = dobind(&writeaddrs[i]);

	sigemptyset(&mask);
	sigaddset(&mask, SIGPIPE);
	if(pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0)
		errsyslog(1, "pthread_sigmask");

	init();
	stateset(Srunning);

	if(!fflag)
		if(daemon(1, debugflag ? 1 : 0) != 0)
			errsyslog(1, "could not daemonize");

	for(i = 0; i < nreadlistens; i++)
		startlisten(&readlistenthread[i], readfds[i], 0);
	for(i = 0; i < nwritelistens; i++)
		startlisten(&writelistenthread[i], writefds[i], 1);

	if(pthread_attr_init(&attrs) != 0
		|| pthread_attr_setstacksize(&attrs, Stacksize) != 0)
		errsyslog(1, "error setting stacksize for listenproc");
	if(pthread_create(&syncprocthread, &attrs, syncproc, nil) != 0)
		errsyslog(1, "error creating syncproc");
	pthread_attr_destroy(&attrs);

	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGUSR1);
	sigaddset(&mask, SIGUSR2);
	if(pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0)
		errsyslog(1, "pthread_sigmask");
	signalproc(nil);
	return 0;
}
