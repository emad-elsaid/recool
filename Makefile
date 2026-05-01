# Recool - Wayland Screen Recorder
# Build configuration for standalone C implementation

# Compiler settings
CC := clang
CFLAGS := -O3 -march=native -flto \
          -Wall -Wextra -Wpedantic \
          -std=c11 -D_POSIX_C_SOURCE=200809L

# Package dependencies
PACKAGES := libpipewire-0.3 dbus-1 libavformat libavcodec libavutil libswscale sqlite3 tesseract lept

# Compilation flags from pkg-config
PKG_CFLAGS := $(shell pkg-config --cflags $(PACKAGES))
PKG_LIBS := $(shell pkg-config --libs $(PACKAGES))

# Target binary
TARGET := recool

# Source files
SRC := recool.c

# Default target
all: $(TARGET)

# Build target
$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $(TARGET) $(SRC) $(PKG_LIBS)
	@echo "Build complete: ./$(TARGET)"
	@du -h $(TARGET) | cut -f1 | xargs echo "Binary size:"

# Clean build artifacts
clean:
	rm -f $(TARGET)

# Install to user bin (optional)
install: $(TARGET)
	mkdir -p $(HOME)/.local/bin
	cp $(TARGET) $(HOME)/.local/bin/
	@echo "Installed to ~/.local/bin/$(TARGET)"

# Uninstall
uninstall:
	rm -f $(HOME)/.local/bin/$(TARGET)
	@echo "Uninstalled from ~/.local/bin"

# Check dependencies
check-deps:
	@echo "Checking required dependencies..."
	@for pkg in $(PACKAGES); do \
		pkg-config --exists $$pkg && echo "  ✓ $$pkg" || echo "  ✗ $$pkg (MISSING)"; \
	done
	@which $(CC) > /dev/null && echo "  ✓ $(CC)" || echo "  ✗ $(CC) (MISSING)"

# Run target - build and execute
run: $(TARGET)
	@echo "Starting $(TARGET)..."
	./$(TARGET)

# Test cleanup - run for 5 seconds then stop
test-cleanup: $(TARGET)
	@echo "Testing cleanup sequence..."
	@echo "Running for 5 seconds, then stopping..."
	@(timeout 5 ./$(TARGET) || true) 2>&1 | tee /tmp/recool-cleanup-test.log
	@echo ""
	@echo "=== Cleanup Validation ==="
	@if grep -q "Segmentation fault\|core dumped" /tmp/recool-cleanup-test.log; then \
		echo "✗ FAILED: Segmentation fault detected"; \
		exit 1; \
	elif ! grep -q "All cleanup complete" /tmp/recool-cleanup-test.log; then \
		echo "✗ FAILED: Cleanup did not complete"; \
		exit 1; \
	else \
		echo "✓ PASSED: Clean shutdown confirmed"; \
		echo "✓ All subsystems cleaned up properly"; \
	fi
	@rm -f /tmp/recool-cleanup-test.log

# Help target
help:
	@echo "Recool - Wayland Screen Recorder"
	@echo ""
	@echo "Targets:"
	@echo "  make          - Build recool binary"
	@echo "  make run      - Build and run recool"
	@echo "  make clean    - Remove build artifacts"
	@echo "  make install  - Install to ~/.local/bin"
	@echo "  make uninstall- Remove from ~/.local/bin"
	@echo "  make check-deps- Verify all dependencies present"
	@echo "  make test-cleanup - Test resource cleanup (5s run + validation)"
	@echo "  make help     - Show this help message"

.PHONY: all clean install uninstall check-deps help run test-cleanup
