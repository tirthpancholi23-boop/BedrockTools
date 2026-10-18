#include "minimap.hpp"
#include "modules/ModuleRegistry.hpp"
#include "config/ConfigManager.hpp"
#include <bedrocktools/events/EventBus.hpp>
#include <pl/ModMenu.hpp>
#include <bedrocktools/memory/Signatures.hpp>
#include <bedrocktools/sdk/Memory.hpp>
#include <bedrocktools/sdk/Offsets.hpp>
#include <bedrocktools/sdk/render/Block.hpp>
#include <bedrocktools/sdk/world/Actor.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {
using GetRuntimeActorList_t = std::vector<void*> (*)(void*);
using ActorIsPlayer_t = bool (*)(void*);
struct BlockPos { int x, y, z; };
using GetBlock_t = void* (*)(void* region, const BlockPos& pos);

GetRuntimeActorList_t s_getActors = nullptr;
ActorIsPlayer_t s_isPlayer = nullptr;
GetBlock_t s_getBlock = nullptr;
MiniMapModule* g_mod = nullptr;

constexpr float kBasePxPerBlock = 6.f;
constexpr std::uint32_t kCatMob = 2;      // same bit Hitbox uses for mobs
constexpr std::uint32_t kCatMonster = 4;  // hostile mobs (ActorCategory::Monster) - assumption

constexpr int kTerrainN = 40;             // terrain texture is kTerrainN x kTerrainN
constexpr int kTerrainEveryTicks = 6;
constexpr int kScanUp = 40, kScanDown = 50;
constexpr int kHeadTex = 32;              // player head texture size (multiple of 8)
constexpr const char* kTerrainId = "bedrocktools.minimap.terrain";

std::uint32_t rgba(float a, float r, float g, float b) {
    auto c = [](float v) { return static_cast<std::uint32_t>(std::clamp(v, 0.f, 1.f) * 255.f); };
    return (c(a) << 24) | (c(r) << 16) | (c(g) << 8) | c(b);
}

PLModMenu_DrawCommand rectCmd(float x, float y, float w, float h, std::uint32_t col) {
    PLModMenu_DrawCommand c = {};
    c.type = PL_DRAW_RECT_FILLED;
    c.x = x; c.y = y; c.w = w; c.h = h; c.color = col;
    return c;
}
PLModMenu_DrawCommand dotCmd(float x, float y, float radius, std::uint32_t col) {
    PLModMenu_DrawCommand c = {};
    c.type = PL_DRAW_CIRCLE_FILLED;
    c.x = x; c.y = y; c.size = radius; c.color = col;
    return c;
}
PLModMenu_DrawCommand imageCmd(float x, float y, float w, float h, const std::string& id) {
    PLModMenu_DrawCommand c = {};
    c.type = PL_DRAW_IMAGE;
    c.x = x; c.y = y; c.w = w; c.h = h; c.color = 0xFFFFFFFF; c.imageId = id;
    return c;
}

// ---------------------------------------------------------------- mob heads (8x8 pixel art)
// Mob identity is not exposed by this SDK, so mobs are classified by hitbox size + hostility.
// Heads are therefore approximations (e.g. any 1.7-tall hostile is drawn as a creeper).
enum MobKind { MK_GenericHostile, MK_GenericPassive, MK_Creeper, MK_Zombie, MK_Spider, MK_Enderman,
               MK_Pig, MK_Cow, MK_Sheep, MK_Chicken, MK_Villager, MK_Count };

const char* kMobIds[MK_Count] = {
    "bedrocktools.minimap.mob.hostile", "bedrocktools.minimap.mob.passive", "bedrocktools.minimap.mob.creeper",
    "bedrocktools.minimap.mob.zombie", "bedrocktools.minimap.mob.spider", "bedrocktools.minimap.mob.enderman",
    "bedrocktools.minimap.mob.pig", "bedrocktools.minimap.mob.cow", "bedrocktools.minimap.mob.sheep",
    "bedrocktools.minimap.mob.chicken", "bedrocktools.minimap.mob.villager"};

using Pal = std::initializer_list<std::pair<char, std::uint32_t>>;
void registerHead(int kind, const char* const rows[8], Pal pal) {
    std::array<std::uint8_t, 8 * 8 * 4> px{};
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            std::uint32_t c = 0xFF00FF;
            for (auto& p : pal) if (p.first == rows[y][x]) c = p.second;
            auto* d = px.data() + (y * 8 + x) * 4;
            d[0] = (c >> 16) & 0xFF; d[1] = (c >> 8) & 0xFF; d[2] = c & 0xFF; d[3] = 255;
        }
    pl::modmenu::registerImage(kMobIds[kind], px, 8, 8);
}

