# The upscalers' own test (upscale_test.c), on this computer.
V = ../..
CFLAGS ?= -O2 -g -Wall
upscale_test: upscale_test.c $(V)/upscale_vita.c $(V)/third_party/xbrz/xbrz.cpp $(V)/third_party/xbrz/xbrz_c.cpp
	$(CC) $(CFLAGS) -I$(V) -c $(V)/upscale_vita.c -o upscale_vita.o
	$(CXX) $(CFLAGS) -std=c++17 -I$(V)/third_party/xbrz -c $(V)/third_party/xbrz/xbrz.cpp -o xbrz.o
	$(CXX) $(CFLAGS) -std=c++17 -I$(V)/third_party/xbrz -c $(V)/third_party/xbrz/xbrz_c.cpp -o xbrz_c.o
	$(CC) $(CFLAGS) -I$(V) upscale_test.c upscale_vita.o xbrz.o xbrz_c.o -o $@ -lstdc++ -lpthread -lm
clean:
	rm -f upscale_test *.o
