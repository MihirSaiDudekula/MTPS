# Makefile for C proxy server
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -pthread -D_GNU_SOURCE -I./include
LIBS = -lpthread
TARGET = bin/proxy

# Source files
SRC_DIR = src
SRC = $(SRC_DIR)/main.c \
      $(SRC_DIR)/cache/cache.c \
      $(SRC_DIR)/http/request.c \
      $(SRC_DIR)/http/response.c \
      $(SRC_DIR)/network/connection.c \
      $(SRC_DIR)/utils/logger.c

# Object files
OBJ_DIR = obj
OBJ = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRC))

# Create necessary directories
MKDIR_P = mkdir -p

# Default target
all: directories $(TARGET)

# Create build directories
directories: $(OBJ_DIR) bin

$(OBJ_DIR):
	$(MKDIR_P) $(OBJ_DIR)
	$(MKDIR_P) $(OBJ_DIR)/cache
	$(MKDIR_P) $(OBJ_DIR)/http
	$(MKDIR_P) $(OBJ_DIR)/network
	$(MKDIR_P) $(OBJ_DIR)/utils

bin:
	$(MKDIR_P) bin

# Link object files to create the executable
$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

# Compile source files to object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build artifacts
clean:
	rm -rf $(OBJ_DIR) bin

.PHONY: all clean directories