void registerMobHeads() {
    { const char* r[8] = {"gGgggGgg","gggGgggg","gKKggKKg","gKKggKKg","gggKKggg","ggKKKKgg","ggKggKgg","gGggggGg"};
      registerHead(MK_Creeper, r, {{'g',0x50A03C},{'G',0x377828},{'K',0x000000}}); }
    { const char* r[8] = {"hhhhhhhh","hhhhhhhh","zzzzzzzz","zEEzzEEz","zzzNNzzz","zzmmmmzz","zzzzzzzz","zzzzzzzz"};
      registerHead(MK_Zombie, r, {{'h',0x4F7A4F},{'z',0x5C8F5C},{'E',0x1E3C78},{'N',0x466E46},{'m',0x3C5F3C}}); }
    { const char* r[8] = {"dddddddd","dddddddd","dRRddRRd","dRRddRRd","dddddddd","dddmmddd","dddmmddd","dddddddd"};
      registerHead(MK_Spider, r, {{'d',0x281E1E},{'R',0xC81E1E},{'m',0x5A4646}}); }
    { const char* r[8] = {"kkkkkkkk","kkkkkkkk","kkkkkkkk","kPPkkPPk","kkkkkkkk","kkkkkkkk","kkkkkkkk","kkkkkkkk"};
      registerHead(MK_Enderman, r, {{'k',0x140F19},{'P',0xDC64FF}}); }
    { const char* r[8] = {"pppppppp","pppppppp","pEppppEp","ppsssspp","ppsNNspp","ppsssspp","pppppppp","pppppppp"};
      registerHead(MK_Pig, r, {{'p',0xF0A0A0},{'E',0x281414},{'s',0xD26E6E},{'N',0x8C3C3C}}); }
    { const char* r[8] = {"cccccccc","cccccccc","cKccccKc","cccccccc","ccmmmmcc","ccmNNmcc","ccmmmmcc","cccccccc"};
      registerHead(MK_Cow, r, {{'c',0x5A3C28},{'K',0x000000},{'m',0xC8AA96},{'N',0x6E5040}}); }
    { const char* r[8] = {"wwwwwwww","wwffffww","wfKffKfw","wffffffw","wffNNffw","wwffffww","wwwwwwww","wwwwwwww"};
      registerHead(MK_Sheep, r, {{'w',0xEBEBEB},{'f',0xBEA082},{'K',0x000000},{'N',0x8C6E55}}); }
    { const char* r[8] = {"wwwwwwww","wwwwwwww","wKwwwwKw","wwwyywww","wwwrrwww","wwwwwwww","wwwwwwww","wwwwwwww"};
      registerHead(MK_Chicken, r, {{'w',0xF0F0F0},{'K',0x000000},{'y',0xFABE3C},{'r',0xC82828}}); }
    { const char* r[8] = {"ssssssss","sBBssBBs","sGsssGss","ssnnnnss","sssnnsss","sssnnsss","ssssssss","ssssssss"};
      registerHead(MK_Villager, r, {{'s',0xC89678},{'B',0x503C28},{'G',0x3C8C3C},{'n',0xAA6E5A}}); }
    { const char* r[8] = {"RRRRRRRR","RKKRRKKR","RKKRRKKR","RRRRRRRR","RRKKKKRR","RRKRRKRR","RRRRRRRR","RRRRRRRR"};
      registerHead(MK_GenericHostile, r, {{'R',0xC83232},{'K',0x1E0A0A}}); }
    { const char* r[8] = {"GGGGGGGG","GKKGGKKG","GKKGGKKG","GGGGGGGG","GGGGGGGG","GKGGGGKG","GGKKKKGG","GGGGGGGG"};
      registerHead(MK_GenericPassive, r, {{'G',0x50C850},{'K',0x0A1E0A}}); }
}

int classifyMob(float w, float h, bool hostile) {
    if (hostile) {
        if (h > 2.6f) return MK_Enderman;
        if (w > 0.6f && h < 1.2f) return MK_Spider;
        if (h >= 1.5f && h < 1.85f && w < 0.7f) return MK_Creeper;
        if (h >= 1.85f && w < 0.8f) return MK_Zombie;
        return MK_GenericHostile;
    }
    if (h >= 1.7f && h <= 2.1f && w < 0.8f) return MK_Villager;
    if (w <= 0.5f && h < 0.9f) return MK_Chicken;
    if (w >= 0.8f && w <= 1.05f) {
        if (h < 1.1f) return MK_Pig;
        return h >= 1.35f ? MK_Cow : MK_Sheep;
    }
    return MK_GenericPassive;
}

// ---------------------------------------------------------------- player skin heads (as in Tablist)
const void* skinImageFromActor(void* actor) {
    using namespace bedrocktools::sdk;
    if (!actor) return nullptr;
    auto skinRef = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(actor) + offsets::Player::mSkin);
    if (!skinRef) return nullptr;
    auto threadOwner = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(skinRef) + offsets::SerializedSkinRef::mSkinImpl);
    if (!threadOwner) return nullptr;
    auto skinImpl = reinterpret_cast<std::uintptr_t>(threadOwner) + offsets::ThreadOwner::mObject;
    auto image = reinterpret_cast<const void*>(skinImpl + offsets::SerializedSkinImpl::mSkinImage);
    if (*reinterpret_cast<const bool*>(skinImpl + offsets::SerializedSkinImpl::mIsPersona)) {
        auto begin = *reinterpret_cast<const std::uintptr_t*>(skinImpl + offsets::SerializedSkinImpl::mSkinAnimatedImages);
        auto end = *reinterpret_cast<const std::uintptr_t*>(skinImpl + offsets::SerializedSkinImpl::mSkinAnimatedImages + sizeof(std::uintptr_t));
        if (begin && end >= begin && end - begin <= offsets::AnimatedImageData::Size * 64) {
            for (auto entry = begin; entry < end; entry += offsets::AnimatedImageData::Size) {
                auto type = *reinterpret_cast<const std::uint32_t*>(entry + offsets::AnimatedImageData::mType);
                if (type == 2 || type == 3) {
                    image = reinterpret_cast<const void*>(entry + offsets::AnimatedImageData::mImage);
                    if (type == 3) break;
                }
            }
        }
    }
    return image;
}

