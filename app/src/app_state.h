#pragma once
// Application state: the edited map, the background job, and the log.
//
// Threading rule enforced here: worker threads only ever touch `Job` under its
// mutex. They never touch the tile grid, ImGui state or D3D resources. Results
// are handed over as plain data and applied by the main thread. Getting this
// wrong is what made the previous build crash mid-generation.
#include "map_types.h"
#include "map_architect.h"
#include "map_serializer.h"
#include "style_manager.h"
#include "app_config.h"

#include <imgui.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace dnd {

enum class Tool {
    Select = 0, Paint, RectFill, PlaceProp, EraseProp, Annotate, Effects, COUNT
};

struct EditSnapshot {
    TileGrid grid;
    std::vector<Feature> features;
    std::vector<Annotation> annotations;
    std::vector<Effect> effects;
};

struct Job {
    std::atomic<bool> running{false};
    std::atomic<bool> cancel{false};
    std::mutex mtx;

    std::string status = "Idle";
    std::vector<std::string> log;

    // Hand-off slots, all guarded by mtx.
    bool hasMap = false;
    MapData map;
    bool hasImage = false;
    std::vector<uint8_t> imagePng;
    std::string finishedMessage;
    bool failed = false;

    void Log(const std::string& line) {
        std::lock_guard<std::mutex> lock(mtx);
        log.push_back(line);
        if (log.size() > 400) log.erase(log.begin(), log.begin() + 100);
    }

    void SetStatus(const std::string& s) {
        std::lock_guard<std::mutex> lock(mtx);
        status = s;
    }

    std::string Status() {
        std::lock_guard<std::mutex> lock(mtx);
        return status;
    }
};

struct AppState {
    AppConfig config;
    std::string configPath = "config.json";
    StyleManager styles;
    Job job;

    // -- document ------------------------------------------------------
    MapData map;            // metadata, areas; zones are regenerated from grid
    TileGrid grid;          // the editable surface
    std::vector<Feature> features;
    std::vector<Annotation> annotations;
    std::vector<Effect> effects;
    std::string currentFile;
    bool dirty = false;

    // -- create tab ----------------------------------------------------
    std::string sceneText =
        "A stone city dock with one large ship moored alongside, a gangway onto the quay, "
        "barrels and cargo crates along the waterfront.";
    std::string selectedStyle = "city_harbour";
    int cols = 25, rows = 19;          // free-form, 10..150 cells
    int layoutIndex = 0;               // 0 = from style
    int seed = 0;
    bool randomSeed = true;
    std::string propDensity = "medium";
    std::string terrainKind = "none";
    std::string terrainAmount = "medium";

    // -- editor --------------------------------------------------------
    Tool tool = Tool::Paint;
    Tile paintTile = Tile::Floor;
    int brushSize = 1;
    std::string propKind = "barrel";
    // Custom prop: the user names the thing and says how much licence the
    // renderer gets with it.
    bool customProp = false;
    std::string customLabel;
    std::string customDesc;
    int customElaboration = 1;         // 0 exact, 1 some, 2 free
    int selectedFeature = -1;
    // Annotation being drawn or edited.
    std::string annLabel;
    std::string annDesc;
    int annElaboration = 1;
    int selectedAnnotation = -1;
    bool showOpenDialog = false;
    // Effects layer.
    std::string effectKind = "fog";
    bool customEffect = false;
    std::string effLabel;
    std::string effDesc;
    int effElaboration = 1;
    int effIntensity = 1;              // 0 low, 1 medium, 2 high
    float zoom = 1.0f;
    ImVec2 pan{40.0f, 40.0f};
    bool showCellGuides = true;
    bool showProps = true;
    int selectedAreaIndex = -1;
    std::deque<EditSnapshot> undoStack, redoStack;

    // -- connection status ---------------------------------------------
    std::string ollamaStatus = "not checked";
    std::string comfyStatus = "not checked";
    bool ollamaOk = false, comfyOk = false;
    std::vector<std::string> ollamaModels;

    // -- helpers -------------------------------------------------------
    void SyncGridFromMap() {
        grid = arch::ZonesToGrid(map);
        features = map.features;
        annotations = map.annotations;
        effects = map.effects;
        undoStack.clear();
        redoStack.clear();
    }

    // Zones are regenerated from the painted grid, so hand edits and generated
    // maps round-trip through exactly the same path.
    void SyncMapFromGrid() {
        map.grid.cols = grid.cols;
        map.grid.rows = grid.rows;
        map.zones = arch::ExtractZones(grid);
        map.features = features;
        map.annotations = annotations;
        map.effects = effects;
    }

    void PushUndo() {
        undoStack.push_back({grid, features, annotations, effects});
        if (undoStack.size() > 60) undoStack.pop_front();
        redoStack.clear();
        dirty = true;
    }

    void Undo() {
        if (undoStack.empty()) return;
        redoStack.push_back({grid, features, annotations, effects});
        grid = undoStack.back().grid;
        features = undoStack.back().features;
        annotations = undoStack.back().annotations;
        effects = undoStack.back().effects;
        undoStack.pop_back();
        dirty = true;
    }

    void Redo() {
        if (redoStack.empty()) return;
        undoStack.push_back({grid, features, annotations, effects});
        grid = redoStack.back().grid;
        features = redoStack.back().features;
        annotations = redoStack.back().annotations;
        effects = redoStack.back().effects;
        redoStack.pop_back();
        dirty = true;
    }
};

}  // namespace dnd
