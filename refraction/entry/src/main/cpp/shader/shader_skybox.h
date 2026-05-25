//
// Created on 2026/4/29.
//
// Node APIs are not fully supported. To solve the compilation error of the interface cannot be found,
// please include "napi/native_api.h".

#ifndef REFRACTIONMODEL_SHADER_SKYBOX_H
#define REFRACTIONMODEL_SHADER_SKYBOX_H

const char* skyboxVertexShader = R"(#version 300 es
layout(location = 0) in vec3 aPos;
out vec3 TexCoords;
uniform mat4 projection;
uniform mat4 view;
void main() {
    TexCoords = aPos;
    vec4 pos = projection * view* vec4(aPos, 1.0);
    gl_Position = pos.xyww;
}
)";

const char* skyboxFragmentShader = R"(#version 300 es
precision highp float;
in vec3 TexCoords;
out vec4 FragColor;
uniform samplerCube skybox;
void main() {
    FragColor = texture(skybox, TexCoords);
}
)";

#endif //REFRACTIONMODEL_SHADER_SKYBOX_H
