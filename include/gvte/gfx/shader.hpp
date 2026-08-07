// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Minimal GL shader-program wrapper: compile + link from source, RAII delete.

#ifndef GVTE_GFX_SHADER_HPP
#define GVTE_GFX_SHADER_HPP

#include <cstdint>
#include <string_view>

#include "gvte/core/types.hpp"

namespace gvte::gfx {

class Program {
public:
    static Result<Program> build(std::string_view vertex_src, std::string_view fragment_src);

    Program(const Program &) = delete;
    Program &operator=(const Program &) = delete;
    Program(Program &&) noexcept;
    Program &operator=(Program &&) noexcept;
    ~Program();

    void use() const noexcept;
    [[nodiscard]] std::uint32_t id() const noexcept { return prog_; }
    [[nodiscard]] int uniform(const char *name) const noexcept;

private:
    Program() = default;
    void destroy() noexcept;
    std::uint32_t prog_{0};
};

} // namespace gvte::gfx

#endif // GVTE_GFX_SHADER_HPP
