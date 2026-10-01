# Builds ShandalarPad as a stand-in for the game's image.dll.
# Needs a 32-bit MinGW-w64 toolchain (Debian/Ubuntu: gcc-mingw-w64-i686).

CC = i686-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -Werror -std=gnu99
LDFLAGS = -shared -static-libgcc -s -Wl,--enable-stdcall-fixup
LIBS = -lm

all: bin/image.dll

bin/image.dll: shandalarpad.c image.def
	mkdir -p bin
	$(CC) $(CFLAGS) -o $@ shandalarpad.c image.def $(LDFLAGS) $(LIBS)

clean:
	rm -f bin/image.dll

.PHONY: all clean
