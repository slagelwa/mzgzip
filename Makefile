CC = g++
CPPFLAGS = $(DFLAGS) $(INCLUDES) 
CFLAGS = -Wall -O2 
LDFLAGS =

.SUFFIXES: .c .o

.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) -g -c -o $@ $<

mzgzip : src/mzgzip.o src/MZGFile.o
	$(CC) $(LDFLAGS) -g -o $@ $^ -lz 

src/mzgzip.o : src/mzgzip.cpp src/MZGFile.h 
	g++ -O3 -static -I. -I../../include \
	   -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64 -DGCC \
	   -c src/mzgzip.cpp -o src/mzgzip.o


src/MZGFile.o : src/MZGFile.cpp src/MZGFile.h
	g++ -O3 -static -I. -I../../include \
	   -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64 -DGCC \
	   -c src/MZGFile.cpp -o src/MZGFile.o

clean :
	rm -f src/*.o mzgzip
