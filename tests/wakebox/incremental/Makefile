hello: main.o impl.o
	gcc -o $@ $^

main.o: impl.h

%.o: %.c %.h
	gcc -c -o $@ $<
