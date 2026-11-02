# make            1단계 전체 빌드
# make bench_pipe / bench_mq / bench_shm   버전별 빌드
# make test       전송 검증 프로그램 빌드 (duplex_*)
# make tsan asan  ThreadSanitizer, AddressSanitizer 빌드 (build/tsan, build/asan)
CC      ?= gcc
SAN     ?=
BUILD   ?= build/release
CFLAGS  := -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -O2 -g -Wall -Wextra -pthread -Iinclude -Isrc -Ibench $(SAN)
LDLIBS  := -pthread -lrt
LDFLAGS := $(SAN)

IPCS := pipe mq shm
BENCH_BINS  := $(foreach i,$(IPCS),$(BUILD)/bench_$(i)_server $(BUILD)/bench_$(i)_client)
DUPLEX_BINS := $(foreach i,$(IPCS),$(BUILD)/duplex_$(i))

.PHONY: all test $(addprefix bench_,$(IPCS)) tsan asan tar clean
all: $(BENCH_BINS) $(DUPLEX_BINS)
test: $(DUPLEX_BINS)

define IPC_RULES
bench_$(1): $(BUILD)/bench_$(1)_server $(BUILD)/bench_$(1)_client
$(BUILD)/bench_$(1)_server: $(BUILD)/bench_server.o $(BUILD)/common.o $(BUILD)/transport_$(1).o
	$$(CC) $$(LDFLAGS) -o $$@ $$^ $$(LDLIBS)
$(BUILD)/bench_$(1)_client: $(BUILD)/bench_client.o $(BUILD)/common.o $(BUILD)/transport_$(1).o
	$$(CC) $$(LDFLAGS) -o $$@ $$^ $$(LDLIBS)
$(BUILD)/duplex_$(1): $(BUILD)/duplex_test.o $(BUILD)/common.o $(BUILD)/transport_$(1).o
	$$(CC) $$(LDFLAGS) -o $$@ $$^ $$(LDLIBS)
endef
$(foreach i,$(IPCS),$(eval $(call IPC_RULES,$(i))))

HEADERS := include/ipc.h src/common.h bench/bench_common.h
$(BUILD)/%.o: src/%.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/%.o: bench/%.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/%.o: tests/%.c $(HEADERS) | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<
$(BUILD):
	mkdir -p $@

tsan:
	$(MAKE) test BUILD=build/tsan SAN="-fsanitize=thread"
asan:
	$(MAKE) test BUILD=build/asan SAN="-fsanitize=address,undefined"

# 제출용 묶음: make tar TEAM=조번호
TEAM ?= 0
tar:
	tar cvf osproject_$(TEAM).tar --exclude=build --exclude=results/pilot --exclude='*.tar' --exclude=.git --exclude=spin .

clean:
	rm -rf build
