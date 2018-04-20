CC=gcc
CFLAGS=-O2 `pkg-config --cflags gmime-3.0 json-glib-1.0 gio-unix-2.0`
LDFLAGS=-static -s `pkg-config --libs --static gmime-3.0 json-glib-1.0 gio-unix-2.0 blkid`

do-convert-stream-to-mime-multipart: src/main.c
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)
