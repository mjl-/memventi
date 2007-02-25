#define MAX(a, b)	((a) > (b) ? (a) : (b))
#define MIN(a, b)	((a) < (b) ? (a) : (b))
#define nil NULL
#define nelem(p)	((sizeof (p))/sizeof (p)[0])
#define GET8(p)		((p)[0])
#define GET16(p)        ((((uint16)((p)[0]))<<8)+((p)[1]))
#define GET24(p)        ((((uint32)GET8(p))<<16)+GET16((p)+1))
#define GET32(p)        ((((uint32)GET16(p))<<16)+GET16((p)+2))
#define GET48(p)        ((((uint64)GET16(p))<<32)+GET32((p)+2))
#define GET64(p)        ((((uint64)GET32(p))<<32)+GET32((p)+4))
#define PUT8(p, v)	((p)[0] = v)
#define PUT16(p, v)     (((p)[0] = (uchar)((v)>>8)), ((p)[1] = (uchar)(v)))
#define PUT32(p, v)     (PUT16((p), (uint16)((v)>>16)), PUT16((p)+2, (uint16)(v)))
#define PUT48(p, v)     (PUT16((p), (uint16)((v)>>32)), PUT32((p)+2, (uint32)(v)))
#define PUT64(p, v)     (PUT32((p), (uint32)((v)>>32)), PUT32((p)+4, (uint32)(v)))

/* pack.c */
void	unpackiheader(uchar *, IHeader *);
void	packiheader(uchar *, IHeader *);
void	toiheader(IHeader *, DHeader *, uvlong);
char	*unpackdheader(uchar *, DHeader *);
void	packdheader(uchar *, DHeader *);
uvlong	getuvlong(uchar *, uint, uint);
void	putuvlong(uchar *, uvlong, uint, uint);

/* util.c */
void	sha1(uchar *, uchar *, uint);
void	*lockedmalloc(ulong);
void	errsyslog(int, const char *, ...);
void	errxsyslog(int, const char *, ...);
void	debug(int, char *, ...);
uvlong	filesize(int);
uvlong	roundup(uvlong, uint);
char *scorestr(uchar *score);
char *dheaderfmt(DHeader *dh);
void	*emalloc(ulong);
void	*trymalloc(ulong);
void	*erealloc(void *, ulong);
ssize_t	preadn(int, void *, size_t, off_t);
ssize_t	writen(int, char *, size_t);
uvlong	msec(void);
int	lockinit(Lock *l);
void	lock(Lock *l);
void	unlock(Lock *l);
int	rwlockinit(RWLock *l);
void	rlock(RWLock *l);
void	wlock(RWLock *l);
void	runlock(RWLock *l);
void	wunlock(RWLock *l);

/* proto.c */
int	readvmsg(FILE *, Vmsg *, uchar *);
int	writevmsg(int, Vmsg *, uchar *);
