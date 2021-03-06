# Automatically generated by configure
# Configured with: './configure' '--platform=opendingux' '--sound-drivers=sdl'
# TARGET = picodrive/picodrive.elf
TARGET = picodrive/picodrive.dge

# CHAINPREFIX := /opt/gcw0-toolchain
CHAINPREFIX ?= /opt/mipsel-RetroFW-linux-uclibc
CROSS_COMPILE ?= $(CHAINPREFIX)/usr/bin/mipsel-linux-

CC = $(CROSS_COMPILE)gcc
CXX = $(CROSS_COMPILE)g++
STRIP = $(CROSS_COMPILE)strip
AS = $(CROSS_COMPILE)as

SYSROOT     := $(shell $(CC) --print-sysroot)
SDL_CFLAGS  := $(shell $(SYSROOT)/usr/bin/sdl-config --cflags)
SDL_LIBS    := $(shell $(SYSROOT)/usr/bin/sdl-config --libs)

CFLAGS +=  $(SDL_CFLAGS) -std=gnu11 -D_GNU_SOURCE=1 -D_REENTRANT -Wno-unused-result
ASFLAGS +=
LDFLAGS +=
LDLIBS += $(SDL_LIBS) -lpthread -lpng -lm -lz

ARCH = mipsel
PLATFORM = opendingux
SOUND_DRIVERS = sdl