bool extractHead(void* actor, std::array<std::uint8_t, kHeadTex * kHeadTex * 4>& out) {
    using namespace bedrocktools::sdk;
    auto image = skinImageFromActor(actor);
    if (!image) return false;
    auto addr = reinterpret_cast<std::uintptr_t>(image);
    auto width = *reinterpret_cast<const std::uint32_t*>(addr + offsets::SkinImage::mWidth);
    auto height = *reinterpret_cast<const std::uint32_t*>(addr + offsets::SkinImage::mHeight);
    auto pixels = *reinterpret_cast<const std::uint8_t* const*>(addr + offsets::Image::mBytesOffset);
    if (!pixels || width < 64 || width > 256 || height < 32 || height > 256 || width % 64 != 0 || height % 32 != 0)
        return false;

    const int scale = static_cast<int>(width / 64);
    const int up = kHeadTex / 8;
    out.fill(0);
    auto layer = [&](int skinX, bool overlay) {
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x) {
                int sx0 = skinX + x * scale + scale / 2;
                int sy0 = 8 * scale + y * scale + scale / 2;
                const auto* src = pixels + (static_cast<std::size_t>(sy0) * width + sx0) * 4;
                for (int sy = 0; sy < up; ++sy)
                    for (int sx = 0; sx < up; ++sx) {
                        auto* dst = out.data() + ((y * up + sy) * kHeadTex + x * up + sx) * 4;
                        if (!overlay) { std::memcpy(dst, src, 4); continue; }
                        const auto alpha = src[3];
                        if (alpha == 0) continue;
                        if (alpha == 255) { std::memcpy(dst, src, 4); continue; }
                        float a = alpha / 255.f, inv = 1.f - a;
                        for (int k = 0; k < 3; ++k) dst[k] = static_cast<std::uint8_t>(src[k] * a + dst[k] * inv + 0.5f);
                        dst[3] = 255;
                    }
            }
    };
    layer(8 * scale, false);
    layer(40 * scale, true);
    return true;
}

// ---------------------------------------------------------------- terrain colours
struct RGB { std::uint8_t r, g, b; };

// returns {0,0,0} + valid=false for air / see-through blocks that should be skipped
RGB blockColor(const std::string& n, bool& valid) {
    valid = true;
    auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
    const bool isAir = n == "minecraft:air" || n == "air" || has("cave_air") || has("void_air");
    if (isAir || has("tallgrass") || has("short_grass") || has("tall_grass") || has("flower") ||
        has("sapling") || has("fern") || has("vine") || has("torch") || has("rail") || has("sugar_cane") ||
        has("double_plant") || has("bush") || has("seagrass") || has("kelp") || has("web") || has("fire") ||
        has("light_block") || has("barrier") || has("structure_void") || has("carpet") || has("snow_layer") ||
        has("pressure_plate") || has("button") || has("lever") || has("sign") || has("moss_carpet") ||
        has("dead_bush") || has("redstone_wire")) {
        valid = false; return {0, 0, 0};
    }
    if (has("lava")) return {220, 100, 20};
    if (has("water")) return {45, 85, 200};
    if (has("powder_snow") || has("snow")) return {245, 245, 250};
    if (has("ice")) return {150, 190, 240};
    if (has("grass_block") || has("grass")) return {92, 150, 52};
    if (has("podzol")) return {90, 60, 30};
    if (has("mycelium")) return {110, 90, 110};
    if (has("mud")) return {80, 65, 60};
    if (has("dirt") || has("farmland") || has("rooted")) return {125, 90, 55};
    if (has("red_sand")) return {190, 100, 50};
    if (has("sand")) return {218, 206, 150};
    if (has("gravel")) return {130, 125, 120};
    if (has("clay")) return {150, 160, 175};
    if (has("leaves") || has("azalea")) return {40, 110, 35};
    if (has("cactus")) return {40, 120, 60};
    if (has("log") || has("wood") || has("planks") || has("stem")) return {110, 85, 50};
    if (has("deepslate")) return {70, 70, 78};
    if (has("obsidian")) return {30, 20, 50};
    if (has("bedrock")) return {60, 60, 60};
    if (has("netherrack") || has("nether_brick")) return {110, 40, 40};
    if (has("soul")) return {85, 65, 50};
    if (has("basalt") || has("blackstone")) return {55, 50, 55};
    if (has("end_stone")) return {220, 220, 160};
    if (has("terracotta")) return {150, 90, 65};
    if (has("copper")) return {190, 110, 80};
    if (has("diamond")) return {90, 220, 230};
    if (has("gold")) return {240, 200, 60};
    if (has("iron")) return {200, 170, 150};
    if (has("emerald")) return {60, 200, 100};
    if (has("redstone")) return {200, 40, 40};
    if (has("lapis")) return {40, 70, 190};
    if (has("coal")) return {50, 50, 50};
    if (has("glass")) return {190, 220, 230};
    if (has("wool") || has("concrete")) return {215, 215, 215};
    if (has("stone") || has("cobble") || has("andesite") || has("diorite") || has("granite") ||
        has("tuff") || has("calcite") || has("dripstone") || has("brick")) return {125, 125, 125};
    // unknown block: stable colour from its name
    std::uint32_t h = 2166136261u;
    for (char c : n) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    return {static_cast<std::uint8_t>(90 + (h & 0x7F)), static_cast<std::uint8_t>(90 + ((h >> 8) & 0x7F)),
            static_cast<std::uint8_t>(90 + ((h >> 16) & 0x7F))};
}

