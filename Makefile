lab2: server.c
	gcc server.c -o lab2 -lpthread

run: lab2
	./lab2 8000 docs --V0 1> out.log

clean:
	rm -f lab2

