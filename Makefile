# Makefile — no-CMake fallback. `make` builds everything into bin/.
#
#   make          build minidb + bench + tests
#   make run      build and launch the shell
#   make test     build and run all component tests
#   make bench    build and run the MVCC-vs-2PL benchmark
#   make clean    remove build artifacts and scratch DB files
CXX      ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Isrc
LDFLAGS  ?= -pthread
BIN      := bin

.PHONY: all run test bench clean
all: $(BIN)/minidb $(BIN)/bench $(BIN)/test_storage $(BIN)/test_btree $(BIN)/test_mvcc $(BIN)/test_locking

$(BIN):
	mkdir -p $(BIN)

$(BIN)/minidb: src/main.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

$(BIN)/bench: benchmarks/bench.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

$(BIN)/test_%: src/test_%.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

run: $(BIN)/minidb
	./$(BIN)/minidb

test: $(BIN)/test_storage $(BIN)/test_btree $(BIN)/test_mvcc $(BIN)/test_locking
	@echo "== storage ==" && ./$(BIN)/test_storage
	@echo "== btree =="   && ./$(BIN)/test_btree
	@echo "== mvcc =="    && ./$(BIN)/test_mvcc
	@echo "== locking ==" && ./$(BIN)/test_locking

bench: $(BIN)/bench
	./$(BIN)/bench

clean:
	rm -rf $(BIN) minidb.db* *.catalog *.wal