struct CachedColor { RGB c; bool valid; };
std::unordered_map<void*, CachedColor> s_colorCache;

CachedColor colorOf(void* block) {
    if (!block) return {{0, 0, 0}, false};
    auto it = s_colorCache.find(block);
    if (it != s_colorCache.end()) return it->second;
    CachedColor cc{{0, 0, 0}, false};
    const std::string* name = reinterpret_cast<bedrocktools::sdk::Block*>(block)->fullName();
    if (name && !name->empty() && name->size() < 128) cc.c = blockColor(*name, cc.valid);
    s_colorCache.emplace(block, cc);
    return cc;
}
}  // namespace

MiniMapModule::MiniMapModule()
    : Module("MiniMap", "Mini map with terrain, underground view, player heads, mob heads and waypoints.") {
    g_mod = this;
}

MiniMapModule::~MiniMapModule() {
    if (g_mod == this) g_mod = nullptr;
}

void MiniMapModule::onInit() {
    using bedrocktools::memory::resolve;
    using bedrocktools::memory::SignatureId;
    s_getActors = reinterpret_cast<GetRuntimeActorList_t>(resolve(SignatureId::ActorManagerList));
    s_isPlayer = reinterpret_cast<ActorIsPlayer_t>(resolve(SignatureId::ActorIsPlayer));
    s_getBlock = reinterpret_cast<GetBlock_t>(resolve(SignatureId::BlockSourceGetBlock));
    registerMobHeads();
    bedrocktools::events::bus().subscribe<bedrocktools::events::LocalPlayerTickEvent>([](auto& event) {
        if (g_mod && g_mod->enabled && event.player) g_mod->onLocalPlayerTick(event.player);
    });
}

void MiniMapModule::onEnable() {
    m_tick = 0;
    updateButtons();
}

void MiniMapModule::onDisable() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entities.clear();
    m_hasSelf = false;
    m_terrainReady = false;
    submitDrawCommands(moduleId, {});
    updateButtons();   // enabled is false here -> removes the buttons
}

// Renders the terrain texture: one pixel = one world column, found by scanning down from the top of the slice.
void MiniMapModule::buildTerrain(void* playerPtr, float px, float py, float pz, float yawDeg) {
    auto* player = static_cast<bedrocktools::sdk::Player*>(playerPtr);
    auto* dim = player->dimension();
    void* region = dim ? static_cast<void*>(dim->blockSource()) : nullptr;
    if (!region || !s_getBlock) return;

    const float pxPerBlock = kBasePxPerBlock * m_zoomScale;
    const float yawR = yawDeg * 3.14159265f / 180.f;
    const float cs = std::cos(yawR), sn = std::sin(yawR);
    const int playerY = static_cast<int>(std::floor(py));
    const int sliceTop = m_useFixedY ? m_fixedY : playerY + static_cast<int>(std::lround(m_sliceOffset));
    const int topY = std::clamp(m_undergroundMode ? sliceTop : playerY + kScanUp, -64, 319);
    const int bottomY = m_undergroundMode ? std::max(topY - 50, -64)
                                          : std::max(std::min(playerY - kScanDown, topY - 30), -64);

    std::array<std::uint8_t, kTerrainN * kTerrainN * 4> img{};
    for (int j = 0; j < kTerrainN; ++j) {
        for (int i = 0; i < kTerrainN; ++i) {
            const float u = ((i + 0.5f) / kTerrainN - 0.5f) * m_mapWidth;   // map pixels from centre
            const float v = ((j + 0.5f) / kTerrainN - 0.5f) * m_mapWidth;
            float dx, dz;
            if (m_rotateWithView) {
                const float right = u / pxPerBlock, fwd = -v / pxPerBlock;
                dx = right * -cs + fwd * -sn;
                dz = right * -sn + fwd * cs;
            } else {
                dx = u / pxPerBlock;
                dz = v / pxPerBlock;
            }
            const int bx = static_cast<int>(std::floor(px + dx));
            const int bz = static_cast<int>(std::floor(pz + dz));

            auto* d = img.data() + (j * kTerrainN + i) * 4;
            for (int y = topY; y >= bottomY; --y) {
                auto cc = colorOf(s_getBlock(region, BlockPos{bx, y, bz}));
                if (!cc.valid) continue;
                float shade;
                if (m_undergroundMode) shade = 1.f - std::min(topY - y, 32) / 32.f * 0.55f;   // deeper floors darker
                else shade = 1.f + std::clamp((y - py) / 40.f, -0.25f, 0.25f);                  // higher = brighter
                d[0] = static_cast<std::uint8_t>(std::clamp(cc.c.r * shade, 0.f, 255.f));
                d[1] = static_cast<std::uint8_t>(std::clamp(cc.c.g * shade, 0.f, 255.f));
                d[2] = static_cast<std::uint8_t>(std::clamp(cc.c.b * shade, 0.f, 255.f));
                d[3] = 255;
                break;
            }
        }
    }
    pl::modmenu::registerImage(kTerrainId, img, kTerrainN, kTerrainN);
    m_terrainReady = true;
}

