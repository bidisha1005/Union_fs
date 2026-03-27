CC      = gcc
CFLAGS  = -Wall -Wextra -g -D_FILE_OFFSET_BITS=64 $(shell pkg-config --cflags fuse3)
LDFLAGS = $(shell pkg-config --libs fuse3)

TARGET  = mini_unionfs

all: $(TARGET)

$(TARGET): mini_unionfs.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
