lib:
	cc -ldl -lm -O3 -shared -fPIC -Wl,-soname,libwritededuper.so -o libwritededuper.so main.c
