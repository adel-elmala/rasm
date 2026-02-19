#pragma once

#include <stdint.h>
#include <utility>

namespace rasm {

    class Entity
    {
        private:
            uint32_t handle;
            // Engine* engine; // Back-reference for entity/component management

        public:
            Entity();
            ~Entity();

            template<typename T, typename... Args>
            T& addComponent(Args&&... args) {
                T* component = new T(std::forward<Args>(args)...);
                return *component;
            }

            // template<typename T>
            // T& get();
    };

}
