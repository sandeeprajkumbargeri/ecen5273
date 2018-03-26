CC      = gcc
CFLAGS  =
RM      = rm -f

default: all

all: udp_client

udp_client:
	$(CC) $(CFLAGS) -o client udp_client.c

clean veryclean:
	$(RM) client
