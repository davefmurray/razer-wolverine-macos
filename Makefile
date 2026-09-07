CC ?= clang
LIBUSB_CFLAGS := $(shell pkg-config --cflags libusb-1.0 2>/dev/null || echo -I/opt/homebrew/opt/libusb/include/libusb-1.0)
LIBUSB_LIBS   := $(shell pkg-config --libs libusb-1.0 2>/dev/null || echo -L/opt/homebrew/opt/libusb/lib -lusb-1.0)

all: build/giptool

build/giptool: src/giptool.c src/gipauth.c src/gipauth.h
	@mkdir -p build
	$(CC) -O2 -Wall -Wextra -Wno-deprecated-declarations $(LIBUSB_CFLAGS) -o $@ src/giptool.c src/gipauth.c $(LIBUSB_LIBS) -framework Security -framework CoreFoundation

clean:
	rm -rf build/giptool
.PHONY: all clean

# x86_64 SDL test client against our custom libSDL2 (run under Rosetta)
SDLX := build/x86_64
build/sdltest: src/sdltest.c $(SDLX)/lib/libSDL2-2.0.0.dylib
	$(CC) -arch x86_64 -O1 -Wall -I$(SDLX)/include/SDL2 -o $@ src/sdltest.c -L$(SDLX)/lib -lSDL2 -Wl,-rpath,$(PWD)/$(SDLX)/lib
