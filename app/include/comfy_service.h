#pragma once
// Stage 2: hand the layout to ComfyUI and wait for the painted map.
//
// Only one graph remains. Ideogram 4 takes the layout as bounding boxes inside a
// structured JSON caption, so nothing is uploaded and no ControlNet is involved.
#include "map_types.h"
#include "winhttp_client.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace dnd {

struct ComfyResult {
    bool ok = false;
    std::vector<std::vector<uint8_t>> images;
    std::string error;
    std::string promptId;
};

class ComfyService {
public:
    static bool CheckConnection(const std::string& baseUrl, std::string& outVersion,
                                std::string& outError) {
        HttpResponse resp = WinHttpClient::Get(baseUrl + "/system_stats", 5);
        if (!resp.success) {
            outError = resp.errorMessage.empty()
                           ? ("HTTP " + std::to_string(resp.statusCode))
                           : resp.errorMessage;
            return false;
        }
        try {
            auto j = nlohmann::json::parse(resp.body);
            outVersion = j.value("system", nlohmann::json::object())
                             .value("comfyui_version", std::string("unknown"));
            return true;
        } catch (const std::exception& e) {
            outError = e.what();
            return false;
        }
    }

    static int64_t ResolveSeed(int64_t configured) {
        if (configured >= 0) return configured;
        static std::mt19937_64 rng{std::random_device{}()};
        return (int64_t)(rng() % 9000000000000000ULL) + 1;
    }

    // Pixel size matching the map's cell aspect, snapped to a multiple of 16.
    static void TargetSize(const GridConfig& grid, float megapixels, int& outW, int& outH) {
        double scale = std::sqrt(megapixels * 1000000.0 /
                                 std::max(1, grid.cols * grid.rows));
        auto snap = [](double v) { return std::max(256, (int)std::lround(v / 16.0) * 16); };
        outW = snap(grid.cols * scale);
        outH = snap(grid.rows * scale);
    }

    // Two UNets - a conditional and an unconditional one - are combined by
    // DualModelGuider to give true classifier-free guidance, alongside
    // Ideogram's own sigma schedule.
    static constexpr int kMinSteps = 4;
    static constexpr int kMaxSteps = 64;

    // The scheduler spread that goes with a step count. Ideogram's own presets
    // pair fewer steps with a wider spread, which is what keeps a short
    // schedule from coming out muddy; a slider that moved the steps and left
    // the spread behind would make every setting between the presets worse than
    // either of them.
    static float StdForSteps(int steps) {
        steps = std::clamp(steps, kMinSteps, kMaxSteps);
        if (steps <= 12) return 2.0f;
        if (steps >= 48) return 1.5f;
        if (steps <= 20) return 2.0f + (1.75f - 2.0f) * (steps - 12) / 8.0f;
        return 1.75f + (1.5f - 1.75f) * (steps - 20) / 28.0f;
    }

