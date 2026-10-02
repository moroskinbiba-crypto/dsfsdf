#include "explorer.hpp"
#include "switch/dmntcht.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace ex {
namespace {

State g{};
DmntCheatProcessMetadata g_meta{};
bool g_dmntInitialized = false;

// Targeted scan: the user supplies one coordinate triplet shown by TOTK's HUD.
// The scan then finds float triples near that value and uses movement/jump
// captures to eliminate false positives. This avoids the unreliable "scan every
// plausible float triple" approach.
constexpr u64 SCAN_CHUNK = 0x40000; // 256 KiB
constexpr std::size_t MAX_CANDIDATES = 1024;
constexpr float MAX_XZ = 12000.0f;
constexpr float MAX_Y = 6000.0f;
constexpr float MOVE_EPS = 0.75f;
constexpr float JUMP_EPS = 2.0f;
constexpr float MAX_STEP = 1500.0f;

const char* profilePath() { return "sdmc:/switch/totk_explorer/profile.txt"; }
const char* calibrationPath() { return "sdmc:/switch/totk_explorer/calibration.txt"; }

bool finiteCoord(float v) { return std::isfinite(v); }

bool plausible(Vec3 p) {
    return finiteCoord(p.x) && finiteCoord(p.y) && finiteCoord(p.z) &&
           std::fabs(p.x) <= MAX_XZ && std::fabs(p.z) <= MAX_XZ &&
           std::fabs(p.y) <= MAX_Y;
}

bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

void fail(const char* message) {
    g.stage = ScanStage::Failed;
    g.error = message;
    g.message = message;
}

bool findTargetProcess() {
    if (R_FAILED(dmntchtGetCheatProcessMetadata(&g_meta))) return false;
    if (g_meta.title_id != TITLE_ID) return false;
    if (g_meta.process_id == 0 || g_meta.heap_extents.base == 0 || g_meta.heap_extents.size < 0x1000)
        return false;
    g.processId = g_meta.process_id;
    g.heapBase = g_meta.heap_extents.base;
    g.heapSize = g_meta.heap_extents.size;
    return true;
}

bool readVec3(u64 address, Vec3& out) {
    if (R_FAILED(dmntchtReadCheatProcessMemory(address, &out, sizeof(out)))) return false;
    return plausible(out);
}

bool readCalibrationFile(Calibration& calibration) {
    FILE* file = std::fopen(calibrationPath(), "rb");
    if (!file) return false;

    float x = 0, y = 0, z = 0, tolerance = 1.0f;
    int count = std::fscanf(file, "%f %f %f %f", &x, &y, &z, &tolerance);
    std::fclose(file);

    if (count < 3 || !plausible({x, y, z})) return false;
    if (count < 4 || tolerance <= 0.0f || tolerance > 10.0f) tolerance = 1.0f;
    calibration = Calibration{true, {x, y, z}, tolerance};
    return true;
}

bool readProfileFile(Profile& profile) {
    FILE* file = std::fopen(profilePath(), "rb");
    if (!file) return false;
    unsigned long long offset = 0;
    float x = 0, y = 0, z = 0;
    int score = 0;
    const int count = std::fscanf(file, "%llx %f %f %f %d", &offset, &x, &y, &z, &score);
    std::fclose(file);
    if (count != 5 || offset >= g.heapSize || !plausible({x, y, z})) return false;
    profile = Profile{true, static_cast<u64>(offset), {x, y, z}, score};
    return true;
}

void addCandidate(u64 address, u64 heapOffset, Vec3 value) {
    if (g.candidatesList.size() >= MAX_CANDIDATES) return;
    g.candidatesList.push_back(Candidate{address, heapOffset, value, 1});
}

void scanChunk() {
    if (g.cursor >= g.heapSize) {
        g.stage = g.candidatesList.empty() ? ScanStage::Failed : ScanStage::WaitMove;
        g.candidates = g.candidatesList.size();
        g.message = g.candidatesList.empty()
            ? "No matches. Check calibration.txt and restart."
            : "Target matches found. Walk Link, then press X.";
        return;
    }

    const u64 remaining = g.heapSize - g.cursor;
    const std::size_t bytes = static_cast<std::size_t>(std::min<u64>(SCAN_CHUNK, remaining));
    if (bytes < sizeof(Vec3)) { g.cursor = g.heapSize; return; }

    static std::vector<u8> buffer;
    buffer.resize(bytes);
    if (R_FAILED(dmntchtReadCheatProcessMemory(g.heapBase + g.cursor, buffer.data(), bytes))) {
        g.cursor += bytes;
        g.scanned += bytes;
        return;
    }

    const auto& t = g.calibration.target;
    for (std::size_t i = 0; i + sizeof(Vec3) <= bytes && g.candidatesList.size() < MAX_CANDIDATES; i += 4) {
        Vec3 value{};
        std::memcpy(&value.x, buffer.data() + i, 4);
        std::memcpy(&value.y, buffer.data() + i + 4, 4);
        std::memcpy(&value.z, buffer.data() + i + 8, 4);
        if (!plausible(value)) continue;
        if (!near(value.x, t.x, g.calibration.tolerance) ||
            !near(value.y, t.y, g.calibration.tolerance) ||
            !near(value.z, t.z, g.calibration.tolerance)) continue;
        addCandidate(g.heapBase + g.cursor + i, g.cursor + i, value);
    }

    g.cursor += bytes;
    g.scanned += bytes;
    g.candidates = g.candidatesList.size();
    g.message = (g.cursor >= g.heapSize)
        ? (g.candidatesList.empty() ? "No matches. Check calibration.txt and restart."
                                    : "Target matches found. Walk Link, then press X.")
        : "Searching for calibration coordinates...";
    if (g.cursor >= g.heapSize)
        g.stage = g.candidatesList.empty() ? ScanStage::Failed : ScanStage::WaitMove;
}

void filterMove() {
    std::vector<Candidate> filtered;
    filtered.reserve(g.candidatesList.size());
    for (const auto& candidate : g.candidatesList) {
        Vec3 current{};
        if (!readVec3(candidate.address, current)) continue;
        const float dx = current.x - candidate.value.x;
        const float dz = current.z - candidate.value.z;
        const float horizontal = std::sqrt(dx * dx + dz * dz);
        if (horizontal < MOVE_EPS || horizontal > MAX_STEP) continue;
        Candidate updated = candidate;
        updated.value = current;
        updated.score += 3;
        filtered.push_back(updated);
    }
    g.candidatesList.swap(filtered);
    g.candidates = g.candidatesList.size();
}

void filterJump() {
    std::vector<Candidate> filtered;
    filtered.reserve(g.candidatesList.size());
    for (const auto& candidate : g.candidatesList) {
        Vec3 current{};
        if (!readVec3(candidate.address, current)) continue;
        const float dy = current.y - candidate.value.y;
        const float dx = current.x - candidate.value.x;
        const float dz = current.z - candidate.value.z;
        const float horizontal = std::sqrt(dx * dx + dz * dz);
        if (std::fabs(dy) < JUMP_EPS || std::fabs(dy) > MAX_STEP || horizontal > MAX_STEP) continue;
        Candidate updated = candidate;
        updated.value = current;
        updated.score += 5;
        filtered.push_back(updated);
    }
    g.candidatesList.swap(filtered);
    g.candidates = g.candidatesList.size();
}

void selectBest() {
    if (g.candidatesList.empty()) {
        fail("No stable coordinate candidate. Recheck calibration and restart.");
        return;
    }
    const auto best = std::max_element(g.candidatesList.begin(), g.candidatesList.end(),
        [](const Candidate& a, const Candidate& b) { return a.score < b.score; });
    g.profile = Profile{true, best->heapOffset, best->value, best->score};
    g.player = best->value;
    g.playerValid = true;
    saveProfile();
    g.stage = ScanStage::Ready;
    g.message = "Coordinate profile selected and saved.";
}

} // namespace

