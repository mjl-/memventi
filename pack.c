#include "memventi.h"


void
unpackiheader(uchar *buf, IHeader *ih)
{
	memcpy(ih->indexscore, buf, Indexscoresize);
	buf += Indexscoresize;
	ih->type = GET8(buf);
	buf += 1;
	ih->offset = GET48(buf);
	buf += 6;
}


void
packiheader(uchar *buf, IHeader *ih)
{
	memcpy(buf, ih->indexscore, Indexscoresize);
	buf += Indexscoresize;
	PUT8(buf, ih->type);
	buf += 1;
	PUT48(buf, ih->offset);
	buf += 6;
}


void
toiheader(IHeader *ih, DHeader *dh, uvlong offset)
{
	memcpy(ih->indexscore, dh->score, Indexscoresize);
	ih->type = dh->type;
	ih->offset = offset;
}


char *
unpackdheader(uchar *buf, DHeader *dh)
{
	uint magic;
	static char errmsg[128];

	magic = GET32(buf);
	if(magic != Headermagic) {
		snprintf(errmsg, sizeof errmsg, "invalid data, magic wrong (have 0x%x, want 0x%x)", magic, Headermagic);
		return errmsg;
	}
	buf += 4;
	memcpy(dh->score, buf, Scoresize);
	buf += Scoresize;
	dh->type = GET8(buf);
	buf += 1;
	dh->size = GET16(buf);
	buf += 2;
	if(dh->size > Datamax)
		return "size too large";
	return nil;
}


void
packdheader(uchar *buf, DHeader *dh)
{
	PUT32(buf, Headermagic);
	buf += 4;
	memcpy(buf, dh->score, Scoresize);
	buf += Scoresize;
	PUT8(buf, dh->type);
	buf += 1;
	PUT16(buf, dh->size);
	buf += 2;
}


uvlong
getuvlong(uchar *data, uint bitoffset, uint bits)
{
	uvlong r;
	int bbits;
	int boff;
	uchar *p;

	r = 0;
	p = &data[bitoffset / 8];

	while(bits > 0) {
		boff = bitoffset % 8;
		bbits = MIN(bits, 8-boff);
		r = (r<<bbits) | ((p[0]>>(8-bbits-boff)) & ((1<<bbits) - 1));
		bitoffset += bbits;
		bits -= bbits;
		p++;
	}
	return r;
}


void
putuvlong(uchar *data, uvlong addr, uint bitoffset, uint bits)
{
	uchar *p;
	uchar v;
	int boff, bbits;

	p = &data[bitoffset/8];
	while(bits > 0) {
		boff = bitoffset % 8;
		bbits = MIN(bits, 8-boff);
		v = p[0];
		p[0] = v & ~((1<<(8-boff))-1);
		p[0] |= ((addr>>(bits-bbits)) & ~(1<<bbits)) << (8-boff-bbits);
		p[0] |= v & ((1<<(8-boff-bbits))-1);
		bitoffset += bbits;
		bits -= bbits;
		p++;
	}
}
