/**
 * @brief 利用opengl显示一张yuv420p格式的图片
 * 
 */

#include "glad/glad.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <string.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static int winH = 368;
static int winW = 640;

const char *vertexShaderSource = "#version 330 core\n"
    "layout (location = 0) in vec3 aPos;\n"
    "layout (location = 1) in vec2 aTexCoord;\n"
    "out vec2 TexCoord;\n"
    "void main()\n"
    "{\n"
    "   gl_Position = vec4(aPos, 1.0);\n"
    "   TexCoord = vec2(aTexCoord.x, 1.0 - aTexCoord.y);\n"
    "}\0";

const char *fragmentShaderSource = "#version 330 core\n"
    "out vec4 FragColor;\n"
    "in vec2 TexCoord;\n"
    "uniform sampler2D yTexture;\n"
    "uniform sampler2D uTexture;\n"
    "uniform sampler2D vTexture;\n"
    "void main()\n"
    "{\n"
    "    vec3 yuv;\n"
    "    vec3 rgb;\n"
    "    yuv.x = texture(yTexture, TexCoord).r;\n"
    "    yuv.y = texture(uTexture, TexCoord).r - 0.5;\n"
    "    yuv.z = texture(vTexture, TexCoord).r - 0.5;\n"
    "    rgb = mat3(\n"
    "           1.0, 1.0,      1.0,\n"
    "           0.0, -0.39465, 2.03211,\n"
    "           1.13983, -0.5806, 0.0\n"
    "           ) * yuv;\n"
    "    FragColor = vec4(rgb, 1.0);\n"
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

    glUseProgram(shaderProgram);
    
    
    

    // 读取yuv数据
    int width = 640, height = 368; // 图像的宽高
    FILE *fp = fopen("/home/ning/res/b.yuv", "r");
    if (!fp) {
        printf("yuv file open failed.\n");
    }
    unsigned char *buf[3] = {0};
    buf[0] = new unsigned char[width * height]; // y
    buf[1] = new unsigned char[width * height / 4]; // u
    buf[2] = new unsigned char[width * height / 4]; // v
    int size = fread(buf[0], 1, width * height, fp);
    printf("read size: %d, need size %d.\n", size, width * height);
    size = fread(buf[1], 1, width * height / 4, fp);
    printf("read size: %d, need size %d.\n", size, width * height / 4);
    size = fread(buf[2], 1, width * height / 4, fp);
    printf("read size: %d, need size %d.\n", size, width * height / 4);

    // 创建y、u、v分量的纹理
    unsigned int textures[3];
    glGenTextures(3, textures);
    // y
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, buf[0]);
    glUniform1i(glGetUniformLocation(shaderProgram, "yTexture"), 0); // 要在此之前激活程序
    // u
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width / 2, height / 2, 0, GL_RED, GL_UNSIGNED_BYTE, buf[1]);
    glUniform1i(glGetUniformLocation(shaderProgram, "uTexture"), 1); 
    // v
    glBindTexture(GL_TEXTURE_2D, textures[2]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RED, width / 2, height / 2, 0, GL_RED, GL_UNSIGNED_BYTE, buf[2]);
    glUniform1i(glGetUniformLocation(shaderProgram, "vTexture"), 2);

    // 清除不用数据
    delete buf[0];
    delete buf[1];
    delete buf[2];
    fclose(fp);


    
    // 纹理坐标系和顶点坐标
    float vertices[] =  {
    //    ---- 位置 ----     --- 纹理坐标 ---
         1.0f, -1.0f, 0.0f,    1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f,    0.0f, 0.0f,
         1.0f,  1.0f, 0.0f,    1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f,    0.0f, 1.0f
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
        
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, textures[0]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, textures[1]);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, textures[2]);

        glUseProgram(shaderProgram); // 激活程序
        glBindVertexArray(VAO);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
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