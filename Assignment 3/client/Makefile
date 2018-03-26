CC      = gcc
CFLAGS  = -lcrypto -lssl -g
RM      = rm -f

default: all

all: dfc

dfc:
	$(CC) -o client client.c $(CFLAGS)

clean veryclean:
	$(RM) client
