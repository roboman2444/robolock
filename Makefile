CC = gcc
LDFLAGS = -lX11 -lpthread -lcrypt -lXext -lXrandr -lc
CFLAGS = -Wall -Ofast  -fstrict-aliasing -march=native
OBJECTS = robolock.o

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

robolock: $(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o $@ $(LDFLAGS)
	@chmod 755 $@
	@chmod u+s $@

debug:	CFLAGS= -Wall -O0 -g  -fstrict-aliasing -march=native
debug: 	$(OBJECTS)
	$(CC) $(CFLAGS) $(OBJECTS) -o robolock-$@ $(LDFLAGS)

clean:
	@echo cleaning oop
	@rm -f $(OBJECTS)
purge:
	@echo purging oop
	@rm -f $(OBJECTS)
	@rm -f robolock
	@rm -f robolock-debug
