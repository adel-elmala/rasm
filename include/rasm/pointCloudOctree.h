// clang-format off
#pragma once

#include "rasm/pointCloud.h"

#include "glm/glm.hpp"

#include <cstdint>
#include <vector>

namespace rasm
{

    constexpr uint32_t  POINT_NODE_NONE         = UINT32_MAX;

    struct PointCloudOctreeDesc
    {
        const glm::dvec3*   positions       = nullptr;
        const uint32_t*     colors          = nullptr;      // as PointCloudChunkDesc::colors; null: none
        const float*        scalars         = nullptr;      // null: none
        uint32_t            count           = 0;
        uint32_t            cells           = 128;          // a node keeps a point of each cell of a cells^3 grid over its cube (a power of two)
        uint32_t            maxNodePoints   = 16384;        // a node with no more points left is a leaf with them all; up to MAX_POINT_CHUNK_POINTS
    };

    struct PointCloudNode
    {
        glm::dvec3          center          = glm::dvec3(0.0);  // its cube's centre: its points are offsets from it, and its entity's position
        double              halfSize        = 0.0;
        glm::dvec3          boundsCenter    = glm::dvec3(0.0);  // a sphere around its points and its children's, inside its parent's
        double              boundsRadius    = 0.0;
        float               spacing         = 0.0f;             // between its points (measurePointSpacing): PointCloudChunkDesc::spacing
        float               geometricError  = 0.0f;             // its spacing, 0 for a leaf: TileDesc::geometricError
        uint32_t            parent          = POINT_NODE_NONE;
        uint32_t            children[8]     = {POINT_NODE_NONE, POINT_NODE_NONE, POINT_NODE_NONE, POINT_NODE_NONE, POINT_NODE_NONE, POINT_NODE_NONE, POINT_NODE_NONE, POINT_NODE_NONE};
        uint32_t            firstPoint      = 0;
        uint32_t            pointCount      = 0;
        uint32_t            level           = 0;
    };

    struct PointCloudOctree
    {
        std::vector<PointCloudNode>     nodes;              // the root first, each node before its children
        std::vector<glm::vec3>          positions;          // node by node, offsets from the node's centre
        std::vector<uint32_t>           colors;             // empty without
        std::vector<float>              scalars;            // empty without
        std::vector<uint32_t>           indices;            // each point's index in the desc
    };

    bool                    buildPointCloudOctree(const PointCloudOctreeDesc& desc, PointCloudOctree& octree);
    PointCloudChunkDesc     getPointCloudNodeChunk(const PointCloudOctree& octree, uint32_t node);

}
// clang-format on
