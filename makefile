# Compiler and flags
CXX = g++
CXXFLAGS = -I/usr/include/libdrm -std=c++17 -Wall -Wextra

# Linker flags
LDFLAGS = -ldrm

# Target executable
TARGET = OpenCard

# Source files
SRCS = OpenCard.cpp

# Build rule
all: $(TARGET)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# Clean rule
clean:
	rm -f $(TARGET)

