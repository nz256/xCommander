CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -std=c11 -Iinclude
LDFLAGS = -lncursesw

TARGET  = xc
SRC_DIR = src
OBJ_DIR = build
PREFIX  = /usr/local

SRCS    = $(wildcard $(SRC_DIR)/*.c)
OBJS    = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

.PHONY: all clean install uninstall

all: $(TARGET)

BUILD_NUM := $(shell b=$$(cat .build_num 2>/dev/null || echo 0); echo $$((b + 1)))
CFLAGS += -DXC_VERSION=\"0.1\" -DXC_BUILD=$(BUILD_NUM)

$(TARGET): $(OBJS)
	@echo $(BUILD_NUM) > .build_num; \
	$(CC) $(OBJS) -o $(TARGET) $(LDFLAGS); \
	echo "Built $(TARGET) v0.1 (build $(BUILD_NUM))"

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c include/*.h | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)

clean:
	rm -rf $(OBJ_DIR) $(TARGET)
