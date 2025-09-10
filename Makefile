all:np_multi_proc.cpp
	g++ np_multi_proc.cpp -o np_multi_proc
clean:
	rm -f np_multi_proc