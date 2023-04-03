# Compiler settings
CC = clang
LD = clang
CC_FLAGS = -O2 -Wall -Wextra -c
LD_FLAGS = -Wall -Wextra
OBJ = obj

bps: $(OBJ) $(OBJ)/bps.o
	$(CC) $(LD_FLAGS) $(OBJ)/bps.o -o bps

$(OBJ):
	mkdir $(OBJ)

$(OBJ)/bps.o: bps.c crc32.h
	$(CC) $(CC_FLAGS) bps.c -o $(OBJ)/bps.o

.PHONY: clean
clean:
	rm -rf $(OBJ) bps