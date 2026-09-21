#include <doctest/doctest.h>
#include "gfx/ProjectMApi.h"
#include "system_lib.h"
#include <string>
#include <vector>

using namespace oss;

TEST_CASE("projectM version gate: major must be 4, minor at least 2") {
    CHECK_FALSE(isSupportedProjectMVersion(4, 1));   // 4.1.7, the latest release
    CHECK(isSupportedProjectMVersion(4, 2));
    CHECK(isSupportedProjectMVersion(4, 9));
    CHECK_FALSE(isSupportedProjectMVersion(5, 0));   // unknown ABI
    CHECK_FALSE(isSupportedProjectMVersion(3, 1));
}

TEST_CASE("projectM candidate paths: the preference comes first; no empty entries") {
    std::vector<std::string> c = projectMCandidatePaths("/opt/pm/libprojectM-4.dylib", "/home/me");
    REQUIRE_FALSE(c.empty());
    CHECK(c.front() == "/opt/pm/libprojectM-4.dylib");
    for (const std::string& s : c) CHECK_FALSE(s.empty());

    std::vector<std::string> d = projectMCandidatePaths("", "/home/me");
    REQUIRE_FALSE(d.empty());
    CHECK(d.size() == c.size() - 1);                 // only the preference entry differs
#if !defined(_WIN32)
    bool sawHome = false;
    for (const std::string& s : d) if (s.rfind("/home/me/.local/lib/", 0) == 0) sawHome = true;
    CHECK(sawHome);
#endif
}

TEST_CASE("ProjectMApi: nothing to open -> unavailable, 'not found'") {
    ProjectMApi api;
    CHECK_FALSE(api.available());
    CHECK_FALSE(api.loadFrom({"/nonexistent/dir/libprojectM-4.dylib"}));
    CHECK_FALSE(api.available());
    CHECK(api.statusText() == "projectM not found");
}

TEST_CASE("ProjectMApi: a real library that is not projectM is rejected by name") {
    ProjectMApi api;
    CHECK_FALSE(api.loadFrom({kSystemLibPath}));
    CHECK_FALSE(api.available());
    CHECK(api.statusText() == "missing symbol projectm_get_version_components");
    CHECK(api.getVersionComponents == nullptr);      // a rejected library leaves the table empty
    CHECK(api.create == nullptr);
    CHECK(api.renderFrameFbo == nullptr);
}
