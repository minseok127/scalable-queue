CXX := g++
CXXFLAGS := -std=c++20 -O2 -Wall -pthread

TARGET := producer_consumer
SRC := producer_consumer.cpp

TARGET2 := producer_consumer_correctness
SRC2 := producer_consumer_correctness.cpp

LDFLAGS += -L..
LDLIBS += -lscq

all: $(TARGET) $(SRC) $(TARGET2) $(SRC2)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC) $(LDFLAGS) -static $(LDLIBS)

$(TARGET2): $(SRC2)
	$(CXX) $(CXXFLAGS) -o $(TARGET2) $(SRC2) $(LDFLAGS) -static $(LDLIBS)

clean:
	rm -f $(TARGET) $(TARGET2)
