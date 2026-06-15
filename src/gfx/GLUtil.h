#pragma once
#include <glad/gl.h>
#include <string>

namespace oss {

GLuint compileShader(GLenum type, const std::string& src);
GLuint linkProgram(const std::string& vertSrc, const std::string& fragSrc);
std::string readFile(const std::string& path);
void checkGLError(const char* where);

} // namespace oss
