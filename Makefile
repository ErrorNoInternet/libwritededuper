lib:
	cc -ldl -lm -O3 -shared -fPIC -Wl,-soname,libwritedduper.so -o libwritedduper.so main.c
