test: main.cpp
	g++ -o gcm_test -lm main.cpp

clean:
	rm -f ./gcm_test
