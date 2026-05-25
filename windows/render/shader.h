//
// Desktop OpenGL version — cleaned up from HarmonyOS original
//

#ifndef WINDOWS_SHADER_H
#define WINDOWS_SHADER_H

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <iostream>

class Shader {
public:
    unsigned int m_id;

    Shader(const char* vertexStr, const char* fragmentStr)
    {
        m_id = CreateProgram(vertexStr, fragmentStr);
    }

    ~Shader() {}

    void Destroy() { DeleteProgram(m_id); }
    void Use() { glUseProgram(m_id); }

    void SetBool(const std::string& name, bool value) const
    {
        glUniform1i(glGetUniformLocation(m_id, name.c_str()), (int)value);
    }
    void SetInt(const std::string& name, int value) const
    {
        glUniform1i(glGetUniformLocation(m_id, name.c_str()), value);
    }
    void SetFloat(const std::string& name, float value) const
    {
        glUniform1f(glGetUniformLocation(m_id, name.c_str()), value);
    }
    void SetVec2(const std::string& name, const glm::vec2& value) const
    {
        glUniform2fv(glGetUniformLocation(m_id, name.c_str()), 1, &value[0]);
    }
    void SetVec2(const std::string& name, float x, float y) const
    {
        glUniform2f(glGetUniformLocation(m_id, name.c_str()), x, y);
    }
    void SetVec3(const std::string& name, const glm::vec3& value) const
    {
        glUniform3fv(glGetUniformLocation(m_id, name.c_str()), 1, &value[0]);
    }
    void SetVec3(const std::string& name, float x, float y, float z) const
    {
        glUniform3f(glGetUniformLocation(m_id, name.c_str()), x, y, z);
    }
    void SetVec4(const std::string& name, const glm::vec4& value) const
    {
        glUniform4fv(glGetUniformLocation(m_id, name.c_str()), 1, &value[0]);
    }
    void SetVec4(const std::string& name, float x, float y, float z, float w)
    {
        glUniform4f(glGetUniformLocation(m_id, name.c_str()), x, y, z, w);
    }
    void SetMat2(const std::string& name, const glm::mat2& mat) const
    {
        glUniformMatrix2fv(glGetUniformLocation(m_id, name.c_str()), 1, GL_FALSE, &mat[0][0]);
    }
    void SetMat3(const std::string& name, const glm::mat3& mat) const
    {
        glUniformMatrix3fv(glGetUniformLocation(m_id, name.c_str()), 1, GL_FALSE, &mat[0][0]);
    }
    void SetMat4(const std::string& name, const glm::mat4& mat) const
    {
        glUniformMatrix4fv(glGetUniformLocation(m_id, name.c_str()), 1, GL_FALSE, &mat[0][0]);
    }

private:
    GLuint CreateProgram(const char* pVertexShaderSource, const char* pFragShaderSource);
    GLuint LoadShader(GLenum shaderType, const char* pSource);
    void CheckGLError(const char* pGLOperation);
    void DeleteProgram(GLuint& program);
};

#endif // WINDOWS_SHADER_H
