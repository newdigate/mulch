#include "ui/RenderDialog.h"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <string>
#include <imgui.h>
#include "app/OfflineRenderer.h"
#include "core/Graph.h"
#include "core/PathUtil.h"
#include "core/Preferences.h"
#include "modules/OutputNode.h"
#include "ui/FileDialog.h"

namespace oss {

// InputText over a std::string, growing the buffer through ImGui's resize callback (what
// misc/cpp/imgui_stdlib.cpp does; that file is not in the build). A fixed char[1024] silently
// TRUNCATED a seeded path longer than that and committed the truncation on the first keystroke
// -- an overwrite of whatever the shortened path happened to name.
static int growString(ImGuiInputTextCallbackData* d) {
    if (d->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* s = static_cast<std::string*>(d->UserData);
        s->resize((std::size_t)d->BufTextLen);   // BufTextLen excludes the NUL; resize keeps one spare
        d->Buf = s->data();
    }
    return 0;
}
static bool inputText(const char* label, std::string& s) {
    return ImGui::InputText(label, s.data(), s.capacity() + 1,
                            ImGuiInputTextFlags_CallbackResize, growString, &s);
}

void RenderDialog::seed(Graph& g, const Preferences& prefs, const std::string& projectPath) {
    settings_ = RenderSettings{};
    settings_.endBar = g.automation().lengthBars();          // the Automation song length
    settings_.width  = prefs.textureWidth;
    settings_.height = prefs.textureHeight;
    // Default file: next to the project as <basename>.mp4, else render.mp4 in the projects dir.
    std::string base = projectPath.empty() ? std::string("render") : fileBaseName(projectPath);
    if (base.size() > 4) {
        // Case-insensitive, matching ensureExtension's own comparison below -- "Foo.OSS" must
        // strip just like "foo.oss", or it becomes "Foo.OSS.mp4".
        std::string tail = base.substr(base.size() - 4);
        std::transform(tail.begin(), tail.end(), tail.begin(),
                       [](unsigned char c){ return (char)std::tolower(c); });
        if (tail == ".oss") base.erase(base.size() - 4);
    }
    std::string dir = projectPath.empty() ? prefs.projectsDir : parentDir(projectPath);
    settings_.outPath = dir.empty() ? base + ".mp4" : dir + "/" + base + ".mp4";
}

void RenderDialog::draw(Graph& g, const Preferences& prefs, OfflineRenderer& r, bool* show,
                        const std::string& projectPath, std::string& status) {
    // The job ends inside Application::frame (between draws): hand its outcome to the toolbar.
    if (wasActive_ && !r.active()) { outcome_ = r.progress().status; status = outcome_; }
    wasActive_ = r.active();

    if (show && *show) {
        // Re-seed whenever the loaded project changes (not just on first open): otherwise a
        // stale path from the PREVIOUS project (its basename, possibly its whole directory)
        // lingers in settings_.outPath, and pressing Render silently overwrites that project's
        // video with this one's -- a typed path never goes through the save dialog's overwrite
        // prompt. Skipped while a job is running so the settings a live job is not consulting
        // (start() already snapshotted them) don't change out from under the visible fields.
        if (!r.active() && (!seeded_ || seededPath_ != projectPath)) {
            seed(g, prefs, projectPath);
            seeded_     = true;
            seededPath_ = projectPath;
        }
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

            // Directory on its own line, FILENAME in the field. A fixed-width field shows the
            // START of the string, so a long directory pushed the filename -- the part you check
            // before overwriting something -- off the right edge where nothing could reveal it.
            // (The field still accepts a full path: a typed separator means "use this as-is".)
            const std::string outDir = parentDir(settings_.outPath);
            if (!outDir.empty()) {
                ImGui::TextDisabled("in %s", outDir.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", settings_.outPath.c_str());
            }
            std::string outName = fileBaseName(settings_.outPath);
            ImGui::SetNextItemWidth(-180.0f);
            if (inputText("Output file", outName)) {
                settings_.outPath = (outDir.empty() || outName.find_first_of("/\\") != std::string::npos)
                                  ? outName : outDir + "/" + outName;
            }
            ImGui::SameLine();
            if (ImGui::Button("Browse...")) {
                std::string defName = fileBaseName(settings_.outPath);
                if (defName.empty()) defName = "render.mp4";
                // Seed from the CURRENT field's directory (wherever the user last pointed this
                // dialog), not unconditionally prefs.projectsDir -- otherwise every re-open snaps
                // back to the projects folder even after the user has already chosen elsewhere.
                // Only the very first Browse (before any directory is known) falls back to it.
                std::string startDir = parentDir(settings_.outPath);
                if (startDir.empty()) startDir = prefs.projectsDir;
                std::string p = saveFileDialog("Render Video", "MP4", {"mp4"}, defName, startDir);
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
                // A typed path never passed through Browse's save dialog (and its overwrite
                // prompt), so it may be missing the extension the encoder needs; fix it up here
                // rather than rejecting it, mirroring what Browse already does to its result.
                settings_.outPath = ensureExtension(settings_.outPath, "mp4");
                if (!r.start(g, settings_, error_)) status = "render failed: " + error_;
                // The job can finish (or fail) inside its very first step(), which runs AFTER
                // this draw() call returns, before draw() is entered again. wasActive_ is
                // recomputed from r.active() at the top of THIS call, before start() ran, so
                // without this the next draw() sees wasActive_ == false and the end-of-job edge
                // never fires: outcome_/status silently stay empty. Arm it here instead.
                else wasActive_ = true;
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
            // speed (captured frames per wall second) is 0 until the first captured frame, i.e.
            // for the whole pre-roll -- "remaining ~0 s, 0.00x real time" would read as a stalled
            // or instant job when it is neither. Show only elapsed time until there is a rate to
            // extrapolate from. p.fps (the job's own rate, not settings_ -- Task 10's --render
            // CLI can start a job the dialog never configured) drives the real-time multiple.
            if (p.framesDone > 0) {
                const double remaining = p.speed > 0.0 ? (double)(p.framesTotal - p.framesDone) / p.speed : 0.0;
                ImGui::Text("Elapsed %.0f s   remaining ~%.0f s   %.2fx real time",
                            p.elapsedSeconds, remaining, p.fps > 0 ? p.speed / (double)p.fps : 0.0);
            } else {
                ImGui::Text("Elapsed %.0f s", p.elapsedSeconds);
            }
            if (!p.status.empty()) ImGui::TextDisabled("%s", p.status.c_str());
            ImGui::TextDisabled("%s", p.outPath.c_str());
            if (ImGui::Button("Cancel")) r.cancel();
        }
        ImGui::EndPopup();
    }
}

} // namespace oss
