/*
 * Copyright (c) 2024 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "model.h"
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <cmath>

const int NUM_RGB = 3;
const int NUM_RGBA = 4;

Model::Model()
{
    m_maxXyz = glm::vec3(-FLT_MAX);
    m_minXyz = glm::vec3(FLT_MAX);
    m_gammaCorrection = false;
}

Model::~Model()
{
    for (Mesh &mesh : m_meshes) {
        mesh.Destroy();
    }
    m_meshes.clear();
    m_textures_loaded.clear();
}

Model* Model::CreateSphere(float radius, int sectors, int stacks, bool withTexCoords)
{
    Model* sphereModel = new Model();
    
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    
    float sectorStep = 2.0f * M_PI / sectors;
    float stackStep = M_PI / stacks;
    
    for (int i = 0; i <= stacks; ++i) {
        float stackAngle = M_PI / 2.0f - i * stackStep;
        float xy = radius * cosf(stackAngle);
        float z = radius * sinf(stackAngle);
        
        for (int j = 0; j <= sectors; ++j) {
            float sectorAngle = j * sectorStep;
            
            Vertex vertex;

            float x = xy * cosf(sectorAngle);
            float y = xy * sinf(sectorAngle);
            vertex.Position = glm::vec3(x, y, z);
            sphereModel->updateMaxMinXyz(vertex.Position);
            
            vertex.Normal = glm::normalize(vertex.Position);
            
            if (withTexCoords) {
                vertex.TexCoords.x = (float)j / sectors;
                vertex.TexCoords.y = (float)i / stacks;
            } else {
                vertex.TexCoords = glm::vec2(0.0f);
            }
            
            vertex.Tangent = glm::vec3(0.0f);
            vertex.Bitangent = glm::vec3(0.0f);
            
            vertices.push_back(vertex);
        }
    }
    
    for (int i = 0; i < stacks; ++i) {
        int k1 = i * (sectors + 1);
        int k2 = k1 + sectors + 1;
        
        for (int j = 0; j < sectors; ++j, ++k1, ++k2) {
            if (i != 0) {
                indices.push_back(k1);
                indices.push_back(k2);
                indices.push_back(k1 + 1);
            }
            if (i != (stacks - 1)) {
                indices.push_back(k1 + 1);
                indices.push_back(k2);
                indices.push_back(k2 + 1);
            }
        }
    }
    
    Mesh sphereMesh(vertices, indices, std::vector<Texture>());
    sphereModel->m_meshes.push_back(sphereMesh);
    
    return sphereModel;
}

Model* Model::CreateSkyboxCube()
{
    Model* skyboxModel = new Model();
    
    float skyboxVertices[] = {
        -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,

        -1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,

         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,

        -1.0f, -1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,

        -1.0f,  1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f, -1.0f,

        -1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f,  1.0f
    };
    
    std::vector<Vertex> vertices(36);
    for (int i = 0; i < 36; ++i) {
        vertices[i].Position = glm::vec3(
             skyboxVertices[i * 3],
             skyboxVertices[i * 3 + 1],
             skyboxVertices[i * 3 + 2]);
        
        vertices[i].Normal = glm::vec3(0.0f);
        vertices[i].TexCoords = glm::vec2(0.0f);
        vertices[i].Tangent = glm::vec3(0.0f);
        vertices[i].Bitangent = glm::vec3(0.0f);
    }
    
    std::vector<unsigned int> indices(36);
    for (unsigned int i = 0; i < 36; ++i)
        indices[i] = i;
    
    Mesh skyboxMesh(vertices, indices, std::vector<Texture>());
    skyboxModel->m_meshes.push_back(skyboxMesh);
    
    return skyboxModel;
}

void Model::Draw(Shader shader)
{
    for (unsigned int i = 0; i < m_meshes.size(); i++) {
        m_meshes[i].Draw(shader);
    }
}

void Model::updateMaxMinXyz(glm::vec3 pos)
{
    m_maxXyz.x = pos.x > m_maxXyz.x ? pos.x : m_maxXyz.x;
    m_maxXyz.y = pos.y > m_maxXyz.y ? pos.y : m_maxXyz.y;
    m_maxXyz.z = pos.z > m_maxXyz.z ? pos.z : m_maxXyz.z;

    m_minXyz.x = pos.x < m_minXyz.x ? pos.x : m_minXyz.x;
    m_minXyz.y = pos.y < m_minXyz.y ? pos.y : m_minXyz.y;
    m_minXyz.z = pos.z < m_minXyz.z ? pos.z : m_minXyz.z;
}
