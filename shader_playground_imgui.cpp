// I forgot to note, this whole thing is completely vibecoded, I dont have
// enegry to deal with dear imgui and opengl

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "imgui.h"

/* ------------------------------------------------------------
 * ImGui / shader controls
 * ------------------------------------------------------------ */

#define UI_WIDTH 300.0f
#define COEFFICIENTS_LENGTH 11

typedef struct {
    float coefficients[COEFFICIENTS_LENGTH];
} ShaderParams;

bool show_ui = false;

/*
 * Fullscreen triangle vertex shader.
 *
 * A triangle larger than the screen is used instead of a quad.
 * This avoids needing a VBO/VAO for a simple shader playground.
 */
static const char *vertex_src =
    "#version 330 core\n"
    "\n"
    "const vec2 vertices[3] = vec2[](\n"
    "    vec2(-1.0, -1.0),\n"
    "    vec2( 3.0, -1.0),\n"
    "    vec2(-1.0,  3.0)\n"
    ");\n"
    "\n"
    "void main() {\n"
    "    gl_Position = vec4(vertices[gl_VertexID], 0.0, 1.0);\n"
    "}\n";

/* ------------------------------------------------------------
 * Read an entire file into memory.
 * ------------------------------------------------------------ */

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");

    if (!file) {
        perror(path);
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }

    long size = ftell(file);

    if (size < 0) {
        fclose(file);
        return NULL;
    }

    rewind(file);

    char *buffer = (char *)malloc((size_t)size + 1);

    if (!buffer) {
        fclose(file);
        fprintf(stderr, "Out of memory\n");
        return NULL;
    }

    size_t bytes_read = fread(buffer, 1, (size_t)size, file);
    buffer[bytes_read] = '\0';

    fclose(file);

    return buffer;
}

/* ------------------------------------------------------------
 * Compile one shader.
 * ------------------------------------------------------------ */

static GLuint compile_shader(GLenum type, const char *source) {
    GLuint shader = glCreateShader(type);

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint success = GL_FALSE;

    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);

    if (!success) {
        char log[4096];

        glGetShaderInfoLog(shader, sizeof(log), NULL, log);

        fprintf(stderr, "\n--- Shader compilation failed ---\n");
        fprintf(stderr, "%s\n", log);
        fprintf(stderr, "---------------------------------\n\n");

        glDeleteShader(shader);

        return 0;
    }

    return shader;
}

/* ------------------------------------------------------------
 * Build the complete shader program.
 *
 * Returns 0 if compilation/linking fails.
 * ------------------------------------------------------------ */

static GLuint build_program(const char *shader_file) {
    char *fragment_src = read_file(shader_file);

    if (!fragment_src) {
        return 0;
    }

    GLuint vertex_shader = compile_shader(GL_VERTEX_SHADER, vertex_src);

    if (!vertex_shader) {
        free(fragment_src);
        return 0;
    }

    GLuint fragment_shader = compile_shader(GL_FRAGMENT_SHADER, fragment_src);

    free(fragment_src);

    if (!fragment_shader) {
        glDeleteShader(vertex_shader);
        return 0;
    }

    GLuint program = glCreateProgram();

    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);

    glLinkProgram(program);

    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    GLint success = GL_FALSE;

    glGetProgramiv(program, GL_LINK_STATUS, &success);

    if (!success) {
        char log[4096];

        glGetProgramInfoLog(program, sizeof(log), NULL, log);

        fprintf(stderr, "\n--- Program linking failed ---\n");
        fprintf(stderr, "%s\n", log);
        fprintf(stderr, "-------------------------------\n\n");

        glDeleteProgram(program);

        return 0;
    }

    fprintf(stderr, "Shader compiled successfully.\n");

    return program;
}

/* ------------------------------------------------------------
 * Get the directory portion of a path.
 *
 * Examples:
 *
 *     shader.frag
 *         -> "."
 *
 *     shaders/test.frag
 *         -> "shaders"
 *
 *     /home/user/shaders/test.frag
 *         -> "/home/user/shaders"
 * ------------------------------------------------------------ */

static char *get_directory(const char *path) {
    const char *slash = strrchr(path, '/');

    if (!slash) {
        char *result = (char *)malloc(2);

        if (!result) {
            return NULL;
        }

        strcpy(result, ".");

        return result;
    }

    size_t length = (size_t)(slash - path);

    if (length == 0) {
        length = 1;
    }

    char *directory = (char *)malloc(length + 1);

    if (!directory) {
        return NULL;
    }

    memcpy(directory, path, length);
    directory[length] = '\0';

    return directory;
}