void MiniMapModule::onLocalPlayerTick(void* localPlayer) {
    auto* player = static_cast<bedrocktools::sdk::Player*>(localPlayer);
    const auto pos = player->position();
    const auto rot = player->rotation();
    ++m_tick;

    if (!m_mapOpen) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entities.clear();
        m_selfX = pos.x; m_selfY = pos.y; m_selfZ = pos.z;
        m_yaw = rot.y;
        m_hasSelf = true;
        return;
    }

    if (m_showTerrain && m_tick % kTerrainEveryTicks == 0) buildTerrain(localPlayer, pos.x, pos.y, pos.z, rot.y);

    std::vector<MiniMapEntity> next;
    const float range = (m_mapWidth * 0.5f) / (kBasePxPerBlock * m_zoomScale) * 1.5f;
    const bool refreshHeads = (m_tick % 20) == 0;

    if (s_getActors) {
        auto* level = player->level();
        void* manager = level ? level->actorManager() : nullptr;
        if (manager) {
            for (void* raw : s_getActors(manager)) {
                if (!raw || raw == localPlayer) continue;
                auto* actor = static_cast<bedrocktools::sdk::Actor*>(raw);
                const auto p = actor->position();
                if (std::fabs(p.x - pos.x) > range || std::fabs(p.z - pos.z) > range) continue;

                const auto box = actor->bounds();
                const float w = box.max.x - box.min.x;
                const float h = box.max.y - box.min.y;

                if (s_isPlayer && s_isPlayer(raw)) {
                    MiniMapEntity e{p.x, p.z, 0, 0, h > 1.2f && h < 1.72f, false, {}};  // crouched hitbox is shorter
                    char key[40];
                    std::snprintf(key, sizeof(key), "minimap_%llx", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(raw)));
                    std::array<std::uint8_t, kHeadTex * kHeadTex * 4> head{};
                    if (refreshHeads || m_tick < 40) {
                        if (extractHead(raw, head)) { pl::modmenu::registerImage(key, head, kHeadTex, kHeadTex); e.image = key; }
                    } else {
                        e.image = key;   // registered earlier; falls back to a dot if it never was
                    }
                    next.push_back(std::move(e));
                } else if (actor->hasCategory(kCatMob)) {
                    const bool hostile = actor->hasCategory(kCatMonster);
                    next.push_back({p.x, p.z, 1, classifyMob(w, h, hostile), false, hostile, {}});
                }
            }
        }
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_entities.swap(next);
    m_selfX = pos.x; m_selfY = pos.y; m_selfZ = pos.z;
    m_yaw = rot.y;
    m_hasSelf = true;
}

void MiniMapModule::addWaypointHere() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_hasSelf) return;
    m_waypoints.push_back({m_selfX, m_selfY, m_selfZ});
}

void MiniMapModule::setFixedYFromText(std::string_view text) {
    try {
        m_fixedY = std::clamp(std::stoi(std::string(text)), -64, 319);
    } catch (...) {}   // ignore text that is not a number
}

bool MiniMapModule::onMenuConfigChanged(std::string_view key, std::string_view value) {
    if (key == "m_useFixedYLevel") { setFixedYFromText(value); return true; }
    if (key == "m_addWaypointButton") { addWaypointHere(); return true; }
    if (key == "m_clearWaypointsButton") {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_waypoints.clear();
        return true;
    }
    return false;
}

bool MiniMapModule::showInLegacyMenu(std::string_view key) const { return key != "waypoints" && key != "m_sliceHeight" && key != "m_mapOpen"; }

