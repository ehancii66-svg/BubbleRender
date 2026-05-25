//
// Desktop OpenGL 3.3 version of skybox shader
//

#ifndef WINDOWS_SHADER_SKYBOX_H
#define WINDOWS_SHADER_SKYBOX_H

const char* skyboxVertexShader = R"(#version 330 core
layout(location = 0) in vec3 aPos;
out vec3 TexCoords;
uniform mat4 projection;
uniform mat4 view;
void main() {
    TexCoords = aPos;
    vec4 pos = projection * view * vec4(aPos, 1.0);
    gl_Position = pos.xyww;
}
)";

const char* skyboxFragmentShader = R"(#version 330 core
in vec3 TexCoords;
out vec4 FragColor;
uniform samplerCube skybox;
void main() {
    FragColor = texture(skybox, TexCoords);
}
)";

#endif // WINDOWS_SHADER_SKYBOX_H
