#include "memventi.h"


static int
readstr(uchar **bufp, uchar *end, uchar *s, int slen)
{
	ushort len;
	uchar *buf;

	buf = *bufp;

	if(buf+2 > end)
		return 0;

	len = GET16(buf);
	buf += 2;
	if(s != nil) {
		if(buf+len > end || len+1 > slen)
			return 0;
		memcpy(s, buf, len);
		s[len] = '\0';
	}
	buf += len;
	*bufp = buf;
	return 1;
}

static int
readmem(uchar **bufp, uchar *end)
{
	ushort len;
	uchar *buf;

	buf = *bufp;

	if(buf+1 > end)
		return 0;
	len = GET8(buf);
	buf += 1;
	if(buf+len > end)
		return 0;
	buf += len;
	*bufp = buf;
	return 1;
}


static void
writestr(uchar *p, char *s, int *lenp)
{
	int slen;

	slen = strlen(s);
	PUT16(p, slen);
	p += 2;
	memcpy(p, s, slen);
	p += slen;
	*lenp = 2+slen;
}


int
readvmsg(FILE *f, Vmsg *m, uchar *buf)
{
	uchar *p;
	uchar *end;

	debug(LOG_DEBUG, "readvmsg: starting read");
	if(fread(buf, 1, 2, f) != 2)
		return 0;
	m->msize = GET16(buf);
	if(m->msize >= 8+Datamax)
		return 0;

	debug(LOG_DEBUG, "readvmsg: incoming message of %u bytes", (uint)m->msize);

	if(fread(buf, 1, m->msize, f) != m->msize)
		return 0;
	if(m->msize < 2)
		return 0;
	end = buf + m->msize;
	debug(LOG_DEBUG, "readvmsg: incoming message read");

	p = buf;
	m->op = GET8(p);
	p += 1;
	m->tag = GET8(p);
	p += 1;

	m->data = nil;
	switch(m->op) {
	case Thello:
		if(readstr(&p, end, nil, 0) == 0)
			return 0;
		if(readstr(&p, end, nil, 0) == 0)
			return 0;
		if(p+1 > end)
			return 0;
		p += 1;

		if(readmem(&p, end) == 0)	/* crypto */
			return 0;
		if(readmem(&p, end) == 0)	/* codec */
			return 0;
		break;
	case Tread:
		if(p+Scoresize+1+1+2 != end)
			return 0;
		memcpy(m->score, p, Scoresize);
		p += Scoresize;
		m->type = GET8(p);
		p += 1;
		p += 1;
		m->count = GET16(p);
		p += 2;
		break;
	case Twrite:
		if(p+1+3 > end)
			return 0;
		m->type = GET8(p);
		p += 1;
		p += 3;
		m->dsize = m->msize - 6;
		m->data = trymalloc(m->dsize);
		if(m->data == nil) {
			debug(LOG_DEBUG, "readvmsg: out of memory for write of %u bytes", (uint)m->dsize);
			return 0;
		}
		memcpy(m->data, p, m->dsize);
		break;
	case Tping:
	case Tsync:
	case Tgoodbye:
		break;
	default:
		return 0;
	}
	return 1;
}


int
writevmsg(int fd, Vmsg *m, uchar *buf)
{
	uchar *p;
	int len;
	int r, n;

	p = buf+4;
	switch(m->op) {
	case Rhello:
		writestr(p, "anonymous", &len);
		p += len;
		PUT8(p, 0);
		p += 1;
		PUT8(p, 0);
		p += 1;

		m->msize = 2+len+2;
		break;
	case Rread:
		memcpy(p, m->data, m->dsize);
		p += m->dsize;

		m->msize = 2+m->dsize;
		break;
	case Rwrite:
		memcpy(p, m->score, Scoresize);
		p += Scoresize;

		m->msize = 2+Scoresize;
		break;
	case Rerror:
		writestr(p, m->msg, &len);
		p += len;

		m->msize = 2+len;
		break;
	case Rping:
	case Rsync:
		m->msize = 2;
		break;
	default:
		syslog(LOG_EMERG, "writevmsg: missing case for op %d", m->op);
		abort();
		return 0;
	}

	p = buf;
	PUT16(p, m->msize);
	p += 2;
	PUT8(p, m->op);
	p += 1;
	PUT8(p, m->tag);
	p += 1;


	n = 2+m->msize;
	debug(LOG_DEBUG, "writevmsg: writing op %d msize %d", m->op, n);
	r = writen(fd, (char *)buf, n);
	if(r != n) {
		debug(LOG_DEBUG, "writen: wrote %d instead of %d", r, n);
		return 0;
	}

	return 1;
}
