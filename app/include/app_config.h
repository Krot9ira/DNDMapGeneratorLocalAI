#pragma once
// config.json load/save. The same file the Python CLI uses, so changing a
// setting in the app changes it for the command line tools too.
#include "map_types.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace dnd {

class ConfigStore {
public:
    static std::string DefaultPath() { return "config.json"; }

    static bool Load(const std::string& path, AppConfig& cfg, std::string& outError) {
        if (!std::filesystem::exists(path)) {
            outError = "config.json not found; using built-in defaults";
            return false;
        }
        try {
            std::ifstream f(path);
            nlohmann::json j;
            f >> j;

            if (j.contains("ollama")) {
                const auto& o = j["ollama"];
                cfg.ollama.base_url = o.value("base_url", cfg.ollama.base_url);
                cfg.ollama.model = o.value("model", cfg.ollama.model);
                cfg.ollama.temperature = o.value("temperature", cfg.ollama.temperature);
                cfg.ollama.timeout_seconds = o.value("timeout", cfg.ollama.timeout_seconds);
            }
            if (j.contains("comfy")) {
                const auto& c = j["comfy"];
                cfg.comfy.base_url = c.value("base_url", cfg.comfy.base_url);
                if (c.contains("ideogram")) {
                    const auto& n = c["ideogram"];
                    cfg.comfy.unet = n.value("unet", cfg.comfy.unet);
                    cfg.comfy.unet_uncond = n.value("unet_unconditional", cfg.comfy.unet_uncond);
                    cfg.comfy.clip = n.value("clip", cfg.comfy.clip);
                    cfg.comfy.vae = n.value("vae", cfg.comfy.vae);
                    cfg.comfy.preset = n.value("preset", cfg.comfy.preset);
                    cfg.comfy.cfg = n.value("cfg", cfg.comfy.cfg);
                    cfg.comfy.cfg_late = n.value("cfg_late", cfg.comfy.cfg_late);
                    cfg.comfy.cfg_late_start = n.value("cfg_late_start", cfg.comfy.cfg_late_start);
                    cfg.comfy.megapixels = n.value("target_megapixels", cfg.comfy.megapixels);
                    if (n.contains("seed") && n["seed"].is_number())
                        cfg.comfy.seed = n["seed"].get<int64_t>();
                }
            }
            cfg.default_style = j.value("default_style", cfg.default_style);
            cfg.default_size = j.value("default_size", cfg.default_size);
            cfg.border_cells = j.value("border_cells", cfg.border_cells);
            cfg.output_dir = j.value("output_dir", cfg.output_dir);
            return true;
        } catch (const std::exception& e) {
            outError = std::string("config.json is not valid JSON: ") + e.what();
            return false;
        }
    }

    static bool Save(const std::string& path, const AppConfig& cfg, std::string& outError) {
        try {
            nlohmann::json j;
            // Preserve anything the Python side keeps that the app does not model.
            if (std::filesystem::exists(path)) {
                std::ifstream in(path);
                try { in >> j; } catch (const std::exception&) { j = nlohmann::json::object(); }
            }
            j["ollama"]["base_url"] = cfg.ollama.base_url;
            j["ollama"]["model"] = cfg.ollama.model;
            j["ollama"]["temperature"] = cfg.ollama.temperature;
            j["ollama"]["timeout"] = cfg.ollama.timeout_seconds;

            j["comfy"]["base_url"] = cfg.comfy.base_url;
            auto& n = j["comfy"]["ideogram"];
            n["unet"] = cfg.comfy.unet;
            n["unet_unconditional"] = cfg.comfy.unet_uncond;
            n["clip"] = cfg.comfy.clip;
            n["vae"] = cfg.comfy.vae;
            n["preset"] = cfg.comfy.preset;
            n["cfg"] = cfg.comfy.cfg;
            n["cfg_late"] = cfg.comfy.cfg_late;
            n["cfg_late_start"] = cfg.comfy.cfg_late_start;
            n["target_megapixels"] = cfg.comfy.megapixels;
            n["seed"] = cfg.comfy.seed;

            j["default_style"] = cfg.default_style;
            j["default_size"] = cfg.default_size;
            j["border_cells"] = cfg.border_cells;
            j["output_dir"] = cfg.output_dir;

            std::ofstream f(path);
            if (!f.is_open()) {
                outError = "cannot write " + path;
                return false;
            }
            f << j.dump(2);
            return true;
        } catch (const std::exception& e) {
            outError = e.what();
            return false;
        }
    }
};

}  // namespace dnd
