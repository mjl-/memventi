CC = cc
LD = cc
CFLAGS = -g -pthread -Wall
LDFLAGS = -static -g -pthread -Wall
LIBS = -lcrypto
NROFF = nroff -mandoc -Tutf8
# to remove escape characters, run through col -b

ofiles = pack.o util.o proto.o

.SUFFIXES: .c .o
.c.o:
	$(CC) $(CFLAGS) -c $<

all: memventi memventi.0

memventi: $(ofiles) memventi.o
	$(LD) $(LDFLAGS) -o $@ $(ofiles) memventi.o $(LIBS)

memventi.0: memventi.8
	$(NROFF) memventi.8 > memventi.0

clean:
	-rm -f memventi *.o memventi.0
