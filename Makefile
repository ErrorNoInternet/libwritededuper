lib: writedupe.c
	cc -shared -fPIC -ldl -Wl,-soname,libwritedupe.so -o libwritedupe.so writedupe.c