/* ------------------------------------------------------------
 * Get the filename portion of a path.
 *
 * Example:
 *
 *     shaders/test.frag
 *         -> test.frag
 * ------------------------------------------------------------ */

static const char *get_filename(const char *path) {
    const char *slash = strrchr(path, '/');

    if (!slash) {
        return path;
    }

    return slash + 1;
}

/* ------------------------------------------------------------
 * Set up Linux inotify.
 *
 * We watch the directory containing the shader rather than the
 * file itself because editors such as Vim commonly save by
 * writing a temporary file and renaming it over the original.
 * ------------------------------------------------------------ */

static int setup_inotify(const char *shader_file) {
    char *directory = get_directory(shader_file);

    if (!directory) {
        fprintf(stderr, "Failed to allocate shader directory.\n");
        return -1;
    }

    int fd = inotify_init1(IN_NONBLOCK);

    if (fd == -1) {
        perror("inotify_init1");
        free(directory);
        return -1;
    }

    int watch = inotify_add_watch(fd, directory,
                                  IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);

    if (watch == -1) {
        perror("inotify_add_watch");
        close(fd);
        free(directory);
        return -1;
    }

    free(directory);

    return fd;
}

/* ------------------------------------------------------------
 * Check whether the shader changed.
 *
 * Returns:
 *
 *   1 = shader changed
 *   0 = no change
 *  -1 = error
 * ------------------------------------------------------------ */

static int shader_changed(int inotify_fd, const char *shader_file) {
    char buffer[4096]
        __attribute__((aligned(__alignof__(struct inotify_event))));

    ssize_t length = read(inotify_fd, buffer, sizeof(buffer));

    if (length < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }

        perror("read(inotify)");
        return -1;
    }

    const char *filename = get_filename(shader_file);

    int changed = 0;

    for (char *ptr = buffer; ptr < buffer + length;) {

        struct inotify_event *event = (struct inotify_event *)ptr;

        if (event->len > 0 && strcmp(event->name, filename) == 0) {

            if (event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE)) {

                changed = 1;
            }
        }

        ptr += sizeof(struct inotify_event) + event->len;
    }

    return changed;
}

/* ------------------------------------------------------------
 * Print command-line usage.
 * ------------------------------------------------------------ */

static void print_usage(const char *program_name) {
    fprintf(stderr,
            "Usage:\n"
            "  %s [shader_file]\n"
            "  %s [shader_file] [width height]\n"
            "\n"
            "Examples:\n"
            "  %s\n"
            "  %s shader.frag\n"
            "  %s shader.frag 600 600\n"
            "  %s shaders/test.frag 800 600\n",
            program_name, program_name, program_name, program_name,
            program_name, program_name);
}

/* ------------------------------------------------------------
 * Main
 * ------------------------------------------------------------ */