void MiniMapModule::onFrame() {
    if (!enabled) return;
    if (!m_mapOpen) { submitDrawCommands(moduleId, {}); return; }

    std::vector<MiniMapEntity> ents;
    std::vector<MiniMapWaypoint> wps;
    float sx, sy, sz, yaw;
    bool terrain;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_hasSelf) return;
        ents = m_entities; wps = m_waypoints;
        sx = m_selfX; sy = m_selfY; sz = m_selfZ; yaw = m_yaw;
        terrain = m_terrainReady;
    }

    const float W = m_mapWidth;
    const float x0 = hudPosX, y0 = hudPosY;
    const float cx = x0 + W * 0.5f, cy = y0 + W * 0.5f;
    const float half = W * 0.5f - 4.f;
    const float pxPerBlock = kBasePxPerBlock * m_zoomScale;
    const float dot = 5.f * m_dotScale;

    std::vector<PLModMenu_DrawCommand> cmds;

    cmds.push_back(rectCmd(x0 - 3, y0 - 3, W + 6, W + 6, rgba(m_opacity, m_undergroundMode ? 0.4f : 1.f, m_undergroundMode ? 0.7f : 1.f, 1.f)));
    cmds.push_back(rectCmd(x0, y0, W, W, rgba(m_opacity, 0.04f, 0.06f, 0.04f)));
    if (m_showTerrain && terrain) cmds.push_back(imageCmd(x0, y0, W, W, kTerrainId));

    const float yawR = yaw * 3.14159265f / 180.f;
    const float cs = std::cos(yawR), sn = std::sin(yawR);
    auto project = [&](float dx, float dz, float& px, float& py) {
        if (m_rotateWithView) {
            const float right = dx * -cs + dz * -sn;
            const float fwd = dx * -sn + dz * cs;
            px = right * pxPerBlock;
            py = -fwd * pxPerBlock;
        } else {
            px = dx * pxPerBlock;
            py = dz * pxPerBlock;
        }
    };

    // north marker
    {
        float px, py;
        project(0.f, -1000.f, px, py);
        const float len = std::sqrt(px * px + py * py);
        if (len > 0.f) {
            px = px / len * (half - 12.f);
            py = py / len * (half - 12.f);
            PLModMenu_DrawCommand t = {};
            t.type = PL_DRAW_TEXT;
            t.x = cx + px; t.y = cy + py - 10.f; t.w = 1.f; t.h = 1.f;
            t.color = rgba(1, 1, 0.3f, 0.3f);
            t.size = 22.f;
            t.text = "N";
            cmds.push_back(t);
        }
    }

    // waypoints (clamped to the edge like a compass)
    if (m_showWaypoints) {
        static const std::uint32_t pal[4] = {
            rgba(1, 0.3f, 0.6f, 1.f), rgba(1, 1.f, 0.4f, 0.8f), rgba(1, 0.6f, 1.f, 0.9f), rgba(1, 1.f, 0.8f, 0.3f)};
        for (size_t i = 0; i < wps.size(); ++i) {
            float px, py;
            project(wps[i].x - sx, wps[i].z - sz, px, py);
            const bool inside = std::fabs(px) <= half && std::fabs(py) <= half;
            px = std::clamp(px, -half, half);
            py = std::clamp(py, -half, half);
            cmds.push_back(dotCmd(cx + px, cy + py, (inside ? 1.4f : 1.0f) * dot, rgba(1, 1, 1, 1)));
            cmds.push_back(dotCmd(cx + px, cy + py, (inside ? 1.0f : 0.7f) * dot, pal[i % 4]));
        }
    }

    // entities: player skin heads and mob heads
    for (const auto& e : ents) {
        if (e.kind == 0 && !m_showPlayers) continue;
        if (e.kind == 1 && !m_showMobs) continue;
        float px, py;
        project(e.x - sx, e.z - sz, px, py);
        if (std::fabs(px) > half || std::fabs(py) > half) continue;
        const float ex = cx + px, ey = cy + py;

        if (e.kind == 0) {
            const float s = 9.f * m_dotScale;   // half size of the head square
            const bool crouch = e.sneaking && m_highlightCrouch;
            const float ring = crouch ? 3.f : 1.5f;
            cmds.push_back(rectCmd(ex - s - ring, ey - s - ring, 2 * (s + ring), 2 * (s + ring),
                                   crouch ? rgba(1, 1.f, 0.55f, 0.f) : rgba(1, 1, 1, 1)));
            if (!e.image.empty()) cmds.push_back(imageCmd(ex - s, ey - s, 2 * s, 2 * s, e.image));
            else cmds.push_back(rectCmd(ex - s, ey - s, 2 * s, 2 * s, rgba(1, 1.f, 1.f, 0.f)));
        } else {
            const float s = 6.f * m_dotScale;
            if (e.hostile) cmds.push_back(rectCmd(ex - s - 1.5f, ey - s - 1.5f, 2 * s + 3.f, 2 * s + 3.f, rgba(1, 1.f, 0.15f, 0.15f)));
            cmds.push_back(imageCmd(ex - s, ey - s, 2 * s, 2 * s, kMobIds[e.mob]));
        }
    }

    // you: white dot + arrow showing facing direction
    {
        float fx = 0.f, fy = -1.f;
        if (!m_rotateWithView) { fx = -sn; fy = cs; }
        const float tip = 16.f * m_dotScale, base = 7.f * m_dotScale;
        PLModMenu_DrawCommand tri = {};
        tri.type = PL_DRAW_TRIANGLE_FILLED;
        tri.x = cx + fx * tip;             tri.y = cy + fy * tip;
        tri.w = cx - fy * base - fx * 3.f; tri.h = cy + fx * base - fy * 3.f;
        tri.x3 = cx + fy * base - fx * 3.f; tri.y3 = cy - fx * base - fy * 3.f;
        tri.color = rgba(1, 1, 1, 1);
        cmds.push_back(tri);
        cmds.push_back(dotCmd(cx, cy, 0.9f * dot, rgba(1, 1, 1, 1)));
    }

    // Y scale bar with numbers: bar right of the map, marker = slice height (-50 .. +50 blocks from you).
    // Numbers are real Y levels at your current position. Buffers live until submitDrawCommands below.
    const int pY = static_cast<int>(std::floor(sy));
    const int effTopY = m_useFixedY ? m_fixedY : pY + static_cast<int>(std::lround(m_sliceOffset));
    char yLbl[6][16];
    if (m_undergroundMode) {
        const int off = effTopY - pY;
        const float bx = x0 + W + 10.f;
        cmds.push_back(rectCmd(bx, y0, 8.f, W, rgba(0.8f, 0.1f, 0.1f, 0.15f)));
        cmds.push_back(rectCmd(bx - 4.f, y0 + W * 0.5f - 1.f, 16.f, 2.f, rgba(1, 1, 1, 1)));      // your height
        const float t = std::clamp((static_cast<float>(off) + 50.f) / 100.f, 0.f, 1.f);
        const float my = y0 + W * (1.f - t);
        cmds.push_back(rectCmd(bx - 6.f, my - 4.f, 20.f, 8.f, rgba(1, 0.3f, 0.9f, 1.f)));         // slice marker

        auto numText = [&](char* buf, int value, float cxText, float cyText, float size, std::uint32_t col) {
            std::snprintf(buf, 16, "%d", value);
            PLModMenu_DrawCommand tc = {};
            tc.type = PL_DRAW_TEXT;
            tc.x = cxText + static_cast<float>(std::strlen(buf)) * size * 0.28f;   // approx. left-align
            tc.y = cyText - size * 0.5f;
            tc.w = 1.f; tc.h = 1.f;
            tc.color = col; tc.size = size; tc.text = buf;
            cmds.push_back(tc);
        };
        numText(yLbl[0], pY + 50, bx + 16.f, y0 + 4.f, 16.f, rgba(0.9f, 1, 1, 1));            // top of scale
        numText(yLbl[1], pY, bx + 16.f, y0 + W * 0.5f, 16.f, rgba(0.9f, 1, 1, 1));            // you
        numText(yLbl[2], pY - 50, bx + 16.f, y0 + W - 4.f, 16.f, rgba(0.9f, 1, 1, 1));        // bottom of scale
        numText(yLbl[3], pY + 25, bx + 16.f, y0 + W * 0.25f, 14.f, rgba(0.6f, 1, 1, 1));      // quarter ticks
        numText(yLbl[5], pY - 25, bx + 16.f, y0 + W * 0.75f, 14.f, rgba(0.6f, 1, 1, 1));
        numText(yLbl[4], pY + off, bx + 70.f, my, 20.f, rgba(1, 1, 0.5f, 1.f));               // slice level (pink marker)
    }

    // Y level / slice level under the map
    char label[96];
    if (m_showCoords) {
        const int playerY = pY;
        if (m_undergroundMode)
            std::snprintf(label, sizeof(label), m_useFixedY ? "CAVE  fixed Y %d  (you %d)  WP %d" : "CAVE  slice Y %d  (you %d)  WP %d",
                          effTopY, playerY, static_cast<int>(wps.size()));
        else
            std::snprintf(label, sizeof(label), "Y %d   X %d  Z %d   WP %d", playerY, static_cast<int>(std::floor(sx)),
                          static_cast<int>(std::floor(sz)), static_cast<int>(wps.size()));
        PLModMenu_DrawCommand t = {};
        t.type = PL_DRAW_TEXT;
        t.x = cx; t.y = y0 + W + 8.f; t.w = 1.f; t.h = 1.f;
        t.color = rgba(1, 1, 1, 1);
        t.size = 20.f;
        t.text = label;
        cmds.push_back(t);
    }

    submitDrawCommands(moduleId, cmds);
}

