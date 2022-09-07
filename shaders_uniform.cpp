/**
 * @brief 
 * @date 2022.09.05
 */

#include "glad/glad.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

const char *vertexShaderSource = "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "uniform float offsetX;\n"
    "void main()\n"
    "{\n"
    "   gl_Position = vec4(aPos.x + offsetX, aPos.y, aPos.z, 1.0);\n"
    "}\0";

const char *fragmentShaderSource = "#version 330 core\n"
    "out vec4 FragColor;\n"
    "uniform vec4 ourColor;\n"
    "void main()\n"
    "{\n"
    "    FragColor = ourColor;\n"
    "}\0";

static int winH = 480;
static int winW = 640;

/**
 * 渲染管线：
 * 
 * 顶点数据     --->    顶点着色器     --->     形状装配    --->    几何着色器
 *              --->    光栅化         --->     片段着色器  --->    测试与混合
 *                                                                       
 */
int main() {
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow *window = glfwCreateWindow(winW, winH, "hello window", NULL, NULL);
    if (!window) {
        printf("glfw create window failed.\n");
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        printf("glad init failed.\n");
        return -1;
    }
    glViewport(0, 0, winW, winH);

    // 顶点坐标
    float vertices[] = {
        -0.5f, -0.5f, 0.0f,
        0.5f, -0.5f, 0.0f,
        0.0f, 0.5f, 0.0f
    };

    // 创建一个顶点缓冲对象，Vertex Buffer Object， 用来向gpu发送顶点
    unsigned int VAO, VBO;
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);
    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO); // 设置缓冲类型
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW); // 将数据复制到显存中
    // 解析顶点数据
    glVertexAttribPointer(
        0, // 需要配置的顶点属性，和 layout(location = 0) 中的0一致，但是有范围限制
        3, // 顶点属性的大小。顶点属性是 vec3，由3个值组成，所以是3
        GL_FLOAT, // 数据类型
        GL_FALSE, // 是否希望数据被标准化到-1, 1之间
        0,//sizeof(float) * 3, // 步长
        (void *)0 // 位置数据在缓冲中起始位置的偏移量(Offset)
        );
    glEnableVertexAttribArray(0);

    // 创建顶点着色器
    unsigned int vertexShader;
    vertexShader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertexShader, 1, &vertexShaderSource, NULL); // 将着色器源码附加到着色器对象上
    glCompileShader(vertexShader);  // 编译着色器源码
    int success;
    char info[512];
    glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(vertexShader, 512, NULL, info);
        printf("vertex shader source compile failed, %s.\n", info);
    }

    // 创建片段着色器
    unsigned int fragmentShader;
    fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragmentShader, 1, &fragmentShaderSource, NULL);
    glCompileShader(fragmentShader);
    memset(info, 0, sizeof(info));
    glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
    if (!success) {
        glGetShaderInfoLog(fragmentShader, 512, NULL, info);
        printf("fragment shader source compile failed, %s.\n", info);
    }

    // 创建着色器程序
    unsigned int shaderProgram;
    shaderProgram = glCreateProgram();
    glAttachShader(shaderProgram, vertexShader);
    glAttachShader(shaderProgram, fragmentShader);
    glLinkProgram(shaderProgram);
    glGetProgramiv(shaderProgram, GL_LINK_STATUS, &success);
        if (!success) {
        glGetProgramInfoLog(shaderProgram, 512, NULL, info);
        printf("shader program linked failed, %s.\n", info);
    }
    glDeleteShader(vertexShader); // 将着色器，链接到着色器程序中后，就不需要着色器对象了，此处删除。
    glDeleteShader(fragmentShader);



    // 线框模式
    // glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);

    while (!glfwWindowShouldClose(window)) {
        glClearColor(0.1, 0.0, 0.1, 0.5);
        glClear(GL_COLOR_BUFFER_BIT);
        float timeValue = glfwGetTime();
        float greenValue = (sin(timeValue) / 2.0f) + 0.5f;
        float offsetX = sin(timeValue) / 2.0f;
        int vertexColorLocation = glGetUniformLocation(shaderProgram, "ourColor");
        int offsetLocation = glGetUniformLocation(shaderProgram, "offsetX");
        glUseProgram(shaderProgram); // 激活程序
        glUniform4f(vertexColorLocation, 0.0f, greenValue, 0.0f, 1.0f);
        glUniform1f(offsetLocation, offsetX);
        glBindVertexArray(VAO);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glfwSwapBuffers(window);
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, true);
        }
    }

    glDeleteBuffers(1, &VBO);
    glDeleteVertexArrays(1, &VAO);
    glDeleteProgram(shaderProgram);
    
    glfwTerminate();
    return 0;
}