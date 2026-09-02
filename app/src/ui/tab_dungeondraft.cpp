#include "tab_dungeondraft.h"
#include "ui_context.h"
#include "../core/process_runner.h"
#include "../core/file_dialogs.h"
#include "../services/render_service.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;

namespace dnd {

void TabDungeondraft::RefreshDungeondraftStatsAsync(AppState& app) {
    if (app.ddDbStatsRefreshing.exchange(true)) return;

    std::thread([&app]() {
        std::string jsonStr;
        ProcessRunner::RunCapture("python -u tools/dungeondraft_indexer.py stats --json", "",
            [&](const std::string& line) { jsonStr += line + "\n"; },
            &app.job.cancel);
        try {
            nlohmann::json j = nlohmann::json::parse(jsonStr);
            app.ddDbPacks = j.value("packs", 0);
            app.ddDbActivePacks = j.value("enabled_packs", 0);
            app.ddDbAssets = j.value("assets", 0);
            app.ddDbStockAssets = j.value("stock_assets", 0);
            app.ddDbCustomAssets = j.value("custom_assets", 0);
            app.ddDbEnriched = j.value("enriched", 0);
            app.ddDbStockEnriched = j.value("stock_enriched", 0);
            app.ddDbCustomEnriched = j.value("custom_enriched", 0);
            app.ddDbStatsOk = true;
        } catch (...) {
            app.ddDbStatsOk = false;
        }
        app.ddDbStatsLoaded = true;
        app.ddDbStatsRefreshing = false;
    }).detach();
}

void TabDungeondraft::StartDungeondraftExport(AppState& app) {
    if (!app.BeginJob("Exporting to Dungeondraft (.dungeondraft_map)...")) return;

    std::string mapPath;
    if (!RenderService::SaveCurrentPlan(app, &mapPath)) {
        app.job.Log("[error] Could not save current map plan to disk.");
        app.FinishJob(false, "Export failed - cannot save map.json");
        return;
    }

    std::string outPath = app.ddOutputPath;
    if (outPath.empty()) {
        std::string dir = OutputDir(app, app.map.meta.name);
        outPath = dir + "/" + app.map.meta.name + ".dungeondraft_map";
        app.ddOutputPath = outPath;
    }

    int seed = app.ddRandomSeed
                   ? (int)(std::chrono::system_clock::now().time_since_epoch().count() & 0x7FFFFFFF)
                   : app.ddSeed;
    bool autoOpen = app.ddAutoOpen;
    bool autoFoundry = app.ddAutoFoundry;
    std::string appPath = app.config.dungeondraft.app_path;

    std::thread([&app, mapPath, outPath, seed, autoOpen, autoFoundry, appPath]() {
        Job& job = app.job;
        job.Log("Exporting plan: " + mapPath + " -> " + outPath);
        std::string cmd = "python -u tools/pipeline.py dungeondraft \"" + mapPath + "\" --out \"" + outPath + "\" --seed " + std::to_string(seed);
        if (autoFoundry) {
            cmd += " --auto-foundry";
        }
        job.Log("Running: " + cmd);

        bool ok = ProcessRunner::RunCapture(cmd, "", [&](const std::string& line) {
            job.Log(line);
        }, &job.cancel);

        if (ok && fs::exists(outPath)) {
            app.ddLastExportFile = outPath;
            app.ddExportSuccess = true;

            std::string reportPath = fs::path(outPath).replace_extension(".report.json").string();
            if (fs::exists(reportPath)) {
                try {
                    std::ifstream rf(reportPath);
                    nlohmann::json rj;
                    rf >> rj;
                    app.ddPlacedWalls = rj.value("walls_placed", 0);
                    app.ddPlacedObjects = rj.value("objects_placed", 0);
                    app.ddPlacedPortals = rj.value("portals_placed", 0);
                    app.ddUnmatchedProps = rj.contains("unmatched_props") && rj["unmatched_props"].is_array()
                                                 ? (int)rj["unmatched_props"].size() : 0;
                    app.ddPropsDescribed = rj.value("props_matched_by_description", 0);
                    app.ddPropsNamed = rj.value("props_matched_by_name", 0);
                    if (rj.contains("packs_referenced") && rj["packs_referenced"].is_array()) {
                        app.ddReferencedPacksCount = (int)rj["packs_referenced"].size();
                    }
                } catch (...) {}
            } else {
                job.Log("[warn] No report written next to the map: " + reportPath);
            }

            job.Log("Dungeondraft export complete: " + outPath);
            if (autoOpen && !appPath.empty() && fs::exists(appPath)) {
                job.Log("Opening in Dungeondraft: " + appPath);
                FileDialogs::OpenFileInDungeondraft(appPath, outPath);
            }
            if (app.ddPlacedObjects == 0 && app.ddUnmatchedProps > 0) {
                job.Log("[warn] No asset matched any prop in the plan. Scan your asset packs first.");
                app.FinishJob(true, "Map written, but empty - scan your asset packs, then export again.");
            } else {
                app.FinishJob(true, "Dungeondraft map created successfully!");
            }
        } else {
            app.ddExportSuccess = false;
            job.Log("[error] Dungeondraft map export failed. See log output above.");
            app.FinishJob(false, "Export failed - see log.");
        }
    }).detach();
}

void TabDungeondraft::StartDungeondraftScan(AppState& app) {
    if (!app.BeginJob("Scanning Dungeondraft asset packs...")) return;
    std::string assetsDir = app.config.dungeondraft.custom_assets_dir;

    std::thread([&app, assetsDir]() {
        Job& job = app.job;
        std::string cmd = "python -u tools/dungeondraft_indexer.py scan";
        if (!assetsDir.empty()) {
            cmd += " --assets-dir \"" + assetsDir + "\"";
        }
        job.Log("Running asset scan: " + cmd);
        bool ok = ProcessRunner::RunCapture(cmd, "", [&](const std::string& line) {
            job.Log(line);
        }, &job.cancel);
        RefreshDungeondraftStatsAsync(app);
        if (app.job.cancel.load()) {
            app.FinishJob(false, "Asset pack scan cancelled.");
            return;
        }
        app.FinishJob(ok, ok ? "Asset pack scan complete!" : "Asset pack scan failed - see log.");
    }).detach();
}

void TabDungeondraft::StartDungeondraftEnrich(AppState& app, const std::string& scope) {
    std::string scopeTitle = (scope == "stock") ? "Stock/Default Assets" :
                             (scope == "custom") ? "Custom Pack Assets" : "All Assets";
    std::string jobName = "Running Vision Model Tagging on " + scopeTitle + "...";
    if (!app.BeginJob(jobName.c_str())) return;
    std::string model = app.config.dungeondraft.vision_model;
    if (model.empty()) model = "gemma4:12b";

    std::thread([&app, model, scope, scopeTitle]() {
        Job& job = app.job;
        std::string cmd = "python -u tools/dungeondraft_enrich.py --model \"" + model + "\" --scope " + scope;
        job.Log("Running vision tagging (" + scopeTitle + ") with model " + model + ": " + cmd);
        bool ok = ProcessRunner::RunCapture(cmd, "", [&](const std::string& line) {
            job.Log(line);
        }, &job.cancel);
        RefreshDungeondraftStatsAsync(app);
        if (app.job.cancel.load()) {
            job.Log("[info] Stopped. Everything catalogued so far is saved; running again resumes here.");
            app.FinishJob(false, scopeTitle + " vision tagging cancelled.");
            return;
        }
        app.FinishJob(ok, ok ? (scopeTitle + " vision tagging pass complete!") : (scopeTitle + " vision tagging failed - see log."));
    }).detach();
}

void TabDungeondraft::StartDungeondraftQualityCheck(AppState& app) {
    if (!app.BeginJob("Checking cataloguing quality on 200 sampled assets...")) return;
    std::string model = app.config.dungeondraft.vision_model;
    if (model.empty()) model = "gemma4:12b";

    std::thread([&app, model]() {
        Job& job = app.job;
        const std::string sheet = "output/enrichment_sample.html";
        std::string cmd = "python -u tools/check_enrichment_quality.py --model \"" + model +
                          "\" --sample 200 --out \"" + sheet + "\"";
        job.Log("Running: " + cmd);
        bool ok = ProcessRunner::RunCapture(cmd, "", [&](const std::string& line) { job.Log(line); }, &job.cancel);

        if (app.job.cancel.load()) {
            app.FinishJob(false, "Quality check cancelled.");
            return;
        }
        if (ok && fs::exists(sheet)) {
            job.Log("Contact sheet: " + fs::absolute(sheet).string());
            FileDialogs::OpenFolderInExplorer(fs::absolute(sheet).string());
        }
        app.FinishJob(ok, ok ? "Quality check done - read the numbers and the contact sheet."
                             : "Quality check failed - see log.");
    }).detach();
}

void TabDungeondraft::StartDungeondraftValidate(AppState& app) {
    if (!app.BeginJob("Validating Asset Database...")) return;
    std::thread([&app]() {
        Job& job = app.job;
        std::string cmd = "python -u tools/dungeondraft_indexer.py validate";
        job.Log("Running: " + cmd);
        bool ok = ProcessRunner::RunCapture(cmd, "", [&](const std::string& line) {
            job.Log(line);
        }, &job.cancel);
        app.FinishJob(ok, ok ? "Asset DB validation complete!" : "Validation found issues - see log.");
    }).detach();
}

void TabDungeondraft::StartDungeondraftFoundrySatisfy(AppState& app, const std::string& exportPath) {
    std::string reportPath = fs::path(exportPath).replace_extension(".report.json").string();
    if (!fs::exists(reportPath)) return;
    if (!app.BeginJob("Generating missing props via Foundry (Ideogram + Cutout)...")) return;

    std::thread([&app, reportPath]() {
        Job& job = app.job;
        std::string cmd = "python -u tools/dungeondraft_foundry.py satisfy \"" + reportPath + "\"";
        job.Log("Running: " + cmd);
        bool ok = ProcessRunner::RunCapture(cmd, "", [&](const std::string& line) {
            job.Log(line);
        }, &job.cancel);
        RefreshDungeondraftStatsAsync(app);
        app.FinishJob(ok, ok ? "Foundry generation and pack assembly complete! Export again to place new props."
                             : "Foundry generation failed - see log.");
    }).detach();
}

void TabDungeondraft::Draw(AppState& app) {
    if (!app.ddDbStatsLoaded) {
        RefreshDungeondraftStatsAsync(app);
    }

    float logH = 160.0f;
    float leftW = PanelWidth(0.50f, 440.0f, 850.0f);

    ImGui::BeginChild("##dd_left", ImVec2(leftW, -logH), ImGuiChildFlags_Borders);
    ImGui::TextColored(AccentGold(), "Dungeondraft Map Export");
    ImGui::TextDisabled("Assemble the current blueprint layout into a native .dungeondraft_map file.");
    ImGui::Separator();

    ImGui::TextColored(AccentGold(), "Current Layout Blueprint");
    ImGui::BulletText("Title: %s", app.map.meta.title.empty() ? "(untitled)" : app.map.meta.title.c_str());
    ImGui::BulletText("Style: %s | Layout: %s", app.map.meta.style.c_str(), app.map.meta.layout.c_str());
    int sb = app.map.meta.border;
    ImGui::BulletText("Grid: %d x %d playable (%d x %d total with %d bleed margin)",
                      app.grid.cols - 2 * sb, app.grid.rows - 2 * sb,
                      app.grid.cols, app.grid.rows, sb);
    ImGui::BulletText("Rooms/Areas: %d | Placed props: %d | Annotations: %d",
                      (int)app.map.areas.size(), (int)app.features.size(), (int)app.annotations.size());

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(AccentGold(), "Export Settings");

    if (app.ddOutputPath.empty() && !app.map.meta.name.empty()) {
        app.ddOutputPath = OutputDir(app, app.map.meta.name) + "/" + app.map.meta.name + ".dungeondraft_map";
    }

    InputTextString("Output .dungeondraft_map", &app.ddOutputPath);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##ddout")) {
        std::string p = FileDialogs::PickDungeondraftSaveFile(app.ddOutputPath);
        if (!p.empty()) app.ddOutputPath = p;
    }

