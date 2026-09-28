CXX := g++
CXXFLAGS := -Wall -Wextra -O2
CPPFLAGS := -Iimgui -Iimgui/backends $(shell pkg-config --cflags glfw3)
LDLIBS := $(shell pkg-config --libs glfw3) -lGLEW -lGL

TARGET := simple-shader-playground

IMGUI_SRC := \
	imgui/imgui.cpp \
	imgui/imgui_draw.cpp \
	imgui/imgui_tables.cpp \
	imgui/imgui_widgets.cpp \
	imgui/backends/imgui_impl_glfw.cpp \
	imgui/backends/imgui_impl_opengl3.cpp

SRC := shader_playground_imgui.cpp $(IMGUI_SRC)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(SRC) -o $@ $(LDLIBS)

run: $(TARGET)
	./$(TARGET) main.frag 1920 960

clean:
	rm -f $(TARGET)
