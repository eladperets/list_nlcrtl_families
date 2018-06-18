.PHONY: tags list_multicast_ids

CC=gcc

list_multicast_ids:
	rm -f ./list_nlcrtl_families
	$(CC) ./list_nlcrtl_families.c `pkg-config --cflags --libs libnl-genl-3.0` -o list_multicast_ids

all: list_multicast_ids 

tags:
	rm tags
	ctags -R --c-kinds=+deflmpstvx --fields=+l --extra=+f /usr/include --extra=+f /usr/include/net

