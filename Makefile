CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O3 -g3 -march=native  -Iinclude

SRC = $(wildcard src/*.c)
OBJ = $(SRC:.c=.o)
EXEC = scheduler

all: $(EXEC)

$(EXEC): $(OBJ)
	$(CC) $(OBJ) -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f src/*.o $(EXEC)

clean_o:
	rm -f src/*.o