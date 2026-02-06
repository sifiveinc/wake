# Bootstrap build file

VERSION	:= $(shell if test -f manifest.wake; then sed -n "/publish releaseAs/ s/^[^']*'\([^']*\)'.*/\1/p" manifest.wake; else git describe --tags --dirty | cut -c 2- | tr -d '\n' | sed 's/$$/-bootstrap/'; fi)

CC	:= cc -std=c11
CXX	:= c++
CXX_VERSION := -std=c++17
CFLAGS	:= -Wall -Wno-format-security -O2 -DVERSION=$(VERSION)
LDFLAGS	:= -lpthread
DESTDIR ?= /usr/local

LOCAL_CFLAGS :=	-Ivendor -Isrc
FUSE_CFLAGS  :=	$(shell pkg-config --silence-errors --cflags fuse)
CORE_CFLAGS  := $(shell pkg-config --silence-errors --cflags sqlite3)	\
		$(shell pkg-config --silence-errors --cflags gmp)	\
		$(shell pkg-config --silence-errors --cflags re2)	\
		$(shell pkg-config --silence-errors --cflags-only-I ncurses)
FUSE_LDFLAGS := $(shell pkg-config --silence-errors --libs fuse    || echo -lfuse)
CORE_LDFLAGS :=	$(shell pkg-config --silence-errors --libs sqlite3 || echo -lsqlite3)	\
		$(shell pkg-config --silence-errors --libs gmp || echo -lgmp)	\
		$(shell pkg-config --silence-errors --libs re2 || echo -lre2)	\
		$(shell pkg-config --silence-errors --libs ncurses tinfo || pkg-config --silence-errors --libs ncurses || echo -lncurses)

