CC      = gcc
CFLAGS  = -pthread
RM      = rm -f

default: all

all: server

udp_server:
	$(CC) $(CFLAGS) -o server.c webserver

clean veryclean:
	$(RM) webserver
