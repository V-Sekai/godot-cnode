# Makefile for standalone C CNode test program

# Detect OS
UNAME_S := $(shell uname -s)

# Default Erlang installation paths
ifeq ($(UNAME_S),Darwin)
	# macOS - try Homebrew paths
	ERL_INTERFACE_BASE := $(shell find /opt/homebrew/lib/erlang/lib -maxdepth 1 -type d -name "erl_interface-*" 2>/dev/null | head -1)
	ifeq ($(ERL_INTERFACE_BASE),)
		ERL_INTERFACE_BASE := $(shell find /usr/local/lib/erlang/lib -maxdepth 1 -type d -name "erl_interface-*" 2>/dev/null | head -1)
	endif
else ifeq ($(UNAME_S),Linux)
	# Linux
	ERL_INTERFACE_BASE := $(shell find /usr/lib/erlang/lib -maxdepth 1 -type d -name "erl_interface-*" 2>/dev/null | head -1)
	ifeq ($(ERL_INTERFACE_BASE),)
		ERL_INTERFACE_BASE := $(shell find /usr/local/lib/erlang/lib -maxdepth 1 -type d -name "erl_interface-*" 2>/dev/null | head -1)
	endif
endif

# Check if erl_interface was found
ifeq ($(ERL_INTERFACE_BASE),)
	$(error Could not find erl_interface. Please set ERL_INTERFACE_BASE manually or install Erlang/OTP)
endif

# Set include and library paths
ERL_INTERFACE_INCLUDE := $(ERL_INTERFACE_BASE)/include
ERL_INTERFACE_LIB := $(ERL_INTERFACE_BASE)/lib

# Compiler and flags
CC := gcc
CFLAGS := -Wall -Wextra -std=c11 -g -O2
CFLAGS += -I$(ERL_INTERFACE_INCLUDE)

# Libraries
LIBS := -L$(ERL_INTERFACE_LIB) -lei

# Platform-specific flags
ifeq ($(UNAME_S),Darwin)
	CFLAGS += -D_DARWIN_C_SOURCE
endif

CFLAGS += -D_POSIX_C_SOURCE=200112L

# Target
TARGET := test_cnode
SRC := test_cnode.c
OBJ := $(SRC:.c=.o)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJ)
	@echo "Linking $(TARGET)..."
	@echo "  Using erl_interface from: $(ERL_INTERFACE_BASE)"
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ) $(LIBS)
	@echo "Build complete: ./$(TARGET)"

$(OBJ): $(SRC)
	@echo "Compiling $(SRC)..."
	@echo "  Using erl_interface from: $(ERL_INTERFACE_BASE)"
	$(CC) $(CFLAGS) -c $(SRC) -o $(OBJ)

clean:
	rm -f $(TARGET) $(OBJ)

run: $(TARGET)
	@echo "Starting test CNode..."
	@echo "Nodename: test_cnode@127.0.0.1"
	@echo "Cookie: godotcookie"
	@echo ""
	./$(TARGET) test_cnode@127.0.0.1 godotcookie

help:
	@echo "Makefile for standalone C CNode"
	@echo ""
	@echo "Targets:"
	@echo "  make          - Build the test_cnode binary"
	@echo "  make run      - Build and run the test CNode"
	@echo "  make clean    - Remove build artifacts"
	@echo "  make help     - Show this help message"
	@echo ""
	@echo "Found erl_interface: $(ERL_INTERFACE_BASE)"
	@echo "  Include: $(ERL_INTERFACE_INCLUDE)"
	@echo "  Library: $(ERL_INTERFACE_LIB)"