void MiniMapModule::loadConfig(const nlohmann::json& j) {
    Module::loadConfig(j);
    auto f = [&](const char* k, float& v) { if (j.contains(k) && j[k].is_number()) v = j[k].get<float>(); };
    auto b = [&](const char* k, bool& v) { if (j.contains(k) && j[k].is_boolean()) v = j[k].get<bool>(); };
    f("hudPosX", hudPosX); f("hudPosY", hudPosY);
    b("isHudModule", isHudModule);
    f("m_mapWidth", m_mapWidth); f("m_zoomScale", m_zoomScale); f("m_dotScale", m_dotScale);
    f("m_opacity", m_opacity);
    if (j.contains("m_sliceHeight") && j["m_sliceHeight"].is_number()) m_sliceOffset = j["m_sliceHeight"].get<float>() - 50.f;
    b("m_rotateWithView", m_rotateWithView); b("m_showTerrain", m_showTerrain);
    b("m_undergroundMode", m_undergroundMode); b("m_showPlayers", m_showPlayers);
    b("m_showMobs", m_showMobs); b("m_showWaypoints", m_showWaypoints);
    b("m_highlightCrouch", m_highlightCrouch); b("m_showCoords", m_showCoords);
    b("m_showButtons", m_showButtons); b("m_mapOpen", m_mapOpen); b("m_useFixedY", m_useFixedY);
    if (j.contains("m_useFixedYLevel") && j["m_useFixedYLevel"].is_string()) setFixedYFromText(j["m_useFixedYLevel"].get<std::string>());
    if (j.contains("waypoints") && j["waypoints"].is_array()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_waypoints.clear();
        for (const auto& w : j["waypoints"])
            if (w.is_array() && w.size() == 3)
                m_waypoints.push_back({w[0].get<float>(), w[1].get<float>(), w[2].get<float>()});
    }
}

void MiniMapModule::saveConfig(nlohmann::json& j) {
    Module::saveConfig(j);
    j["hudPosX"] = hudPosX; j["hudPosY"] = hudPosY;
    j["isHudModule"] = isHudModule;
    j["m_mapWidth"] = m_mapWidth; j["m_zoomScale"] = m_zoomScale; j["m_dotScale"] = m_dotScale;
    j["m_opacity"] = m_opacity;
    j["m_sliceHeight"] = m_sliceOffset + 50.f;   // shifted so the stored value is never negative
    j["m_rotateWithView"] = m_rotateWithView; j["m_showTerrain"] = m_showTerrain;
    j["m_undergroundMode"] = m_undergroundMode; j["m_showPlayers"] = m_showPlayers;
    j["m_showMobs"] = m_showMobs; j["m_showWaypoints"] = m_showWaypoints;
    j["m_highlightCrouch"] = m_highlightCrouch; j["m_showCoords"] = m_showCoords;
    j["m_showButtons"] = m_showButtons; j["m_mapOpen"] = m_mapOpen; j["m_useFixedY"] = m_useFixedY;
    j["m_useFixedYLevel"] = std::to_string(m_fixedY);   // text box: type an exact Y level
    j["m_addWaypointButton"] = false;
    j["m_clearWaypointsButton"] = false;
    nlohmann::json arr = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& w : m_waypoints) arr.push_back({w.x, w.y, w.z});
    }
    j["waypoints"] = arr;
}

