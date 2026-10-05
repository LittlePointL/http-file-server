CXX = g++
CXXFLAGS = -Wall -std=c++14 -g
TARGET = build/bin/fileserver

SRCS = $(wildcard *.cpp)
OBJS = $(patsubst %.cpp, build/obj/%.o, $(SRCS))

$(TARGET): $(OBJS) | build/bin build/obj
	$(CXX) $(CXXFLAGS) -o $@ $^

build/obj/%.o: %.cpp | build/obj
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/obj build/bin:
	mkdir -p $@

clean:
	rm -rf build/obj build/bin/fileserver

.PHONY: clean



                           