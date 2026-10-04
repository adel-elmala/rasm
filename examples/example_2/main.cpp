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
    if (!engine.running())
        return 1;

    // Create a scene
    rasm::SceneHandle scene = engine.createScene();

    // Load resources
    rasm::MeshHandle potteryGLTF = engine.loadGLTF("assets/models/gltf/ancient_egyptian_pottery/scene.gltf");
    rasm::MeshHandle characterGLTF = engine.loadGLTF("assets/models/gltf/cartoon-character/source/model.glb");
    rasm::MeshHandle roomGLTF = engine.loadGLTF("assets/models/gltf/coffee-room/scene.gltf");

    if (!potteryGLTF.isValid() || !characterGLTF.isValid() || !roomGLTF.isValid())
    {
        return 1;
    }

    // Create entities in the scene
    rasm::EntityHandle pottery = engine.createEntity("pottery", potteryGLTF, {}, rasm::Transform{
                                                                                     glm::vec3(0.0f, 0.0f, -10.0f),     // position
                                                                                     glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
                                                                                     glm::vec3(0.002f)                  // scale
                                                                                 });

    rasm::EntityHandle character = engine.createEntity("character", characterGLTF, {}, rasm::Transform{
                                                                                           glm::vec3(3.0f, 0.0f, -10.0f),                                           // position
                                                                                           glm::angleAxis(glm::radians(90.0f * 3.0f), glm::vec3(0.0f, 1.0f, 0.0f)), // rotation
                                                                                           glm::vec3(2.0f)                                                          // scale
                                                                                       });

    rasm::EntityHandle room = engine.createEntity("room", roomGLTF, {}, rasm::Transform{
                                                                            glm::vec3(-5.0f, 0.0f, -10.0f),    // position
                                                                            glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
                                                                            glm::vec3(2.0f)                    // scale
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
    engine.addEntityToScene(scene, room);
    engine.addEntityToScene(scene, character);
    engine.addCameraToScene(scene, camera);
    engine.addLightToScene(scene, light);

    // Main loop
    while (engine.running())
    {
        // Update transforms, animations, etc.
        engine.updateTransform(pottery, engine.getTransform(pottery).rotate(glm::vec3(0.0f, 1.0f, 0.0f), glm::radians(0.01f)));
        engine.updateTransform(character, engine.getTransform(character).rotate(glm::vec3(0.0f, 1.0f, 0.0f), glm::radians(0.01f)));
        engine.updateTransform(room, engine.getTransform(room).rotate(glm::vec3(0.0f, 1.0f, 0.0f), glm::radians(0.01f)));

        // Render the scene
        engine.render2(scene, camera);
    }

    return 0;
}
