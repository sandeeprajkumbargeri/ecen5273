CC      = gcc
CFLAGS  =
RM      = rm -f

default: all

all: udp_server

udp_server:
	$(CC) $(CFLAGS) -o server udp_server.c

clean veryclean:
	$(RM) server