    static nlohmann::json BuildGraph(const ComfyConfig& cfg, const std::string& captionJson,
                                     int width, int height, int64_t seed) {
        int steps = cfg.steps > 0 ? cfg.steps : 48;
        steps = std::clamp(steps, kMinSteps, kMaxSteps);
        float mu = 0.0f;
        float sigmaStd = StdForSteps(steps);

        auto snap = [](int v) { return std::max(256, ((v + 15) / 16) * 16); };
        width = snap(width);
        height = snap(height);

        nlohmann::json g;
        g["unet"] = {{"class_type", "UNETLoader"},
                     {"inputs", {{"unet_name", cfg.unet}, {"weight_dtype", "default"}}}};
        g["unet_uncond"] = {{"class_type", "UNETLoader"},
                            {"inputs", {{"unet_name", cfg.unet_uncond},
                                        {"weight_dtype", "default"}}}};
        g["clip"] = {{"class_type", "CLIPLoader"},
                     {"inputs", {{"clip_name", cfg.clip}, {"type", "ideogram4"},
                                 {"device", "default"}}}};
        g["vae"] = {{"class_type", "VAELoader"}, {"inputs", {{"vae_name", cfg.vae}}}};
        g["pos"] = {{"class_type", "CLIPTextEncode"},
                    {"inputs", {{"text", captionJson},
                                {"clip", nlohmann::json::array({"clip", 0})}}}};
        g["neg"] = {{"class_type", "ConditioningZeroOut"},
                    {"inputs", {{"conditioning", nlohmann::json::array({"pos", 0})}}}};
        g["cfg_override"] = {{"class_type", "CFGOverride"},
                             {"inputs", {{"cfg", cfg.cfg_late},
                                         {"start_percent", cfg.cfg_late_start},
                                         {"end_percent", 1.0},
                                         {"model", nlohmann::json::array({"unet", 0})}}}};
        g["guider"] = {{"class_type", "DualModelGuider"},
                       {"inputs", {{"cfg", cfg.cfg},
                                   {"model", nlohmann::json::array({"cfg_override", 0})},
                                   {"positive", nlohmann::json::array({"pos", 0})},
                                   {"model_negative", nlohmann::json::array({"unet_uncond", 0})},
                                   {"negative", nlohmann::json::array({"neg", 0})}}}};
        g["sigmas"] = {{"class_type", "Ideogram4Scheduler"},
                       {"inputs", {{"steps", steps}, {"width", width}, {"height", height},
                                   {"mu", mu}, {"std", sigmaStd}}}};
        g["sampler_sel"] = {{"class_type", "KSamplerSelect"},
                            {"inputs", {{"sampler_name", "euler"}}}};
        g["noise"] = {{"class_type", "RandomNoise"}, {"inputs", {{"noise_seed", seed}}}};
        g["latent"] = {{"class_type", "EmptyFlux2LatentImage"},
                       {"inputs", {{"width", width}, {"height", height}, {"batch_size", 1}}}};
        g["sampler"] = {{"class_type", "SamplerCustomAdvanced"},
                        {"inputs", {{"noise", nlohmann::json::array({"noise", 0})},
                                    {"guider", nlohmann::json::array({"guider", 0})},
                                    {"sampler", nlohmann::json::array({"sampler_sel", 0})},
                                    {"sigmas", nlohmann::json::array({"sigmas", 0})},
                                    {"latent_image", nlohmann::json::array({"latent", 0})}}}};
        g["decode"] = {{"class_type", "VAEDecode"},
                       {"inputs", {{"samples", nlohmann::json::array({"sampler", 0})},
                                   {"vae", nlohmann::json::array({"vae", 0})}}}};
        g["save"] = {{"class_type", "SaveImage"},
                     {"inputs", {{"filename_prefix", "battlemap"},
                                 {"images", nlohmann::json::array({"decode", 0})}}}};
        return g;
    }

    // Hand the graphics card back. Both services want most of the card, so
    // whichever one is about to work asks the other to let go first.
    static bool FreeMemory(const std::string& baseUrl) {
        nlohmann::json payload = {{"unload_models", true}, {"free_memory", true}};
        HttpResponse resp = WinHttpClient::PostJson(baseUrl + "/free", payload.dump(), 30);
        return resp.success && resp.statusCode >= 200 && resp.statusCode < 300;
    }

    static std::string QueuePrompt(const std::string& baseUrl, const nlohmann::json& graph,
                                   std::string& outError) {
        nlohmann::json payload;
        payload["prompt"] = graph;
        payload["client_id"] = "dnd-battlemap-app";
        HttpResponse resp = WinHttpClient::PostJson(baseUrl + "/prompt", payload.dump(), 120);
        if (!resp.success) {
            // ComfyUI puts validation failures in the body; surfacing the node and
            // reason is the difference between a fixable error and a mystery.
            outError = "ComfyUI rejected the job";
            try {
                auto j = nlohmann::json::parse(resp.body);
                if (j.contains("error")) {
                    auto e = j["error"];
                    outError = e.is_object() ? e.value("message", outError) : e.dump();
                }
                if (j.contains("node_errors") && !j["node_errors"].empty())
                    outError += " | " + j["node_errors"].dump();
            } catch (const std::exception&) {
                if (!resp.body.empty()) outError += ": " + resp.body.substr(0, 400);
            }
            return "";
        }
        try {
            return nlohmann::json::parse(resp.body).value("prompt_id", std::string(""));
        } catch (const std::exception& e) {
            outError = e.what();
            return "";
        }
    }

