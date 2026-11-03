CC     = cc
CFLAGS = -std=c11 -O2 -Wall -Wextra -pthread
OBJS   = common.o wire.o chat.o frame.o console.o probe.o air.o

cachecache: main.c $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(OBJS): common.h

clean:
	rm -f cachecache *.o

.PHONY: clean
