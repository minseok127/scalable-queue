CXX := g++
CXXFLAGS := -std=c++20 -O2 -Wall -pthread

TARGET := producer_consumer
SRC := producer_consumer.cpp

LDFLAGS += -L..
LDLIBS += -lscq

all: $(TARGET) $(SRC)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS) -static $(LDLIBS)

clean:
	rm -f $(TARGET)
