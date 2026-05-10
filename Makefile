CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -g -Iinclude
LDFLAGS =

all: folders controller runner
	@ln -sf bin/controller controller
	@ln -sf bin/runner runner

controller: bin/controller

runner: bin/runner

folders:
	@mkdir -p src include obj bin tmp

bin/controller: obj/controller.o
	$(CC) $(LDFLAGS) $^ -o $@

bin/runner: obj/runner.o
	$(CC) $(LDFLAGS) $^ -o $@

obj/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f obj/* tmp/* bin/* controller runner