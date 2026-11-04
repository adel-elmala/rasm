#include "importMeshText.h"

#include "doctest/doctest.h"

TEST_CASE("an OBJ can mix faces with and without normals and texture coordinates")
{
    CHECK(importMeshText("v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvn 0 0 1\nf 1/1/1 2/1/1 3/1/1\nf 1 2 3\n", ".obj").loaded);
}

TEST_CASE("an OBJ can name normals and texture coordinates it never defines")
{
    CHECK(importMeshText("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1/1/1 2/2/2 3/3/3\n", ".obj").loaded);
}
