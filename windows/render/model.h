//
// Desktop OpenGL version — cleaned up from HarmonyOS original
//

#ifndef WINDOWS_MODEL_H
#define WINDOWS_MODEL_H

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <string>
#include <vector>

#include "mesh.h"
#include "shader.h"

class Model {
public:
    Model();
    ~Model();

    void Draw(Shader& shader);

    static Model* CreateSphere(float radius = 1.0f, int sector = 36, int stacks = 18, bool withTexCoords = true);
    static Model* CreateSkyboxCube();

private:
    std::string m_directory;
    std::vector<Texture> m_textures_loaded;
    std::vector<Mesh> m_meshes;
    glm::vec3 m_maxXyz, m_minXyz;

    void updateMaxMinXyz(glm::vec3 pos);
};

#endif // WINDOWS_MODEL_H
