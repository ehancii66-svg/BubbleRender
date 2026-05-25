//
// Desktop OpenGL 3.3 version of background shader
//

#ifndef WINDOWS_SHADER_BACKGROUND_H
#define WINDOWS_SHADER_BACKGROUND_H

const char* backgroundVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
uniform mat4 uMVP;
out vec2 vUV;
out vec3 vNormal;
void main() {
    vUV = aTexCoord;
    vNormal = normalize(aNormal);
    gl_Position = uMVP * vec4(aPos, 1.0);
}
)";

const char* backgroundFragmentShader = R"(#version 330 core
in vec2 vUV;
in vec3 vNormal;
out vec4 FragColor;
void main() {
    FragColor = vec4(1.0, 1.0, 1.0, 1.0);
}
)";

#endif // WINDOWS_SHADER_BACKGROUND_H
