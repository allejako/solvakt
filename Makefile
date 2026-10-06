# Solvakt - Local Energy Optimization Platform
#
#   make            bygg server, fetcher och klient
#   make debug      bygg om med -O0
#   make release    bygg om med -O2
#   make test       bygg och kör alla tester
#   make bench      kör benchmark för beräkningssteget
#   make profile    profilera beräkningssteget med gprof
#   make valgrind   kör tester och benchmark under Valgrind
#   make run        starta servern i förgrunden (Ctrl-C stoppar)
#   make stop       stoppa en körande server med SIGTERM
#   make clean      ta bort byggfiler

CC  = gcc
CXX = g++
OPT ?= -O2

CFLAGS   = -std=c11 -D_GNU_SOURCE -Wall -Wextra -pthread -g $(OPT) -Isrc -MMD -MP
CXXFLAGS = -std=c++17 -Wall -Wextra -pthread -g $(OPT) -Isrc -MMD -MP
LDFLAGS  = -pthread
LDLIBS   = -lm

BIN   = bin
BUILD = build
CONFIG = config/solvakt.conf

SERVER  = $(BIN)/solvakt-server
FETCHER = $(BIN)/solvakt-fetcher
CLIENT  = $(BIN)/solvakt-client

# Objekt som delas mellan servern, testerna och benchmarken
LIB_OBJS = \
    $(BUILD)/common/log.o \
    $(BUILD)/common/config.o \
    $(BUILD)/common/queue.o \
    $(BUILD)/server/parse.o \
    $(BUILD)/server/compute.o \
    $(BUILD)/server/cache.o \
    $(BUILD)/libs/cJSON.o

SERVER_OBJS = \
    $(BUILD)/server/main.o \
    $(BUILD)/server/server.o \
    $(BUILD)/server/fetcher_proc.o \
    $(LIB_OBJS)

FETCHER_OBJS = $(BUILD)/fetcher/main.o

CLIENT_OBJS = \
    $(BUILD)/client/main.o \
    $(BUILD)/client/Client.o \
    $(BUILD)/client/UnixSocket.o

TESTS = test_queue test_config test_cache test_parse test_compute
TEST_BINS = $(addprefix $(BIN)/,$(TESTS))
BENCH = $(BIN)/bench_compute

.PHONY: all debug release test bench profile valgrind run stop clean

all: $(SERVER) $(FETCHER) $(CLIENT)

debug:
	$(MAKE) clean
	$(MAKE) all OPT=-O0

release:
	$(MAKE) clean
	$(MAKE) all OPT=-O2

$(SERVER): $(SERVER_OBJS) | $(BIN)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(FETCHER): $(FETCHER_OBJS) | $(BIN)
	$(CC) $(LDFLAGS) -o $@ $^ -lcurl

$(CLIENT): $(CLIENT_OBJS) | $(BIN)
	$(CXX) $(LDFLAGS) -o $@ $^

$(BUILD)/libs/cJSON.o: src/libs/cJSON.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c -o $@ $<

$(BUILD)/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BIN):
	@mkdir -p $(BIN)

# ── Tester ───────────────────────────────────────────────────────────
$(BIN)/test_%: tests/test_%.c tests/check.h $(LIB_OBJS) | $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIB_OBJS) $(LDLIBS)

test: $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "== $$t"; ./$$t || exit 1; done
	@echo "Alla tester godkända"

# ── Benchmark och profilering av beräkningssteget ────────────────────
BENCH_SRCS = bench/bench_compute.c src/server/compute.c
BENCH_N ?= 200

$(BENCH): $(BENCH_SRCS) | $(BIN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BENCH_SRCS) $(LDLIBS)

bench: $(BENCH)
	./$(BENCH) $(BENCH_N)

# Byggs alltid med -pg och -O0 så att gprof ser varje funktion för sig.
profile: | $(BIN)
	$(CC) -std=c11 -D_GNU_SOURCE -Wall -Wextra -g -O0 -pg -Isrc \
	    -o $(BIN)/bench_compute_prof $(BENCH_SRCS) -lm
	./$(BIN)/bench_compute_prof $(BENCH_N)
	gprof -b $(BIN)/bench_compute_prof gmon.out

# ── Minnesanalys ─────────────────────────────────────────────────────
VALGRIND = valgrind --leak-check=full --error-exitcode=1 -q

valgrind: $(TEST_BINS) $(BENCH)
	@for t in $(TEST_BINS); do echo "== valgrind $$t"; $(VALGRIND) ./$$t || exit 1; done
	$(VALGRIND) ./$(BENCH) 5
	@echo "Inga minnesfel eller läckor"

# ── Köra ─────────────────────────────────────────────────────────────
run: all
	./$(SERVER) -c $(CONFIG)

stop:
	@pkill -TERM -x solvakt-server && echo "SIGTERM skickad" || echo "Ingen server kör"

clean:
	rm -rf $(BUILD) $(BIN) gmon.out

-include $(shell find $(BUILD) -name '*.d' 2>/dev/null)
