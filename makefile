obj = threadpool.o test01.o

inc_path = ./src/
CC = g++
target= test01	
CPPFLAGS = -std=c++17 -g -lpthread

ALL:$(target)

$(target):$(obj) 
	$(CC) $(CPPFLAGS) $(obj) -o $(target)

test01.o:./test/test01.cpp $(inc_path)
	$(CXX) $(CPPFLAGS) -c ./test/test01.cpp 

threadpool.o:./src/threadpool.cpp $(inc_path)
	$(CC) $(CPPFLAGS) -c ./src/threadpool.cpp 
	
clean:
	-rm -rf $(target) $(obj)

.PHONY: clean ALL
