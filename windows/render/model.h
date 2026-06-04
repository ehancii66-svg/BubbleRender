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

    // Access underlying meshes (for dynamic update)
    Mesh* getMesh(int index) {
        if (index >= 0 && index < (int)m_meshes.size())
            return &m_meshes[index];
        return nullptr;
    }
    int getMeshCount() const { return (int)m_meshes.size(); }

    static Model* CreateSphere(float radius = 1.0f, int sector = 36, int stacks = 18, bool withTexCoords = true);
    static Model* CreateSkyboxCube();

    // Create a Model from arbitrary vertex data (for simulation mesh rendering)
    static Model* CreateFromVertices(
        const std::vector<glm::vec3>& positions,
        const std::vector<glm::vec3>& normals,
        const std::vector<unsigned int>& indices);

private:
    std::string m_directory;
    std::vector<Texture> m_textures_loaded;
    std::vector<Mesh> m_meshes;
    glm::vec3 m_maxXyz, m_minXyz;

    void updateMaxMinXyz(glm::vec3 pos);
};

#endif // WINDOWS_MODEL_H
