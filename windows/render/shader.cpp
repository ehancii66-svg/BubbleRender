//
// Desktop OpenGL version — cleaned up from HarmonyOS original
//

#include "shader.h"

GLuint Shader::CreateProgram(const char* pVertexShaderSource, const char* pFragShaderSource)
{
    if ((nullptr == pVertexShaderSource) || (nullptr == pFragShaderSource)) {
        std::cerr << "SHADER createProgram: vertexShader or fragShader is null" << std::endl;
        return 0;
    }
    GLuint program = 0;
    GLuint vertexShaderHandle = LoadShader(GL_VERTEX_SHADER, pVertexShaderSource);
    if (!vertexShaderHandle) return program;
    GLuint fragShaderHandle = LoadShader(GL_FRAGMENT_SHADER, pFragShaderSource);
    if (!fragShaderHandle) return program;

    program = glCreateProgram();
    if (program) {
        glAttachShader(program, vertexShaderHandle);
        CheckGLError("glAttachShader");
        glAttachShader(program, fragShaderHandle);
        CheckGLError("glAttachShader");
        glLinkProgram(program);
        GLint linkStatus = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &linkStatus);

        glDetachShader(program, vertexShaderHandle);
        glDeleteShader(vertexShaderHandle);
        vertexShaderHandle = 0;
        glDetachShader(program, fragShaderHandle);
        glDeleteShader(fragShaderHandle);
        fragShaderHandle = 0;
        if (linkStatus != GL_TRUE) {
            GLint bufLength = 0;
            glGetProgramiv(program, GL_INFO_LOG_LENGTH, &bufLength);
            if (bufLength) {
                char* buf = (char*)malloc((size_t)bufLength);
                if (buf) {
                    glGetProgramInfoLog(program, bufLength, NULL, buf);
                    std::cerr << "SHADER CreateProgram Could not link program:" << std::endl << buf << std::endl;
                    free(buf);
                }
            }
            glDeleteProgram(program);
            program = 0;
        }
    }
    return program;
}

GLuint Shader::LoadShader(GLenum shaderType, const char* pSource)
{
    GLuint shader = glCreateShader(shaderType);
    if (shader) {
        glShaderSource(shader, 1, &pSource, NULL);
        glCompileShader(shader);
        GLint compiled = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            GLint infoLen = 0;
            glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLen);
            if (infoLen) {
                char* buf = (char*)malloc((size_t)infoLen);
                if (buf) {
                    glGetShaderInfoLog(shader, infoLen, NULL, buf);
                    std::cerr << "SHADER LoadShader Could not compile shader " << shaderType << ":" << std::endl << buf << std::endl;
                    free(buf);
                }
                glDeleteShader(shader);
                shader = 0;
            }
        }
    }
    return shader;
}

void Shader::DeleteProgram(GLuint& program)
{
    if (program) {
        glUseProgram(0);
        glDeleteProgram(program);
        program = 0;
    }
}

void Shader::CheckGLError(const char* pGLOperation)
{
    for (GLint error = glGetError(); error; error = glGetError()) {
        std::cerr << "SHADER CheckGLError GL Operation " << pGLOperation << " glError (0x" << std::hex << error << ")" << std::endl;
    }
}
