CXX ?=g++
DEBUG ?=1

ifeq ($(DEBUG),1)
	CXXFLAG += -g
else 
	CXXFLAG += -O2  
endif


server: main.cpp ./timer/lst_timer.cpp ./http/http_server.cpp ./log/log.cpp webserver.cpp 
	$(CXX) -o server $^ $(CXXFLAG) -lpthread --std=c++11

clean:
	rm  -r server