State& state() { return g; }

Result initMemory() {
    if (g_dmntInitialized) return 0;
    Result rc = dmntchtInitialize();
    if (R_FAILED(rc)) { fail("dmnt:cht is unavailable."); return rc; }
    g_dmntInitialized = true;
    g.dmntReady = true;

    bool hasProcess = false;
    if (R_SUCCEEDED(dmntchtHasCheatProcess(&hasProcess)) && !hasProcess) {
        rc = dmntchtForceOpenCheatProcess();
        if (R_FAILED(rc)) { fail("Could not open current game process."); return rc; }
        g.attachedByUs = true;
    }
    if (!findTargetProcess()) {
        if (g.attachedByUs) { dmntchtForceCloseCheatProcess(); g.attachedByUs = false; }
        fail("TOTK 1.4.3 process not detected.");
        return 1;
    }
    loadCalibration();
    loadProfile();
    if (g.profile.valid) {
        refreshPlayer();
        if (g.playerValid) {
            g.stage = ScanStage::Ready;
            g.message = "Saved profile loaded. Restart discovery if coordinates are wrong.";
        }
    }
    return 0;
}

void shutdownMemory() {
    if (g.attachedByUs) { dmntchtForceCloseCheatProcess(); g.attachedByUs = false; }
    if (g_dmntInitialized) { dmntchtExit(); g_dmntInitialized = false; }
    g.dmntReady = false;
}

