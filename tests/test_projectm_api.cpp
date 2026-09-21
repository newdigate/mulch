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
    std::size_t homeAt = d.size(), usrLocalAt = d.size();
    for (std::size_t i = 0; i < d.size(); ++i) {
        if (homeAt == d.size()     && d[i].rfind("/home/me/.local/lib/", 0) == 0) homeAt = i;
        if (usrLocalAt == d.size() && d[i].rfind("/usr/local/lib/", 0) == 0)      usrLocalAt = i;
    }
    CHECK(homeAt < d.size());
    CHECK(usrLocalAt < d.size());
    CHECK(homeAt < usrLocalAt);                      // the user's own build wins over a system install
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
    CHECK(api.statusText() == std::string("missing symbol projectm_get_version_components (") + kSystemLibPath + ")");
    CHECK(api.getVersionComponents == nullptr);      // a rejected library leaves the table empty
    CHECK(api.create == nullptr);
    CHECK(api.renderFrameFbo == nullptr);
}

TEST_CASE("ProjectMApi: an old library is rejected with its version and path; the table stays empty") {
    ProjectMApi api;
    CHECK_FALSE(api.loadFrom({OSS_PM_FAKE_41}));
    CHECK_FALSE(api.available());
    CHECK(api.statusText() == std::string("found projectM 4.1.0, needs 4.2+ (") + OSS_PM_FAKE_41 + ")");
    CHECK(api.getVersionComponents == nullptr);      // bound from the rejected library, must not survive it
    CHECK(api.create == nullptr);
    CHECK(api.versionText().empty());
    CHECK(api.loadedPath().empty());
}

TEST_CASE("ProjectMApi: falls through absent and rejected candidates to a good one") {
    ProjectMApi api;
    REQUIRE(api.loadFrom({"/nonexistent/dir/libprojectM-4.dylib", OSS_PM_FAKE_41, OSS_PM_FAKE_42}));
    CHECK(api.available());
    CHECK(api.versionText() == "4.2.0");
    CHECK(api.statusText() == "projectM 4.2.0");
    CHECK(api.loadedPath() == OSS_PM_FAKE_42);
    CHECK(api.create != nullptr);
    CHECK(api.burnTexture != nullptr);
    CHECK(api.loadFrom({"/nonexistent/other.dylib"}));   // a no-op once available
    CHECK(api.loadedPath() == OSS_PM_FAKE_42);
}

TEST_CASE("ProjectMApi remembers the preference path of the load that succeeded") {
    ProjectMApi api;
    CHECK(api.loadedPrefPath().empty());
    REQUIRE(api.loadFrom({OSS_PM_FAKE_42}, "/the/pref/value"));
    CHECK(api.loadedPrefPath() == "/the/pref/value");
    CHECK(api.loadFrom({OSS_PM_FAKE_42}, "/a/later/pref"));      // a no-op once available...
    CHECK(api.loadedPrefPath() == "/the/pref/value");            // ...so the remembered pref does not move
}
