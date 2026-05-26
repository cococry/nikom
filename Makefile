BIN=nikom
LIB=-lX11 -lXcomposite -lXdamage -lXfixes -lGL -lXrandr

all:
	mkdir -p build
	$(CC) -o build/${BIN} src/*.c ${LIB}
run:
	./build/nikom 
