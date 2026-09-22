#include "ui/RenderDialog.h"
#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include "app/OfflineRenderer.h"
#include "core/Graph.h"
#include "core/PathUtil.h"
#include "core/Preferences.h"
#include "modules/OutputNode.h"
#include "ui/FileDialog.h"

namespace oss {

void RenderDialog::seed(Graph& g, const Preferences& prefs, const std::string& projectPath) {
    settings_ = RenderSettings{};
    settings_.endBar = g.automation().lengthBars();          // the Automation song length
    settings_.width  = prefs.textureWidth;
    settings_.height = prefs.textureHeight;
    // Default file: next to the project as <basename>.mp4, else render.mp4 in the projects dir.
    std::string base = projectPath.empty() ? std::string("render") : fileBaseName(projectPath);
    if (base.size() > 4 && base.compare(base.size() - 4, 4, ".oss") == 0) base.erase(base.size() - 4);
    std::string dir = projectPath.empty() ? prefs.projectsDir : parentDir(projectPath);
    settings_.outPath = dir.empty() ? base + ".mp4" : dir + "/" + base + ".mp4";
}

void RenderDialog::draw(Graph& g, const Preferences& prefs, OfflineRenderer& r, bool* show,
                        const std::string& projectPath, std::string& status) {
    // The job ends inside Application::frame (between draws): hand its outcome to the toolbar.
    if (wasActive_ && !r.active()) { outcome_ = r.progress().status; status = outcome_; }
    wasActive_ = r.active();

    if (show && *show) {
        if (!seeded_) { seed(g, prefs, projectPath); seeded_ = true; }
        ImGui::SetNextWindowSize(ImVec2(480.0f, 0.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Render Video", show)) {
            const Transport& t = g.transport();

            float sb = (float)settings_.startBar, eb = (float)settings_.endBar, pb = (float)settings_.prerollBars;
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::InputFloat("Start bar", &sb, 0.0f, 0.0f, "%.2f")) settings_.startBar = std::max(0.0f, sb);
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::InputFloat("Finish bar", &eb, 0.0f, 0.0f, "%.2f")) settings_.endBar = eb;
            ImGui::SameLine();
            if (ImGui::Button("Use loop range")) { settings_.startBar = t.loopStartBar; settings_.endBar = t.loopEndBar; }
            ImGui::SetNextItemWidth(100.0f);
            if (ImGui::InputFloat("Pre-roll (bars)", &pb, 0.0f, 0.0f, "%.2f")) settings_.prerollBars = std::max(0.0f, pb);

            ImGui::SetNextItemWidth(100.0f);
            std::string fpsLabel = std::to_string(settings_.fps) + " fps";
            if (ImGui::BeginCombo("Frame rate", fpsLabel.c_str())) {
                for (int i = 0; i < kRenderFrameRateCount; ++i) {
                    std::string lbl = std::to_string(kRenderFrameRates[i]) + " fps";
                    if (ImGui::Selectable(lbl.c_str(), kRenderFrameRates[i] == settings_.fps)) settings_.fps = kRenderFrameRates[i];
                }
                ImGui::EndCombo();
            }

            ImGui::SetNextItemWidth(100.0f); ImGui::InputInt("Width",  &settings_.width,  0, 0);
            ImGui::SetNextItemWidth(100.0f); ImGui::InputInt("Height", &settings_.height, 0, 0);
            ImGui::SameLine();
            if (ImGui::Button("Use live size")) { settings_.width = prefs.textureWidth; settings_.height = prefs.textureHeight; }

            char pathBuf[1024];
            std::snprintf(pathBuf, sizeof(pathBuf), "%s", settings_.outPath.c_str());
            ImGui::SetNextItemWidth(-100.0f);
            if (ImGui::InputText("##outpath", pathBuf, sizeof(pathBuf))) settings_.outPath = pathBuf;
            ImGui::SameLine();
            if (ImGui::Button("Browse...")) {
                std::string defName = fileBaseName(settings_.outPath);
                if (defName.empty()) defName = "render.mp4";
                std::string p = saveFileDialog("Render Video", "MP4", {"mp4"}, defName, prefs.projectsDir);
                if (!p.empty()) settings_.outPath = ensureExtension(p, "mp4");
            }

            const double spb = t.secondsPerBar();
            const long long frames = renderFrameCount(settings_, spb);
            ImGui::Text("%.2f bars -> %lld frames (%.1f s at %.2f bpm)",
                        settings_.endBar - settings_.startBar, frames, (double)frames / settings_.fps, t.bpm);

            bool hasOutput = false;
            for (const auto& n : g.nodes()) if (dynamic_cast<OutputNode*>(n.get())) { hasOutput = true; break; }
            std::string why;
            const bool valid = validateRenderSettings(settings_, hasOutput, why);

            ImGui::BeginDisabled(!valid || r.active());
            if (ImGui::Button("Render")) {
                error_.clear(); outcome_.clear();
                if (!r.start(g, settings_, error_)) status = "render failed: " + error_;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Close")) *show = false;
            if (!valid)                 ImGui::TextDisabled("%s", why.c_str());
            if (!error_.empty())        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s", error_.c_str());
            else if (!outcome_.empty()) ImGui::TextUnformatted(outcome_.c_str());
        }
        ImGui::End();
    }

    // Progress modal while the job runs. Opened/closed from the root ID stack (not inside the
    // window above), and closed explicitly when the job ends so the popup never lingers.
    if (r.active() && !ImGui::IsPopupOpen("Rendering")) ImGui::OpenPopup("Rendering");
    if (ImGui::BeginPopupModal("Rendering", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!r.active()) {
            ImGui::CloseCurrentPopup();
        } else {
            const OfflineRenderer::Progress& p = r.progress();
            if (p.phase == OfflineRenderer::Phase::Preroll) {
                ImGui::Text("Pre-roll %lld / %lld", p.prerollDone, p.prerollTotal);
                ImGui::ProgressBar(p.prerollTotal ? (float)p.prerollDone / (float)p.prerollTotal : 0.0f, ImVec2(380.0f, 0.0f));
            }
            ImGui::Text("Frame %lld / %lld", p.framesDone, p.framesTotal);
            ImGui::ProgressBar(p.framesTotal ? (float)p.framesDone / (float)p.framesTotal : 0.0f, ImVec2(380.0f, 0.0f));
            const double remaining = p.speed > 0.0 ? (double)(p.framesTotal - p.framesDone) / p.speed : 0.0;
            ImGui::Text("Elapsed %.0f s   remaining ~%.0f s   %.2fx real time",
                        p.elapsedSeconds, remaining, p.speed / (double)settings_.fps);
            if (!p.status.empty()) ImGui::TextDisabled("%s", p.status.c_str());
            ImGui::TextDisabled("%s", p.outPath.c_str());
            if (ImGui::Button("Cancel")) r.cancel();
        }
        ImGui::EndPopup();
    }
}

} // namespace oss
