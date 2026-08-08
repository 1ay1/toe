// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Minimal GL shader-program wrapper: compile + link from source, RAII delete.

#ifndef TOE_GFX_SHADER_HPP
#define TOE_GFX_SHADER_HPP

#include <cstdint>
#include <string_view>

#include "toe/core/types.hpp"

namespace toe::gfx {

class Program {
public:
    static Result<Program> build(std::string_view vertex_src, std::string_view fragment_src);

    // A default-constructed Program is empty/invalid (id 0) until assigned from
    // Program::build(); valid() reports false.
    Program() = default;
    Program(const Program &) = delete;
    Program &operator=(const Program &) = delete;
    Program(Program &&) noexcept;
    Program &operator=(Program &&) noexcept;
    ~Program();

    void use() const noexcept;
    [[nodiscard]] std::uint32_t id() const noexcept { return prog_; }
    [[nodiscard]] bool valid() const noexcept { return prog_ != 0; }
    [[nodiscard]] int uniform(const char *name) const noexcept;

private:
    void destroy() noexcept;
    std::uint32_t prog_{0};
};

} // namespace toe::gfx

#endif // TOE_GFX_SHADER_HPP
