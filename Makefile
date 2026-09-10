CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -lgpiod -lpthread -lm

TARGET  = sx1278_final
SRCS    = main.c lora.c
OBJS    = $(SRCS:.c=.o)
HDRS    = lora.h

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	sudo ./$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