COMMON_DIRS := src/compat src/util src/json src/wcl
COMMON_C    := $(foreach dir,$(COMMON_DIRS),$(wildcard $(dir)/*.c)) \
               vendor/whereami/whereami.c
COMMON_CPP  := $(foreach dir,$(COMMON_DIRS),$(wildcard $(dir)/*.cpp))
COMMON_OBJS := src/json/jlexer.o \
               $(patsubst %.cpp,%.o,$(COMMON_CPP)) $(patsubst %.c,%.o,$(COMMON_C))

WAKE_DIRS := $(COMMON_DIRS) src/dst src/optimizer src/parser src/runtime src/types src/wcl tools/wake
WAKE_C    := $(foreach dir,$(WAKE_DIRS),$(wildcard $(dir)/*.c)) \
             vendor/blake2/blake2b-ref.c vendor/utf8proc/utf8proc.c \
             vendor/siphash/siphash.c vendor/whereami/whereami.c \
             vendor/gopt/gopt.c vendor/gopt/gopt-errors.c vendor/gopt/gopt-arg.c
WAKE_CPP  := $(foreach dir,$(WAKE_DIRS),$(wildcard $(dir)/*.cpp))
WAKE_OBJS := src/parser/lexer.o src/parser/parser.o src/json/jlexer.o \
             $(patsubst %.cpp,%.o,$(WAKE_CPP)) $(patsubst %.c,%.o,$(WAKE_C))

WAKE_ENV := WAKE_PATH=$(shell dirname $(shell which $(firstword $(CC))))

all:		wake.db
	$(WAKE_ENV) BOOTSTRAP_WAKE=true ./bin/wake build default

clean:
	rm -rf .build/* bin/* lib/wake/* */*.o */*/*.o src/json/jlexer.cpp src/parser/lexer.cpp src/parser/parser.cpp src/parser/parser.h src/version.h wake.db
	touch bin/stamp lib/wake/stamp

wake.db:	bin/wake bin/wakebox lib/wake/fuse-waked lib/wake/shim-wake lib/wake/wake-hash bin/wake-migrate
	test -f $@ || ./bin/wake --init .

install:	all
	$(WAKE_ENV) ./bin/wake install $(DESTDIR)

# Formats all .h and .cpp file under the current directory
# It assumes clang is available on the PATH and will fail otherwise
formatAll:
	@clang-format -i --style=file $(shell ./scripts/which_clang_files all)
	@bin/wake-format.native-cpp14-release --auto --in-place


# Formats all changed or staged .h or .cpp files
# It assumes clang is available on the PATH and will fail otherwise
format:
# || true is added after the if expression since it resolves with false when false
# and we don't want make to report that as an error
	@FILES=$$(./scripts/which_clang_files changed) && \
	if [ "$$FILES" ]; then \
		clang-format -i --style=file $$FILES; \
	fi || true  && \
	FILES=$$(./scripts/which_wake_files changed) && \
	if [ "$$FILES" ]; then \
		bin/wake-format.native-cpp14-release -i $$FILES; \
	fi || true

test:		wake.db
	$(WAKE_ENV) ./bin/wake --in test_wake runTests

# We need to bootstrap job-cache to run the tests
unittest:	all
	$(WAKE_ENV) ./bin/wake --in test_wake runUnitTests

remoteCacheTests:	all
	$(WAKE_ENV) ./bin/wake -d -x 'testPostgres Unit'

tarball:	wake.db
	$(WAKE_ENV) ./bin/wake build tarball

vscode:		wake.db
	$(WAKE_ENV) ./bin/wake vscode

wasm:		wake.db
	$(WAKE_ENV) ./bin/wake build wasm

static:	wake.db
	$(WAKE_ENV) ./bin/wake static

bin/wake:	$(WAKE_OBJS)
	$(CXX) $(CFLAGS) $(CXX_VERSION) -o $@ $^ $(LDFLAGS) $(CORE_LDFLAGS)

bin/wakebox:		tools/wakebox/main.cpp src/wakefs/*.cpp vendor/gopt/*.c $(COMMON_OBJS)
	$(CXX) $(CFLAGS) $(LOCAL_CFLAGS) $(CXX_VERSION) $^ -o $@ $(LDFLAGS) $(CORE_LDFLAGS)

lib/wake/fuse-waked:	tools/fuse-waked/main.cpp $(COMMON_OBJS)
	$(CXX) $(CFLAGS) $(LOCAL_CFLAGS) $(FUSE_CFLAGS) $(CXX_VERSION) $^ -o $@ $(LDFLAGS)  $(CORE_LDFLAGS) $(FUSE_LDFLAGS)

lib/wake/shim-wake:	tools/shim-wake/main.o vendor/blake2/blake2b-ref.o src/wcl/filepath.o
	$(CXX) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(CORE_LDFLAGS)

lib/wake/wake-hash: tools/wake-hash/main.o vendor/blake2/blake2b-ref.o $(COMMON_OBJS)
	$(CXX) $(CFLAGS) -o $@ $^ $(LOCAL_CFLAGS) $(CXX_VERSION) $(LDFLAGS) $(CORE_LDFLAGS)

bin/wake-migrate: tools/wake-migrate/main.o $(COMMON_OBJS)
	$(CXX) $(CFLAGS) -o $@ $^ $(LOCAL_CFLAGS) $(CXX_VERSION) $(LDFLAGS) $(CORE_LDFLAGS) -lsqlite3

%.o:	%.cpp	$(filter-out src/parser/parser.h,$(wildcard */*/*.h)) | src/parser/parser.h
	$(CXX) $(CFLAGS) $(LOCAL_CFLAGS) $(CORE_CFLAGS) $(CXX_VERSION) -o $@ -c $<

%.o:	%.c	$(filter-out src/version.h,$(wildcard */*.h))
	$(CC) $(CFLAGS) $(LOCAL_CFLAGS) -o $@ -c $<

# Rely on wake to recreate this file if re2c is available
%.cpp:	%.cpp.gz
	gzip -dc $^ > $@.tmp
	mv -f $@.tmp $@

%.h:	%.h.gz
	gzip -dc $^ > $@.tmp
	mv -f $@.tmp $@

.PRECIOUS:	src/parser/lexer.cpp src/parser/parser.cpp src/parser/parser.h src/json/jlexer.cpp
.SUFFIXES:
