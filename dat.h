/* memventi.c */

typedef unsigned char uchar;
typedef unsigned long long uvlong;
typedef ushort uint16;
typedef uint uint32;
typedef uvlong uint64;


enum {
	Magicsize	= 4,
	Scoresize	= 20,
	Indexscoresize	= 8,
	Diskiheadersize	= Indexscoresize+1+6,
	Diskdheadersize	= Magicsize+Scoresize+1+2,

	Chainentriesmin	= 8,
	Bufallocsize	= 1*1024*1024,
	Chainallocn	= 8*1024,

	Datamax		= 56 * 1024,
	Stringmax	= 1024,

	Headermagic	= 0x2f9d81e5,
};


typedef struct DHeader DHeader;
typedef struct IHeader IHeader;

struct DHeader {
	uchar score[Scoresize];
	uchar type;
	ushort size;
};


struct IHeader {
	uchar indexscore[Indexscoresize];
	uchar type;
	uvlong offset;
};


/* proto.c */
enum {
	Rerror		= 1,
	Tping,
	Rping,
	Thello,
	Rhello,
	Tgoodbye,
	Rgoodbye,
	Tread		= 12,
	Rread,
	Twrite,
	Rwrite,
	Tsync,
	Rsync,
};

typedef struct Vmsg Vmsg;

struct Vmsg {
	ushort msize;
	uchar op;
	uchar tag;
	uchar score[Scoresize];
	uchar type;
	ushort count;
	char *msg;
	uchar *data;
	ushort dsize;
};


/* util.c */
typedef struct Lock Lock;
typedef struct RWLock RWLock;

struct Lock {
	pthread_mutex_t lock;
};

struct RWLock {
	pthread_rwlock_t rwlock;
};

extern int debugflag;
extern struct syslog_data sdata;
