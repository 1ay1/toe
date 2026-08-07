// SPDX-License-Identifier: LGPL-2.0-or-later

#include "gvte/gfx/shader.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include <epoxy/gl.h>

namespace gvte::gfx {

namespace {

Result<std::uint32_t> compile(GLenum kind, std::string_view src) {
    const GLuint sh = glCreateShader(kind);
    const char *data = src.data();
    const GLint len = static_cast<GLint>(src.size());
    glShaderSource(sh, 1, &data, &len);
    glCompileShader(sh);

    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint log_len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &log_len);
        std::string log(static_cast<std::size_t>(std::max(log_len, 1)), '\0');
        glGetShaderInfoLog(sh, log_len, nullptr, log.data());
        glDeleteShader(sh);
        return fail("shader compile failed: " + log);
    }
    return sh;
}

} // namespace

Result<Program> Program::build(std::string_view vs, std::string_view fs) {
    auto v = compile(GL_VERTEX_SHADER, vs);
    if (!v) return std::unexpected(v.error());
    auto f = compile(GL_FRAGMENT_SHADER, fs);
    if (!f) {
        glDeleteShader(*v);
        return std::unexpected(f.error());
    }

    const GLuint prog = glCreateProgram();
    glAttachShader(prog, *v);
    glAttachShader(prog, *f);
    glLinkProgram(prog);
    glDeleteShader(*v);
    glDeleteShader(*f);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint log_len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &log_len);
        std::string log(static_cast<std::size_t>(std::max(log_len, 1)), '\0');
        glGetProgramInfoLog(prog, log_len, nullptr, log.data());
        glDeleteProgram(prog);
        return fail("program link failed: " + log);
    }

    Program p;
    p.prog_ = prog;
    return p;
}

Program::Program(Program &&o) noexcept : prog_{std::exchange(o.prog_, 0)} {}

Program &Program::operator=(Program &&o) noexcept {
    if (this != &o) {
        destroy();
        prog_ = std::exchange(o.prog_, 0);
    }
    return *this;
}

Program::~Program() { destroy(); }

void Program::destroy() noexcept {
    if (prog_) {
        glDeleteProgram(prog_);
        prog_ = 0;
    }
}

void Program::use() const noexcept { glUseProgram(prog_); }

int Program::uniform(const char *name) const noexcept {
    return glGetUniformLocation(prog_, name);
}

} // namespace gvte::gfx
