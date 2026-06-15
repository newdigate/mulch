#include "gfx/GLUtil.h"
#include <fstream>
#include <sstream>
#include <vector>
#include <cstdio>

namespace oss {

GLuint compileShader(GLenum type, const std::string& src) {
    GLuint sh = glCreateShader(type);
    const char* c = src.c_str();
    glShaderSource(sh, 1, &c, nullptr);
    glCompileShader(sh);
    GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(len > 1 ? len : 1);
        glGetShaderInfoLog(sh, (GLsizei)log.size(), nullptr, log.data());
        std::fprintf(stderr, "[shader compile error]\n%s\n", log.data());
    }
    return sh;
}

GLuint linkProgram(const std::string& vertSrc, const std::string& fragSrc) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragSrc);
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs); glAttachShader(prog, fs);
    glLinkProgram(prog);
    GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0; glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::vector<char> log(len > 1 ? len : 1);
        glGetProgramInfoLog(prog, (GLsizei)log.size(), nullptr, log.data());
        std::fprintf(stderr, "[program link error]\n%s\n", log.data());
    }
    glDeleteShader(vs); glDeleteShader(fs);
    return prog;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) { std::fprintf(stderr, "[readFile] cannot open %s\n", path.c_str()); return ""; }
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
}

void checkGLError(const char* where) {
    GLenum e;
    while ((e = glGetError()) != GL_NO_ERROR)
        std::fprintf(stderr, "[GL error] 0x%04X at %s\n", e, where);
}

} // namespace oss