// ---------------------------------------------------------------- on-screen touch buttons
namespace {
std::string iconSvg(const char* path) {
    return std::string("<svg viewBox=\"0 0 64 64\" xmlns=\"http://www.w3.org/2000/svg\">"
        "<path fill=\"#C6C6C6\" stroke=\"#373737\" stroke-width=\"2\" d=\"M2,2 L62,2 L62,62 L2,62 Z M4,4 L60,4 L60,60 L4,60 Z\"/>"
        "<path fill=\"#8B8B8B\" stroke=\"#5B5B5B\" stroke-width=\"2\" d=\"M6,6 L58,6 L58,58 L6,58 Z M8,8 L56,8 L56,56 L8,56 Z\"/>"
        "<path fill=\"#373737\" fill-rule=\"evenodd\" d=\"") + path + "\"/></svg>";
}

const char* kMagnifier = "M28,14 A14,14 0 1,1 28,42 A14,14 0 1,1 28,14 Z M28,20 A8,8 0 1,0 28,36 A8,8 0 1,0 28,20 Z M38,34 L52,48 L48,52 L34,38 Z";

struct BtnDef { const char* id; const char* label; std::string svg; };
const std::vector<BtnDef>& buttonDefs() {
    static const std::vector<BtnDef> defs = {
        {"bedrocktools.MiniMap.Button.Map", "Map open/close",
         iconSvg("M10,14 L24,10 L40,14 L54,10 V50 L40,54 L24,50 L10,54 Z M24,10 V50 M40,14 V54")},
        {"bedrocktools.MiniMap.Button.Waypoint", "Waypoint",
         iconSvg("M32,10 C22,10 16,17 16,25 C16,37 32,54 32,54 C32,54 48,37 48,25 C48,17 42,10 32,10 Z M32,19 A6,6 0 1,1 31.9,19 Z")},
        {"bedrocktools.MiniMap.Button.YUp", "Y +",
         iconSvg("M8,16 L16,16 L22,28 L28,16 L36,16 L26,34 L26,50 L18,50 L18,34 Z M46,12 L54,24 L49,24 L49,50 L43,50 L43,24 L38,24 Z")},
        {"bedrocktools.MiniMap.Button.YDown", "Y -",
         iconSvg("M8,16 L16,16 L22,28 L28,16 L36,16 L26,34 L26,50 L18,50 L18,34 Z M46,52 L54,40 L49,40 L49,14 L43,14 L43,40 L38,40 Z")},
        {"bedrocktools.MiniMap.Button.Cave", "Cave view",
         iconSvg("M10,14 H54 V26 H10 Z M10,38 H54 V50 H10 Z M26,26 H38 V38 H26 Z")},
        {"bedrocktools.MiniMap.Button.ZoomIn", "Map zoom +",
         iconSvg((std::string(kMagnifier) + " M27,24 H29 V27 H32 V29 H29 V32 H27 V29 H24 V27 H27 Z").c_str())},
        {"bedrocktools.MiniMap.Button.ZoomOut", "Map zoom -",
         iconSvg((std::string(kMagnifier) + " M24,27 H32 V29 H24 Z").c_str())},
    };
    return defs;
}
}  // namespace

void MiniMapModule::saveNow() { bedrocktools::config::ConfigManager::get().save(); }

void MiniMapModule::updateButtons() {
    const bool wantMain = enabled && m_showButtons;
    for (const auto& d : buttonDefs()) {
        const std::string id = d.id;
        const bool isMapBtn = id.find("Button.Map") != std::string::npos;
        const bool want = wantMain && (isMapBtn || m_mapOpen);   // action buttons only while the map is open
        if (!want) { pl::modmenu::unregisterButton(d.id); continue; }
        pl::modmenu::ButtonBuilder(d.id, d.label)
            .moduleId(moduleId.c_str())
            .behavior(pl::modmenu::ButtonBehavior::Toggle)
            .stylePreset(pl::modmenu::ButtonStylePreset::Accent)
            .styleColors(0x00000001, 0x00000001, 0x00000001)
            .svgIcon(d.svg.c_str())
            .activeSvgIcon(d.svg.c_str())
            .onEvent([this, id](std::string_view, pl::modmenu::ButtonEvent event, float) {
                if (event != pl::modmenu::ButtonEvent::StateChanged) return;
                // Each button is a Toggle used as a tap: every state change performs its action once.
                if (id.find("Button.Map") != std::string::npos) { m_mapOpen = !m_mapOpen; updateButtons(); }
                else if (id.find("Waypoint") != std::string::npos) addWaypointHere();
                else if (id.find("YUp") != std::string::npos) { if (m_useFixedY) m_fixedY = std::min(m_fixedY + 2, 319); else m_sliceOffset = std::min(m_sliceOffset + 2.f, 50.f); }
                else if (id.find("YDown") != std::string::npos) { if (m_useFixedY) m_fixedY = std::max(m_fixedY - 2, -64); else m_sliceOffset = std::max(m_sliceOffset - 2.f, -50.f); }
                else if (id.find("Cave") != std::string::npos) m_undergroundMode = !m_undergroundMode;
                else if (id.find("ZoomIn") != std::string::npos) m_zoomScale = std::min(m_zoomScale * 1.25f, 4.f);
                else if (id.find("ZoomOut") != std::string::npos) m_zoomScale = std::max(m_zoomScale / 1.25f, 0.3f);
                saveNow();
            })
            .registerButton();
    }
}
