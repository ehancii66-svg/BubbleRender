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

#ifndef REFRACTIONMODEL_MODEL_H
#define REFRACTIONMODEL_MODEL_H
#pragma once

#include <fstream>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string>

#include <vector>
#include "assimp/Importer.hpp"
#include "assimp/scene.h"
#include "assimp/postprocess.h"
#include "mesh.h"
#include "shader.h"

#include <rawfile/raw_file_manager.h>

using namespace std;

class Model {
public:
    Model();
    
    ~Model();
    
    void Draw(Shader shader);
    
    static Model* CreateSphere(float radius = 1.0f, int sector = 36, int stacks = 18, bool withTexCoords = true);

    static Model* CreateSkyboxCube();

private:
    string m_directory;
    vector<Texture> m_textures_loaded;
    vector<Mesh> m_meshes;
    glm::vec3 m_maxXyz, m_minXyz;
    bool m_gammaCorrection;
    bool m_hasTexture;
    bool m_loadTextures;
    
    void updateMaxMinXyz(glm::vec3 pos);

};
#endif // REFRACTIONMODEL_MODEL_H