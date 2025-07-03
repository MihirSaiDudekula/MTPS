# Makefile for C proxy server
CC = gcc
CFLAGS = -Wall -Wextra -std=c99
TARGET = proxy
SRC = src/proxy.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TARGET) *.o

.PHONY: all clean
