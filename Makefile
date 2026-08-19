all:
	gcc main.c -Wall -lncursesw -o main
	./main
	rm main

build:
	gcc main.c -Wall -lncursesw -o main

run:
	./main

clean:
	rm main
