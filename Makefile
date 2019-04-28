iterative: 01_iterative/main.c
		gcc -o $@ $<

forking: 02_forking/main.c
		gcc -o $@ $<

preforked: 03_preforked/main.c
		gcc -o $@ $<

threaded: 04_threaded/main.c
		gcc -o $@ $< -lpthread

prethreaded: 05_prethreaded/main.c
		gcc -o $@ $< -lpthread

poll: 06_poll/main.c
		gcc -o $@ $<

epoll: 07_epoll/main.c
		gcc -o $@ $<

all: iterative forking preforked threaded prethreaded poll epoll

.PHONY: clean

clean:
	rm -f iterative forking preforked threaded prethreaded poll epoll
