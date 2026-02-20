#include "glm/glm.hpp"
#include "glm/gtc/quaternion.hpp"

#include "rasm/core/engine.h"
#include "rasm/core/material.h"
#include "rasm/core/mesh.h"
#include "rasm/core/math.h"
#include "rasm/core/camera.h"
#include "rasm/core/light.h"

int main() {
    // 1. Create engine instance
    rasm::EngineConfig config;
    config.appName = "My Game";
    config.windowWidth = 1920;
    config.windowHeight = 1080;
    config.enableValidation = true;

    rasm::Engine engine(config);

    // 2. Create a scene
    rasm::Scene scene = engine.createScene();

    // 3. Load resources
    rasm::MeshHandle cubeMesh = engine.loadMesh("assets/cube.obj");
    rasm::TextureHandle albedoTexture = engine.loadTexture("assets/brick_diffuse.png");
    rasm::TextureHandle normalTexture = engine.loadTexture("assets/brick_normal.png");

    // 4. Create a material
    rasm::Material material = engine.createMaterial(rasm::MaterialTemplate::PBR);
    material.setTexture(rasm::PbrSlot::Albedo, albedoTexture);
    material.setTexture(rasm::PbrSlot::Normal, normalTexture);
    material.setFloat(rasm::PbrParam::Roughness, 0.5f);
    material.setFloat(rasm::PbrParam::Metallic, 0.0f);

    // 5. Create entities in the scene
    rasm::Entity cube = scene.createEntity("Cube");
    cube.addComponent<rasm::Mesh>(cubeMesh);
    cube.addComponent<rasm::Material>(material);
    cube.addComponent<rasm::Transform>(
        glm::vec3(0.0f, 0.0f, 0.0f),  // position
        glm::quat(),                  // rotation
        glm::vec3(1.0f)               // scale
    );

    // 6. Create a camera
    rasm::Entity camera = scene.createEntity("MainCamera");
    rasm::Camera& cameraLens = camera.addComponent<rasm::Camera>(rasm::CameraType::Perspective);
    cameraLens.setPerspective(45.0f, 16.0f / 9.0f, 0.1f, 1000.0f);
    camera.addComponent<rasm::Transform>(glm::vec3(0.0f, 0.0f, 5.0f));

    // 7. Create a light
    rasm::Entity light = scene.createEntity("DirectionalLight");
    rasm::Light& sun = light.addComponent<rasm::Light>(rasm::LightType::Directional);
    sun.setColor(glm::vec3(1.0f, 1.0f, 1.0f));
    sun.setIntensity(1.0f);
    light.addComponent<rasm::Transform>().setRotation(glm::angleAxis(glm::radians(-45.0f), glm::vec3(1.0f, 0.0f, 0.0f)));

    // 8. Main loop
    while (engine.running()) {
        engine.beginFrame();

        // Update transforms, animations, etc.
        cube.getComponent<rasm::Transform>().rotate(glm::vec3(0.0f, 1.0f, 0.0f), 0.01f);

        // Render the scene
        engine.render(scene, camera);
        engine.endFrame();
    }

    printf("Hello, Rasm!\n");
    return 0;
}
