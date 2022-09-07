
/**
 * @brief 纹理测试 
 */
#include "glad/glad.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"


static int winH = 480;
static int winW = 640;

const char *vertexShaderSource = "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 1) in vec2 aTexCoord;\n"
    "out vec2 TexCoord;\n"
    "void main()\n"
    "{\n"
    "   gl_Position = vec4(aPos, 1.0);\n"
    "   TexCoord = aTexCoord;\n"
    "}\0";

const char *fragmentShaderSource = "#version 330 core\n"
    "out vec4 FragColor;\n"
    "in vec2 TexCoord;\n"
    "uniform sampler2D ourTexture;\n"
    "void main()\n"
    "{\n"
    "    FragColor = texture(ourTexture, TexCoord);\n"
    "}\0";

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

    // 加载图像
    int width, height, nrChannels;
    unsigned char *data = stbi_load("../res/wall.jpg", &width, &height, &nrChannels, 0);

    // 生成纹理
    unsigned int texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);	// set texture wrapping to GL_REPEAT (default wrapping method)
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    // set texture filtering parameters
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(
        GL_TEXTURE_2D, // 指定纹理目标
        0, // 指定多级渐远纹理的级别，0为基本级别
        GL_RGB, // 指定纹理存储的格式，测试用的图像只有RGB值，因此我们也把纹理储存为RGB值
        width, height, // 纹理的宽度和高度
        0, // 不考虑，直接置0
        GL_RGB, GL_UNSIGNED_BYTE, // 源图的格式和数据类型
        data // 图像数据
        );
    glGenerateMipmap(GL_TEXTURE_2D); // 自动生成所有需要的多级渐远纹理
    stbi_image_free(data); // 纹理已经生成，可以过河拆桥

    // 纹理坐标系和顶点坐标系不同
    float vertices[] =  {
    //    ---- 位置 ----   --- 纹理坐标 ---
        -0.5f, -0.5f, 0.0f,  0.0f, 1.0f,
        0.5f, -0.5f, 0.0f,   1.0f, 1.0f,
        0.0f, 0.5f, 0.0f,    0.5f, 0.0f,
    };

    unsigned int VAO, VBO;
    glGenVertexArrays(1, &VAO);
    glBindVertexArray(VAO);
    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    while (!glfwWindowShouldClose(window)) {
        glClearColor(0.1, 0.0, 0.1, 0.5);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(shaderProgram); // 激活程序
        glBindVertexArray(VAO);
        // glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
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