void startAutoScan() {
    if (!g.dmntReady && R_FAILED(initMemory())) return;
    if (!g.calibration.valid) {
        fail("Create calibration.txt with current TOTK X Y Z first.");
        return;
    }
    g.candidatesList.clear();
    g.candidates = 0;
    g.cursor = 0;
    g.scanned = 0;
    g.candidatesSeen = 0;
    g.playerValid = false;
    g.error.clear();
    g.stage = ScanStage::Scanning;
    g.message = "Searching memory for the supplied X/Y/Z...";
}

void captureMove() {
    if (g.stage != ScanStage::WaitMove) return;
    filterMove();
    if (g.candidatesList.empty()) { fail("No moving match. Increase tolerance or move farther."); return; }
    g.stage = ScanStage::WaitJump;
    g.message = "Now jump or change elevation, then press X.";
}

void captureJump() { if (g.stage == ScanStage::WaitJump) { filterJump(); selectBest(); } }

void resetScan() {
    g.stage = ScanStage::Idle;
    g.message = "Ready";
    g.error.clear();
    g.candidatesList.clear();
    g.candidates = 0;
    g.candidatesSeen = 0;
    g.cursor = 0;
    g.scanned = 0;
    g.playerValid = false;
}

void refreshPlayer() {
    if (!g.profile.valid || !g.heapBase || g.profile.offset >= g.heapSize) { g.playerValid = false; return; }
    Vec3 value{};
    if (!readVec3(g.heapBase + g.profile.offset, value)) { g.playerValid = false; return; }
    g.player = value;
    g.playerValid = true;
}

void tick() {
    if (!g.dmntReady) return;
    if (g.stage == ScanStage::Scanning) scanChunk();
    else if (g.stage == ScanStage::Ready) refreshPlayer();
}

void saveProfile() {
    if (!g.profile.valid) return;
    FILE* file = std::fopen(profilePath(), "wb");
    if (!file) return;
    std::fprintf(file, "%llx %.7g %.7g %.7g %d\n",
        static_cast<unsigned long long>(g.profile.offset),
        static_cast<double>(g.profile.value.x), static_cast<double>(g.profile.value.y),
        static_cast<double>(g.profile.value.z), g.profile.score);
    std::fclose(file);
}

void loadProfile() { Profile p{}; if (readProfileFile(p)) g.profile = p; }
void loadCalibration() { Calibration c{}; if (readCalibrationFile(c)) g.calibration = c; }

const char* stageText(ScanStage stage) {
    switch (stage) {
        case ScanStage::Idle: return "Ready";
        case ScanStage::Scanning: return "Scanning";
        case ScanStage::WaitMove: return "Walk Link / press X";
        case ScanStage::WaitJump: return "Jump / press X";
        case ScanStage::Ready: return "Ready";
        case ScanStage::Failed: return "Failed";
        default: return "Unknown";
    }
}

std::string layerName(const Vec3& p) {
    if (p.y > 500.0f) return "Sky";
    if (p.y < -100.0f) return "Depths";
    return "Surface";
}

} // namespace ex
