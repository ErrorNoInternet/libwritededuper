lib: writedduper.c
	cc -ldl -lm -shared -fPIC -Wl,-soname,libwritedduper.so -o libwritedduper.so writedduper.c
