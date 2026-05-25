//
// Desktop OpenGL 3.3 version of quad shader
//

#ifndef WINDOWS_SHADER_QUAD_H
#define WINDOWS_SHADER_QUAD_H

const char* quadVertexShader = R"(#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aPos * 0.5 + 0.5;
}
)";

const char* quadFragmentShader = R"(#version 330 core
uniform sampler2D uTexture;
in vec2 vUV;
out vec4 FragColor;
void main() {
    FragColor = texture(uTexture, vUV);
}
)";

#endif // WINDOWS_SHADER_QUAD_H
