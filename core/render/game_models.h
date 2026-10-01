#pragma once
#include <string>
#include <vector>

#include "../vrmath.h"

// Loads object models and their textures from the player's own game files
// (RES\obj.crf, a zip archive) at runtime: nothing from the game is shipped.
// Models are the engine's LGMD object format (.bin, versions 3 and 4, single
// sub-object); textures are GIFs (decoded with GDI+).
namespace game_models {

struct Texture {
    unsigned w = 0, h = 0;
    std::vector<unsigned> pixels;  // A8R8G8B8; transparent where the GIF is
};

struct Triangle {
    Vec3 p[3];         // model space (feet, the model's own axes)
    float uv[3][2];
    int material;      // index into Model::textures
};

struct Model {
    std::string name;
    std::vector<Triangle> triangles;
    std::vector<Texture> textures;  // per material (a 1x1 grey if it couldn't be loaded)
    Vec3 bbox_min, bbox_max;
};

// Loads e.g. "sword.bin" from RES\obj.crf next to the game's exe. Textures are
// looked up by the materials' names in the archive's txt16 / txt folders.
bool Load(const char* name, Model& out);

// Decodes an image file in memory (PNG, GIF, ...) into A8R8G8B8 pixels.
bool DecodeImage(const void* data, size_t size, Texture& out);

// The texture coordinates of a creature mesh's vertices (LGMM, e.g. the
// first-person arms "ARMSW2.BIN" / "BJACHAND.BIN") from RES\mesh.crf.
struct Uv {
    float u, v;
};
bool LoadMeshUvs(const char* name, std::vector<Uv>& out);

}  // namespace game_models
