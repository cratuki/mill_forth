all:
	gcc -g -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-function main.c -o exe

clean:
	rm exe

