Usage:
```
./simple-shader-playground
./simple-shader-playground shader.frag
./simple-shader-playground shader.frag 600 600
./simple-shader-playground shaders/test.frag 800 600
```

Compile:
```
gcc -Wall -Wextra -O2 main.c -o simple-shader-playground $(pkg-config --cflags --libs glfw3) -lGLEW -lGL
```
