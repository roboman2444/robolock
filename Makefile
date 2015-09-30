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
	@chmod 755 robolock-$@
	@chmod u+s robolock-$@


clean:
	@echo cleaning oop
	@rm -f $(OBJECTS)
purge:
	@echo purging oop
	@rm -f $(OBJECTS)
	@rm -f robolock
	@rm -f robolock-debug
install:
	@echo installing to /usr/bin
	@cp robolock /usr/bin/robolock
	@chmod 755 /usr/bin/robolock
	@chmod u+s /usr/bin/robolock
uninstall:
	@echo uninstalling
	@rm -f /usr/bin/robolock
