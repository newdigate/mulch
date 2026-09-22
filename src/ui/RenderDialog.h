#pragma once
#include <string>
#include "core/OfflineRender.h"

namespace oss {

class Graph;
struct Preferences;
class OfflineRenderer;

// The offline render UI: a "Render Video" settings window (start / finish bar in the Loop-field
// convention, pre-roll, frame rate, size, output file) and, while a job runs, a modal
// "Rendering" popup with progress + Cancel. The modal blocks graph edits, so a render is
// deterministic. Settings live here for the session (not persisted).
class RenderDialog {
public:
    // Draws the settings window when *show, and the progress modal while `r` is active.
    // `projectPath` seeds the default file name; `status` receives the job's outcome line (for
    // the toolbar) when it ends.
    void draw(Graph& g, const Preferences& prefs, OfflineRenderer& r, bool* show,
              const std::string& projectPath, std::string& status);

private:
    void seed(Graph& g, const Preferences& prefs, const std::string& projectPath);

    RenderSettings settings_;
    bool           seeded_     = false;   // defaults filled on first open
    std::string    seededPath_;           // the projectPath settings_ was last seeded from
    bool           wasActive_  = false;   // to notice the job ending between draws
    std::string    error_;                // start() failure, shown inline
    std::string    outcome_;              // last job's outcome line, shown inline
};

} // namespace oss
