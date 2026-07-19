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

    // 2. Create a scene
    rasm::Scene scene = engine.createScene();

    // 3. Load resources
    rasm::MeshHandle bunnyMesh = engine.loadMesh("assets/models/bunny.obj");
    rasm::TextureHandle albedoTexture = engine.loadTexture("assets/textures/bunny-atlas.jpg");
    rasm::TextureHandle normalTexture = engine.loadTexture("assets/textures/test1.jpg");

    // 4. Create a material
    rasm::Material material = engine.createMaterial(rasm::MaterialTemplate::PBR);
    material.setTexture(rasm::PbrSlot::ALBEDO, albedoTexture);
    material.setTexture(rasm::PbrSlot::NORMAL, normalTexture);
    material.setFloat(rasm::PbrParam::ROUGHNESS, 0.5f);
    material.setFloat(rasm::PbrParam::METALLIC, 0.0f);

    // 5. Create entities in the scene
    rasm::Entity bunny = scene.createEntity("bunny");
    bunny.addComponent<rasm::Mesh>(bunnyMesh);
    bunny.addComponent<rasm::Material>(material);
    bunny.addComponent<rasm::Transform>(
        glm::vec3(0.0f, 0.0f, -10.0f),     // position
        glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
        glm::vec3(0.05f)                   // scale
    );

    rasm::Entity bunny2 = scene.createEntity("bunny2");
    bunny2.addComponent<rasm::Mesh>(bunnyMesh);
    bunny2.addComponent<rasm::Material>(material);
    bunny2.addComponent<rasm::Transform>(
        glm::vec3(5.0f, 0.0f, -10.0f),     // position
        glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
        glm::vec3(0.05f)                   // scale
    );

    rasm::Entity bunny3 = scene.createEntity("bunny3");
    bunny3.addComponent<rasm::Mesh>(bunnyMesh);
    bunny3.addComponent<rasm::Material>(material);
    bunny3.addComponent<rasm::Transform>(
        glm::vec3(-5.0f, 1.0f, -10.0f),     // position
        glm::quat(1.0f, 0.0f, 0.0f, 0.0f), // rotation
        glm::vec3(0.05f)                   // scale
    );

    // 6. Create a camera
    rasm::Entity camera = scene.createEntity("MainCamera");
    rasm::Camera &cameraLens = camera.addComponent<rasm::Camera>(rasm::CameraType::PERSPECTIVE);
    cameraLens.setPerspective(45.0f, static_cast<float>(config.windowWidth) / static_cast<float>(config.windowHeight), 0.1f, 32.0f);
    camera.addComponent<rasm::Transform>(glm::vec3(0.0f, 0.0f, -1.0f));

    // 7. Create a light
    rasm::Entity light = scene.createEntity("DirectionalLight");
    rasm::Light &sun = light.addComponent<rasm::Light>(rasm::LightType::DIRECTIONAL);
    sun.setColor(glm::vec3(1.0f, 1.0f, 1.0f));
    sun.setIntensity(1.0f);
    light.addComponent<rasm::Transform>().setRotation(glm::angleAxis(glm::radians(-45.0f), glm::vec3(1.0f, 0.0f, 0.0f)));

    // 8. Main loop
    while (engine.running())
    {
        // Update transforms, animations, etc.
        bunny.getComponent<rasm::Transform>().rotate(glm::vec3(0.0f, 1.0f, 0.0f), glm::radians(0.01f));
        bunny2.getComponent<rasm::Transform>().rotate(glm::vec3(1.0f, 1.0f, 0.0f), glm::radians(0.01f));
        bunny3.getComponent<rasm::Transform>().rotate(glm::vec3(1.0f, 1.0f, 1.0f), glm::radians(0.01f));

        // Render the scene
        engine.render(scene, camera);
    }

    return 0;
}
