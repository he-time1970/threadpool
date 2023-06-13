CC            = gcc
CXX           = g++
DEFINES       = 
CFLAGS        = -Wall -W $(DEFINES)
CXXFLAGS      = -std=c++11  -Wall -W $(DEFINES)
INCPATH       = 
LINK          = g++
LFLAGS        = 
LIBS          = -pthread

TARGET = main.out

$(TARGET):main.cpp
	$(CXX) $(CXXFLAGS) main.cpp -o main.out  $(LIBS)

.PHONY: clean
clean:
	rm main.out


