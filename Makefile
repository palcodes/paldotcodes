# Build the site tool. Requires any C++17 compiler; nothing else.
#   make            -> ./site (or site.exe)
#   make release    -> static binary for deployment
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
SRC      := src/main.cpp src/org.cpp src/mathml.cpp src/build.cpp src/server.cpp

# Note: the output is deliberately NOT named "site.exe" — Windows Smart App
# Control has a cached bad-reputation verdict for that generic name.
ifeq ($(OS),Windows_NT)
  LIBS := -lws2_32
  OUT  := palsite.exe
else
  LIBS := -lpthread
  OUT  := palsite
endif

$(OUT): $(SRC) src/*.hpp
	$(CXX) $(CXXFLAGS) -o $(OUT) $(SRC) $(LIBS)

release: $(SRC) src/*.hpp
	$(CXX) $(CXXFLAGS) -static -static-libgcc -static-libstdc++ -o $(OUT) $(SRC) $(LIBS)

clean:
	rm -rf $(OUT) dist

.PHONY: release clean