    static ComfyResult PollAndDownload(const std::string& baseUrl, const std::string& promptId,
                                       int timeoutSeconds,
                                       const std::function<void(const std::string&)>& onProgress,
                                       const std::function<bool()>& shouldCancel = nullptr) {
        ComfyResult result;
        result.promptId = promptId;
        auto start = std::chrono::steady_clock::now();
        std::string lastNote;

        while (true) {
            if (shouldCancel && shouldCancel()) {
                result.error = "cancelled";
                return result;
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::steady_clock::now() - start).count();
            if (elapsed > timeoutSeconds) {
                result.error = "timed out after " + std::to_string(timeoutSeconds) + "s";
                return result;
            }

            HttpResponse resp = WinHttpClient::Get(baseUrl + "/history/" + promptId, 20);
            if (resp.success) {
                try {
                    auto j = nlohmann::json::parse(resp.body);
                    if (j.contains(promptId)) {
                        auto entry = j[promptId];
                        auto status = entry.value("status", nlohmann::json::object());
                        if (status.contains("messages")) {
                            for (const auto& m : status["messages"]) {
                                if (m.is_array() && !m.empty() &&
                                    m[0].get<std::string>() == "execution_error") {
                                    result.error = "ComfyUI execution error: " + m.dump();
                                    return result;
                                }
                            }
                        }
                        bool done = status.value("completed", false) ||
                                    status.value("status_str", std::string("")) == "success";
                        if (done) {
                            DownloadImages(baseUrl,
                                           entry.value("outputs", nlohmann::json::object()),
                                           result);
                            result.ok = !result.images.empty();
                            if (!result.ok) result.error = "ComfyUI produced no images";
                            return result;
                        }
                        if (status.value("status_str", std::string("")) == "error") {
                            result.error = "ComfyUI reported an error";
                            return result;
                        }
                    }
                } catch (const std::exception&) {
                    // history is not ready yet; keep waiting
                }
            }

            if (onProgress) {
                std::string note = QueueNote(baseUrl, promptId) + ", " +
                                   std::to_string(elapsed) + "s elapsed";
                if (note != lastNote) { onProgress(note); lastNote = note; }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        }
    }

private:
    static std::string QueueNote(const std::string& baseUrl, const std::string& promptId) {
        HttpResponse resp = WinHttpClient::Get(baseUrl + "/queue", 10);
        if (!resp.success) return "rendering";
        try {
            auto j = nlohmann::json::parse(resp.body);
            for (const auto& item : j.value("queue_running", nlohmann::json::array()))
                if (item.is_array() && item.size() > 1 && item[1] == promptId) return "rendering";
            int pos = 1;
            for (const auto& item : j.value("queue_pending", nlohmann::json::array())) {
                if (item.is_array() && item.size() > 1 && item[1] == promptId)
                    return "queued (position " + std::to_string(pos) + ")";
                ++pos;
            }
        } catch (const std::exception&) {
        }
        return "rendering";
    }

    static void DownloadImages(const std::string& baseUrl, const nlohmann::json& outputs,
                               ComfyResult& result) {
        for (auto it = outputs.begin(); it != outputs.end(); ++it) {
            if (!it.value().contains("images")) continue;
            for (const auto& img : it.value()["images"]) {
                std::string type = img.value("type", std::string("output"));
                if (type == "temp") continue;
                std::string url = baseUrl + "/view?filename=" +
                                  UrlEncode(img.value("filename", std::string(""))) +
                                  "&subfolder=" +
                                  UrlEncode(img.value("subfolder", std::string(""))) +
                                  "&type=" + type;
                HttpResponse resp = WinHttpClient::Get(url, 180);
                if (resp.success && !resp.rawData.empty()) result.images.push_back(resp.rawData);
            }
        }
    }

    static std::string UrlEncode(const std::string& s) {
        static const char* hex = "0123456789ABCDEF";
        std::string out;
        for (unsigned char c : s) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out.push_back((char)c);
            else if (c == ' ') out += "%20";
            else { out.push_back('%'); out.push_back(hex[c >> 4]); out.push_back(hex[c & 15]); }
        }
        return out;
    }
};

}  // namespace dnd
