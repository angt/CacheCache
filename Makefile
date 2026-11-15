CC     = cc
CFLAGS = -std=c11 -O2 -Wall -Wextra -pthread
OBJS   = main.o common.o wire.o chat.o frame.o console.o modem.o air.o

cachecache: $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@

$(OBJS): common.h protocol.h

clean:
	rm -f cachecache *.o

.PHONY: clean
