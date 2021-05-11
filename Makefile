default: hello.c
	gcc hello.c -o lab2 -lpthread

run: hello.c
	./lab2 8000 docs --V0 1> out.log

clean:
	rm -f lab2 *.txt

