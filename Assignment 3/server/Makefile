CC      = gcc
CFLAGS  = -pthread -lcrypto -lssl -g
RM      = rm -f

default: all

all: dfs

dfs:
	$(CC) -o server server.c $(CFLAGS)

clean veryclean:
	$(RM) server
