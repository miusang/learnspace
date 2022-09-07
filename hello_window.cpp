#include "glad/glad.h"
#include <GLFW/glfw3.h>

#include <iostream>
using namespace std;


static int winH = 480;
static int winW = 640;

void frame_buffer_size_callback(GLFWwindow *window, int width, int height) {
    // cout << "size changed, w:" << width << ", h: " << height << "." << endl;
    glViewport(0, 0, width, height);
}

int main() {
    cout << "hello window test" << endl;
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow *window = glfwCreateWindow(winW, winH, "hello window", NULL, NULL);
    if (!window) {
        cout << "glfw create window failed." << endl;
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        cout << "glad init failed." << endl;
        return -1;
    }
    glViewport(0, 0, winW, winH);
    glfwSetFramebufferSizeCallback(window, frame_buffer_size_callback);
    while (!glfwWindowShouldClose(window)) {
        glClearColor(0.1, 0.0, 0.1, 0.5);
        glClear(GL_COLOR_BUFFER_BIT);
        glfwSwapBuffers(window);
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, true);
        }
    }
    glfwTerminate();
    return 0;
}