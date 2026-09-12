# Unit tests for the pure RAPL arithmetic.
#
# rapl_calc.c compiles outside the kernel on purpose - see the comment at the
# top of that file.
#
#   make        build and run
#   make clean  remove artifacts

PROG=	test_rapl_calc
SRCS=	test_rapl_calc.c ../../src/rapl_calc.c
CFLAGS=	-I../../src -Wall -Wextra -Werror -std=c99 -O1 -g

all: run

$(PROG): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

run: $(PROG)
	@./$(PROG)

clean:
	rm -f $(PROG) *.o

.PHONY: all run clean