int main(int argc, char **argv) {

    /* --------------------------------------------------------
     * Command-line arguments
     * -------------------------------------------------------- */

    if (argc != 1 && argc != 2 && argc != 4) {

        print_usage(argv[0]);
        return 1;
    }

    /*
     * Default shader.
     *
     * If no filename is provided, use shader.frag.
     */
    const char *shader_file = "shader.frag";

    if (argc >= 2) {
        shader_file = argv[1];
    }

    /*
     * requested_width / requested_height are zero when
     * no custom size was supplied.
     */
    int requested_width = 0;
    int requested_height = 0;

    if (argc == 4) {
        requested_width = atoi(argv[2]);
        requested_height = atoi(argv[3]);

        if (requested_width <= 0 || requested_height <= 0) {

            fprintf(stderr, "Width and height must be positive integers.\n");

            return 1;
        }
    }

    /* --------------------------------------------------------
     * GLFW initialization
     * -------------------------------------------------------- */

    if (!glfwInit()) {
        fprintf(stderr, "Failed to initialize GLFW\n");

        return 1;
    }

    /*
     * Request OpenGL 3.3 Core.
     *
     * This matches:
     *
     *     #version 330 core
     *
     * in the fragment shader.
     */
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    /*
     * The GLFW window itself starts at this size.
     *
     * The actual framebuffer size can be different on HiDPI
     * displays, which is why we use glfwGetFramebufferSize()
     * when no custom render size is supplied.
     */
    int window_width = 1000;
    int window_height = 700;

    if (requested_width > 0 && requested_height > 0) {
        window_width = requested_width + (int)UI_WIDTH;
        window_height = requested_height;
    }

    GLFWwindow *window = glfwCreateWindow(window_width, window_height,
                                          "Shader Playground", NULL, NULL);

    if (!window) {
        fprintf(stderr, "Failed to create GLFW window\n");

        glfwTerminate();

        return 1;
    }

    glfwMakeContextCurrent(window);

    /* --------------------------------------------------------
     * GLEW initialization
     * -------------------------------------------------------- */

    glewExperimental = GL_TRUE;

    GLenum glew_status = glewInit();

    if (glew_status != GLEW_OK) {
        fprintf(stderr, "Failed to initialize GLEW: %s\n",
                glewGetErrorString(glew_status));

        glfwDestroyWindow(window);
        glfwTerminate();

        return 1;
    }

    /* --------------------------------------------------------
     * ImGui initialization
     * -------------------------------------------------------- */

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    ShaderParams params = {
        {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f}};

    /* --------------------------------------------------------
     * VSync
     * -------------------------------------------------------- */

    glfwSwapInterval(1);

    /* --------------------------------------------------------
     * OpenGL state
     * -------------------------------------------------------- */

    glClearColor(0.05f, 0.05f, 0.05f, 1.0f);

    /*
     * With a Core profile we need a VAO bound even though we
     * aren't actually using vertex attributes.
     */
    GLuint vao;

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    /* --------------------------------------------------------
     * Initial shader compilation
     * -------------------------------------------------------- */

    GLuint program = build_program(shader_file);

    if (!program) {
        fprintf(stderr, "Initial shader compilation failed.\n"
                        "Fix the shader and save it to retry.\n");
    }

    /* --------------------------------------------------------
     * Set up inotify
     * -------------------------------------------------------- */

    int inotify_fd = setup_inotify(shader_file);

    if (inotify_fd == -1) {

        if (program) {
            glDeleteProgram(program);
        }

        glDeleteVertexArrays(1, &vao);

        glfwDestroyWindow(window);
        glfwTerminate();

        return 1;
    }

    /* --------------------------------------------------------
     * Startup information
     * -------------------------------------------------------- */

    fprintf(stderr, "\n");
    fprintf(stderr, "Shader playground running.\n");
    fprintf(stderr, "Shader: %s\n", shader_file);

    if (requested_width > 0 && requested_height > 0) {

        fprintf(stderr, "Render size: %d x %d\n", requested_width,
                requested_height);

    } else {
        fprintf(stderr, "Render size: framebuffer size\n");
    }

    fprintf(stderr, "Save the shader to reload it.\n");

    fprintf(stderr, "Press ESC or close the window to exit.\n");

    fprintf(stderr, "\n");

    /* --------------------------------------------------------
     * Main loop
     * -------------------------------------------------------- */

    while (!glfwWindowShouldClose(window)) {

        glfwPollEvents();

        static bool right_shift_was_down = false;

        bool right_shift_down =
            glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS;

        if (right_shift_down && !right_shift_was_down) {
            show_ui = !show_ui;
        }

        right_shift_was_down = right_shift_down;

        /* ----------------------------------------------------
         * ESC closes the window.
         * ---------------------------------------------------- */

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {

            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        /* ----------------------------------------------------
         * Check for shader changes.
         * ---------------------------------------------------- */

        int changed = shader_changed(inotify_fd, shader_file);

        if (changed == -1) {
            break;
        }

        if (changed) {

            fprintf(stderr, "\n");
            fprintf(stderr, "Shader changed. Recompiling...\n");

            GLuint new_program = build_program(shader_file);

            /*
             * Only replace the currently active program if
             * compilation AND linking succeeded.
             */
            if (new_program) {

                if (program) {
                    glDeleteProgram(program);
                }

                program = new_program;

                fprintf(stderr, "New shader activated.\n");

            } else {

                fprintf(stderr, "Keeping previous working shader.\n");
            }
        }

        /* ----------------------------------------------------
         * Determine render dimensions.
         *
         * If the user supplied width/height on the command
         * line, use those.
         *
         * Otherwise use the actual framebuffer dimensions.
         *
         * This matters on HiDPI displays.
         * ---------------------------------------------------- */

        int framebuffer_width;
        int framebuffer_height;
        glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);

        float content_scale_x;
        float content_scale_y;
        glfwGetWindowContentScale(window, &content_scale_x, &content_scale_y);

        int ui_width = (int)(UI_WIDTH * content_scale_x);
        int width;
        int height;

        if (requested_width > 0 && requested_height > 0) {
            width = requested_width;
            height = requested_height;
        } else {
            width = framebuffer_width - ui_width;
            height = framebuffer_height;
        }

        if (width < 1)
            width = 1;
        if (height < 1)
            height = 1;

        glViewport(0, 0, width, height);

        /* ----------------------------------------------------
         * Clear
         * ---------------------------------------------------- */

        glClear(GL_COLOR_BUFFER_BIT);

        /* ----------------------------------------------------
         * Draw shader
         * ---------------------------------------------------- */

        if (program) {

            glUseProgram(program);

            /* ------------------------------------------------
             * Shader uniforms
             * ------------------------------------------------ */

            GLint time = glGetUniformLocation(program, "iTime");

            GLint time_delta = glGetUniformLocation(program, "iTimeDelta");

            GLint frame = glGetUniformLocation(program, "iFrame");

            GLint resolution = glGetUniformLocation(program, "iResolution");

            /*
             * iResolution is a vec2:
             *
             *     uniform vec2 iResolution;
             *
             * Therefore use glUniform2f().
             */
            if (resolution >= 0) {

                glUniform2f(resolution, (float)width, (float)height);
            }

            /* ------------------------------------------------
             * User controls
             * ------------------------------------------------ */

            GLint coefficients = glGetUniformLocation(program, "coefficients");

            if (coefficients >= 0) {
                glUniform1fv(coefficients, COEFFICIENTS_LENGTH,
                             params.coefficients);
            }

            /* ------------------------------------------------
             * Time
             * ------------------------------------------------ */

            float current_time = (float)glfwGetTime();

            static float previous_time = 0.0f;

            float delta = current_time - previous_time;

            previous_time = current_time;

            if (time >= 0) {
                glUniform1f(time, current_time);
            }

            if (time_delta >= 0) {
                glUniform1f(time_delta, delta);
            }

            /* ------------------------------------------------
             * Frame number
             * ------------------------------------------------ */

            static int frame_number = 0;

            if (frame >= 0) {
                glUniform1i(frame, frame_number);
            }

            frame_number++;

            /* ------------------------------------------------
             * Fullscreen triangle.
             * ------------------------------------------------ */

            glDrawArrays(GL_TRIANGLES, 0, 3);
        }

        /* --------------------------------------------------------
         * ImGui controls
         * -------------------------------------------------------- */

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        if (show_ui) {
            int window_pixel_width;
            int window_pixel_height;
            glfwGetWindowSize(window, &window_pixel_width,
                              &window_pixel_height);

            ImGui::SetNextWindowPos(
                ImVec2((float)window_pixel_width - UI_WIDTH, 0.0f),
                ImGuiCond_Always);
            ImGui::SetNextWindowSize(
                ImVec2(UI_WIDTH, (float)window_pixel_height), ImGuiCond_Always);

            ImGuiWindowFlags panel_flags = ImGuiWindowFlags_NoMove |
                                           ImGuiWindowFlags_NoResize |
                                           ImGuiWindowFlags_NoCollapse;

            ImGui::Begin("Shader Controls", NULL, panel_flags);

            ImGui::Text("Shader parameters");
            ImGui::Separator();

            ImGui::Text("Polynomial coefficients");
            ImGui::Separator();

            for (int i = 0; i < COEFFICIENTS_LENGTH; i++) {
                char label[32];

                snprintf(label, sizeof(label), "x^%d",
                         COEFFICIENTS_LENGTH - 1 - i);

                ImGui::SliderFloat(label, &params.coefficients[i], -5.0f, 5.0f,
                                   "%.3f");
            }

            ImGui::End();
        }
        /* ImGui needs the full framebuffer viewport, not the shader viewport.
         */
        glViewport(0, 0, framebuffer_width, framebuffer_height);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    /* --------------------------------------------------------
     * Cleanup
     * -------------------------------------------------------- */

    close(inotify_fd);

    if (program) {
        glDeleteProgram(program);
    }

    glDeleteVertexArrays(1, &vao);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
