lib: writedupe.c
	cc -ldl -lm -shared -fPIC -Wl,-soname,libwritedupe.so -o libwritedupe.so writedupe.c
