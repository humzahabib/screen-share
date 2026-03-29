# Compiler and flags
CXX = gcc
CXXFLAGS = -I/usr/include/libdrm -Wall -Wextra

# Linker flags
LDFLAGS = -ldrm -lpthread edid.h

# Target executable
TARGET = vmonitor

# Source files
SRCS = vmonitor.c

# Build rule
all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Clean rule
clean:
	rm -f $(TARGET)

