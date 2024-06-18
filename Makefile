lib:
	$(CC) -g -ldl -lhiredis -O3 -shared -fPIC -Wl,-soname,libwritededuper.so -o libwritededuper.so main.c
