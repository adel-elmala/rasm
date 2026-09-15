#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

int main()
{
    // 1. Create engine instance
    rasm::EngineConfig config = {
        .appName = "example_0",
        .windowWidth = 720,
        .windowHeight = 480,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN

    };

    rasm::Engine engine(config);
#if 0

    // 2. Create a scene
    rasm::SceneHandle scene = engine.createScene();

    // 3. Load resources
    rasm::MeshHandle bunnyMesh = engine.loadMesh("assets/models/bunny.obj");

    rasm::TextureHandle albedoTexture = engine.loadTexture("assets/textures/bunny-atlas.jpg");
    rasm::TextureHandle normalTexture = engine.loadTexture("assets/textures/test1.jpg");

    // 4. Create a material
    rasm::TextureHandle textures[] = { albedoTexture, normalTexture };
    rasm::MaterialHandle material = engine.createMaterial(rasm::MaterialType::PBR, textures);

    // 5. Create entities in the scene
    rasm::EntityHandle bunny = engine.createEntity("bunny", bunnyMesh, material, rasm::Transform{
                                                                                     glm::vec3(0.0f, 0.0f, -10.0f),     // position
                                                                                     glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
                                                                                     glm::vec3(0.05f)                   // scale
        });

    rasm::EntityHandle bunny2 = engine.createEntity("bunny2", bunnyMesh, material, rasm::Transform{
                                                                                       glm::vec3(5.0f, 0.0f, -10.0f),     // position
                                                                                       glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
                                                                                       glm::vec3(0.05f)                   // scale
        });

    rasm::EntityHandle bunny3 = engine.createEntity("bunny3", bunnyMesh, material, rasm::Transform{
                                                                                       glm::vec3(-5.0f, 1.0f, -10.0f),    // position
                                                                                       glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
                                                                                       glm::vec3(0.05f)                   // scale
        });

    // 6. Create a camera
    rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{ .type = rasm::CameraType::PERSPECTIVE,
                                                                           .perspective{ .fovY = 45.0f,
                                                                                        .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight),
                                                                                        .nearZ = 0.1f,
                                                                                        .farZ = 32.0f } },
        rasm::Transform{
            glm::vec3(0.0f, 0.0f, -1.0f),      // position
            glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
            glm::vec3(1.0f)                    // scale
        });

    // 7. Create a light
    rasm::LightHandle light = engine.createLight(rasm::LightType::DIRECTIONAL,  // type
        glm::vec3(1.0f, 1.0f, 1.0f),   // color
        1.0f,                          // intensity
        glm::vec3(0.0f, 10.0f, 0.0f),  // position
        glm::vec3(0.0f, -1.0f, 0.0f)); // direction

    engine.addEntityToScene(scene, bunny);
    engine.addEntityToScene(scene, bunny2);
    engine.addEntityToScene(scene, bunny3);
    engine.addCameraToScene(scene, camera);
    engine.addLightToScene(scene, light);


    // 8. Main loop
    while (engine.running())
    {
        // Update transforms, animations, etc.
        engine.updateTransform(bunny, engine.getTransform(bunny).rotate(glm::vec3(0.0f, 1.0f, 0.0f), glm::radians(0.01f)));
        engine.updateTransform(bunny2, engine.getTransform(bunny2).rotate(glm::vec3(1.0f, 1.0f, 0.0f), glm::radians(0.01f)));
        engine.updateTransform(bunny3, engine.getTransform(bunny3).rotate(glm::vec3(1.0f, 1.0f, 1.0f), glm::radians(0.01f)));

        // Render the scene
        engine.render(scene, camera);
    }

#endif
    return 0;
}
