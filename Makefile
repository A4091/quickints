CC=m68k-amigaos-gcc
CFLAGS=-Wall -fomit-frame-pointer -Os
LDFLAGS=-mcrt=clib2 -lgcc -lc -lamiga
PROG=quickints
OBJS=quickints.o main.o

all: $(PROG)
clean:
	rm -f *.o $(PROG)
$(PROG): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^
%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<
