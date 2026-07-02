#include <doctest/doctest.h>
#include "core/PathUtil.h"

using namespace oss;

TEST_CASE("fileBaseName strips directories") {
    CHECK(fileBaseName("/a/b/c.oss") == "c.oss");
    CHECK(fileBaseName("c.oss") == "c.oss");
    CHECK(fileBaseName("a\\b\\c.oss") == "c.oss");   // backslash separator
    CHECK(fileBaseName("") == "");
    CHECK(fileBaseName("/a/b/") == "");              // trailing slash -> empty basename
    CHECK(fileBaseName("/") == "");                  // bare separator -> empty basename
}

TEST_CASE("ensureExtension appends only when missing (case-insensitive)") {
    CHECK(ensureExtension("foo", "oss") == "foo.oss");
    CHECK(ensureExtension("foo.oss", "oss") == "foo.oss");
    CHECK(ensureExtension("foo.OSS", "oss") == "foo.OSS");          // already present (any case) -> unchanged
    CHECK(ensureExtension("path/to/proj", "oss") == "path/to/proj.oss");
    CHECK(ensureExtension("", "oss") == "");                         // empty stays empty
    CHECK(ensureExtension("name.bak", "oss") == "name.bak.oss");     // different ext -> appended
    CHECK(ensureExtension("foo", "OSS") == "foo.OSS");   // upper-case ext appended verbatim
    CHECK(ensureExtension("foo.oss", "OSS") == "foo.oss"); // case-insensitive match -> unchanged
}

TEST_CASE("parentDir returns the directory portion") {
    CHECK(parentDir("a/b/c.png") == "a/b");
    CHECK(parentDir("c.png") == "");                 // no separator -> empty
    CHECK(parentDir("a\\b\\c.png") == "a\\b");        // backslash separator
    CHECK(parentDir("/a/b/") == "/a/b");              // trailing slash -> dir before it
    CHECK(parentDir("") == "");
}
