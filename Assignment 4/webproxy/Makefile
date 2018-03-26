CC      = gcc
CFLAGS  = -pthread -lcrypto -lssl -g
RM      = rm -f

default: all

all: proxy

proxy:
	$(CC) -o webproxy webproxy.c $(CFLAGS)

clean veryclean:
	$(RM) webproxy
