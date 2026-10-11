CC = gcc
CFLAGS = -Wall -Wextra -O3 -std=c99
TARGET = 7aba

SRCS = main.c lexer.c parser.c eval.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c main.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)

run: all
	./$(TARGET) pro.7ba

.PHONY: all clean run
