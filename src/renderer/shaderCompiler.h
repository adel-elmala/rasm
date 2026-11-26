// clang-format off
#pragma once

#include "core/cacheFile.h"
#include "rasm/types.h"

#include <slang.h>
#include <slang-com-ptr.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace rasm
{
    enum class FieldType
    {
        FLOAT,
        INT,
        UINT,
        UINT64,
        POINTER,
        OTHER,
    };

    // A field of a GPU struct as the shaders lay it out: a nested struct's and an array's fields are flattened into
    // vectors, matrices and scalars, named like "material.normal.scale" or "previousWorld[2]".
    struct StructField
    {
        std::string             name;
        std::string             typeName;
        uint32_t                offset          = 0;
        uint32_t                size            = 0;
        FieldType               type            = FieldType::OTHER;
        uint32_t                count           = 1;    // scalars: a vector's or a matrix's
        uint32_t                columns         = 1;    // a vector's scalars, a matrix's columns; a matrix is stored column by column
    };

    struct StructMember
    {
        std::string             name;
        uint32_t                offset          = 0;
        std::string             structName;
    };

    struct StructLayout
    {
        uint32_t                    size        = 0;
        std::vector<StructMember>   members;
        std::vector<StructField>    fields;
    };

    using StructLayouts = std::unordered_map<std::string, StructLayout>;

    struct CachedShader
    {
        std::vector<uint8_t>    code;
        std::string             pushConstants;
        StructLayouts           layouts;
    };

    class WorkerPool;

    class ShaderCompiler
    {
    public:
        enum class Target
        {
            SPIRV,
            MSL,
            HLSL,
            GLSL,
            WGSL
        };

        struct CompiledShader
        {
            std::vector<uint8_t>    code;
            bool                    pending         = false;
        };

        struct Stats
        {
            uint32_t            cacheHits       = 0;
            uint32_t            compiles        = 0;
            uint32_t            mainCompiles    = 0;
            uint32_t            workerSessions  = 0;
            double              compileSeconds  = 0.0;
        };

        struct Request
        {
            std::string         source;
            std::string         outputPath;
            std::string         entryPoint;
        };

                        ShaderCompiler();
                        ~ShaderCompiler();

        bool            initialize(Target target, const std::string& shaderDir, const std::string& cacheDir, uint32_t workerThreads = 0);
        void            shutdown();

        CompiledShader  compile(const std::string& sourcePath, const std::string& outputPath, const std::string& entryPointName);
        CompiledShader  compileFromString(const std::string& sourceCode, const std::string& outputPath, const std::string& entryPointName, bool wait = true);
        void            prefetch(const Request* requests, uint32_t count, bool first);
        void            waitForPrefetches();
        // The layout of a struct some compiled program reached, or nullptr.
        const std::vector<StructField>* findStructLayout(const std::string& name) const;
        Stats           getStats() const;

    private:
        enum class PrefetchState
        {
            QUEUED,
            RUNNING,
            DONE,
            FAILED,
        };

        struct Prefetch
        {
            PrefetchState       state           = PrefetchState::QUEUED;
            CacheKey            key;
            CacheKey            sourceKey;
            Request             request;
            CachedShader        shader;
        };

        struct LoadedModule
        {
            Slang::ComPtr<slang::IModule>                   module;
            std::string                                     path;
        };

        struct SlangContext
        {
            Slang::ComPtr<slang::IGlobalSession>            globalSession;
            Slang::ComPtr<slang::ISession>                  session;
            std::unordered_map<std::string, LoadedModule>   modules;
        };

        bool            createSlangContext(SlangContext& context);
        CacheKey        getKey(const std::string& sourceCode, const std::string& outputPath, const std::string& entryPointName) const;
        bool            readCachedEntry(const CacheKey& key, CachedShader& shader);
        bool            compileEntry(SlangContext& context, const CacheKey& key, const std::string& sourceCode, const std::string& outputPath, const std::string& entryPointName,
                                     CachedShader& shader, bool onWorker);
        bool            compileWithSlang(SlangContext& context, const std::string& sourceCode, const std::string& outputPath, const std::string& entryPointName, CompiledShader& shader,
                                         std::string& pushConstants, StructLayouts& layouts, bool& cacheable);
        CompiledShader  useShader(const CachedShader& shader, const std::string& outputPath);
        void            runPrefetch();
        void            endPrefetch(std::unique_lock<std::mutex>& lock, Prefetch& entry, bool compiled, bool onWorker);
        void            releaseIdleContexts();

    private:
        Target                                      target          = Target::SPIRV;
        SlangContext                                mainContext;
        std::string                                 buildPath       = "./build/shaders/";
        std::string                                 searchPath;
        std::filesystem::path                       libDir;
        std::filesystem::path                       cacheFolder;
        CacheKey                                    baseKey;
        StructLayouts                               structLayouts;
        mutable std::mutex                          mutex;
        std::condition_variable                     prefetchDone;
        Stats                                       stats;
        std::unordered_map<std::string, Prefetch>   prefetches;
        std::deque<std::string>                     queue;
        uint32_t                                    openPrefetches  = 0;
        bool                                        stopped         = false;
        std::vector<std::unique_ptr<SlangContext>>  idleContexts;
        std::unique_ptr<WorkerPool>                 workers;        // last: it joins its threads before the members they use go
    };

}
// clang-format on