    ImGui::Checkbox("Random variation seed", &app.ddRandomSeed);
    if (!app.ddRandomSeed) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140);
        ImGui::InputInt("Seed##ddseed", &app.ddSeed);
    }

    ImGui::Checkbox("Auto-render missing props via Foundry (Ideogram + Cutout)", &app.ddAutoFoundry);
    ImGui::SetItemTooltip("If any prop in the plan has no match in your asset packs, automatically render it via ComfyUI Ideogram 4, cut out its background, and package it into a custom .dungeondraft_pack.");

    if (!app.config.dungeondraft.app_path.empty()) {
        ImGui::Checkbox("Launch in Dungeondraft when done", &app.ddAutoOpen);
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(app.job.running.load() || app.grid.cols <= 0);
    ImGui::PushStyleColor(ImGuiCol_Button, GoButton());
    if (ImGui::Button("EXPORT TO DUNGEONDRAFT (.dungeondraft_map)", ImVec2(-1, 40))) {
        StartDungeondraftExport(app);
    }
    ImGui::PopStyleColor();
    ImGui::EndDisabled();

    if (app.ddExportSuccess && !app.ddLastExportFile.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.45f, 0.90f, 0.55f, 1.0f), "Export Successful!");
        ImGui::TextWrapped("Saved to: %s", app.ddLastExportFile.c_str());
        ImGui::BulletText("Placed walls: %d", app.ddPlacedWalls);
        ImGui::BulletText("Placed objects: %d", app.ddPlacedObjects);
        ImGui::BulletText("Portals/doors: %d", app.ddPlacedPortals);
        ImGui::BulletText("Referenced packs: %d", app.ddReferencedPacksCount);
        ImGui::BulletText("Props chosen from the catalogue: %d, from the file name alone: %d",
                          app.ddPropsDescribed, app.ddPropsNamed);
        ImGui::SetItemTooltip("A prop chosen by file name is a guess. Tag your assets with the "
                              "vision model to choose by what the picture actually shows.");
        if (app.ddUnmatchedProps > 0) {
            ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f),
                               "%d plan props found no asset and were left out.", app.ddUnmatchedProps);
            ImGui::BeginDisabled(app.job.running.load());
            if (ImGui::Button("Render Missing Props Now with Foundry", ImVec2(-1, 30))) {
                StartDungeondraftFoundrySatisfy(app, app.ddLastExportFile);
            }
            ImGui::EndDisabled();
        }

        ImGui::Spacing();
        if (!app.config.dungeondraft.app_path.empty() && fs::exists(app.config.dungeondraft.app_path)) {
            if (ImGui::Button("Open in Dungeondraft", ImVec2(200, 32))) {
                FileDialogs::OpenFileInDungeondraft(app.config.dungeondraft.app_path, app.ddLastExportFile);
            }
            ImGui::SameLine();
        }
        if (ImGui::Button("Open Output Folder", ImVec2(180, 32))) {
            fs::path p(app.ddLastExportFile);
            FileDialogs::OpenFolderInExplorer(p.parent_path().string());
        }
    }

    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##dd_right", ImVec2(0, -logH), ImGuiChildFlags_Borders);
    ImGui::TextColored(AccentGold(), "Dungeondraft Asset Library");
    ImGui::TextDisabled("Indexed assets and packs from Dungeondraft.pck and custom packs.");
    ImGui::Separator();

    ImGui::Text("Database file: %s", fs::exists("data/assets.db") ? "data/assets.db (Found)" : "data/assets.db (Not found)");
    if (app.ddDbStatsLoaded && !app.ddDbStatsOk) {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.45f, 1.0f),
                           "Could not read the library. Is Python on PATH? Scan the packs to build it.");
    }
    ImGui::BulletText("Total Packs in DB:    %d", app.ddDbPacks);
    ImGui::BulletText("Active/Enabled Packs: %d", app.ddDbActivePacks);
    ImGui::BulletText("Total Textures:       %d (Enriched: %d)", app.ddDbAssets, app.ddDbEnriched);
    ImGui::Indent(16.0f);
    ImGui::BulletText("Stock / Default:  %d textures (Enriched: %d)", app.ddDbStockAssets, app.ddDbStockEnriched);
    ImGui::BulletText("Custom Packs:     %d textures (Enriched: %d)", app.ddDbCustomAssets, app.ddDbCustomEnriched);
    ImGui::Unindent(16.0f);

    ImGui::Spacing();
    if (ImGui::Button("Refresh Statistics", ImVec2(-1, 28))) {
        RefreshDungeondraftStatsAsync(app);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(AccentGold(), "Asset Indexing & Vision Tools");

    ImGui::BeginDisabled(app.job.running.load());
    if (ImGui::Button("Scan & Index Asset Packs", ImVec2(-1, 32))) {
        StartDungeondraftScan(app);
    }
    ImGui::SetItemTooltip("Scans stock Dungeondraft assets and your custom packs folder, extracting metadata and thumbnails.");

    ImGui::Spacing();
    ImGui::Text("Vision Tagging (Ollama: %s):", app.config.dungeondraft.vision_model.c_str());

    if (ImGui::Button("Check Quality on 200 Assets First", ImVec2(-1, 30))) {
        StartDungeondraftQualityCheck(app);
    }
    ImGui::SetItemTooltip("Catalogues 200 sampled assets, scores how often the model just reworded "
                          "the file name, and opens a contact sheet. Do this before a full pass: a "
                          "bad description is cached and never looked at again.");

    if (ImGui::Button("Tag Stock Assets Only", ImVec2(-1, 30))) {
        StartDungeondraftEnrich(app, "stock");
    }
    ImGui::SetItemTooltip("Runs vision model tagging ONLY on stock/default Dungeondraft assets (%d textures).", app.ddDbStockAssets);

    if (ImGui::Button("Tag Custom Assets Only", ImVec2(-1, 30))) {
        StartDungeondraftEnrich(app, "custom");
    }
    ImGui::SetItemTooltip("Runs vision model tagging ONLY on custom pack assets (%d textures).", app.ddDbCustomAssets);

    if (ImGui::Button("Tag All Assets (Stock + Custom)", ImVec2(-1, 30))) {
        StartDungeondraftEnrich(app, "all");
    }
    ImGui::SetItemTooltip("Runs vision model tagging on all unenriched textures across both stock and custom packs.");

    ImGui::Spacing();
    if (ImGui::Button("Validate Asset Database", ImVec2(-1, 28))) {
        StartDungeondraftValidate(app);
    }
    ImGui::EndDisabled();

    if (app.job.running.load()) {
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Cancel Current Task", ImVec2(-1, 30))) {
            app.job.cancel = true;
            app.job.Log("[info] Cancel requested.");
        }
    }

    ImGui::EndChild();

    DrawJobLog(app, logH);
}

} // namespace dnd
