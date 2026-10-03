#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/rasm.h"

int main()
{
    // Create engine instance
    rasm::EngineConfig config = {
        .appName = "example_2",
        .windowWidth = 720,
        .windowHeight = 480,
        .enableValidation = true,
        .preferredBackend = rasm::Backend::VULKAN

    };

    rasm::Engine engine(config);

    // Create a scene
    rasm::SceneHandle scene = engine.createScene();

    // Load resources
    rasm::MeshHandle potteryGLTF = engine.loadGLTF("assets/models/gltf/ancient_egyptian_pottery/scene.gltf");

    if (!potteryGLTF.isValid())
    {
        return 1;
    }

    // Create entities in the scene
    rasm::EntityHandle pottery = engine.createEntity("pottery", potteryGLTF, {}, rasm::Transform{
                                                                             glm::vec3(0.0f, 0.0f, -10.0f),     // position
                                                                             glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
                                                                             glm::vec3(0.005f)                  // scale
                                                                         });

    rasm::EntityHandle pottery2 = engine.createEntity("pottery2", potteryGLTF, {}, rasm::Transform{
                                                                               glm::vec3(5.0f, 0.0f, -10.0f),     // position
                                                                               glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
                                                                               glm::vec3(0.005f)                  // scale
                                                                           });

    rasm::EntityHandle pottery3 = engine.createEntity("pottery3", potteryGLTF, {}, rasm::Transform{
                                                                               glm::vec3(-5.0f, 1.0f, -10.0f),    // position
                                                                               glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
                                                                               glm::vec3(0.005f)                  // scale
                                                                           });

    // Create a camera
    rasm::CameraHandle camera = engine.createCamera(rasm::CameraProjection{.type = rasm::CameraType::PERSPECTIVE,
                                                                           .perspective{.fovY = 45.0f,
                                                                                        .aspect = static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight),
                                                                                        .nearZ = 0.1f,
                                                                                        .farZ = 32.0f}},
                                                    rasm::Transform{
                                                        glm::vec3(0.0f, 0.0f, -1.0f),      // position
                                                        glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
                                                        glm::vec3(1.0f)                    // scale
                                                    });

    // Create a light
    rasm::LightHandle light = engine.createLight(rasm::LightType::DIRECTIONAL,  // type
                                                 glm::vec3(1.0f, 1.0f, 1.0f),   // color
                                                 1.0f,                          // intensity
                                                 glm::vec3(0.0f, 10.0f, 0.0f),  // position
                                                 glm::vec3(0.0f, -1.0f, 0.0f)); // direction

    engine.addEntityToScene(scene, pottery);
    engine.addEntityToScene(scene, pottery2);
    engine.addEntityToScene(scene, pottery3);
    engine.addCameraToScene(scene, camera);
    engine.addLightToScene(scene, light);

    // Main loop
    while (engine.running())
    {
        // Update transforms, animations, etc.
        engine.updateTransform(pottery, engine.getTransform(pottery).rotate(glm::vec3(0.0f, 1.0f, 0.0f), glm::radians(0.01f)));
        engine.updateTransform(pottery2, engine.getTransform(pottery2).rotate(glm::vec3(1.0f, 1.0f, 0.0f), glm::radians(0.01f)));
        engine.updateTransform(pottery3, engine.getTransform(pottery3).rotate(glm::vec3(1.0f, 1.0f, 1.0f), glm::radians(0.01f)));

        // Render the scene
        engine.render2(scene, camera);
    }

    return 0;
}
